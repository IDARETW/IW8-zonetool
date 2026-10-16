#include "replay_heightfield.h"

#include "iw8_structs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace iw8::heightfield
{
namespace
{
template <class T> void Store(uint8_t *record, const size_t offset, const T value)
{
    std::memcpy(record + offset, &value, sizeof(value));
}

// Replay GfxWorld offsets (Load_GfxWorld 0xD92E80; ACTS Replay::GfxWorld).
constexpr size_t kHeightfieldCount = 0x3D80;
constexpr size_t kHeightfields = 0x3D88;
constexpr size_t kOutdoorLookupMatrix = 0x3E98;
constexpr size_t kOutdoorImage = 0x3ED8;
// GfxHeightfield: bounds +0x00, lookupMatrix +0x18, image +0x58 (96 bytes).
constexpr size_t kHeightfieldSize = 0x60;

// Shipped Replay header fields (mp_hideout "*heightmap0" and "$outdoor"):
// flags 3, semantic 1, category 1, one level; byte 0x31 is 8 on the heightmap
// (as on the grading CLUT) and 0 on the outdoor image.
constexpr uint32_t kFormatR8 = 1;
constexpr uint32_t kFormatL8 = 3;
constexpr unsigned kOutdoorSize = 16;
constexpr uint8_t kOutdoorValue = 125;

// Texel density: about 8 units, as a power-of-two square between 256 and 2048
// (stock mp_hideout: 4096 texels over ~90k units).
unsigned Resolution(const replaybounds::Bounds &bounds)
{
    const double extent = 2.0 * std::max(bounds.halfSize[0], bounds.halfSize[1]);
    unsigned size = 256;
    while (size < 2048 && size * 8.0 < extent)
        size *= 2;
    return size;
}

void RegisterImage(ZoneWriter &writer, const std::string &name, const uint32_t format,
                   const uint8_t byte31, const unsigned size, const std::vector<uint8_t> &pixels)
{
    writer.add(ASSET_TYPE_IMAGE, name, [name, format, byte31, size, pixels](ZoneWriter &out) {
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(15);
        std::array<uint8_t, iw8sz::IMAGE> image{};
        Store(image.data(), 0x00, PTR_FOLLOWS);
        Store(image.data(), 0x14, format);
        Store(image.data(), 0x18, uint32_t{3});
        Store(image.data(), 0x1C, static_cast<uint32_t>(pixels.size()));
        Store(image.data(), 0x24, static_cast<uint16_t>(size));
        Store(image.data(), 0x26, static_cast<uint16_t>(size));
        Store(image.data(), 0x28, uint16_t{1});
        Store(image.data(), 0x2A, uint16_t{1});
        image[0x2E] = 1; // semantic
        image[0x2F] = 1; // category
        image[0x30] = 1; // levelCount
        image[0x31] = byte31;
        Store(image.data(), 0xE0, PTR_FOLLOWS);
        out.write(image.data(), image.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(name.c_str());
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(15);
        out.write(pixels.data(), pixels.size());
        out.popStream();
        out.popStream();
        out.popStream();
        out.pushStream(XFILE_BLOCK_TEMP_POSTLOAD);
        out.align(15);
        out.reserveCalc(iw8sz::IMAGE);
        out.popStream();
    });
}
} // namespace

Data Build(const std::vector<float> &triangles, const replaybounds::Bounds &bounds)
{
    if (triangles.empty() || triangles.size() % 9)
        throw std::runtime_error("Replay heightfield requires world triangles");
    Data data;
    data.bounds = bounds;
    for (auto &half : data.bounds.halfSize)
        half = std::max(half, 0.5f);
    const auto &mid = data.bounds.midpoint;
    const auto &half = data.bounds.halfSize;
    const double minX = double(mid[0]) - half[0], maxY = double(mid[1]) + half[1],
                 minY = double(mid[1]) - half[1], minZ = double(mid[2]) - half[2];
    const double sizeX = 2.0 * half[0], sizeY = 2.0 * half[1], sizeZ = 2.0 * half[2];
    const unsigned n = data.size = Resolution(data.bounds);

    // Heightmap texel (column, row) covers u, v in [c/n, (c+1)/n), with
    // u = (x - minX) / sizeX and v = (maxY - y) / sizeY.
    std::vector<float> height(size_t(n) * n, -std::numeric_limits<float>::infinity());
    const auto toU = [&](const double x) { return (x - minX) / sizeX * n; };
    const auto toV = [&](const double y) { return (maxY - y) / sizeY * n; };
    const auto raise = [&](const double u, const double v, const double z) {
        const int c = int(std::floor(u)), r = int(std::floor(v));
        if (c < 0 || r < 0 || c >= int(n) || r >= int(n))
            return;
        auto &cell = height[size_t(r) * n + c];
        cell = std::max(cell, float(z));
    };
    for (size_t t = 0; t < triangles.size(); t += 9)
    {
        double u[3], v[3], z[3];
        for (unsigned k = 0; k < 3; ++k)
        {
            u[k] = toU(triangles[t + 3 * k]);
            v[k] = toV(triangles[t + 3 * k + 1]);
            z[k] = triangles[t + 3 * k + 2];
        }
        // Edges: walls and other near-vertical triangles project to lines, so
        // every edge is sampled at half-texel steps at its own height.
        for (unsigned k = 0; k < 3; ++k)
        {
            const unsigned j = (k + 1) % 3;
            const double length = std::max(std::abs(u[j] - u[k]), std::abs(v[j] - v[k]));
            const unsigned steps = unsigned(std::ceil(length * 2)) + 1;
            for (unsigned s = 0; s <= steps; ++s)
            {
                const double f = double(s) / steps;
                raise(u[k] + (u[j] - u[k]) * f, v[k] + (v[j] - v[k]) * f,
                      z[k] + (z[j] - z[k]) * f);
            }
        }
        // Interior: the triangle's plane height at each covered texel centre.
        const double area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
        if (std::abs(area) < 1e-9)
            continue;
        const int c0 = std::max(0, int(std::floor(std::min({u[0], u[1], u[2]})))),
                  c1 = std::min(int(n) - 1, int(std::floor(std::max({u[0], u[1], u[2]})))),
                  r0 = std::max(0, int(std::floor(std::min({v[0], v[1], v[2]})))),
                  r1 = std::min(int(n) - 1, int(std::floor(std::max({v[0], v[1], v[2]}))));
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
            {
                const double pu = c + .5, pv = r + .5;
                const double w1 = ((pu - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (pv - v[0])) / area;
                const double w2 = ((u[1] - u[0]) * (pv - v[0]) - (pu - u[0]) * (v[1] - v[0])) / area;
                const double w0 = 1 - w1 - w2;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                raise(pu, pv, w0 * z[0] + w1 * z[1] + w2 * z[2]);
            }
    }
    // Uncovered texels hold the bottom of the bounds.
    data.pixels.resize(height.size());
    for (size_t i = 0; i < height.size(); ++i)
    {
        const double normal = std::isfinite(height[i]) ? (height[i] - minZ) / sizeZ : 0.0;
        data.pixels[i] = uint8_t(std::lround(std::clamp(normal, 0.0, 1.0) * 255.0));
    }

    // Row-major float4x4, row vector convention (row 3 is the translation):
    // u = x/sizeX - minX/sizeX; v = maxY/sizeY - y/sizeY (heightmap) or
    // y/sizeY - minY/sizeY (outdoor); z = z/sizeZ - minZ/sizeZ.
    auto &m = data.lookup;
    m[0] = float(1 / sizeX);
    m[5] = float(-1 / sizeY);
    m[10] = float(1 / sizeZ);
    m[12] = float(-minX / sizeX);
    m[13] = float(maxY / sizeY);
    m[14] = float(-minZ / sizeZ);
    m[15] = 1;
    data.outdoorLookup = m;
    data.outdoorLookup[5] = float(1 / sizeY);
    data.outdoorLookup[13] = float(-minY / sizeY);
    return data;
}

void Register(ZoneWriter &writer, const Data &data)
{
    RegisterImage(writer, kHeightmapName, kFormatR8, 8, data.size, data.pixels);
    RegisterImage(writer, kOutdoorName, kFormatL8, 0, kOutdoorSize,
                  std::vector<uint8_t>(kOutdoorSize * kOutdoorSize, kOutdoorValue));
}

void Stamp(std::vector<uint8_t> &world, const Data &data, const ZoneWriter &writer)
{
    Store(world.data(), kHeightfieldCount, uint32_t{1});
    Store(world.data(), kHeightfields, PTR_FOLLOWS);
    std::memcpy(world.data() + kOutdoorLookupMatrix, data.outdoorLookup.data(),
                sizeof(data.outdoorLookup));
    Store(world.data(), kOutdoorImage, writer.assetAlias(ASSET_TYPE_IMAGE, kOutdoorName));
}

void Emit(ZoneWriter &writer, const Data &data)
{
    writer.align(7);
    uint8_t record[kHeightfieldSize]{};
    data.bounds.Write(record);
    std::memcpy(record + 0x18, data.lookup.data(), sizeof(data.lookup));
    Store(record, 0x58, writer.assetAlias(ASSET_TYPE_IMAGE, kHeightmapName));
    writer.write(record, sizeof(record));
}
} // namespace iw8::heightfield
