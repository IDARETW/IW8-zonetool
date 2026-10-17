#include "iw3_lightgrid.h"

#include "common/fs_util.h"
#include "common/json.hpp"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace iw3
{
namespace
{
using Json = nlohmann::json;

constexpr std::string_view Magic = "IW8GLG01";
constexpr float SqrtThree = 1.7320508075688772f;
constexpr float SqrtFive = 2.2360679774997897f;

using Probe = std::array<std::array<float, 9>, 3>;

struct Entry
{
    std::uint16_t color{};
    std::uint8_t primaryLight{};
    std::uint8_t traceMask{};
};

struct Cell
{
    std::array<std::int32_t, 3> position{};
    Entry entry{};
};

template <typename T> T Read(const std::span<const std::uint8_t> data, const std::size_t offset)
{
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("truncated IW3 light-grid data");
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

template <typename T> void Append(std::vector<std::uint8_t> &output, const T &value)
{
    const auto *data = reinterpret_cast<const std::uint8_t *>(&value);
    output.insert(output.end(), data, data + sizeof(value));
}

void AppendBytes(std::vector<std::uint8_t> &output, const void *value, const std::size_t size)
{
    const auto *data = static_cast<const std::uint8_t *>(value);
    output.insert(output.end(), data, data + size);
}

std::uint16_t FloatToHalf(const float value)
{
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23) & 0xFFu;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent == 0xFFu)
        return static_cast<std::uint16_t>(sign | (mantissa ? 0x7E00u : 0x7C00u));

    const int adjusted = static_cast<int>(exponent) - 127 + 15;
    if (adjusted >= 31)
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    if (adjusted <= 0)
    {
        if (adjusted < -10)
            return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const unsigned shift = static_cast<unsigned>(14 - adjusted);
        const std::uint32_t rounded =
            (mantissa + ((1u << (shift - 1)) - 1u) + ((mantissa >> shift) & 1u)) >> shift;
        return static_cast<std::uint16_t>(sign | rounded);
    }

    mantissa += 0xFFFu + ((mantissa >> 13) & 1u);
    if (mantissa & 0x800000u)
    {
        mantissa = 0;
        if (adjusted + 1 >= 31)
            return static_cast<std::uint16_t>(sign | 0x7C00u);
        return static_cast<std::uint16_t>(sign | ((adjusted + 1) << 10));
    }
    return static_cast<std::uint16_t>(sign | (adjusted << 10) | (mantissa >> 13));
}

Json ReadJson(const std::filesystem::path &path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot read IW3 light-grid metadata " + path.string());
    Json value;
    input >> value;
    return value;
}

std::vector<std::uint8_t> ReadArray(const std::filesystem::path &root, const Json &metadata,
                                    const char *key, const std::size_t expected)
{
    const Json &description = metadata.at(key);
    const std::string relative = description.at("file").get<std::string>();
    const auto declared = description.at("bytes").get<std::uint64_t>();
    const std::filesystem::path relativePath(relative);
    if (relative.empty() || relativePath.is_absolute() || relative.find(':') != std::string::npos ||
        relative.find('\\') != std::string::npos ||
        std::ranges::find(relativePath, "..") != relativePath.end() || declared != expected ||
        declared > 64ull * 1024 * 1024)
        throw std::runtime_error(std::string("invalid IW3 light-grid ") + key + " descriptor");

    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error)
        throw std::runtime_error("cannot resolve IW3 light-grid root");
    const auto path = std::filesystem::weakly_canonical(root / relativePath, error);
    const auto &rootText = canonicalRoot.native();
    const auto &pathText = path.native();
    const bool rootPrefix = !error && pathText.size() >= rootText.size() &&
                            std::equal(rootText.begin(), rootText.end(), pathText.begin(),
                                       [](const wchar_t left, const wchar_t right) {
                                           return std::towlower(left) == std::towlower(right);
                                       });
    const bool componentBoundary =
        rootPrefix && (pathText.size() == rootText.size() ||
                       pathText[rootText.size()] == std::filesystem::path::preferred_separator);
    if (!componentBoundary)
        throw std::runtime_error(std::string("unsafe IW3 light-grid ") + key + " path");

    std::vector<std::uint8_t> data;
    if (!zt::read_file(path.string(), data) || data.size() != expected)
        throw std::runtime_error(std::string("incorrect IW3 light-grid ") + key + " length");
    return data;
}

std::vector<Cell> ReadCells(const std::filesystem::path &root, const Json &metadata,
                            std::vector<std::uint8_t> &palette)
{
    const int rowAxis = metadata.at("row_axis").get<int>();
    const int columnAxis = metadata.at("col_axis").get<int>();
    if (!((rowAxis == 0 && columnAxis == 1) || (rowAxis == 1 && columnAxis == 0)))
        throw std::runtime_error("unsupported IW3 light-grid axes");

    std::array<int, 3> minimum{};
    std::array<int, 3> maximum{};
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        minimum[axis] = metadata.at("mins").at(axis).get<int>();
        maximum[axis] = metadata.at("maxs").at(axis).get<int>();
        if (minimum[axis] < 0 || maximum[axis] < minimum[axis] || maximum[axis] > 65535)
            throw std::runtime_error("invalid IW3 light-grid bounds");
    }
    const std::size_t rows = static_cast<std::size_t>(maximum[rowAxis] - minimum[rowAxis] + 1);
    const std::size_t entryCount = metadata.at("entry_count").get<std::size_t>();
    const std::size_t colorCount = metadata.at("color_count").get<std::size_t>();
    if (metadata.at("row_count").get<std::size_t>() != rows || entryCount > 8'388'608 ||
        colorCount > 65'536)
        throw std::runtime_error("invalid IW3 light-grid counts");

    const auto starts = ReadArray(root, metadata, "row_starts", rows * 2);
    const auto rawBytes = metadata.at("row_data").at("bytes").get<std::size_t>();
    const auto raw = ReadArray(root, metadata, "row_data", rawBytes);
    const auto entriesRaw = ReadArray(root, metadata, "entries", entryCount * 4);
    palette = ReadArray(root, metadata, "colors", colorCount * 168);

    std::vector<Entry> entries(entryCount);
    for (std::size_t index = 0; index < entryCount; ++index)
    {
        entries[index].color = Read<std::uint16_t>(entriesRaw, index * 4);
        entries[index].primaryLight = entriesRaw[index * 4 + 2];
        entries[index].traceMask = entriesRaw[index * 4 + 3];
        if (entries[index].color >= colorCount)
            throw std::runtime_error("IW3 light-grid entry references a missing color");
    }

    std::vector<Cell> cells;
    cells.reserve(entryCount);
    std::size_t expectedEntry = 0;
    std::size_t previousEnd = 0;
    for (std::size_t row = 0; row < rows; ++row)
    {
        const std::uint16_t start = Read<std::uint16_t>(starts, row * 2);
        if (start == 0xFFFFu)
            continue;
        std::size_t cursor = static_cast<std::size_t>(start) * 4;
        if (cursor < previousEnd || cursor > raw.size() || 12 > raw.size() - cursor)
            throw std::runtime_error("overlapping or truncated IW3 light-grid row");
        const std::uint16_t firstColumn = Read<std::uint16_t>(raw, cursor);
        const std::uint16_t width = Read<std::uint16_t>(raw, cursor + 2);
        const std::uint16_t bottom = Read<std::uint16_t>(raw, cursor + 4);
        const std::uint16_t height = Read<std::uint16_t>(raw, cursor + 6);
        const std::uint32_t firstEntry = Read<std::uint32_t>(raw, cursor + 8);
        cursor += 12;
        if (firstEntry != expectedEntry || !width || !height ||
            firstColumn < minimum[columnAxis] || firstColumn + width - 1 > maximum[columnAxis] ||
            bottom < minimum[2] || bottom + height - 1 > maximum[2])
            throw std::runtime_error("invalid IW3 light-grid row range");

        std::size_t consumed = 0;
        while (consumed < width)
        {
            if (cursor > raw.size() || 2 > raw.size() - cursor)
                throw std::runtime_error("truncated IW3 light-grid column run");
            const std::uint8_t run = raw[cursor++];
            const std::uint8_t count = raw[cursor++];
            if (!run || consumed + run > width)
                throw std::runtime_error("invalid IW3 light-grid column run");
            std::uint8_t startZ = 0;
            if (count)
            {
                if (cursor == raw.size())
                    throw std::runtime_error("missing IW3 light-grid column height");
                startZ = raw[cursor++];
                if (startZ + count > height || expectedEntry + std::size_t(run) * count > entryCount)
                    throw std::runtime_error("IW3 light-grid column exceeds its row");
            }
            for (unsigned column = 0; column < run; ++column)
            {
                for (unsigned z = 0; z < count; ++z)
                {
                    std::array<int, 3> coordinate{0, 0, bottom + startZ + static_cast<int>(z)};
                    coordinate[rowAxis] = minimum[rowAxis] + static_cast<int>(row);
                    coordinate[columnAxis] = firstColumn + static_cast<int>(consumed + column);
                    Cell cell;
                    cell.position = {coordinate[0] * 32 - 131072,
                                     coordinate[1] * 32 - 131072,
                                     coordinate[2] * 64 - 131072};
                    cell.entry = entries[expectedEntry++];
                    cells.push_back(cell);
                }
            }
            consumed += run;
        }
        previousEnd = (cursor + 3) & ~std::size_t{3};
        if (previousEnd > raw.size() ||
            std::any_of(raw.begin() + cursor, raw.begin() + previousEnd,
                        [](const std::uint8_t value) { return value != 0; }))
            throw std::runtime_error("nonzero or truncated IW3 light-grid row padding");
    }
    if (expectedEntry != entryCount || previousEnd != raw.size())
        throw std::runtime_error("unconsumed IW3 light-grid data");
    return cells;
}

std::array<float, 9> EvaluateShBasis(const std::array<float, 3> &direction)
{
    const auto [x, y, z] = direction;
    return {0.2820947918f,
            -0.4886025119f * y,
            0.4886025119f * z,
            -0.4886025119f * x,
            1.0925484306f * x * y,
            -1.0925484306f * y * z,
            0.3153915653f * (3.0f * z * z - 1.0f),
            -1.0925484306f * x * z,
            0.5462742153f * (x * x - y * y)};
}

const std::array<std::array<double, 56>, 9> &LightGridProjection()
{
    static const auto projection = [] {
        constexpr std::array<std::array<float, 3>, 3> rotation{{
            {0.47140452f, 0.0f, 0.33333334f},
            {-0.23570226f, 0.40824828f, 0.33333334f},
            {-0.23570226f, -0.40824828f, 0.33333334f}}};
        std::array<std::array<double, 9>, 56> samples{};
        std::size_t sampleIndex = 0;
        for (int z = 0; z < 4; ++z)
        {
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 4; ++x)
                {
                    if (x > 0 && x < 3 && y > 0 && y < 3 && z > 0 && z < 3)
                        continue;
                    const std::array<float, 3> delta{
                        x * (2.0f / 3.0f) - 1.0f,
                        y * (2.0f / 3.0f) - 1.0f,
                        z * (2.0f / 3.0f) - 1.0f};
                    std::array<float, 3> direction{};
                    for (unsigned component = 0; component < 3; ++component)
                    {
                        for (unsigned axis = 0; axis < 3; ++axis)
                            direction[component] += delta[axis] * rotation[component][axis];
                    }
                    const float length = std::sqrt(direction[0] * direction[0] +
                                                   direction[1] * direction[1] +
                                                   direction[2] * direction[2]);
                    if (!(length > 0.0f) || sampleIndex >= samples.size())
                        throw std::runtime_error("invalid IW3 light-grid sample basis");
                    for (float &component : direction)
                        component /= length;
                    const auto basis = EvaluateShBasis(direction);
                    for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
                        samples[sampleIndex][coefficient] = basis[coefficient];
                    ++sampleIndex;
                }
            }
        }
        if (sampleIndex != samples.size())
            throw std::runtime_error("incorrect IW3 light-grid sample count");

        std::array<std::array<double, 18>, 9> inverse{};
        for (unsigned row = 0; row < 9; ++row)
        {
            for (unsigned column = 0; column < 9; ++column)
                for (const auto &sample : samples)
                    inverse[row][column] += sample[row] * sample[column];
            inverse[row][row + 9] = 1.0;
        }
        for (unsigned column = 0; column < 9; ++column)
        {
            unsigned pivot = column;
            for (unsigned row = column + 1; row < 9; ++row)
                if (std::abs(inverse[row][column]) > std::abs(inverse[pivot][column]))
                    pivot = row;
            if (std::abs(inverse[pivot][column]) < 1.0e-10)
                throw std::runtime_error("singular IW3 light-grid SH projection");
            if (pivot != column)
                std::swap(inverse[pivot], inverse[column]);
            const double divisor = inverse[column][column];
            for (double &value : inverse[column])
                value /= divisor;
            for (unsigned row = 0; row < 9; ++row)
            {
                if (row == column)
                    continue;
                const double factor = inverse[row][column];
                for (unsigned entry = 0; entry < 18; ++entry)
                    inverse[row][entry] -= factor * inverse[column][entry];
            }
        }

        std::array<std::array<double, 56>, 9> result{};
        for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
            for (unsigned sample = 0; sample < 56; ++sample)
                for (unsigned term = 0; term < 9; ++term)
                    result[coefficient][sample] +=
                        inverse[coefficient][term + 9] * samples[sample][term];

        for (unsigned expected = 0; expected < 9; ++expected)
        {
            for (unsigned actual = 0; actual < 9; ++actual)
            {
                double value = 0.0;
                for (unsigned sample = 0; sample < 56; ++sample)
                    value += result[actual][sample] * samples[sample][expected];
                if (std::abs(value - (actual == expected ? 1.0 : 0.0)) > 1.0e-5)
                    throw std::runtime_error("invalid IW3 light-grid SH projection");
            }
        }
        return result;
    }();
    return projection;
}

std::vector<Probe> BuildProbes(const std::vector<std::uint8_t> &palette, const float lightUnitScale = 1.0f)
{
    std::vector<Probe> probes(palette.size() / 168);
    const auto &projection = LightGridProjection();
    // IW3 grid samples are shading values in the same 8-bit units as its
    // lightmaps, which the world materials consume unscaled beside the source
    // sun. The former x32/pi gain put dynamic objects at ~10x the ambient of
    // the walls around them: Office's DC sat at 18-20 everywhere against a
    // 3.0 sun. Shipped Shipment keeps outdoor DC near 1.3x its sun intensity;
    // these units give Nuketown ~1.4x, and preserve the source contrast.
    constexpr float sourceScale = 1.0f / 255.0f;
    for (std::size_t probe = 0; probe < probes.size(); ++probe)
    {
        for (unsigned channel = 0; channel < 3; ++channel)
        {
            for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
            {
                double value = 0.0;
                for (unsigned sample = 0; sample < 56; ++sample)
                    value += projection[coefficient][sample] *
                             palette[probe * 168 + sample * 3 + channel];
                probes[probe][channel][coefficient] =
                    static_cast<float>(value * sourceScale * lightUnitScale);
            }
        }
    }
    return probes;
}

std::array<std::uint8_t, 32> PackProbe(const Probe &probe, const std::uint8_t traceMask)
{
    for (const auto &channel : probe)
        for (const float value : channel)
            if (!std::isfinite(value) || std::abs(value) > 60000.0f)
                throw std::runtime_error("invalid IW3 diffuse probe coefficient");
    if (probe[0][0] < 0.0f || probe[1][0] < 0.0f || probe[2][0] < 0.0f)
        throw std::runtime_error("negative IW3 diffuse probe intensity");
    const auto red = FloatToHalf(probe[0][0]);
    const auto green = FloatToHalf(probe[1][0]);
    const auto blue = FloatToHalf(probe[2][0]);
    const std::uint32_t dc = (red >> 4) | (std::uint32_t(green >> 4) << 11) |
                             (std::uint32_t(blue >> 5) << 22);
    std::array<std::uint8_t, 32> packed{};
    std::memcpy(packed.data(), &dc, sizeof(dc));
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        const float dcValue = probe[channel][0];
        for (unsigned coefficient = 1; coefficient < 9; ++coefficient)
        {
            float normalized = 0.0f;
            if (dcValue > 0.0f)
            {
                const float range = coefficient < 4 ? SqrtThree : SqrtFive;
                normalized = std::clamp(probe[channel][coefficient] / (dcValue * range),
                                        -1.0f, 1.0f);
            }
            packed[4 + channel * 8 + coefficient - 1] =
                static_cast<std::uint8_t>(normalized * 127.0f + 127.5f);
        }
    }
    // Replay keeps a per-probe 8-bit visibility/trace value in byte 28 of the
    // otherwise compressed SH record.  IW3 stores the corresponding mask on
    // each light-grid entry, so preserve it byte-for-byte; its bit ordering is
    // already authored and must not be inferred from the primary-light index.
    packed[28] = traceMask;
    packed[29] = 255;
    return packed;
}

float Median(std::vector<float> values)
{
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    if (values.size() % 2)
        return values[middle];
    const float high = values[middle];
    std::nth_element(values.begin(), values.begin() + middle - 1, values.begin() + middle);
    return (values[middle - 1] + high) * 0.5f;
}

// Triangulate only the convex hull of corners actually present in an IW3 cell.
// The least corner is the fan apex; a four-corner face uses its least corner
// and opposite diagonal, matching the complete-cell boundary triangulation.
std::vector<std::array<unsigned, 4>> SparseCellTetrahedra(const unsigned mask)
{
    constexpr std::array<std::array<int, 3>, 8> points{{
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}}};
    std::array<unsigned, 8> vertices{};
    unsigned count = 0;
    for (unsigned vertex = 0; vertex < 8; ++vertex)
        if (mask & (1u << vertex))
            vertices[count++] = vertex;
    if (count < 4)
        return {};

    const unsigned anchor = vertices[0];
    std::vector<unsigned> faces;
    for (unsigned i = 0; i < count; ++i)
    {
        for (unsigned j = i + 1; j < count; ++j)
        {
            for (unsigned k = j + 1; k < count; ++k)
            {
                const auto &a = points[vertices[i]];
                const auto &b = points[vertices[j]];
                const auto &c = points[vertices[k]];
                const std::array<int, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                const std::array<int, 3> v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const std::array<int, 3> normal{
                    u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                    u[0] * v[1] - u[1] * v[0]};
                if (normal == std::array<int, 3>{0, 0, 0})
                    continue;
                bool positive = false;
                bool negative = false;
                unsigned face = 0;
                for (unsigned n = 0; n < count; ++n)
                {
                    const unsigned vertex = vertices[n];
                    const auto &p = points[vertex];
                    const int side = normal[0] * (p[0] - a[0]) +
                                     normal[1] * (p[1] - a[1]) +
                                     normal[2] * (p[2] - a[2]);
                    positive |= side > 0;
                    negative |= side < 0;
                    if (!side)
                        face |= 1u << vertex;
                }
                if (positive == negative)
                    continue;
                if (!(face & (1u << anchor)) &&
                    std::ranges::find(faces, face) == faces.end())
                    faces.push_back(face);
            }
        }
    }

    std::ranges::sort(faces);
    std::vector<std::array<unsigned, 4>> tetrahedra;
    for (const unsigned face : faces)
    {
        std::array<unsigned, 4> corners{};
        unsigned faceCount = 0;
        for (unsigned vertex = 0; vertex < 8; ++vertex)
            if (face & (1u << vertex))
                corners[faceCount++] = vertex;
        if (faceCount == 3)
        {
            tetrahedra.push_back({anchor, corners[0], corners[1], corners[2]});
            continue;
        }
        if (faceCount != 4)
            throw std::runtime_error("unsupported IW3 light-grid hull face");

        unsigned opposite = 1;
        int longest = -1;
        for (unsigned index = 1; index < 4; ++index)
        {
            const auto delta = std::array<int, 3>{
                points[corners[index]][0] - points[corners[0]][0],
                points[corners[index]][1] - points[corners[0]][1],
                points[corners[index]][2] - points[corners[0]][2]};
            const int length = delta[0] * delta[0] + delta[1] * delta[1] +
                               delta[2] * delta[2];
            if (length > longest)
            {
                longest = length;
                opposite = index;
            }
        }
        std::array<unsigned, 2> others{};
        for (unsigned index = 1, next = 0; index < 4; ++index)
            if (index != opposite)
                others[next++] = corners[index];
        tetrahedra.push_back({anchor, corners[0], others[0], corners[opposite]});
        tetrahedra.push_back({anchor, corners[0], corners[opposite], others[1]});
    }
    return tetrahedra;
}

bool SparseTetrahedronContains(const std::array<std::int32_t, 3> &point,
                               const std::array<unsigned, 4> &tetrahedron)
{
    const auto corner = [](const unsigned mask) {
        return std::array<std::int64_t, 3>{(mask & 1) ? 64 : 0, (mask & 2) ? 64 : 0,
                                           (mask & 4) ? 64 : 0};
    };
    const auto difference = [](const auto &a, const auto &b) {
        return std::array<std::int64_t, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto determinant = [](const auto &a, const auto &b, const auto &c) {
        return a[0] * (b[1] * c[2] - b[2] * c[1]) +
               a[1] * (b[2] * c[0] - b[0] * c[2]) +
               a[2] * (b[0] * c[1] - b[1] * c[0]);
    };
    const auto origin = corner(tetrahedron[0]);
    const auto a = difference(corner(tetrahedron[1]), origin);
    const auto b = difference(corner(tetrahedron[2]), origin);
    const auto c = difference(corner(tetrahedron[3]), origin);
    const auto q = difference(std::array<std::int64_t, 3>{point[0], point[1], point[2]},
                              origin);
    const auto volume = determinant(a, b, c);
    if (!volume)
        throw std::runtime_error("degenerate IW3 light-grid tetrahedron");
    const auto sign = volume < 0 ? -1 : 1;
    const auto x = sign * determinant(q, b, c);
    const auto y = sign * determinant(a, q, c);
    const auto z = sign * determinant(a, b, q);
    return x >= 0 && y >= 0 && z >= 0 && x + y + z <= sign * volume;
}

std::vector<std::uint8_t> BuildPayload(std::vector<Cell> cells,
                                       const std::vector<std::uint8_t> &palette, const float lightUnitScale = 1.0f)
{
    if (cells.empty() || cells.size() > 2'000'000 || palette.empty() || palette.size() % 168)
        throw std::runtime_error("invalid IW3 light-grid probe or palette count");
    std::ranges::sort(cells, {}, &Cell::position);
    std::map<std::array<std::int32_t, 3>, std::uint32_t> lookup;
    std::array<std::int32_t, 3> low = cells.front().position;
    std::array<std::int32_t, 3> high = cells.front().position;
    for (std::uint32_t index = 0; index < cells.size(); ++index)
    {
        const auto &position = cells[index].position;
        if (std::abs(position[0]) > 100000 || std::abs(position[1]) > 100000 ||
            std::abs(position[2]) > 100000 || position[0] % 32 || position[1] % 32 ||
            position[2] % 64 || !lookup.emplace(position, index).second)
            throw std::runtime_error("invalid, duplicate, or off-lattice IW3 light-grid probe");
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            low[axis] = std::min(low[axis], position[axis]);
            high[axis] = std::max(high[axis], position[axis]);
        }
    }
    auto alignDown = [](const std::int32_t value, const std::int32_t alignment) {
        std::int32_t quotient = value / alignment;
        if (value < 0 && value % alignment)
            --quotient;
        return quotient * alignment;
    };
    auto alignUp = [&](const std::int32_t value, const std::int32_t alignment) {
        return -alignDown(-value, alignment);
    };

    std::array<std::int32_t, 3> gridLow{};
    std::array<std::int32_t, 3> gridHigh{};
    std::array<std::uint32_t, 3> dimensions{};
    std::uint64_t voxelCount = 0;
    std::uint64_t rootCount64 = 0;
    unsigned rootShift = 5;
    for (; rootShift <= 20; ++rootShift)
    {
        const std::int32_t voxelSize = 1 << rootShift;
        voxelCount = 1;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            gridLow[axis] = alignDown(low[axis], voxelSize);
            gridHigh[axis] = alignUp(high[axis], voxelSize);
            dimensions[axis] =
                static_cast<std::uint32_t>((gridHigh[axis] - gridLow[axis]) / voxelSize);
            if (!dimensions[axis])
                throw std::runtime_error("unsupported IW3 light-grid extent");
            voxelCount *= dimensions[axis];
        }
        rootCount64 = static_cast<std::uint64_t>(dimensions[0]) * dimensions[1];
        if (rootCount64 <= 1'000'000 && voxelCount <= 8'000'000)
            break;
    }
    if (rootShift > 20)
        throw std::runtime_error("IW3 light-grid extent exceeds Replay voxel-tree limits");

    std::vector<std::uint32_t> voxels(static_cast<std::size_t>(voxelCount), UINT32_MAX);
    std::vector<std::array<std::uint32_t, 4>> tetrahedra;
    constexpr std::array<std::array<unsigned, 3>, 6> permutations{{
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
    std::array<std::vector<std::array<unsigned, 4>>, 256> cellTetrahedra;
    for (unsigned mask = 1; mask < 255; ++mask)
        cellTetrahedra[mask] = SparseCellTetrahedra(mask);
    for (const auto &order : permutations)
    {
        unsigned mask = 0;
        std::array<unsigned, 4> tetrahedron{0, 0, 0, 0};
        for (unsigned step = 0; step < 3; ++step)
        {
            mask |= 1u << order[step];
            tetrahedron[step + 1] = mask;
        }
        cellTetrahedra[255].push_back(tetrahedron);
    }

    // A sparse hull need not contain its lowest corner. Enumerate each possible
    // source-cell base around authored probes, then deduplicate the bases.
    std::vector<std::array<std::int32_t, 3>> candidateBases;
    candidateBases.reserve(cells.size() * 8);
    for (const Cell &cell : cells)
        for (unsigned mask = 0; mask < 8; ++mask)
            candidateBases.push_back({cell.position[0] - ((mask & 1) ? 32 : 0),
                                      cell.position[1] - ((mask & 2) ? 32 : 0),
                                      cell.position[2] - ((mask & 4) ? 64 : 0)});
    std::ranges::sort(candidateBases);
    candidateBases.erase(std::unique(candidateBases.begin(), candidateBases.end()),
                         candidateBases.end());

    // Keep complete cells first: their tetrahedron numbers and voxel starts are
    // stable, while sparse cells contribute only their authored convex hulls.
    std::size_t octantStarts = 0;
    for (const bool sparsePass : {false, true})
    {
        const std::size_t baseCount = sparsePass ? candidateBases.size() : cells.size();
        for (std::size_t baseIndex = 0; baseIndex < baseCount; ++baseIndex)
        {
            const auto &base = sparsePass ? candidateBases[baseIndex] : cells[baseIndex].position;
            std::array<std::uint32_t, 8> corners{};
            unsigned presentMask = 0;
            for (unsigned mask = 0; mask < 8; ++mask)
            {
                const std::array<std::int32_t, 3> position{
                    base[0] + ((mask & 1) ? 32 : 0), base[1] + ((mask & 2) ? 32 : 0),
                    base[2] + ((mask & 4) ? 64 : 0)};
                const auto found = lookup.find(position);
                if (found == lookup.end())
                    continue;
                corners[mask] = found->second;
                presentMask |= 1u << mask;
            }
            if ((presentMask == 255) == sparsePass)
                continue;
            const auto &localTetrahedra = cellTetrahedra[presentMask];
            if (localTetrahedra.empty())
                continue;
            std::vector<std::uint32_t> starts;
            starts.reserve(localTetrahedra.size());
            for (const auto &local : localTetrahedra)
            {
                if (tetrahedra.size() >= 8'000'000)
                    throw std::runtime_error("IW3 light-grid tetrahedra exceed Replay limits");
                starts.push_back(static_cast<std::uint32_t>(tetrahedra.size()));
                tetrahedra.push_back({corners[local[0]], corners[local[1]],
                                      corners[local[2]], corners[local[3]]});
            }
            const std::int32_t voxelSize = 1 << rootShift;
            std::array<std::uint32_t, 3> firstVoxel{};
            std::array<std::uint32_t, 3> lastVoxel{};
            constexpr std::array<std::int32_t, 3> sourceCellSize{32, 32, 64};
            bool intersectsGrid = true;
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                const auto start = std::max(base[axis], gridLow[axis]);
                const auto end = std::min(base[axis] + sourceCellSize[axis], gridHigh[axis]);
                if (end <= start)
                {
                    intersectsGrid = false;
                    break;
                }
                firstVoxel[axis] = static_cast<std::uint32_t>((start - gridLow[axis]) / voxelSize);
                lastVoxel[axis] =
                    static_cast<std::uint32_t>((end - 1 - gridLow[axis]) / voxelSize);
            }
            if (!intersectsGrid)
                continue;
            for (std::uint32_t y = firstVoxel[1]; y <= lastVoxel[1]; ++y)
            {
                for (std::uint32_t x = firstVoxel[0]; x <= lastVoxel[0]; ++x)
                {
                    for (std::uint32_t z = firstVoxel[2]; z <= lastVoxel[2]; ++z)
                    {
                        const std::size_t voxel =
                            (static_cast<std::size_t>(y) * dimensions[0] + x) * dimensions[2] + z;
                        if (voxels[voxel] != UINT32_MAX)
                            continue;
                        if (!sparsePass)
                        {
                            voxels[voxel] = starts[0];
                            continue;
                        }
                        // A sparse voxel starts a walk only when its center or
                        // an octant center lies inside an authored simplex.
                        // This reaches partial voxels without extending the hull.
                        for (unsigned sample = 0; sample < 9 && voxels[voxel] == UINT32_MAX;
                             ++sample)
                        {
                            const unsigned octant = sample ? sample - 1 : 0;
                            const std::array<std::int32_t, 3> offset{
                                static_cast<std::int32_t>(voxelSize *
                                    (sample ? ((octant & 1) ? 3 : 1) : 2) / 4),
                                static_cast<std::int32_t>(voxelSize *
                                    (sample ? ((octant & 2) ? 3 : 1) : 2) / 4),
                                static_cast<std::int32_t>(voxelSize *
                                    (sample ? ((octant & 4) ? 3 : 1) : 2) / 4)};
                            const std::array<std::int32_t, 3> coordinate{
                                2 * (gridLow[0] + static_cast<std::int32_t>(x) * voxelSize +
                                     offset[0] - base[0]),
                                2 * (gridLow[1] + static_cast<std::int32_t>(y) * voxelSize +
                                     offset[1] - base[1]),
                                gridLow[2] + static_cast<std::int32_t>(z) * voxelSize +
                                    offset[2] - base[2]};
                            for (std::size_t localIndex = 0;
                                 localIndex < localTetrahedra.size(); ++localIndex)
                            {
                                if (SparseTetrahedronContains(coordinate,
                                                              localTetrahedra[localIndex]))
                                {
                                    voxels[voxel] = starts[localIndex];
                                    octantStarts += sample != 0;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (tetrahedra.empty())
        throw std::runtime_error("IW3 light-grid has no authored tetrahedra");

    // Replay treats UINT32_MAX as an unassigned voxel and samples its fallback
    // probe.  Preserve sparse IW3 columns instead of extending tetrahedra into
    // cells without authored light-grid coverage.
    const std::size_t unassignedVoxelCount =
        std::count(voxels.begin(), voxels.end(), UINT32_MAX);
    if (unassignedVoxelCount == voxels.size())
        throw std::runtime_error("IW3 light-grid has no populated Replay voxels");
    zt::info("iw3: retained %zu unassigned Replay light-grid voxels and %zu authored starts "
             "(%zu sparse octant starts)", unassignedVoxelCount,
             voxels.size() - unassignedVoxelCount, octantStarts);

    std::vector<std::array<std::uint32_t, 4>> neighbors(tetrahedra.size(),
                                                        {UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                                         UINT32_MAX});
    std::map<std::array<std::uint32_t, 3>, std::pair<std::uint32_t, unsigned>> faces;
    for (std::uint32_t index = 0; index < tetrahedra.size(); ++index)
    {
        for (unsigned face = 0; face < 4; ++face)
        {
            std::array<std::uint32_t, 3> key{};
            for (unsigned source = 0, target = 0; source < 4; ++source)
                if (source != face)
                    key[target++] = tetrahedra[index][source];
            std::ranges::sort(key);
            const auto found = faces.find(key);
            if (found == faces.end())
            {
                faces.emplace(key, std::pair{index, face});
            }
            else
            {
                const auto [otherIndex, otherFace] = found->second;
                neighbors[index][face] = otherIndex;
                neighbors[otherIndex][otherFace] = index;
                faces.erase(found);
            }
        }
    }

    const auto probes = BuildProbes(palette, lightUnitScale);
    std::array<float, 32> fallback{};
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
        {
            std::vector<float> occupied;
            occupied.reserve(cells.size());
            for (const Cell &cell : cells)
                occupied.push_back(probes[cell.entry.color][channel][coefficient]);
            fallback[channel * 9 + coefficient] = Median(std::move(occupied));
        }
    }
    fallback[27] = 1.0f;
    fallback[28] = 1.0f;

    std::vector<std::uint8_t> output;
    const std::uint32_t probeCount = static_cast<std::uint32_t>(cells.size());
    const std::uint32_t tetrahedronCount = static_cast<std::uint32_t>(tetrahedra.size());
    const std::uint32_t rootCount = static_cast<std::uint32_t>(rootCount64);
    output.reserve(264ull + probeCount * 44ull + tetrahedronCount * 32ull +
                   rootCount * 12ull + voxelCount * 4ull);
    AppendBytes(output, Magic.data(), Magic.size());
    Append(output, probeCount);
    Append(output, tetrahedronCount);
    Append(output, rootCount);
    Append(output, static_cast<std::uint32_t>(voxelCount));

    constexpr float diagonal = 0.5773502691896258f;
    constexpr std::array<std::array<float, 3>, 7> axes{{
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {diagonal, diagonal, diagonal},
        {diagonal, -diagonal, diagonal}, {-diagonal, diagonal, diagonal},
        {-diagonal, -diagonal, diagonal}}};
    for (const auto &axis : axes)
    {
        float minimum = 0.0f;
        float maximum = 0.0f;
        for (unsigned component = 0; component < 3; ++component)
        {
            minimum += axis[component] * (axis[component] >= 0 ? low[component] : high[component]);
            maximum += axis[component] * (axis[component] >= 0 ? high[component] : low[component]);
        }
        Append(output, axis[0]);
        Append(output, axis[1]);
        Append(output, minimum);
        Append(output, maximum);
    }
    for (const float value : fallback)
        Append(output, FloatToHalf(value));

    for (const auto dimension : dimensions)
        Append(output, static_cast<std::int32_t>(dimension));
    for (const std::int32_t value : {0, static_cast<std::int32_t>(rootShift),
                                     std::max(0, static_cast<std::int32_t>(rootShift) - 2),
                                     std::max(0, static_cast<std::int32_t>(rootShift) - 4), 0})
        Append(output, value);
    for (const auto value : gridLow)
        Append(output, static_cast<float>(value));
    Append(output, 0.0f);
    for (const auto value : gridHigh)
        Append(output, static_cast<float>(value));
    Append(output, 0.0f);

    for (const Cell &cell : cells)
    {
        const auto packed = PackProbe(probes[cell.entry.color], cell.entry.traceMask);
        AppendBytes(output, packed.data(), packed.size());
    }
    for (const Cell &cell : cells)
        for (const auto value : cell.position)
            Append(output, static_cast<float>(value));
    for (const auto &tetrahedron : tetrahedra)
        AppendBytes(output, tetrahedron.data(), sizeof(tetrahedron));
    for (const auto &neighbor : neighbors)
        AppendBytes(output, neighbor.data(), sizeof(neighbor));
    for (std::uint32_t y = 0; y < dimensions[1]; ++y)
    {
        for (std::uint32_t x = 0; x < dimensions[0]; ++x)
        {
            Append(output, 0x80000000u | ((y * dimensions[0] + x) * dimensions[2]));
            Append(output, 0u);
            Append(output, dimensions[2] - 1);
        }
    }
    AppendBytes(output, voxels.data(), voxels.size() * sizeof(voxels.front()));
    if (output.size() > 512ull * 1024 * 1024)
        throw std::runtime_error("native Replay light-grid exceeds the supported size");
    zt::info("iw3: converted %u authored light probes into %u native tetrahedra "
             "(%ux%ux%u, %u-unit voxels)",
             probeCount, tetrahedronCount, dimensions[0], dimensions[1], dimensions[2],
             1u << rootShift);
    return output;
}
} // namespace

bool PrepareNativeLightGrid(const std::filesystem::path &sourceRoot,
                            const std::string &sourceAsset,
                            const std::filesystem::path &output, const float lightUnitScale)
{
    const auto metadataPath =
        sourceRoot / "maps" / "mp" / (sourceAsset + ".lightgrid.json");
    std::error_code error;
    if (!std::filesystem::is_regular_file(metadataPath, error))
    {
        zt::warn("iw3: source map has no exported light grid");
        return false;
    }
    const Json metadata = ReadJson(metadataPath);
    if (metadata.value("available", true) == false)
    {
        zt::warn("iw3: source map has no compiled light grid");
        return false;
    }
    if (metadata.at("schema").get<int>() != 1 ||
        metadata.at("name").get<std::string>() != "maps/mp/" + sourceAsset)
        throw std::runtime_error("mismatched IW3 light-grid export");

    std::vector<std::uint8_t> palette;
    const auto cells = ReadCells(sourceRoot, metadata, palette);
    const auto payload = BuildPayload(cells, palette, lightUnitScale);
    if (!zt::write_file(output.string(), payload))
        throw std::runtime_error("cannot write temporary native Replay light grid");
    return true;
}

std::vector<std::array<std::array<float, 3>, 6>> SampleLightGridAmbientCubes(
    const std::filesystem::path &sourceRoot, const std::string &sourceAsset,
    const std::vector<std::array<float, 3>> &points, const unsigned sunPrimaryLight,
    std::vector<float> *sunVisibility)
{
    std::vector<std::array<std::array<float, 3>, 6>> cubes;
    const auto metadataPath = sourceRoot / "maps" / "mp" / (sourceAsset + ".lightgrid.json");
    std::error_code error;
    if (points.empty() || !std::filesystem::is_regular_file(metadataPath, error))
        return cubes;
    const Json metadata = ReadJson(metadataPath);
    if (metadata.value("available", true) == false)
        return cubes;
    std::vector<std::uint8_t> palette;
    const auto cells = ReadCells(sourceRoot, metadata, palette);
    const auto probes = BuildProbes(palette);
    if (cells.empty() || probes.empty())
        return cubes;

    // Grid coordinates: 32 units in x/y, 64 in z, from -131072 (ReadCells).
    const auto key = [](const std::int64_t x, const std::int64_t y, const std::int64_t z) {
        return static_cast<std::uint64_t>(x & 0xffff) | static_cast<std::uint64_t>(y & 0xffff) << 16 |
               static_cast<std::uint64_t>(z & 0xffff) << 32;
    };
    std::unordered_map<std::uint64_t, Entry> grid;
    grid.reserve(cells.size());
    for (const Cell &cell : cells)
        grid.emplace(key((cell.position[0] + 131072) / 32, (cell.position[1] + 131072) / 32,
                         (cell.position[2] + 131072) / 64),
                     cell.entry);
    constexpr std::array<std::array<float, 3>, 6> directions{
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    std::array<std::array<float, 9>, 6> basis{};
    for (std::size_t face = 0; face < 6; ++face)
        basis[face] = EvaluateShBasis(directions[face]);

    cubes.resize(points.size());
    if (sunVisibility)
        sunVisibility->assign(points.size(), 1.0f);
    std::size_t nearestFallbacks = 0, unlit = 0, shaded = 0;
    for (std::size_t index = 0; index < points.size(); ++index)
    {
        const std::array<double, 3> coordinate{(points[index][0] + 131072.0) / 32.0,
                                               (points[index][1] + 131072.0) / 32.0,
                                               (points[index][2] + 131072.0) / 64.0};
        std::array<std::int64_t, 3> base{};
        std::array<double, 3> fraction{};
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            base[axis] = static_cast<std::int64_t>(std::floor(coordinate[axis]));
            fraction[axis] = coordinate[axis] - static_cast<double>(base[axis]);
        }
        Probe sum{};
        double total = 0.0, sunlit = 0.0;
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            const auto found = grid.find(key(base[0] + (corner & 1), base[1] + ((corner >> 1) & 1),
                                             base[2] + ((corner >> 2) & 1)));
            if (found == grid.end())
                continue;
            double weight = 1.0;
            for (unsigned axis = 0; axis < 3; ++axis)
                weight *= (corner >> axis) & 1 ? fraction[axis] : 1.0 - fraction[axis];
            if (weight <= 0.0)
                continue;
            for (unsigned channel = 0; channel < 3; ++channel)
                for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
                    sum[channel][coefficient] += static_cast<float>(
                        weight * probes.at(found->second.color)[channel][coefficient]);
            total += weight;
            if (found->second.primaryLight == sunPrimaryLight)
                sunlit += weight;
        }
        if (total < 1.0e-3)
        {
            // No surrounding cells (solid volume or outside the grid): the
            // nearest cell in growing shells, as IW3 falls back to.
            std::optional<Entry> nearest;
            for (std::int64_t radius = 1; radius <= 8 && !nearest; ++radius)
                for (std::int64_t z = -radius; z <= radius && !nearest; ++z)
                    for (std::int64_t y = -radius; y <= radius && !nearest; ++y)
                        for (std::int64_t x = -radius; x <= radius && !nearest; ++x)
                        {
                            if (std::max({std::abs(x), std::abs(y), std::abs(z)}) != radius)
                                continue;
                            const auto found = grid.find(key(base[0] + x, base[1] + y, base[2] + z));
                            if (found != grid.end())
                                nearest = found->second;
                        }
            if (!nearest)
            {
                ++unlit;
                continue;
            }
            ++nearestFallbacks;
            sum = probes.at(nearest->color);
            total = 1.0;
            sunlit = nearest->primaryLight == sunPrimaryLight ? 1.0 : 0.0;
        }
        if (sunVisibility)
        {
            (*sunVisibility)[index] = static_cast<float>(sunlit / total);
            shaded += sunlit / total < 0.5;
        }
        for (std::size_t face = 0; face < 6; ++face)
            for (unsigned channel = 0; channel < 3; ++channel)
            {
                double value = 0.0;
                for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
                    value += sum[channel][coefficient] / total * basis[face][coefficient];
                cubes[index][face][channel] = static_cast<float>(std::max(0.0, value));
            }
    }
    zt::info("iw3: lit %zu static model instance(s) from the IW3 light grid (%zu nearest-cell, %zu "
             "outside the grid, %zu out of the sun)",
             points.size() - unlit, nearestFallbacks, unlit, shaded);
    return cubes;
}

namespace
{
// Com_CanPrimaryLightAffectPoint (light->dir points toward the light).
bool CanPrimaryLightAffectPoint(const LightGridPrimaryLight &light, const std::array<float, 3> &point)
{
    const std::array<float, 3> delta{light.origin[0] - point[0], light.origin[1] - point[1],
                                     light.origin[2] - point[2]};
    const float distanceSquared = delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2];
    if (distanceSquared >= light.radius * light.radius)
        return false;
    if (light.type == 3 || light.rotationLimit <= -light.cosHalfFovOuter)
        return true;
    const float spotDotTimesDistance = delta[0] * light.dir[0] + delta[1] * light.dir[1] + delta[2] * light.dir[2];
    float cosHalfFov = light.cosHalfFovOuter;
    if (light.rotationLimit != 1.0f)
    {
        // CosOfSumOfArcCos
        cosHalfFov = light.cosHalfFovOuter * light.rotationLimit -
                     std::sqrt(std::max(0.0f, (1 - light.cosHalfFovOuter * light.cosHalfFovOuter) *
                                                  (1 - light.rotationLimit * light.rotationLimit)));
        if (cosHalfFov <= 0.0f)
            return spotDotTimesDistance <= cosHalfFov * light.radius;
    }
    return spotDotTimesDistance > 0.0f &&
           cosHalfFov * cosHalfFov * distanceSquared <= spotDotTimesDistance * spotDotTimesDistance;
}

// R_SetLightGridColors: the 56 grid directions per texel of the 4x4x4 volume
// (x fastest, then y, then z); the inner core repeats corner samples.
constexpr std::array<std::uint8_t, 64> kModelLightingTexelColors{
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 0,  3,  21, 22, 12, 15, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 40, 43, 33, 34, 52, 55, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55};
} // namespace

std::vector<std::array<std::uint8_t, 256>> SampleStaticModelLighting(
    const std::filesystem::path &sourceRoot, const std::string &sourceAsset,
    const std::vector<StaticModelLightingInput> &inputs,
    const std::vector<LightGridPrimaryLight> &primaryLights, const LightGridSightBlocked &blocked)
{
    std::vector<std::array<std::uint8_t, 256>> volumes;
    const auto metadataPath = sourceRoot / "maps" / "mp" / (sourceAsset + ".lightgrid.json");
    std::error_code error;
    if (inputs.empty() || !std::filesystem::is_regular_file(metadataPath, error))
        return volumes;
    const Json metadata = ReadJson(metadataPath);
    if (metadata.value("available", true) == false)
        return volumes;
    std::vector<std::uint8_t> palette;
    const auto cells = ReadCells(sourceRoot, metadata, palette);
    const std::size_t colorCount = palette.size() / 168;
    if (cells.empty() || !colorCount)
        return volumes;
    const int rowAxis = metadata.at("row_axis").get<int>();
    const int columnAxis = metadata.at("col_axis").get<int>();
    const unsigned sun = metadata.at("sun_primary_light_index").get<unsigned>();
    const bool hasLightRegions = metadata.value("has_light_regions", false);

    // Grid points by integer coordinate (32 units in x/y, 64 in z, from -131072).
    const auto key = [](const std::int64_t x, const std::int64_t y, const std::int64_t z) {
        return static_cast<std::uint64_t>(x & 0xffff) | static_cast<std::uint64_t>(y & 0xffff) << 16 |
               static_cast<std::uint64_t>(z & 0xffff) << 32;
    };
    std::unordered_map<std::uint64_t, Entry> grid;
    std::unordered_map<std::uint64_t, std::int64_t> columnBottom;
    grid.reserve(cells.size());
    for (const Cell &cell : cells)
    {
        const std::int64_t x = (cell.position[0] + 131072) / 32, y = (cell.position[1] + 131072) / 32,
                           z = (cell.position[2] + 131072) / 64;
        grid.emplace(key(x, y, z), cell.entry);
        auto [bottom, inserted] = columnBottom.try_emplace(key(x, y, 0), z);
        if (!inserted)
            bottom->second = std::min(bottom->second, z);
    }
    const auto light = [&](const unsigned index) -> const LightGridPrimaryLight * {
        return index < primaryLights.size() ? &primaryLights[index] : nullptr;
    };

    volumes.resize(inputs.size());
    std::size_t ground = 0, extrapolated = 0, traced = 0, suppressed = 0;
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        const StaticModelLightingInput &input = inputs[index];
        auto &texels = volumes[index];
        if (input.groundLit && input.groundLighting)
        {
            // R_SetModelGroundLighting: the packed colour in every texel (B, G, R, A).
            for (unsigned texel = 0; texel < 64; ++texel)
            {
                texels[texel * 4 + 0] = static_cast<std::uint8_t>(input.groundLighting >> 16);
                texels[texel * 4 + 1] = static_cast<std::uint8_t>(input.groundLighting >> 8);
                texels[texel * 4 + 2] = static_cast<std::uint8_t>(input.groundLighting);
                texels[texel * 4 + 3] = static_cast<std::uint8_t>(input.groundLighting >> 24);
            }
            ++ground;
            continue;
        }

        // R_LightGridLookup.
        const auto &sample = input.origin;
        std::array<std::int64_t, 3> pos{(static_cast<std::int64_t>(std::floor(sample[0])) + 0x20000) >> 5,
                                        (static_cast<std::int64_t>(std::floor(sample[1])) + 0x20000) >> 5,
                                        (static_cast<std::int64_t>(std::floor(sample[2])) + 0x20000) >> 6};
        const float lerpRow = (sample[rowAxis] + 131072.0f) * 0.03125f - static_cast<float>(pos[rowAxis]);
        const float lerpColumn =
            (sample[columnAxis] + 131072.0f) * 0.03125f - static_cast<float>(pos[columnAxis]);
        const float lerpZ = (sample[2] + 131072.0f) * 0.015625f - static_cast<float>(pos[2]);
        std::array<float, 8> weight{};
        std::array<const Entry *, 8> entry{};
        unsigned defaultEntry = 1;
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            weight[corner] = ((corner & 4) ? lerpRow : 1 - lerpRow) *
                             ((corner & 2) ? lerpColumn : 1 - lerpColumn) * ((corner & 1) ? lerpZ : 1 - lerpZ);
            std::array<std::int64_t, 3> p = pos;
            p[rowAxis] += (corner >> 2) & 1;
            p[columnAxis] += (corner >> 1) & 1;
            p[2] += corner & 1;
            const auto found = grid.find(key(p[0], p[1], p[2]));
            entry[corner] = found == grid.end() ? nullptr : &found->second;
        }
        // R_GetLightGridSampleEntryQuad: below the data of either base column,
        // extrapolation uses palette entry 0 instead of 1.
        for (unsigned row = 0; row < 2; ++row)
        {
            std::array<std::int64_t, 3> p = pos;
            p[rowAxis] += row;
            const auto bottom = columnBottom.find(key(p[0], p[1], 0));
            if (bottom != columnBottom.end() && pos[2] < bottom->second)
                defaultEntry = 0;
        }
        std::uint8_t lookupPrimary = 0;
        float bestWeight = 0;
        bool honorSuppression = false;
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            const Entry *current = entry[corner];
            if (!current)
                continue;
            if (weight[corner] < 0.001f) // EQUAL_EPSILON
            {
                entry[corner] = nullptr;
                continue;
            }
            bool suppress = false;
            if (current->traceMask & (1u << corner))
            {
                // R_IsValidLightGridSample: sight from the sample to the corner,
                // nudged 0.01 toward the sample.
                std::array<float, 3> gridPos{static_cast<float>(pos[0]) * 32.0f - 131072.0f,
                                             static_cast<float>(pos[1]) * 32.0f - 131072.0f,
                                             static_cast<float>(pos[2]) * 64.0f - 131072.0f};
                gridPos[rowAxis] += (corner & 4) ? 32.0f : 0.0f;
                gridPos[columnAxis] += (corner & 2) ? 32.0f : 0.0f;
                gridPos[2] += (corner & 1) ? 64.0f : 0.0f;
                std::array<float, 3> direction{sample[0] - gridPos[0], sample[1] - gridPos[1],
                                               sample[2] - gridPos[2]};
                const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                               direction[2] * direction[2]);
                if (length > 0)
                    for (float &component : direction)
                        component /= length;
                const std::array<float, 3> nudged{gridPos[0] + 0.01f * direction[0],
                                                  gridPos[1] + 0.01f * direction[1],
                                                  gridPos[2] + 0.01f * direction[2]};
                ++traced;
                suppress = blocked(sample, nudged);
                suppressed += suppress;
            }
            if (suppress)
            {
                if (honorSuppression)
                {
                    entry[corner] = nullptr;
                    continue;
                }
            }
            else if (!honorSuppression)
            {
                honorSuppression = true;
                bestWeight = weight[corner];
                lookupPrimary = current->primaryLight;
                for (unsigned previous = 0; previous < corner; ++previous)
                    entry[previous] = nullptr;
                continue;
            }
            const std::uint8_t candidate = current->primaryLight;
            const bool take = lookupPrimary ? candidate && (lookupPrimary == 255 ||
                                                            (candidate != 255 && weight[corner] > bestWeight))
                                            : true;
            if (take)
            {
                bestWeight = weight[corner];
                lookupPrimary = candidate;
            }
        }

        // R_GetLightingAtPoint.
        unsigned primary = lookupPrimary;
        if (primary == 255)
            primary = sun;
        else if (hasLightRegions && primary != sun)
            primary = input.primaryLight;
        std::array<std::uint16_t, 8> sampleColor{};
        std::array<float, 8> sampleWeight{};
        unsigned sampleCount = 0;
        float maxWeight = 0, visible = 0, occluded = 0;
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            const Entry *current = entry[corner];
            if (!current)
                continue;
            if (current->primaryLight == primary)
                visible += weight[corner];
            else if (!current->primaryLight || (current->primaryLight == 255 && primary))
            {
                // R_CanLightInfluenceLightGridCorner.
                const LightGridPrimaryLight *primaryLight = light(primary);
                bool influences = !primaryLight || primaryLight->type == 1;
                if (!influences)
                {
                    std::array<float, 3> gridPos{std::floor(sample[0] * 0.03125f) * 32.0f,
                                                 std::floor(sample[1] * 0.03125f) * 32.0f,
                                                 std::floor(sample[2] * 0.015625f) * 64.0f};
                    gridPos[rowAxis] += (corner & 4) ? 32.0f : 0.0f;
                    gridPos[columnAxis] += (corner & 2) ? 32.0f : 0.0f;
                    gridPos[2] += (corner & 1) ? 64.0f : 0.0f;
                    influences = CanPrimaryLightAffectPoint(*primaryLight, gridPos);
                }
                if (influences)
                    occluded += weight[corner];
            }
            maxWeight += weight[corner];
            // R_AddLightGridSample
            unsigned slot = 0;
            while (slot < sampleCount && sampleColor[slot] != current->color)
                ++slot;
            if (slot == sampleCount)
            {
                sampleColor[sampleCount] = current->color;
                sampleWeight[sampleCount++] = 0;
            }
            sampleWeight[slot] += weight[corner];
        }

        std::array<std::uint8_t, 168> colors{};
        float primaryWeight = 1.0f;
        if (sampleCount)
        {
            if (primary)
                primaryWeight = occluded == 0 ? (visible != 0 ? 1.0f : 0.0f) : visible / (visible + occluded);
            else
                primaryWeight = 0;
            if (sampleCount == 1)
                std::memcpy(colors.data(), palette.data() + std::size_t(sampleColor[0]) * 168, 168);
            else
            {
                // R_GetLightGridColorsFixedPointBlendWeights + R_FixedPointBlendLightGridColors.
                const float scale = 1.0f / maxWeight;
                std::array<std::uint16_t, 8> fixedWeight{};
                std::uint16_t sum = 0;
                unsigned largest = 0;
                for (unsigned slot = 0; slot < sampleCount; ++slot)
                {
                    fixedWeight[slot] = static_cast<std::uint16_t>(static_cast<int>(scale * 256.0f * sampleWeight[slot] + 0.5f));
                    sum = static_cast<std::uint16_t>(sum + fixedWeight[slot]);
                    if (fixedWeight[largest] < fixedWeight[slot])
                        largest = slot;
                }
                fixedWeight[largest] = static_cast<std::uint16_t>(fixedWeight[largest] + 256 - sum);
                std::array<std::uint16_t, 168> accumulated{};
                for (unsigned slot = 0; slot < sampleCount; ++slot)
                    for (unsigned byte = 0; byte < 168; ++byte)
                        accumulated[byte] = static_cast<std::uint16_t>(
                            accumulated[byte] + fixedWeight[slot] * palette[std::size_t(sampleColor[slot]) * 168 + byte]);
                for (unsigned byte = 0; byte < 168; ++byte)
                    colors[byte] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(accumulated[byte] + 127) >> 8);
            }
        }
        else
        {
            // R_ExtrapolateLightingAtPoint (GFX_MODELLIGHT_EXTRAPOLATE).
            const std::size_t fallback = colorCount <= defaultEntry ? colorCount - 1 : defaultEntry;
            std::memcpy(colors.data(), palette.data() + fallback * 168, 168);
            primary = sun;
            ++extrapolated;
        }
        // The model's primary light gets the weight; only a sun primary lights the
        // model with the sun (alpha = sun weight).
        const auto alpha = static_cast<std::uint8_t>(
            primary == sun ? static_cast<int>(primaryWeight * 255.0f + 0.5f) : 0);
        for (unsigned texel = 0; texel < 64; ++texel)
        {
            const unsigned color = kModelLightingTexelColors[texel];
            texels[texel * 4 + 0] = colors[color * 3 + 0];
            texels[texel * 4 + 1] = colors[color * 3 + 1];
            texels[texel * 4 + 2] = colors[color * 3 + 2];
            texels[texel * 4 + 3] = alpha;
        }
    }
    zt::info("iw3: lit %zu static model(s) as IW3 does (%zu ground-lit, %zu extrapolated; %zu grid "
             "corner trace(s), %zu suppressed)",
             inputs.size(), ground, extrapolated, traced, suppressed);
    return volumes;
}

std::optional<EntityLightGrid> BuildEntityLightGrid(const std::filesystem::path &sourceRoot,
                                                    const std::string &sourceAsset)
{
    const auto metadataPath = sourceRoot / "maps" / "mp" / (sourceAsset + ".lightgrid.json");
    std::error_code error;
    if (!std::filesystem::is_regular_file(metadataPath, error))
        return std::nullopt;
    const Json metadata = ReadJson(metadataPath);
    if (metadata.value("available", true) == false)
        return std::nullopt;
    std::vector<std::uint8_t> palette;
    const auto cells = ReadCells(sourceRoot, metadata, palette);
    const std::size_t colorCount = palette.size() / 168;
    if (cells.empty() || !colorCount || colorCount >= 65535)
        return std::nullopt;
    EntityLightGrid grid;
    std::array<int, 3> maximum{};
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        grid.minimum[axis] = metadata.at("mins").at(axis).get<int>();
        maximum[axis] = metadata.at("maxs").at(axis).get<int>();
        grid.dims[axis] = static_cast<unsigned>(maximum[axis] - grid.minimum[axis] + 1);
    }
    const std::size_t entryTexels = std::size_t(grid.dims[0]) * grid.dims[1] * grid.dims[2];
    grid.width = 4096;
    grid.paletteStart = static_cast<unsigned>((entryTexels + grid.width - 1) / grid.width * grid.width);
    grid.paletteColors = colorCount;
    const std::size_t total = grid.paletteStart + colorCount * 56;
    grid.height = static_cast<unsigned>((total + grid.width - 1) / grid.width);
    grid.pixels.assign(std::size_t(grid.width) * grid.height * 4, 0);
    for (const Cell &cell : cells)
    {
        const std::array<int, 3> point{(cell.position[0] + 131072) / 32 - grid.minimum[0],
                                       (cell.position[1] + 131072) / 32 - grid.minimum[1],
                                       (cell.position[2] + 131072) / 64 - grid.minimum[2]};
        if (point[0] < 0 || point[1] < 0 || point[2] < 0 || point[0] >= int(grid.dims[0]) ||
            point[1] >= int(grid.dims[1]) || point[2] >= int(grid.dims[2]))
            throw std::runtime_error("IW3 light-grid entry is outside its bounds");
        const std::size_t texel =
            (std::size_t(point[2]) * grid.dims[1] + point[1]) * grid.dims[0] + point[0];
        const unsigned color = cell.entry.color + 1u;
        grid.pixels[texel * 4 + 0] = static_cast<std::uint8_t>(color);
        grid.pixels[texel * 4 + 1] = static_cast<std::uint8_t>(color >> 8);
        grid.pixels[texel * 4 + 2] = cell.entry.primaryLight;
        grid.pixels[texel * 4 + 3] = 255;
    }
    for (std::size_t color = 0; color < colorCount; ++color)
        for (unsigned sample = 0; sample < 56; ++sample)
        {
            const std::size_t texel = grid.paletteStart + color * 56 + sample;
            for (unsigned channel = 0; channel < 3; ++channel)
                grid.pixels[texel * 4 + channel] = palette[color * 168 + sample * 3 + channel];
            grid.pixels[texel * 4 + 3] = 255;
        }
    zt::info("iw3: entity light grid %ux%ux%u points, %zu palette colours (%ux%u image)", grid.dims[0],
             grid.dims[1], grid.dims[2], colorCount, grid.width, grid.height);
    return grid;
}

std::optional<LightGridVolume> BuildLightGridVolume(const std::filesystem::path &sourceRoot,
                                                    const std::string &sourceAsset)
{
    const auto metadataPath = sourceRoot / "maps" / "mp" / (sourceAsset + ".lightgrid.json");
    std::error_code error;
    if (!std::filesystem::is_regular_file(metadataPath, error))
        return std::nullopt;
    const Json metadata = ReadJson(metadataPath);
    if (metadata.value("available", true) == false)
        return std::nullopt;
    std::vector<std::uint8_t> palette;
    const auto cells = ReadCells(sourceRoot, metadata, palette);
    const auto probes = BuildProbes(palette);
    if (cells.empty() || probes.empty())
        return std::nullopt;

    std::array<std::int64_t, 3> low{INT64_MAX, INT64_MAX, INT64_MAX}, high{INT64_MIN, INT64_MIN, INT64_MIN};
    const auto coordinate = [](const Cell &cell) {
        return std::array<std::int64_t, 3>{(cell.position[0] + 131072) / 32,
                                           (cell.position[1] + 131072) / 32,
                                           (cell.position[2] + 131072) / 64};
    };
    for (const Cell &cell : cells)
    {
        const auto c = coordinate(cell);
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            low[axis] = std::min(low[axis], c[axis]);
            high[axis] = std::max(high[axis], c[axis]);
        }
    }
    // Coarsen until the three coefficient groups of every Z slice fit a
    // 4096 x 4096 atlas.
    std::array<std::int64_t, 3> step{1, 1, 1};
    LightGridVolume volume;
    for (;;)
    {
        for (unsigned axis = 0; axis < 3; ++axis)
            volume.dims[axis] = static_cast<unsigned>((high[axis] - low[axis]) / step[axis] + 1);
        const unsigned tileWidth = volume.dims[0] + 2, tileHeight = volume.dims[1] + 2;
        volume.tilesPerRow = tileWidth <= 4096 ? 4096 / tileWidth : 0;
        if (volume.tilesPerRow)
        {
            const unsigned tiles = 3 * volume.dims[2];
            const unsigned rows = (tiles + volume.tilesPerRow - 1) / volume.tilesPerRow;
            // Replay's resident image loader pads each mip to 16 bytes; keep rows 16-byte
            // multiples so the RGBA8 file length is exactly width * height * 4.
            volume.width = (std::min(tiles, volume.tilesPerRow) * tileWidth + 3) & ~3u;
            volume.height = rows * tileHeight;
            if (volume.height <= 4096)
                break;
        }
        if (step[0] <= step[2] * 2)
        {
            step[0] *= 2;
            step[1] *= 2;
        }
        else
            step[2] *= 2;
        if (step[0] > 64)
            return std::nullopt;
    }
    volume.cell = {32.0f * step[0], 32.0f * step[1], 64.0f * step[2]};
    for (unsigned axis = 0; axis < 3; ++axis)
        volume.origin[axis] = static_cast<float>(low[axis] * (axis == 2 ? 64 : 32) - 131072) +
                              0.5f * (volume.cell[axis] - (axis == 2 ? 64.0f : 32.0f));

    // L0 + L1 per channel (12 values) per volume cell, averaged over the IW3
    // cells it covers.
    const std::size_t count = std::size_t(volume.dims[0]) * volume.dims[1] * volume.dims[2];
    std::vector<std::array<float, 12>> sh(count);
    std::vector<float> weight(count, 0.0f);
    const auto index = [&](const std::array<std::int64_t, 3> &c) {
        return (std::size_t(c[2]) * volume.dims[1] + std::size_t(c[1])) * volume.dims[0] + std::size_t(c[0]);
    };
    for (const Cell &cell : cells)
    {
        auto c = coordinate(cell);
        for (unsigned axis = 0; axis < 3; ++axis)
            c[axis] = (c[axis] - low[axis]) / step[axis];
        const std::size_t at = index(c);
        const Probe &probe = probes.at(cell.entry.color);
        for (unsigned channel = 0; channel < 3; ++channel)
            for (unsigned coefficient = 0; coefficient < 4; ++coefficient)
                sh[at][channel * 4 + coefficient] += probe[channel][coefficient];
        weight[at] += 1.0f;
    }
    std::array<double, 12> total{};
    double totalWeight = 0.0;
    for (std::size_t at = 0; at < count; ++at)
        if (weight[at] > 0.0f)
        {
            for (unsigned value = 0; value < 12; ++value)
            {
                sh[at][value] /= weight[at];
                total[value] += sh[at][value];
            }
            totalWeight += 1.0;
            weight[at] = 1.0f;
        }
    // Empty cells (solid, outside the playable grid) take their filled
    // neighbours' average, so trilinear filtering never blends in black.
    std::size_t filled = 0;
    for (unsigned pass = 0; pass < 8; ++pass)
    {
        std::vector<std::size_t> updates;
        std::vector<std::array<float, 12>> values;
        for (unsigned z = 0; z < volume.dims[2]; ++z)
            for (unsigned y = 0; y < volume.dims[1]; ++y)
                for (unsigned x = 0; x < volume.dims[0]; ++x)
                {
                    const std::size_t at = index({x, y, z});
                    if (weight[at] > 0.0f)
                        continue;
                    std::array<float, 12> sum{};
                    unsigned neighbours = 0;
                    const std::array<std::array<int, 3>, 6> offsets{
                        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
                    for (const auto &offset : offsets)
                    {
                        const std::int64_t nx = std::int64_t(x) + offset[0], ny = std::int64_t(y) + offset[1],
                                           nz = std::int64_t(z) + offset[2];
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= volume.dims[0] || ny >= volume.dims[1] ||
                            nz >= volume.dims[2])
                            continue;
                        const std::size_t other = index({nx, ny, nz});
                        if (weight[other] <= 0.0f)
                            continue;
                        for (unsigned value = 0; value < 12; ++value)
                            sum[value] += sh[other][value];
                        ++neighbours;
                    }
                    if (!neighbours)
                        continue;
                    for (auto &value : sum)
                        value /= static_cast<float>(neighbours);
                    updates.push_back(at);
                    values.push_back(sum);
                }
        for (std::size_t update = 0; update < updates.size(); ++update)
        {
            sh[updates[update]] = values[update];
            weight[updates[update]] = 1.0f;
        }
        filled += updates.size();
        if (updates.empty())
            break;
    }
    for (std::size_t at = 0; at < count; ++at)
        if (weight[at] <= 0.0f)
            for (unsigned value = 0; value < 12; ++value)
                sh[at][value] = totalWeight > 0.0 ? static_cast<float>(total[value] / totalWeight) : 0.0f;

    // Group layout (see the header): channel*4 + coefficient indexes sh.
    constexpr std::array<std::array<unsigned, 4>, 3> groups{{{0, 4, 8, 1}, {2, 3, 5, 6}, {7, 9, 10, 11}}};
    const auto encode = [](const float value, const bool l0) {
        const float unit = l0 ? value / 8.0f : value / 8.0f + 0.5f;
        return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0f, 1.0f) * 255.0f));
    };
    const unsigned tileWidth = volume.dims[0] + 2, tileHeight = volume.dims[1] + 2;
    volume.pixels.assign(std::size_t(volume.width) * volume.height * 4, 0);
    for (unsigned group = 0; group < 3; ++group)
        for (unsigned z = 0; z < volume.dims[2]; ++z)
        {
            const unsigned tile = group * volume.dims[2] + z;
            const unsigned originX = (tile % volume.tilesPerRow) * tileWidth;
            const unsigned originY = (tile / volume.tilesPerRow) * tileHeight;
            for (unsigned ty = 0; ty < tileHeight; ++ty)
                for (unsigned tx = 0; tx < tileWidth; ++tx)
                {
                    // The border replicates the edge cells.
                    const unsigned x = std::min(std::max(tx, 1u) - 1, volume.dims[0] - 1);
                    const unsigned y = std::min(std::max(ty, 1u) - 1, volume.dims[1] - 1);
                    const auto &cell = sh[index({x, y, z})];
                    auto *out = volume.pixels.data() +
                                (std::size_t(originY + ty) * volume.width + originX + tx) * 4;
                    for (unsigned channel = 0; channel < 4; ++channel)
                    {
                        const unsigned value = groups[group][channel];
                        out[channel] = encode(cell[value], value % 4 == 0);
                    }
                }
        }
    zt::info("iw3: light-grid volume %ux%ux%u cells of %gx%gx%g units (%zu filled from neighbours), "
             "%ux%u atlas",
             volume.dims[0], volume.dims[1], volume.dims[2], volume.cell[0], volume.cell[1],
             volume.cell[2], filled, volume.width, volume.height);
    return volume;
}
} // namespace iw3
