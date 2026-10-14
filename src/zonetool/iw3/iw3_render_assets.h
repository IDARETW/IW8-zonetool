#pragma once

#include "common/json.hpp"
#include "../iw8/replay_sunshadow.h"

#include <array>
#include <cstddef>
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
    // IW3 polygonOffset (decals layered on coplanar geometry).
    bool depthBias{};
    unsigned sourceSurfaceTypeBits{};
    std::array<float, 4> environment{0.8f, 4.0f, 2.5f, 0.625f};
    unsigned worldMaterialIndex{};
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
    unsigned instanceLightingCount{};
    std::vector<LightmapRectangle> lightmaps;
    std::array<std::size_t, 6> skyTiles{};
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
                               const EntityLightVolume *entityLightVolume = nullptr);

// The map's IW3 loading-screen image (images/loadscreen_<map>, from its _load
// zone or IWD) as a custom-map package preview: "MWRI", version 1, width,
// height, then top-down RGBA8, at most 2048 on each side. Empty when the map
// has no loading-screen image.
std::vector<std::uint8_t> BuildLoadscreenPreview(const std::filesystem::path &exportRoot,
                                                 const std::vector<std::filesystem::path> &sourcePaths,
                                                 const std::string &sourceMap);

} // namespace iw3
