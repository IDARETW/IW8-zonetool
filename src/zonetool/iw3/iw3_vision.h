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

std::optional<ExpFog> ReadMapExpFog(const std::filesystem::path &exportRoot,
                                    const std::string &sourceMap);
std::optional<Film> ReadMapFilm(const std::filesystem::path &exportRoot,
                                const std::string &sourceMap);

// iw3: IW3's look (shader-baked setExpFog, Replay's native fog off).
// iw8: Replay's native atmospheric fog and volumetric scattering (godrays) from a
// stock vision template; the surface shader adds no fog of its own.
enum class LightingStyle
{
    iw3,
    iw8,
};

// HLSL preamble for the converted world/static-model pixel shader; empty without fog.
std::string FogShaderDefines(const std::optional<ExpFog> &fog, LightingStyle style);

// Replay GfxGradingClut payload: 32^3 R16G16B16A16_UNORM texels (red fastest), each
// the ST2084-encoded BT.2020 output for the ST2084-encoded BT.2020 input at its grid
// point, as consumed by cs_universal_clut_generate.
inline constexpr unsigned GradingClutSize = 32;
std::vector<std::uint8_t> BuildGradingClutPixels(const Film &film);

// Replay-format vision file (all 218 fields, engine defaults unless converted).
// sunDirection points toward the sun (iw8 atmospheric scattering).
std::string BuildReplayVision(const std::optional<ExpFog> &fog, const std::string &gradingClut,
                              LightingStyle style, const std::array<float, 3> &sunDirection);
} // namespace iw3
