#pragma once

#include "common/json.hpp"
#include "../iw8/replay_sunshadow.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <set>
#include <unordered_map>
#include <vector>

namespace iw3
{
enum class SurfaceKind : unsigned
{
    opaque,
    sky,
    glass,
    cutout,
    skipped,
};

struct MaterialPlan
{
    std::size_t tile{};
    SurfaceKind kind{SurfaceKind::opaque};
    bool castsShadow{true};
    unsigned flags{};
    // IW3 cull state: 0 none, 1 back, 2 front.
    unsigned cullMode{1};
    // IW3 polygonOffset or a decal sort (decals layered on coplanar geometry).
    bool depthBias{};
    // Blended layer: 0 alpha, 1 additive, 2 multiply, 3 srcalpha additive.
    unsigned blend{};
    unsigned sourceSurfaceTypeBits{};
    std::array<float, 4> environment{0.8f, 4.0f, 2.5f, 0.625f};
    // Model falloff layers: row of RenderPlan::modelFalloffDefines (0 = the
    // stock ramp), packed into bits 10-15 of the model vertex metadata.
    unsigned falloffIndex{};
    unsigned worldMaterialIndex{};
    // Atlas world surfaces with an IW3 detail map: row of RenderPlan::worldDetails
    // (a "_detail<row>" world material variant), -1 for none.
    int worldDetail{-1};
    // IW3 wc_water world surfaces: row of RenderPlan::worldWaters (a
    // "_water<row>" world material variant), -1 for none.
    int worldWater{-1};
    std::string modelMaterial;
    // DObj-path (script, dynamic and effect XModel) material.
    std::string entityMaterial;
    std::string glassMaterial;
};

// World cutout surfaces share generated atlas materials. IW3 double-sided
// lattices are two coplanar faces, so back-face culling must survive per
// material, as must decal depth bias.
inline std::string CutoutVariantKind(const MaterialPlan &material)
{
    return std::string("foliage") + (material.cullMode == 1 ? "_back" : "") +
           (material.depthBias ? "_bias" : "");
}

// World blended surfaces share generated atlas materials per blend and depth bias.
inline std::string GlassVariantKind(const MaterialPlan &material)
{
    return std::string("glass") + (material.blend ? "_b" + std::to_string(material.blend) : "") +
           (material.depthBias ? "_bias" : "");
}

// Replay GfxBlendStateBits: srcRgb | dstRgb << 4 | opRgb << 8 | srcAlpha << 11 |
// dstAlpha << 15 | opAlpha << 19 (source ONE = 0, ZERO = 1, SRCCOLOR = 2,
// SRCALPHA = 4, INVSRCALPHA = 5; destination ZERO = 0, ONE = 1, SRCCOLOR = 2).
inline std::uint64_t GlassBlendBits(const unsigned blend)
{
    switch (blend)
    {
    case 1: return 0x8010;  // one / one
    case 2: return 0x8821;  // zero / srccolor (alpha zero / one)
    case 3: return 0x8010;  // srcalpha / one: one / one, the shader applies alpha
    default: return 0x28050; // srcalpha / invsrcalpha (alpha one / invsrcalpha)
    }
}

struct LightmapRectangle
{
    unsigned x{};
    unsigned y{};
    unsigned width{};
    unsigned height{};
};

struct ReflectionProbePlan
{
    std::size_t sourceIndex{};
    std::array<float, 3> origin{};
    nlohmann::json image;
    std::array<std::array<float, 9>, 4> sh{};
};

struct NativeLightmapPlan
{
    unsigned width{};
    unsigned height{};
    std::array<unsigned, 3> formats{39u, 32u, 40u};
    std::array<std::string, 3> files;
};

// World-space IW3 light-grid volume for entity (DObj) materials: an RGBA8 atlas
// (iw3_lightgrid.h LightGridVolume) bound as the fourth texture, and the pixel
// shader defines that locate it (MAP_LIGHT_VOLUME_*).
struct EntityLightVolume
{
    std::string image;
    std::string file;
    unsigned width{};
    unsigned height{};
    std::string defines;
};

struct RenderPlan
{
    std::unordered_map<std::string, MaterialPlan> materials;
    std::unordered_map<std::string, std::string> fxMaterialAliases;
    // Replay Particle_MaterialData (ParticleModuleInitMaterial +0x20, 192 bytes)
    // for IW3 FX materials with feather, falloff or eye-offset terms.
    std::unordered_map<std::string, std::array<std::uint8_t, 192>> fxParticleMaterialData;
    // IW3 distortion materials. Their elements draw with a stock Replay
    // distortion material (common_mp, always loaded in MP) and the stock
    // distortion particle state.
    std::set<std::string> fxDistortionMaterials;
    // Stock Replay materials the map references by name (",<name>").
    std::set<std::string> stockMaterials;
    // Per-instance IW3 light-grid lighting for static placements (6 RGBA8
    // texels per instance, 256 instances per row; written by PrepareFastfile).
    std::string instanceLightingImage;
    std::string instanceLightingFile;
    // Baked world-space sun depth (replaysunshadow::BakeDepthImage), sampled by
    // the world, static-model and entity shaders; written by PrepareFastfile.
    std::string sunDepthImage;
    std::string sunDepthFile;
    unsigned instanceLightingCount{};
    std::vector<LightmapRectangle> lightmaps;
    std::array<std::size_t, 6> skyTiles{};
    // IW3 world detail maps (wc_*d0*): the detail image's own colour-atlas tile
    // and detailScale.xy, one world technique variant each.
    struct WorldDetail
    {
        std::size_t tile{};
        std::array<float, 2> scale{1, 1};
    };
    std::vector<WorldDetail> worldDetails;
    // IW3 wc_water: the baked wave-map flipbook (material slot 0x3D, t10) and
    // the water_l_sun constants, one world technique variant each.
    struct WorldWater
    {
        std::string image;
        std::string file;
        std::array<float, 4> environment{};
        std::array<float, 3> color{};
    };
    std::vector<WorldWater> worldWaters;
    unsigned columns{};
    // Pixel size of the square source atlas (ATLAS_SIZE in the shaders).
    unsigned atlasSize{4096};
    bool hasCutout{};
    bool hasCutoutBias{};
    bool hasOpaqueBias{};
    bool hasGlass{};
    std::string material;
    std::string materialDefinition;
    std::vector<nlohmann::json> additionalMaterials;
    std::vector<nlohmann::json> assetMaterials;
    std::vector<ReflectionProbePlan> reflectionProbes;
    nlohmann::json reflectionProbeArrayImage;
    // HLSL define: map-average reflection SH evaluated by translucent glass.
    std::string glassEnvironment;
    // HLSL defines for the model vertex shaders: the per-material IW3 falloff
    // parameters, near fade and eye offset (MaterialPlan::falloffIndex rows).
    std::string modelFalloffDefines;
    std::vector<NativeLightmapPlan> nativeLightmaps;
    replaysunshadow::Scene shadowScene;
    std::unordered_map<std::string, unsigned> worldShadowMaterials;
    std::unordered_map<std::string, unsigned> modelShadowMaterials;
};

RenderPlan PrepareRenderAssets(const std::filesystem::path &exportRoot,
                               const nlohmann::json &world,
                               const std::vector<std::string> &surfaceMaterials,
                               const std::vector<std::string> &worldMaterials,
                               const std::vector<std::string> &modelMaterials,
                               const std::vector<std::string> &entityMaterials,
                               const std::vector<std::string> &fxMaterials,
                               const std::vector<std::string> &shadowWorldMaterials,
                               const std::vector<std::string> &shadowModelMaterials,
                               const std::vector<std::filesystem::path> &sourcePaths,
                               const std::filesystem::path &mapDirectory,
                               const std::string &map,
                               const std::filesystem::path &cacheRoot = {},
                               const std::string &surfaceShaderDefines = {},
                               unsigned staticModelInstances = 0,
                               const EntityLightVolume *entityLightVolume = nullptr,
                               float lightUnitScale = 1.0f, bool iw3Effects = false);

// The map's IW3 loading-screen image (images/loadscreen_<map>, from its _load
// zone or IWD) as a custom-map package preview: "MWRI", version 1, width,
// height, then top-down RGBA8, at most 2048 on each side. Empty when the map
// has no loading-screen image.
std::vector<std::uint8_t> BuildLoadscreenPreview(const std::filesystem::path &exportRoot,
                                                 const std::vector<std::filesystem::path> &sourcePaths,
                                                 const std::string &sourceMap);

} // namespace iw3
