#pragma once
#include "iw8_zone.h"
#include "replay_bounds.h"
#include <array>
#include <memory>
#include <string>
#include <vector>
namespace replayrender
{
struct Shader
{
    unsigned type;
    std::string name, debugName;
    std::vector<uint8_t> header, program;
};
// Resident pixel bytes, shared by every material definition of the same image file
// (Greenband's 98 glass panes each carry the 3 x 89 MB world atlas: 30 GB as copies).
// Writes through data() copy a shared buffer first.
class ImagePixels
{
public:
    const uint8_t *data() const { return bytes ? bytes->data() : nullptr; }
    uint8_t *data() { return own().data(); }
    size_t size() const { return bytes ? bytes->size() : 0; }
    bool empty() const { return !size(); }
    void resize(const size_t count) { own().resize(count); }
    void share(std::shared_ptr<std::vector<uint8_t>> source) { bytes = std::move(source); }
    const std::shared_ptr<std::vector<uint8_t>> &shared() const { return bytes; }

private:
    std::vector<uint8_t> &own()
    {
        if (!bytes)
            bytes = std::make_shared<std::vector<uint8_t>>();
        else if (bytes.use_count() > 1)
            bytes = std::make_shared<std::vector<uint8_t>>(*bytes);
        return *bytes;
    }
    std::shared_ptr<std::vector<uint8_t>> bytes;
};
struct Image
{
    std::string name;
    uint16_t width{}, height{};
    unsigned mipCount = 1, format = 7;
    uint32_t flags = 0;
    uint16_t depth = 1, numElements = 1;
    uint8_t semantic = 1, category = 1;
    ImagePixels pixels;
};
struct Technique
{
    std::string name;
    std::vector<uint8_t> header, states, rootsig, statebits, args;
    std::array<std::string, 4> shaders;
};
struct DecalVolumeMaterial
{
    std::string name;
    std::array<std::string, 6> channels;
    uint32_t flags{};
    std::array<float, 3> colorTint{1.0f, 1.0f, 1.0f};
    float alphaDissolveParms{};
    float emissiveScale{1.0f};
    uint32_t packedDisplacementScaleAndBias{3154132992u};
    float displacementCutoffDistance{750.0f};
    float displacementCutoffFalloff{0.004f};
    uint32_t packedTemperatureBaseAndScale{48128u};
    uint8_t rows{1};
    uint8_t columns{1};
};
struct Material
{
    std::string material;
    std::vector<uint8_t> materialInfo, constants, bufferIndices, textureHeaders;
    std::string techset;
    std::vector<uint8_t> techsetHeader;
    std::vector<Shader> shaders;
    std::vector<Technique> techniques;
    std::vector<std::string> images;
    std::string decalVolumeMaterial;
    DecalVolumeMaterial decalVolumeDefinition;
    std::vector<Image> imageDefinitions;
    std::vector<std::array<std::vector<uint8_t>, 4>> buffers;
};
struct BrushModel
{
    replaybounds::Bounds bounds{};
    unsigned firstSurface = 0;
    unsigned surfaceCount = 0;
};
struct DpvsPlane
{
    std::array<float, 3> normal{};
    float distance{};
    uint8_t type{};
};
struct Portal
{
    std::array<float, 4> plane{};
    unsigned cell{};
    std::vector<std::array<float, 3>> vertices;
    std::array<std::array<float, 3>, 2> hullAxis{};
};
struct CellTree
{
    replaybounds::Bounds bounds{};
    unsigned firstSurface{};
    unsigned surfaceCount{};
    unsigned childrenOffset{};
    uint16_t childCount{};
    std::vector<uint16_t> staticModelIndexes;
    std::vector<unsigned> surfaceIndexes;
};
struct Cell
{
    replaybounds::Bounds bounds{};
    std::vector<Portal> portals;
    std::vector<CellTree> trees;
};
struct ReflectionProbe
{
    std::array<float, 3> origin{};
    replaybounds::Bounds volume{};
    Image image;
    std::array<std::array<float, 9>, 4> sh{};
};
struct NativeLightmapImage
{
    uint32_t format{};
    uint16_t width{};
    uint16_t height{};
    std::vector<uint8_t> pixels;
};
struct NativeLightmap
{
    std::array<NativeLightmapImage, 3> images;
};
struct GlassPane
{
    std::string material;
    std::string materialShattered;
    std::array<float, 3> origin{};
    std::array<float, 4> quaternion{};
    std::array<float, 4> texVecs{};
    std::array<float, 2> texCoordOrigin{};
    float halfWidth{};
    float halfHeight{};
    float halfThickness{};
};
// GfxSModelSurfMatIndirection.cullType classes of the GPU triangle/cluster
// culling (stock mp_shipment survey of 1473 surfaces, 2026-10-16): 0xEF opaque
// back-face culled; 0xFF only model decals laid on other geometry (logos,
// "_decals" meshes, licence plates: bit 4); 0x6F glass/translucent (bit 7
// clear); 0x6E double-sided (bit 0 clear). Decal and layer meshes classed as
// ordinary opaque lost triangles to occlusion against the surface under them,
// so they flickered and drew dotted (Greenband cables, posters, carpets).
inline uint8_t StaticModelCullType(const bool backfaceCull, const bool decal,
                                   const bool translucent)
{
    if (decal)
        return backfaceCull ? 0xFF : 0x7E;
    if (translucent)
        return backfaceCull ? 0x6F : 0x6E;
    return backfaceCull ? 0xEF : 0x6E;
}
struct StaticModelSurface
{
    replaybounds::Bounds bounds{};
    std::string material;
    // StaticModelCullType: two-sided surfaces must not be back-face culled.
    uint8_t cullType{0xEF};
};
struct StaticModelLod
{
    float distance{};
    std::vector<StaticModelSurface> surfaces;
};
struct StaticModel
{
    std::string name;
    replaybounds::Bounds bounds{};
    std::vector<StaticModelLod> lods;
};
struct StaticModelInstance
{
    unsigned model{};
    std::array<float, 3> origin{};
    std::array<float, 4> quaternion{0, 0, 0, 1};
    float scale{1};
};
struct StaticModels
{
    std::vector<StaticModel> models;
    std::vector<StaticModelInstance> instances;
};
struct Mesh : Material
{
    replaybounds::Bounds sceneBounds{{0, 0, 0}, {100000, 100000, 100000}};
    replaybounds::Bounds drawBounds{{0, 0, 0}, {100000, 100000, 100000}};
    std::vector<Material> additionalMaterials;
    std::vector<Material> assetMaterials;
    // Stock materials (common zones) referenced by name only.
    std::vector<std::string> stockMaterials;
    // GfxWorld sunflare_t (+0x3E28) values, in field order from spriteSize:
    // 6 floats, 2 ints (flare fades), 3 floats, 2 ints (blind fades), 3 floats,
    // 2 ints (glare fades). Empty leaves hasValidData false.
    std::vector<double> sunFlare;
    std::vector<BrushModel> brushModels;
    std::vector<DpvsPlane> planes;
    std::vector<uint16_t> nodes;
    std::vector<Cell> cells;
    std::vector<unsigned> sortedSurfaceIndexes;
    std::vector<ReflectionProbe> reflectionProbes;
    Image reflectionProbeArrayImage;
    std::vector<NativeLightmap> nativeLightmaps;
    std::vector<GlassPane> glassPanes;
    std::vector<unsigned> surfaceMaterials;
    unsigned opaqueCount = 0;
    std::vector<uint8_t> surfaces, bounds, drawSurfs, surfData, positions, aux, indices;
    // Nine floats per opaque, non-sky world-model triangle (GfxWorld heightfield).
    std::vector<float> solidTriangles;
    // The same surfaces by index: static casters for local-light shadow lists.
    std::vector<uint32_t> shadowCasterSurfaces;
    unsigned count = 0;
    unsigned words() const
    {
        return (count + 31) / 32;
    }
    unsigned worldSurfaceCount() const
    {
        return brushModels.empty() ? count : brushModels.front().surfaceCount;
    }
};
Mesh Load(const std::string &path);
void RegisterMaterial(iw8::ZoneWriter &writer, const Mesh &mesh);
void RegisterMapMaterials(iw8::ZoneWriter &main, iw8::ZoneWriter &techsets,
                          const Mesh &mesh);
void RegisterMaterialReference(iw8::ZoneWriter &writer, const std::string &name);
void RegisterReflectionProbeImage(iw8::ZoneWriter &writer, const Mesh &mesh);
void StampWorld(std::vector<uint8_t> &world, const Mesh &mesh);
void EmitSurfaces(iw8::ZoneWriter &writer, const Mesh &mesh);
void StampTransient(uint8_t *transient, const Mesh &mesh);
void EmitVertices(iw8::ZoneWriter &writer, const Mesh &mesh);
void EmitSortedSurfaces(iw8::ZoneWriter &writer, const Mesh &mesh,
                        const StaticModels &staticModels);
void StampStaticModels(std::vector<uint8_t> &world, const StaticModels &models);
void EmitStaticModels(iw8::ZoneWriter &writer, const StaticModels &models,
                      unsigned lastSunPrimaryLightIndex);
std::vector<std::vector<replaybounds::Bounds>>
BuildCellTreeBounds(const Mesh &mesh, const StaticModels &models);
void StampReflectionProbes(std::vector<uint8_t> &world, const Mesh &mesh,
                           const iw8::ZoneWriter &writer);
void EmitReflectionProbes(iw8::ZoneWriter &writer, const Mesh &mesh);
// World-space bounds of one static-model instance.
replaybounds::Bounds StaticModelWorldBounds(const StaticModels &models, size_t instance);
// primaryLights: static local-light indices registered as Umbra light objects.
std::vector<uint8_t> BuildUmbraTome(const Mesh &mesh, const StaticModels &staticModels,
                                    const std::vector<uint32_t> &primaryLights = {},
                                    uint32_t reflectionProbeInstances = 0);
} // namespace replayrender
