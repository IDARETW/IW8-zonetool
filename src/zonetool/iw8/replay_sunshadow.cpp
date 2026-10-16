#include "replay_sunshadow.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

namespace replaysunshadow
{
namespace
{
constexpr std::uint32_t TileResolution = 512;
constexpr std::size_t MaximumBytes = 0x7ffffffcu;

void Store(std::vector<std::uint8_t> &bytes, const std::size_t offset,
           const std::uint32_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::uint32_t Read(const std::vector<std::uint8_t> &bytes, const std::size_t offset)
{
    std::uint32_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void CheckDepthRange(const float origin, const float inverseSpan)
{
    if (!std::isfinite(origin) || !std::isfinite(inverseSpan) || inverseSpan <= 0.0f)
        throw std::runtime_error("invalid compressed sun-shadow depth range");
}
} // namespace

std::vector<std::uint8_t> EncodeTile(const std::span<const float> depths,
                                    const float origin, const float inverseSpan)
{
    if (depths.size() != TileResolution * TileResolution)
        throw std::runtime_error("compressed sun-shadow tile must contain 512x512 depths");
    CheckDepthRange(origin, inverseSpan);
    for (const float value : depths)
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
            throw std::runtime_error("compressed sun-shadow depth is outside [0, 1]");

    struct Node
    {
        std::size_t offset;
        unsigned x, y, size;
    };
    std::vector<std::uint8_t> tile(80);
    Store(tile, 4, TileResolution);
    Store(tile, 8, std::bit_cast<std::uint32_t>(1.0f / TileResolution));
    Store(tile, 16, 0x00020000);
    Store(tile, 24, std::bit_cast<std::uint32_t>(origin));
    Store(tile, 28, std::bit_cast<std::uint32_t>(inverseSpan));
    // Replay's prepass and sunvis start at one of four 12-byte roots. Root +4
    // supplies inherited depth; +8 is not read by either valid-tree traversal.
    std::vector<Node> pending;
    for (unsigned root = 0; root < 4; ++root)
        pending.push_back({32 + 12 * root, (root & 1) * 256u,
                           (root >> 1) * 256u, 256});

    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor)
    {
        const Node node = pending[cursor];
        const unsigned childSize = node.size / 2;
        const std::size_t base = tile.size();
        if (base <= node.offset || base - node.offset > 0x00ffffffu)
            throw std::runtime_error("compressed sun-shadow child offset exceeds 24 bits");
        std::uint32_t packed = static_cast<std::uint32_t>(base - node.offset) << 8;
        for (unsigned child = 0; child < 4; ++child)
        {
            const unsigned x = node.x + (child & 1) * childSize;
            const unsigned y = node.y + (child >> 1) * childSize;
            const auto first = std::bit_cast<std::uint32_t>(depths[y * TileResolution + x]);
            bool uniform = true;
            for (unsigned row = y; row < y + childSize && uniform; ++row)
                for (unsigned col = x; col < x + childSize; ++col)
                    if (std::bit_cast<std::uint32_t>(depths[row * TileResolution + col]) != first)
                    {
                        uniform = false;
                        break;
                    }

            // Type 1 retains the root's inherited zero. Type 3 owns an exact
            // float leaf. Only positive zero can be omitted as type 0.
            const unsigned type = uniform ? (first == 0 ? 0 : 3) : 1;
            packed |= type << (2 * child);
            if (type)
            {
                const std::size_t offset = tile.size();
                tile.resize(offset + 4);
                if (type == 1)
                    pending.push_back({offset, x, y, childSize});
                else
                    Store(tile, offset, first);
            }
        }
        Store(tile, node.offset, packed);
    }
    Store(tile, 0, static_cast<std::uint32_t>(tile.size()));
    Store(tile, 12, static_cast<std::uint32_t>(tile.size()));
    return tile;
}

std::vector<std::uint8_t> EncodeIntervalTile(const std::span<const float> upper,
                                             const std::span<const float> lower,
                                             const float origin, const float inverseSpan,
                                             float &uniform)
{
    constexpr std::size_t texels = TileResolution * TileResolution;
    if (upper.size() != texels || lower.size() != texels)
        throw std::runtime_error("compressed sun-shadow tile must contain 512x512 intervals");
    CheckDepthRange(origin, inverseSpan);
    // Level k holds (512 >> k)^2 nodes: the lowest upper bound and the highest
    // lower bound of their texels, and whether any texel has a caster. A texel
    // without a caster still bounds the stored depth: whatever receives there
    // (water and other non-casting surfaces, players) is lit, so a shared leaf
    // must stay below the lowest receiver (normalized depth inverseSpan, the
    // one-unit padding). Treating those texels as free let a leaf carry a
    // neighbouring caster's depth over open water (Discovery's ship shadow).
    const float floorDepth = 0.5f * inverseSpan;
    constexpr unsigned levels = 10;
    std::array<std::vector<float>, levels> high, low;
    std::array<std::vector<std::uint8_t>, levels> occupied;
    high[0].resize(texels);
    low[0].resize(texels);
    occupied[0].resize(texels);
    for (std::size_t index = 0; index < texels; ++index)
    {
        if (!std::isfinite(upper[index]) || upper[index] < 0.0f || upper[index] > 1.0f ||
            std::isnan(lower[index]))
            throw std::runtime_error("compressed sun-shadow depth is outside [0, 1]");
        const bool caster = upper[index] > 0.0f;
        high[0][index] = caster ? upper[index] : floorDepth;
        low[0][index] = caster ? lower[index] : -INFINITY;
        occupied[0][index] = caster;
    }
    for (unsigned level = 1; level < levels; ++level)
    {
        const unsigned size = TileResolution >> level, previous = size * 2;
        high[level].resize(std::size_t(size) * size);
        low[level].resize(std::size_t(size) * size);
        occupied[level].resize(std::size_t(size) * size);
        for (unsigned y = 0; y < size; ++y)
            for (unsigned x = 0; x < size; ++x)
            {
                float h = INFINITY, l = -INFINITY;
                std::uint8_t any = 0;
                for (unsigned child = 0; child < 4; ++child)
                {
                    const std::size_t from =
                        std::size_t(y * 2 + (child >> 1)) * previous + x * 2 + (child & 1);
                    h = std::min(h, high[level - 1][from]);
                    l = std::max(l, low[level - 1][from]);
                    any |= occupied[level - 1][from];
                }
                high[level][std::size_t(y) * size + x] = h;
                low[level][std::size_t(y) * size + x] = l;
                occupied[level][std::size_t(y) * size + x] = any;
            }
    }
    uniform = 0.0f;
    if (!occupied[levels - 1][0])
        throw std::runtime_error("compressed sun-shadow tile has no caster");
    if (low[levels - 1][0] < high[levels - 1][0])
    {
        uniform = high[levels - 1][0];
        return {};
    }

    struct Node
    {
        std::size_t offset;
        unsigned x, y, size;
    };
    std::vector<std::uint8_t> tile(80);
    Store(tile, 4, TileResolution);
    Store(tile, 8, std::bit_cast<std::uint32_t>(1.0f / TileResolution));
    Store(tile, 16, 0x00020000);
    Store(tile, 24, std::bit_cast<std::uint32_t>(origin));
    Store(tile, 28, std::bit_cast<std::uint32_t>(inverseSpan));
    std::vector<Node> pending;
    for (unsigned root = 0; root < 4; ++root)
        pending.push_back({32 + 12 * root, (root & 1) * 256u, (root >> 1) * 256u, 256});
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor)
    {
        const Node node = pending[cursor];
        const unsigned childSize = node.size / 2;
        const unsigned level = std::countr_zero(childSize);
        const unsigned levelWidth = TileResolution >> level;
        const std::size_t base = tile.size();
        if (base <= node.offset || base - node.offset > 0x00ffffffu)
            throw std::runtime_error("compressed sun-shadow child offset exceeds 24 bits");
        std::uint32_t packed = static_cast<std::uint32_t>(base - node.offset) << 8;
        for (unsigned child = 0; child < 4; ++child)
        {
            const unsigned x = node.x + (child & 1) * childSize;
            const unsigned y = node.y + (child >> 1) * childSize;
            const std::size_t index = std::size_t(y >> level) * levelWidth + (x >> level);
            const float h = high[level][index], l = low[level][index];
            // 0: no caster (inherited zero), 3: one depth for the block, 1: split.
            const unsigned type = !occupied[level][index] ? 0 : (childSize == 1 || l < h) ? 3 : 1;
            packed |= type << (2 * child);
            if (!type)
                continue;
            const std::size_t offset = tile.size();
            tile.resize(offset + 4);
            if (type == 1)
                pending.push_back({offset, x, y, childSize});
            else
                Store(tile, offset, std::bit_cast<std::uint32_t>(h));
        }
        Store(tile, node.offset, packed);
    }
    Store(tile, 0, static_cast<std::uint32_t>(tile.size()));
    Store(tile, 12, static_cast<std::uint32_t>(tile.size()));

    // Decode the tree and check every texel against its interval: first
    // surfaces stay lit (stored <= upper), the surface behind stays shadowed
    // (stored > lower), texels without a caster stay lit (stored <= floor).
    std::vector<float> decoded(texels, 0.0f);
    std::vector<Node> walk;
    for (unsigned root = 0; root < 4; ++root)
        walk.push_back({32 + 12 * root, (root & 1) * 256u, (root >> 1) * 256u, 256});
    while (!walk.empty())
    {
        const Node node = walk.back();
        walk.pop_back();
        const std::uint32_t packed = Read(tile, node.offset);
        std::size_t child = node.offset + (packed >> 8);
        const unsigned childSize = node.size / 2;
        for (unsigned index = 0; index < 4; ++index)
        {
            const unsigned type = (packed >> (2 * index)) & 3;
            if (!type)
                continue;
            const unsigned x = node.x + (index & 1) * childSize;
            const unsigned y = node.y + (index >> 1) * childSize;
            if (type == 1)
                walk.push_back({child, x, y, childSize});
            else
            {
                const float value = std::bit_cast<float>(Read(tile, child));
                for (unsigned row = y; row < y + childSize; ++row)
                    std::fill_n(decoded.begin() + std::size_t(row) * TileResolution + x, childSize, value);
            }
            child += 4;
        }
    }
    for (std::size_t index = 0; index < texels; ++index)
        if (upper[index] > 0.0f ? decoded[index] > upper[index] ||
                                      (decoded[index] <= lower[index] &&
                                       decoded[index] != upper[index])
                                : decoded[index] > floorDepth)
            throw std::runtime_error("compressed sun-shadow interval tile failed verification");
    return tile;
}

std::vector<std::uint8_t> EncodeForest(
    const std::uint32_t resolution, const std::uint16_t width, const std::uint16_t height,
    const float origin, const float inverseSpan,
    const std::span<const std::vector<std::uint8_t>> tiles,
    const std::span<const float> uniform)
{
    CheckDepthRange(origin, inverseSpan);
    if (resolution < TileResolution || !std::has_single_bit(resolution) ||
        !width || !height || std::uint32_t(width) > resolution / TileResolution ||
        std::uint32_t(height) > resolution / TileResolution ||
        tiles.size() != std::size_t(width) * height || tiles.size() > (MaximumBytes - 32) / 4 ||
        (!uniform.empty() && uniform.size() != tiles.size()))
        throw std::runtime_error("invalid compressed sun-shadow forest dimensions");
    // A uniform cell stores its positive depth itself (high bit clear); a
    // tile cell stores 0x80000000 | offset.
    for (std::size_t cell = 0; cell < uniform.size(); ++cell)
        if (uniform[cell] != 0.0f &&
            (!tiles[cell].empty() || !std::isfinite(uniform[cell]) || uniform[cell] < 0.0f ||
             uniform[cell] > 1.0f))
            throw std::runtime_error("invalid compressed sun-shadow uniform cell");

    std::size_t size = 32 + tiles.size() * 4;
    for (const auto &tile : tiles)
    {
        if (tile.empty())
            continue;
        if (tile.size() < 80 || (tile.size() & 3) || Read(tile, 0) != tile.size() ||
            Read(tile, 12) != tile.size() || Read(tile, 4) != TileResolution ||
            Read(tile, 8) != std::bit_cast<std::uint32_t>(1.0f / TileResolution) ||
            Read(tile, 16) != 0x00020000)
            throw std::runtime_error("invalid compressed sun-shadow mini-tile header");
        CheckDepthRange(std::bit_cast<float>(Read(tile, 24)),
                        std::bit_cast<float>(Read(tile, 28)));
        if (tile.size() > MaximumBytes - size)
            throw std::runtime_error("compressed sun-shadow forest exceeds 31-bit pointers");
        size += tile.size();
    }

    std::vector<std::uint8_t> forest(32 + tiles.size() * 4);
    forest.reserve(size);
    Store(forest, 0, static_cast<std::uint32_t>(size));
    Store(forest, 4, resolution);
    Store(forest, 8, std::bit_cast<std::uint32_t>(1.0f / resolution));
    Store(forest, 12, static_cast<std::uint32_t>(size));
    Store(forest, 16, 0x00030009);
    Store(forest, 20, std::uint32_t(width) | (std::uint32_t(height) << 16));
    Store(forest, 24, std::bit_cast<std::uint32_t>(origin));
    Store(forest, 28, std::bit_cast<std::uint32_t>(inverseSpan));
    for (std::size_t cell = 0; cell < tiles.size(); ++cell)
        if (!tiles[cell].empty())
        {
            Store(forest, 32 + 4 * cell, 0x80000000u | static_cast<std::uint32_t>(forest.size()));
            forest.insert(forest.end(), tiles[cell].begin(), tiles[cell].end());
        }
        else if (!uniform.empty() && uniform[cell] != 0.0f)
            Store(forest, 32 + 4 * cell, std::bit_cast<std::uint32_t>(uniform[cell]));
    return forest;
}

namespace
{
using Vec3 = std::array<float, 3>;

// Sun-space grid shared by the compressed shadow and the depth image: x =
// right.p, y = -up.p and depth (sun.p + origin) * inverseSpan; tile (x, y)
// covers texels [x * 512, x * 512 + 512) from (startX, startY).
struct TileGrid
{
    Vec3 right{}, up{};
    float startX{}, startY{}, sampleSize{};
    std::uint32_t width{}, height{};
    float origin{}, inverseSpan{};
};

using TileVisitor = std::function<void(unsigned x, unsigned y, const std::vector<float> &first,
                                       const std::vector<float> &behind)>;

void SunAxes(const Vec3 &sunDirection, Vec3 &right, Vec3 &up)
{
    const auto dot = [](const Vec3 &a, const Vec3 &b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto cross = [](const Vec3 &a, const Vec3 &b) -> Vec3 {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0]};
    };
    right = cross(std::abs(sunDirection[2]) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 0, 1}, sunDirection);
    const float rightLength = std::sqrt(dot(right, right));
    for (float &component : right)
        component /= rightLength;
    up = cross(sunDirection, right);
}

// Rasterizes the casters into 512x512 sun-space tiles: the first surface
// along the sun and, by depth peeling, the nearest surface behind it. Tiles
// without a caster are skipped. Two-sided rasterization treats every caster
// as an occluder whatever its facing (a ceiling seen from above).
bool RenderTiles(const Scene &scene, const std::array<float, 3> &sunDirection,
                 const replaybounds::Bounds &receiverBounds, const float sampleSize,
                 const replaybounds::Bounds *focus, const bool twoSided,
                 const std::function<void(const TileGrid &)> &begin, const TileVisitor &visit)
{
    using Microsoft::WRL::ComPtr;
    const auto dot = [](const Vec3 &a, const Vec3 &b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto check = [](const HRESULT result, const char *operation) {
        if (FAILED(result))
            throw std::runtime_error(std::string("sun-shadow ") + operation +
                                     " failed: " + std::to_string(static_cast<unsigned>(result)));
    };
    const float sunLength = dot(sunDirection, sunDirection);
    if (!std::isfinite(sunLength) || std::abs(sunLength - 1.0f) > 0.001f)
        throw std::runtime_error("compressed sun-shadow direction must be a unit vector");
    if (scene.surfaces.empty())
        return false;

    // Replay 0xEB9090 -> 0x198E4B0. Keep the stored direction unchanged:
    // 0xEB8230 compares it with the active sun with a 0.001 component tolerance.
    Vec3 right, up;
    SunAxes(sunDirection, right, up);
    std::vector<Surface> projected = scene.surfaces;
    // Distant background geometry (IW3 terrain past 100000 units) casts no
    // shadow on the play space and would stretch the shadow crop; drop it.
    for (auto &surface : projected)
    {
        if (surface.vertices.size() % 3)
            continue;
        std::vector<decltype(surface.vertices)::value_type> kept;
        kept.reserve(surface.vertices.size());
        for (std::size_t first = 0; first < surface.vertices.size(); first += 3)
        {
            bool inRange = true;
            for (std::size_t corner = 0; corner < 3; ++corner)
                for (const float value : surface.vertices[first + corner].position)
                    inRange = inRange && std::isfinite(value) && std::abs(value) <= 100000.0f;
            if (inRange)
                kept.insert(kept.end(), surface.vertices.begin() + first,
                            surface.vertices.begin() + first + 3);
        }
        surface.vertices = std::move(kept);
    }
    std::erase_if(projected, [](const Surface &surface) { return surface.vertices.empty(); });
    if (projected.empty())
        return false;
    // The focus box's footprint in sun space (x = right, y = -up).
    float focusMinX = -INFINITY, focusMinY = -INFINITY, focusMaxX = INFINITY, focusMaxY = INFINITY;
    if (focus)
    {
        focusMinX = focusMinY = INFINITY;
        focusMaxX = focusMaxY = -INFINITY;
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            Vec3 point{};
            for (unsigned axis = 0; axis < 3; ++axis)
                point[axis] = focus->midpoint[axis] +
                              ((corner >> axis) & 1 ? focus->halfSize[axis] : -focus->halfSize[axis]);
            const float x = dot(right, point), y = -dot(up, point);
            focusMinX = std::min(focusMinX, x);
            focusMaxX = std::max(focusMaxX, x);
            focusMinY = std::min(focusMinY, y);
            focusMaxY = std::max(focusMaxY, y);
        }
    }
    Vec3 minimum{INFINITY, INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY, -INFINITY};
    for (auto &surface : projected)
    {
        if (surface.material >= scene.materials.size() || surface.vertices.empty() ||
            surface.vertices.size() % 3)
            throw std::runtime_error("invalid compressed sun-shadow caster surface");
        for (auto &vertex : surface.vertices)
        {
            for (const float value : vertex.position)
                if (!std::isfinite(value) || std::abs(value) > 100000.0f)
                    throw std::runtime_error("invalid compressed sun-shadow caster position");
            if (!std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1]) ||
                !std::isfinite(vertex.alpha) || vertex.alpha < 0 || vertex.alpha > 1)
                throw std::runtime_error("invalid compressed sun-shadow caster attributes");
            vertex.position = {dot(right, vertex.position), -dot(up, vertex.position),
                                dot(sunDirection, vertex.position)};
        }
        if (focus)
        {
            // Keep the triangles that can shadow the focus box.
            std::vector<decltype(surface.vertices)::value_type> kept;
            for (std::size_t first = 0; first < surface.vertices.size(); first += 3)
            {
                float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
                for (std::size_t corner = 0; corner < 3; ++corner)
                {
                    const auto &position = surface.vertices[first + corner].position;
                    minX = std::min(minX, position[0]);
                    maxX = std::max(maxX, position[0]);
                    minY = std::min(minY, position[1]);
                    maxY = std::max(maxY, position[1]);
                }
                if (maxX >= focusMinX && minX <= focusMaxX && maxY >= focusMinY && minY <= focusMaxY)
                    kept.insert(kept.end(), surface.vertices.begin() + first,
                                surface.vertices.begin() + first + 3);
            }
            surface.vertices = std::move(kept);
        }
        for (const auto &vertex : surface.vertices)
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
                maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
            }
    }
    std::erase_if(projected, [](const Surface &surface) { return surface.vertices.empty(); });
    if (projected.empty())
        return false;
    // The shadow covers at most the focus footprint.
    minimum[0] = std::max(minimum[0], focusMinX);
    minimum[1] = std::max(minimum[1], focusMinY);
    maximum[0] = std::min(maximum[0], focusMaxX);
    maximum[1] = std::min(maximum[1], focusMaxY);
    const float tileWorldSize = TileResolution * sampleSize;
    const float startX = std::floor(minimum[0] / tileWorldSize) * tileWorldSize;
    const float startY = std::floor(minimum[1] / tileWorldSize) * tileWorldSize;
    const auto width = static_cast<std::uint32_t>(std::max(1.0f,
        std::ceil((maximum[0] - startX) / tileWorldSize)));
    const auto height = static_cast<std::uint32_t>(std::max(1.0f,
        std::ceil((maximum[1] - startY) / tileWorldSize)));
    if (width > UINT16_MAX || height > UINT16_MAX)
        throw std::runtime_error("compressed sun-shadow crop exceeds native dimensions");
    // The shader's outer near-plane guard makes receivers below the depth
    // range unconditionally lit. Include the physical world's receiver
    // bounds, not just eligible casters: a non-casting floor can be below
    // every caster and must still receive their shadows.
    float receiverRadius = 0;
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        if (!std::isfinite(receiverBounds.midpoint[axis]) ||
            !std::isfinite(receiverBounds.halfSize[axis]) ||
            receiverBounds.halfSize[axis] < 0 ||
            std::abs(receiverBounds.midpoint[axis]) + receiverBounds.halfSize[axis] > 262145)
            throw std::runtime_error("invalid compressed sun-shadow receiver bounds");
        receiverRadius += std::abs(sunDirection[axis]) * receiverBounds.halfSize[axis];
    }
    const float receiverCenter = dot(sunDirection, receiverBounds.midpoint);
    minimum[2] = std::min(minimum[2], receiverCenter - receiverRadius);
    maximum[2] = std::max(maximum[2], receiverCenter + receiverRadius);
    // Padding keeps occupied samples away from the zero/no-caster sentinel.
    // This changes the normalization range, not the world-space caster depth:
    // receivers and occluders use the same origin and inverse span.
    const float origin = 1.0f - minimum[2];
    const float inverseSpan = 1.0f / (maximum[2] - minimum[2] + 2.0f);
    begin({right, up, startX, startY, sampleSize, width, height, origin, inverseSpan});
    std::vector<std::vector<std::size_t>> candidates(std::size_t(width) * height);
    for (std::size_t index = 0; index < projected.size(); ++index)
    {
        auto &surface = projected[index];
        float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
        for (auto &vertex : surface.vertices)
        {
            vertex.position[0] = (vertex.position[0] - startX) / sampleSize;
            vertex.position[1] = (vertex.position[1] - startY) / sampleSize;
            vertex.position[2] = (vertex.position[2] + origin) * inverseSpan;
            minX = std::min(minX, vertex.position[0]);
            minY = std::min(minY, vertex.position[1]);
            maxX = std::max(maxX, vertex.position[0]);
            maxY = std::max(maxY, vertex.position[1]);
        }
        // A focus crop can leave triangles starting before the grid.
        const auto left = std::min(width - 1, static_cast<unsigned>(std::max(0.0f, minX) / TileResolution));
        const auto top = std::min(height - 1, static_cast<unsigned>(std::max(0.0f, minY) / TileResolution));
        const auto lastX = std::min(width - 1, static_cast<unsigned>(maxX / TileResolution));
        const auto lastY = std::min(height - 1, static_cast<unsigned>(maxY / TileResolution));
        for (unsigned y = top; y <= lastY; ++y)
            for (unsigned x = left; x <= lastX; ++x)
                candidates[y * width + x].push_back(index);
    }

    constexpr char shaderSource[] = R"(
cbuffer Material : register(b0) { uint alphaTest; uint atlasTile; float2 imageSize; };
Texture2D<float> alphaImage : register(t0);
SamplerState alphaSampler : register(s0);
struct Input { float3 position : POSITION; float2 uv : TEXCOORD; float alpha : COLOR; };
struct Output { float4 position : SV_POSITION; float2 uv : TEXCOORD; float alpha : COLOR; };
Output vs(Input input) {
    Output output;
    output.position = float4(input.position.x / 256.0 - 1.0,
                             1.0 - input.position.y / 256.0, input.position.z, 1.0);
    output.uv = input.uv;
    output.alpha = input.alpha;
    return output;
}
void ps(Output input) {
    float alpha;
    if (atlasTile) {
        float footprint = max(length(ddx(input.uv) * imageSize),
                              length(ddy(input.uv) * imageSize));
        float lod = clamp(log2(max(1.0, footprint)), 0, log2(imageSize.x) - 2);
        float border = 0.5 * exp2(ceil(lod));
        float2 uv = clamp(0.5 + frac(input.uv) * (imageSize - 1),
                          border, imageSize - border) / imageSize;
        alpha = alphaImage.SampleLevel(alphaSampler, uv, lod);
    } else {
        alpha = alphaImage.Sample(alphaSampler, input.uv);
    }
    alpha *= input.alpha;
    if (alphaTest == 2) { if (alpha <= 0) discard; }
    else if (alphaTest == 3) { if (alpha >= 0.5) discard; }
    else clip(alpha - 0.5);
}
// Second pass: the nearest surface behind the first one (depth peeling).
Texture2D<float> firstDepth : register(t1);
cbuffer Peel : register(b1) { float peelEpsilon; float3 peelPadding; };
void peel(Output input) {
    if (alphaTest != 0) ps(input);
    if (input.position.z >= firstDepth.Load(int3(input.position.xy, 0)) - peelEpsilon) discard;
})";
    const auto compile = [&](const char *entry, const char *target) {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT status = D3DCompile(shaderSource, sizeof(shaderSource) - 1, nullptr,
            nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(status))
            throw std::runtime_error(errors
                ? std::string(static_cast<const char *>(errors->GetBufferPointer()),
                              errors->GetBufferSize()) : "sun-shadow shader compilation failed");
        return code;
    };
    const auto vertexCode = compile("vs", "vs_5_0"), pixelCode = compile("ps", "ps_5_0");
    const auto peelCode = compile("peel", "ps_5_0");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                            D3D11_SDK_VERSION, &device, nullptr, &context), "create WARP device");
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    check(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
                                     nullptr, &vertexShader), "create vertex shader");
    check(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
                                    nullptr, &pixelShader), "create pixel shader");
    ComPtr<ID3D11PixelShader> peelShader;
    check(device->CreatePixelShader(peelCode->GetBufferPointer(), peelCode->GetBufferSize(),
                                    nullptr, &peelShader), "create depth-peel shader");
    const std::array<D3D11_INPUT_ELEMENT_DESC, 3> elements{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0}}};
    static_assert(sizeof(Vertex) == 24);
    ComPtr<ID3D11InputLayout> inputLayout;
    check(device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()),
                                    vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
                                    &inputLayout), "create caster input layout");
    context->IASetInputLayout(inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader.Get(), nullptr, 0);
    std::array<ComPtr<ID3D11RasterizerState>, 3> rasterizers;
    for (unsigned index = 0; index < rasterizers.size(); ++index)
    {
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = std::array{D3D11_CULL_NONE, D3D11_CULL_BACK, D3D11_CULL_FRONT}[index];
        raster.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&raster, &rasterizers[index]), "create rasterizer");
    }
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;
    check(device->CreateDepthStencilState(&depth, &depthState), "create depth state");
    context->OMSetDepthStencilState(depthState.Get(), 0);
    const D3D11_VIEWPORT viewport{0, 0, float(TileResolution), float(TileResolution), 0, 1};
    context->RSSetViewports(1, &viewport);
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = texture.Height = TileResolution;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R32_TYPELESS;
    texture.SampleDesc.Count = 1;
    texture.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> target, readback;
    check(device->CreateTexture2D(&texture, nullptr, &target), "create depth image");
    D3D11_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
    depthViewDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthViewDescription.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> depthView;
    check(device->CreateDepthStencilView(target.Get(), &depthViewDescription, &depthView),
          "create depth view");
    texture.Usage = D3D11_USAGE_STAGING;
    texture.BindFlags = 0;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&texture, nullptr, &readback), "create readback image");
    // The first-surface depth, read by the depth-peel pass.
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture.CPUAccessFlags = 0;
    ComPtr<ID3D11Texture2D> firstCopy;
    check(device->CreateTexture2D(&texture, nullptr, &firstCopy), "create first-surface image");
    D3D11_SHADER_RESOURCE_VIEW_DESC firstViewDescription{};
    firstViewDescription.Format = DXGI_FORMAT_R32_FLOAT;
    firstViewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    firstViewDescription.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> firstView;
    check(device->CreateShaderResourceView(firstCopy.Get(), &firstViewDescription, &firstView),
          "create first-surface view");
    // A surface within peelEpsilon of the first (one world unit along the sun
    // is inverseSpan) is the same layer.
    const float peelEpsilon = 0.25f * inverseSpan;
    ComPtr<ID3D11Buffer> peelConstants, opaqueConstants;
    {
        const std::array<float, 4> values{peelEpsilon, 0, 0, 0};
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(values);
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
        check(device->CreateBuffer(&buffer, &data, &peelConstants), "create depth-peel constants");
        const std::array<std::uint32_t, 4> opaque{0, 0, 0, 0};
        const D3D11_SUBRESOURCE_DATA opaqueData{opaque.data(), 0, 0};
        check(device->CreateBuffer(&buffer, &opaqueData, &opaqueConstants), "create opaque constants");
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> alphaSampler;
    check(device->CreateSamplerState(&sampler, &alphaSampler), "create alpha sampler");
    context->PSSetSamplers(0, 1, alphaSampler.GetAddressOf());
    std::vector<ComPtr<ID3D11Buffer>> constants(scene.materials.size());
    std::vector<ComPtr<ID3D11ShaderResourceView>> alphaViews(scene.materials.size());
    for (std::size_t index = 0; index < scene.materials.size(); ++index)
    {
        const auto &material = scene.materials[index];
        if (material.cullMode > 2 || material.alphaTest > 3)
            throw std::runtime_error("invalid compressed sun-shadow material state");
        if (!material.alphaTest)
            continue;
        if (!material.width || !material.height || material.width > 16384 ||
            material.height > 16384 || material.alpha.size() != std::size_t(material.width) * material.height ||
            (material.atlasTile && (material.width != material.height || material.width < 4)))
            throw std::runtime_error("invalid compressed sun-shadow alpha image");
        const std::array<std::uint32_t, 4> values{material.alphaTest, material.atlasTile ? 1u : 0u,
            std::bit_cast<std::uint32_t>(float(material.width)),
            std::bit_cast<std::uint32_t>(float(material.height))};
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(values);
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA constantData{values.data(), 0, 0};
        check(device->CreateBuffer(&buffer, &constantData, &constants[index]), "create alpha constants");
        std::vector<std::vector<std::uint8_t>> levels{material.alpha};
        std::vector<D3D11_SUBRESOURCE_DATA> subresources;
        unsigned mipWidth = material.width, mipHeight = material.height;
        for (;;)
        {
            subresources.push_back({levels.back().data(), mipWidth, 0});
            if ((mipWidth == 1 && mipHeight == 1) ||
                (material.atlasTile && mipWidth == 4 && mipHeight == 4))
                break;
            const unsigned nextWidth = std::max(1u, mipWidth / 2);
            const unsigned nextHeight = std::max(1u, mipHeight / 2);
            std::vector<std::uint8_t> next(std::size_t(nextWidth) * nextHeight);
            for (unsigned y = 0; y < nextHeight; ++y)
                for (unsigned x = 0; x < nextWidth; ++x)
                {
                    unsigned sum = 0;
                    for (unsigned dy = 0; dy < 2; ++dy)
                        for (unsigned dx = 0; dx < 2; ++dx)
                            sum += levels.back()[std::min(y * 2 + dy, mipHeight - 1) * mipWidth +
                                                  std::min(x * 2 + dx, mipWidth - 1)];
                    next[y * nextWidth + x] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            levels.push_back(std::move(next));
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }
        D3D11_TEXTURE2D_DESC alphaTexture{};
        alphaTexture.Width = material.width;
        alphaTexture.Height = material.height;
        alphaTexture.MipLevels = static_cast<UINT>(levels.size());
        alphaTexture.ArraySize = 1;
        alphaTexture.Format = DXGI_FORMAT_R8_UNORM;
        alphaTexture.SampleDesc.Count = 1;
        alphaTexture.Usage = D3D11_USAGE_IMMUTABLE;
        alphaTexture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> alphaImage;
        check(device->CreateTexture2D(&alphaTexture, subresources.data(), &alphaImage), "create alpha image");
        check(device->CreateShaderResourceView(alphaImage.Get(), nullptr, &alphaViews[index]),
              "create alpha image view");
    }

    std::vector<float> pixels(TileResolution * TileResolution);
    std::vector<float> behind(TileResolution * TileResolution);
    std::vector<Vertex> vertices;
    // Draws one tile's casters into the depth image: the first surface, or
    // with peel the nearest surface behind the first (firstCopy).
    const auto drawCasters = [&](const unsigned x, const unsigned y,
                                 const std::vector<std::size_t> &cellCasters, const bool peel) {
        context->OMSetRenderTargets(0, nullptr, depthView.Get());
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 0, 0);
        if (peel)
        {
            context->PSSetShaderResources(1, 1, firstView.GetAddressOf());
            context->PSSetConstantBuffers(1, 1, peelConstants.GetAddressOf());
        }
        for (const auto index : cellCasters)
        {
            const auto &surface = projected[index];
            const auto &material = scene.materials[surface.material];
            vertices.clear();
            for (std::size_t first = 0; first < surface.vertices.size(); first += 3)
            {
                std::array<Vertex, 3> triangle;
                float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
                for (unsigned corner = 0; corner < 3; ++corner)
                {
                    auto &vertex = triangle[corner];
                    vertex = surface.vertices[first + corner];
                    vertex.position[0] -= float(x * TileResolution);
                    vertex.position[1] -= float(y * TileResolution);
                    minX = std::min(minX, vertex.position[0]);
                    maxX = std::max(maxX, vertex.position[0]);
                    minY = std::min(minY, vertex.position[1]);
                    maxY = std::max(maxY, vertex.position[1]);
                }
                if (maxX >= 0 && maxY >= 0 && minX < TileResolution && minY < TileResolution)
                    vertices.insert(vertices.end(), triangle.begin(), triangle.end());
            }
            if (vertices.empty())
                continue;
            if (vertices.size() > UINT32_MAX / sizeof(Vertex))
                throw std::runtime_error("compressed sun-shadow vertex buffer exceeds native size");
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
            buffer.Usage = D3D11_USAGE_IMMUTABLE;
            buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            const D3D11_SUBRESOURCE_DATA vertexData{vertices.data(), 0, 0};
            ComPtr<ID3D11Buffer> vertexBuffer;
            check(device->CreateBuffer(&buffer, &vertexData, &vertexBuffer), "create caster vertices");
            const UINT stride = sizeof(Vertex), offset = 0;
            context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
            context->RSSetState(rasterizers[twoSided ? 0u : material.cullMode].Get());
            context->PSSetShader(peel ? peelShader.Get() : material.alphaTest ? pixelShader.Get() : nullptr,
                                 nullptr, 0);
            context->PSSetConstantBuffers(0, 1, material.alphaTest
                                                    ? constants[surface.material].GetAddressOf()
                                                    : opaqueConstants.GetAddressOf());
            context->PSSetShaderResources(0, 1, alphaViews[surface.material].GetAddressOf());
            context->Draw(static_cast<UINT>(vertices.size()), 0);
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        if (peel)
        {
            ID3D11ShaderResourceView *none = nullptr;
            context->PSSetShaderResources(1, 1, &none);
        }
    };
    const auto readDepth = [&](std::vector<float> &out) {
        context->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "read caster depth");
        for (unsigned row = 0; row < TileResolution; ++row)
            std::memcpy(out.data() + row * TileResolution,
                        static_cast<const std::uint8_t *>(mapped.pData) + row * mapped.RowPitch,
                        TileResolution * sizeof(float));
        context->Unmap(readback.Get(), 0);
    };
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            const auto cell = y * width + x;
            if (candidates[cell].empty())
                continue;
            drawCasters(x, y, candidates[cell], false);
            readDepth(pixels);
            if (std::none_of(pixels.begin(), pixels.end(), [](float value) { return value != 0; }))
                continue;
            context->CopyResource(firstCopy.Get(), target.Get());
            drawCasters(x, y, candidates[cell], true);
            readDepth(behind);
            visit(x, y, pixels, behind);
        }
    return true;
}

// Stored-depth interval of a texel with a caster (first > 0): the first
// surface stays lit, the surface behind stays shadowed and receivers more
// than tolerance below the first surface (players under cover) are shadowed.
// A caster with no surface behind it (a hull over non-casting water) keeps
// the same tolerance; an unbounded lower let merged leaves drop its occluder
// onto the receivers below.
float LowerBound(const float first, const float behind, const float inverseSpan)
{
    const float margin = 2.0f * inverseSpan;
    const float tolerance = 32.0f * inverseSpan;
    return behind > 0.0f ? std::max(behind + margin, first - tolerance) : first - tolerance;
}
} // namespace

Data Bake(const Scene &scene, const std::array<float, 3> &sunDirection,
          const replaybounds::Bounds &receiverBounds, const float sampleSize,
          const replaybounds::Bounds *focus)
{
    if (!(sampleSize == 1.0f || sampleSize == 2.0f))
        throw std::runtime_error("compressed sun-shadow sample size must be 1 or 2 (shipped values)");
    Data result;
    TileGrid grid;
    std::vector<std::vector<std::uint8_t>> tiles;
    std::vector<float> uniformCells;
    std::vector<float> lowerBounds(TileResolution * TileResolution);
    std::size_t encodedSize = 0;
    // Two-sided: IW3 light baking blocks the sun with every opaque surface.
    // With material culling a ceiling (back-facing the sun) let sunlight into
    // rooms whose roof top IW3 never drew.
    const bool baked = RenderTiles(
        scene, sunDirection, receiverBounds, sampleSize, focus, true,
        [&](const TileGrid &value) {
            grid = value;
            auto &parameters = result.parameters;
            parameters.sunDirection = sunDirection;
            parameters.resolution = std::bit_ceil(std::max(grid.width, grid.height)) * TileResolution;
            parameters.sampleSize = sampleSize;
            parameters.centerX = parameters.resolution * sampleSize * 0.5f + grid.startX;
            parameters.centerY = -grid.startY - parameters.resolution * sampleSize * 0.5f;
            parameters.forestSize = grid.width * grid.height;
            parameters.reserved = grid.width << 16;
            tiles.assign(std::size_t(grid.width) * grid.height, {});
            uniformCells.assign(tiles.size(), 0.0f);
            encodedSize = 32 + tiles.size() * 4;
        },
        [&](const unsigned x, const unsigned y, const std::vector<float> &pixels,
            const std::vector<float> &behind) {
            for (std::size_t index = 0; index < pixels.size(); ++index)
                lowerBounds[index] = LowerBound(pixels[index], behind[index], grid.inverseSpan);
            const auto cell = y * grid.width + x;
            float uniform = 0.0f;
            tiles[cell] =
                EncodeIntervalTile(pixels, lowerBounds, grid.origin, grid.inverseSpan, uniform);
            uniformCells[cell] = uniform;
            if (uniform != 0.0f)
                ++result.uniformCells;
            else
                ++result.treeCells;
            if (tiles[cell].size() > MaximumBytes - encodedSize)
                throw std::runtime_error("compressed sun-shadow bake exceeds 31-bit pointers");
            encodedSize += tiles[cell].size();
        });
    if (!baked)
        return {};
    result.bytes = EncodeForest(result.parameters.resolution,
                                static_cast<std::uint16_t>(grid.width),
                                static_cast<std::uint16_t>(grid.height), grid.origin,
                                grid.inverseSpan, tiles, uniformCells);
    return result;
}

DepthImage BakeDepthImage(const Scene &scene, const std::array<float, 3> &sunDirection,
                          const replaybounds::Bounds &region)
{
    // Texel size: the smallest (in eighths of a unit) whose tile grid, with
    // one tile of slack per axis for the alignment to whole tiles, fits the
    // image.
    Vec3 right, up;
    SunAxes(sunDirection, right, up);
    float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        Vec3 point{};
        for (unsigned axis = 0; axis < 3; ++axis)
            point[axis] = region.midpoint[axis] +
                          ((corner >> axis) & 1 ? region.halfSize[axis] : -region.halfSize[axis]);
        const float x = right[0] * point[0] + right[1] * point[1] + right[2] * point[2];
        const float y = -(up[0] * point[0] + up[1] * point[1] + up[2] * point[2]);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    float texel = 1.0f;
    const auto tilesFor = [&](const float value) {
        // The grid starts at a whole tile below the footprint (RenderTiles).
        const float tile = TileResolution * value;
        return std::max(1.0f, std::ceil((maxX - std::floor(minX / tile) * tile) / tile)) *
               std::max(1.0f, std::ceil((maxY - std::floor(minY / tile) * tile) / tile));
    };
    while (tilesFor(texel) > DepthImageTiles)
        texel += 0.125f;

    DepthImage image;
    image.width = DepthImageWidth;
    image.height = DepthImageHeight;
    std::vector<std::uint16_t> depth;
    std::uint32_t stride = 0;
    TileGrid grid;
    const bool baked = RenderTiles(
        scene, sunDirection, region, texel, &region, true,
        [&](const TileGrid &value) {
            grid = value;
            if (grid.width * grid.height > DepthImageTiles)
                throw std::runtime_error("sun depth image grid exceeds the image");
            stride = grid.width * TileResolution;
            depth.assign(std::size_t(stride) * grid.height * TileResolution, 0);
        },
        [&](const unsigned x, const unsigned y, const std::vector<float> &pixels,
            const std::vector<float> &behind) {
            for (unsigned row = 0; row < TileResolution; ++row)
                for (unsigned column = 0; column < TileResolution; ++column)
                {
                    const std::size_t index = std::size_t(row) * TileResolution + column;
                    const float first = pixels[index];
                    if (first <= 0.0f)
                        continue;
                    // The interval's midpoint, at most the first surface.
                    const float lower =
                        std::min(LowerBound(first, behind[index], grid.inverseSpan), first);
                    const float stored = (first + lower) * 0.5f;
                    depth[std::size_t(y * TileResolution + row) * stride + x * TileResolution +
                          column] = static_cast<std::uint16_t>(
                        std::clamp(std::lround(stored * 65535.0f), 1l, 65535l));
                }
        });
    // Row 0: header floats; from row 1 the depths in row-major order, two
    // 16-bit depths per RGBA8 texel (high byte first), 0 where nothing casts.
    image.rgba.assign(std::size_t(image.width) * image.height * 4, 0);
    const std::array<float, 18> header{right[0], right[1], right[2], up[0], up[1], up[2],
                                       sunDirection[0], sunDirection[1], sunDirection[2],
                                       grid.startX, grid.startY, texel, grid.origin,
                                       grid.inverseSpan, float(grid.width * TileResolution),
                                       float(grid.height * TileResolution), baked ? 1.0f : 0.0f,
                                       float(stride)};
    for (std::size_t index = 0; index < header.size(); ++index)
    {
        const auto bits = std::bit_cast<std::uint32_t>(header[index]);
        for (unsigned byte = 0; byte < 4; ++byte)
            image.rgba[index * 4 + byte] = static_cast<std::uint8_t>(bits >> (8 * byte));
    }
    for (std::size_t linear = 0; linear < depth.size(); ++linear)
    {
        const std::uint16_t value = depth[linear];
        const std::size_t at = (std::size_t(image.width) + linear / 2) * 4 + (linear & 1) * 2;
        image.rgba[at] = static_cast<std::uint8_t>(value >> 8);
        image.rgba[at + 1] = static_cast<std::uint8_t>(value);
    }
    image.texelSize = texel;
    image.baked = baked;
    return image;
}
} // namespace replaysunshadow
