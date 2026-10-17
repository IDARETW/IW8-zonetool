#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iw3
{
// setExpFog(startDist, halfwayDist, r, g, b, transition) from the map's main script.
// IW3 fogs as exp(-ln2 / halfwayDist * (distance - startDist)), blended toward the
// gamma-encoded colour in the framebuffer (R_SetFrameFog / Scr_SetExponentialFog).
struct ExpFog
{
    float start{};
    float halfway{};
    std::array<float, 3> color{};
};

// The r_film* block of the map's vision file. IW3 applies it to the final
// gamma-encoded image (R_UpdateColorManipulation + the postfx film shader).
struct Film
{
    float contrast{1.0f};
    float brightness{};
    float desaturation{};
    bool invert{};
    std::array<float, 3> lightTint{1.0f, 1.0f, 1.0f};
    std::array<float, 3> darkTint{1.0f, 1.0f, 1.0f};
};

// sun/<map>.sun: the sun sprite IW3 draws in the sky (r_sun_fx_position pitch
// and yaw, r_sunsprite_size in 480-line pixels) and its flare strength
// (r_sunflare_max_alpha). Maps without one draw no sun sprite.
struct SunSprite
{
    std::array<float, 3> direction{};
    float radiusDegrees{};
    float flareAlpha{};
};

std::optional<ExpFog> ReadMapExpFog(const std::filesystem::path &exportRoot,
                                    const std::string &sourceMap);
std::optional<SunSprite> ReadMapSun(const std::filesystem::path &exportRoot,
                                    const std::string &sourceMap);
std::optional<Film> ReadMapFilm(const std::filesystem::path &exportRoot,
                                const std::string &sourceMap);

// --style. iw3: IW3's look (film colour grading, shader-baked setExpFog with
// Replay's native fog off, the baked IW3 sun mask over native shadows, IW3 sun
// specular, converted IW3 effects).
// iw8: the converted map data shown with Replay's own systems: stock fog,
// atmospheric scattering, volumetrics and auto-exposure tonemapping, no IW3
// grading, probe reflections and GGX sun specular, native sun shadows only, and
// stock Replay effects where one matches the IW3 effect.
enum class MapStyle
{
    iw3,
    iw8,
};

// --style iw8 renders in stock Replay light units, matched to the shipped maps
// (owner 2026-10-16: realistic lighting that matches shipped IW8 maps, not one
// map's sun for all). The IW3 sun is the compiled ComWorld sun light (what IW3
// shades with). Day suns: its quantile among CoD4's 19 stock day suns maps to
// the same quantile of the 11 shipped Replay day suns; night (moonlight: blue >
// 1.4 x red/green, CoD4 Cargoship and Crash Snow) interpolates toward the two
// darkest shipped references (King, Emporium). Exposure: the tonemap fields
// (vision 63-76) of the shipped map whose sun is nearest. Every IW3-unit term
// scales by lightUnitScale = sunIntensity / IW3 sun luminance.
struct Iw8LightingReference
{
    const char *map;
    float sunIntensity;
    std::array<float, 3> sunColor; // linear sRGB
    std::array<const char *, 14> tonemap; // vision fields 63-76
};
struct Iw8LightingMatch
{
    float sunIntensity{};     // ComWorld sun intensity
    float lightUnitScale{1};  // stock light units per IW3 unit
    bool night{};
    const Iw8LightingReference *exposure{};
};
Iw8LightingMatch MatchIw8Lighting(const std::array<float, 3> &iw3CompiledSun);

// HLSL preamble for the converted world/static-model pixel shader; empty without fog.
std::string FogShaderDefines(const std::optional<ExpFog> &fog, MapStyle style,
                             const std::optional<SunSprite> &sun = std::nullopt,
                             float lightUnitScale = 1.0f);

// Replay GfxGradingClut payload: 32^3 R16G16B16A16_UNORM texels (red fastest), each
// the ST2084-encoded BT.2020 output for the ST2084-encoded BT.2020 input at its grid
// point, as consumed by cs_universal_clut_generate.
inline constexpr unsigned GradingClutSize = 32;
std::vector<std::uint8_t> BuildGradingClutPixels(const Film &film);

// Replay-format vision file (all 218 fields, engine defaults unless converted).
// sunDirection points toward the sun (iw8 atmospheric scattering).
std::string BuildReplayVision(const std::optional<ExpFog> &fog, const std::string &gradingClut,
                              MapStyle style, const std::array<float, 3> &sunDirection,
                              const Iw8LightingReference *exposure = nullptr,
                              float lightUnitScale = 1.0f);
} // namespace iw3
