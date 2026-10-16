#include "replay_locallights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace iw8::locallights
{
namespace
{
using Vec3 = std::array<float, 3>;

template <class T> void Store(uint8_t *record, const size_t offset, const T value)
{
    std::memcpy(record + offset, &value, sizeof(value));
}

// Replay GfxWorld offsets (Load_GfxWorld 0xD92E80; ACTS Replay::GfxWorld).
constexpr size_t kFrustumLights = 0x3CA0;
constexpr size_t kLightViewFrustums = 0x3D60;
constexpr size_t kMotionDetectEntries = 0x3F10;
constexpr size_t kMotionDetectBits = 0x3F18;
constexpr size_t kEntityMotionEntries = 0x3F20;
constexpr size_t kSpotOmniAligned = 0x3F30;
constexpr size_t kShadowBitsPitch = 0x3F34;
constexpr size_t kEntityShadowVisEntries = 0x3F38;
constexpr size_t kEntityShadowVis = 0x3F40;
constexpr size_t kDynEntShadowVisEntries = 0x3F60;
constexpr size_t kDynEntShadowVis = 0x3F68;
constexpr size_t kShadowGeomOptimized = 0x3F78;
constexpr size_t kLightAABB = 0x3F80;
// R_EntityMoved indexes localClient * 80 + entityNum / 32 (write_maps stamps 160).
constexpr uint32_t kEntityMotionWords = 160;

constexpr unsigned kSegments = 16;
constexpr double kMaxCapStep = std::numbers::pi / 12; // 15 degrees per cap ring
constexpr size_t kMaxLeafLights = 16;

double Dot(const Vec3 &a, const Vec3 &b)
{
    return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
}

Vec3 Basis(const Vec3 &direction, Vec3 &v)
{
    const Vec3 reference = std::abs(direction[2]) < 0.999f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    Vec3 u{float(reference[1] * direction[2] - reference[2] * direction[1]),
           float(reference[2] * direction[0] - reference[0] * direction[2]),
           float(reference[0] * direction[1] - reference[1] * direction[0])};
    const double length = std::sqrt(Dot(u, u));
    for (auto &c : u)
        c = float(c / length);
    v = {direction[1] * u[2] - direction[2] * u[1], direction[2] * u[0] - direction[0] * u[2],
         direction[0] * u[1] - direction[1] * u[0]};
    return u;
}

Vec3 Point(const Light &light, const Vec3 &u, const Vec3 &v, const double polar,
           const double azimuth, const double distance)
{
    Vec3 p{};
    for (unsigned k = 0; k < 3; ++k)
        p[k] = float(light.origin[k] +
                     distance * (std::cos(polar) * light.direction[k] +
                                 std::sin(polar) * (std::cos(azimuth) * u[k] +
                                                    std::sin(azimuth) * v[k])));
    return p;
}

// Appends triangles so that (b - a) x (c - a) points into the convex volume,
// the winding of every shipped frustum-light and view-frustum triangle.
template <class Index>
void Triangle(std::vector<Index> &out, const std::vector<Vec3> &points, const Vec3 &inside,
              unsigned a, unsigned b, unsigned c)
{
    Vec3 e1, e2, to;
    for (unsigned k = 0; k < 3; ++k)
    {
        e1[k] = points[b][k] - points[a][k];
        e2[k] = points[c][k] - points[a][k];
        to[k] = inside[k] - points[a][k];
    }
    const Vec3 n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                 e1[0] * e2[1] - e1[1] * e2[0]};
    if (Dot(n, to) < 0)
        std::swap(b, c);
    out.push_back(Index(a));
    out.push_back(Index(b));
    out.push_back(Index(c));
}

// Convex polyhedron circumscribing the light's reach: a spot is the cone of
// its outer angle capped by the radius sphere; an omni is the sphere. Polygon
// faces lie outside the true surface (angle widened by 1/cos(pi/N), radius by
// the chord factors), so binning never loses a lit pixel.
void VolumeMesh(const Light &light, std::vector<Vec3> &points, std::vector<uint16_t> &triangles)
{
    Vec3 v;
    const Vec3 u = Basis(light.direction, v);
    const double ring = std::cos(std::numbers::pi / kSegments);
    if (light.type == 2)
    {
        const double outer = std::acos(std::clamp<double>(light.cosHalfFovOuter, 0.0175, 1.0));
        const double polar = std::atan(std::tan(outer) / ring);
        const unsigned rings = std::max(1u, unsigned(std::ceil(polar / kMaxCapStep)));
        const double step = polar / rings;
        const double reach = light.radius / (ring * std::cos(step * .5));
        points.push_back(light.origin);                                // 0 apex
        points.push_back(Point(light, u, v, 0, 0, reach));             // 1 cap pole
        for (unsigned r = 1; r <= rings; ++r)
            for (unsigned s = 0; s < kSegments; ++s)
                points.push_back(Point(light, u, v, step * r,
                                       2 * std::numbers::pi * s / kSegments, reach));
        Vec3 inside{};
        for (unsigned k = 0; k < 3; ++k)
            inside[k] = float(light.origin[k] + light.direction[k] * reach * .5 * std::cos(polar));
        const auto at = [](unsigned r, unsigned s) { return 2 + (r - 1) * kSegments + s % kSegments; };
        for (unsigned s = 0; s < kSegments; ++s)
        {
            Triangle(triangles, points, inside, 1, at(1, s), at(1, s + 1));
            for (unsigned r = 1; r < rings; ++r)
            {
                Triangle(triangles, points, inside, at(r, s), at(r + 1, s), at(r + 1, s + 1));
                Triangle(triangles, points, inside, at(r, s), at(r + 1, s + 1), at(r, s + 1));
            }
            Triangle(triangles, points, inside, 0, at(rings, s + 1), at(rings, s));
        }
        return;
    }
    constexpr unsigned rings = 8;
    const double step = std::numbers::pi / rings;
    const double reach = light.radius / (ring * std::cos(step * .5));
    points.push_back(Point(light, u, v, 0, 0, reach));
    points.push_back(Point(light, u, v, std::numbers::pi, 0, reach));
    for (unsigned r = 1; r < rings; ++r)
        for (unsigned s = 0; s < kSegments; ++s)
            points.push_back(
                Point(light, u, v, step * r, 2 * std::numbers::pi * s / kSegments, reach));
    const auto at = [](unsigned r, unsigned s) { return 2 + (r - 1) * kSegments + s % kSegments; };
    for (unsigned s = 0; s < kSegments; ++s)
    {
        Triangle(triangles, points, light.origin, 0, at(1, s), at(1, s + 1));
        for (unsigned r = 1; r + 1 < rings; ++r)
        {
            Triangle(triangles, points, light.origin, at(r, s), at(r + 1, s), at(r + 1, s + 1));
            Triangle(triangles, points, light.origin, at(r, s), at(r + 1, s + 1), at(r, s + 1));
        }
        Triangle(triangles, points, light.origin, 1, at(rings - 1, s + 1), at(rings - 1, s));
    }
}

// Shadow view frustum of a spot: square pyramid circumscribing the outer cone,
// from one unit past the light to its radius. Planes keep the inside positive
// (n.p + w >= 0), as R_PrimaryLightFrustumTestSphere requires.
Data::View ViewFrustum(const Light &light)
{
    Vec3 v;
    const Vec3 u = Basis(light.direction, v);
    const double outer = std::acos(std::clamp<double>(light.cosHalfFovOuter, 0.0175, 1.0));
    const double slope = std::tan(outer);
    Data::View view;
    std::vector<Vec3> corners;
    for (const double depth : {1.0, double(light.radius)})
        for (const int a : {-1, 1})
            for (const int b : {-1, 1})
            {
                Vec3 p{};
                for (unsigned k = 0; k < 3; ++k)
                    p[k] = float(light.origin[k] + depth * light.direction[k] +
                                 depth * slope * (a * u[k] + b * v[k]));
                corners.push_back(p);
            }
    view.vertices = corners;
    const auto plane = [&](const Vec3 &normal) {
        const double length = std::sqrt(Dot(normal, normal));
        return Vec3{float(normal[0] / length), float(normal[1] / length), float(normal[2] / length)};
    };
    const double along = Dot(light.direction, light.origin);
    view.planes.push_back({light.direction[0], light.direction[1], light.direction[2],
                           float(-(along + 1.0))});
    view.planes.push_back({-light.direction[0], -light.direction[1], -light.direction[2],
                           float(along + light.radius)});
    for (const Vec3 &side : {u, v})
        for (const int sign : {-1, 1})
        {
            Vec3 n{};
            for (unsigned k = 0; k < 3; ++k)
                n[k] = float(light.direction[k] * slope - sign * side[k]);
            n = plane(n);
            view.planes.push_back({n[0], n[1], n[2], float(-Dot(n, light.origin))});
        }
    Vec3 inside{};
    for (unsigned k = 0; k < 3; ++k)
        inside[k] = float(light.origin[k] + light.direction[k] * (1.0 + light.radius) * .5);
    // Corner index = depth * 4 + (a > 0) * 2 + (b > 0); six quads, two triangles each.
    constexpr unsigned faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                      {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (const auto &face : faces)
    {
        Triangle(view.indices, corners, inside, face[0], face[1], face[2]);
        Triangle(view.indices, corners, inside, face[0], face[2], face[3]);
    }
    return view;
}

bool Reaches(const Light &light, const replaybounds::Bounds &bounds)
{
    Vec3 offset{};
    for (unsigned k = 0; k < 3; ++k)
        offset[k] = bounds.midpoint[k] - light.origin[k];
    const double sphere = std::sqrt(Dot(bounds.halfSize, bounds.halfSize));
    const double distance = std::sqrt(Dot(offset, offset));
    if (distance > light.radius + sphere)
        return false;
    if (light.type != 2)
        return true;
    const double axial = Dot(offset, light.direction);
    if (axial < -sphere)
        return false;
    const double radial = std::sqrt(std::max(0.0, distance * distance - axial * axial));
    const double outer = std::acos(std::clamp<double>(light.cosHalfFovOuter, 0.0175, 1.0));
    return radial * std::cos(outer) - axial * std::sin(outer) <= sphere;
}

replaybounds::Bounds BoundsOf(const std::vector<Vec3> &points)
{
    replaybounds::Accumulator accumulator;
    for (const auto &p : points)
        accumulator.Add(p);
    return accumulator.Finish();
}

replaybounds::Bounds Union(const replaybounds::Bounds &a, const replaybounds::Bounds &b)
{
    std::vector<Vec3> corners;
    for (const auto *box : {&a, &b})
        for (const int sign : {-1, 1})
            corners.push_back({box->midpoint[0] + sign * box->halfSize[0],
                               box->midpoint[1] + sign * box->halfSize[1],
                               box->midpoint[2] + sign * box->halfSize[2]});
    return BoundsOf(corners);
}
} // namespace

Data Build(const std::vector<Light> &lights, const uint32_t lastSunPrimaryLightIndex,
           const replayrender::Mesh &mesh, const replayrender::StaticModels &staticModels)
{
    Data data;
    data.lightCount = static_cast<uint32_t>(lights.size());
    data.indexOffsets.assign(lights.size(), 0);
    data.vertexOffsets.assign(lights.size(), 0);
    data.indexCounts.assign(lights.size(), 0);
    data.vertexCounts.assign(lights.size(), 0);
    data.views.resize(lights.size());
    data.shadows.resize(lights.size());
    std::vector<replaybounds::Bounds> lightBounds(lights.size());
    for (uint32_t index = 0; index < lights.size(); ++index)
    {
        const Light &light = lights[index];
        if (light.type != 2 && light.type != 3)
            continue;
        if (index <= lastSunPrimaryLightIndex)
            throw std::runtime_error("Replay local lights must follow the sun primary light");
        if (!std::isfinite(light.radius) || light.radius <= 0 ||
            std::abs(Dot(light.direction, light.direction) - 1.0) > 1e-3)
            throw std::runtime_error("invalid local primary light for GfxWorld light data");
        std::vector<Vec3> points;
        std::vector<uint16_t> triangles;
        VolumeMesh(light, points, triangles);
        data.indexOffsets[index] = static_cast<uint32_t>(data.indices.size());
        data.vertexOffsets[index] = static_cast<uint32_t>(data.vertices.size());
        data.indexCounts[index] = static_cast<uint16_t>(triangles.size());
        data.vertexCounts[index] = static_cast<uint16_t>(points.size());
        data.indices.insert(data.indices.end(), triangles.begin(), triangles.end());
        for (const auto &p : points)
            data.vertices.push_back({p[0], p[1], p[2], 1.0f});
        lightBounds[index] = BoundsOf(points);
        data.umbraLights.push_back(index);
        if (NeedsDynamicShadows(light))
        {
            data.views[index] = ViewFrustum(light);
            data.aabbLights.push_back(static_cast<uint16_t>(index));
        }
        auto &shadow = data.shadows[index];
        for (const uint32_t surface : mesh.shadowCasterSurfaces)
        {
            replaybounds::Bounds box;
            std::memcpy(box.midpoint.data(), mesh.bounds.data() + 56 * size_t(surface), 12);
            std::memcpy(box.halfSize.data(), mesh.bounds.data() + 56 * size_t(surface) + 12, 12);
            if (shadow.surfaces.size() < UINT16_MAX && Reaches(light, box))
                shadow.surfaces.push_back(surface);
        }
        for (size_t instance = 0; instance < staticModels.instances.size(); ++instance)
            if (shadow.models.size() < UINT16_MAX &&
                Reaches(light, replayrender::StaticModelWorldBounds(staticModels, instance)))
                shadow.models.push_back(static_cast<uint16_t>(instance));
    }
    if (data.umbraLights.empty())
        return data;
    if (data.vertices.size() > UINT32_MAX / 16 || data.indices.size() > UINT32_MAX)
        throw std::runtime_error("GfxWorld frustum-light mesh is too large");
    const uint32_t localCount = data.lightCount - (lastSunPrimaryLightIndex + 1);
    data.aligned = (localCount + 31u) & ~31u;

    // BVH: one leaf root when it fits, else a root over leaves of nearby lights.
    if (!data.aabbLights.empty())
    {
        auto order = data.aabbLights;
        replaybounds::Bounds all = lightBounds[order.front()];
        for (const auto light : order)
            all = Union(all, lightBounds[light]);
        if (order.size() <= kMaxLeafLights)
        {
            data.nodes.push_back({all, 0x8000, static_cast<uint16_t>(order.size())});
        }
        else
        {
            const unsigned axis = unsigned(std::max_element(all.halfSize.begin(), all.halfSize.end()) -
                                           all.halfSize.begin());
            std::stable_sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) {
                return lightBounds[a].midpoint[axis] < lightBounds[b].midpoint[axis];
            });
            const size_t leaves = (order.size() + kMaxLeafLights - 1) / kMaxLeafLights;
            if (leaves + 1 > 0x7FFF)
                throw std::runtime_error("too many dynamic-shadow lights for lightAABB");
            data.nodes.push_back({all, 1, static_cast<uint16_t>(leaves)});
            for (size_t first = 0; first < order.size(); first += kMaxLeafLights)
            {
                const size_t count = std::min(kMaxLeafLights, order.size() - first);
                replaybounds::Bounds leaf = lightBounds[order[first]];
                for (size_t k = first; k < first + count; ++k)
                    leaf = Union(leaf, lightBounds[order[k]]);
                data.nodes.push_back(
                    {leaf, static_cast<uint16_t>(0x8000 | first), static_cast<uint16_t>(count)});
            }
        }
        data.aabbLights = order;
    }
    return data;
}

void Stamp(std::vector<uint8_t> &world, const Data &data, const uint32_t dynamicModelCount,
           const uint32_t dynamicBrushCount)
{
    if (data.Empty())
        return;
    uint8_t *w = world.data();
    Store(w, kFrustumLights + 0x00, data.lightCount);
    Store(w, kFrustumLights + 0x04, static_cast<uint32_t>(data.indices.size()));
    Store(w, kFrustumLights + 0x08, static_cast<uint32_t>(data.vertices.size()));
    for (size_t pointer = 0x10; pointer <= 0x38; pointer += 8)
        Store(w, kFrustumLights + pointer, PTR_FOLLOWS);
    Store(w, kLightViewFrustums, PTR_FOLLOWS);
    // Shipped mp_hideout (124 local lights): aligned 128, pitch 128, motion
    // detect 8 entries, entity shadow vis 160 * 128, dynent vis count * 8.
    Store(w, kMotionDetectEntries, data.aligned / 16);
    Store(w, kMotionDetectBits, PTR_FOLLOWS);
    Store(w, kSpotOmniAligned, data.aligned);
    Store(w, kShadowBitsPitch, data.aligned);
    Store(w, kEntityShadowVisEntries, kEntityMotionWords * data.aligned);
    Store(w, kEntityShadowVis, PTR_FOLLOWS);
    const std::array<uint32_t, 2> dynamic{dynamicModelCount, dynamicBrushCount};
    for (size_t basis = 0; basis < 2; ++basis)
    {
        const uint32_t entries = dynamic[basis] * (data.aligned / 16);
        Store(w, kDynEntShadowVisEntries + basis * 4, entries);
        if (entries)
            Store(w, kDynEntShadowVis + basis * 8, PTR_FOLLOWS);
    }
    Store(w, kShadowGeomOptimized, PTR_FOLLOWS);
    if (!data.nodes.empty())
    {
        Store(w, kLightAABB + 0x00, static_cast<uint16_t>(data.nodes.size()));
        Store(w, kLightAABB + 0x02, static_cast<uint16_t>(data.aabbLights.size()));
        Store(w, kLightAABB + 0x08, PTR_FOLLOWS);
        Store(w, kLightAABB + 0x10, PTR_FOLLOWS);
    }
}

size_t ZeroFillBytes(const Data &data, const uint32_t dynamicModelCount,
                     const uint32_t dynamicBrushCount)
{
    if (data.Empty())
        return 0;
    // Each family is a separate 4-aligned uint allocation in stream 4.
    return 4 * (size_t(data.aligned / 16) + size_t(kEntityMotionWords) * data.aligned +
                size_t(dynamicModelCount + dynamicBrushCount) * (data.aligned / 16));
}

void EmitFrustumLights(ZoneWriter &writer, const Data &data)
{
    if (data.Empty())
        return;
    writer.align(3);
    writer.write(data.indexOffsets.data(), data.indexOffsets.size() * 4);
    writer.align(3);
    writer.write(data.vertexOffsets.data(), data.vertexOffsets.size() * 4);
    writer.align(1);
    writer.write(data.indexCounts.data(), data.indexCounts.size() * 2);
    writer.align(1);
    writer.write(data.vertexCounts.data(), data.vertexCounts.size() * 2);
    writer.align(3);
    writer.write(data.indices.data(), data.indices.size() * 2);
    writer.align(3);
    writer.write(data.vertices.data(), data.vertices.size() * 16);
}

void EmitViewFrustums(ZoneWriter &writer, const Data &data)
{
    if (data.Empty())
        return;
    writer.align(7);
    for (const auto &view : data.views)
    {
        uint8_t record[32]{};
        record[0] = static_cast<uint8_t>(view.planes.size());
        record[1] = static_cast<uint8_t>(view.indices.size());
        record[2] = static_cast<uint8_t>(view.vertices.size());
        if (!view.planes.empty())
        {
            Store(record, 8, PTR_FOLLOWS);
            Store(record, 16, PTR_FOLLOWS);
            Store(record, 24, PTR_FOLLOWS);
        }
        writer.write(record, sizeof(record));
    }
    for (const auto &view : data.views)
    {
        if (view.planes.empty())
            continue;
        writer.align(3);
        writer.write(view.planes.data(), view.planes.size() * 16);
        writer.write(view.indices.data(), view.indices.size());
        writer.align(3);
        writer.write(view.vertices.data(), view.vertices.size() * 12);
    }
}

void EmitShadowGeometry(ZoneWriter &writer, const Data &data)
{
    if (data.Empty())
        return;
    writer.align(7);
    for (const auto &shadow : data.shadows)
    {
        uint8_t record[24]{};
        Store(record, 0, static_cast<uint16_t>(shadow.surfaces.size()));
        Store(record, 2, static_cast<uint16_t>(shadow.models.size()));
        if (!shadow.surfaces.empty())
            Store(record, 8, PTR_FOLLOWS);
        if (!shadow.models.empty())
            Store(record, 16, PTR_FOLLOWS);
        writer.write(record, sizeof(record));
    }
    for (const auto &shadow : data.shadows)
    {
        if (!shadow.surfaces.empty())
        {
            writer.align(3);
            writer.write(shadow.surfaces.data(), shadow.surfaces.size() * 4);
        }
        if (!shadow.models.empty())
        {
            writer.align(1);
            writer.write(shadow.models.data(), shadow.models.size() * 2);
        }
    }
}

void EmitLightAABB(ZoneWriter &writer, const Data &data)
{
    if (data.nodes.empty())
        return;
    writer.align(3);
    for (const auto &node : data.nodes)
    {
        uint8_t record[28]{};
        node.bounds.Write(record);
        Store(record, 24, node.firstChild);
        Store(record, 26, node.childCount);
        writer.write(record, sizeof(record));
    }
    writer.align(1);
    writer.write(data.aabbLights.data(), data.aabbLights.size() * 2);
}
} // namespace iw8::locallights
