#include "iw3_vision.h"

#include "common/fs_util.h"
#include "common/log.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <regex>
#include <stdexcept>
#include <string_view>

namespace iw3
{
namespace
{
enum class VisionFieldKind
{
    boolean,
    scalar,
    vector,
    image,
    gradingClut,
    fogSpline,
};

struct VisionField
{
    const char *token;
    VisionFieldKind kind;
    std::array<float, 3> defaults;
};

constexpr VisionField kVisionFields[] = {
#include "replay_vision_fields.inc"
};
static_assert(std::size(kVisionFields) == 218, "Replay 1.20 parses 218 vision fields");

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// GSC without comments or developer blocks; string literals are kept verbatim.
std::string StripGscComments(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();)
    {
        if (text[at] == '"')
        {
            const std::size_t begin = at++;
            while (at < text.size() && text[at] != '"')
                at += text[at] == '\\' ? 2 : 1;
            at = std::min(at + 1, text.size());
            out.append(text, begin, at - begin);
        }
        else if (text.compare(at, 2, "//") == 0)
        {
            at = text.find('\n', at);
            if (at == std::string::npos)
                break;
        }
        else if (text.compare(at, 2, "/*") == 0 || text.compare(at, 2, "/#") == 0)
        {
            const char *close = text[at + 1] == '*' ? "*/" : "#/";
            const std::size_t end = text.find(close, at + 2);
            at = end == std::string::npos ? text.size() : end + 2;
            out += ' ';
        }
        else
            out += text[at++];
    }
    return out;
}

bool ParseFloat(std::string_view text, float &value)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    if (!text.empty() && text.front() == '+')
        text.remove_prefix(1);
    if (text.empty())
        return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
           std::isfinite(value);
}

std::string Number(const float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.9g", value);
    return buffer;
}

std::string Vector(const std::array<float, 3> &value)
{
    return Number(value[0]) + ' ' + Number(value[1]) + ' ' + Number(value[2]);
}

// ITU-R BT.2100 PQ (SMPTE ST 2084), exactly as cs_universal_clut_generate evaluates it.
constexpr double kPqM1 = 0.1593017578125;
constexpr double kPqM2 = 78.84375;
constexpr double kPqC1 = 0.8359375;
constexpr double kPqC2 = 18.8515625;
constexpr double kPqC3 = 18.6875;

double PqDecode(const double encoded)
{
    const double power = std::pow(std::clamp(encoded, 0.0, 1.0), 1.0 / kPqM2);
    return std::pow(std::max(power - kPqC1, 0.0) / (kPqC2 - kPqC3 * power), 1.0 / kPqM1);
}

double PqEncode(const double linear)
{
    const double power = std::pow(std::max(linear, 0.0), kPqM1);
    return std::pow((kPqC1 + kPqC2 * power) / (1.0 + kPqC3 * power), kPqM2);
}

using Color = std::array<double, 3>;

Color Multiply(const double (&matrix)[3][3], const Color &value)
{
    Color result{};
    for (unsigned row = 0; row < 3; ++row)
        result[row] = matrix[row][0] * value[0] + matrix[row][1] * value[1] +
                      matrix[row][2] * value[2];
    return result;
}

// The two gamut matrices cs_universal_clut_generate uses.
constexpr double kBt709ToBt2020[3][3] = {
    {0.6274, 0.3293, 0.0433}, {0.0691, 0.9195, 0.0114}, {0.0164, 0.0880, 0.8956}};
constexpr double kBt2020ToBt709[3][3] = {
    {1.6605, -0.5877, -0.0728}, {-0.1246, 1.1330, -0.0084}, {-0.0182, -0.1006, 1.1187}};

// The universal CLUT feeds grading CLUTs the exposed scene value scaled so that
// 1024 is PQ peak. IW3 had no tone mapper: its film pass saw the gamma-encoded
// framebuffer where 1 is white. Treat exposed 1.0 as that white and the 2.2
// display gamma as the encoding (IW3's own film shader works on these values).
constexpr double kPqScale = 1024.0;
constexpr double kDisplayGamma = 2.2;

Color ApplyFilm(const Film &film, const Color &linear)
{
    // Highlights above white keep the grade of white (IW3 clipped them there).
    const double scale = std::max({1.0, linear[0], linear[1], linear[2]});
    Color encoded{};
    for (unsigned channel = 0; channel < 3; ++channel)
        encoded[channel] = std::pow(linear[channel] / scale, 1.0 / kDisplayGamma);

    // R_UpdateColorManipulation; r_contrast 1, r_brightness 0 and r_desaturation 1
    // leave the vision values unchanged.
    const double desaturation = std::max(static_cast<double>(film.desaturation), 1.0 / 4096.0);
    const double desaturationScale = 1.0 / desaturation - 1.0;
    double tintScale = film.contrast * desaturation;
    double bias = film.brightness + 0.5 - film.contrast * 0.5;
    if (film.invert)
    {
        tintScale = -tintScale;
        bias += 1.0;
    }
    // Film shader: lum = dot(c, Rec.601); c' = c * desaturationScale + lum;
    // out = c' * (tintBase + tintDelta * lum) + bias.
    const double luminance =
        encoded[0] * 0.299 + encoded[1] * 0.587 + encoded[2] * 0.114;
    Color result{};
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        const double base = film.darkTint[channel] * tintScale;
        const double delta = (film.lightTint[channel] - film.darkTint[channel]) * tintScale;
        const double graded =
            (encoded[channel] * desaturationScale + luminance) * (base + delta * luminance) + bias;
        // The 8-bit IW3 framebuffer clamps the graded value.
        result[channel] = std::pow(std::clamp(graded, 0.0, 1.0), kDisplayGamma) * scale;
    }
    return result;
}

bool IsIdentity(const Film &film)
{
    return film.contrast == 1.0f && film.brightness == 0.0f && film.desaturation == 0.0f &&
           !film.invert && film.lightTint == std::array<float, 3>{1.0f, 1.0f, 1.0f} &&
           film.darkTint == std::array<float, 3>{1.0f, 1.0f, 1.0f};
}
// The map script plus its createart script (maps\createart\<map>_art), where
// Radiant's art tools write setExpFog and VisionSetNaked; comments removed.
std::string ReadMapArtScripts(const std::filesystem::path &exportRoot, const std::string &sourceMap)
{
    std::string result;
    for (const auto &path : {exportRoot / "maps" / "mp" / (sourceMap + ".gsc"),
                             exportRoot / "maps" / "createart" / (sourceMap + "_art.gsc")})
    {
        std::string text;
        if (zt::read_file_str(path.string(), text))
            result += StripGscComments(text) + "\n";
    }
    return result;
}
} // namespace

std::optional<ExpFog> ReadMapExpFog(const std::filesystem::path &exportRoot,
                                    const std::string &sourceMap)
{
    const std::string text = ReadMapArtScripts(exportRoot, sourceMap);
    if (text.empty())
        return std::nullopt;
    static const std::regex call(R"(\bsetexpfog\s*\(([^()]*)\))", std::regex::icase);
    std::optional<ExpFog> result;
    for (auto match = std::sregex_iterator(text.begin(), text.end(), call);
         match != std::sregex_iterator(); ++match)
    {
        std::vector<float> values;
        bool literal = true;
        const std::string arguments = (*match)[1].str();
        std::size_t begin = 0;
        while (literal)
        {
            const std::size_t comma = arguments.find(',', begin);
            float value{};
            literal = ParseFloat(std::string_view(arguments).substr(
                                     begin, comma == std::string::npos ? std::string::npos
                                                                       : comma - begin),
                                 value);
            values.push_back(value);
            if (comma == std::string::npos)
                break;
            begin = comma + 1;
        }
        if (!literal || values.size() < 5 || values.size() > 6 || values[0] < 0.0f ||
            values[1] <= 0.0f)
        {
            zt::warn("iw3: %s setExpFog(%s) is not a literal fog; not converted",
                     sourceMap.c_str(), arguments.c_str());
            continue;
        }
        ExpFog fog{values[0], values[1], {}};
        for (unsigned channel = 0; channel < 3; ++channel)
            fog.color[channel] = std::clamp(values[2 + channel], 0.0f, 1.0f);
        if (!result)
            result = fog;
        else if (result->start != fog.start || result->halfway != fog.halfway ||
                 result->color != fog.color)
            zt::warn("iw3: %s sets more than one exponential fog; converting the first",
                     sourceMap.c_str());
    }
    if (result)
        zt::info("iw3: exponential fog start %g halfway %g colour (%g %g %g)", result->start,
                 result->halfway, result->color[0], result->color[1], result->color[2]);
    return result;
}

std::optional<Film> ReadMapFilm(const std::filesystem::path &exportRoot,
                                const std::string &sourceMap)
{
    // The map (or its createart script) names its vision with VisionSetNaked;
    // IW3 also loads vision/<map>.vision by default.
    std::string visionName = sourceMap;
    {
        const std::string scripts = ReadMapArtScripts(exportRoot, sourceMap);
        static const std::regex naked(R"re(\bvisionsetnaked\s*\(\s*"([^"]+)")re",
                                      std::regex::icase);
        std::smatch match;
        if (std::regex_search(scripts, match, naked) &&
            std::filesystem::exists(exportRoot / "vision" / (match[1].str() + ".vision")))
            visionName = match[1].str();
    }
    const auto path = exportRoot / "vision" / (visionName + ".vision");
    std::string text;
    if (!zt::read_file_str(path.string(), text))
        return std::nullopt;
    std::map<std::string, std::string> values;
    static const std::regex line(R"re(^\s*(\S+)\s+"([^"]*)")re");
    std::size_t begin = 0;
    while (begin < text.size())
    {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos)
            end = text.size();
        std::smatch match;
        const std::string row = text.substr(begin, end - begin);
        if (std::regex_search(row, match, line))
            values[Lower(match[1].str())] = match[2].str();
        begin = end + 1;
    }
    const auto scalar = [&](const char *name, float &out) {
        const auto found = values.find(name);
        if (found != values.end() && !ParseFloat(found->second, out))
            throw std::runtime_error("IW3 vision " + path.string() + " has an invalid " + name);
    };
    const auto vector = [&](const char *name, std::array<float, 3> &out) {
        const auto found = values.find(name);
        if (found == values.end())
            return;
        float x{}, y{}, z{};
        char tail{};
        if (std::sscanf(found->second.c_str(), "%f %f %f %c", &x, &y, &z, &tail) != 3)
            throw std::runtime_error("IW3 vision " + path.string() + " has an invalid " + name);
        out = {x, y, z};
    };
    float enabled{};
    scalar("r_filmenable", enabled);
    if (enabled == 0.0f)
        return std::nullopt;
    Film film;
    float invert{};
    scalar("r_filmcontrast", film.contrast);
    scalar("r_filmbrightness", film.brightness);
    scalar("r_filmdesaturation", film.desaturation);
    scalar("r_filminvert", invert);
    film.invert = invert != 0.0f;
    vector("r_filmlighttint", film.lightTint);
    vector("r_filmdarktint", film.darkTint);
    if (IsIdentity(film))
        return std::nullopt;
    zt::info("iw3: film contrast %g brightness %g desaturation %g light tint (%g %g %g) dark "
             "tint (%g %g %g)",
             film.contrast, film.brightness, film.desaturation, film.lightTint[0],
             film.lightTint[1], film.lightTint[2], film.darkTint[0], film.darkTint[1],
             film.darkTint[2]);
    return film;
}

std::string FogShaderDefines(const std::optional<ExpFog> &fog, const LightingStyle style)
{
    // iw8: opaque surfaces get Replay's native fog pass (stock opaque forward+
    // shaders apply no fog of their own; only translucent ones do), and the
    // surface shader adds Replay's probe reflections (MAP_IW8_LIGHTING).
    if (style == LightingStyle::iw8)
        return "#define MAP_IW8_LIGHTING 1\n";
    if (!fog)
        return {};
    return "#define MAP_FOG 1\n#define MAP_FOG_START " + Number(fog->start) +
           "\n#define MAP_FOG_DENSITY " +
           Number(static_cast<float>(0.69314718055994531 / fog->halfway)) +
           "\n#define MAP_FOG_COLOR float3(" + Number(fog->color[0]) + "," +
           Number(fog->color[1]) + "," + Number(fog->color[2]) + ")\n";
}

std::vector<std::uint8_t> BuildGradingClutPixels(const Film &film)
{
    constexpr unsigned size = GradingClutSize;
    std::vector<std::uint8_t> pixels(std::size_t(size) * size * size * 8);
    std::size_t at = 0;
    for (unsigned blue = 0; blue < size; ++blue)
        for (unsigned green = 0; green < size; ++green)
            for (unsigned red = 0; red < size; ++red)
            {
                const Color input2020{PqDecode(red / double(size - 1)) * kPqScale,
                                      PqDecode(green / double(size - 1)) * kPqScale,
                                      PqDecode(blue / double(size - 1)) * kPqScale};
                const Color input709 = Multiply(kBt2020ToBt709, input2020);
                Color inGamut{};
                for (unsigned channel = 0; channel < 3; ++channel)
                    inGamut[channel] = std::max(input709[channel], 0.0);
                const Color graded = ApplyFilm(film, inGamut);
                Color output709{};
                // Grade the representable part; keep any out-of-gamut remainder.
                for (unsigned channel = 0; channel < 3; ++channel)
                    output709[channel] = graded[channel] + (input709[channel] - inGamut[channel]);
                const Color output2020 = Multiply(kBt709ToBt2020, output709);
                for (unsigned channel = 0; channel < 4; ++channel)
                {
                    const double encoded =
                        channel == 3 ? 0.0
                                     : std::clamp(PqEncode(output2020[channel] / kPqScale), 0.0, 1.0);
                    const auto unorm = static_cast<std::uint16_t>(std::lround(encoded * 65535.0));
                    pixels[at++] = static_cast<std::uint8_t>(unorm);
                    pixels[at++] = static_cast<std::uint8_t>(unorm >> 8);
                }
            }
    return pixels;
}

std::string BuildReplayVision(const std::optional<ExpFog> &fog, const std::string &gradingClut,
                              const LightingStyle style, const std::array<float, 3> &sunDirection)
{
    std::map<std::string_view, std::string> overrides;
    if (style == LightingStyle::iw8)
    {
        // Stock mp_shipment's fog, atmospheric scattering and volumetric fields
        // (replay_vision_iw8_template.inc), lit by this map's sun. A map with
        // IW3 fog keeps its colour and distances.
        static constexpr std::pair<std::string_view, std::string_view> kTemplate[] = {
#include "replay_vision_iw8_template.inc"
        };
        for (const auto &[token, value] : kTemplate)
            overrides[token] = std::string(value);
        overrides["RLNQONPKS"] = Vector(sunDirection); // fogSunDir
        if (fog)
        {
            const float start = std::clamp(fog->start, 0.0f, 20000.0f);
            const float halfway = std::clamp(fog->halfway, 1.0f, 70000.0f);
            overrides["MSTOLQOTKN"] = Number(halfway);    // fogDensity
            overrides["RPNOKKL"] = Number(start);         // fogStartDist
            overrides["NMLMMNQLSQ"] = Vector(fog->color); // fogColor
        }
    }
    else if (!fog)
    {
        // IW3 maps without setExpFog have no fog; Replay's default vision fog
        // (70000-unit half distance, full opacity) would still haze big maps.
        overrides["TRPROKTLS"] = "0"; // fogMaxOpacity
    }
    else
    {
        // Replay's non-atmospheric fog (fogAtmosphericScattering 0) evaluates
        // exp2(-(distance - fogStartDist) / fogDensity): fogDensity is the IW3 halfway.
        const float start = std::clamp(fog->start, 0.0f, 20000.0f);
        const float halfway = std::clamp(fog->halfway, 1.0f, 70000.0f);
        if (start != fog->start || halfway != fog->halfway)
            zt::warn("iw3: exponential fog clamped to Replay's vision ranges");
        overrides["MSTOLQOTKN"] = Number(halfway);       // fogDensity
        overrides["RPNOKKL"] = Number(start);            // fogStartDist
        overrides["NTPTLNNQQL"] = "1";                   // fogColorIntensity
        overrides["NMLMMNQLSQ"] = Vector(fog->color);    // fogColor
        // The surface shader bakes this fog (FogShaderDefines). Stock opaque
        // forward+ shaders apply no fog, so a native fog pass would add it a
        // second time; its opacity stays 0 (the parameters document the map).
        overrides["TRPROKTLS"] = "0";                    // fogMaxOpacity
        overrides["MPNPSMMTTT"] = Number(halfway);       // fogSunDensity
        overrides["MLORTTKOLN"] = "1";                   // fogSunColorIntensity
        overrides["MRTOMMTTQM"] = Vector(fog->color);    // fogSunColor
        overrides["MKKLQTSKSO"] = "0";                   // fogAtmosphericScattering
        overrides["SNNNRLTML"] = Number(halfway);        // fogHalfPlaneDistance
    }
    if (!gradingClut.empty())
        overrides["NONMKTKNNM"] = gradingClut; // grading CLUT (stock: <map>_clut)

    std::string text;
    for (const auto &field : kVisionFields)
    {
        std::string value;
        if (const auto found = overrides.find(field.token); found != overrides.end())
            value = found->second;
        else if (field.kind == VisionFieldKind::scalar)
            value = Number(field.defaults[0]);
        else if (field.kind == VisionFieldKind::vector)
            value = Vector(field.defaults);
        else if (field.kind == VisionFieldKind::boolean)
            value = field.defaults[0] != 0.0f ? "1" : "0";
        text += field.token;
        text += " \"" + value + "\"\n";
    }
    return text;
}
} // namespace iw3
