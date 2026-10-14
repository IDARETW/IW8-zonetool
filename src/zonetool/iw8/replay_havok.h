#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

namespace iw8::havok
{

// Numbered breakable panes: physicscontents_glass (0x10, Physics_CreateContents)
// and surftype_glass_pane (9). Backlot's numbered panes are glass_solid (39) with
// solid|soundclip|soundreflect: surface 39 has zero bullet_penetration depth, so
// Replay's penetrating bullet loop stops before G_Bullet_DamageGlass, and the
// collision filter only drops a destroyed piece whose filter info carries 0x10.
// Mantling or sprinting through a pane also requires 0x10 and surface 9 or 40.
inline constexpr std::uint32_t kBreakableGlassContents = 0x10u;
inline constexpr std::uint32_t kBreakableGlassSurfaceMaterial = 9u;
inline constexpr std::uint32_t kGlassMaterialNameCrc = 0xF728E572u;

struct BakeInput
{
    std::filesystem::path replayExecutable;
    std::filesystem::path collision;
    std::filesystem::path footsteps;
};

struct CollisionModel
{
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    std::uint32_t hullCount{};
    std::uint16_t shapeIndex{std::numeric_limits<std::uint16_t>::max()};
};

struct CollisionSlab
{
    std::array<float, 3> direction{};
    float midpoint{};
    float halfSize{};
};

struct CollisionHull
{
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    std::uint32_t contents{};
    std::uint32_t model{};
    std::vector<CollisionSlab> slabs;
};

struct TriggerModel
{
    std::uint32_t collisionModel{};
    bool staticPhysics{true};
};

struct LadderFace
{
    std::array<float, 3> bottom{};
    std::array<float, 3> top{};
    std::array<float, 3> normal{};
    float width{};
};

struct BakeResult
{
    std::vector<std::uint8_t> world;
    std::vector<std::uint8_t> entities;
    std::vector<CollisionModel> models;
    std::vector<CollisionHull> hulls;
    std::vector<TriggerModel> triggers;
    std::vector<LadderFace> ladders;
};

struct PhysicsMesh
{
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::uint32_t contents{1};
    // Replay surface type (0..63) reported by bullet and footstep traces.
    std::uint32_t surfaceType{};
};

// A simulated clutter body, as shipped Replay DynEnt clutter (mp_hideout
// decor_misc_props_01_bucket_metal, uk_spray_bottle_01, kitchen_01_teapot_01):
// one dynamic convex polytope with mass properties. Light props (debris)
// use simulation category 5, PBQ_Debris and PMP_Debris; heavier ones 4,
// PBQ_Dynamic and PMP_Dynamic. materialCrc names a physicsmaterial.hkx entry.
struct ClutterBody
{
    float mass{1.0f};
    std::uint32_t materialCrc{};
    bool debris{true};
};

struct PhysicsAsset
{
    std::string name;
    std::vector<std::uint8_t> havokData;
    std::uint32_t contents{1};
    std::uint32_t useCategory{3};
    std::uint32_t simulationCategory{1};
};

void PrepareCollisionBaker(const std::filesystem::path &replayExecutable);
bool FindOpaqueString(std::string_view value, std::uint32_t &id);
BakeResult BakeCollision(const BakeInput &input);
// keyframedBody stores the body contents and sentinel motion ids in the
// serialized body, as shipped KeyframedEntity (script model) assets do.
// With clutter, the body is a simulated convex hull of the mesh vertices
// (useCategory 3; the simulation category comes from the clutter class).
PhysicsAsset BakePhysicsAsset(std::string name, const PhysicsMesh &mesh,
                              std::uint32_t useCategory = 3,
                              std::uint32_t simulationCategory = 1,
                              bool keyframedBody = false,
                              const ClutterBody *clutter = nullptr);

} // namespace iw8::havok
