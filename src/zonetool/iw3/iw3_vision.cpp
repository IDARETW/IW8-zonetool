#include "iw3_vision.h"

#include "common/fs_util.h"
#include "common/log.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
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

std::optional<SunSprite> ReadMapSun(const std::filesystem::path &exportRoot,
                                    const std::string &sourceMap)
{
    std::ifstream input(exportRoot / "sun" / (sourceMap + ".sun"));
    if (!input)
        return std::nullopt;
    std::map<std::string, std::string> values;
    static const std::regex line(R"re(^\s*(\w+)\s+"([^"]*)")re");
    for (std::string text; std::getline(input, text);)
        if (std::smatch match; std::regex_search(text, match, line))
            values[Lower(match[1].str())] = match[2].str();
    const auto sprite = values.find("r_sunsprite_shader");
    if (sprite == values.end() || sprite->second.empty())
        return std::nullopt;
    const auto number = [&](const std::string &key, const float fallback) {
        float value = fallback;
        if (const auto found = values.find(key); found != values.end())
            ParseFloat(found->second, value);
        return value;
    };
    std::array<float, 3> angles{};
    if (const auto found = values.find("r_sun_fx_position"); found != values.end())
    {
        std::istringstream stream(found->second);
        stream >> angles[0] >> angles[1] >> angles[2];
    }
    // IW3 angles: negative pitch looks up.
    constexpr float degrees = 3.14159265f / 180.0f;
    SunSprite sun;
    sun.direction = {std::cos(angles[0] * degrees) * std::cos(angles[1] * degrees),
                     std::cos(angles[0] * degrees) * std::sin(angles[1] * degrees),
                     -std::sin(angles[0] * degrees)};
    // r_sunsprite_size is in 480-line virtual pixels at CoD4's 65 degree view.
    sun.radiusDegrees =
        std::clamp(number("r_sunsprite_size", 16.0f) / 480.0f * 65.0f * 0.5f, 0.2f, 10.0f);
    sun.flareAlpha = std::clamp(number("r_sunflare_max_alpha", 0.0f), 0.0f, 1.0f);
    zt::info("iw3: sun sprite pitch %g yaw %g radius %g degrees, flare %g", angles[0], angles[1],
             sun.radiusDegrees, sun.flareAlpha);
    return sun;
}

namespace
{
// Shipped Replay 1.20 MP maps (ACTS dumps of each zone's ComWorld sun and
// <map>.vision tonemap fields 63-76), ordered by sun intensity. Donetsk matches
// Quarry and is left out; King and Emporium are the dark (interior / dusk) maps.
constexpr Iw8LightingReference kIw8LightingReferences[] = {
    {"mp_m_king", 1.55685f, {1.2781f, 0.952621f, 0.650445f}, {"3","1","1","6","0","-10","0","0.075","4.56","6.605912","8.57","-1.6","-0.279","1.28"}},
    {"mp_emporium", 3.39796f, {1.4395f, 0.9178f, 0.520111f}, {"3","1","1","8.5","0","-10","0","0.075","5.6","8.229471","11.9","-2","-0.6758758","1.5"}},
    {"mp_shipment", 8.63193f, {1.04877f, 0.993345f, 0.922306f}, {"3","1","1","10.5","0","-10","0.6","0.75","6.1","12.14071","15","-2","-0.3869346","1.5"}},
    {"mp_village2", 11.0536f, {0.983046f, 1.00121f, 1.0379f}, {"3","1","1","10","0","-10","0","0.075","8","11","14","-1.5","0","1.5"}},
    {"mp_br_quarry", 11.512f, {1.14193f, 0.976265f, 0.817178f}, {"3","1","1","12","0","-10","0","0.15","9","11.75","16","-1","0.15","2"}},
    {"mp_rust", 11.6969f, {1.11411f, 0.989194f, 0.771041f}, {"3","1","1","10","0","-10","0","0.06","7.53","10.3","14","-3","-1","1.3"}},
    {"mp_backlot2", 20.3368f, {0.98634f, 1.00452f, 0.995408f}, {"3","1","1","12","0","-10","-0.1","0.075","6.2","11.23978","16","-5","-0.9157199","3.2"}},
    {"mp_m_fork", 23.7478f, {1.02338f, 0.996126f, 0.969523f}, {"3","1","1","11.5","0","-10","0","0.075","5.6","10.31878","14.5","-2.38","-1.024797","1.5"}},
    {"mp_m_speed", 23.9449f, {0.97691f, 1.00372f, 1.03118f}, {"3","1","1","14","0","-10","0","0.075","7","11.34573","15.6","-1","0.05577891","1.6"}},
    {"mp_aniyah_tac", 24.6499f, {0.994142f, 1.00325f, 0.985086f}, {"3","1","1","12","0","-10","0","0.075","8.6","12.49304","16.2","-3.2","-0.7722935","2.2"}},
    {"mp_t_gun_course", 25.8181f, {1.13566f, 0.979416f, 0.804427f}, {"3","1","1","14","0","-10","0","0.1","11.5","13.25","15","-0.25","0.4","0.75"}},
    {"mp_hideout", 26.9233f, {0.984644f, 1.00247f, 1.02072f}, {"3","1","1","12","0","-10","0","0.075","9","10.7015","15","-3","-0.9118453","2"}},
    {"mp_m_speedball", 30.6723f, {1.00331f, 1.00331f, 0.95747f}, {"3","1","1","13","0","-10","0.65","0.075","10.8","13.21207","14.6","-0.5","0.04669083","1.5"}},
};
// Compiled CoD4 MP day suns (ComWorld sun light luminance, Rec. 709), ascending:
// carentan citystreets bog crossfire strike bloc farm backlot crash overgrown
// pipeline countdown vacant broadcast killhouse shipment creek convoy showdown.
constexpr float kIw3DaySuns[] = {0.3817f, 0.4033f, 0.5596f, 0.683f, 0.693f, 0.7568f, 0.8413f,
                                 0.8853f, 0.8951f, 0.9116f, 0.9564f, 1.0444f, 1.0755f, 1.1007f,
                                 1.1443f, 1.1548f, 1.3165f, 1.3316f, 1.3633f};
// Night (moonlight) suns: crash_snow, cargoship.
constexpr float kIw3NightSuns[] = {0.0427f, 0.7542f};
} // namespace

Iw8LightingMatch MatchIw8Lighting(const std::array<float, 3> &sun)
{
    Iw8LightingMatch match;
    const float luminance = 0.2126f * sun[0] + 0.7152f * sun[1] + 0.0722f * sun[2];
    if (!(luminance > 0.0f))
        return match;
    match.night = sun[2] > 1.4f * std::max(sun[0], sun[1]);
    constexpr std::size_t dayFirst = 2, dayCount = std::size(kIw8LightingReferences) - 2;
    if (match.night)
    {
        // Log-log interpolation between the two night references.
        const float t = std::clamp(std::log(luminance / kIw3NightSuns[0]) /
                                       std::log(kIw3NightSuns[1] / kIw3NightSuns[0]),
                                   0.0f, 1.0f);
        match.sunIntensity = std::exp(std::lerp(std::log(kIw8LightingReferences[0].sunIntensity),
                                                std::log(kIw8LightingReferences[1].sunIntensity), t));
    }
    else
    {
        // Quantile of the IW3 sun among CoD4's day suns -> the same quantile of the
        // shipped day suns (piecewise linear in log intensity).
        constexpr std::size_t n = std::size(kIw3DaySuns);
        float rank = 0;
        if (luminance >= kIw3DaySuns[n - 1])
            rank = static_cast<float>(n - 1);
        else if (luminance > kIw3DaySuns[0])
            for (std::size_t i = 1; i < n; ++i)
                if (luminance <= kIw3DaySuns[i])
                {
                    rank = static_cast<float>(i - 1) +
                           (luminance - kIw3DaySuns[i - 1]) / (kIw3DaySuns[i] - kIw3DaySuns[i - 1]);
                    break;
                }
        const float position = rank / static_cast<float>(n - 1) * static_cast<float>(dayCount - 1);
        const std::size_t low = std::min(static_cast<std::size_t>(position), dayCount - 2);
        const float f = position - static_cast<float>(low);
        match.sunIntensity =
            std::exp(std::lerp(std::log(kIw8LightingReferences[dayFirst + low].sunIntensity),
                               std::log(kIw8LightingReferences[dayFirst + low + 1].sunIntensity), f));
    }
    match.lightUnitScale = match.sunIntensity / luminance;
    // Exposure from the shipped map of the same class whose sun is nearest.
    float best = INFINITY;
    for (std::size_t i = match.night ? 0 : dayFirst; i < (match.night ? dayFirst : std::size(kIw8LightingReferences)); ++i)
    {
        const float distance = std::abs(std::log(kIw8LightingReferences[i].sunIntensity / match.sunIntensity));
        if (distance < best)
        {
            best = distance;
            match.exposure = &kIw8LightingReferences[i];
        }
    }
    return match;
}

std::string FogShaderDefines(const std::optional<ExpFog> &fog, const MapStyle style,
                             const std::optional<SunSprite> &sun, const float lightUnitScale)
{
    // iw8: the surface shader adds Replay's probe reflections and sun specular
    // (MAP_IW8_LIGHTING) and draws the map's IW3 sun sprite and flare in the sky
    // (owner, 2026-10-15: sun position and glow as in the original map; a map
    // without a sun file, such as Discovery's night, shows no sun). Both styles
    // bake IW3's own exponential fog.
    std::string lighting = style == MapStyle::iw8 ? "#define MAP_IW8_LIGHTING 1\n" : "";
    if (style == MapStyle::iw8)
        lighting += "#define MAP_LIGHT_UNIT_SCALE " + Number(lightUnitScale) + "\n";
    if (style == MapStyle::iw8 && sun)
        lighting += "#define MAP_SUN_SPRITE float4(" + Number(sun->direction[0]) + "," +
                    Number(sun->direction[1]) + "," + Number(sun->direction[2]) + "," +
                    Number(sun->radiusDegrees * 3.14159265f / 180.0f) +
                    ")\n#define MAP_SUN_FLARE " + Number(sun->flareAlpha) + "\n";
    if (!fog)
        return lighting;
    return lighting + "#define MAP_FOG 1\n#define MAP_FOG_START " + Number(fog->start) +
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
                              const MapStyle style, const std::array<float, 3> &sunDirection,
                              const Iw8LightingReference *exposure, const float lightUnitScale)
{
    std::map<std::string_view, std::string> overrides;
    if (style == MapStyle::iw8)
    {
        // Stock mp_shipment's fog, atmospheric scattering and volumetric fields
        // (replay_vision_iw8_template.inc), lit by this map's sun. A map with
        // IW3 fog keeps its colour and distances.
        static constexpr std::pair<std::string_view, std::string_view> kTemplate[] = {
#include "replay_vision_iw8_template.inc"
        };
        for (const auto &[token, value] : kTemplate)
            overrides[token] = std::string(value);
        // Shipment's fog, atmosphere and height-fog fields (35-62) are dropped:
        // fogAtmosphericScattering hazed every object of a map without IW3 fog
        // white while the sky stayed clear (owner test 2026-10-15: Aerodrome,
        // Agroprom, Arnhem, Greenband, Gulag too bright, sky haze mismatched).
        // Both styles use IW3's fog below.
        for (const std::string_view token :
             {"MSTOLQOTKN", "RPNOKKL", "NTPTLNNQQL", "NMLMMNQLSQ", "TRPROKTLS", "MPNPSMMTTT",
              "MLORTTKOLN", "MRTOMMTTQM", "NKLTMMOOLP", "NONQQTPNLT", "NORQKLTPTR", "LOROLOTNMP",
              "SPQNRNKQR", "MKKLQTSKSO", "SNNNRLTML", "NRTMTRQTRQ", "NLQKSSPMKP", "MKQSQRPPQR",
              "LNOQTMMLSP", "LNMRMNLNTT", "LRKPNSNORL", "MPRLMQNQO", "NPMPRRLNPO", "NOSTLRKPQ",
              "POKSTQPPQ", "LOLKSRNSMR", "NKOQRKQRMT"})
            overrides.erase(token);
        overrides["RLNQONPKS"] = Vector(sunDirection); // fogSunDir
        // IW3 sky textures already carry their haze; scattering on top washed the
        // sky out (owner test, 2026-10-14). Stock skies are HDR, ours are not.
        overrides["NORQKLTPTR"] = "0"; // fogSkyIntensity
        // Shipment's veil and sun-lit volumetric haze (fields 137-147: veil .09,
        // air density .002, warm albedo, sun anisotropy .6) lit every view ray,
        // the sky most, toward the sun: "saturated by the sun yet bland" in
        // zt-28..zt-31 whatever the fog or exposure. They stay at the defaults
        // --style iw3 uses (no veil, no volumetric air).
        for (const std::string_view token :
             {"MMQLTMKTPK", "NSKNLMKQSR", "MQMPKLKK", "NKKRQROOQK", "LKNNNKTPTM", "MQSQOLLMRM",
              "MQRSTPRORQ", "NPKPLTKRNS", "MLPOMNQPPK", "MTNSRNMTKN"})
            overrides.erase(token);
        // Exposure: fixed, as --style iw3, compensated for the light-unit scale
        // (tonemapExposure is EV-like: brighter shipped maps use higher values,
        // King 6 .. Sawmill 12), so the image keeps the brightness IW3 units
        // gave at exposure 10 while the scene is in the matched stock units.
        // Stock auto exposure (the matched map's fields 63-76) over-brightened
        // the converted scenes (owner test round 85, Nuketown): the stock maps'
        // HDR skies hold metered exposure down; converted IW3 skies do not.
        // The other tonemap fields (63-76) return to the fixed-exposure defaults
        // as before the calibration: round 88 left Shipment's (template) auto
        // exposure curve in place, which metered the scene up (owner round 93:
        // "overall still too bright"; the pre-calibration look was right).
        (void)exposure;
        for (const std::string_view token :
             {"LTTOTTRLTS", "NLMSSPLMPK", "MRKQQMPTKN", "LPKQPKSRKQ", "NOOTTPORTQ", "LTNRTLQOKQ",
              "NNRKNLLSQN", "MMQMQKSTS", "LSLPLPRQNO", "NLPRMSTSMM", "MOPORNQOTQ", "MTPSRMRRLP",
              "LRPSSPPPPT", "NMKNKNMKNN"})
            overrides.erase(token);
        overrides["LPKQPKSRKQ"] = Number(10.0f + std::log2(std::max(lightUnitScale, 1e-3f))); // tonemapExposure
        // Fog: IW3's own, baked by the surface shader exactly as --style iw3
        // does it (below); the native fog pass stays off.
    }
    if (!fog)
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
