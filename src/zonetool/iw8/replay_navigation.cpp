#include "replay_navigation.h"

#include "common/log.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>

// Replay 1.20 MP bot navigation (all layouts verified against the shipped
// mp_shipment aipaths/navmesh/tacgraph and the Replay zone loaders; see
// obsidian re/replay-120-glass-stairs-nav and bot-nav-research/bfx-navgraph-format.md):
//   PathData   Load 0xDF33E0: nodes (192 B), per-node links, nodeTree, extra data.
//   NavMeshData q_Load_NavMeshData 0xDDC890: one nav_resource_s whose
//              pGraphBuffer is a bfx resource holding one NavGraphImage.
//   TacticalGraphData 0xDF4A30: points, triangular vis rows, quad tree.
// Bots require pathdata, path extra data, NavMeshData.hasExposureData, path
// zones and a pathnode within 300 units of every spawn (sv_bot_mp 269-273).

namespace iw8::navigation
{
struct Data
{
    struct Link
    {
        float distance{};
        std::uint16_t node{};
    };
    struct Node
    {
        Vec3 origin{};
        std::uint16_t type{};
        std::int32_t tacpoint{-1};
        std::array<float, 2> forward{};
        std::vector<Link> links;
    };
    struct TreeNode
    {
        std::int32_t axis{-1};
        float dist{};
        std::array<std::int32_t, 2> children{-1, -1};
        std::vector<std::uint16_t> nodes;
    };
    struct TacPoint
    {
        Vec3 position{};
        std::uint8_t type{};
        std::uint16_t next{0xFFFF};
        std::uint16_t prev{0xFFFF};
    };
    struct Quad
    {
        std::array<float, 2> mid{};
        std::array<float, 2> half{};
        std::uint16_t list{0xFFFF};
        std::array<std::int32_t, 4> children{-1, -1, -1, -1};
    };

    std::vector<std::uint8_t> navGraph;
    Vec3 staticHalf{};
    Vec3 staticMid{};

    std::vector<Node> nodes;
    std::uint32_t fixedNodeCount{};
    std::uint32_t zoneCount{};
    std::vector<TreeNode> tree;
    std::vector<std::uint8_t> exposure;
    std::vector<std::uint8_t> noPeek;
    std::vector<std::uint8_t> zones;

    std::vector<TacPoint> tacPoints;
    std::vector<std::vector<std::uint8_t>> visRows;
    std::int32_t visBytes{};
    std::vector<Quad> quads;
};

namespace
{
// Shipped mp_shipment bfx build parameters (NavGraphHeader +40..+56).
constexpr float kBuildScale = 72.0f;
constexpr float kVoxelSize = 5.0f;
constexpr float kAgentRadius = 15.0f;
constexpr float kStepHeight = 18.0f;
constexpr float kAgentHeight = 72.0f;

constexpr float kCell = 8.0f;
constexpr float kMinFloorNormal = 0.7f;
constexpr int kErodeCells = 2;
constexpr int kMaxRectCells = 24;
constexpr float kRectPlaneTolerance = 2.0f;
constexpr float kLinkDistance = 256.0f;
constexpr int kCoverageCells = 12;
constexpr std::size_t kMaxFixedNodes = 3000;
// Com_MapUsesPlayerViewHeight: mp_ maps put tacpoints at ground + 60.
constexpr float kEyeHeight = 60.0f;
constexpr std::array<float, 3> kStanceEyes{60.0f, 40.0f, 11.0f};

constexpr std::uint16_t kPathDataVersion = 30;
constexpr std::int32_t kNavMeshVersion = 19;
constexpr std::uint32_t kDynamicNodeSlots = 64;
constexpr std::uint16_t kPathnodeType = 1;
constexpr std::uint32_t kGraphVersion = 0x2E;
constexpr std::size_t kGraphHeaderSize = 340;
constexpr std::size_t kAreaSize = 72;
constexpr std::size_t kEdgeSize = 32;

constexpr float kInfinity = std::numeric_limits<float>::infinity();

Vec3 Add(const Vec3 &a, const Vec3 &b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Vec3 Sub(const Vec3 &a, const Vec3 &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 Scale(const Vec3 &a, const float s)
{
    return {a[0] * s, a[1] * s, a[2] * s};
}
float Dot(const Vec3 &a, const Vec3 &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Vec3 Cross(const Vec3 &a, const Vec3 &b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float Length(const Vec3 &a)
{
    return std::sqrt(Dot(a, a));
}
float Distance(const Vec3 &a, const Vec3 &b)
{
    return Length(Sub(a, b));
}

struct Box
{
    Vec3 lo{kInfinity, kInfinity, kInfinity};
    Vec3 hi{-kInfinity, -kInfinity, -kInfinity};
    void Add(const Vec3 &p)
    {
        for (std::size_t a = 0; a < 3; ++a)
        {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    void Add(const Box &b)
    {
        for (std::size_t a = 0; a < 3; ++a)
        {
            lo[a] = std::min(lo[a], b.lo[a]);
            hi[a] = std::max(hi[a], b.hi[a]);
        }
    }
    bool Valid() const
    {
        return lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2];
    }
};

template <typename Fn> void ParallelFor(const std::size_t count, Fn &&fn)
{
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = std::min<unsigned>(hardware, 32u);
    if (count < 32 || threads == 1)
    {
        for (std::size_t index = 0; index < count; ++index)
            fn(index);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::exception_ptr failure;
    std::mutex failureLock;
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned thread = 0; thread < threads; ++thread)
        pool.emplace_back([&] {
            try
            {
                for (;;)
                {
                    const std::size_t index = next.fetch_add(1);
                    if (index >= count)
                        break;
                    fn(index);
                }
            }
            catch (...)
            {
                const std::lock_guard lock(failureLock);
                if (!failure)
                    failure = std::current_exception();
                next = count;
            }
        });
    for (auto &thread : pool)
        thread.join();
    if (failure)
        std::rethrow_exception(failure);
}

// ---------------------------------------------------------------------------
// Collision scene: convex hulls and triangles in one BVH.
// ---------------------------------------------------------------------------
struct Span
{
    float lo{};
    float hi{};
    Vec3 topNormal{};
    bool surface{};
};

class Scene
{
  public:
    explicit Scene(const Source &source)
    {
        std::size_t degenerate = 0;
        for (const Hull &hull : source.hulls)
        {
            if (!hull.flags || hull.points.size() < 4)
                continue;
            ConvexPrim prim;
            prim.planes = HullPlanes(hull.points);
            if (prim.planes.size() < 4)
            {
                ++degenerate;
                continue;
            }
            for (const Vec3 &point : hull.points)
                prim.box.Add(point);
            prim.flags = hull.flags;
            hulls_.push_back(std::move(prim));
        }
        for (const Triangle &triangle : source.triangles)
        {
            if (!triangle.flags)
                continue;
            TrianglePrim prim;
            prim.a = triangle.corners[0];
            prim.e1 = Sub(triangle.corners[1], triangle.corners[0]);
            prim.e2 = Sub(triangle.corners[2], triangle.corners[0]);
            const Vec3 normal = Cross(prim.e1, prim.e2);
            const float length = Length(normal);
            if (!(length > 1.0e-6f))
                continue;
            prim.normal = Scale(normal, 1.0f / length);
            for (const Vec3 &corner : triangle.corners)
                prim.box.Add(corner);
            prim.flags = triangle.flags;
            triangles_.push_back(prim);
        }
        for (const auto &prim : hulls_)
            bounds_.Add(prim.box);
        for (const auto &prim : triangles_)
            bounds_.Add(prim.box);
        // Pieces reaching the top of the world are the sky box.
        if (bounds_.Valid())
        {
            for (auto &prim : hulls_)
                prim.sky = prim.box.hi[2] >= bounds_.hi[2] - 1.0f;
            for (auto &prim : triangles_)
                prim.sky = prim.box.hi[2] >= bounds_.hi[2] - 1.0f;
        }
        BuildTree();
        zt::info("navigation: collision scene has %zu convex hulls (%zu degenerate skipped), "
                 "%zu triangles, %zu BVH nodes",
                 hulls_.size(), degenerate, triangles_.size(), nodes_.size());
    }

    const Box &Bounds() const
    {
        return bounds_;
    }

    bool Blocked(const Vec3 &from, const Vec3 &to, const std::uint32_t mask) const
    {
        float best = 1.0f;
        bool sky = false;
        return Trace(from, to, mask, true, best, sky);
    }

    // Fraction in [0, 1] of the first hit, or > 1 when the segment is clear.
    float FirstHit(const Vec3 &from, const Vec3 &to, const std::uint32_t mask, bool &sky) const
    {
        float best = 1.0f;
        sky = false;
        if (!Trace(from, to, mask, false, best, sky))
            return 2.0f;
        return best;
    }

    void Column(const float x, const float y, std::vector<Span> &spans) const
    {
        spans.clear();
        if (nodes_.empty())
            return;
        std::uint32_t stack[128];
        std::size_t depth = 0;
        stack[depth++] = 0;
        while (depth)
        {
            const Node &node = nodes_[stack[--depth]];
            if (x < node.box.lo[0] || x > node.box.hi[0] || y < node.box.lo[1] ||
                y > node.box.hi[1])
                continue;
            if (node.count)
            {
                for (std::uint32_t item = node.first; item < node.first + node.count; ++item)
                    ColumnItem(items_[item], x, y, spans);
                continue;
            }
            stack[depth++] = node.right;
            stack[depth++] = static_cast<std::uint32_t>(&node - nodes_.data()) + 1;
        }
    }

  private:
    struct ConvexPrim
    {
        std::vector<std::array<float, 4>> planes; // outward normal, distance
        Box box;
        std::uint32_t flags{};
        bool sky{};
    };
    struct TrianglePrim
    {
        Vec3 a{};
        Vec3 e1{};
        Vec3 e2{};
        Vec3 normal{};
        Box box;
        std::uint32_t flags{};
        bool sky{};
    };
    struct Node
    {
        Box box;
        std::uint32_t first{};
        std::uint32_t count{};
        std::uint32_t right{};
    };
    static constexpr std::uint32_t kTriangleBit = 0x80000000u;

    static std::vector<std::array<float, 4>> HullPlanes(const std::vector<Vec3> &points)
    {
        // Supporting planes through vertex triples; IW3 brushes and the
        // converter's split pieces have few vertices, so this stays cheap.
        std::vector<std::array<double, 4>> planes;
        const std::size_t count = points.size();
        constexpr double epsilon = 0.02;
        for (std::size_t i = 0; i < count; ++i)
            for (std::size_t j = i + 1; j < count; ++j)
                for (std::size_t k = j + 1; k < count; ++k)
                {
                    const double ax = points[j][0] - points[i][0];
                    const double ay = points[j][1] - points[i][1];
                    const double az = points[j][2] - points[i][2];
                    const double bx = points[k][0] - points[i][0];
                    const double by = points[k][1] - points[i][1];
                    const double bz = points[k][2] - points[i][2];
                    double nx = ay * bz - az * by;
                    double ny = az * bx - ax * bz;
                    double nz = ax * by - ay * bx;
                    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (length < 1.0e-3)
                        continue;
                    nx /= length;
                    ny /= length;
                    nz /= length;
                    double d = nx * points[i][0] + ny * points[i][1] + nz * points[i][2];
                    bool above = false;
                    bool below = false;
                    for (std::size_t m = 0; m < count && !(above && below); ++m)
                    {
                        const double side =
                            nx * points[m][0] + ny * points[m][1] + nz * points[m][2] - d;
                        if (side > epsilon)
                            above = true;
                        else if (side < -epsilon)
                            below = true;
                    }
                    if ((above && below) || (!above && !below))
                        continue;
                    if (above)
                    {
                        nx = -nx;
                        ny = -ny;
                        nz = -nz;
                        d = -d;
                    }
                    const bool duplicate =
                        std::any_of(planes.begin(), planes.end(), [&](const auto &plane) {
                            return plane[0] * nx + plane[1] * ny + plane[2] * nz > 0.99999 &&
                                   std::abs(plane[3] - d) < 2.0 * epsilon;
                        });
                    if (!duplicate)
                        planes.push_back({nx, ny, nz, d});
                }
        std::vector<std::array<float, 4>> result;
        result.reserve(planes.size());
        for (const auto &plane : planes)
            result.push_back({static_cast<float>(plane[0]), static_cast<float>(plane[1]),
                              static_cast<float>(plane[2]), static_cast<float>(plane[3])});
        return result;
    }

    const Box &ItemBox(const std::uint32_t item) const
    {
        return item & kTriangleBit ? triangles_[item & ~kTriangleBit].box : hulls_[item].box;
    }

    void BuildTree()
    {
        items_.clear();
        for (std::uint32_t index = 0; index < hulls_.size(); ++index)
            items_.push_back(index);
        for (std::uint32_t index = 0; index < triangles_.size(); ++index)
            items_.push_back(index | kTriangleBit);
        nodes_.clear();
        if (items_.empty())
            return;
        nodes_.reserve(items_.size() * 2);
        Build(0, static_cast<std::uint32_t>(items_.size()));
    }

    std::uint32_t Build(const std::uint32_t first, const std::uint32_t count)
    {
        const auto index = static_cast<std::uint32_t>(nodes_.size());
        nodes_.emplace_back();
        Box box;
        Box centers;
        for (std::uint32_t item = first; item < first + count; ++item)
        {
            const Box &itemBox = ItemBox(items_[item]);
            box.Add(itemBox);
            centers.Add(Scale(Add(itemBox.lo, itemBox.hi), 0.5f));
        }
        nodes_[index].box = box;
        if (count <= 4)
        {
            nodes_[index].first = first;
            nodes_[index].count = count;
            return index;
        }
        std::size_t axis = 0;
        for (std::size_t a = 1; a < 3; ++a)
            if (centers.hi[a] - centers.lo[a] > centers.hi[axis] - centers.lo[axis])
                axis = a;
        const std::uint32_t half = count / 2;
        std::nth_element(items_.begin() + first, items_.begin() + first + half,
                         items_.begin() + first + count,
                         [&](const std::uint32_t left, const std::uint32_t right) {
                             const Box &a = ItemBox(left);
                             const Box &b = ItemBox(right);
                             return a.lo[axis] + a.hi[axis] < b.lo[axis] + b.hi[axis];
                         });
        Build(first, half);
        const std::uint32_t right = Build(first + half, count - half);
        nodes_[index].right = right;
        nodes_[index].count = 0;
        return index;
    }

    static bool RayBox(const Box &box, const Vec3 &origin, const Vec3 &inverse, const float limit)
    {
        float tmin = 0.0f;
        float tmax = limit;
        for (std::size_t a = 0; a < 3; ++a)
        {
            float t0 = (box.lo[a] - origin[a]) * inverse[a];
            float t1 = (box.hi[a] - origin[a]) * inverse[a];
            if (std::isnan(t0) || std::isnan(t1))
            {
                if (origin[a] < box.lo[a] || origin[a] > box.hi[a])
                    return false;
                continue;
            }
            if (t0 > t1)
                std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax)
                return false;
        }
        return true;
    }

    // Hits that start inside a hull are ignored, so a sample point resting on
    // a surface never blocks itself.
    static bool SegmentHull(const ConvexPrim &prim, const Vec3 &origin, const Vec3 &direction,
                            float &t)
    {
        float enter = 0.0f;
        float exit = 1.0f;
        bool inside = true;
        for (const auto &plane : prim.planes)
        {
            const float distance =
                plane[0] * origin[0] + plane[1] * origin[1] + plane[2] * origin[2] - plane[3];
            const float denominator =
                plane[0] * direction[0] + plane[1] * direction[1] + plane[2] * direction[2];
            if (distance > -0.01f)
                inside = false;
            if (std::abs(denominator) < 1.0e-9f)
            {
                if (distance > 0.0f)
                    return false;
                continue;
            }
            const float hit = -distance / denominator;
            if (denominator < 0.0f)
                enter = std::max(enter, hit);
            else
                exit = std::min(exit, hit);
            if (enter > exit)
                return false;
        }
        if (inside)
            return false;
        t = enter;
        return true;
    }

    static bool SegmentTriangle(const TrianglePrim &prim, const Vec3 &origin,
                                const Vec3 &direction, float &t)
    {
        const Vec3 p = Cross(direction, prim.e2);
        const float determinant = Dot(prim.e1, p);
        if (std::abs(determinant) < 1.0e-9f)
            return false;
        const float inverse = 1.0f / determinant;
        const Vec3 s = Sub(origin, prim.a);
        const float u = Dot(s, p) * inverse;
        if (u < 0.0f || u > 1.0f)
            return false;
        const Vec3 q = Cross(s, prim.e1);
        const float v = Dot(direction, q) * inverse;
        if (v < 0.0f || u + v > 1.0f)
            return false;
        t = Dot(prim.e2, q) * inverse;
        return t > 1.0e-5f && t <= 1.0f;
    }

    bool Trace(const Vec3 &from, const Vec3 &to, const std::uint32_t mask, const bool any,
               float &best, bool &sky) const
    {
        if (nodes_.empty())
            return false;
        const Vec3 direction = Sub(to, from);
        const Vec3 inverse{1.0f / direction[0], 1.0f / direction[1], 1.0f / direction[2]};
        bool hit = false;
        std::uint32_t stack[128];
        std::size_t depth = 0;
        stack[depth++] = 0;
        while (depth)
        {
            const Node &node = nodes_[stack[--depth]];
            if (!RayBox(node.box, from, inverse, best))
                continue;
            if (!node.count)
            {
                stack[depth++] = node.right;
                stack[depth++] = static_cast<std::uint32_t>(&node - nodes_.data()) + 1;
                continue;
            }
            for (std::uint32_t item = node.first; item < node.first + node.count; ++item)
            {
                const std::uint32_t id = items_[item];
                float t = 0.0f;
                bool itemSky = false;
                if (id & kTriangleBit)
                {
                    const TrianglePrim &prim = triangles_[id & ~kTriangleBit];
                    if (!(prim.flags & mask) || !SegmentTriangle(prim, from, direction, t))
                        continue;
                    itemSky = prim.sky;
                }
                else
                {
                    const ConvexPrim &prim = hulls_[id];
                    if (!(prim.flags & mask) || !SegmentHull(prim, from, direction, t))
                        continue;
                    itemSky = prim.sky;
                }
                if (t > best)
                    continue;
                hit = true;
                best = t;
                sky = itemSky;
                if (any)
                    return true;
            }
        }
        return hit;
    }

    void ColumnItem(const std::uint32_t id, const float x, const float y,
                    std::vector<Span> &spans) const
    {
        if (id & kTriangleBit)
        {
            const TrianglePrim &prim = triangles_[id & ~kTriangleBit];
            if (!(prim.flags & kBlocksMovement) || std::abs(prim.normal[2]) < 1.0e-4f)
                return;
            // 2D barycentric test against the triangle's XY projection.
            const float px = x - prim.a[0];
            const float py = y - prim.a[1];
            const float d = prim.e1[0] * prim.e2[1] - prim.e1[1] * prim.e2[0];
            if (std::abs(d) < 1.0e-9f)
                return;
            const float u = (px * prim.e2[1] - py * prim.e2[0]) / d;
            const float v = (prim.e1[0] * py - prim.e1[1] * px) / d;
            if (u < 0.0f || v < 0.0f || u + v > 1.0f)
                return;
            Span span;
            span.lo = span.hi = prim.a[2] + u * prim.e1[2] + v * prim.e2[2];
            span.topNormal = prim.normal[2] < 0.0f ? Scale(prim.normal, -1.0f) : prim.normal;
            span.surface = true;
            spans.push_back(span);
            return;
        }
        const ConvexPrim &prim = hulls_[id];
        if (!(prim.flags & kBlocksMovement))
            return;
        float lo = -kInfinity;
        float hi = kInfinity;
        Vec3 top{0.0f, 0.0f, 1.0f};
        for (const auto &plane : prim.planes)
        {
            const float rest = plane[3] - plane[0] * x - plane[1] * y;
            if (std::abs(plane[2]) < 1.0e-6f)
            {
                if (rest < 0.0f)
                    return;
                continue;
            }
            const float z = rest / plane[2];
            if (plane[2] > 0.0f)
            {
                if (z < hi)
                {
                    hi = z;
                    top = {plane[0], plane[1], plane[2]};
                }
            }
            else
                lo = std::max(lo, z);
        }
        if (!(lo < hi) || !std::isfinite(lo) || !std::isfinite(hi))
            return;
        spans.push_back({lo, hi, top, false});
    }

    std::vector<ConvexPrim> hulls_;
    std::vector<TrianglePrim> triangles_;
    std::vector<std::uint32_t> items_;
    std::vector<Node> nodes_;
    Box bounds_;
};

// ---------------------------------------------------------------------------
// Walkable voxel columns.
// ---------------------------------------------------------------------------
struct Layer
{
    float z{};
    float gx{};
    float gy{};
};

struct Grid
{
    float minX{};
    float minY{};
    float cell{kCell};
    int nx{};
    int ny{};
    std::vector<std::uint32_t> columnStart; // nx*ny + 1
    std::vector<Layer> layers;              // cells
    std::vector<std::uint32_t> column;      // per cell
    std::vector<std::int32_t> neighbor;     // per cell * 4: +x, +y, -x, -y
    std::vector<std::uint8_t> kept;

    int ColumnX(const std::uint32_t index) const
    {
        return static_cast<int>(column[index] % static_cast<std::uint32_t>(nx));
    }
    int ColumnY(const std::uint32_t index) const
    {
        return static_cast<int>(column[index] / static_cast<std::uint32_t>(nx));
    }
    float CenterX(const int ix) const
    {
        return minX + (static_cast<float>(ix) + 0.5f) * cell;
    }
    float CenterY(const int iy) const
    {
        return minY + (static_cast<float>(iy) + 0.5f) * cell;
    }
    Vec3 Center(const std::uint32_t index) const
    {
        return {CenterX(ColumnX(index)), CenterY(ColumnY(index)), layers[index].z};
    }
    float HeightAt(const std::uint32_t cellIndex, const float x, const float y) const
    {
        const Layer &layer = layers[cellIndex];
        return layer.z + layer.gx * (x - CenterX(ColumnX(cellIndex))) +
               layer.gy * (y - CenterY(ColumnY(cellIndex)));
    }
    std::int32_t Next(const std::uint32_t cellIndex, const int direction) const
    {
        const std::int32_t next = neighbor[cellIndex * 4 + direction];
        return next >= 0 && kept[static_cast<std::size_t>(next)] ? next : -1;
    }
};

constexpr int kDirX[4]{1, 0, -1, 0};
constexpr int kDirY[4]{0, 1, 0, -1};

std::vector<Layer> ColumnFloors(const Scene &scene, const float x, const float y,
                                std::vector<Span> &spans)
{
    scene.Column(x, y, spans);
    struct Candidate
    {
        float z;
        Vec3 normal;
    };
    std::vector<Candidate> candidates;
    for (const Span &span : spans)
    {
        const Vec3 &normal = span.topNormal;
        if (normal[2] >= kMinFloorNormal)
            candidates.push_back({span.hi, normal});
    }
    std::vector<Layer> floors;
    if (candidates.empty())
        return floors;
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.z < b.z; });
    for (const Candidate &candidate : candidates)
    {
        const float floor = candidate.z;
        const bool blocked = std::any_of(spans.begin(), spans.end(), [&](const Span &span) {
            return span.hi > floor + 0.25f && span.lo < floor + kAgentHeight;
        });
        if (blocked)
            continue;
        Layer layer;
        layer.z = floor;
        layer.gx = std::clamp(-candidate.normal[0] / candidate.normal[2], -1.5f, 1.5f);
        layer.gy = std::clamp(-candidate.normal[1] / candidate.normal[2], -1.5f, 1.5f);
        if (!floors.empty() && floor - floors.back().z < 1.0f)
            floors.back() = layer;
        else
            floors.push_back(layer);
    }
    return floors;
}

bool BuildGrid(const Scene &scene, const Box &region, Grid &grid)
{
    grid.minX = region.lo[0];
    grid.minY = region.lo[1];
    grid.cell = kCell;
    for (;;)
    {
        grid.nx = static_cast<int>(std::ceil((region.hi[0] - region.lo[0]) / grid.cell));
        grid.ny = static_cast<int>(std::ceil((region.hi[1] - region.lo[1]) / grid.cell));
        if (static_cast<std::size_t>(grid.nx) * static_cast<std::size_t>(grid.ny) <= 6'000'000)
            break;
        grid.cell *= 1.5f;
    }
    if (grid.nx <= 0 || grid.ny <= 0)
        return false;
    std::vector<std::vector<std::vector<Layer>>> rows(static_cast<std::size_t>(grid.ny));
    ParallelFor(static_cast<std::size_t>(grid.ny), [&](const std::size_t iy) {
        std::vector<Span> spans;
        auto &row = rows[iy];
        row.resize(static_cast<std::size_t>(grid.nx));
        for (int ix = 0; ix < grid.nx; ++ix)
            row[static_cast<std::size_t>(ix)] =
                ColumnFloors(scene, grid.CenterX(ix), grid.CenterY(static_cast<int>(iy)), spans);
    });
    const std::size_t columns = static_cast<std::size_t>(grid.nx) * grid.ny;
    grid.columnStart.assign(columns + 1, 0);
    std::size_t total = 0;
    for (std::size_t iy = 0; iy < rows.size(); ++iy)
        for (std::size_t ix = 0; ix < rows[iy].size(); ++ix)
        {
            grid.columnStart[iy * grid.nx + ix] = static_cast<std::uint32_t>(total);
            total += rows[iy][ix].size();
        }
    grid.columnStart[columns] = static_cast<std::uint32_t>(total);
    if (total >= 0x7FFFFFFF)
        throw std::runtime_error("navigation grid exceeds 2^31 cells");
    grid.layers.reserve(total);
    grid.column.reserve(total);
    for (std::size_t iy = 0; iy < rows.size(); ++iy)
        for (std::size_t ix = 0; ix < rows[iy].size(); ++ix)
            for (const Layer &layer : rows[iy][ix])
            {
                grid.layers.push_back(layer);
                grid.column.push_back(static_cast<std::uint32_t>(iy * grid.nx + ix));
            }

    // Step connections between neighbouring columns, then require both
    // directions to agree so every portal is symmetric.
    grid.neighbor.assign(total * 4, -1);
    ParallelFor(total, [&](const std::size_t cellIndex) {
        const auto cell = static_cast<std::uint32_t>(cellIndex);
        const int ix = grid.ColumnX(cell);
        const int iy = grid.ColumnY(cell);
        const float z = grid.layers[cell].z;
        for (int direction = 0; direction < 4; ++direction)
        {
            const int jx = ix + kDirX[direction];
            const int jy = iy + kDirY[direction];
            if (jx < 0 || jy < 0 || jx >= grid.nx || jy >= grid.ny)
                continue;
            const std::size_t target = static_cast<std::size_t>(jy) * grid.nx + jx;
            std::int32_t best = -1;
            float bestDelta = kStepHeight + 0.001f;
            for (std::uint32_t other = grid.columnStart[target];
                 other < grid.columnStart[target + 1]; ++other)
            {
                const float delta = std::abs(grid.layers[other].z - z);
                if (delta < bestDelta)
                {
                    bestDelta = delta;
                    best = static_cast<std::int32_t>(other);
                }
            }
            if (best < 0)
                continue;
            const float top = std::max(z, grid.layers[static_cast<std::size_t>(best)].z);
            const Vec3 from{grid.CenterX(ix), grid.CenterY(iy), 0.0f};
            const Vec3 to{grid.CenterX(jx), grid.CenterY(jy), 0.0f};
            bool open = true;
            for (const float height : {kStepHeight + 2.0f, 56.0f})
            {
                Vec3 a = from;
                Vec3 b = to;
                a[2] = b[2] = top + height;
                if (scene.Blocked(a, b, kBlocksMovement))
                {
                    open = false;
                    break;
                }
            }
            if (open)
                grid.neighbor[cellIndex * 4 + direction] = best;
        }
    });
    for (std::size_t cell = 0; cell < total; ++cell)
        for (int direction = 0; direction < 4; ++direction)
        {
            const std::int32_t other = grid.neighbor[cell * 4 + direction];
            if (other >= 0 &&
                grid.neighbor[static_cast<std::size_t>(other) * 4 + ((direction + 2) & 3)] !=
                    static_cast<std::int32_t>(cell))
                grid.neighbor[cell * 4 + direction] = -1;
        }
    grid.kept.assign(total, 1);
    return true;
}

// Keeps the agent radius away from walls and ledges.
void Erode(Grid &grid)
{
    const std::size_t total = grid.layers.size();
    std::vector<int> distance(total, std::numeric_limits<int>::max());
    std::vector<std::uint32_t> queue;
    for (std::size_t cell = 0; cell < total; ++cell)
    {
        bool border = false;
        for (int direction = 0; direction < 4; ++direction)
            border = border || grid.neighbor[cell * 4 + direction] < 0;
        if (border)
        {
            distance[cell] = 0;
            queue.push_back(static_cast<std::uint32_t>(cell));
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head)
    {
        const std::uint32_t cell = queue[head];
        for (int direction = 0; direction < 4; ++direction)
        {
            const std::int32_t next = grid.neighbor[cell * 4 + direction];
            if (next >= 0 && distance[static_cast<std::size_t>(next)] > distance[cell] + 1)
            {
                distance[static_cast<std::size_t>(next)] = distance[cell] + 1;
                queue.push_back(static_cast<std::uint32_t>(next));
            }
        }
    }
    for (std::size_t cell = 0; cell < total; ++cell)
        if (distance[cell] < kErodeCells)
            grid.kept[cell] = 0;
}

// Nearest kept cell to a marker, searching a few columns around it.
std::int32_t Locate(const Grid &grid, const Vec3 &point, const float below, const float above,
                    const int radius)
{
    const int ix = static_cast<int>(std::floor((point[0] - grid.minX) / grid.cell));
    const int iy = static_cast<int>(std::floor((point[1] - grid.minY) / grid.cell));
    std::int32_t best = -1;
    float bestScore = kInfinity;
    for (int ring = 0; ring <= radius; ++ring)
    {
        for (int dy = -ring; dy <= ring; ++dy)
            for (int dx = -ring; dx <= ring; ++dx)
            {
                if (std::max(std::abs(dx), std::abs(dy)) != ring)
                    continue;
                const int jx = ix + dx;
                const int jy = iy + dy;
                if (jx < 0 || jy < 0 || jx >= grid.nx || jy >= grid.ny)
                    continue;
                const std::size_t column = static_cast<std::size_t>(jy) * grid.nx + jx;
                for (std::uint32_t cell = grid.columnStart[column];
                     cell < grid.columnStart[column + 1]; ++cell)
                {
                    if (!grid.kept[cell])
                        continue;
                    const float z = grid.layers[cell].z;
                    if (z < point[2] - below || z > point[2] + above)
                        continue;
                    const float ex = grid.CenterX(jx) - point[0];
                    const float ey = grid.CenterY(jy) - point[1];
                    const float ez = (z - point[2]) * 0.5f;
                    const float score = ex * ex + ey * ey + ez * ez;
                    if (score < bestScore)
                    {
                        bestScore = score;
                        best = static_cast<std::int32_t>(cell);
                    }
                }
            }
        if (best >= 0)
            return best;
    }
    return best;
}

// Straight walk across connected cells (4-connected DDA).
bool WalkableLine(const Grid &grid, const std::uint32_t fromCell, const Vec3 &from,
                  const std::uint32_t toCell, const Vec3 &to)
{
    const float gx0 = (from[0] - grid.minX) / grid.cell;
    const float gy0 = (from[1] - grid.minY) / grid.cell;
    const float gx1 = (to[0] - grid.minX) / grid.cell;
    const float gy1 = (to[1] - grid.minY) / grid.cell;
    int ix = grid.ColumnX(fromCell);
    int iy = grid.ColumnY(fromCell);
    const int tx = grid.ColumnX(toCell);
    const int ty = grid.ColumnY(toCell);
    const float dx = gx1 - gx0;
    const float dy = gy1 - gy0;
    const int stepX = dx > 0.0f ? 1 : -1;
    const int stepY = dy > 0.0f ? 1 : -1;
    float tMaxX = dx != 0.0f
                      ? (stepX > 0 ? static_cast<float>(ix + 1) - gx0 : gx0 - static_cast<float>(ix)) /
                            std::abs(dx)
                      : kInfinity;
    float tMaxY = dy != 0.0f
                      ? (stepY > 0 ? static_cast<float>(iy + 1) - gy0 : gy0 - static_cast<float>(iy)) /
                            std::abs(dy)
                      : kInfinity;
    const float tDeltaX = dx != 0.0f ? 1.0f / std::abs(dx) : kInfinity;
    const float tDeltaY = dy != 0.0f ? 1.0f / std::abs(dy) : kInfinity;
    std::int32_t current = static_cast<std::int32_t>(fromCell);
    int guard = 2 * (std::abs(tx - ix) + std::abs(ty - iy)) + 4;
    while ((ix != tx || iy != ty) && guard-- > 0)
    {
        int direction;
        if (iy == ty || (ix != tx && tMaxX < tMaxY))
        {
            direction = stepX > 0 ? 0 : 2;
            ix += stepX;
            tMaxX += tDeltaX;
        }
        else
        {
            direction = stepY > 0 ? 1 : 3;
            iy += stepY;
            tMaxY += tDeltaY;
        }
        current = grid.Next(static_cast<std::uint32_t>(current), direction);
        if (current < 0)
            return false;
    }
    return current == static_cast<std::int32_t>(toCell);
}

// ---------------------------------------------------------------------------
// bfx NavGraph resource.
// ---------------------------------------------------------------------------
void Put32(std::vector<std::uint8_t> &bytes, const std::size_t offset, const std::uint32_t value)
{
    std::memcpy(bytes.data() + offset, &value, 4);
}
void PutF(std::vector<std::uint8_t> &bytes, const std::size_t offset, const float value)
{
    std::memcpy(bytes.data() + offset, &value, 4);
}
void Put64(std::vector<std::uint8_t> &bytes, const std::size_t offset, const std::uint64_t value)
{
    std::memcpy(bytes.data() + offset, &value, 8);
}

struct Area
{
    std::vector<std::uint32_t> vertices; // corner classes, CCW from above
    std::vector<std::int32_t> adjacent;  // per edge: area index or -1
    Vec3 center{};
    Box box;
    std::uint32_t usage{};
    std::size_t offset{}; // from the NavGraphHeader start
};

struct Mesh
{
    std::vector<Vec3> corners;
    std::vector<Area> areas;
    std::vector<std::int32_t> cellArea;
};

class UnionFind
{
  public:
    explicit UnionFind(const std::size_t count) : parent_(count)
    {
        std::iota(parent_.begin(), parent_.end(), 0u);
    }
    std::uint32_t Find(std::uint32_t value)
    {
        while (parent_[value] != value)
        {
            parent_[value] = parent_[parent_[value]];
            value = parent_[value];
        }
        return value;
    }
    void Join(const std::uint32_t a, const std::uint32_t b)
    {
        const std::uint32_t ra = Find(a);
        const std::uint32_t rb = Find(b);
        if (ra != rb)
            parent_[std::max(ra, rb)] = std::min(ra, rb);
    }

  private:
    std::vector<std::uint32_t> parent_;
};

bool PlaneFits(const Grid &grid, const std::vector<std::uint32_t> &cells)
{
    double s1 = 0, su = 0, sv = 0, sz = 0, suu = 0, suv = 0, svv = 0, suz = 0, svz = 0;
    for (const std::uint32_t cell : cells)
    {
        const double u = grid.ColumnX(cell);
        const double v = grid.ColumnY(cell);
        const double z = grid.layers[cell].z;
        s1 += 1;
        su += u;
        sv += v;
        sz += z;
        suu += u * u;
        suv += u * v;
        svv += v * v;
        suz += u * z;
        svz += v * z;
    }
    // Centered normal equations with a small ridge for single rows/columns.
    const double mu = su / s1;
    const double mv = sv / s1;
    const double mz = sz / s1;
    const double cuu = suu / s1 - mu * mu + 1.0e-6;
    const double cuv = suv / s1 - mu * mv;
    const double cvv = svv / s1 - mv * mv + 1.0e-6;
    const double cuz = suz / s1 - mu * mz;
    const double cvz = svz / s1 - mv * mz;
    const double det = cuu * cvv - cuv * cuv;
    const double b = (cuz * cvv - cvz * cuv) / det;
    const double c = (cvz * cuu - cuz * cuv) / det;
    for (const std::uint32_t cell : cells)
    {
        const double predicted =
            mz + b * (grid.ColumnX(cell) - mu) + c * (grid.ColumnY(cell) - mv);
        if (std::abs(predicted - grid.layers[cell].z) > kRectPlaneTolerance)
            return false;
    }
    return true;
}

Mesh BuildMesh(const Grid &grid)
{
    const std::size_t total = grid.layers.size();
    Mesh mesh;
    mesh.cellArea.assign(total, -1);
    std::vector<std::vector<std::vector<std::uint32_t>>> rects;
    // Seed rectangles from the most interior cells and grow them in all four
    // directions: scanline growth leaves one-cell strips along every
    // diagonal boundary (Nuketown is rotated against the grid axes).
    std::vector<int> depth(total, std::numeric_limits<int>::max());
    std::vector<std::uint32_t> queue;
    for (std::uint32_t cell = 0; cell < total; ++cell)
    {
        if (!grid.kept[cell])
            continue;
        bool border = false;
        for (int direction = 0; direction < 4; ++direction)
            border = border || grid.Next(cell, direction) < 0;
        if (border)
        {
            depth[cell] = 0;
            queue.push_back(cell);
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head)
        for (int direction = 0; direction < 4; ++direction)
        {
            const std::int32_t next = grid.Next(queue[head], direction);
            if (next >= 0 && depth[static_cast<std::size_t>(next)] > depth[queue[head]] + 1)
            {
                depth[static_cast<std::size_t>(next)] = depth[queue[head]] + 1;
                queue.push_back(static_cast<std::uint32_t>(next));
            }
        }
    std::vector<std::uint32_t> seeds;
    for (std::uint32_t cell = 0; cell < total; ++cell)
        if (grid.kept[cell])
            seeds.push_back(cell);
    std::stable_sort(seeds.begin(), seeds.end(), [&](const std::uint32_t a, const std::uint32_t b) {
        return depth[a] > depth[b];
    });
    const auto freeCell = [&](const std::int32_t cell) {
        return cell >= 0 && mesh.cellArea[static_cast<std::size_t>(cell)] < 0;
    };
    std::vector<std::uint32_t> all;
    std::vector<std::uint32_t> line;
    for (const std::uint32_t start : seeds)
    {
        if (mesh.cellArea[start] >= 0)
            continue;
        // rows bottom to top, cells left to right
        std::vector<std::vector<std::uint32_t>> rect{{start}};
        all.assign(1, start);
        for (bool grown = true; grown;)
        {
            grown = false;
            for (int direction = 0; direction < 4; ++direction)
            {
                const std::size_t width = rect.front().size();
                const std::size_t height = rect.size();
                const bool alongX = (direction & 1) == 0;
                if (static_cast<int>(alongX ? width : height) >= kMaxRectCells)
                    continue;
                line.clear();
                bool ok = true;
                if (alongX)
                {
                    for (std::size_t row = 0; row < height && ok; ++row)
                    {
                        const std::uint32_t edge =
                            direction == 0 ? rect[row].back() : rect[row].front();
                        const std::int32_t next = grid.Next(edge, direction);
                        ok = freeCell(next) && (row == 0 || grid.Next(line.back(), 1) == next);
                        if (ok)
                            line.push_back(static_cast<std::uint32_t>(next));
                    }
                }
                else
                {
                    const auto &edgeRow = direction == 1 ? rect.back() : rect.front();
                    for (std::size_t column = 0; column < width && ok; ++column)
                    {
                        const std::int32_t next = grid.Next(edgeRow[column], direction);
                        ok = freeCell(next) && (column == 0 || grid.Next(line.back(), 0) == next);
                        if (ok)
                            line.push_back(static_cast<std::uint32_t>(next));
                    }
                }
                if (!ok)
                    continue;
                const std::size_t before = all.size();
                all.insert(all.end(), line.begin(), line.end());
                if (!PlaneFits(grid, all))
                {
                    all.resize(before);
                    continue;
                }
                if (direction == 0)
                    for (std::size_t row = 0; row < height; ++row)
                        rect[row].push_back(line[row]);
                else if (direction == 2)
                    for (std::size_t row = 0; row < height; ++row)
                        rect[row].insert(rect[row].begin(), line[row]);
                else if (direction == 1)
                    rect.push_back(line);
                else
                    rect.insert(rect.begin(), line);
                grown = true;
            }
        }
        const auto areaIndex = static_cast<std::int32_t>(rects.size());
        for (const auto &cells : rect)
            for (const std::uint32_t cell : cells)
                mesh.cellArea[cell] = areaIndex;
        rects.push_back(std::move(rect));
    }

    // Corner classes: connected cells share the lattice corners on their
    // common side, so neighbouring areas get bit-identical portal vertices.
    UnionFind corners(total * 4);
    for (std::uint32_t cell = 0; cell < total; ++cell)
    {
        if (!grid.kept[cell])
            continue;
        if (const std::int32_t right = grid.Next(cell, 0); right >= 0)
        {
            corners.Join(cell * 4 + 1, static_cast<std::uint32_t>(right) * 4 + 0);
            corners.Join(cell * 4 + 2, static_cast<std::uint32_t>(right) * 4 + 3);
        }
        if (const std::int32_t up = grid.Next(cell, 1); up >= 0)
        {
            corners.Join(cell * 4 + 3, static_cast<std::uint32_t>(up) * 4 + 0);
            corners.Join(cell * 4 + 2, static_cast<std::uint32_t>(up) * 4 + 1);
        }
    }
    constexpr float kCornerX[4]{-0.5f, 0.5f, 0.5f, -0.5f};
    constexpr float kCornerY[4]{-0.5f, -0.5f, 0.5f, 0.5f};
    std::vector<double> heightSum(total * 4, 0.0);
    std::vector<std::uint32_t> heightCount(total * 4, 0);
    for (std::uint32_t cell = 0; cell < total; ++cell)
    {
        if (!grid.kept[cell])
            continue;
        const Layer &layer = grid.layers[cell];
        for (std::uint32_t corner = 0; corner < 4; ++corner)
        {
            const std::uint32_t root = corners.Find(cell * 4 + corner);
            heightSum[root] += layer.z + layer.gx * kCornerX[corner] * grid.cell +
                               layer.gy * kCornerY[corner] * grid.cell;
            ++heightCount[root];
        }
    }
    std::vector<std::int32_t> cornerIndex(total * 4, -1);
    const auto cornerOf = [&](const std::uint32_t cell, const std::uint32_t corner) {
        const std::uint32_t root = corners.Find(cell * 4 + corner);
        if (cornerIndex[root] < 0)
        {
            cornerIndex[root] = static_cast<std::int32_t>(mesh.corners.size());
            const int ix = grid.ColumnX(cell);
            const int iy = grid.ColumnY(cell);
            mesh.corners.push_back(
                {grid.minX + (static_cast<float>(ix) + 0.5f + kCornerX[corner]) * grid.cell,
                 grid.minY + (static_cast<float>(iy) + 0.5f + kCornerY[corner]) * grid.cell,
                 static_cast<float>(heightSum[root] / heightCount[root])});
        }
        return static_cast<std::uint32_t>(cornerIndex[root]);
    };

    mesh.areas.resize(rects.size());
    for (std::size_t index = 0; index < rects.size(); ++index)
    {
        const auto &rect = rects[index];
        Area &area = mesh.areas[index];
        const std::size_t width = rect.front().size();
        const std::size_t height = rect.size();
        // CCW from above: bottom L->R, right B->T, top R->L, left T->B.
        struct Side
        {
            std::vector<std::uint32_t> cells;
            std::uint32_t startCorner;
            int direction;
        };
        std::array<Side, 4> sides{};
        sides[0] = {rect.front(), 0, 3};
        sides[1].startCorner = 1;
        sides[1].direction = 0;
        for (std::size_t r = 0; r < height; ++r)
            sides[1].cells.push_back(rect[r][width - 1]);
        sides[2] = {std::vector<std::uint32_t>(rect.back().rbegin(), rect.back().rend()), 2, 1};
        sides[3].startCorner = 3;
        sides[3].direction = 2;
        for (std::size_t r = height; r-- > 0;)
            sides[3].cells.push_back(rect[r][0]);
        for (const Side &side : sides)
        {
            std::int32_t runArea = std::numeric_limits<std::int32_t>::min();
            for (const std::uint32_t cell : side.cells)
            {
                const std::int32_t next = grid.Next(cell, side.direction);
                const std::int32_t adjacent =
                    next >= 0 ? mesh.cellArea[static_cast<std::size_t>(next)] : -1;
                if (adjacent != runArea)
                {
                    area.vertices.push_back(cornerOf(cell, side.startCorner));
                    area.adjacent.push_back(adjacent);
                    runArea = adjacent;
                }
            }
        }
        const Vec3 &c0 = mesh.corners[cornerOf(rect.front().front(), 0)];
        const Vec3 &c1 = mesh.corners[cornerOf(rect.front().back(), 1)];
        const Vec3 &c2 = mesh.corners[cornerOf(rect.back().back(), 2)];
        const Vec3 &c3 = mesh.corners[cornerOf(rect.back().front(), 3)];
        area.center = {(c0[0] + c2[0]) * 0.5f, (c0[1] + c2[1]) * 0.5f,
                       (c0[2] + c1[2] + c2[2] + c3[2]) * 0.25f};
        for (const std::uint32_t vertex : area.vertices)
            area.box.Add(mesh.corners[vertex]);
    }
    return mesh;
}

void AppendKdTree(std::vector<std::uint8_t> &out, const Mesh &mesh,
                  std::vector<std::uint32_t> &order, const std::size_t first,
                  const std::size_t count)
{
    const auto put32 = [&](const std::uint32_t value) {
        const std::size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &value, 4);
    };
    const auto putF = [&](const float value) {
        const std::size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &value, 4);
    };
    if (count == 1)
    {
        const Area &area = mesh.areas[order[first]];
        if (area.offset >= 0x80000000u)
            throw std::runtime_error("navigation graph exceeds the KD-tree leaf range");
        put32(0x80000000u | static_cast<std::uint32_t>(area.offset));
        return;
    }
    Box centers;
    for (std::size_t index = first; index < first + count; ++index)
        centers.Add(mesh.areas[order[index]].center);
    std::size_t axis = 0;
    for (std::size_t a = 1; a < 3; ++a)
        if (centers.hi[a] - centers.lo[a] > centers.hi[axis] - centers.lo[axis])
            axis = a;
    const std::size_t half = count / 2;
    std::nth_element(order.begin() + static_cast<std::ptrdiff_t>(first),
                     order.begin() + static_cast<std::ptrdiff_t>(first + half),
                     order.begin() + static_cast<std::ptrdiff_t>(first + count),
                     [&](const std::uint32_t a, const std::uint32_t b) {
                         return mesh.areas[a].center[axis] < mesh.areas[b].center[axis];
                     });
    float leftMax = -kInfinity;
    float rightMin = kInfinity;
    for (std::size_t index = first; index < first + half; ++index)
        leftMax = std::max(leftMax, mesh.areas[order[index]].box.hi[axis]);
    for (std::size_t index = first + half; index < first + count; ++index)
        rightMin = std::min(rightMin, mesh.areas[order[index]].box.lo[axis]);
    const std::size_t node = out.size();
    put32(0);
    putF(leftMax);
    putF(rightMin);
    AppendKdTree(out, mesh, order, first, half);
    const std::size_t right = out.size() - node;
    if (right >= 0x10000000u)
        throw std::runtime_error("navigation KD-tree branch exceeds 28 bits");
    const std::uint32_t word = static_cast<std::uint32_t>(axis) << 28 |
                               static_cast<std::uint32_t>(right);
    std::memcpy(out.data() + node, &word, 4);
    AppendKdTree(out, mesh, order, first + half, count - half);
}

std::vector<std::uint8_t> BuildNavGraph(Mesh &mesh, Box &graphBox)
{
    std::size_t offset = kGraphHeaderSize;
    for (Area &area : mesh.areas)
    {
        if (area.vertices.size() < 3 || area.vertices.size() > 127)
            throw std::runtime_error("navigation area has an unsupported edge count");
        area.offset = offset;
        offset += kAreaSize + kEdgeSize * area.vertices.size();
        graphBox.Add(area.box);
    }
    const std::size_t areaBytes = offset - kGraphHeaderSize;

    std::vector<std::uint8_t> kd(28);
    std::memcpy(kd.data(), graphBox.lo.data(), 12);
    std::memcpy(kd.data() + 12, graphBox.hi.data(), 12);
    std::vector<std::uint32_t> order(mesh.areas.size());
    std::iota(order.begin(), order.end(), 0u);
    AppendKdTree(kd, mesh, order, 0, order.size());
    Put32(kd, 24, static_cast<std::uint32_t>(kd.size() - 28));

    const std::size_t graphBytes = kGraphHeaderSize + areaBytes + kd.size();
    std::vector<std::uint8_t> graph(graphBytes, 0);
    // NavGraphHeader.
    Put32(graph, 0, kGraphVersion);
    Put32(graph, 8, static_cast<std::uint32_t>(areaBytes));
    Put32(graph, 12, static_cast<std::uint32_t>(kd.size()));
    Put32(graph, 24, 56); // m_sizeofSerializedLinkDat
    Put32(graph, 32, static_cast<std::uint32_t>(graphBytes));
    PutF(graph, 40, kBuildScale);
    PutF(graph, 44, kVoxelSize);
    PutF(graph, 48, kAgentRadius);
    PutF(graph, 52, kStepHeight);
    PutF(graph, 56, kAgentHeight);
    std::memcpy(graph.data() + 60, graphBox.lo.data(), 12);
    std::memcpy(graph.data() + 72, graphBox.hi.data(), 12);
    Put32(graph, 84, 2); // m_buildUpAxis: Z

    for (const Area &area : mesh.areas)
    {
        const std::size_t base = area.offset;
        std::memcpy(graph.data() + base + 32, area.center.data(), 12);
        float radius = 0.0f;
        for (const std::uint32_t vertex : area.vertices)
            radius = std::max(radius, Distance(area.center, mesh.corners[vertex]));
        PutF(graph, base + 44, radius);
        Put32(graph, base + 48, 0xFFFFFFFFu); // m_searchCost
        Put32(graph, base + 52, area.usage);
        const auto edgeCount = static_cast<std::uint32_t>(area.vertices.size());
        // The vertex farthest from the first edge completes the area basis.
        const Vec3 &v0 = mesh.corners[area.vertices[0]];
        const Vec3 edge = Sub(mesh.corners[area.vertices[1]], v0);
        std::uint32_t basis = 2;
        float farthest = -1.0f;
        for (std::uint32_t vertex = 2; vertex < edgeCount; ++vertex)
        {
            const float distance = Length(Cross(edge, Sub(mesh.corners[area.vertices[vertex]], v0)));
            if (distance > farthest + 1.0e-4f)
            {
                farthest = distance;
                basis = vertex;
            }
        }
        Put32(graph, base + 56, 0x3FFFFu << 7 | edgeCount);
        Put32(graph, base + 60, basis << 24 | 0x110000u);
        Put32(graph, base + 64, 0x80000000u);
        for (std::uint32_t edgeIndex = 0; edgeIndex < edgeCount; ++edgeIndex)
        {
            const std::size_t at = base + kAreaSize + kEdgeSize * edgeIndex;
            const std::int32_t adjacent = area.adjacent[edgeIndex];
            std::uint32_t cost = 0;
            if (adjacent >= 0)
            {
                const Area &other = mesh.areas[static_cast<std::size_t>(adjacent)];
                Put64(graph, at, other.offset);
                cost = static_cast<std::uint32_t>(
                    std::ceil(Distance(area.center, other.center) * 1000.0f / kBuildScale));
            }
            std::memcpy(graph.data() + at + 8, mesh.corners[area.vertices[edgeIndex]].data(), 12);
            Put32(graph, at + 20, 0xFFFC0000u);
            Put32(graph, at + 24, cost);
        }
    }
    std::memcpy(graph.data() + kGraphHeaderSize + areaBytes, kd.data(), kd.size());

    // bfx resource: ResourceHeader, one NavGraphImage section, image header.
    const std::size_t body = 12 + 12 + graph.size();
    std::vector<std::uint8_t> resource(24 + body, 0);
    Put32(resource, 4, 2);
    Put32(resource, 8, static_cast<std::uint32_t>(body));
    Put32(resource, 20, 0x1000001u);
    Put32(resource, 24, 0x10000u); // component 1, NavGraphImage
    Put32(resource, 28, static_cast<std::uint32_t>(12 + graph.size()));
    Put32(resource, 32, 1); // 64-bit pointers
    Put32(resource, 40, kGraphVersion);
    Put32(resource, 44, 1); // graph count
    std::memcpy(resource.data() + 48, graph.data(), graph.size());
    std::uint32_t checksum = 0;
    for (std::size_t at = 24; at < resource.size(); at += 4)
    {
        std::uint32_t word;
        std::memcpy(&word, resource.data() + at, 4);
        checksum += word;
    }
    Put32(resource, 12, checksum);
    return resource;
}

// ---------------------------------------------------------------------------
// Path nodes.
// ---------------------------------------------------------------------------
struct PathNode
{
    Vec3 origin{};
    std::uint32_t cell{};
};

std::int32_t BuildTree(std::vector<Data::TreeNode> &tree, const std::vector<PathNode> &nodes,
                       std::vector<std::uint16_t> &indices, const std::size_t first,
                       const std::size_t count)
{
    const auto index = static_cast<std::int32_t>(tree.size());
    tree.emplace_back();
    if (count <= 6)
    {
        tree[static_cast<std::size_t>(index)].nodes.assign(
            indices.begin() + static_cast<std::ptrdiff_t>(first),
            indices.begin() + static_cast<std::ptrdiff_t>(first + count));
        return index;
    }
    Box box;
    for (std::size_t at = first; at < first + count; ++at)
        box.Add(nodes[indices[at]].origin);
    const int axis = box.hi[1] - box.lo[1] > box.hi[0] - box.lo[0] ? 1 : 0;
    std::sort(indices.begin() + static_cast<std::ptrdiff_t>(first),
              indices.begin() + static_cast<std::ptrdiff_t>(first + count),
              [&](const std::uint16_t a, const std::uint16_t b) {
                  return nodes[a].origin[axis] < nodes[b].origin[axis];
              });
    const std::size_t half = count / 2;
    const float dist = (nodes[indices[first + half - 1]].origin[axis] +
                        nodes[indices[first + half]].origin[axis]) *
                       0.5f;
    // Path_NodesInCylinder: child[0] holds origins below dist.
    const std::int32_t left = BuildTree(tree, nodes, indices, first, half);
    const std::int32_t right = BuildTree(tree, nodes, indices, first + half, count - half);
    tree[static_cast<std::size_t>(index)].axis = axis;
    tree[static_cast<std::size_t>(index)].dist = dist;
    tree[static_cast<std::size_t>(index)].children = {left, right};
    return index;
}

std::int32_t BuildQuad(std::vector<Data::Quad> &quads, std::vector<Data::TacPoint> &points,
                       const std::vector<std::uint16_t> &members, const std::array<float, 2> &mid,
                       const std::array<float, 2> &half, const int depth)
{
    const auto index = static_cast<std::int32_t>(quads.size());
    quads.emplace_back();
    quads[static_cast<std::size_t>(index)].mid = mid;
    quads[static_cast<std::size_t>(index)].half = half;
    if (members.size() <= 8 || depth >= 10)
    {
        // Head insertion in point order (stock head.prev == 0xFFFF).
        std::uint16_t head = 0xFFFF;
        for (const std::uint16_t point : members)
        {
            points[point].next = head;
            points[point].prev = 0xFFFF;
            if (head != 0xFFFF)
                points[head].prev = point;
            head = point;
        }
        quads[static_cast<std::size_t>(index)].list = head;
        return index;
    }
    // Stock child order: (x-,y+), (x+,y+), (x-,y-), (x+,y-).
    std::array<std::vector<std::uint16_t>, 4> split;
    for (const std::uint16_t point : members)
    {
        const bool east = points[point].position[0] >= mid[0];
        const bool north = points[point].position[1] >= mid[1];
        split[(north ? 0u : 2u) + (east ? 1u : 0u)].push_back(point);
    }
    const std::array<float, 2> quarter{half[0] * 0.5f, half[1] * 0.5f};
    for (std::size_t child = 0; child < 4; ++child)
    {
        const std::array<float, 2> childMid{mid[0] + (child & 1u ? quarter[0] : -quarter[0]),
                                            mid[1] + (child < 2 ? quarter[1] : -quarter[1])};
        const std::int32_t built = BuildQuad(quads, points, split[child], childMid, quarter, depth + 1);
        quads[static_cast<std::size_t>(index)].children[child] = built;
    }
    return index;
}

std::uint8_t RangeByte(const float distance)
{
    return static_cast<std::uint8_t>(std::clamp(distance / 100.0f, 0.0f, 255.0f));
}
} // namespace

Result Build(const Source &source)
{
    Result result;
    std::vector<Vec3> seeds = source.spawns;
    seeds.insert(seeds.end(), source.pathNodes.begin(), source.pathNodes.end());
    if (seeds.empty())
    {
        zt::warn("navigation: no spawns or pathnodes; bot navigation is not generated");
        return result;
    }
    const auto started = std::chrono::steady_clock::now();
    const Scene scene(source);
    if (!scene.Bounds().Valid())
    {
        zt::warn("navigation: no blocking collision; bot navigation is not generated");
        return result;
    }

    Box region;
    for (const Vec3 &seed : seeds)
        region.Add(seed);
    for (std::size_t axis = 0; axis < 2; ++axis)
    {
        region.lo[axis] = std::max(region.lo[axis] - 1024.0f, scene.Bounds().lo[axis]);
        region.hi[axis] = std::min(region.hi[axis] + 1024.0f, scene.Bounds().hi[axis]);
    }
    Grid grid;
    if (!BuildGrid(scene, region, grid))
    {
        zt::warn("navigation: empty walkable region");
        return result;
    }
    const std::size_t rawCells = grid.layers.size();
    Erode(grid);

    // Keep what spawns and pathnodes can reach.
    std::vector<std::uint8_t> reached(grid.layers.size(), 0);
    std::vector<std::uint32_t> queue;
    std::size_t unseededSpawns = 0;
    for (const Vec3 &spawn : source.spawns)
    {
        const std::int32_t cell = Locate(grid, spawn, 96.0f, 40.0f, 6);
        if (cell < 0)
        {
            ++unseededSpawns;
            continue;
        }
        if (!reached[static_cast<std::size_t>(cell)])
        {
            reached[static_cast<std::size_t>(cell)] = 1;
            queue.push_back(static_cast<std::uint32_t>(cell));
        }
    }
    for (const Vec3 &node : source.pathNodes)
    {
        const std::int32_t cell = Locate(grid, node, 72.0f, 36.0f, 4);
        if (cell >= 0 && !reached[static_cast<std::size_t>(cell)])
        {
            reached[static_cast<std::size_t>(cell)] = 1;
            queue.push_back(static_cast<std::uint32_t>(cell));
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head)
        for (int direction = 0; direction < 4; ++direction)
        {
            const std::int32_t next = grid.Next(queue[head], direction);
            if (next >= 0 && !reached[static_cast<std::size_t>(next)])
            {
                reached[static_cast<std::size_t>(next)] = 1;
                queue.push_back(static_cast<std::uint32_t>(next));
            }
        }
    for (std::size_t cell = 0; cell < grid.layers.size(); ++cell)
        grid.kept[cell] = grid.kept[cell] && reached[cell];
    const std::size_t keptCells = queue.size();
    if (keptCells == 0)
    {
        zt::warn("navigation: no spawn reaches walkable ground; bot navigation is not generated");
        return result;
    }
    zt::info("navigation: %dx%d columns at %.1f units, %zu floor cells, %zu walkable after "
             "radius erosion and reachability (%zu spawn(s) off the grid)",
             grid.nx, grid.ny, grid.cell, rawCells, keptCells, unseededSpawns);

    // ---- navmesh
    // Area usage flags stay 0: bfx::AreaHandle::IsUsable (0x1FC9660) accepts
    // a zero mask for every agent filter, and bit 31 marks stairs
    // (AINavigator2D::IsOnStairs).
    Mesh mesh = BuildMesh(grid);
    const float skyTop = scene.Bounds().hi[2] + 64.0f;
    auto data = std::make_shared<Data>();
    Box graphBox;
    data->navGraph = BuildNavGraph(mesh, graphBox);
    std::size_t portals = 0;
    std::size_t edges = 0;
    for (const Area &area : mesh.areas)
    {
        edges += area.adjacent.size();
        portals += static_cast<std::size_t>(
            std::count_if(area.adjacent.begin(), area.adjacent.end(),
                          [](const std::int32_t adjacent) { return adjacent >= 0; }));
    }
    // nav_misc_s and the tactical quad tree use the graph bounds widened to
    // the origin (mp_shipment: y 0..2627.5 while its areas start at 1332).
    Box misc = graphBox;
    misc.Add(Vec3{0.0f, 0.0f, 0.0f});
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        data->staticHalf[axis] = (misc.hi[axis] - misc.lo[axis]) * 0.5f;
        data->staticMid[axis] = (misc.hi[axis] + misc.lo[axis]) * 0.5f;
    }
    zt::info("navigation: navmesh %zu areas, %zu edges (%zu portal sides), %zu corners, "
             "%zu-byte bfx resource",
             mesh.areas.size(), edges, portals, mesh.corners.size(), data->navGraph.size());

    // ---- pathnodes
    std::vector<PathNode> nodes;
    const auto addNode = [&](const std::uint32_t cell, const Vec3 &hint) {
        const int ix = grid.ColumnX(cell);
        const int iy = grid.ColumnY(cell);
        float x = grid.CenterX(ix);
        float y = grid.CenterY(iy);
        if (static_cast<int>(std::floor((hint[0] - grid.minX) / grid.cell)) == ix &&
            static_cast<int>(std::floor((hint[1] - grid.minY) / grid.cell)) == iy)
        {
            x = hint[0];
            y = hint[1];
        }
        const Vec3 origin{x, y, grid.HeightAt(cell, x, y)};
        for (const PathNode &existing : nodes)
            if (Distance(existing.origin, origin) < 8.0f)
                return;
        nodes.push_back({origin, cell});
    };
    std::size_t droppedMarkers = 0;
    for (const Vec3 &marker : source.pathNodes)
    {
        const std::int32_t cell = Locate(grid, marker, 72.0f, 36.0f, 4);
        if (cell < 0)
            ++droppedMarkers;
        else
            addNode(static_cast<std::uint32_t>(cell), marker);
    }
    const std::size_t markerNodes = nodes.size();
    for (const Vec3 &spawn : source.spawns)
    {
        const std::int32_t cell = Locate(grid, spawn, 96.0f, 40.0f, 6);
        if (cell < 0)
            continue;
        const Vec3 position = grid.Center(static_cast<std::uint32_t>(cell));
        const bool covered = std::any_of(nodes.begin(), nodes.end(), [&](const PathNode &node) {
            return Distance(node.origin, position) < 96.0f;
        });
        if (!covered)
            addNode(static_cast<std::uint32_t>(cell), spawn);
    }
    const std::size_t spawnNodes = nodes.size() - markerNodes;
    {
        // Fill gaps so every walkable cell is near a node.
        std::vector<int> distance(grid.layers.size(), std::numeric_limits<int>::max());
        const auto spread = [&](const std::uint32_t source) {
            std::vector<std::uint32_t> frontier{source};
            distance[source] = 0;
            for (std::size_t head = 0; head < frontier.size(); ++head)
            {
                const std::uint32_t cell = frontier[head];
                for (int direction = 0; direction < 4; ++direction)
                {
                    const std::int32_t next = grid.Next(cell, direction);
                    if (next >= 0 && distance[static_cast<std::size_t>(next)] > distance[cell] + 1)
                    {
                        distance[static_cast<std::size_t>(next)] = distance[cell] + 1;
                        frontier.push_back(static_cast<std::uint32_t>(next));
                    }
                }
            }
        };
        for (const PathNode &node : nodes)
            spread(node.cell);
        while (nodes.size() < kMaxFixedNodes)
        {
            std::size_t farthest = 0;
            int farthestDistance = -1;
            for (std::size_t cell = 0; cell < grid.layers.size(); ++cell)
                if (grid.kept[cell] && distance[cell] != std::numeric_limits<int>::max() &&
                    distance[cell] > farthestDistance)
                {
                    farthestDistance = distance[cell];
                    farthest = cell;
                }
            if (farthestDistance <= kCoverageCells)
                break;
            const std::size_t before = nodes.size();
            addNode(static_cast<std::uint32_t>(farthest),
                    grid.Center(static_cast<std::uint32_t>(farthest)));
            if (nodes.size() == before)
                break;
            spread(static_cast<std::uint32_t>(farthest));
        }
    }
    if (nodes.size() > kMaxFixedNodes)
        nodes.resize(kMaxFixedNodes);
    std::vector<std::uint8_t> spawnNode(nodes.size(), 0);
    for (std::size_t node = markerNodes; node < markerNodes + spawnNodes && node < nodes.size();
         ++node)
        spawnNode[node] = 1;

    // Links: straight walkable lines up to the stock 256-unit reach.
    std::vector<std::vector<Data::Link>> links(nodes.size());
    ParallelFor(nodes.size(), [&](const std::size_t a) {
        for (std::size_t b = 0; b < nodes.size(); ++b)
        {
            if (a == b)
                continue;
            const float distance = Distance(nodes[a].origin, nodes[b].origin);
            if (distance > kLinkDistance)
                continue;
            if (WalkableLine(grid, nodes[a].cell, nodes[a].origin, nodes[b].cell,
                             nodes[b].origin) &&
                WalkableLine(grid, nodes[b].cell, nodes[b].origin, nodes[a].cell,
                             nodes[a].origin))
                links[a].push_back({distance, static_cast<std::uint16_t>(b)});
        }
    });
    // Unlinked nodes are kept only where a spawn needs one within 300 units
    // (sv_bot_mp 273); linked nodes go first because the pathdata check
    // (sv_bot_mp 272) wants a linked node among the first ten.
    std::size_t droppedUnlinked = 0;
    {
        std::vector<std::size_t> order;
        for (std::size_t node = 0; node < nodes.size(); ++node)
            if (!links[node].empty())
                order.push_back(node);
        for (std::size_t node = 0; node < nodes.size(); ++node)
            if (links[node].empty() && spawnNode[node])
                order.push_back(node);
        droppedUnlinked = nodes.size() - order.size();
        std::vector<std::int32_t> remap(nodes.size(), -1);
        for (std::size_t index = 0; index < order.size(); ++index)
            remap[order[index]] = static_cast<std::int32_t>(index);
        std::vector<PathNode> keptNodes;
        std::vector<std::vector<Data::Link>> keptLinks;
        for (const std::size_t old : order)
        {
            keptNodes.push_back(nodes[old]);
            auto list = std::move(links[old]);
            for (Data::Link &link : list)
                link.node = static_cast<std::uint16_t>(remap[link.node]);
            keptLinks.push_back(std::move(list));
        }
        nodes = std::move(keptNodes);
        links = std::move(keptLinks);
    }
    const std::size_t fixedCount = nodes.size();
    if (fixedCount == 0 || links.front().empty())
    {
        zt::warn("navigation: no linked pathnodes; bot navigation is not generated");
        return result;
    }
    std::size_t linkCount = 0;
    std::size_t isolated = 0;
    for (const auto &list : links)
    {
        linkCount += list.size();
        isolated += list.empty() ? 1u : 0u;
    }

    // Zones: regions of the walkable cell graph around pathnode zoneSeeds picked
    // by farthest-point sampling, so every zone is contiguous and owns a node.
    std::vector<std::int32_t> zoneOf(fixedCount, -1);
    std::size_t zoneCount = 0;
    std::vector<std::vector<std::uint8_t>> zoneTouching;
    {
        const std::size_t total = grid.layers.size();
        const double walkArea = static_cast<double>(keptCells) * grid.cell * grid.cell;
        const std::size_t target = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::lround(walkArea / (420.0 * 420.0))), 2, 64);
        std::vector<int> distance(total, std::numeric_limits<int>::max());
        const auto spread = [&](const std::uint32_t source) {
            std::vector<std::uint32_t> frontier{source};
            distance[source] = 0;
            for (std::size_t head = 0; head < frontier.size(); ++head)
            {
                const std::uint32_t cell = frontier[head];
                for (int direction = 0; direction < 4; ++direction)
                {
                    const std::int32_t next = grid.Next(cell, direction);
                    if (next >= 0 && distance[static_cast<std::size_t>(next)] > distance[cell] + 1)
                    {
                        distance[static_cast<std::size_t>(next)] = distance[cell] + 1;
                        frontier.push_back(static_cast<std::uint32_t>(next));
                    }
                }
            }
        };
        std::vector<std::size_t> zoneSeeds;
        for (std::size_t node = 0; node < fixedCount; ++node)
            if (distance[nodes[node].cell] == std::numeric_limits<int>::max())
            {
                zoneSeeds.push_back(node);
                spread(nodes[node].cell);
            }
        while (zoneSeeds.size() < target && zoneSeeds.size() < 250)
        {
            std::size_t farthest = 0;
            int farthestDistance = -1;
            for (std::size_t node = 0; node < fixedCount; ++node)
                if (distance[nodes[node].cell] > farthestDistance)
                {
                    farthestDistance = distance[nodes[node].cell];
                    farthest = node;
                }
            if (farthestDistance <= 8)
                break;
            zoneSeeds.push_back(farthest);
            spread(nodes[farthest].cell);
        }
        std::vector<std::int32_t> cellZone(total, -1);
        std::vector<std::uint32_t> frontier;
        for (std::size_t zone = 0; zone < zoneSeeds.size(); ++zone)
        {
            const std::uint32_t cell = nodes[zoneSeeds[zone]].cell;
            if (cellZone[cell] < 0)
            {
                cellZone[cell] = static_cast<std::int32_t>(zone);
                frontier.push_back(cell);
            }
        }
        for (std::size_t head = 0; head < frontier.size(); ++head)
        {
            const std::uint32_t cell = frontier[head];
            for (int direction = 0; direction < 4; ++direction)
            {
                const std::int32_t next = grid.Next(cell, direction);
                if (next >= 0 && cellZone[static_cast<std::size_t>(next)] < 0)
                {
                    cellZone[static_cast<std::size_t>(next)] = cellZone[cell];
                    frontier.push_back(static_cast<std::uint32_t>(next));
                }
            }
        }
        // Seeds sharing a cell leave empty zones; renumber the used ones.
        std::vector<std::int32_t> renumber(zoneSeeds.size(), -1);
        for (std::size_t node = 0; node < fixedCount; ++node)
        {
            const std::int32_t zone = cellZone[nodes[node].cell];
            if (zone < 0)
                throw std::runtime_error("navigation pathnode lies outside every zone");
            if (renumber[static_cast<std::size_t>(zone)] < 0)
                renumber[static_cast<std::size_t>(zone)] = static_cast<std::int32_t>(zoneCount++);
            zoneOf[node] = renumber[static_cast<std::size_t>(zone)];
        }
        zoneTouching.assign(zoneCount, std::vector<std::uint8_t>(zoneCount, 0));
        for (std::uint32_t cell = 0; cell < total; ++cell)
        {
            if (!grid.kept[cell] || cellZone[cell] < 0)
                continue;
            const std::int32_t a = renumber[static_cast<std::size_t>(cellZone[cell])];
            for (int direction = 0; direction < 2; ++direction)
            {
                const std::int32_t next = grid.Next(cell, direction);
                if (next < 0 || cellZone[static_cast<std::size_t>(next)] < 0)
                    continue;
                const std::int32_t b =
                    renumber[static_cast<std::size_t>(cellZone[static_cast<std::size_t>(next)])];
                if (a >= 0 && b >= 0 && a != b)
                    zoneTouching[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)] =
                        zoneTouching[static_cast<std::size_t>(b)][static_cast<std::size_t>(a)] = 1;
            }
        }
    }

    // Zone nodes follow the 64 dynamic slots, positioned on the member
    // closest to the zone centroid and linked to touching zones.
    std::vector<std::size_t> zoneAnchor(zoneCount, 0);
    {
        std::vector<Vec3> sums(zoneCount, Vec3{});
        std::vector<std::size_t> counts(zoneCount, 0);
        for (std::size_t node = 0; node < fixedCount; ++node)
        {
            sums[static_cast<std::size_t>(zoneOf[node])] =
                Add(sums[static_cast<std::size_t>(zoneOf[node])], nodes[node].origin);
            ++counts[static_cast<std::size_t>(zoneOf[node])];
        }
        std::vector<float> best(zoneCount, kInfinity);
        for (std::size_t node = 0; node < fixedCount; ++node)
        {
            const auto zone = static_cast<std::size_t>(zoneOf[node]);
            const Vec3 centroid = Scale(sums[zone], 1.0f / static_cast<float>(counts[zone]));
            const float distance = Distance(centroid, nodes[node].origin);
            if (distance < best[zone])
            {
                best[zone] = distance;
                zoneAnchor[zone] = node;
            }
        }
    }
    const std::size_t nodeCount = fixedCount + kDynamicNodeSlots + zoneCount;
    if (nodeCount > 0xFFFE)
        throw std::runtime_error("navigation produced too many path nodes");
    data->nodes.resize(nodeCount);
    data->fixedNodeCount = static_cast<std::uint32_t>(fixedCount);
    data->zoneCount = static_cast<std::uint32_t>(zoneCount);
    for (std::size_t node = 0; node < fixedCount; ++node)
    {
        Data::Node &out = data->nodes[node];
        out.origin = nodes[node].origin;
        out.type = kPathnodeType;
        out.tacpoint = static_cast<std::int32_t>(node);
        out.forward = {1.0f, 0.0f};
        out.links = std::move(links[node]);
    }
    for (std::size_t zone = 0; zone < zoneCount; ++zone)
    {
        Data::Node &out = data->nodes[fixedCount + kDynamicNodeSlots + zone];
        out.origin = nodes[zoneAnchor[zone]].origin;
        for (std::size_t other = 0; other < zoneCount; ++other)
            if (zoneTouching[zone][other])
                out.links.push_back(
                    {Distance(nodes[zoneAnchor[zone]].origin, nodes[zoneAnchor[other]].origin),
                     static_cast<std::uint16_t>(fixedCount + kDynamicNodeSlots + other)});
    }
    data->zones.assign(nodeCount, 0xFF);
    for (std::size_t node = 0; node < fixedCount; ++node)
        data->zones[node] = static_cast<std::uint8_t>(zoneOf[node]);
    for (std::size_t zone = 0; zone < zoneCount; ++zone)
        data->zones[fixedCount + kDynamicNodeSlots + zone] = static_cast<std::uint8_t>(zone);
    data->noPeek.assign(nodeCount * 2, 0xFF);
    {
        std::vector<std::uint16_t> indices(fixedCount);
        std::iota(indices.begin(), indices.end(), std::uint16_t{0});
        BuildTree(data->tree, nodes, indices, 0, fixedCount);
    }

    // Path extra data: 33 bytes per node (Path_NodeExposure*: octant
    // (yaw + 180) / 45, stance stride 8: stand, crouch, prone, traffic; byte
    // 32 traffic), then two sky bitsets (lenient, strict).
    const std::size_t skyBytes = (nodeCount + 7) / 8;
    data->exposure.assign(nodeCount * 33 + skyBytes * 2, 0);
    ParallelFor(fixedCount, [&](const std::size_t node) {
        const Vec3 &origin = nodes[node].origin;
        std::uint8_t *out = data->exposure.data() + node * 33;
        constexpr float kRange = 25500.0f;
        for (std::size_t octant = 0; octant < 8; ++octant)
            for (std::size_t stance = 0; stance < 3; ++stance)
            {
                const Vec3 eye{origin[0], origin[1], origin[2] + kStanceEyes[stance]};
                float longest = 0.0f;
                for (const float offset : {11.25f, 22.5f, 33.75f})
                {
                    const float yaw =
                        (-180.0f + 45.0f * static_cast<float>(octant) + offset) * 0.0174532925f;
                    const Vec3 end{eye[0] + std::cos(yaw) * kRange,
                                   eye[1] + std::sin(yaw) * kRange, eye[2]};
                    bool sky = false;
                    const float fraction = scene.FirstHit(eye, end, kBlocksSight, sky);
                    longest = std::max(longest, std::min(fraction, 1.0f) * kRange);
                }
                out[stance * 8 + octant] = RangeByte(longest);
                if (stance == 0)
                    out[24 + octant] = RangeByte(longest);
            }
        const Vec3 eye{origin[0], origin[1], origin[2] + kEyeHeight};
        int open = 0;
        constexpr int kSkyRays = 5;
        for (int ray = 0; ray < kSkyRays; ++ray)
        {
            Vec3 end{eye[0], eye[1], skyTop};
            if (ray)
            {
                const float yaw = static_cast<float>(ray - 1) * 1.5707963f;
                const float reach = (skyTop - eye[2]) * 0.577f; // 30 degrees off vertical
                end[0] += std::cos(yaw) * reach;
                end[1] += std::sin(yaw) * reach;
            }
            bool sky = false;
            if (scene.FirstHit(eye, end, kBlocksSight, sky) > 1.0f || sky)
                ++open;
        }
        // The sky bitsets share bytes across nodes; park the count in this
        // node's own traffic byte and apply the bits serially below.
        out[32] = static_cast<std::uint8_t>(open);
    });
    for (std::size_t node = 0; node < fixedCount; ++node)
    {
        std::uint8_t &marker = data->exposure[node * 33 + 32];
        const int open = marker;
        marker = 0;
        const auto bit = static_cast<std::uint8_t>(1u << (node % 8));
        if (open > 0)
            data->exposure[nodeCount * 33 + node / 8] |= bit;
        if (open == 5)
            data->exposure[nodeCount * 33 + skyBytes + node / 8] |= bit;
    }

    // ---- tactical graph: one point per fixed pathnode (m_PointID = node
    // index), then a coarse ground grid; eye height for mp_ maps.
    std::vector<Data::TacPoint> &points = data->tacPoints;
    for (std::size_t node = 0; node < fixedCount; ++node)
        points.push_back({Add(nodes[node].origin, {0.0f, 0.0f, kEyeHeight}), 1});
    {
        const int spacing = std::max(1, static_cast<int>(std::lround(96.0f / grid.cell)));
        for (std::size_t cell = 0; cell < grid.layers.size() && points.size() < 4000; ++cell)
        {
            if (!grid.kept[cell])
                continue;
            const int ix = grid.ColumnX(static_cast<std::uint32_t>(cell));
            const int iy = grid.ColumnY(static_cast<std::uint32_t>(cell));
            if (ix % spacing != spacing / 2 || iy % spacing != spacing / 2)
                continue;
            const Vec3 ground = grid.Center(static_cast<std::uint32_t>(cell));
            const bool near = std::any_of(nodes.begin(), nodes.end(), [&](const PathNode &node) {
                const float dx = node.origin[0] - ground[0];
                const float dy = node.origin[1] - ground[1];
                return dx * dx + dy * dy < 48.0f * 48.0f &&
                       std::abs(node.origin[2] - ground[2]) < 40.0f;
            });
            if (!near)
                points.push_back({Add(ground, {0.0f, 0.0f, kEyeHeight}), 0});
        }
    }
    const std::size_t pointCount = points.size();
    if (pointCount < 2 || pointCount > 0xFFFE)
        throw std::runtime_error("navigation tactical graph has an invalid point count");
    data->visRows.resize(pointCount - 1);
    ParallelFor(pointCount - 1, [&](const std::size_t row) {
        const std::size_t start = row + 1;
        const std::size_t end = pointCount - 1;
        std::vector<std::uint8_t> &bits = data->visRows[row];
        bits.assign((end - start) / 8 + 1, 0);
        for (std::size_t other = start; other <= end; ++other)
            if (!scene.Blocked(points[row].position, points[other].position, kBlocksSight))
                bits[(other - start) / 8] |= static_cast<std::uint8_t>(1u << ((other - start) % 8));
    });
    std::size_t visBytes = 0;
    std::size_t visiblePairs = 0;
    for (const auto &row : data->visRows)
    {
        visBytes += row.size() + 32;
        for (const std::uint8_t byte : row)
            visiblePairs += static_cast<std::size_t>(std::popcount(byte));
    }
    data->visBytes = static_cast<std::int32_t>(visBytes);
    {
        std::vector<std::uint16_t> members(pointCount);
        std::iota(members.begin(), members.end(), std::uint16_t{0});
        BuildQuad(data->quads, points, members,
                  {data->staticMid[0], data->staticMid[1]},
                  {data->staticHalf[0], data->staticHalf[1]}, 0);
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    zt::info("navigation: %zu fixed pathnodes (%zu IW3 markers placed, %zu off-mesh; %zu spawn "
             "and %zu coverage nodes; %zu unlinked dropped), %zu links, %zu unlinked spawn "
             "nodes, %zu zones",
             fixedCount, markerNodes, droppedMarkers, spawnNodes,
             fixedCount + droppedUnlinked - markerNodes - spawnNodes, droppedUnlinked, linkCount,
             isolated, zoneCount);
    zt::info("navigation: tactical graph %zu points, %zu visible pairs, %zu vis bytes; %.1fs",
             pointCount, visiblePairs, visBytes, seconds);
    if (const char *debugPath = std::getenv("IW8_ZONETOOL_NAV_DEBUG"); debugPath && *debugPath)
    {
        // Diagnostic: top walkable height per column, area outlines, nodes,
        // links and zones as JSON for offline rendering.
        if (FILE *file = std::fopen(debugPath, "wb"))
        {
            std::fprintf(file, "{\"minX\":%g,\"minY\":%g,\"cell\":%g,\"nx\":%d,\"ny\":%d,\"top\":[",
                         grid.minX, grid.minY, grid.cell, grid.nx, grid.ny);
            const std::size_t columns = static_cast<std::size_t>(grid.nx) * grid.ny;
            for (std::size_t column = 0; column < columns; ++column)
            {
                float top = -99999.0f;
                for (std::uint32_t cell = grid.columnStart[column]; cell < grid.columnStart[column + 1];
                     ++cell)
                    if (grid.kept[cell])
                        top = std::max(top, grid.layers[cell].z);
                std::fprintf(file, column ? ",%.0f" : "%.0f", top);
            }
            std::fprintf(file, "],\"areas\":[");
            for (std::size_t index = 0; index < mesh.areas.size(); ++index)
            {
                std::fprintf(file, index ? ",[" : "[");
                const Area &area = mesh.areas[index];
                for (std::size_t vertex = 0; vertex < area.vertices.size(); ++vertex)
                {
                    const Vec3 &corner = mesh.corners[area.vertices[vertex]];
                    std::fprintf(file, vertex ? ",[%.1f,%.1f,%.1f,%d]" : "[%.1f,%.1f,%.1f,%d]",
                                 corner[0], corner[1], corner[2], area.adjacent[vertex]);
                }
                std::fprintf(file, "]");
            }
            std::fprintf(file, "],\"nodes\":[");
            for (std::size_t node = 0; node < data->nodes.size(); ++node)
            {
                const Data::Node &out = data->nodes[node];
                std::fprintf(file, node ? ",[%.1f,%.1f,%.1f,%u,[" : "[%.1f,%.1f,%.1f,%u,[",
                             out.origin[0], out.origin[1], out.origin[2], data->zones[node]);
                for (std::size_t link = 0; link < out.links.size(); ++link)
                    std::fprintf(file, link ? ",%u" : "%u", out.links[link].node);
                std::fprintf(file, "]]");
            }
            std::fprintf(file, "],\"fixed\":%u,\"zones\":%u,\"spawns\":[", data->fixedNodeCount,
                         data->zoneCount);
            for (std::size_t spawn = 0; spawn < source.spawns.size(); ++spawn)
                std::fprintf(file, spawn ? ",[%.1f,%.1f,%.1f]" : "[%.1f,%.1f,%.1f]",
                             source.spawns[spawn][0], source.spawns[spawn][1],
                             source.spawns[spawn][2]);
            std::fprintf(file, "]}");
            std::fclose(file);
            zt::info("navigation: wrote debug dump %s", debugPath);
        }
    }
    result.data = std::move(data);
    return result;
}

std::vector<Vec3> FixedPathNodes(const Result &result)
{
    std::vector<Vec3> origins;
    if (!result.data)
        return origins;
    origins.reserve(result.data->fixedNodeCount);
    for (std::uint32_t node = 0; node < result.data->fixedNodeCount; ++node)
        origins.push_back(result.data->nodes[node].origin);
    return origins;
}

namespace
{
void Stamp16(std::uint8_t *base, const std::size_t offset, const std::uint16_t value)
{
    std::memcpy(base + offset, &value, 2);
}
void Stamp32(std::uint8_t *base, const std::size_t offset, const std::uint32_t value)
{
    std::memcpy(base + offset, &value, 4);
}
void StampF(std::uint8_t *base, const std::size_t offset, const float value)
{
    std::memcpy(base + offset, &value, 4);
}
void Stamp64(std::uint8_t *base, const std::size_t offset, const std::uint64_t value)
{
    std::memcpy(base + offset, &value, 8);
}

// Root object in stream 1 for Load, stream 2 for Preload/Postload (as GlassWorld).
template <typename Body>
void EmitRoot(ZoneWriter &zw, const std::uint8_t *root, const std::size_t size, Body &&body)
{
    zw.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
    zw.align(7);
    zw.write(root, size);
    zw.pushStream(XFILE_BLOCK_VIRTUAL);
    body();
    zw.popStream();
    zw.popStream();
    zw.pushStream(XFILE_BLOCK_TEMP_POSTLOAD);
    zw.align(7);
    zw.reserveCalc(size);
    zw.align(31);
    zw.popStream();
}

void EmitTree(ZoneWriter &zw, const Data &data, const std::int32_t index)
{
    const Data::TreeNode &node = data.tree[static_cast<std::size_t>(index)];
    std::array<std::uint8_t, 24> record{};
    Stamp32(record.data(), 0, static_cast<std::uint32_t>(node.axis));
    StampF(record.data(), 4, node.dist);
    if (node.axis < 0)
    {
        Stamp32(record.data(), 8, static_cast<std::uint32_t>(node.nodes.size()));
        Stamp64(record.data(), 16, node.nodes.empty() ? PTR_NULL : PTR_FOLLOWS);
    }
    else
    {
        Stamp64(record.data(), 8, PTR_FOLLOWS);
        Stamp64(record.data(), 16, PTR_FOLLOWS);
    }
    zw.write(record.data(), record.size());
    if (node.axis < 0)
    {
        if (!node.nodes.empty())
        {
            zw.align(1);
            zw.write(node.nodes.data(), node.nodes.size() * 2);
        }
        return;
    }
    for (const std::int32_t child : node.children)
    {
        zw.align(7);
        EmitTree(zw, data, child);
    }
}

void EmitPathData(ZoneWriter &zw, const std::string &name, const Data &data)
{
    const auto nodeCount = static_cast<std::uint32_t>(data.nodes.size());
    std::array<std::uint8_t, 0x98> root{};
    Stamp64(root.data(), 0x00, PTR_FOLLOWS);
    Stamp32(root.data(), 0x08, nodeCount);
    Stamp64(root.data(), 0x10, PTR_FOLLOWS);
    Stamp16(root.data(), 0x1A, kPathDataVersion);
    Stamp32(root.data(), 0x28, 1);
    Stamp64(root.data(), 0x30, PTR_FOLLOWS);
    root[0x48] = 1; // usePathExtraData
    Stamp32(root.data(), 0x4C, static_cast<std::uint32_t>(data.exposure.size()));
    Stamp64(root.data(), 0x50, PTR_FOLLOWS);
    Stamp32(root.data(), 0x58, static_cast<std::uint32_t>(data.noPeek.size()));
    Stamp64(root.data(), 0x60, PTR_FOLLOWS);
    Stamp32(root.data(), 0x68, data.zoneCount);
    Stamp32(root.data(), 0x6C, static_cast<std::uint32_t>(data.zones.size()));
    Stamp64(root.data(), 0x70, PTR_FOLLOWS);
    Stamp32(root.data(), 0x78, data.fixedNodeCount);
    Stamp32(root.data(), 0x7C, kDynamicNodeSlots);
    EmitRoot(zw, root.data(), root.size(), [&] {
        zw.writeStr(name);
        zw.align(7);
        std::vector<std::uint8_t> nodes(static_cast<std::size_t>(nodeCount) * 192, 0);
        for (std::uint32_t index = 0; index < nodeCount; ++index)
        {
            const Data::Node &node = data.nodes[index];
            std::uint8_t *out = nodes.data() + static_cast<std::size_t>(index) * 192;
            std::memcpy(out + 0x20, node.origin.data(), 12); // vLocalOrigin
            StampF(out, 0x2C, 0.0f);                         // yaw_orient.fLocalAngle
            StampF(out, 0x30, node.forward[0]);
            StampF(out, 0x34, node.forward[1]);
            Stamp16(out, 0x40, 0xFFFF); // wOverlapNode
            Stamp16(out, 0x42, 0xFFFF);
            if (node.tacpoint >= 0)
            {
                Stamp16(out, 0x44, static_cast<std::uint16_t>(node.tacpoint));
                out[0x46] = 1; // m_GraphIDPlusOne
            }
            if (!node.links.empty())
                Stamp64(out, 0x48, PTR_FOLLOWS);
            Stamp16(out, 0x50, static_cast<std::uint16_t>(node.links.size()));
            Stamp16(out, 0x52, node.type);
        }
        zw.write(nodes.data(), nodes.size());
        for (const Data::Node &node : data.nodes)
        {
            if (node.links.empty())
                continue;
            zw.align(3);
            std::vector<std::uint8_t> links(node.links.size() * 12, 0);
            for (std::size_t link = 0; link < node.links.size(); ++link)
            {
                StampF(links.data(), link * 12, node.links[link].distance);
                Stamp16(links.data(), link * 12 + 4, node.links[link].node);
            }
            zw.write(links.data(), links.size());
        }
        zw.align(7);
        EmitTree(zw, data, 0);
        zw.write(data.exposure.data(), data.exposure.size());
        zw.write(data.noPeek.data(), data.noPeek.size());
        zw.write(data.zones.data(), data.zones.size());
    });
}

void EmitNavMesh(ZoneWriter &zw, const std::string &name, const Data &data)
{
    std::array<std::uint8_t, 0xA8> root{};
    Stamp64(root.data(), 0x00, PTR_FOLLOWS);
    Stamp32(root.data(), 0x08, static_cast<std::uint32_t>(kNavMeshVersion));
    Stamp32(root.data(), 0x0C, 1); // hasExposureData
    Stamp32(root.data(), 0x10, 1); // numNavResources
    Stamp64(root.data(), 0x18, PTR_FOLLOWS);
    std::memcpy(root.data() + 0x90, data.staticHalf.data(), 12);
    std::memcpy(root.data() + 0x9C, data.staticMid.data(), 12);
    EmitRoot(zw, root.data(), root.size(), [&] {
        zw.writeStr(name);
        zw.align(7);
        std::array<std::uint8_t, 0x70> resource{};
        Stamp32(resource.data(), 0x04, 0xFFFFFFFFu); // modelIdx -1: world mesh
        Stamp32(resource.data(), 0x18, static_cast<std::uint32_t>(data.navGraph.size()));
        Stamp64(resource.data(), 0x60, PTR_FOLLOWS);
        zw.write(resource.data(), resource.size());
        zw.align(3);
        zw.write(data.navGraph.data(), data.navGraph.size());
    });
}

void EmitQuadChildren(ZoneWriter &zw, const Data &data, const Data::Quad &quad)
{
    if (quad.children[0] < 0)
        return;
    zw.align(7);
    for (std::size_t child = 0; child < 4; ++child)
        zw.writeFollows();
    for (const std::int32_t child : quad.children)
    {
        const Data::Quad &node = data.quads[static_cast<std::size_t>(child)];
        std::array<std::uint8_t, 32> record{};
        StampF(record.data(), 0x00, node.mid[0]);
        StampF(record.data(), 0x04, node.mid[1]);
        StampF(record.data(), 0x08, node.half[0]);
        StampF(record.data(), 0x0C, node.half[1]);
        Stamp16(record.data(), 0x10, node.list);
        Stamp64(record.data(), 0x18, node.children[0] >= 0 ? PTR_FOLLOWS : PTR_NULL);
        zw.align(7);
        zw.write(record.data(), record.size());
        EmitQuadChildren(zw, data, node);
    }
}

void EmitTacGraph(ZoneWriter &zw, const std::string &name, const Data &data)
{
    std::array<std::uint8_t, 24> root{};
    Stamp64(root.data(), 0x00, PTR_FOLLOWS);
    Stamp64(root.data(), 0x08, PTR_FOLLOWS);
    Stamp32(root.data(), 0x10, 1);
    EmitRoot(zw, root.data(), root.size(), [&] {
        zw.writeStr(name);
        zw.align(7);
        const auto pointCount = static_cast<std::uint16_t>(data.tacPoints.size());
        const Data::Quad &top = data.quads.front();
        std::array<std::uint8_t, 56> graph{};
        Stamp64(graph.data(), 0x00, PTR_FOLLOWS);
        Stamp16(graph.data(), 0x08, pointCount);
        Stamp32(graph.data(), 0x0C, static_cast<std::uint32_t>(data.visBytes));
        Stamp64(graph.data(), 0x10, data.visRows.empty() ? PTR_NULL : PTR_FOLLOWS);
        StampF(graph.data(), 0x18, top.mid[0]);
        StampF(graph.data(), 0x1C, top.mid[1]);
        StampF(graph.data(), 0x20, top.half[0]);
        StampF(graph.data(), 0x24, top.half[1]);
        Stamp16(graph.data(), 0x28, top.list);
        Stamp64(graph.data(), 0x30, top.children[0] >= 0 ? PTR_FOLLOWS : PTR_NULL);
        zw.write(graph.data(), graph.size());

        zw.align(3);
        std::vector<std::uint8_t> points(data.tacPoints.size() * 40, 0);
        for (std::size_t index = 0; index < data.tacPoints.size(); ++index)
        {
            const Data::TacPoint &point = data.tacPoints[index];
            std::uint8_t *out = points.data() + index * 40;
            std::memcpy(out, point.position.data(), 12);
            out[0x0F] = point.type;
            Stamp16(out, 0x20, point.next);
            Stamp16(out, 0x22, point.prev);
        }
        zw.write(points.data(), points.size());

        if (!data.visRows.empty())
        {
            zw.align(7);
            std::vector<std::uint8_t> rows(data.visRows.size() * 16, 0);
            for (std::size_t row = 0; row < data.visRows.size(); ++row)
            {
                Stamp16(rows.data(), row * 16, static_cast<std::uint16_t>(row + 1));
                Stamp16(rows.data(), row * 16 + 2, static_cast<std::uint16_t>(pointCount - 1));
                Stamp64(rows.data(), row * 16 + 8, PTR_FOLLOWS);
            }
            zw.write(rows.data(), rows.size());
            for (const auto &row : data.visRows)
                zw.write(row.data(), row.size());
        }
        EmitQuadChildren(zw, data, top);
    });
}
} // namespace

void Register(ZoneWriter &writer, const std::string &assetName, const Result &result)
{
    if (!result.data)
        return;
    const auto data = result.data;
    writer.add(ASSET_TYPE_AIPATHS, assetName,
               [assetName, data](ZoneWriter &zw) { EmitPathData(zw, assetName, *data); });
    writer.add(ASSET_TYPE_NAVMESH, assetName,
               [assetName, data](ZoneWriter &zw) { EmitNavMesh(zw, assetName, *data); });
    writer.add(ASSET_TYPE_TACGRAPH, assetName,
               [assetName, data](ZoneWriter &zw) { EmitTacGraph(zw, assetName, *data); });
}
} // namespace iw8::navigation
