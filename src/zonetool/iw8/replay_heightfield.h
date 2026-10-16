#pragma once

#include "iw8_zone.h"
#include "replay_bounds.h"

#include <array>
#include <cstdint>
#include <vector>

namespace iw8::heightfield
{
// GfxWorld heightfield (+0x3D80 count, +0x3D88 GfxHeightfield[]) and outdoor
// mask (+0x3E98 outdoorLookupMatrix, +0x3ED8 outdoorImage), in the layout of
// shipped Replay worlds (mp_hideout):
//  - "*heightmap0": square R8 (GfxPixelFormat 1) top-down maximum height of the
//    world geometry, normalised over the heightfield bounds. Row 0 is max Y.
//  - "$outdoor": 16x16 L8 (format 3) texels of 125, the stock placeholder.
// lookupMatrix maps a world point to (u, v, normalised z): v is flipped for the
// heightmap and unflipped for the outdoor matrix, as stock.
struct Data
{
    replaybounds::Bounds bounds{};
    std::array<float, 16> lookup{};
    std::array<float, 16> outdoorLookup{};
    unsigned size = 0;
    std::vector<uint8_t> pixels;
};

constexpr const char *kHeightmapName = "*heightmap0";
constexpr const char *kOutdoorName = "$outdoor";

// Rasterizes nine-float triangles (Mesh::solidTriangles) over `bounds`.
Data Build(const std::vector<float> &triangles, const replaybounds::Bounds &bounds);
// Registers both images; must precede the gfx_map asset.
void Register(ZoneWriter &writer, const Data &data);
// Stamps the counts, pointers and outdoor fields into the GfxWorld body.
void Stamp(std::vector<uint8_t> &world, const Data &data, const ZoneWriter &writer);
// Writes the GfxHeightfield array at its Load_GfxWorld position (after the
// light view frustums, before the brush models).
void Emit(ZoneWriter &writer, const Data &data);
} // namespace iw8::heightfield
