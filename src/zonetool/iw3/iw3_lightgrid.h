#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace iw3
{
bool PrepareNativeLightGrid(const std::filesystem::path &sourceRoot,
                            const std::string &sourceAsset,
                            const std::filesystem::path &output, float lightUnitScale = 1.0f);

// IW3 lights a static model from its light grid at the model's lighting origin.
// For each point: the grid trilinearly interpolated (missing cells skipped, else
// the nearest cell within 8 steps), as an ambient cube of shading values in IW3
// light-grid units (the units the world lightmaps use) for +X,-X,+Y,-Y,+Z,-Z.
// Empty when the map has no exported light grid.
// With sunVisibility, also each point's IW3 sun visibility: the interpolated
// share of grid cells whose primary light is the sun (IW3 lit a static model
// by the sun only where its grid point's primary light was the sun).
std::vector<std::array<std::array<float, 3>, 6>> SampleLightGridAmbientCubes(
    const std::filesystem::path &sourceRoot, const std::string &sourceAsset,
    const std::vector<std::array<float, 3>> &points, unsigned sunPrimaryLight = 0,
    std::vector<float> *sunVisibility = nullptr);

// IW3 R_SetStaticModelLighting, ported: each static model's 4x4x4 model-lighting
// volume as IW3 uploads it (R_SetLightGridColors: x fastest, then y, then z; the
// outer shell holds the 56 grid directions, the inner 2x2x2 core copies corner
// samples), RGBA8 with A = the primary-light (sun) weight. Ground-lit models
// (XModel flags bit 0 with a non-zero groundLighting) fill every texel with that
// colour (bytes B, G, R, A as IW3 stores it); the rest blend the grid at the
// lighting origin with R_GetLightingAtPoint's corner weights, trace suppression,
// fixed-point colour blend and extrapolation. Empty when the map has no grid.
struct StaticModelLightingInput
{
    std::array<float, 3> origin{}; // avg(mins, maxs)
    std::uint8_t primaryLight{};   // GfxStaticModelDrawInst::primaryLightIndex
    bool groundLit{};
    std::uint32_t groundLighting{}; // GfxColor packed
};
struct LightGridPrimaryLight
{
    unsigned type{}; // 1 dir, 2 spot, 3 omni
    std::array<float, 3> origin{}, dir{};
    float radius{}, cosHalfFovOuter{}, rotationLimit{1};
};
// CM_BoxSightTrace(start, end, contents 0x2001) != 0.
using LightGridSightBlocked =
    std::function<bool(const std::array<float, 3> &start, const std::array<float, 3> &end)>;
std::vector<std::array<std::uint8_t, 256>> SampleStaticModelLighting(
    const std::filesystem::path &sourceRoot, const std::string &sourceAsset,
    const std::vector<StaticModelLightingInput> &inputs,
    const std::vector<LightGridPrimaryLight> &primaryLights, const LightGridSightBlocked &blocked);

// The IW3 light grid for entity (DObj) shading, as IW3's R_GetLightingAtPoint
// reads it: one RGBA8 image of `width` texels per row holding first the grid
// entries over [minimum, minimum + dims) (texel x + dims.x * (y + dims.y * z):
// R + 256 G = palette colour + 1, 0 = no entry; B = primary light), then from
// texel `paletteStart` the palette's 56 colours per entry (RGB). The entity
// pixel shader blends the 8 corners at the model's lighting origin and samples
// the 4x4x4 model-lighting volume by normal, as the lit model shaders do.
struct EntityLightGrid
{
    std::array<int, 3> minimum{}; // grid coordinates (32, 32, 64 units from -131072)
    std::array<unsigned, 3> dims{};
    unsigned width{};
    unsigned height{};
    unsigned paletteStart{};
    std::size_t paletteColors{}; // 56 texels each from paletteStart
    std::vector<std::uint8_t> pixels;
};
std::optional<EntityLightGrid> BuildEntityLightGrid(const std::filesystem::path &sourceRoot,
                                                    const std::string &sourceAsset);

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
