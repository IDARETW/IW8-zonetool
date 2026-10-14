#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iw3
{
bool PrepareNativeLightGrid(const std::filesystem::path &sourceRoot,
                            const std::string &sourceAsset,
                            const std::filesystem::path &output);

// IW3 lights a static model from its light grid at the model's lighting origin.
// For each point: the grid trilinearly interpolated (missing cells skipped, else
// the nearest cell within 8 steps), as an ambient cube of shading values in IW3
// light-grid units (the units the world lightmaps use) for +X,-X,+Y,-Y,+Z,-Z.
// Empty when the map has no exported light grid.
std::vector<std::array<std::array<float, 3>, 6>> SampleLightGridAmbientCubes(
    const std::filesystem::path &sourceRoot, const std::string &sourceAsset,
    const std::vector<std::array<float, 3>> &points);

// The IW3 light grid as a world-space L1 SH volume for moving or scripted
// models (IW3 lit those from the grid at their lighting origin). Cells hold
// the grid's SH (L0 + L1, IW3 light-grid units), empty cells are filled from
// their neighbours. Packed as a 2D RGBA8 atlas: three coefficient groups x Z
// slices, each an (x + 2) x (y + 2) tile with a one-texel border, tilesPerRow
// tiles per row. Group 0 = (L0 r,g,b, L1x r), 1 = (L1y r, L1z r, L1x g, L1y g),
// 2 = (L1z g, L1x b, L1y b, L1z b); L0 / 8 and L1 / 8 + 0.5. The basis is
// EvaluateShBasis: L1x = -0.488603 y, L1y = 0.488603 z, L1z = -0.488603 x.
struct LightGridVolume
{
    std::array<float, 3> origin{}; // world position of cell (0,0,0)
    std::array<float, 3> cell{};   // cell size
    std::array<unsigned, 3> dims{};
    unsigned tilesPerRow{};
    unsigned width{};
    unsigned height{};
    std::vector<std::uint8_t> pixels;
};
std::optional<LightGridVolume> BuildLightGridVolume(const std::filesystem::path &sourceRoot,
                                                    const std::string &sourceAsset);
}
