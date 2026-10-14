#pragma once

#include "replay_bounds.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace replaysunshadow
{
struct Vertex
{
    std::array<float, 3> position{};
    std::array<float, 2> uv{};
    float alpha{1.0f};
};

struct Material
{
    unsigned cullMode{1}; // Replay: none=0, back=1, front=2.
    unsigned alphaTest{}; // none=0, >=0.5=1, >0=2, <0.5=3.
    unsigned width{}, height{};
    bool atlasTile{}; // Generated world pass samples a clamped, repeating atlas tile.
    std::vector<std::uint8_t> alpha;
};

struct Surface
{
    unsigned material{};
    std::vector<Vertex> vertices; // Expanded triangle list, in world space.
};

struct Scene
{
    std::vector<Material> materials;
    std::vector<Surface> surfaces;
};

// Replay GfxWorld +0x3860; checked loader and projection-builder layout.
struct Parameters
{
    std::array<float, 3> sunDirection{};
    std::uint32_t resolution{};
    float centerX{}, centerY{}, sampleSize{};
    float nearPlane{}, farPlane{};
    std::uint32_t forestSize{}, flags{}, reserved{};
};
static_assert(sizeof(Parameters) == 48);
static_assert(offsetof(Parameters, resolution) == 12);
static_assert(offsetof(Parameters, centerX) == 16);
static_assert(offsetof(Parameters, centerY) == 20);
static_assert(offsetof(Parameters, sampleSize) == 24);
static_assert(offsetof(Parameters, nearPlane) == 28);
static_assert(offsetof(Parameters, farPlane) == 32);
static_assert(offsetof(Parameters, forestSize) == 36);
static_assert(offsetof(Parameters, flags) == 40);
static_assert(offsetof(Parameters, reserved) == 44);

struct Data
{
    Parameters parameters;
    std::vector<std::uint8_t> bytes;
    // Occupied forest cells stored as one depth, and as quadtree tiles.
    std::uint32_t uniformCells{};
    std::uint32_t treeCells{};
};

// sampleSize is world units per shadow texel (shipped: 1.0 mp_rust, 2.0 mp_backlot2).
// With focus, only casters whose sun projection overlaps the focus box's sun
// projection are baked, and the shadow covers only that projection (huge IW3
// worlds whose play space is a small part of the terrain).
Data Bake(const Scene &scene, const std::array<float, 3> &sunDirection,
          const replaybounds::Bounds &receiverBounds, float sampleSize = 1.0f,
          const replaybounds::Bounds *focus = nullptr);

// A mini tile stores normalized occluder depth. Positive zero means no caster.
// Replay compares (projectedReceiver + origin) * inverseSpan with this depth.
std::vector<std::uint8_t> EncodeTile(std::span<const float> depths,
                                    float origin, float inverseSpan);

// Lossy tile from per-texel depth intervals, as shipped blobs are built (mp_rust:
// 83% of its occupied cells are one depth for a whole 512x512 tile). A texel
// with a caster (upper > 0) accepts any stored depth in (lower, upper]: upper is
// its first (sun-facing) surface, which stays lit; lower sits above the surface
// behind it, which stays shadowed. A quadtree node becomes one leaf when its
// texels' intervals overlap. When one depth serves the whole tile, uniform is
// set and the result is empty (the forest cell stores the depth itself).
std::vector<std::uint8_t> EncodeIntervalTile(std::span<const float> upper,
                                             std::span<const float> lower, float origin,
                                             float inverseSpan, float &uniform);

// Tiles are in row-major order; an empty vector is an unoccupied forest cell
// unless uniform (when given) holds a positive depth for it.
// The full logical resolution can exceed the cropped forest's dimensions.
std::vector<std::uint8_t> EncodeForest(
    std::uint32_t resolution, std::uint16_t width, std::uint16_t height,
    float origin, float inverseSpan,
    std::span<const std::vector<std::uint8_t>> tiles,
    std::span<const float> uniform = {});
} // namespace replaysunshadow
