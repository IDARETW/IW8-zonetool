#pragma once

#include "common/json.hpp"
#include "../iw8/replay_sunshadow.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
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

struct RenderPlan
{
    std::unordered_map<std::string, MaterialPlan> materials;
    std::unordered_map<std::string, std::string> fxMaterialAliases;
    std::vector<LightmapRectangle> lightmaps;
    std::array<std::size_t, 6> skyTiles{};
    unsigned columns{};
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
                               const std::filesystem::path &cacheRoot = {});

std::string RegisterGlassUvRemap(RenderPlan &plan,
                                 const std::filesystem::path &mapDirectory,
                                 const std::string &map,
                                 const std::string &material,
                                 const std::array<float, 6> &uvAffine);
} // namespace iw3
