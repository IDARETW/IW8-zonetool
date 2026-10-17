#pragma once

#include "iw8_zone.h"
#include "replay_bounds.h"
#include "replay_render.h"

#include <array>
#include <cstdint>
#include <vector>

namespace iw8::locallights
{
// One ComWorld primary light as the converter emits it (index = ComWorld slot).
struct Light
{
    uint8_t type = 0; // 0 null, 1 sun, 2 spot, 3 omni
    bool canUseShadowMap = false;
    std::array<float, 3> origin{};
    std::array<float, 3> direction{}; // emission axis (= -ComPrimaryLight dir)
    float radius = 0;
    float cosHalfFovOuter = 0;
};

// ComPrimaryLight +3 needsDynamicShadows. Shipped mp_hideout sets it on exactly
// the shadow-mapped spots (37/37), and lightAABB lists exactly those lights.
inline bool NeedsDynamicShadows(const Light &light)
{
    return light.type == 2 && light.canUseShadowMap;
}

// GfxWorld local-light structures in the layout of shipped Replay worlds:
//  - frustumLights (+0x3CA0): per-light convex volume mesh. The light binning
//    (0x186C4F0 -> 0x186F0A0) takes a static light's view-depth range from it;
//    a light without a mesh is never binned and lights nothing.
//  - lightViewFrustums (+0x3D60): 6-plane hexahedron per shadow-mapped spot
//    (R_PrimaryLightFrustumTestSphere/Box, entity linking).
//  - lightAABB (+0x3F80): BVH over needsDynamicShadows lights
//    (R_Link*ToPrimaryLights); node 0 is the root, firstChild bit 15 = leaf.
//  - shadowGeomOptimized (+0x3F78): per-light static casters (raw surface
//    indices, remapped by 0x19296B0; static-model instance indices).
//  - shadow/motion bitset sizes (+0x3F10..+0x3F68), zero-filled in stream 4.
struct Data
{
    uint32_t lightCount = 0;
    uint32_t aligned = 0; // staticSpotOmniPrimaryLightCountAligned / shadowBitsArrayPitch
    std::vector<uint32_t> umbraLights;

    std::vector<uint32_t> indexOffsets, vertexOffsets;
    std::vector<uint16_t> indexCounts, vertexCounts, indices;
    std::vector<std::array<float, 4>> vertices;

    struct View
    {
        std::vector<std::array<float, 4>> planes;
        std::vector<uint8_t> indices;
        std::vector<std::array<float, 3>> vertices;
    };
    std::vector<View> views;

    struct Node
    {
        replaybounds::Bounds bounds{};
        uint16_t firstChild = 0, childCount = 0;
    };
    std::vector<Node> nodes;
    std::vector<uint16_t> aabbLights;

    struct Shadow
    {
        std::vector<uint32_t> surfaces;
        std::vector<uint16_t> models;
    };
    std::vector<Shadow> shadows;

    bool Empty() const
    {
        return umbraLights.empty();
    }
};

Data Build(const std::vector<Light> &lights, uint32_t lastSunPrimaryLightIndex,
           const replayrender::Mesh &mesh, const replayrender::StaticModels &staticModels);
// Stamps the GfxWorld fields; dynamic counts size the dynent shadow bitsets.
void Stamp(std::vector<uint8_t> &world, const Data &data, uint32_t dynamicModelCount,
           uint32_t dynamicBrushCount);
// Stream-4 bytes for the zero-filled bitsets Stamp declares.
size_t ZeroFillBytes(const Data &data, uint32_t dynamicModelCount, uint32_t dynamicBrushCount);
// Load_GfxWorld order: frustumLights after GfxWorldDraw, then lightViewFrustums
// (before the heightfields); shadowGeomOptimized then lightAABB before the DPVS.
void EmitFrustumLights(ZoneWriter &writer, const Data &data);
void EmitViewFrustums(ZoneWriter &writer, const Data &data);
void EmitShadowGeometry(ZoneWriter &writer, const Data &data);
void EmitLightAABB(ZoneWriter &writer, const Data &data);
} // namespace iw8::locallights
