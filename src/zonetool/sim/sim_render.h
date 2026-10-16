#pragma once
#include "iw8/replay_render.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace sim
{
struct Camera
{
    std::array<float, 3> origin{};
    float yaw{};   // degrees, CoD convention (0 = +X, 90 = +Y)
    float pitch{}; // degrees, positive looks down
    float fov{65}; // horizontal, degrees
};

enum class View
{
    exact = 0,      // the zone's own lit shaders
    albedo,         // material colour only (debug shader)
    normals,        // shading normals
    surfaceId,      // world surface / model surface identity
    materialId,     // material identity
    lightmap,       // baked lighting only
    sunVisibility,  // baked sun visibility
    cullClass,      // static-model cullType class
    depth,
};

struct RenderOptions
{
    unsigned width = 1920, height = 1080;
    View view = View::exact;
    float exposure = 1.0f;
    bool flipWinding = false;
    bool drawWorld = true, drawModels = true, drawDynamic = true, drawGlass = true;
    int forceLod = -1;
    // Draw world surfaces and model batches in reverse order. Pixels whose owner
    // changes are depth ties the game resolves by draw order (its lit pass is
    // depth-equal), i.e. surfaces that flicker as the engine's order changes.
    bool reverseOrder = false;
    bool opaqueOnly = false; // skip blended surfaces (picking under decal layers)
    // exact view: tonemap on the GPU and read back 8-bit sRGB (Frame::rgba8)
    // instead of the HDR target (the viewer's fast path).
    bool gpuTonemap = false;
};

struct Frame
{
    unsigned width{}, height{};
    std::vector<float> rgba; // linear HDR
    std::vector<float> depth;
    std::vector<uint32_t> ids; // id views: material id, instance id per pixel
    std::vector<uint8_t> rgba8; // gpuTonemap: tonemapped sRGB, alpha 255
};

struct MaterialInfo
{
    std::string name, techset;
    bool translucent{}, opaque{}; // opaque = drawn in the depth prepass
};

struct Spawn
{
    std::string classname;
    std::array<float, 3> origin{};
    float yaw{};
};

struct Stats
{
    unsigned worldSurfaces{}, staticInstances{}, staticModels{}, dynamicEntities{},
        glassPanes{}, materials{}, pipelines{};
    unsigned drawCalls{}, triangles{};
    double frameMs{};
};

class Simulator
{
public:
    Simulator();
    ~Simulator();
    void Load(const std::string &simJsonPath);
    Frame Render(const Camera &camera, const RenderOptions &options);
    // 8-bit sRGB rows after exposure; alpha 255.
    static std::vector<uint8_t> Tonemap(const Frame &frame, float exposure);
    static std::vector<uint8_t> EncodeJpeg(const uint8_t *rgba, unsigned width, unsigned height,
                                           int quality);
    static std::vector<uint8_t> EncodePng(const uint8_t *rgba, unsigned width, unsigned height);

    const std::vector<Spawn> &Spawns() const { return spawns_; }
    const Stats &LastStats() const { return stats_; }
    const std::string &MapName() const { return map_; }
    std::array<float, 3> BoundsMin() const { return boundsMin_; }
    std::array<float, 3> BoundsMax() const { return boundsMax_; }
    // Material and surface under a rendered pixel (debug picking).
    std::string Describe(const Camera &camera, const RenderOptions &options, unsigned x,
                         unsigned y);
    MaterialInfo Material(uint32_t id) const;
    // Model name for an instance id from the id pass (empty for world surfaces).
    std::string InstanceModel(uint32_t instance) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<Spawn> spawns_;
    Stats stats_;
    std::string map_;
    std::array<float, 3> boundsMin_{}, boundsMax_{};
};
} // namespace sim
