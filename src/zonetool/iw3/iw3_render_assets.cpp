#include "iw3_render_assets.h"

#include "resources.h"

#include "common/fs_util.h"
#include "common/log.h"
#include "directxtex/BC.h"

#include <Windows.h>
#include <bcrypt.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace iw3
{
namespace
{
using Json = nlohmann::json;

struct Image
{
    unsigned width{};
    unsigned height{};
    std::vector<std::uint8_t> rgba;
    // Authored source levels in largest-to-smallest order, excluding this level.
    std::vector<Image> mips;
};

struct Cubemap
{
    unsigned width{};
    unsigned height{};
    unsigned mipCount{};
    std::vector<std::vector<Image>> faces;
    std::vector<std::uint8_t> residentPixels;
};

struct SourceMaterial
{
    std::string name;
    unsigned surfaceTypeBits{};
    std::string color;
    std::string normal;
    std::string response;
    std::array<float, 4> tint{1, 1, 1, 1};
    std::array<float, 4> environment{0.8f, 4.0f, 2.5f, 0.625f};
    SurfaceKind kind{SurfaceKind::opaque};
    bool castsShadow{true};
    bool shadowOnly{};
    unsigned flags{};
    unsigned cullMode{1}; // Replay: none=0, back=1, front=2.
    bool depthBias{};     // IW3 polygonOffset on the color pass.
};

enum class FxMaterialFamily
{
    unsupported,
    alpha,
    alphaFeather,
    additive,
    additiveFeather,
    cloud,
    decal,
    // IW3 distortion_* screen refraction. Replay distortion needs a native
    // refraction technique; these elements are left out of converted effects.
    distortion,
};

struct SourceFxMaterial
{
    std::string name;
    std::string techniqueSet;
    std::string image;
    std::string normal;
    std::string response;
    std::array<float, 4> tint{1, 1, 1, 1};
    unsigned rows{1};
    unsigned columns{1};
    FxMaterialFamily family{FxMaterialFamily::unsupported};
    // srcalpha/one blending, drawn by the one/one additive family with the
    // colour map premultiplied by its alpha (the same result).
    bool premultiplied{};
};

std::vector<std::uint8_t> ReadBytes(const std::filesystem::path &path)
{
    std::vector<std::uint8_t> data;
    if (!zt::read_file(path.string(), data))
        throw std::runtime_error("cannot read " + path.string());
    return data;
}

void WriteBytes(const std::filesystem::path &path, const std::vector<std::uint8_t> &data)
{
    std::filesystem::create_directories(path.parent_path());
    if (!zt::write_file(path.string(), data))
        throw std::runtime_error("cannot write " + path.string());
}

std::array<std::uint8_t, 8> EncodeBc4Block(const std::array<std::uint8_t, 16> &values)
{
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    std::array<std::uint8_t, 8> block{};
    block[0] = *maximum;
    block[1] = *minimum;
    if (block[0] == block[1])
        return block;

    std::array<unsigned, 8> palette{block[0], block[1]};
    for (unsigned index = 1; index <= 6; ++index)
        palette[index + 1] = ((7 - index) * unsigned(block[0]) +
                              index * unsigned(block[1]) + 3) /
                             7;

    std::uint64_t indices = 0;
    for (unsigned pixel = 0; pixel < values.size(); ++pixel)
    {
        unsigned best = 0;
        unsigned distance = UINT_MAX;
        for (unsigned candidate = 0; candidate < palette.size(); ++candidate)
        {
            const unsigned delta = palette[candidate] > values[pixel]
                                       ? palette[candidate] - values[pixel]
                                       : values[pixel] - palette[candidate];
            if (delta < distance)
            {
                distance = delta;
                best = candidate;
            }
        }
        indices |= std::uint64_t(best) << (pixel * 3);
    }
    for (unsigned byte = 0; byte < 6; ++byte)
        block[byte + 2] = static_cast<std::uint8_t>(indices >> (byte * 8));
    return block;
}

std::vector<std::uint8_t> EncodeBc4(const unsigned width, const unsigned height,
                                    const std::vector<std::uint8_t> &values)
{
    if (values.size() != static_cast<std::size_t>(width) * height)
        throw std::runtime_error("invalid BC4 source dimensions");
    const unsigned blocksWide = (width + 3) / 4;
    const unsigned blocksHigh = (height + 3) / 4;
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(blocksWide) * blocksHigh * 8);
    for (unsigned blockY = 0; blockY < blocksHigh; ++blockY)
        for (unsigned blockX = 0; blockX < blocksWide; ++blockX)
        {
            std::array<std::uint8_t, 16> blockValues{};
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x)
                {
                    const unsigned sourceX = std::min(blockX * 4 + x, width - 1);
                    const unsigned sourceY = std::min(blockY * 4 + y, height - 1);
                    blockValues[y * 4 + x] = values[static_cast<std::size_t>(sourceY) * width +
                                                        sourceX];
                }
            const auto block = EncodeBc4Block(blockValues);
            std::memcpy(encoded.data() +
                            (static_cast<std::size_t>(blockY) * blocksWide + blockX) * 8,
                        block.data(), block.size());
        }
    return encoded;
}

std::vector<std::uint8_t> EncodeBc5(const unsigned width, const unsigned height,
                                    const std::vector<std::uint8_t> &xValues,
                                    const std::vector<std::uint8_t> &yValues)
{
    const auto x = EncodeBc4(width, height, xValues);
    const auto y = EncodeBc4(width, height, yValues);
    std::vector<std::uint8_t> encoded(x.size() * 2);
    for (std::size_t block = 0; block < x.size() / 8; ++block)
    {
        std::memcpy(encoded.data() + block * 16, x.data() + block * 8, 8);
        std::memcpy(encoded.data() + block * 16 + 8, y.data() + block * 8, 8);
    }
    return encoded;
}

std::uint32_t EncodeUnsignedFloat(const float value, const unsigned mantissaBits)
{
    if (!(value > 0.0f))
        return 0;
    int exponent = 0;
    const float mantissa = std::frexp(value, &exponent) * 2.0f;
    int biasedExponent = exponent - 1 + 15;
    if (biasedExponent <= 0)
    {
        const auto denormal = static_cast<std::uint32_t>(
            std::lround(std::ldexp(value, 14 + static_cast<int>(mantissaBits))));
        return std::min(denormal, (1u << mantissaBits) - 1);
    }
    if (biasedExponent >= 31)
        return (30u << mantissaBits) | ((1u << mantissaBits) - 1);
    auto fraction = static_cast<std::uint32_t>(
        std::lround((mantissa - 1.0f) * static_cast<float>(1u << mantissaBits)));
    if (fraction == (1u << mantissaBits))
    {
        fraction = 0;
        if (++biasedExponent >= 31)
            return (30u << mantissaBits) | ((1u << mantissaBits) - 1);
    }
    return (static_cast<std::uint32_t>(biasedExponent) << mantissaBits) | fraction;
}

std::uint32_t EncodeR11G11B10(const float red, const float green, const float blue)
{
    const std::uint32_t r = EncodeUnsignedFloat(red, 6);
    const std::uint32_t g = EncodeUnsignedFloat(green, 6);
    const std::uint32_t b = EncodeUnsignedFloat(blue, 5);
    return r | (g << 11) | (b << 22);
}

struct NativeLightmapPixel
{
    std::uint32_t irradiance{};
    std::uint8_t directionX{};
    std::uint8_t directionY{};
};

std::array<std::uint8_t, 2> EncodeOctDirection(const float slopeX, const float slopeY)
{
    const float denominator = 1.0f + std::abs(slopeX) + std::abs(slopeY);
    const float octX = slopeX / denominator;
    const float octY = slopeY / denominator;
    const auto encode = [](const float value) {
        return static_cast<std::uint8_t>(
            std::lround(std::clamp(value * 127.5f, 0.0f, 255.0f)));
    };
    return {encode(1.0f + octX + octY), encode(1.0f + octX - octY)};
}

NativeLightmapPixel EncodeNativeLightmapPixel(const std::array<std::uint8_t, 4> &a,
                                             const std::array<std::uint8_t, 4> &b)
{
    // IW3's secondary lightmap evaluates A * normal.z + B * dot(normal, D).
    // The retained Replay shader evaluates irradiance * dot(normal, D') / D'.z
    // when its BC4 ambient fraction is zero. Fit the shared direction to the
    // three color channels, preserving the source flat-normal result before
    // Replay's own exposure scale is applied.
    constexpr float byteScale = 1.0f / 255.0f;
    const float slopeX = 4.07999992f * (a[3] * byteScale) - 2.07999992f;
    const float slopeY = 4.06451607f * (b[3] * byteScale) - 2.06451607f;
    const float directionZ = 1.0f / std::sqrt(1.0f + slopeX * slopeX + slopeY * slopeY);
    std::array<float, 3> irradiance{};
    float irradianceSquared = 0.0f;
    float directionalDot = 0.0f;
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        const float directional = b[channel] * byteScale;
        irradiance[channel] = a[channel] * byteScale + directional * directionZ;
        irradianceSquared += irradiance[channel] * irradiance[channel];
        directionalDot += irradiance[channel] * directional;
    }
    const float ratio = irradianceSquared > 0.0f
                            ? directionZ * directionalDot / irradianceSquared
                            : 0.0f;
    const auto direction = EncodeOctDirection(slopeX * ratio, slopeY * ratio);
    return {EncodeR11G11B10(irradiance[0], irradiance[1], irradiance[2]),
            direction[0], direction[1]};
}

Image EncodeNativeWorldNormal(Image source)
{
    for (std::size_t offset = 0; offset < source.rgba.size(); offset += 4)
    {
        const float slopeX = 4.07999992f * (source.rgba[offset] / 255.0f) - 2.07999992f;
        const float slopeY = 4.06451607f * (source.rgba[offset + 1] / 255.0f) - 2.06451607f;
        const auto direction = EncodeOctDirection(slopeX, slopeY);
        // The retained Replay shader decodes its normal from G and A and reads
        // gloss from R (shipped "<n>&<g>"). The IW3 R channel is the normal X
        // slope; start neutral, the caller stores IW3 gloss when it has one.
        source.rgba[offset] = 128;
        source.rgba[offset + 1] = direction[0];
        source.rgba[offset + 2] = 255;
        source.rgba[offset + 3] = direction[1];
    }
    return source;
}

Json ReadJson(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    return Json::parse(input);
}

void WriteJson(const std::filesystem::path &path, const Json &value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot write " + path.string());
    output << value;
    if (!output)
        throw std::runtime_error("cannot finish " + path.string());
}

std::string ResourceText(const unsigned id)
{
    const HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!resource)
        throw std::runtime_error("iw3 converter resource is missing");
    const HGLOBAL loaded = LoadResource(nullptr, resource);
    const auto *data = static_cast<const char *>(LockResource(loaded));
    const DWORD size = SizeofResource(nullptr, resource);
    if (!loaded || !data || !size)
        throw std::runtime_error("iw3 converter resource is empty");
    return std::string(data, data + size);
}

template <class T> T Read(const std::span<const std::uint8_t> data, const std::size_t offset)
{
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("truncated DDS image");
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::uint8_t Expand5(const unsigned value)
{
    return static_cast<std::uint8_t>((value << 3) | (value >> 2));
}

std::uint8_t Expand6(const unsigned value)
{
    return static_cast<std::uint8_t>((value << 2) | (value >> 4));
}

std::array<std::uint8_t, 4> Color565(const std::uint16_t value)
{
    return {Expand5((value >> 11) & 31), Expand6((value >> 5) & 63), Expand5(value & 31), 255};
}

void StorePixel(Image &image, const unsigned x, const unsigned y,
                const std::array<std::uint8_t, 4> &color)
{
    if (x >= image.width || y >= image.height)
        return;
    std::memcpy(image.rgba.data() + (static_cast<std::size_t>(y) * image.width + x) * 4,
                color.data(), 4);
}

void DecodeColorBlock(Image &image, const unsigned blockX, const unsigned blockY,
                      const std::uint8_t *block, const bool dxt1)
{
    std::array<std::array<std::uint8_t, 4>, 4> colors{};
    const std::uint16_t a = static_cast<std::uint16_t>(block[0] | block[1] << 8);
    const std::uint16_t b = static_cast<std::uint16_t>(block[2] | block[3] << 8);
    colors[0] = Color565(a);
    colors[1] = Color565(b);
    if (!dxt1 || a > b)
    {
        for (unsigned channel = 0; channel < 3; ++channel)
        {
            colors[2][channel] =
                static_cast<std::uint8_t>((2 * colors[0][channel] + colors[1][channel]) / 3);
            colors[3][channel] =
                static_cast<std::uint8_t>((colors[0][channel] + 2 * colors[1][channel]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    }
    else
    {
        for (unsigned channel = 0; channel < 3; ++channel)
            colors[2][channel] =
                static_cast<std::uint8_t>((colors[0][channel] + colors[1][channel]) / 2);
        colors[2][3] = 255;
        colors[3] = {0, 0, 0, 0};
    }
    const std::uint32_t indices = Read<std::uint32_t>(std::span(block, 8), 4);
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 4; ++x)
            StorePixel(image, blockX * 4 + x, blockY * 4 + y,
                       colors[(indices >> (2 * (y * 4 + x))) & 3]);
}

std::array<std::uint8_t, 16> DecodeDxt5Alpha(const std::uint8_t *block)
{
    std::array<std::uint8_t, 8> table{};
    table[0] = block[0];
    table[1] = block[1];
    if (table[0] > table[1])
        for (unsigned index = 1; index <= 6; ++index)
            table[index + 1] =
                static_cast<std::uint8_t>(((7 - index) * table[0] + index * table[1]) / 7);
    else
    {
        for (unsigned index = 1; index <= 4; ++index)
            table[index + 1] =
                static_cast<std::uint8_t>(((5 - index) * table[0] + index * table[1]) / 5);
        table[6] = 0;
        table[7] = 255;
    }
    std::uint64_t bits = 0;
    for (unsigned index = 0; index < 6; ++index)
        bits |= static_cast<std::uint64_t>(block[index + 2]) << (index * 8);
    std::array<std::uint8_t, 16> alpha{};
    for (unsigned index = 0; index < 16; ++index)
        alpha[index] = table[(bits >> (index * 3)) & 7];
    return alpha;
}

unsigned MaskShift(const std::uint32_t mask)
{
    if (!mask)
        return 0;
    unsigned shift = 0;
    while (((mask >> shift) & 1) == 0)
        ++shift;
    return shift;
}

unsigned MaskBits(std::uint32_t mask)
{
    unsigned bits = 0;
    while (mask)
    {
        bits += mask & 1;
        mask >>= 1;
    }
    return bits;
}

std::uint8_t ExtractChannel(const std::uint32_t value, const std::uint32_t mask,
                            const std::uint8_t fallback)
{
    if (!mask)
        return fallback;
    const unsigned shift = MaskShift(mask);
    const unsigned bits = MaskBits(mask);
    const std::uint32_t sample = (value & mask) >> shift;
    const std::uint32_t maximum = (1u << bits) - 1u;
    return static_cast<std::uint8_t>((sample * 255u + maximum / 2u) / maximum);
}

void DecodeUncompressed(Image &image, const std::uint8_t *source, const unsigned bits,
                        const std::array<std::uint32_t, 4> &masks)
{
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
        throw std::runtime_error("unsupported uncompressed DDS pixels");

    const unsigned stride = bits / 8;
    const bool luminance = !masks[1] && !masks[2] && (masks[0] || bits == 8);
    for (std::size_t index = 0; index < static_cast<std::size_t>(image.width) * image.height;
         ++index)
    {
        std::uint32_t value = 0;
        std::memcpy(&value, source + index * stride, stride);
        auto *pixel = image.rgba.data() + index * 4;
        if (luminance)
        {
            const std::uint8_t sample =
                masks[0] ? ExtractChannel(value, masks[0], 0) : static_cast<std::uint8_t>(value);
            pixel[0] = pixel[1] = pixel[2] = sample;
            pixel[3] = ExtractChannel(value, masks[3], 255);
            continue;
        }
        for (unsigned channel = 0; channel < 4; ++channel)
            pixel[channel] = ExtractChannel(value, masks[channel], channel == 3 ? 255 : 0);
    }
}

std::size_t DdsLevelSize(const unsigned width, const unsigned height, const std::uint32_t fourCC,
                         const unsigned bits)
{
    if (fourCC == 0x31545844u)
        return static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * 8;
    if (fourCC == 0x33545844u || fourCC == 0x35545844u)
        return static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * 16;
    return (static_cast<std::size_t>(width) * height * bits + 7) / 8;
}

Image DecodeDdsLevel(std::span<const std::uint8_t> source, unsigned width, unsigned height,
                     std::uint32_t fourCC, unsigned bits,
                     const std::array<std::uint32_t, 4> &masks);

std::vector<Image> DecodeDds(const std::filesystem::path &path)
{
    const auto bytes = ReadBytes(path);
    const std::span<const std::uint8_t> data(bytes);
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0 ||
        Read<std::uint32_t>(data, 4) != 124)
        throw std::runtime_error("invalid DDS image " + path.string());
    const unsigned height = Read<std::uint32_t>(data, 12);
    const unsigned width = Read<std::uint32_t>(data, 16);
    const unsigned mipCount = std::max(1u, Read<std::uint32_t>(data, 28));
    const std::uint32_t fourCC = Read<std::uint32_t>(data, 84);
    const unsigned bits = Read<std::uint32_t>(data, 88);
    const std::array<std::uint32_t, 4> masks{
        Read<std::uint32_t>(data, 92), Read<std::uint32_t>(data, 96),
        Read<std::uint32_t>(data, 100), Read<std::uint32_t>(data, 104)};
    const std::uint32_t caps2 = Read<std::uint32_t>(data, 112);
    if (!width || !height || width > 8192 || height > 8192 || mipCount > 14 ||
        (fourCC && fourCC != 0x31545844u && fourCC != 0x33545844u && fourCC != 0x35545844u))
        throw std::runtime_error("unsupported DDS image " + path.string());
    const unsigned faces = (caps2 & 0x200u) ? 6u : 1u;
    if (faces == 6 && (caps2 & 0xFC00u) != 0xFC00u)
        throw std::runtime_error("incomplete DDS cubemap " + path.string());

    std::size_t offset = 128;
    std::vector<Image> result;
    result.reserve(faces);
    for (unsigned face = 0; face < faces; ++face)
    {
        const std::size_t topSize = DdsLevelSize(width, height, fourCC, bits);
        if (offset > bytes.size() || topSize > bytes.size() - offset)
            throw std::runtime_error("truncated DDS pixels " + path.string());
        Image image{width, height,
                    std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        const std::uint8_t *source = bytes.data() + offset;
        if (fourCC)
        {
            const unsigned blockSize = fourCC == 0x31545844u ? 8u : 16u;
            for (unsigned by = 0; by < (height + 3) / 4; ++by)
                for (unsigned bx = 0; bx < (width + 3) / 4; ++bx)
                {
                    const std::uint8_t *block =
                        source +
                        (static_cast<std::size_t>(by) * ((width + 3) / 4) + bx) * blockSize;
                    DecodeColorBlock(image, bx, by, block + (blockSize - 8), fourCC == 0x31545844u);
                    if (fourCC == 0x33545844u)
                    {
                        std::uint64_t alpha = Read<std::uint64_t>(std::span(block, 16), 0);
                        for (unsigned index = 0; index < 16; ++index)
                        {
                            const unsigned x = bx * 4 + index % 4, y = by * 4 + index / 4;
                            if (x < width && y < height)
                                image.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] =
                                    static_cast<std::uint8_t>(((alpha >> (index * 4)) & 15) * 17);
                        }
                    }
                    else if (fourCC == 0x35545844u)
                    {
                        const auto alpha = DecodeDxt5Alpha(block);
                        for (unsigned index = 0; index < 16; ++index)
                        {
                            const unsigned x = bx * 4 + index % 4, y = by * 4 + index / 4;
                            if (x < width && y < height)
                                image.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] =
                                    alpha[index];
                        }
                    }
                }
        }
        else
        {
            try
            {
                DecodeUncompressed(image, source, bits, masks);
            }
            catch (const std::runtime_error &)
            {
                throw std::runtime_error("unsupported uncompressed DDS image " + path.string());
            }
        }
        for (unsigned level = 0, w = width, h = height; level < mipCount; ++level)
        {
            if (level && w == 1 && h == 1 && !image.mips.empty() &&
                image.mips.back().width == 1 && image.mips.back().height == 1)
                throw std::runtime_error("DDS has too many mip levels " + path.string());
            const std::size_t size = DdsLevelSize(w, h, fourCC, bits);
            if (offset > bytes.size() || size > bytes.size() - offset)
                throw std::runtime_error("truncated DDS mip pixels " + path.string());
            if (level)
                image.mips.push_back(DecodeDdsLevel(data.subspan(offset, size), w, h,
                                                    fourCC, bits, masks));
            offset += size;
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        result.push_back(std::move(image));
    }
    return result;
}

Image DecodeDdsLevel(const std::span<const std::uint8_t> source, const unsigned width,
                     const unsigned height, const std::uint32_t fourCC, const unsigned bits,
                     const std::array<std::uint32_t, 4> &masks)
{
    const std::size_t required = DdsLevelSize(width, height, fourCC, bits);
    if (source.size() != required)
        throw std::runtime_error("invalid DDS mip payload");
    Image image{width, height,
                std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    if (fourCC)
    {
        const unsigned blockSize = fourCC == 0x31545844u ? 8u : 16u;
        for (unsigned blockY = 0; blockY < (height + 3) / 4; ++blockY)
            for (unsigned blockX = 0; blockX < (width + 3) / 4; ++blockX)
            {
                const auto *block =
                    source.data() +
                    (static_cast<std::size_t>(blockY) * ((width + 3) / 4) + blockX) * blockSize;
                DecodeColorBlock(image, blockX, blockY, block + (blockSize - 8),
                                 fourCC == 0x31545844u);
                if (fourCC == 0x33545844u)
                {
                    const std::uint64_t alpha = Read<std::uint64_t>(std::span(block, blockSize), 0);
                    for (unsigned index = 0; index < 16; ++index)
                    {
                        const unsigned x = blockX * 4 + index % 4;
                        const unsigned y = blockY * 4 + index / 4;
                        if (x < width && y < height)
                            image.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] =
                                static_cast<std::uint8_t>(((alpha >> (index * 4)) & 15) * 17);
                    }
                }
                else if (fourCC == 0x35545844u)
                {
                    const auto alpha = DecodeDxt5Alpha(block);
                    for (unsigned index = 0; index < 16; ++index)
                    {
                        const unsigned x = blockX * 4 + index % 4;
                        const unsigned y = blockY * 4 + index / 4;
                        if (x < width && y < height)
                            image.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] =
                                alpha[index];
                    }
                }
            }
    }
    else
    {
        DecodeUncompressed(image, source.data(), bits, masks);
    }
    return image;
}

Cubemap DecodeDdsCubemap(const std::filesystem::path &path)
{
    const auto bytes = ReadBytes(path);
    const std::span<const std::uint8_t> data(bytes);
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0 ||
        Read<std::uint32_t>(data, 4) != 124)
        throw std::runtime_error("invalid reflection-probe DDS " + path.string());
    const unsigned height = Read<std::uint32_t>(data, 12);
    const unsigned width = Read<std::uint32_t>(data, 16);
    const unsigned mipCount = std::max(1u, Read<std::uint32_t>(data, 28));
    const std::uint32_t fourCC = Read<std::uint32_t>(data, 84);
    const unsigned bits = Read<std::uint32_t>(data, 88);
    const std::array<std::uint32_t, 4> masks{
        Read<std::uint32_t>(data, 92), Read<std::uint32_t>(data, 96),
        Read<std::uint32_t>(data, 100), Read<std::uint32_t>(data, 104)};
    const std::uint32_t caps2 = Read<std::uint32_t>(data, 112);
    if (!width || width != height || width > 4096 || mipCount > 13 ||
        (caps2 & 0xFE00u) != 0xFE00u ||
        (fourCC && fourCC != 0x31545844u && fourCC != 0x33545844u && fourCC != 0x35545844u))
        throw std::runtime_error("unsupported reflection-probe DDS " + path.string());
    unsigned terminalWidth = width;
    unsigned terminalHeight = height;
    for (unsigned level = 1; level < mipCount; ++level)
    {
        if (terminalWidth == 1 && terminalHeight == 1)
            throw std::runtime_error("reflection-probe DDS has too many mips " + path.string());
        terminalWidth = std::max(1u, terminalWidth / 2);
        terminalHeight = std::max(1u, terminalHeight / 2);
    }

    Cubemap result{width, height, mipCount, std::vector<std::vector<Image>>(6), {}};
    for (auto &face : result.faces)
        face.reserve(mipCount);
    std::size_t offset = 128;
    for (unsigned face = 0; face < 6; ++face)
    {
        unsigned mipWidth = width;
        unsigned mipHeight = height;
        for (unsigned level = 0; level < mipCount; ++level)
        {
            const std::size_t size = DdsLevelSize(mipWidth, mipHeight, fourCC, bits);
            if (offset > bytes.size() || size > bytes.size() - offset)
                throw std::runtime_error("truncated reflection-probe DDS " + path.string());
            result.faces[face].push_back(DecodeDdsLevel(data.subspan(offset, size), mipWidth,
                                                        mipHeight, fourCC, bits, masks));
            offset += size;
            mipWidth = std::max(1u, mipWidth / 2);
            mipHeight = std::max(1u, mipHeight / 2);
        }
    }
    if (offset != bytes.size())
        throw std::runtime_error("reflection-probe DDS contains trailing data " + path.string());
    // IW3 reflection captures are stored as RGBA bytes, but the unlinker labels
    // them A8R8G8B8 (red mask 0xFF0000). Nuketown's sky faces read (151,223,244)
    // as RGB and peach as BGR; honour the byte order the capture actually used.
    if (!fourCC && bits == 32 && masks[0] == 0x00FF0000u && masks[2] == 0x000000FFu)
        for (auto &face : result.faces)
            for (auto &image : face)
                for (std::size_t pixel = 0; pixel < image.rgba.size(); pixel += 4)
                    std::swap(image.rgba[pixel], image.rgba[pixel + 2]);
    for (unsigned level = 0; level < mipCount; ++level)
        for (unsigned face = 0; face < 6; ++face)
        {
            result.residentPixels.insert(result.residentPixels.end(),
                                         result.faces[face][level].rgba.begin(),
                                         result.faces[face][level].rgba.end());
            result.residentPixels.resize((result.residentPixels.size() + 15) & ~std::size_t{15});
        }
    return result;
}

std::vector<Image> DecodeIwi(const std::filesystem::path &path)
{
    const auto bytes = ReadBytes(path);
    const std::span<const std::uint8_t> data(bytes);
    if (bytes.size() < 28 || std::memcmp(bytes.data(), "IWi\x06", 4) != 0)
        throw std::runtime_error("invalid IW3 IWI image " + path.string());
    const unsigned format = bytes[4], flags = bytes[5];
    const unsigned width = Read<std::uint16_t>(data, 6), height = Read<std::uint16_t>(data, 8);
    const unsigned depth = Read<std::uint16_t>(data, 10);
    if (!width || !height || width > 4096 || height > 4096 || (flags & 8) || depth > 1 ||
        format < 1 || format > 13 || (format >= 6 && format <= 10))
        throw std::runtime_error("unsupported IW3 IWI image " + path.string());
    const unsigned faces = flags & 4 ? 6u : 1u;
    const unsigned bytesPerPixel = format == 1 ? 4u : format == 2 ? 3u : format == 3 ? 2u : 1u;
    const std::uint32_t fourCC = format == 11   ? 0x31545844u
                                 : format == 12 ? 0x33545844u
                                 : format == 13 ? 0x35545844u
                                                : 0u;
    const std::size_t faceSize = fourCC ? DdsLevelSize(width, height, fourCC, 0)
                                        : static_cast<std::size_t>(width) * height * bytesPerPixel;
    if (faceSize > bytes.size() || faceSize * faces > bytes.size() - 28 ||
        Read<std::uint32_t>(data, 12) != bytes.size())
        throw std::runtime_error("truncated IW3 IWI image " + path.string());
    const std::size_t start = bytes.size() - faceSize * faces;
    if ((flags & 2u) && start != 28)
        throw std::runtime_error("unexpected IW3 IWI mip payload " + path.string());
    std::vector<Image> result;
    result.reserve(faces);
    for (unsigned face = 0; face < faces; ++face)
    {
        const std::uint8_t *source = bytes.data() + start + face * faceSize;
        Image image{width, height,
                    std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        if (fourCC)
        {
            const unsigned blockSize = fourCC == 0x31545844u ? 8u : 16u;
            for (unsigned by = 0; by < (height + 3) / 4; ++by)
                for (unsigned bx = 0; bx < (width + 3) / 4; ++bx)
                {
                    const std::uint8_t *block =
                        source +
                        (static_cast<std::size_t>(by) * ((width + 3) / 4) + bx) * blockSize;
                    DecodeColorBlock(image, bx, by, block + (blockSize - 8), fourCC == 0x31545844u);
                    if (fourCC == 0x33545844u)
                    {
                        const std::uint64_t alpha = Read<std::uint64_t>(std::span(block, 16), 0);
                        for (unsigned index = 0; index < 16; ++index)
                        {
                            const unsigned x = bx * 4 + index % 4, y = by * 4 + index / 4;
                            if (x < width && y < height)
                                image.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] =
                                    static_cast<std::uint8_t>(((alpha >> (index * 4)) & 15) * 17);
                        }
                    }
                    else if (fourCC == 0x35545844u)
                    {
                        const auto alpha = DecodeDxt5Alpha(block);
                        for (unsigned index = 0; index < 16; ++index)
                        {
                            const unsigned x = bx * 4 + index % 4, y = by * 4 + index / 4;
                            if (x < width && y < height)
                                image.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + 3] =
                                    alpha[index];
                        }
                    }
                }
        }
        else
        {
            for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height; ++index)
            {
                const std::uint8_t *pixel = source + index * bytesPerPixel;
                auto *target = image.rgba.data() + index * 4;
                if (format == 1)
                    target[0] = pixel[2], target[1] = pixel[1], target[2] = pixel[0],
                    target[3] = pixel[3];
                else if (format == 2)
                    target[0] = pixel[2], target[1] = pixel[1], target[2] = pixel[0],
                    target[3] = 255;
                else if (format == 3)
                    target[0] = target[1] = target[2] = pixel[0], target[3] = pixel[1];
                else if (format == 4)
                    target[0] = target[1] = target[2] = pixel[0], target[3] = 255;
                else
                    target[0] = target[1] = target[2] = 255, target[3] = pixel[0];
            }
        }
        result.push_back(std::move(image));
    }
    // IW3 stores its mip levels smallest first. Preserve the authored levels
    // for direct Replay materials instead of averaging the top level again;
    // thin alpha details such as shattered glass otherwise disappear in mips.
    if (faces == 1 && !(flags & 2u))
    {
        std::size_t offset = start;
        unsigned mipWidth = width, mipHeight = height;
        while (mipWidth > 1 || mipHeight > 1)
        {
            mipWidth = std::max(1u, mipWidth / 2);
            mipHeight = std::max(1u, mipHeight / 2);
            const std::size_t size =
                fourCC ? DdsLevelSize(mipWidth, mipHeight, fourCC, 0)
                       : static_cast<std::size_t>(mipWidth) * mipHeight * bytesPerPixel;
            if (offset < 28 || size > offset - 28)
                throw std::runtime_error("truncated IW3 IWI mip chain " + path.string());
            offset -= size;
            if (fourCC)
            {
                result.front().mips.push_back(DecodeDdsLevel(data.subspan(offset, size),
                                                              mipWidth, mipHeight, fourCC, 0,
                                                              {0, 0, 0, 0}));
                continue;
            }
            Image mip{mipWidth, mipHeight,
                      std::vector<std::uint8_t>(static_cast<std::size_t>(mipWidth) *
                                                mipHeight * 4)};
            for (std::size_t index = 0; index <
                                        static_cast<std::size_t>(mipWidth) * mipHeight;
                 ++index)
            {
                const auto *pixel = bytes.data() + offset + index * bytesPerPixel;
                auto *target = mip.rgba.data() + index * 4;
                if (format == 1)
                    target[0] = pixel[2], target[1] = pixel[1], target[2] = pixel[0],
                    target[3] = pixel[3];
                else if (format == 2)
                    target[0] = pixel[2], target[1] = pixel[1], target[2] = pixel[0],
                    target[3] = 255;
                else if (format == 3)
                    target[0] = target[1] = target[2] = pixel[0], target[3] = pixel[1];
                else if (format == 4)
                    target[0] = target[1] = target[2] = pixel[0], target[3] = 255;
                else
                    target[0] = target[1] = target[2] = 255, target[3] = pixel[0];
            }
            result.front().mips.push_back(std::move(mip));
        }
        if (offset != 28)
            throw std::runtime_error("invalid IW3 IWI mip layout " + path.string());
    }
    return result;
}

Image SolidImage(const std::array<std::uint8_t, 4> color)
{
    return {1, 1, {color[0], color[1], color[2], color[3]}};
}

std::vector<Image> LoadImage(const std::filesystem::path &root,
                             const std::vector<std::filesystem::path> &sourcePaths,
                             const std::string &name)
{
    if (name == "$white")
        return {SolidImage({255, 255, 255, 255})};
    if (name == "$black")
        return {SolidImage({0, 0, 0, 255})};
    if (name == "$identitynormalmap")
        return {SolidImage({128, 128, 255, 255})};
    const auto path = root / "images" / (name + ".dds");
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error))
        return DecodeDds(path);
    // Extracted from a map-local IWD.
    if (const auto iwi = root / "images" / (name + ".iwi");
        std::filesystem::is_regular_file(iwi, error))
        return DecodeIwi(iwi);
    for (const auto &source : sourcePaths)
    {
        for (const auto &candidate :
             {source / "images" / (name + ".dds"), source / "raw" / "images" / (name + ".dds")})
            if (std::filesystem::is_regular_file(candidate, error))
                return DecodeDds(candidate);
        for (const auto &candidate :
             {source / "images" / (name + ".iwi"), source / "raw" / "images" / (name + ".iwi")})
            if (std::filesystem::is_regular_file(candidate, error))
                return DecodeIwi(candidate);
    }
    // Composite images the IW3 converter generates (`~<name>~<hash>`) change
    // hash on every recompile; a map IWD packed from another compile carries
    // the same image under a different hash.
    if (const auto tilde = name.rfind('~'); name.starts_with('~') && tilde != std::string::npos &&
                                            tilde > 0 && name.size() - tilde == 9)
    {
        const std::string prefix = name.substr(0, tilde + 1);
        std::vector<std::filesystem::path> directories{root / "images"};
        for (const auto &source : sourcePaths)
        {
            directories.push_back(source / "images");
            directories.push_back(source / "raw" / "images");
        }
        for (const auto &directory : directories)
        {
            if (!std::filesystem::is_directory(directory, error))
                continue;
            for (const auto &entry : std::filesystem::directory_iterator(directory, error))
            {
                const auto file = entry.path().filename().string();
                const auto extension = entry.path().extension().string();
                if (!file.starts_with(prefix) || entry.path().stem().string().size() != name.size())
                    continue;
                if (extension == ".dds" || extension == ".iwi")
                {
                    zt::warn("iw3: image '%s' is missing; using '%s' from another compile",
                             name.c_str(), file.c_str());
                    return extension == ".dds" ? DecodeDds(entry.path()) : DecodeIwi(entry.path());
                }
            }
        }
    }
    throw std::runtime_error("missing IW3 image '" + name +
                             "'; add the matching IW3 main and raw directories with --search-path");
}

float Linear(const float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float Srgb(const float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// Texture resampling decodes and re-encodes 8-bit sRGB for every texel; use
// exact tables instead of pow(). A byte b holds sRGB [(b-0.5)/255, (b+0.5)/255),
// so the 255 linear thresholds reproduce lround(Srgb(v) * 255) exactly.
float LinearByte(const std::uint8_t value)
{
    static const auto table = [] {
        std::array<float, 256> result{};
        for (unsigned index = 0; index < result.size(); ++index)
            result[index] = Linear(static_cast<float>(index) / 255.0f);
        return result;
    }();
    return table[value];
}

std::uint8_t SrgbByte(const float linear)
{
    static const auto thresholds = [] {
        std::array<float, 255> result{};
        for (unsigned index = 0; index < result.size(); ++index)
            result[index] = Linear((static_cast<float>(index) + 0.5f) / 255.0f);
        return result;
    }();
    return static_cast<std::uint8_t>(
        std::upper_bound(thresholds.begin(), thresholds.end(), linear) - thresholds.begin());
}

std::array<std::array<float, 9>, 4> ProjectReflectionSh(const Cubemap &cubemap)
{
    if (cubemap.faces.size() != 6 || cubemap.faces.front().empty())
        throw std::runtime_error("reflection probe has no cubemap faces");
    std::array<std::array<double, 9>, 4> accumulated{};
    double totalWeight = 0.0;
    for (unsigned face = 0; face < 6; ++face)
    {
        const Image &image = cubemap.faces[face].front();
        if (image.width != cubemap.width || image.height != cubemap.height)
            throw std::runtime_error("reflection-probe cubemap faces do not match");
        for (unsigned y = 0; y < image.height; ++y)
            for (unsigned x = 0; x < image.width; ++x)
            {
                const double u = 2.0 * (static_cast<double>(x) + 0.5) / image.width - 1.0;
                const double v = 2.0 * (static_cast<double>(y) + 0.5) / image.height - 1.0;
                std::array<double, 3> direction{};
                switch (face)
                {
                case 0:
                    direction = {1.0, -v, -u};
                    break;
                case 1:
                    direction = {-1.0, -v, u};
                    break;
                case 2:
                    direction = {u, 1.0, v};
                    break;
                case 3:
                    direction = {u, -1.0, -v};
                    break;
                case 4:
                    direction = {u, -v, 1.0};
                    break;
                default:
                    direction = {-u, -v, -1.0};
                    break;
                }
                const double length =
                    std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                              direction[2] * direction[2]);
                for (double &component : direction)
                    component /= length;
                const double dx = direction[0], dy = direction[1], dz = direction[2];
                const std::array<double, 9> basis{0.282095,
                                                  0.488603 * dy,
                                                  0.488603 * dz,
                                                  0.488603 * dx,
                                                  1.092548 * dx * dy,
                                                  1.092548 * dy * dz,
                                                  0.315392 * (3.0 * dz * dz - 1.0),
                                                  1.092548 * dx * dz,
                                                  0.546274 * (dx * dx - dy * dy)};
                const double weight = 1.0 / std::pow(1.0 + u * u + v * v, 1.5);
                const std::size_t offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
                const std::array<double, 4> sample{Linear(image.rgba[offset + 0] / 255.0f),
                                                   Linear(image.rgba[offset + 1] / 255.0f),
                                                   Linear(image.rgba[offset + 2] / 255.0f),
                                                   image.rgba[offset + 3] / 255.0};
                for (unsigned channel = 0; channel < 4; ++channel)
                    for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
                        accumulated[channel][coefficient] +=
                            sample[channel] * basis[coefficient] * weight;
                totalWeight += weight;
            }
    }
    if (!(totalWeight > 0.0) || !std::isfinite(totalWeight))
        throw std::runtime_error("reflection-probe SH projection has invalid weight");
    constexpr double sphereArea = 12.56637061435917295385;
    std::array<std::array<float, 9>, 4> result{};
    for (unsigned channel = 0; channel < 4; ++channel)
        for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
        {
            const double value = accumulated[channel][coefficient] * sphereArea / totalWeight;
            if (!std::isfinite(value))
                throw std::runtime_error("reflection-probe SH projection is non-finite");
            result[channel][coefficient] = static_cast<float>(value);
        }
    return result;
}

std::array<float, 3> SampleCubemapLinear(const Cubemap &cubemap, const unsigned level,
                                        const std::array<float, 3> &direction)
{
    if (cubemap.faces.size() != 6 || level >= cubemap.mipCount)
        throw std::runtime_error("invalid reflection-probe cubemap level");
    const Image &first = cubemap.faces.front().at(level);
    if (!first.width || first.width != first.height)
        throw std::runtime_error("reflection-probe cubemap level is not square");
    for (const auto &face : cubemap.faces)
        if (face.size() <= level || face[level].width != first.width ||
            face[level].height != first.height)
            throw std::runtime_error("reflection-probe cubemap level dimensions differ");

    const float ax = std::abs(direction[0]);
    const float ay = std::abs(direction[1]);
    const float az = std::abs(direction[2]);
    unsigned faceIndex = 0;
    float u = 0.0f, v = 0.0f;
    if (ax >= ay && ax >= az)
    {
        if (direction[0] >= 0.0f)
            faceIndex = 0, u = -direction[2] / ax, v = -direction[1] / ax;
        else
            faceIndex = 1, u = direction[2] / ax, v = -direction[1] / ax;
    }
    else if (ay >= ax && ay >= az)
    {
        if (direction[1] >= 0.0f)
            faceIndex = 2, u = direction[0] / ay, v = direction[2] / ay;
        else
            faceIndex = 3, u = direction[0] / ay, v = -direction[2] / ay;
    }
    else
    {
        if (direction[2] >= 0.0f)
            faceIndex = 4, u = direction[0] / az, v = -direction[1] / az;
        else
            faceIndex = 5, u = -direction[0] / az, v = -direction[1] / az;
    }

    const Image &face = cubemap.faces[faceIndex][level];
    const float sourceX = (u * 0.5f + 0.5f) * face.width - 0.5f;
    const float sourceY = (v * 0.5f + 0.5f) * face.height - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(sourceX)), 0,
                              static_cast<int>(face.width) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(sourceY)), 0,
                              static_cast<int>(face.height) - 1);
    const int x1 = std::min(x0 + 1, static_cast<int>(face.width) - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(face.height) - 1);
    const float fx = std::clamp(sourceX - std::floor(sourceX), 0.0f, 1.0f);
    const float fy = std::clamp(sourceY - std::floor(sourceY), 0.0f, 1.0f);
    std::array<float, 3> result{};
    for (unsigned iy = 0; iy < 2; ++iy)
        for (unsigned ix = 0; ix < 2; ++ix)
        {
            const float weight = (ix ? fx : 1.0f - fx) * (iy ? fy : 1.0f - fy);
            const auto offset =
                (static_cast<std::size_t>(iy ? y1 : y0) * face.width + (ix ? x1 : x0)) * 4;
            for (unsigned channel = 0; channel < result.size(); ++channel)
                result[channel] += Linear(face.rgba[offset + channel] / 255.0f) * weight;
        }
    return result;
}

std::vector<std::uint8_t> BuildReflectionProbeArray(const std::vector<Cubemap> &cubemaps,
                                                    const unsigned width,
                                                    const unsigned mipCount)
{
    if (cubemaps.empty() || width < 4 || (width & (width - 1)) || !mipCount)
        throw std::runtime_error("reflection-probe array has no images");
    const std::size_t taskCount = static_cast<std::size_t>(mipCount) * cubemaps.size();
    std::vector<std::vector<std::uint8_t>> slices(taskCount);
    std::atomic_size_t nextTask{};
    std::exception_ptr failure;
    std::mutex failureMutex;
    const auto encode = [&]()
    {
        try
        {
            while (true)
            {
                const std::size_t task = nextTask.fetch_add(1);
                if (task >= taskCount)
                    return;
                const unsigned level = static_cast<unsigned>(task / cubemaps.size());
                const auto &cubemap = cubemaps[task % cubemaps.size()];
                const unsigned targetWidth = std::max(1u, width >> level);
                if (targetWidth < 4 || targetWidth % 4)
                    throw std::runtime_error("reflection-probe array mip is not BC6H block aligned");
                const unsigned sourceLevel =
                    std::min(level ? level - 1 : 0, cubemap.mipCount - 1);
                std::vector<float> image(static_cast<std::size_t>(targetWidth) * targetWidth * 4);
                for (unsigned y = 0; y < targetWidth; ++y)
                    for (unsigned x = 0; x < targetWidth; ++x)
                    {
                        const float ox =
                            2.0f * (static_cast<float>(x) + 0.5f) / targetWidth - 1.0f;
                        const float oy =
                            2.0f * (static_cast<float>(y) + 0.5f) / targetWidth - 1.0f;
                        std::array<float, 3> direction{
                            ox, oy, 1.0f - std::abs(ox) - std::abs(oy)};
                        if (direction[2] < 0.0f)
                        {
                            const auto sign = [](const float value) {
                                return value < 0.0f ? -1.0f : 1.0f;
                            };
                            const float oldX = direction[0];
                            direction[0] = (1.0f - std::abs(direction[1])) * sign(oldX);
                            direction[1] = (1.0f - std::abs(oldX)) * sign(direction[1]);
                        }
                        const float length =
                            std::sqrt(direction[0] * direction[0] +
                                      direction[1] * direction[1] +
                                      direction[2] * direction[2]);
                        for (float &component : direction)
                            component /= length;
                        const auto sample =
                            SampleCubemapLinear(cubemap, sourceLevel, direction);
                        const std::size_t pixel =
                            (static_cast<std::size_t>(y) * targetWidth + x) * 4;
                        for (unsigned channel = 0; channel < sample.size(); ++channel)
                            image[pixel + channel] = sample[channel];
                        image[pixel + 3] = 1.0f;
                    }
                const std::size_t expected =
                    static_cast<std::size_t>(targetWidth / 4) * (targetWidth / 4) * 16;
                auto &compressed = slices[task];
                compressed.resize(expected);
                DirectX::XMVECTOR block[16];
                for (unsigned blockY = 0; blockY < targetWidth / 4; ++blockY)
                    for (unsigned blockX = 0; blockX < targetWidth / 4; ++blockX)
                    {
                        for (unsigned y = 0; y < 4; ++y)
                            for (unsigned x = 0; x < 4; ++x)
                            {
                                const std::size_t pixel =
                                    (static_cast<std::size_t>(blockY * 4 + y) * targetWidth +
                                     blockX * 4 + x) *
                                    4;
                                block[y * 4 + x] =
                                    DirectX::XMVectorSet(image[pixel], image[pixel + 1],
                                                         image[pixel + 2], 1.0f);
                            }
                        const std::size_t target =
                            (static_cast<std::size_t>(blockY) * (targetWidth / 4) + blockX) *
                            16;
                        DirectX::D3DXEncodeBC6HU(compressed.data() + target, block,
                                                 DirectX::BC_FLAGS_NONE);
                    }
            }
        }
        catch (...)
        {
            std::lock_guard lock(failureMutex);
            if (!failure)
                failure = std::current_exception();
            nextTask.store(taskCount);
        }
    };
    const unsigned workerCount =
        std::max(1u, std::min<unsigned>(static_cast<unsigned>(taskCount),
                                       std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned worker = 0; worker < workerCount; ++worker)
        workers.emplace_back(encode);
    for (auto &worker : workers)
        worker.join();
    if (failure)
        std::rethrow_exception(failure);

    std::vector<std::uint8_t> result;
    for (const auto &slice : slices)
    {
        result.insert(result.end(), slice.begin(), slice.end());
        result.resize((result.size() + 15) & ~std::size_t{15});
    }
    return result;
}

Image Resize(const Image &source, const unsigned width, const unsigned height, const bool color,
             const std::array<float, 4> &tint = {1, 1, 1, 1})
{
    Image output{width, height,
                 std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    for (unsigned y = 0; y < height; ++y)
    {
        const float sy = (static_cast<float>(y) + 0.5f) * source.height / height - 0.5f;
        const int sy0 = static_cast<int>(std::floor(sy));
        const int y0 =
            std::clamp(sy0, 0, static_cast<int>(source.height) - 1);
        const int y1 = std::clamp(sy0 + 1, 0, static_cast<int>(source.height) - 1);
        const float fy = sy - sy0;
        for (unsigned x = 0; x < width; ++x)
        {
            const float sx = (static_cast<float>(x) + 0.5f) * source.width / width - 0.5f;
            const int sx0 = static_cast<int>(std::floor(sx));
            const int x0 =
                std::clamp(sx0, 0, static_cast<int>(source.width) - 1);
            const int x1 = std::clamp(sx0 + 1, 0, static_cast<int>(source.width) - 1);
            const float fx = sx - sx0;
            float sample[4]{};
            for (unsigned iy = 0; iy < 2; ++iy)
                for (unsigned ix = 0; ix < 2; ++ix)
                {
                    const float weight = (ix ? fx : 1 - fx) * (iy ? fy : 1 - fy);
                    const std::size_t offset =
                        (static_cast<std::size_t>(iy ? y1 : y0) * source.width +
                         static_cast<unsigned>(ix ? x1 : x0)) *
                        4;
                    const float alpha = source.rgba[offset + 3] / 255.0f;
                    for (unsigned channel = 0; channel < 4; ++channel)
                    {
                        const float value = color && channel < 3
                                                ? LinearByte(source.rgba[offset + channel]) * alpha
                                                : source.rgba[offset + channel] / 255.0f;
                        sample[channel] += value * weight;
                    }
                }
            const float alpha = std::clamp(sample[3] * tint[3], 0.0f, 1.0f);
            for (unsigned channel = 0; channel < 4; ++channel)
            {
                auto &target = output.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + channel];
                const float value = sample[channel];
                if (color && channel < 3)
                    target = SrgbByte(std::clamp(
                        (sample[3] > 1.0e-6f ? value / sample[3] : 0.0f) * tint[channel], 0.0f,
                        1.0f));
                else
                    target = static_cast<std::uint8_t>(std::clamp(
                        std::lround((channel == 3 ? alpha : value) * 255.0f), 0l, 255l));
            }
        }
    }
    return output;
}

void Paste(Image &target, const Image &source, const unsigned x, const unsigned y)
{
    if (x > target.width || y > target.height || source.width > target.width - x ||
        source.height > target.height - y)
        throw std::runtime_error("IW3 atlas write exceeds its dimensions");
    for (unsigned row = 0; row < source.height; ++row)
        std::memcpy(target.rgba.data() + (static_cast<std::size_t>(y + row) * target.width + x) * 4,
                    source.rgba.data() + static_cast<std::size_t>(row) * source.width * 4,
                    static_cast<std::size_t>(source.width) * 4);
}

template <std::size_t Size>
std::array<float, Size> Literal(const Json &value, const std::array<float, Size> &fallback)
{
    if (!value.is_array() || value.size() != Size)
        return fallback;
    std::array<float, Size> result{};
    for (std::size_t index = 0; index < Size; ++index)
    {
        result[index] = value.at(index).get<float>();
        if (!std::isfinite(result[index]) || result[index] < 0 || result[index] > 64)
            throw std::runtime_error("invalid IW3 material constant");
    }
    return result;
}

std::filesystem::path FindMaterial(const std::filesystem::path &root,
                                   const std::vector<std::filesystem::path> &sourcePaths,
                                   const std::string &name)
{
    if (name.empty() || name.find("..") != std::string::npos ||
        name.find(':') != std::string::npos || name.find('\\') != std::string::npos ||
        name.front() == '/')
        throw std::runtime_error("invalid IW3 material name");
    std::vector<std::filesystem::path> relativePaths{std::filesystem::path("materials") /
                                                     (name + ".json")};
    if (name.starts_with("mc/"))
        relativePaths.push_back(std::filesystem::path("materials") / (name.substr(3) + ".json"));

    std::error_code error;
    std::vector<std::filesystem::path> roots{root};
    roots.insert(roots.end(), sourcePaths.begin(), sourcePaths.end());
    for (const auto &source : roots)
        for (const auto &relative : relativePaths)
        {
            const auto candidate = source / relative;
            if (std::filesystem::is_regular_file(candidate, error))
                return candidate;
            error.clear();
        }
    return {};
}

std::array<float, 4> FxLiteral(const Json &value, const char *field)
{
    if (!value.is_array() || value.size() != 4)
        throw std::runtime_error(std::string("invalid IW3 FX material ") + field);
    std::array<float, 4> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const double component = value.at(index).get<double>();
        if (!std::isfinite(component) || std::abs(component) > 1000000.0)
            throw std::runtime_error(std::string("invalid IW3 FX material ") + field);
        result[index] = static_cast<float>(component);
    }
    return result;
}

SourceFxMaterial ReadFxMaterial(const std::filesystem::path &root,
                                const std::vector<std::filesystem::path> &sourcePaths,
                                const std::string &name)
{
    const auto path = FindMaterial(root, sourcePaths, name);
    if (path.empty())
        throw std::runtime_error("missing IW3 FX material '" + name +
                                 "'; add the matching IW3 main and raw directories with "
                                 "--search-path");
    const Json source = ReadJson(path);
    SourceFxMaterial material;
    material.name = name;
    material.techniqueSet = source.at("techniqueSet").get<std::string>();
    const bool noFogAlias = material.techniqueSet.ends_with("_nofog");
    if (material.techniqueSet == "effect" || material.techniqueSet == "effect_nofog")
        material.family = FxMaterialFamily::alpha;
    else if (material.techniqueSet == "effect_zfeather" ||
        material.techniqueSet == "effect_zfeather_nofog")
        material.family = FxMaterialFamily::alphaFeather;
    else if (material.techniqueSet == "effect_zfeather_add" ||
             material.techniqueSet == "effect_zfeather_add_nofog")
        material.family = FxMaterialFamily::additiveFeather;
    // particle_cloud_add is an additive cloud; Replay draws it as an additive sprite.
    // The view-angle falloff and eye offset of the other additive variants have
    // no Replay counterpart; they draw as plain additive sprites.
    else if (material.techniqueSet == "effect_add_nofog" || material.techniqueSet == "effect_add" ||
             material.techniqueSet == "particle_cloud_add" ||
             material.techniqueSet == "particle_cloud_outdoor_add" ||
             material.techniqueSet == "effect_falloff_add" ||
             material.techniqueSet == "effect_falloff_add_nofog" ||
             material.techniqueSet == "effect_add_eyeoffset" ||
             material.techniqueSet == "effect_add_nofog_eyeoffset")
        material.family = FxMaterialFamily::additive;
    // Likewise the falloff and spot-shadowed alpha variants draw as plain
    // (feathered) alpha sprites.
    else if (material.techniqueSet == "effect_falloff" ||
             material.techniqueSet == "effect_falloff_nofog")
        material.family = FxMaterialFamily::alpha;
    else if (material.techniqueSet == "effect_zfeather_falloff" ||
             material.techniqueSet == "effect_zfeather_falloff_nofog" ||
             material.techniqueSet == "effect_zfeather_spot" ||
             material.techniqueSet == "effect_zfeather_spot_nofog")
        material.family = FxMaterialFamily::alphaFeather;
    // The view-angle falloff of these additive sprites (IW3 fire roar) has no
    // Replay counterpart; the feathered additive family draws them unfaded.
    else if (material.techniqueSet == "effect_zfeather_falloff_add" ||
             material.techniqueSet == "effect_zfeather_falloff_add_nofog")
        material.family = FxMaterialFamily::additiveFeather;
    else if (material.techniqueSet.starts_with("distortion"))
    {
        material.family = FxMaterialFamily::distortion;
        return material;
    }
    else if (material.techniqueSet == "particle_cloud" ||
             material.techniqueSet == "particle_cloud_outdoor")
        material.family = FxMaterialFamily::cloud;
    else if (std::regex_match(material.techniqueSet,
                              std::regex("[mw]c_l_sm_b0c0(n0)?(s0)?")))
        material.family = FxMaterialFamily::decal;
    else
        return material;

    if (source.at("_game") != "iw3" || source.at("_type") != "material" ||
        source.at("_version") != 1)
        throw std::runtime_error("unsupported IW3 FX material metadata: " + name);
    const auto &atlas = source.at("textureAtlas");
    material.rows = atlas.at("rows").get<unsigned>();
    material.columns = atlas.at("columns").get<unsigned>();
    if (!material.rows || !material.columns || material.rows > 255 || material.columns > 255 ||
        !std::has_single_bit(material.columns) ||
        static_cast<std::uint64_t>(material.rows) * material.columns > 65535)
        throw std::runtime_error("unsupported IW3 FX material atlas: " + name);

    const auto validImageName = [](const std::string &image) {
        return !image.empty() && image.find("..") == std::string::npos &&
               image.find(':') == std::string::npos && image.find('\\') == std::string::npos &&
               image.front() != '/';
    };

    if (material.family == FxMaterialFamily::decal)
    {
        const int stateFlags = source.at("stateFlags").get<int>();
        if (source.at("cameraRegion") != "decal" || (stateFlags != 52 && stateFlags != 53) ||
            material.rows != 1 || material.columns != 1)
            throw std::runtime_error("unsupported IW3 FX decal metadata: " + name);
        const auto &textures = source.at("textures");
        if (!textures.is_array() || textures.empty() || textures.size() > 3)
            throw std::runtime_error("IW3 FX decal texture set is unsupported: " + name);
        for (const auto &texture : textures)
        {
            const std::string semantic = texture.at("semantic").get<std::string>();
            const std::string image = texture.at("image").get<std::string>();
            if (!validImageName(image))
                throw std::runtime_error("IW3 FX decal has an invalid image name: " + name);
            // "$identitynormalmap" and other "$" names are engine defaults.
            if (image.front() == '$' && semantic != "colorMap")
                continue;
            if (semantic == "colorMap" && material.image.empty())
                material.image = image;
            else if (semantic == "normalMap" && material.normal.empty())
                material.normal = image;
            else if (semantic == "specularMap" && material.response.empty())
                material.response = image;
            else
                throw std::runtime_error("IW3 FX decal has an unsupported texture semantic: " +
                                         name);
        }
        if (material.image.empty())
            throw std::runtime_error("IW3 FX decal has no color map: " + name);
        bool foundTint = false, foundEnvironment = false;
        for (const auto &constant : source.at("constants"))
        {
            const std::string key = constant.at("name").get<std::string>();
            if (key == "colorTint")
                material.tint = FxLiteral(constant.at("literal"), "color tint"), foundTint = true;
            else if (key == "envMapParms")
                FxLiteral(constant.at("literal"), "environment parameters"),
                    foundEnvironment = true;
            else
                throw std::runtime_error("IW3 FX decal has an unsupported constant: " + name);
        }
        (void)foundEnvironment;
        if (!foundTint)
            throw std::runtime_error("IW3 FX decal is missing its color tint: " + name);
        return material;
    }

    const int spriteStateFlags = source.at("stateFlags").get<int>();
    if (source.at("cameraRegion") != "emissive" ||
        (spriteStateFlags != 16 &&
         !(material.family == FxMaterialFamily::alpha &&
           (spriteStateFlags == 20 || spriteStateFlags == 24))))
    {
        if (material.family == FxMaterialFamily::alpha)
        {
            material.family = FxMaterialFamily::unsupported;
            return material;
        }
        throw std::runtime_error("unsupported IW3 FX material metadata: " + name);
    }

    const auto &textures = source.at("textures");
    if (!textures.is_array() || textures.size() != 1)
        throw std::runtime_error("IW3 FX material must have one color texture: " + name);
    const auto &texture = textures.front();
    if (texture.at("name") != "colorMap" || texture.at("semantic") != "colorMap")
        throw std::runtime_error("IW3 FX material has an unsupported texture semantic: " + name);
    material.image = texture.at("image").get<std::string>();
    if (!validImageName(material.image))
        throw std::runtime_error("IW3 FX material has an invalid image name: " + name);
    const auto &sampler = texture.at("samplerState");
    const std::string filter = sampler.at("filter").get<std::string>();
    const std::string mipMap = sampler.at("mipMap").get<std::string>();
    const bool clamped = sampler.at("clampU").get<bool>() &&
                         sampler.at("clampV").get<bool>() &&
                         sampler.at("clampW").get<bool>();
    const bool wrapped = !sampler.at("clampU").get<bool>() &&
                         !sampler.at("clampV").get<bool>() &&
                         !sampler.at("clampW").get<bool>();
    const bool outdoorCloud = material.techniqueSet == "particle_cloud_outdoor";
    const bool supportedFilter = filter == "linear" || filter == "aniso2x" ||
                                 (outdoorCloud && filter == "aniso4x");
    // Sprite UVs stay inside [0, 1], so clamp and wrap addressing only differ in
    // the last texel at the border; both are converted.
    if ((material.family == FxMaterialFamily::cloud ? !clamped : !(wrapped || clamped)) ||
        !supportedFilter ||
        (mipMap != "disabled" && mipMap != "nearest" && mipMap != "linear"))
    {
        if (!noFogAlias)
            zt::warn("iw3: FX material %s sampler has no pinned Replay mapping; its effect "
                     "elements are not converted",
                     name.c_str());
        material.family = FxMaterialFamily::unsupported;
        return material;
    }

    bool foundColorState = false;
    for (const auto &state : source.at("stateBits"))
    {
        if (!state.value("colorWriteRgb", false) || state.value("polymodeLine", false))
            continue;
        if (foundColorState)
            throw std::runtime_error("IW3 FX material has multiple color states: " + name);
        foundColorState = true;
        const bool shared = state.at("blendOpRgb") == "add" &&
                            state.at("depthTest") == "less_equal" &&
                            !state.at("depthWrite").get<bool>();
        // IW3 opaque cutout particles (alphaTest ge128, one/zero, depth write)
        // draw through the alpha-blended family: their alpha is a hard mask.
        const bool cutout = state.at("alphaTest") == "ge128" &&
                            state.at("srcBlendRgb") == "one" &&
                            state.at("dstBlendRgb") == "zero";
        const bool alpha = (state.at("alphaTest") == "gt0" &&
                            state.at("srcBlendRgb") == "srcalpha" &&
                            state.at("dstBlendRgb") == "invsrcalpha") ||
                           cutout;
        // invdestcolor/one (IW3 "screen" fire) is drawn additively.
        const bool additive = state.at("alphaTest") == "disabled" &&
                              (state.at("srcBlendRgb") == "one" ||
                               state.at("srcBlendRgb") == "invdestcolor") &&
                              state.at("dstBlendRgb") == "one";
        const bool alphaAdditive = state.at("srcBlendRgb") == "srcalpha" &&
                                   state.at("dstBlendRgb") == "one";
        const bool wantsAlpha = material.family == FxMaterialFamily::alpha ||
                                 material.family == FxMaterialFamily::alphaFeather ||
                                 material.family == FxMaterialFamily::cloud;
        if (shared && alphaAdditive)
        {
            material.family = material.family == FxMaterialFamily::alphaFeather ||
                                      material.family == FxMaterialFamily::additiveFeather
                                  ? FxMaterialFamily::additiveFeather
                                  : FxMaterialFamily::additive;
            material.premultiplied = true;
            continue;
        }
        if ((!shared && !cutout) || (wantsAlpha ? !alpha : !additive))
        {
            // An unmapped blend (srcalpha/one particle_cloud in mp_broadcast2)
            // drops only the effect elements that use this material.
            zt::warn("iw3: FX material %s blend state has no pinned Replay mapping; its effect "
                     "elements are not converted",
                     name.c_str());
            material.family = FxMaterialFamily::unsupported;
            return material;
        }
    }
    if (!foundColorState)
        throw std::runtime_error("IW3 FX material has no color state: " + name);

    if (material.family == FxMaterialFamily::cloud)
    {
        if (material.rows != 1 || material.columns != 1 || !source.at("constants").empty())
            throw std::runtime_error("IW3 cloud material has no pinned Replay atlas mapping: " +
                                     name);
        return material;
    }

    bool foundFeather = false, foundTint = false;
    for (const auto &constant : source.at("constants"))
    {
        const std::string key = constant.at("name").get<std::string>();
        if (key == "featherParms" &&
            (material.family == FxMaterialFamily::alphaFeather ||
             material.family == FxMaterialFamily::additiveFeather))
            FxLiteral(constant.at("literal"), "feather parameters"), foundFeather = true;
        else if (key == "colorTint")
            material.tint = FxLiteral(constant.at("literal"), "color tint"), foundTint = true;
        else if (material.techniqueSet.find("falloff") != std::string::npos ||
                 material.techniqueSet.find("eyeoffset") != std::string::npos ||
                 material.techniqueSet.find("_spot") != std::string::npos)
            continue; // parameters of the unconverted falloff, eye offset and spot terms
        else
            throw std::runtime_error("IW3 FX material has an unsupported constant: " + name);
    }
    const bool requiresFeather = material.family == FxMaterialFamily::alphaFeather ||
                                 material.family == FxMaterialFamily::additiveFeather;
    // IW3 additive clouds carry no tint constant (white).
    if (!foundTint && material.techniqueSet.starts_with("particle_cloud"))
        foundTint = true;
    if (!foundTint || (requiresFeather && !foundFeather))
        throw std::runtime_error("IW3 FX material is missing required constants: " + name);
    return material;
}

SourceMaterial ReadMaterial(const std::filesystem::path &root,
                            const std::vector<std::filesystem::path> &sourcePaths,
                            const std::string &name)
{
    const auto path = FindMaterial(root, sourcePaths, name);
    if (path.empty())
    {
        // CoD4's stock placeholders are not exported as material JSON. They
        // are untextured lit surfaces, so the conversion can represent them
        // without inventing a texture dependency.
        if (name == "mc/lambert1" || name == "mc/gfx_missing_fx")
        {
            SourceMaterial material;
            material.name = name;
            material.color = "$white";
            material.normal = "$identitynormalmap";
            material.response = "$white";
            return material;
        }
        throw std::runtime_error(
            "missing IW3 material '" + name +
            "'; add the matching IW3 main and raw directories with --search-path");
    }
    const Json source = ReadJson(path);
    SourceMaterial material;
    material.name = name;
    material.surfaceTypeBits = source.value("surfaceTypeBits", 0u);
    if (source.contains("gameFlags"))
    {
        const auto &flags = source.at("gameFlags");
        if (!flags.is_array())
            throw std::runtime_error("Invalid IW3 material game flags: " + name);
        material.castsShadow = false;
        for (const auto &flag : flags)
            if (flag == "CASTS_SHADOW")
                material.castsShadow = true;
    }
    const std::string techset = source.at("techniqueSet").get<std::string>();
    if (techset == "shadowcaster")
    {
        material.kind = SurfaceKind::skipped;
        material.shadowOnly = true;
        for (const auto &state : source.value("stateBits", Json::array()))
        {
            if (!state.contains("cullFace"))
                continue;
            const std::string cull = state.at("cullFace").get<std::string>();
            if (cull != "none" && cull != "back" && cull != "front")
                throw std::runtime_error("Unsupported IW3 material culling: " + name);
            material.cullMode = cull == "none" ? 0 : cull == "back" ? 1 : 2;
            break;
        }
        return material;
    }
    if (std::regex_search(techset, std::regex("(^|_)sky($|_)", std::regex::icase)))
    {
        material.kind = SurfaceKind::skipped;
        return material;
    }
    bool authoredAlpha =
        std::regex_search(techset, std::regex("(^|_)[at][0-9]", std::regex::icase)) ||
        techset.find("alphatest") != std::string::npos;
    const bool blended = std::regex_search(techset, std::regex("(^|_)b[0-9]", std::regex::icase));
    std::string alphaMode = "ge128";
    bool foundColorState = false;
    for (const auto &texture : source.value("textures", Json::array()))
    {
        const std::string semantic = texture.value("semantic", std::string{});
        if (semantic == "colorMap")
            material.color = texture.at("image").get<std::string>();
        else if (semantic == "normalMap")
            material.normal = texture.at("image").get<std::string>();
        else if (semantic == "specularMap")
            material.response = texture.at("image").get<std::string>();
    }
    for (const auto &constant : source.value("constants", Json::array()))
    {
        const std::string key = constant.value("name", std::string{});
        if (key == "colorTint")
            material.tint = Literal<4>(constant.at("literal"), material.tint);
        else if (key == "envMapParms")
            material.environment = Literal<4>(constant.at("literal"), material.environment);
    }
    for (const auto &state : source.value("stateBits", Json::array()))
    {
        if (!foundColorState && state.value("colorWriteRgb", false) &&
            !state.value("polymodeLine", false))
        {
            const auto cull = state.value("cullFace", std::string("back"));
            if (cull != "none" && cull != "back" && cull != "front")
                throw std::runtime_error("Unsupported IW3 material culling: " + name);
            material.cullMode = cull == "none" ? 0 : cull == "back" ? 1 : 2;
            foundColorState = true;
        }
        const std::string test = state.value("alphaTest", std::string{});
        if (test == "gt0" || test == "lt128" || test == "ge128")
        {
            authoredAlpha = true;
            if (state.value("colorWriteRgb", false) && !state.value("polymodeLine", false))
                alphaMode = test;
        }
    }
    if (material.color.empty())
        throw std::runtime_error("IW3 material has no color map: " + name);
    // Decals offset their lit pass (IW3 technique 7). Every IW3 material also
    // carries an offset2 debug state, so only the lit pass's state counts.
    if (const auto entries = source.find("stateBitsEntry");
        entries != source.end() && entries->is_array() && entries->size() > 7)
    {
        const int litState = entries->at(7).get<int>();
        const auto &states = source.value("stateBits", Json::array());
        if (litState >= 0 && static_cast<std::size_t>(litState) < states.size())
        {
            const std::string offset =
                states.at(litState).value("polygonOffset", std::string("offset0"));
            material.depthBias = offset != "offset0" && offset != "offsetShadowmap";
        }
    }
    material.kind =
        blended ? SurfaceKind::glass : (authoredAlpha ? SurfaceKind::cutout : SurfaceKind::opaque);
    if (material.kind == SurfaceKind::cutout)
        material.flags = alphaMode == "gt0" ? 8u : (alphaMode == "lt128" ? 16u : 0u);
    // `$identitynormalmap` is a stock flat-normal placeholder.  Its texture
    // payload is retained for the native material contract, but setting the
    // authored-normal flag would make SourceWorldNormal decode it as a
    // slope-map (the shader's flag is the normal-map enable bit).
    if (!material.normal.empty() && material.normal != "$identitynormalmap")
        material.flags |= 32u;
    if (!material.response.empty())
        material.flags |= 64u;
    // IW3 unlit techniques (mc_unlit, mc_unlit_replace, lamp and screen
    // materials) show their colour map as emitted light.
    if (techset.find("unlit") != std::string::npos)
        material.flags |= 128u;
    return material;
}

bool FoliageName(const std::string &value)
{
    return std::regex_search(
        value, std::regex("leaf|leaves|foliage|tree|bush|branch|grass|fern|pine|hedge|palm",
                          std::regex::icase));
}

std::string TileKey(const SourceMaterial &material)
{
    std::string key = material.color + '\x1f' + material.normal + '\x1f' + material.response;
    for (const float value : material.tint)
    {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        key.append(reinterpret_cast<const char *>(&bits), sizeof(bits));
    }
    return key;
}

void ClassifyFoliage(SourceMaterial &material, const Image &color)
{
    if (material.kind != SurfaceKind::opaque || !FoliageName(material.name + " " + material.color))
        return;
    std::size_t low = 0, high = 0;
    for (std::size_t offset = 3; offset < color.rgba.size(); offset += 4)
        color.rgba[offset] < 128 ? ++low : ++high;
    const std::size_t threshold = color.rgba.size() / 4 / 1000;
    if (low > threshold && high > threshold)
        material.kind = SurfaceKind::cutout;
}

std::vector<std::uint8_t> Sha256(const std::span<const std::uint8_t> data)
{
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength = 0, digestLength = 0, returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &returned, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digestLength),
                          sizeof(digestLength), &returned, 0) < 0)
        throw std::runtime_error("cannot initialize SHA-256");
    std::vector<std::uint8_t> object(objectLength), digest(digestLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) <
            0 ||
        BCryptFinishHash(hash, digest.data(), digestLength, 0) < 0)
    {
        if (hash)
            BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("cannot calculate SHA-256");
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

std::string Hex(const std::span<const std::uint8_t> data)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output(data.size() * 2, '0');
    for (std::size_t index = 0; index < data.size(); ++index)
    {
        output[index * 2] = digits[data[index] >> 4];
        output[index * 2 + 1] = digits[data[index] & 15];
    }
    return output;
}

std::vector<std::uint8_t> Unhex(const std::string &text)
{
    if (text.size() % 2)
        throw std::runtime_error("invalid embedded technique hex");
    std::vector<std::uint8_t> output(text.size() / 2);
    auto digit = [](const char value) -> unsigned {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        throw std::runtime_error("invalid embedded technique hex digit");
    };
    for (std::size_t index = 0; index < output.size(); ++index)
        output[index] =
            static_cast<std::uint8_t>((digit(text[index * 2]) << 4) | digit(text[index * 2 + 1]));
    return output;
}

template <class T> T At(const std::vector<std::uint8_t> &data, const std::size_t offset)
{
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("invalid technique template");
    T result{};
    std::memcpy(&result, data.data() + offset, sizeof(result));
    return result;
}

template <class T>
void Put(std::vector<std::uint8_t> &data, const std::size_t offset, const T value)
{
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("invalid technique template");
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

std::string HlslNumber(const float value)
{
    if (!std::isfinite(value))
        throw std::runtime_error("IW3 material has a nonfinite shader constant");
    std::array<char, 48> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::general,
                                      std::numeric_limits<float>::max_digits10);
    if (result.ec != std::errc{})
        throw std::runtime_error("cannot format IW3 material shader constant");
    return std::string(buffer.data(), result.ptr);
}

std::vector<std::uint8_t> Compile(const std::string &source, const char *target, const char *name)
{
    ID3DBlob *program = nullptr, *errors = nullptr;
    const HRESULT result = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, "main",
                                      target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &program, &errors);
    std::string message;
    if (errors)
    {
        message.assign(static_cast<const char *>(errors->GetBufferPointer()),
                       errors->GetBufferSize());
        errors->Release();
    }
    if (FAILED(result) || !program)
        throw std::runtime_error("cannot compile IW3 Replay shader: " + message);
    const auto *begin = static_cast<const std::uint8_t *>(program->GetBufferPointer());
    std::vector<std::uint8_t> output(begin, begin + program->GetBufferSize());
    program->Release();
    if (output.size() < 4 || std::memcmp(output.data(), "DXBC", 4) != 0)
        throw std::runtime_error("D3D compiler returned an invalid shader");
    return output;
}

Json Shader(const unsigned type, const std::string &label, const std::string &debugName,
            const std::vector<std::uint8_t> &program)
{
    const auto digest = Sha256(program);
    const std::string name = "mw120r_" + label + "_" + Hex(std::span(digest).first(12));
    std::array<std::uint8_t, 40> header{};
    const std::uint32_t length = static_cast<std::uint32_t>(program.size());
    std::uint32_t identity{};
    std::memcpy(&identity, digest.data(), sizeof(identity));
    std::memcpy(header.data() + 32, &length, sizeof(length));
    std::memcpy(header.data() + 36, &identity, sizeof(identity));
    return {{"type", type},
            {"name", name},
            {"debugName", debugName},
            {"header", Hex(header)},
            {"program", Hex(program)}};
}

void PatchStateIdentity(Json &technique, const std::vector<std::uint8_t> &seed)
{
    auto states = Unhex(technique.at("states").get<std::string>());
    const auto header = Unhex(technique.at("header").get<std::string>());
    for (std::size_t offset = 0; offset < states.size(); offset += 16)
    {
        std::vector<std::uint8_t> input = seed;
        input.insert(input.end(), header.begin(), header.end());
        input.push_back(static_cast<std::uint8_t>(offset));
        const auto digest = Sha256(input);
        std::copy_n(digest.begin(), 8, states.begin() + offset);
    }
    technique["states"] = Hex(states);
}

// Shipped Replay decal techniques (tw/..._812004, tmo/..._814000) set otherBits
// 0x10, the polygon-offset depth bias. IW3 decals need it to avoid z-fighting
// with the coplanar surface they sit on.
void ApplyDepthBias(Json &techset)
{
    techset["name"] = techset.at("name").get<std::string>() + "_bias";
    for (auto &technique : techset.at("techniques"))
    {
        auto header = Unhex(technique.at("header").get<std::string>());
        Put<std::uint64_t>(header, 0xA0, At<std::uint64_t>(header, 0xA0) | 0x10ull);
        technique["header"] = Hex(header);
        technique["name"] = technique.at("name").get<std::string>() + "_bias";
        PatchStateIdentity(technique, header);
    }
}

std::string CoveragePrefix(const std::string &source, const std::string &signature)
{
    const std::string original = "float4 main(Input input) : SV_TARGET0";
    const std::size_t start = source.find(original);
    const std::size_t clip = source.find("clip(texel.a - .5);", start);
    if (start == std::string::npos || clip == std::string::npos)
        throw std::runtime_error("embedded IW3 shader is missing alpha coverage");
    return source.substr(0, start) + signature +
           source.substr(start + original.size(),
                         clip + std::strlen("clip(texel.a - .5);") - (start + original.size()));
}

std::string PrepassSource(const std::string &source)
{
    return CoveragePrefix(source, "float main(Input input) : SV_TARGET1") + R"(
    float3 normal = normalize(input.normal);
    uint3 packed = (uint3)(saturate(normal * .5 + .5) * 1023.0);
    return asfloat(packed.x | (packed.y << 10) | (packed.z << 20));
}
)";
}

std::string VelocitySource(const std::string &source)
{
    // Replay's VELRGD prepass writes screen motion to target 0 beside the packed
    // normal. The encoding is the shipped model prepass (cb7[176].x selects raw).
    return CoveragePrefix(source, "void main(Input input, out float4 velocity : SV_TARGET0, "
                                  "out float packedNormal : SV_TARGET1)") +
           R"(
    float3 normal = normalize(input.normal);
    uint3 packed = (uint3)(saturate(normal * .5 + .5) * 1023.0);
    packedNormal = asfloat(packed.x | (packed.y << 10) | (packed.z << 20));
    float4 delta = (input.clipPosition.xyxy / input.clipPosition.w -
                    input.previousClipPosition.xyxy / input.previousClipPosition.w) *
                   float4(.5, -.5, 960, -540);
    float2 fast = sqrt(saturate((abs(delta.zw) - 3) * 0.012987));
    float2 slow = min(abs(delta.zw) * (1.0 / 3.0), 1);
    float2 encoded = (slow - fast) * .5 + fast;
    encoded = saturate((delta.xy >= 0 ? encoded : -encoded) * .5 + 0.501961);
    velocity = float4(0, 0, replayLighting[176].x != 0 ? delta.xy : encoded);
}
)";
}

std::string ShadowSource(const std::string &source)
{
    std::string output = CoveragePrefix(source, "void main(Input input)");
    const std::string key = "uint kind = flags % 4;";
    const std::size_t where = output.find(key);
    if (where == std::string::npos)
        throw std::runtime_error("embedded IW3 shader is missing its material kind");
    output.insert(where + key.size(), "\n    if (kind != 3) return;");
    output += "\n}\n";
    return output;
}

void AppendTextureArgument(Json &technique, std::string_view hex);

void AppendCoverageArgument(Json &technique, bool staticModel)
{
    AppendTextureArgument(technique, staticModel ? "051000000000" : "051001000000");
}

// Per-instance IW3 light-grid lighting for static models (StaticModelLightingImage):
// material texture slot 0x3D bound to the lit pixel shader's t6.
constexpr std::uint8_t kInstanceLightingSlot = 0x3D;
constexpr unsigned kInstanceLightingColumns = 256; // instances per image row (6 texels each)

void AppendTextureArgument(Json &technique, const std::string_view hex)
{
    auto header = Unhex(technique.at("header").get<std::string>());
    auto args = Unhex(technique.at("args").get<std::string>());
    const auto argument = Unhex(std::string(hex));
    // Shipped records keep each argument category sorted by type (code
    // constants, textures, then code images); insert into the stable block.
    std::size_t at = (std::size_t{header[0x78]} + header[0x79]) * 6;
    const std::size_t end = at + std::size_t{header[0x7A]} * 6;
    if (end > args.size())
        throw std::runtime_error("technique argument counts exceed the argument array");
    while (at < end && args[at] <= argument[0])
        at += 6;
    args.insert(args.begin() + static_cast<std::ptrdiff_t>(at), argument.begin(), argument.end());
    ++header[0x7A];
    technique["header"] = Hex(header);
    technique["args"] = Hex(args);
}

void KeepReferencedShaders(Json &techset)
{
    std::set<std::string> needed;
    for (const auto &technique : techset.at("techniques"))
        for (unsigned index = 0; index < 4; ++index)
            if (!technique.at("shaders").at(index).is_null())
                needed.insert(std::to_string(14 + index) + ":" +
                              technique.at("shaders").at(index).get<std::string>());
    Json shaders = Json::object();
    for (const auto &key : needed)
        shaders[key] = techset.at("shaders").at(key);
    techset["shaders"] = std::move(shaders);
}

void ApplyStaticModelTechniqueLayout(Json &techset)
{
    struct Layout
    {
        std::string_view header;
        std::string_view states;
        std::string_view args;
    };
    static constexpr std::array layouts{
        Layout{"000000000000000000000000000001ffff00ffff0000ffffffffffffffffffffffffffffffffff00000"
               "00000000000000000000000000000000000000000000000000000000000000000000000000000000000"
               "00000000000000000000000000000000000000000000000000000000000000000000000000000002000"
               "00000006000020200000000000000000000000000000000000000000000000001000000010e00000000"
               "0000000000000fffffff0000000000000000",
               "e5d7c9c4c8440c1b0000000000000000c58419b67124f4440000000000000000",
               "00020000a400000205000100"},
        Layout{"00000000000000001b000000000001ffff00ffff0000ffffffffffffffffffffffffffffffffff00000"
               "00000000000000000000000000000000000000000000000000000000000000000000000000000000000"
               "00000000000000000000000000000000000000000000000000000000000000000000000000000002000"
               "00000006000000100000000000000000000000000000000000000000000000001000000210e00000000"
               "000000000000ffffffff0000000000000000",
               "29a55f10348ebbff0000000000000000", "00020000a400000205000100"},
        Layout{"00000000000000001c000000000001ffff00ffff0000ffffffffffffffffffffffffffffffffff00000"
               "00000000000000000000000000000000000000000000000000000000000000000000000000000000000"
               "00000000000000000000000000000000000000000000000000000000000000000000000000000002000"
               "00000006000000100000000000000000000000000000000000000000000000001000000220e00000000"
               "000000000000ffffffff0000000000000000",
               "7cc9a43f6b5db46a0000000000000000", "00020000a400000205000100"},
        Layout{"000000000000000022000000000001ffffffffff0000ffffffffffffffffffffffffffffffffff00000"
               "00000000000000000000000000000000000000000000000000000000000000000000000000000000000"
               "0000000000000000000000000000000000000000000000000000000000000000000000000000000e040"
               "00000008000020100000000000000000000000000000000000000000000000002000000010800000000"
               "000000000000000000000000000000000000",
               "9553be8776054af60000000000000000",
               "00020000a400001204000000000205000100001006000400001007006700011003004f00030200001f0"
               "0030201001d00030202001e00031004002c00051000000000051001000900051002003b000812070000"
               "00091001000000091002000900091003003600091000004800"},
    };
    if (techset.at("techniques").size() != layouts.size())
        throw std::runtime_error("embedded Replay technique set has the wrong pass count");
    techset["header"] = "00000000000000002010000400000000140009000000000001000018040000000000000000"
                        "000000000000000000000000000000000000000000000000000000";
    for (std::size_t index = 0; index < layouts.size(); ++index)
    {
        Json &technique = techset.at("techniques").at(index);
        technique["header"] = layouts[index].header;
        technique["states"] = layouts[index].states;
        technique["args"] = layouts[index].args;
    }
    techset["source"] =
        "Exact Replay 1.20 static-model technique layout; process/GPU pointers removed.";
}

enum class TechniqueGeometry
{
    world,
    staticModel,
    // Script, dynamic and effect XModels draw through Replay's DObj path.
    entity,
};

void ReplaceAtlasTokens(std::string &source, const unsigned columns, const unsigned atlasSize)
{
    for (const auto &[token, value] :
         {std::pair<std::string_view, unsigned>{"ATLAS_COLUMNS", columns},
          std::pair<std::string_view, unsigned>{"ATLAS_SIZE", atlasSize}})
        for (std::size_t at = source.find(token); at != std::string::npos;
             at = source.find(token, at + 1))
            source.replace(at, token.size(), std::to_string(value));
}

Json BuildTechset(const std::string &map, const unsigned columns, const unsigned atlasSize,
                  const TechniqueGeometry geometry,
                  const std::string &glassEnvironment, const bool instanceLighting = false,
                  const std::string &lightVolumeDefines = {})
{
    const bool staticModel = geometry == TechniqueGeometry::staticModel;
    const bool entity = geometry == TechniqueGeometry::entity;
    const bool model = staticModel || entity;
    Json techset = Json::parse(
        ResourceText(entity ? IDR_IW3_ENTITY_TECHSET_TEMPLATE : IDR_IW3_TECHSET_TEMPLATE));
    if (staticModel)
        ApplyStaticModelTechniqueLayout(techset);
    std::string pixelSource =
        "#define MAP_SOURCE_SUN_MASK 1\n#define MAP_SOURCE_CHANNELS 1\n" + glassEnvironment;
    if (model)
        pixelSource += "#define STATIC_MODEL 1\n";
    if (staticModel && instanceLighting)
        pixelSource += "#define MAP_INSTANCE_LIGHTING 1\n";
    if (entity && !lightVolumeDefines.empty())
        pixelSource += lightVolumeDefines;
    pixelSource += ResourceText(IDR_IW3_PIXEL_SHADER);
    ReplaceAtlasTokens(pixelSource, columns, atlasSize);
    // Model coverage passes bind no view mip bias; sample with the gradients alone.
    const std::string coverageSource =
        model ? "#define MAP_MIP_BIAS 0\n" + pixelSource : pixelSource;
    const auto pixel = Compile(pixelSource, "ps_5_0", "iw3_map_surface.hlsl");
    const std::string vertexDefines = entity ? "#define MAP_ENTITY 1\n" : "";
    const char *vertexName = entity        ? "iw3_entity_vertex.hlsl"
                             : staticModel ? "iw3_model_vertex.hlsl"
                                           : "iw3_world_vertex.hlsl";
    const std::string vertexText =
        ResourceText(model ? IDR_IW3_MODEL_VERTEX_SHADER : IDR_IW3_VERTEX_SHADER);
    const auto vertex = Compile(vertexDefines + vertexText, "vs_5_0", vertexName);
    Json pixelShader = Shader(17, "iw3_surface", "iw3_map_surface.hlsl", pixel);
    Json vertexShader = Shader(14, entity ? "iw3_entity" : staticModel ? "iw3_model" : "iw3_world",
                               vertexName, vertex);
    Json &lit = techset.at("techniques").back();
    auto litHeader = Unhex(lit.at("header").get<std::string>());
    if (!model)
        litHeader[0x9C] = 35;
    lit["header"] = Hex(litHeader);
    const Json oldPixel = lit.at("shaders").at(3);
    if (oldPixel.is_string())
        techset.at("shaders").erase("17:" + oldPixel.get<std::string>());
    lit["shaders"][3] = pixelShader.at("name");
    lit["shaders"][0] = vertexShader.at("name");
    techset["shaders"]["17:" + pixelShader.at("name").get<std::string>()] = pixelShader;
    techset["shaders"]["14:" + vertexShader.at("name").get<std::string>()] = vertexShader;
    const auto techDigest = Sha256(pixel);
    techset["name"] = "tw/mw120r_" + map +
                      (entity ? "_entity_" : staticModel ? "_model_" : "_") +
                      Hex(std::span(techDigest).first(6));
    lit["name"] =
        "TECHNIQUE_LIT_FORWARDPLUS_BITMASK_" + techset.at("name").get<std::string>().substr(3);
    if ((staticModel && instanceLighting) || (entity && !lightVolumeDefines.empty()))
        AppendTextureArgument(lit, "051006003d00"); // type 5, pixel stage, t6, slot 0x3D
    PatchStateIdentity(lit, pixel);

    const auto depthProgram =
        Compile(PrepassSource(coverageSource), "ps_5_0", "iw3_depth_coverage.hlsl");
    Json depthShader = Shader(17, "iw3_depth", "iw3_depth_coverage.hlsl", depthProgram);
    Json &depth = techset.at("techniques").front();
    auto depthHeader = Unhex(depth.at("header").get<std::string>());
    if (At<std::uint32_t>(depthHeader, 8) != 0)
        throw std::runtime_error("embedded technique set has no depth prepass");
    if (!model)
    {
        depthHeader[0x9C] = 35;
    }
    depth["header"] = Hex(depthHeader);
    depth["shaders"] =
        Json::array({vertexShader.at("name"), nullptr, nullptr, depthShader.at("name")});
    depth["name"] = "TECHNIQUE_DEPTH_PREPASS_" + depthShader.at("name").get<std::string>();
    AppendCoverageArgument(depth, model);
    PatchStateIdentity(depth, depthProgram);
    techset["shaders"]["17:" + depthShader.at("name").get<std::string>()] = depthShader;

    const auto shadowProgram =
        Compile(ShadowSource(coverageSource), "ps_5_0", "iw3_shadow_coverage.hlsl");
    Json shadowShader = Shader(17, "iw3_shadow", "iw3_shadow_coverage.hlsl", shadowProgram);
    Json shadowVertexShader;
    if (model)
    {
        const char *shadowVertexName =
            entity ? "iw3_entity_shadow_vertex.hlsl" : "iw3_model_shadow_vertex.hlsl";
        const auto shadowVertexProgram =
            Compile(vertexDefines + "#define MAP_MODEL_SHADOW_PASS 1\n" + vertexText, "vs_5_0",
                    shadowVertexName);
        shadowVertexShader = Shader(14, entity ? "iw3_entity_shadow" : "iw3_model_shadow",
                                    shadowVertexName, shadowVertexProgram);
        techset["shaders"]["14:" + shadowVertexShader.at("name").get<std::string>()] =
            shadowVertexShader;
    }
    // Replay picks the DObj prepass variant per draw (velocity, stencil and
    // object-id, rigid or skinned). The lit pass only shades where its prepass
    // wrote depth, so every variant the shipped mco techset carries must exist
    // or the object draws nothing. Stencil/object-id differ only in state.
    std::vector<std::uint8_t> velocityProgram;
    Json velocityShader;
    Json velocityVertexShader;
    if (entity)
    {
        velocityProgram = Compile(VelocitySource("#define MAP_ENTITY_VELOCITY 1\n"
                                                 "#define MAP_LIGHTING_ROWS 177\n" +
                                                 coverageSource),
                                  "ps_5_0", "iw3_velocity_coverage.hlsl");
        velocityShader = Shader(17, "iw3_velocity", "iw3_velocity_coverage.hlsl", velocityProgram);
        const auto velocityVertexProgram =
            Compile(vertexDefines + "#define MAP_ENTITY_VELOCITY 1\n" + vertexText, "vs_5_0",
                    "iw3_entity_velocity_vertex.hlsl");
        velocityVertexShader = Shader(14, "iw3_entity_velocity", "iw3_entity_velocity_vertex.hlsl",
                                      velocityVertexProgram);
        techset["shaders"]["17:" + velocityShader.at("name").get<std::string>()] = velocityShader;
        techset["shaders"]["14:" + velocityVertexShader.at("name").get<std::string>()] =
            velocityVertexShader;
    }
    std::vector<unsigned> shadowTypes;
    for (auto &technique : techset.at("techniques"))
    {
        auto header = Unhex(technique.at("header").get<std::string>());
        const unsigned type = At<std::uint32_t>(header, 8);
        const bool velocityPrepass =
            type == 3 || type == 6 || type == 12 || type == 15 || type == 21 || type == 24;
        if (entity && velocityPrepass)
        {
            // DObj prepass with motion: previous-frame bone rows and view.
            technique["shaders"] = Json::array(
                {velocityVertexShader.at("name"), nullptr, nullptr, velocityShader.at("name")});
            technique["name"] = "TECHNIQUE_DEPTH_PREPASS_" + std::to_string(type) + "_" +
                                velocityShader.at("name").get<std::string>();
            AppendCoverageArgument(technique, model);
            PatchStateIdentity(technique, velocityProgram);
            continue;
        }
        if (entity && (type == 9 || type == 18))
        {
            technique["shaders"] =
                Json::array({vertexShader.at("name"), nullptr, nullptr, depthShader.at("name")});
            technique["name"] = "TECHNIQUE_DEPTH_PREPASS_" + std::to_string(type) + "_" +
                                depthShader.at("name").get<std::string>();
            AppendCoverageArgument(technique, model);
            PatchStateIdentity(technique, depthProgram);
            continue;
        }
        if (type != 27 && type != 28)
            continue;
        if (!model)
        {
            header[0x9C] = 35;
        }
        technique["header"] = Hex(header);
        technique["shaders"] =
            Json::array({model ? shadowVertexShader.at("name") : vertexShader.at("name"),
                         nullptr, nullptr, shadowShader.at("name")});
        technique["name"] = "TECHNIQUE_SHADOW_" + std::to_string(type) + "_" +
                            shadowShader.at("name").get<std::string>();
        AppendCoverageArgument(technique, model);
        PatchStateIdentity(technique, shadowProgram);
        shadowTypes.push_back(type);
    }
    std::sort(shadowTypes.begin(), shadowTypes.end());
    if (shadowTypes != std::vector<unsigned>{27, 28})
        throw std::runtime_error("embedded technique set has incomplete shadow passes");
    techset["shaders"]["17:" + shadowShader.at("name").get<std::string>()] = shadowShader;
    auto header = Unhex(techset.at("header").get<std::string>());
    if (staticModel)
    {
        Put<std::uint32_t>(header, 0x10, 589844u);
        Put<std::uint64_t>(header, 8, At<std::uint64_t>(header, 8) & ~0x400000000ull);
    }
    Put<std::uint32_t>(header, 8, At<std::uint32_t>(header, 8) & ~0x1000u);
    techset["header"] = Hex(header);
    techset["coveragePrepass"] = true;
    techset["coverageShadows"] = true;
    for (const auto &technique : techset.at("techniques"))
        if (technique.at("shaders").at(0).is_null() || technique.at("shaders").at(3).is_null())
            throw std::runtime_error("embedded technique set has an unassigned pass: " +
                                     technique.at("name").get<std::string>());
    KeepReferencedShaders(techset);
    return techset;
}

Json CreateVariant(const Json &base, Json &material, const std::string &stem,
                   const std::string &map, const std::string &kind)
{
    Json techset = base;
    Json lit = techset.at("techniques").back();
    auto header = Unhex(lit.at("header").get<std::string>());
    std::uint64_t other = At<std::uint64_t>(header, 0xA0);
    const bool foliage = kind.ends_with("foliage"), glass = kind.ends_with("glass");
    other = (other & ~(0xE00ull | 3ull)) | (foliage ? 0x800ull : 0xC00ull);
    Put<std::uint64_t>(header, 0xA0, other);
    Put<std::uint64_t>(header, 0xA8, glass ? 0x28054ull : 0ull);
    lit["header"] = Hex(header);
    const auto identity = Sha256(header);
    techset["name"] = "tw/mw120r_" + map + "_" + kind + "_" + Hex(std::span(identity).first(6));
    lit["name"] = "TECHNIQUE_LIT_FORWARDPLUS_BITMASK_mw120r_" + map + "_" + kind;
    PatchStateIdentity(lit, header);
    Json techniques = Json::array();
    if (foliage)
    {
        for (const auto &source : techset.at("techniques"))
        {
            auto coverageHeader = Unhex(source.at("header").get<std::string>());
            if (At<std::uint32_t>(coverageHeader, 8) == 34)
                continue;
            Json coverage = source;
            Put<std::uint64_t>(coverageHeader, 0xA0,
                               At<std::uint64_t>(coverageHeader, 0xA0) & ~3ull);
            coverage["header"] = Hex(coverageHeader);
            PatchStateIdentity(coverage, coverageHeader);
            techniques.push_back(coverage);
        }
    }
    techniques.push_back(lit);
    techset["techniques"] = std::move(techniques);
    auto techsetHeader = Unhex(techset.at("header").get<std::string>());
    std::uint64_t flags = At<std::uint64_t>(techsetHeader, 8) & ~(0x8080ull | 1ull);
    if (foliage)
        flags |= 0xA1;
    else if (glass)
        flags = (flags & ~0x20ull) | 1ull;
    Put<std::uint64_t>(techsetHeader, 8, flags);
    std::fill(techsetHeader.begin() + 24, techsetHeader.begin() + 56, std::uint8_t{0});
    for (const auto &technique : techset.at("techniques"))
    {
        const auto techniqueHeader = Unhex(technique.at("header").get<std::string>());
        const unsigned type = At<std::uint32_t>(techniqueHeader, 8);
        if (type >= 256)
            throw std::runtime_error("technique type exceeds the Replay mask");
        techsetHeader[24 + type / 8] |= static_cast<std::uint8_t>(1u << (type % 8));
    }
    techset["header"] = Hex(techsetHeader);
    KeepReferencedShaders(techset);
    const std::string materialFile = stem + "." + kind + ".material.json";
    const std::string techsetFile = stem + "." + kind + ".techset.json";
    Json variant = material;
    variant["techset"] = techset.at("name");
    variant["techsetDefinition"] = techsetFile;
    return {{"definition", materialFile},
            {"techsetFile", techsetFile},
            {"techset", std::move(techset)},
            {"material", std::move(variant)}};
}

Image Downsample(const Image &source, const bool color)
{
    const unsigned width = std::max(1u, source.width / 2), height = std::max(1u, source.height / 2);
    Image output{width, height,
                 std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            float sum[4]{};
            for (unsigned iy = 0; iy < 2; ++iy)
                for (unsigned ix = 0; ix < 2; ++ix)
                {
                    const unsigned sx = std::min(source.width - 1, x * 2 + ix);
                    const unsigned sy = std::min(source.height - 1, y * 2 + iy);
                    const std::size_t offset =
                        (static_cast<std::size_t>(sy) * source.width + sx) * 4;
                    const float alpha = source.rgba[offset + 3] / 255.0f;
                    for (unsigned channel = 0; channel < 4; ++channel)
                    {
                        const float value = color && channel < 3
                                                ? LinearByte(source.rgba[offset + channel]) * alpha
                                                : source.rgba[offset + channel] / 255.0f;
                        sum[channel] += value * 0.25f;
                    }
                }
            for (unsigned channel = 0; channel < 4; ++channel)
            {
                auto &target = output.rgba[(static_cast<std::size_t>(y) * width + x) * 4 + channel];
                const float value = sum[channel];
                if (color && channel < 3)
                    target = SrgbByte(
                        std::clamp(sum[3] > 1.0e-6f ? value / sum[3] : 0.0f, 0.0f, 1.0f));
                else
                    target = static_cast<std::uint8_t>(
                        std::clamp(std::lround(value * 255.0f), 0l, 255l));
            }
        }
    return output;
}

// Alpha-tested textures lose coverage in averaged mips, so thin cutouts (wire,
// fence slats, foliage) erode and vanish with distance. Scale each mip's alpha
// so the fraction of texels passing the 0.5 test matches the top level.
void PreserveAlphaCoverage(Image &image, const bool color)
{
    const auto passing = [](const Image &level, const unsigned threshold) {
        std::size_t count = 0;
        for (std::size_t index = 3; index < level.rgba.size(); index += 4)
            count += level.rgba[index] >= threshold;
        return count;
    };
    const std::size_t texels = static_cast<std::size_t>(image.width) * image.height;
    if (!texels)
        return;
    const double coverage = double(passing(image, 128)) / double(texels);
    if (coverage <= 0.0 || coverage >= 1.0)
        return;
    Image previous = image;
    for (std::size_t level = 0;; ++level)
    {
        if (previous.width == 1 && previous.height == 1)
            break;
        const unsigned width = std::max(1u, previous.width / 2);
        const unsigned height = std::max(1u, previous.height / 2);
        if (level >= image.mips.size())
            image.mips.push_back(Downsample(previous, color));
        Image &mip = image.mips[level];
        if (mip.width != width || mip.height != height)
            throw std::runtime_error("invalid authored image mip dimensions");
        std::array<std::size_t, 256> histogram{};
        for (std::size_t index = 3; index < mip.rgba.size(); index += 4)
            ++histogram[mip.rgba[index]];
        // Lowest alpha whose pass count still reaches the top-level coverage.
        const auto wanted = static_cast<std::size_t>(
            std::ceil(coverage * double(width) * double(height)));
        unsigned threshold = 255;
        for (std::size_t total = 0; threshold > 0; --threshold)
        {
            total += histogram[threshold];
            if (total >= wanted)
                break;
        }
        threshold = std::max(threshold, 1u);
        // Cap the boost: large scales pushed faint gap texels (white lattice
        // gaps) over the cutoff and made them flicker between mips.
        const float scale = std::min(128.0f / float(threshold), 2.0f);
        for (std::size_t index = 3; index < mip.rgba.size(); index += 4)
            mip.rgba[index] = static_cast<std::uint8_t>(
                std::min(255l, std::lround(mip.rgba[index] * scale)));
        previous = mip;
    }
}

std::vector<std::uint8_t> MipChain(Image image, const unsigned levels, const bool color)
{
    std::vector<std::uint8_t> output;
    auto authoredMips = std::move(image.mips);
    image.mips.clear();
    for (unsigned level = 0; level < levels; ++level)
    {
        output.insert(output.end(), image.rgba.begin(), image.rgba.end());
        output.resize((output.size() + 15) & ~std::size_t{15});
        if (level + 1 < levels)
        {
            const unsigned width = std::max(1u, image.width / 2);
            const unsigned height = std::max(1u, image.height / 2);
            if (level < authoredMips.size())
            {
                if (authoredMips[level].width != width ||
                    authoredMips[level].height != height ||
                    authoredMips[level].rgba.size() !=
                        static_cast<std::size_t>(width) * height * 4)
                    throw std::runtime_error("invalid authored image mip dimensions");
                image = std::move(authoredMips[level]);
            }
            else
                image = Downsample(image, color);
        }
    }
    return output;
}

// Fast BC7 mode 6 block (one subset, 7-bit RGBA endpoints with p-bits,
// 4-bit indices): principal-axis endpoints, all four p-bit pairs scored.
// DirectXTex's reference BC7 search took hours for an 8192 atlas.
void EncodeBc7Mode6Block(const std::uint8_t (&pixels)[16][4], std::uint8_t *out)
{
    static constexpr int weights[16]{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    float mean[4]{};
    for (const auto &pixel : pixels)
        for (int c = 0; c < 4; ++c)
            mean[c] += pixel[c] / 16.0f;
    float covariance[4][4]{};
    for (const auto &pixel : pixels)
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b)
                covariance[a][b] += (pixel[a] - mean[a]) * (pixel[b] - mean[b]);
    float axis[4]{1, 1, 1, 1};
    for (int iteration = 0; iteration < 8; ++iteration)
    {
        float next[4]{};
        for (int a = 0; a < 4; ++a)
            for (int b = 0; b < 4; ++b)
                next[a] += covariance[a][b] * axis[b];
        float length = 0;
        for (const float value : next)
            length = std::max(length, std::abs(value));
        if (length < 1e-6f)
            break;
        for (int a = 0; a < 4; ++a)
            axis[a] = next[a] / length;
    }
    float low = 1e30f, high = -1e30f;
    for (const auto &pixel : pixels)
    {
        float t = 0;
        for (int c = 0; c < 4; ++c)
            t += (pixel[c] - mean[c]) * axis[c];
        low = std::min(low, t);
        high = std::max(high, t);
    }
    float axisLength = 0;
    for (const float value : axis)
        axisLength += value * value;
    if (axisLength > 0)
    {
        low /= axisLength;
        high /= axisLength;
    }
    float ends[2][4];
    for (int c = 0; c < 4; ++c)
    {
        ends[0][c] = std::clamp(mean[c] + axis[c] * low, 0.0f, 255.0f);
        ends[1][c] = std::clamp(mean[c] + axis[c] * high, 0.0f, 255.0f);
    }
    int bestError = INT_MAX;
    int bestQ[2][4]{}, bestP[2]{};
    std::uint8_t bestIndex[16]{};
    for (int pbits = 0; pbits < 4; ++pbits)
    {
        const int p[2]{pbits & 1, pbits >> 1};
        int q[2][4], value[2][4];
        for (int e = 0; e < 2; ++e)
            for (int c = 0; c < 4; ++c)
            {
                q[e][c] = std::clamp(static_cast<int>(std::lround((ends[e][c] - p[e]) / 2.0f)), 0, 127);
                value[e][c] = q[e][c] * 2 + p[e];
            }
        int palette[16][4];
        for (int i = 0; i < 16; ++i)
            for (int c = 0; c < 4; ++c)
                palette[i][c] = ((64 - weights[i]) * value[0][c] + weights[i] * value[1][c] + 32) >> 6;
        int error = 0;
        std::uint8_t index[16];
        for (int pixel = 0; pixel < 16 && error < bestError; ++pixel)
        {
            int best = INT_MAX;
            for (int i = 0; i < 16; ++i)
            {
                int d = 0;
                for (int c = 0; c < 4; ++c)
                {
                    const int delta = palette[i][c] - pixels[pixel][c];
                    d += delta * delta;
                }
                if (d < best)
                {
                    best = d;
                    index[pixel] = static_cast<std::uint8_t>(i);
                }
            }
            error += best;
        }
        if (error < bestError)
        {
            bestError = error;
            std::memcpy(bestQ, q, sizeof(q));
            bestP[0] = p[0];
            bestP[1] = p[1];
            std::memcpy(bestIndex, index, sizeof(index));
        }
    }
    if (bestIndex[0] & 8)
    {
        // The anchor index stores only three bits: swap the endpoints.
        for (int c = 0; c < 4; ++c)
            std::swap(bestQ[0][c], bestQ[1][c]);
        std::swap(bestP[0], bestP[1]);
        for (auto &index : bestIndex)
            index = static_cast<std::uint8_t>(15 - index);
    }
    std::memset(out, 0, 16);
    unsigned position = 0;
    const auto put = [&](const unsigned value, const unsigned bits) {
        for (unsigned bit = 0; bit < bits; ++bit, ++position)
            if (value >> bit & 1u)
                out[position >> 3] |= static_cast<std::uint8_t>(1u << (position & 7));
    };
    put(1u << 6, 7); // mode 6
    for (int c = 0; c < 4; ++c)
    {
        put(static_cast<unsigned>(bestQ[0][c]), 7);
        put(static_cast<unsigned>(bestQ[1][c]), 7);
    }
    put(static_cast<unsigned>(bestP[0]), 1);
    put(static_cast<unsigned>(bestP[1]), 1);
    put(bestIndex[0], 3);
    for (int pixel = 1; pixel < 16; ++pixel)
        put(bestIndex[pixel], 4);
}

// Encoded caches are reused only by the converter build that wrote them: an
// encoder change keeps the input digest, so the key must name the encoder too.
const std::string &ConverterIdentity()
{
    static const std::string identity = [] {
        std::wstring path(MAX_PATH, L'\0');
        path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
        const auto bytes = ReadBytes(std::filesystem::path(path));
        const auto digest = Sha256(bytes);
        return Hex(std::span(digest).first(8));
    }();
    return identity;
}

// BC7 encoding of a square RGBA8 mip chain laid out by MipChain.
// Values pass through unchanged, so sRGB data stays sRGB for format 45.
std::vector<std::uint8_t> EncodeBc7Chain(const std::vector<std::uint8_t> &rgba,
                                         const unsigned size, const unsigned levels,
                                         const std::filesystem::path &cacheRoot)
{
    const auto inputDigest = Sha256(rgba);
    const auto cacheFile =
        cacheRoot.empty() ? std::filesystem::path{}
                          : cacheRoot / ("atlas_" + Hex(std::span(inputDigest).first(16)) + "_" + ConverterIdentity() + ".bc7");
    std::error_code cacheError;
    if (!cacheFile.empty() && std::filesystem::is_regular_file(cacheFile, cacheError))
        return ReadBytes(cacheFile);
    struct Level
    {
        unsigned width;
        std::size_t source;
        std::size_t target;
    };
    std::vector<Level> table;
    std::size_t source = 0, target = 0;
    for (unsigned level = 0, width = size; level < levels; ++level, width = std::max(4u, width / 2))
    {
        if (width % 4)
            throw std::runtime_error("BC7 atlas level is not a multiple of four");
        table.push_back({width, source, target});
        source += (std::size_t{width} * width * 4 + 15) & ~std::size_t{15};
        target += std::size_t{width / 4} * (width / 4) * 16;
    }
    if (source != rgba.size())
        throw std::runtime_error("BC7 atlas input does not match its mip chain");
    std::vector<std::uint8_t> output(target);
    std::vector<std::pair<unsigned, unsigned>> rows; // level, block row
    for (unsigned level = 0; level < table.size(); ++level)
        for (unsigned row = 0; row < table[level].width / 4; ++row)
            rows.emplace_back(level, row);
    std::atomic<std::size_t> next{0};
    const auto encode = [&] {
        std::uint8_t block[16][4];
        for (std::size_t task; (task = next.fetch_add(1)) < rows.size();)
        {
            const Level &level = table[rows[task].first];
            const unsigned blockY = rows[task].second;
            const unsigned blocksWide = level.width / 4;
            for (unsigned blockX = 0; blockX < blocksWide; ++blockX)
            {
                for (unsigned y = 0; y < 4; ++y)
                    std::memcpy(block[y * 4],
                                rgba.data() + level.source +
                                    ((std::size_t{blockY} * 4 + y) * level.width + blockX * 4) * 4,
                                16);
                EncodeBc7Mode6Block(block, output.data() + level.target +
                                               (std::size_t{blockY} * blocksWide + blockX) * 16);
            }
        }
    };
    const unsigned workerCount = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < workerCount; ++worker)
        workers.emplace_back(encode);
    for (auto &worker : workers)
        worker.join();
    if (!cacheFile.empty())
        WriteBytes(cacheFile, output);
    return output;
}

std::string ImageName(const std::string &map, const unsigned channel,
                      const std::vector<std::uint8_t> &pixels)
{
    const auto digest = Sha256(pixels);
    return "mw120r/" + map + "_" + std::to_string(channel) + "_" + Hex(std::span(digest).first(8));
}

// IW3 specular RGB is sRGB reflectance (dielectrics sit near F0 0.04, about
// 56/255), whereas Replay's intensity is a multiplier of its default dielectric
// (255 for "&defaultspec"). Returns the intensity in every channel.
Image ReplaySpecularIntensity(Image specular)
{
    specular.mips.clear();
    static const auto reflectance = [] {
        std::array<std::uint8_t, 256> table{};
        for (unsigned value = 0; value < table.size(); ++value)
            table[value] = static_cast<std::uint8_t>(std::min(
                255l, std::lround(255.0f * LinearByte(static_cast<std::uint8_t>(value)) / 0.04f)));
        return table;
    }();
    for (std::size_t offset = 0; offset + 3 < specular.rgba.size(); offset += 4)
    {
        const auto intensity = reflectance[std::max(
            {specular.rgba[offset], specular.rgba[offset + 1], specular.rgba[offset + 2]})];
        specular.rgba[offset] = specular.rgba[offset + 1] = specular.rgba[offset + 2] =
            specular.rgba[offset + 3] = intensity;
    }
    return specular;
}

// Replay "<n>&<g>": octahedral normal in G/A and gloss in R. IW3 gloss is the
// specular map's alpha; without one the identity normal keeps gloss 128.
Image ReplayNormalGloss(const Image *normal, const Image *response)
{
    Image packed = normal ? EncodeNativeWorldNormal(*normal)
                          : Image{1, 1, std::vector<std::uint8_t>{128, 128, 255, 128}};
    if (response)
    {
        if (packed.width == 1 && packed.height == 1)
        {
            const auto identity = packed.rgba;
            packed = {response->width, response->height,
                      std::vector<std::uint8_t>(std::size_t(response->width) * response->height * 4)};
            for (std::size_t offset = 0; offset < packed.rgba.size(); offset += 4)
                std::copy(identity.begin(), identity.end(), packed.rgba.begin() + offset);
        }
        packed.mips.clear();
        const Image gloss = Resize(*response, packed.width, packed.height, false);
        for (std::size_t offset = 0; offset < packed.rgba.size(); offset += 4)
            packed.rgba[offset] = gloss.rgba[offset + 3];
    }
    return packed;
}

std::string FxMaterialName(const std::string &map, const SourceFxMaterial &source)
{
    const std::string key = map + '\x1f' + source.name + '\x1f' + source.techniqueSet;
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t *>(key.data()), key.size());
    const auto digest = Sha256(bytes);
    return "elcq/mw120r_fx_" + Hex(std::span(digest).first(8));
}

Json BuildFxMaterialDefinition(const SourceFxMaterial &source)
{
    constexpr std::string_view alphaSimpleTechset =
        "elcq/unlit_6_effect_bad_ta0_802_4_0_1_0_0_0_100000023_0_0_2_0_0";
    constexpr std::string_view alphaTechset =
        "elcq/unlit_6_effect_bad_ta0_802_1004_0_1_0_0_0_100000023_0_0_2_0_0";
    constexpr std::string_view additiveTechset =
        "elcq/unlit_6_effect_bad_tca_802_10a4_0_1_0_0_0_100060023_0_0_3_0_0";
    constexpr std::string_view additiveSimpleTechset =
        "elcq/unlit_6_effect_bad_tca_802_4_0_1_0_0_0_100060023_0_0_3_0_0";
    constexpr std::string_view cloudTechset =
        "elcq/unlit_6_effect_bad_ta0_802_4_0_1_0_0_0_100020023_0_0_2_0_0";
    if (source.family == FxMaterialFamily::decal)
    {
        std::vector<std::uint8_t> info(32);
        Put<std::uint32_t>(info, 4, 98304u);
        Put<std::uint32_t>(info, 0xC, 0x800000u);
        info[0x10] = 3;
        info[0x11] = 29;
        info[0x1A] = 1;
        info[0x1B] = 1;
        return {{"schema", 1},
                {"source", source.name},
                {"info", Hex(info)},
                {"techset", "null"},
                {"textures", Json::array()},
                {"constants", ""},
                {"bufferIndices", ""},
                {"buffers", Json::array()},
                {"imageDefinitions", Json::array()},
                {"decalVolumeMaterial", "i/vfx_decal_surface_glass_2"}};
    }
    const bool alpha = source.family == FxMaterialFamily::alpha;
    const bool simpleAlpha = alpha && source.rows == 1 && source.columns == 1;
    const bool additive = source.family == FxMaterialFamily::additive ||
                          source.family == FxMaterialFamily::additiveFeather;
    const bool simpleAdditive = source.family == FxMaterialFamily::additive &&
                                source.rows == 1 && source.columns == 1;
    const bool cloud = source.family == FxMaterialFamily::cloud;
    if (!alpha && source.family != FxMaterialFamily::alphaFeather && !additive && !cloud)
        throw std::runtime_error("IW3 FX material family is not ready for Replay emission");

    std::vector<std::uint8_t> info(32);
    Put<std::uint32_t>(info, 0, 1u);
    Put<std::uint32_t>(info, 0xC, 0x40200Cu);
    info[0x10] = 4;
    info[0x11] = 35;
    info[0x12] = 16;
    info[0x14] = 1;
    info[0x15] = cloud || simpleAlpha ? 1 : simpleAdditive ? 2 : additive ? 4 : 3;
    info[0x16] = cloud || simpleAlpha || simpleAdditive ? 1 : 2;
    info[0x1A] = static_cast<std::uint8_t>(source.rows);
    info[0x1B] = static_cast<std::uint8_t>(source.columns);

    std::vector<std::uint8_t> constants;
    const auto appendConstant = [&](const std::uint32_t index, const auto &value) {
        const std::size_t offset = constants.size();
        constants.resize(offset + 20);
        Put(constants, offset, index);
        std::memcpy(constants.data() + offset + 4, value.data(), 16);
    };
    if (additive)
    {
        // Native Replay one-image additive contracts carry a target-owned
        // effect-color setup at index 9. The shipped 1x1 tca/4 and atlas
        // tca/10a4 fixtures use distinct third words.
        appendConstant(9u,
                       std::array<std::uint32_t, 4>{0xFFFFFFFFu, 0x00FF17E7u,
                                                     simpleAdditive ? 0x4C2B5D08u
                                                                    : 0x4C2B5CAFu,
                                                     0u});
    }
    appendConstant(36u, source.tint);
    if (!cloud && !simpleAlpha && !simpleAdditive)
    {
        const std::array<float, 4> atlas{static_cast<float>(source.columns),
                                         static_cast<float>(source.rows),
                                         1.0f / static_cast<float>(source.columns),
                                         1.0f / static_cast<float>(source.rows)};
        appendConstant(94u, atlas);
        const std::array<std::uint32_t, 4> atlasBits{
            source.rows * source.columns, source.columns - 1,
            static_cast<std::uint32_t>(std::countr_zero(source.columns)), 0u};
        appendConstant(95u, atlasBits);

        std::vector<std::uint8_t> vertexConstants(160);
        std::memcpy(vertexConstants.data() + 128, atlas.data(), 16);
        std::memcpy(vertexConstants.data() + 144, atlasBits.data(), 16);
        std::vector<std::uint8_t> bufferIndices(195, 0xFF);
        bufferIndices[29] = 0;
        bufferIndices[30] = 1;

        return {{"schema", 1},
                {"source", source.name},
                {"info", Hex(info)},
                {"techset", additive ? additiveTechset : alphaTechset},
                {"textures", Json::array({{{"header", "1200000000000000"},
                                            {"image", ""}}})},
                {"constants", Hex(constants)},
                {"bufferIndices", Hex(bufferIndices)},
                {"buffers", Json::array({Json::array({Hex(vertexConstants), "", "", ""}),
                                          Json::array({"", "", "", ""})})},
                {"imageDefinitions", Json::array()}};
    }

    std::vector<std::uint8_t> pixelConstants(48);
    if (simpleAdditive)
    {
        const std::array<std::uint32_t, 4> effectColor{
            0xFFFFFFFFu, 0x00FF17E7u, 0x4C2B5D08u, 0u};
        std::memcpy(pixelConstants.data() + 16, effectColor.data(), 16);
    }
    std::memcpy(pixelConstants.data() + 32, source.tint.data(), 16);
    std::vector<std::uint8_t> bufferIndices(195, 0xFF);
    bufferIndices[30] = 0;
    return {{"schema", 1},
            {"source", source.name},
            {"info", Hex(info)},
            {"techset", cloud ? cloudTechset
                               : simpleAdditive ? additiveSimpleTechset : alphaSimpleTechset},
            {"textures", Json::array({{{"header", "1200000000000000"},
                                        {"image", ""}}})},
            {"constants", Hex(constants)},
            {"bufferIndices", Hex(bufferIndices)},
            {"buffers", Json::array({Json::array({"", "", "", Hex(pixelConstants)})})},
            {"imageDefinitions", Json::array()}};
}
} // namespace

RenderPlan PrepareRenderAssets(const std::filesystem::path &exportRoot, const Json &world,
                               const std::vector<std::string> &surfaceMaterials,
                               const std::vector<std::string> &worldMaterials,
                               const std::vector<std::string> &modelMaterials,
                               const std::vector<std::string> &entityMaterials,
                               const std::vector<std::string> &fxMaterials,
                               const std::vector<std::string> &shadowWorldMaterials,
                               const std::vector<std::string> &shadowModelMaterials,
                               const std::vector<std::filesystem::path> &sourcePaths,
                               const std::filesystem::path &mapDirectory, const std::string &map,
                               const std::filesystem::path &cacheRoot,
                               const std::string &surfaceShaderDefines,
                               const unsigned staticModelInstances,
                               const EntityLightVolume *entityLightVolume)
{
    zt::LapTimer timing("render");
    std::set<std::string> unique(surfaceMaterials.begin(), surfaceMaterials.end());
    std::vector<SourceMaterial> materials;
    materials.reserve(unique.size());
    std::unordered_map<std::string, std::vector<Image>> images;
    auto image = [&](const std::string &name) -> const std::vector<Image> & {
        auto found = images.find(name);
        if (found == images.end())
            found = images.emplace(name, LoadImage(exportRoot, sourcePaths, name)).first;
        return found->second;
    };
    for (const std::string &name : unique)
    {
        SourceMaterial material = ReadMaterial(exportRoot, sourcePaths, name);
        if (material.kind != SurfaceKind::skipped)
        {
            const auto &color = image(material.color);
            if (color.size() != 1)
                throw std::runtime_error("IW3 material color map is a cubemap: " + name);
            ClassifyFoliage(material, color.front());
        }
        materials.push_back(std::move(material));
    }
    // IW3 DXT5 normal maps keep Y replicated in RGB and X in alpha (red equals
    // green texel for texel). Every consumer here reads X from red, so move
    // alpha into red once per image; this left one bump axis flat before.
    std::set<std::string> swizzledNormals;
    std::size_t movedNormals = 0;
    for (const SourceMaterial &material : materials)
    {
        if (material.normal.empty() || material.normal == "$identitynormalmap" ||
            !swizzledNormals.insert(material.normal).second)
            continue;
        image(material.normal);
        auto &levels = images.at(material.normal);
        const auto replicatedY = [](const Image &level) {
            std::size_t matching = 0, texels = level.rgba.size() / 4;
            std::uint8_t lowAlpha = 255, highAlpha = 0;
            for (std::size_t offset = 0; offset < level.rgba.size(); offset += 4)
            {
                matching += std::abs(int(level.rgba[offset]) - int(level.rgba[offset + 1])) <= 8;
                lowAlpha = std::min(lowAlpha, level.rgba[offset + 3]);
                highAlpha = std::max(highAlpha, level.rgba[offset + 3]);
            }
            return texels && matching * 100 >= texels * 97 && highAlpha - lowAlpha > 16;
        };
        if (levels.empty() || !replicatedY(levels.front()))
            continue;
        const auto swizzle = [](Image &level) {
            for (std::size_t offset = 0; offset < level.rgba.size(); offset += 4)
            {
                level.rgba[offset] = level.rgba[offset + 3];
                level.rgba[offset + 3] = 255;
            }
        };
        for (Image &level : levels)
        {
            swizzle(level);
            for (Image &mip : level.mips)
                swizzle(mip);
        }
        ++movedNormals;
    }
    zt::info("iw3: moved X from alpha to red in %zu of %zu DXT5 normal map(s)", movedNormals,
             swizzledNormals.size());
    timing.lap("material images");

    std::map<std::string, std::size_t> tileNumbers;
    for (const auto &material : materials)
        if (material.kind != SurfaceKind::skipped)
            tileNumbers.emplace(TileKey(material), 0);
    std::size_t nextTile = 0;
    const std::size_t visibleCount = tileNumbers.size();
    RenderPlan plan;
    const auto &sourceProbes = world.at("reflection_probes");
    if (!sourceProbes.is_array() || sourceProbes.empty() || sourceProbes.size() > 256)
        throw std::runtime_error("invalid IW3 reflection-probe table");
    plan.reflectionProbes.reserve(sourceProbes.size());
    std::vector<Cubemap> probeCubemaps;
    probeCubemaps.reserve(sourceProbes.size());
    // IW3 reserves probe 0 for R_CreateDefaultProbe's flat diagnostic image.
    // R_FindNearestReflectionProbe/R_AddAllProbesToAllCells skip it when
    // authored captures exist. Only those captures belong in Replay's array,
    // SH table and spatial instances; retain the sole default for unprobed maps.
    const std::size_t firstProbe = sourceProbes.size() > 1 ? 1 : 0;
    for (std::size_t index = firstProbe; index < sourceProbes.size(); ++index)
    {
        const auto &source = sourceProbes.at(index);
        const auto &sourceOrigin = source.at("origin");
        const std::string sourceName = source.at("image").get<std::string>();
        if (!sourceOrigin.is_array() || sourceOrigin.size() != 3 || sourceName.empty() ||
            sourceName.find("..") != std::string::npos ||
            sourceName.find(':') != std::string::npos ||
            sourceName.find('\\') != std::string::npos || sourceName.front() == '/')
            throw std::runtime_error("invalid IW3 reflection probe");
        ReflectionProbePlan probe;
        probe.sourceIndex = index;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            probe.origin[axis] = sourceOrigin.at(axis).get<float>();
            if (!std::isfinite(probe.origin[axis]) || std::abs(probe.origin[axis]) > 100000.0f)
                throw std::runtime_error("IW3 reflection-probe origin is outside Replay range");
        }
        std::string sourceFile = sourceName;
        std::ranges::replace(sourceFile, '*', '_');
        const auto sourcePath = exportRoot / "images" / (sourceFile + ".dds");
        std::error_code error;
        if (!std::filesystem::is_regular_file(sourcePath, error))
            throw std::runtime_error("missing IW3 reflection-probe image '" + sourceName + "'");
        Cubemap cubemap = DecodeDdsCubemap(sourcePath);
        probe.sh = ProjectReflectionSh(cubemap);
        const std::string filename = map + "_reflection_probe_" + std::to_string(index) + ".rgba";
        const auto digest = Sha256(cubemap.residentPixels);
        const std::string targetName = "mw120r/" + map + "_reflection_probe_" +
                                       std::to_string(index) + "_" +
                                       Hex(std::span(digest).first(8));
        WriteBytes(mapDirectory / filename, cubemap.residentPixels);
        probe.image = {{"name", targetName},
                       {"width", cubemap.width},
                       {"height", cubemap.height},
                       {"rgba8", filename},
                       {"format", 7},
                       {"flags", 0x8001u},
                       {"depth", 1},
                       {"numElements", 1},
                       {"semantic", 1},
                       {"category", 1},
                       {"mipCount", cubemap.mipCount}};
        plan.reflectionProbes.push_back(std::move(probe));
        probeCubemaps.push_back(std::move(cubemap));
    }
    {
        std::array<std::array<double, 3>, 9> average{};
        for (const auto &probe : plan.reflectionProbes)
            for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
                for (unsigned channel = 0; channel < 3; ++channel)
                    average[coefficient][channel] +=
                        probe.sh[channel][coefficient] / plan.reflectionProbes.size();
        plan.glassEnvironment = "#define MAP_GLASS_SH {";
        for (unsigned coefficient = 0; coefficient < 9; ++coefficient)
            plan.glassEnvironment +=
                std::string(coefficient ? "," : "") + "float3(" +
                HlslNumber(static_cast<float>(average[coefficient][0])) + "," +
                HlslNumber(static_cast<float>(average[coefficient][1])) + "," +
                HlslNumber(static_cast<float>(average[coefficient][2])) + ")";
        plan.glassEnvironment += "}\n";
        // Every converted surface variant shares this preamble (map fog included).
        plan.glassEnvironment += surfaceShaderDefines;
    }
    constexpr unsigned arrayWidth = 256;
    constexpr unsigned arrayMipCount = 6;
    timing.lap("probe cubemaps and SH");
    // The reference BC6H encoder dominates conversion time, but its output
    // depends only on the probe images; reuse it across rebuilds.
    std::vector<std::uint8_t> probeKey{'p', 'r', 'o', 'b', 'e', '-', 'b', 'c', '6', 'h', '-', 'v', '1'};
    const auto appendKey = [&probeKey](const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            probeKey.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    appendKey(arrayWidth);
    appendKey(arrayMipCount);
    for (const Cubemap &cubemap : probeCubemaps)
    {
        appendKey(cubemap.width);
        appendKey(cubemap.height);
        appendKey(cubemap.mipCount);
        for (const auto &face : cubemap.faces)
            for (const Image &level : face)
            {
                appendKey(level.width);
                appendKey(level.height);
                probeKey.insert(probeKey.end(), level.rgba.begin(), level.rgba.end());
            }
    }
    const auto probeDigest = Sha256(probeKey);
    const auto probeCache =
        cacheRoot.empty() ? std::filesystem::path{}
                          : cacheRoot / ("probes_" + Hex(std::span(probeDigest).first(16)) + "_" + ConverterIdentity() + ".bc6h");
    std::vector<std::uint8_t> arrayPixels;
    std::error_code cacheError;
    if (!probeCache.empty() && std::filesystem::is_regular_file(probeCache, cacheError))
    {
        arrayPixels = ReadBytes(probeCache);
        zt::info("iw3: reusing cached reflection-probe array %s", probeCache.string().c_str());
    }
    else
    {
        arrayPixels = BuildReflectionProbeArray(probeCubemaps, arrayWidth, arrayMipCount);
        if (!probeCache.empty())
            WriteBytes(probeCache, arrayPixels);
    }
    timing.lap("probe BC6H array");
    constexpr std::size_t bytesPerSlice = 87360;
    if (arrayPixels.size() != probeCubemaps.size() * bytesPerSlice)
        throw std::runtime_error("BC6H reflection-probe array has an invalid resident size");
    const auto arrayDigest = Sha256(arrayPixels);
    const std::string arrayFile = map + "_reflection_probe_array_octahedron.bc6h";
    const std::string arrayName = "mw120r/" + map + "_reflection_probe_array_octahedron_" +
                                  Hex(std::span(arrayDigest).first(8));
    WriteBytes(mapDirectory / arrayFile, arrayPixels);
    plan.reflectionProbeArrayImage = {{"name", arrayName},
                                      {"width", arrayWidth},
                                      {"height", arrayWidth},
                                      {"bc6h", arrayFile},
                                      {"format", 42},
                                      {"flags", 0x10020001u},
                                      {"depth", 1},
                                      {"numElements", probeCubemaps.size()},
                                      {"semantic", 1},
                                      {"category", 1},
                                      {"mipCount", arrayMipCount}};
    timing.lap("materials and reflection probes");
    std::vector<std::array<unsigned, 2>> lightmapDimensions;
    for (const auto &pair : world.value("lightmaps", Json::array()))
    {
        if (!pair.is_array() || pair.size() != 2 || pair.at(1).at("format") != 21 ||
            pair.at(1).at("height").get<unsigned>() % 2)
            throw std::runtime_error("unsupported IW3 lightmap format");
        lightmapDimensions.push_back(
            {pair.at(1).at("width"), pair.at(1).at("height").get<unsigned>() / 2});
    }
    unsigned columns = 0, cell = 0;
    // One 8192 BC7 atlas keeps 256-pixel material tiles where 287 Nuketown
    // materials left 128 at 4096. Multiple IW3 lightmaps keep 4096: the native
    // Replay lightmap below is copied 1:1 from the atlas pixels.
    const unsigned atlasSize = lightmapDimensions.size() > 1 ? 4096u : 8192u;
    plan.atlasSize = atlasSize;
    std::vector<bool> occupied;
    for (const unsigned candidate : {4u, 8u, 16u, 32u})
    {
        const unsigned candidateCell = atlasSize / candidate;
        std::vector<bool> candidateOccupied(static_cast<std::size_t>(candidate) * candidate);
        std::vector<LightmapRectangle> rectangles;
        bool fits = true;
        for (const auto &dimensions : lightmapDimensions)
        {
            const unsigned cellsWide = (dimensions[0] + candidateCell - 1) / candidateCell;
            const unsigned cellsHigh = (dimensions[1] + candidateCell - 1) / candidateCell;
            bool placed = false;
            if (cellsWide <= candidate && cellsHigh <= candidate)
                for (int row = static_cast<int>(candidate - cellsHigh); row >= 0 && !placed; --row)
                    for (unsigned column = 0; column + cellsWide <= candidate && !placed; ++column)
                    {
                        bool free = true;
                        for (unsigned y = 0; y < cellsHigh && free; ++y)
                            for (unsigned x = 0; x < cellsWide; ++x)
                                free &= !candidateOccupied[(row + y) * candidate + column + x];
                        if (!free)
                            continue;
                        for (unsigned y = 0; y < cellsHigh; ++y)
                            for (unsigned x = 0; x < cellsWide; ++x)
                                candidateOccupied[(row + y) * candidate + column + x] = true;
                        rectangles.push_back({column * candidateCell,
                                              static_cast<unsigned>(row) * candidateCell,
                                              dimensions[0], dimensions[1]});
                        placed = true;
                    }
            if (!placed)
            {
                fits = false;
                break;
            }
        }
        const std::size_t freeCells =
            std::count(candidateOccupied.begin(), candidateOccupied.end(), false);
        if (fits && freeCells >= visibleCount + 6)
        {
            columns = candidate;
            cell = candidateCell;
            occupied = std::move(candidateOccupied);
            plan.lightmaps = std::move(rectangles);
            break;
        }
    }
    if (!columns)
        throw std::runtime_error("IW3 material and lightmap data exceed the Replay atlas");
    std::vector<std::size_t> freeTiles;
    for (std::size_t index = 0; index < occupied.size(); ++index)
        if (!occupied[index])
            freeTiles.push_back(index);
    nextTile = 0;
    for (auto &[key, number] : tileNumbers)
        number = freeTiles[nextTile++];
    const std::size_t atlasBytes = std::size_t{atlasSize} * atlasSize * 4;
    std::array<Image, 3> atlases{Image{atlasSize, atlasSize, std::vector<std::uint8_t>(atlasBytes)},
                                 Image{atlasSize, atlasSize, std::vector<std::uint8_t>(atlasBytes)},
                                 Image{atlasSize, atlasSize, std::vector<std::uint8_t>(atlasBytes)}};
    struct AtlasTileMipSource
    {
        std::size_t tile;
        std::string image;
        std::array<float, 4> tint;
    };
    std::array<std::vector<AtlasTileMipSource>, 3> atlasTileMips;

    plan.columns = columns;
    std::set<std::size_t> writtenTiles;
    for (const SourceMaterial &material : materials)
    {
        MaterialPlan item;
        item.kind = material.kind;
        item.castsShadow = material.castsShadow;
        item.flags = material.flags;
        item.cullMode = material.cullMode;
        item.depthBias = material.depthBias;
        item.sourceSurfaceTypeBits = material.surfaceTypeBits;
        item.environment = material.environment;
        if (material.kind != SurfaceKind::skipped)
        {
            item.tile = tileNumbers.at(TileKey(material));
            if (writtenTiles.insert(item.tile).second)
            {
                const auto retainMips = [&](const unsigned channel, const std::string &name) {
                    if (name.empty())
                        return;
                    const Image &source = image(name).front();
                    if (source.width == cell && source.height == cell && !source.mips.empty())
                        atlasTileMips[channel].push_back(
                            {item.tile, name, material.tint});
                };
                const Image color =
                    Resize(image(material.color).front(), cell, cell, true, material.tint);
                Paste(atlases[0], color, static_cast<unsigned>(item.tile % columns) * cell,
                      static_cast<unsigned>(item.tile / columns) * cell);
                retainMips(0, material.color);
                if (!material.normal.empty())
                {
                    Paste(atlases[1], Resize(image(material.normal).front(), cell, cell, false),
                          static_cast<unsigned>(item.tile % columns) * cell,
                          static_cast<unsigned>(item.tile / columns) * cell);
                    retainMips(1, material.normal);
                }
                if (!material.response.empty())
                {
                    Paste(atlases[2], Resize(image(material.response).front(), cell, cell, false),
                          static_cast<unsigned>(item.tile % columns) * cell,
                          static_cast<unsigned>(item.tile / columns) * cell);
                    retainMips(2, material.response);
                }
            }
            plan.hasCutout |= item.kind == SurfaceKind::cutout;
            plan.hasCutoutBias |= item.kind == SurfaceKind::cutout && item.depthBias;
            plan.hasOpaqueBias |= item.kind == SurfaceKind::opaque && item.depthBias;
            plan.hasGlass |= item.kind == SurfaceKind::glass;
        }
        plan.materials.emplace(material.name, item);
    }

    const std::set<std::string> worldCasters(shadowWorldMaterials.begin(),
                                             shadowWorldMaterials.end());
    const std::set<std::string> modelCasters(shadowModelMaterials.begin(),
                                             shadowModelMaterials.end());
    timing.lap("atlas tiles");
    for (const SourceMaterial &source : materials)
    {
        if (!source.castsShadow ||
            (source.kind != SurfaceKind::opaque && source.kind != SurfaceKind::cutout &&
             !source.shadowOnly))
            continue;
        const auto addCaster = [&](const bool worldDomain) {
            const auto &requested = worldDomain ? worldCasters : modelCasters;
            if (!requested.contains(source.name))
                return;
            replaysunshadow::Material caster;
            // The generated world foliage shadow variants disable culling on
            // both depth passes; model variants retain their authored cull.
            caster.cullMode = worldDomain && source.kind == SurfaceKind::cutout
                                  ? 0u
                                  : source.cullMode;
            caster.atlasTile = worldDomain;
            if (source.kind == SurfaceKind::cutout)
                caster.alphaTest = source.flags & 8u ? 2u : source.flags & 16u ? 3u : 1u;
            if (!source.color.empty())
            {
                const Image &resident = image(source.color).front();
                caster.width = worldDomain ? cell : resident.width;
                caster.height = worldDomain ? cell : resident.height;
                if (caster.alphaTest)
                {
                    const Image mask = Resize(resident, caster.width, caster.height, true,
                                              source.tint);
                    const std::size_t pixels =
                        static_cast<std::size_t>(caster.width) * caster.height;
                    caster.alpha.reserve(pixels);
                    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
                        caster.alpha.push_back(mask.rgba[pixel * 4 + 3]);
                }
            }
            else
            {
                caster.width = caster.height = 1; // Shadow-only techset is opaque.
            }
            if (plan.shadowScene.materials.size() >= UINT32_MAX)
                throw std::runtime_error("IW3 shadow material table exceeds Replay limits");
            const unsigned index = static_cast<unsigned>(plan.shadowScene.materials.size());
            plan.shadowScene.materials.push_back(std::move(caster));
            (worldDomain ? plan.worldShadowMaterials : plan.modelShadowMaterials)
                .emplace(source.name, index);
        };
        addCaster(true);
        addCaster(false);
    }

    for (std::size_t face = 0; face < plan.skyTiles.size(); ++face)
        plan.skyTiles[face] = freeTiles[nextTile++];
    const std::string skyName = world.at("sky").get<std::string>();
    // A map with no sky surfaces (an enclosed map such as mp_gulag) exports no
    // sky image. Nothing draws the sky then; a dark cubemap fills its tiles.
    if (skyName.empty() && !images.contains(skyName))
    {
        images.emplace(skyName, std::vector<Image>(6, SolidImage({24, 26, 30, 255})));
        zt::info("iw3: map has no sky image; using a dark placeholder sky cubemap");
    }
    const auto &sky = image(skyName);
    if (sky.size() != 6)
        throw std::runtime_error("IW3 sky image is not a complete cubemap: " + skyName);
    for (unsigned face = 0; face < 6; ++face)
        Paste(atlases[0], Resize(sky[face], cell, cell, true),
              static_cast<unsigned>(plan.skyTiles[face] % columns) * cell,
              static_cast<unsigned>(plan.skyTiles[face] / columns) * cell);

    std::size_t lightmapIndex = 0;
    for (const auto &pair : world.value("lightmaps", Json::array()))
    {
        if (!pair.is_array() || pair.size() != 2 || pair.at(0).at("format") != 50 ||
            pair.at(1).at("format") != 21)
            throw std::runtime_error("unsupported IW3 lightmap format");
        const auto &primary = pair.at(0), &secondary = pair.at(1);
        const unsigned width = secondary.at("width"), fullHeight = secondary.at("height"),
                       height = fullHeight / 2;
        if (!width || !height || fullHeight % 2)
            throw std::runtime_error("invalid IW3 lightmap dimensions");
        const auto firstBytes = ReadBytes(exportRoot / primary.at("file").get<std::string>());
        const auto secondBytes = ReadBytes(exportRoot / secondary.at("file").get<std::string>());
        if (firstBytes.size() != static_cast<std::size_t>(primary.at("width").get<unsigned>()) *
                                     primary.at("height").get<unsigned>() ||
            secondBytes.size() != static_cast<std::size_t>(width) * height * 8)
            throw std::runtime_error("invalid IW3 lightmap byte count");
        const auto &rectangle = plan.lightmaps.at(lightmapIndex++);
        Image primaryMask{primary.at("width"), primary.at("height"),
                          std::vector<std::uint8_t>(
                              static_cast<std::size_t>(primary.at("width").get<unsigned>()) *
                              primary.at("height").get<unsigned>() * 4)};
        for (std::size_t index = 0; index < firstBytes.size(); ++index)
            primaryMask.rgba[index * 4 + 0] = primaryMask.rgba[index * 4 + 1] =
                primaryMask.rgba[index * 4 + 2] = primaryMask.rgba[index * 4 + 3] =
                    firstBytes[index];
        const Image mask = Resize(primaryMask, width, height, false);
        Image color{width, height,
                    std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        Image normal = color, response = color;
        const std::size_t half = static_cast<std::size_t>(width) * height * 4;
        for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height; ++index)
        {
            const auto *a = secondBytes.data() + index * 4;
            const auto *b = secondBytes.data() + half + index * 4;
            color.rgba[index * 4 + 0] = a[2];
            color.rgba[index * 4 + 1] = a[1];
            color.rgba[index * 4 + 2] = a[0];
            color.rgba[index * 4 + 3] = mask.rgba[index * 4];
            normal.rgba[index * 4 + 0] = a[3];
            normal.rgba[index * 4 + 1] = b[3];
            normal.rgba[index * 4 + 2] = a[2];
            normal.rgba[index * 4 + 3] = a[1];
            response.rgba[index * 4 + 0] = b[2];
            response.rgba[index * 4 + 1] = b[1];
            response.rgba[index * 4 + 2] = b[0];
            response.rgba[index * 4 + 3] = a[0];
        }
        // Replay 1.20's native temporary lightmap atlas takes one BC4 ambient
        // fraction, one R11G11B10F irradiance image and one BC5 octahedral
        // direction image per target lightmap. IW3's primary image is a sun
        // visibility mask, not that ambient fraction. The baked opaque world
        // material consumes this native atlas; generated materials still read
        // the separate source atlases above.
        if (lightmapDimensions.size() == 1)
        {
            const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
            std::vector<std::uint8_t> maskValues(pixelCount), directionX(pixelCount),
                directionY(pixelCount), irradiance(pixelCount * sizeof(std::uint32_t));
            for (std::size_t index = 0; index < pixelCount; ++index)
            {
                const auto *a = secondBytes.data() + index * 4;
                const auto *b = secondBytes.data() + half + index * 4;
                const auto sample = EncodeNativeLightmapPixel(
                    {a[2], a[1], a[0], a[3]}, {b[2], b[1], b[0], b[3]});
                maskValues[index] = 0;
                directionX[index] = sample.directionX;
                directionY[index] = sample.directionY;
                std::memcpy(irradiance.data() + index * sizeof(sample.irradiance),
                            &sample.irradiance, sizeof(sample.irradiance));
            }
            NativeLightmapPlan native;
            native.width = width;
            native.height = height;
            native.files = {map + "_native_lightmap_0.bc4",
                            map + "_native_lightmap_0.r11g11b10f",
                            map + "_native_lightmap_0.bc5"};
            WriteBytes(mapDirectory / native.files[0], EncodeBc4(width, height, maskValues));
            WriteBytes(mapDirectory / native.files[1], irradiance);
            WriteBytes(mapDirectory / native.files[2],
                       EncodeBc5(width, height, directionX, directionY));
            plan.nativeLightmaps.push_back(std::move(native));
        }
        Paste(atlases[0], color, rectangle.x, rectangle.y);
        Paste(atlases[1], normal, rectangle.x, rectangle.y);
        Paste(atlases[2], response, rectangle.x, rectangle.y);
    }

    if (lightmapDimensions.size() > 1)
    {
        constexpr unsigned nativeWidth = 4096;
        constexpr unsigned nativeHeight = 4096;
        constexpr std::size_t nativePixelCount =
            static_cast<std::size_t>(nativeWidth) * nativeHeight;
        NativeLightmapPlan native;
        native.width = nativeWidth;
        native.height = nativeHeight;
        native.files = {map + "_native_lightmap_0.bc4",
                        map + "_native_lightmap_0.r11g11b10f",
                        map + "_native_lightmap_0.bc5"};

        std::vector<std::uint8_t> maskValues(nativePixelCount),
            irradiance(nativePixelCount * sizeof(std::uint32_t)),
            directionX(nativePixelCount), directionY(nativePixelCount);
        for (std::size_t index = 0; index < nativePixelCount; ++index)
        {
            const auto *color = atlases[0].rgba.data() + index * 4;
            const auto *normal = atlases[1].rgba.data() + index * 4;
            const auto *response = atlases[2].rgba.data() + index * 4;
            const auto sample = EncodeNativeLightmapPixel(
                {color[0], color[1], color[2], normal[0]},
                {response[0], response[1], response[2], normal[1]});
            directionX[index] = sample.directionX;
            directionY[index] = sample.directionY;
            std::memcpy(irradiance.data() + index * sizeof(sample.irradiance),
                        &sample.irradiance, sizeof(sample.irradiance));
        }
        WriteBytes(mapDirectory / native.files[0],
                   EncodeBc4(nativeWidth, nativeHeight, maskValues));
        WriteBytes(mapDirectory / native.files[1], irradiance);
        WriteBytes(mapDirectory / native.files[2],
                   EncodeBc5(nativeWidth, nativeHeight, directionX, directionY));
        plan.nativeLightmaps.push_back(std::move(native));
    }

    timing.lap("shadow casters, sky and lightmaps");
    const unsigned mipCount = static_cast<unsigned>(std::log2(cell)) - 1;
    Json material = Json::parse(ResourceText(IDR_IW3_MATERIAL_TEMPLATE));
    const std::string stem = map + ".d3dbsp";
    plan.material = "w/mw120r_" + map;
    plan.instanceLightingImage = "mw120r/" + map + "_smodel_lighting";
    plan.instanceLightingFile = map + "_smodel_lighting.rgba";
    plan.instanceLightingCount = staticModelInstances;
    plan.materialDefinition = stem + ".material.json";
    std::set<std::string> writtenResidentImages;
    const auto residentImage = [&](Json &definition, const unsigned channel, Image source,
                                   const std::array<float, 4> &tint,
                                   const std::string_view domain,
                                   const bool alphaCoverage = false,
                                   const Image *alphaChannel = nullptr) {
        if (channel == 0)
        {
            for (auto &mip : source.mips)
                mip = Resize(mip, mip.width, mip.height, true, tint);
            auto authoredMips = std::move(source.mips);
            source = Resize(source, source.width, source.height, true, tint);
            source.mips = std::move(authoredMips);
            if (alphaCoverage)
                PreserveAlphaCoverage(source, true);
        }
        unsigned levels = 1, width = source.width, height = source.height;
        while (width > 1 || height > 1)
        {
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
            ++levels;
        }
        const unsigned sourceWidth = source.width, sourceHeight = source.height;
        auto pixels = MipChain(std::move(source), levels, channel == 0);
        if (alphaChannel)
        {
            // Color mips weight texels by alpha as coverage, so a data channel
            // stored in alpha gets its own chain and replaces alpha afterwards.
            Image data = Resize(*alphaChannel, sourceWidth, sourceHeight, false);
            const auto dataPixels = MipChain(std::move(data), levels, false);
            if (dataPixels.size() != pixels.size())
                throw std::runtime_error("alpha data chain does not match its color chain");
            for (std::size_t offset = 3; offset < pixels.size(); offset += 4)
                pixels[offset] = dataPixels[offset];
        }
        const auto digest = Sha256(pixels);
        const std::string id = Hex(std::span(digest).first(8));
        const std::string filename = map + "_" + std::string(domain) + "_" +
                                     std::to_string(channel) + "_" + id + ".rgba";
        const std::string name = "mw120r/" + map + "_" + std::string(domain) + "_" +
                                 std::to_string(channel) + "_" + id;
        if (writtenResidentImages.insert(filename).second)
            WriteBytes(mapDirectory / filename, pixels);
        definition["textures"][channel]["image"] = name;
        definition["imageDefinitions"].push_back({{"name", name},
                                                  {"width", sourceWidth},
                                                  {"height", sourceHeight},
                                                  {"rgba8", filename},
                                                  {"format", channel == 0 ? 7 : 6},
                                                  {"mipCount", levels}});
    };
    // Replay's decal volumes are drawn from four 4096 atlases in Replay formats
    // 45 (BC7 sRGB), 44 (BC7), 39 (BC4) and 34 (see sub_1418829D0). Channel
    // images are copied into them block for block (sub_14187E490); only 1x1
    // RGBA8 constant colours are re-encoded on the GPU. An RGBA8 channel image
    // therefore fills its atlas region with garbage, which rendered pink.
    const auto decalImage = [&](Json &definition, const unsigned channel, Image source,
                                const std::string_view domain, const unsigned format,
                                const Image *alphaChannel = nullptr) {
        const bool bc4 = format == 39;
        if (source.width == 1 && source.height == 1 && !bc4)
        {
            residentImage(definition, channel, std::move(source), {1, 1, 1, 1}, domain, false,
                          alphaChannel);
            return;
        }
        unsigned size = 8;
        while (size < std::max(source.width, source.height) && size < 1024)
            size *= 2;
        const bool color = format == 45;
        if (source.width != size || source.height != size)
        {
            source = Resize(source, size, size, color);
            source.mips.clear();
        }
        // Levels stop at the 4x4 block; the atlas copy skips smaller mips.
        unsigned levels = 1;
        for (unsigned width = size; width > 4; width /= 2)
            ++levels;
        std::vector<std::uint8_t> encoded;
        if (bc4)
        {
            // The mask channel is the IW3 colour map's alpha (decal opacity).
            Image mask = source;
            for (std::size_t offset = 0; offset < mask.rgba.size(); offset += 4)
                mask.rgba[offset] = mask.rgba[offset + 1] = mask.rgba[offset + 2] =
                    source.rgba[offset + 3];
            for (auto &mip : mask.mips)
                for (std::size_t offset = 0; offset < mip.rgba.size(); offset += 4)
                    mip.rgba[offset] = mip.rgba[offset + 1] = mip.rgba[offset + 2] =
                        mip.rgba[offset + 3];
            const auto chain = MipChain(std::move(mask), levels, false);
            std::size_t offset = 0;
            for (unsigned level = 0, width = size; level < levels; ++level, width /= 2)
            {
                std::vector<std::uint8_t> values(std::size_t{width} * width);
                for (std::size_t texel = 0; texel < values.size(); ++texel)
                    values[texel] = chain[offset + texel * 4 + 3];
                offset += (values.size() * 4 + 15) & ~std::size_t{15};
                const auto blocks = EncodeBc4(width, width, values);
                encoded.insert(encoded.end(), blocks.begin(), blocks.end());
                encoded.resize((encoded.size() + 15) & ~std::size_t{15});
            }
        }
        else
        {
            auto pixels = MipChain(std::move(source), levels, color);
            if (alphaChannel)
            {
                Image data = Resize(*alphaChannel, size, size, false);
                const auto dataPixels = MipChain(std::move(data), levels, false);
                if (dataPixels.size() != pixels.size())
                    throw std::runtime_error("decal alpha chain does not match its color chain");
                for (std::size_t offset = 3; offset < pixels.size(); offset += 4)
                    pixels[offset] = dataPixels[offset];
            }
            encoded = EncodeBc7Chain(pixels, size, levels, {});
        }
        const auto digest = Sha256(encoded);
        const std::string id = Hex(std::span(digest).first(8));
        const std::string filename = map + "_" + std::string(domain) + "_" +
                                     std::to_string(channel) + "_" + id +
                                     (bc4 ? ".bc4" : ".bc7");
        const std::string name = "mw120r/" + map + "_" + std::string(domain) + "_" +
                                 std::to_string(channel) + "_" + id;
        if (writtenResidentImages.insert(filename).second)
            WriteBytes(mapDirectory / filename, encoded);
        definition["textures"][channel]["image"] = name;
        definition["imageDefinitions"].push_back({{"name", name},
                                                  {"width", size},
                                                  {"height", size},
                                                  {bc4 ? "bc4" : "bc7", filename},
                                                  {"format", format},
                                                  {"mipCount", levels}});
    };
    material["techsetDefinition"] = stem + ".techset.json";
    material["imageDefinitions"] = Json::array();
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        // Keep authored mip coverage inside exact-size material tiles. The
        // surrounding atlas (including lightmaps and sky) still downsamples
        // normally; the source's lower levels must not be re-averaged away.
        for (unsigned level = 1; level < mipCount && !atlasTileMips[channel].empty(); ++level)
        {
            Image next = Downsample(level == 1 ? atlases[channel]
                                               : atlases[channel].mips.back(),
                                    channel == 0);
            const unsigned mipCell = cell >> level;
            for (const auto &tile : atlasTileMips[channel])
            {
                const auto &sourceMips = image(tile.image).front().mips;
                if (level > sourceMips.size() || sourceMips[level - 1].width != mipCell ||
                    sourceMips[level - 1].height != mipCell)
                    continue;
                const Image authored = Resize(sourceMips[level - 1], mipCell, mipCell,
                                              channel == 0,
                                              channel == 0 ? tile.tint
                                                           : std::array<float, 4>{1, 1, 1, 1});
                Paste(next, authored, static_cast<unsigned>(tile.tile % columns) * mipCell,
                      static_cast<unsigned>(tile.tile / columns) * mipCell);
            }
            atlases[channel].mips.push_back(std::move(next));
        }
        auto chain = MipChain(std::move(atlases[channel]), mipCount, channel == 0);
        if (atlasSize > 4096)
        {
            chain = EncodeBc7Chain(chain, atlasSize, mipCount, cacheRoot);
            const std::string filename = map + "_atlas_" + std::to_string(channel) + ".bc7";
            const std::string name = ImageName(map, channel, chain);
            WriteBytes(mapDirectory / filename, chain);
            material["textures"][channel]["image"] = name;
            // Replay format table 0x1423E25F0: 44 BC7_UNORM, 45 BC7_UNORM_SRGB.
            material["imageDefinitions"].push_back({{"name", name},
                                                    {"width", atlasSize},
                                                    {"height", atlasSize},
                                                    {"bc7", filename},
                                                    {"format", channel == 0 ? 45 : 44},
                                                    {"mipCount", mipCount}});
            continue;
        }
        const std::string filename = map + "_atlas_" + std::to_string(channel) + ".rgba";
        const std::string name = ImageName(map, channel, chain);
        WriteBytes(mapDirectory / filename, chain);
        material["textures"][channel]["image"] = name;
        material["imageDefinitions"].push_back({{"name", name},
                                                {"width", atlasSize},
                                                {"height", atlasSize},
                                                {"rgba8", filename},
                                                {"format", channel == 0 ? 7 : 6},
                                                {"mipCount", mipCount}});
    }
    zt::info("iw3: source atlas %ux%u, %u columns, %u-pixel material tiles, %u mips", atlasSize,
             atlasSize, columns, cell, mipCount);
    timing.lap("atlas mips");
    Json techset =
        BuildTechset(map, columns, plan.atlasSize, TechniqueGeometry::world, plan.glassEnvironment);
    timing.lap("world techset");
    material["techset"] = techset.at("name");
    WriteJson(mapDirectory / plan.materialDefinition, material);
    WriteJson(mapDirectory / (stem + ".techset.json"), techset);
    std::vector<std::string> worldVariants;
    std::set<std::string> cutoutKinds;
    for (const auto &[name, entry] : plan.materials)
        if (entry.kind == SurfaceKind::cutout)
            cutoutKinds.insert(CutoutVariantKind(entry));
    worldVariants.insert(worldVariants.end(), cutoutKinds.begin(), cutoutKinds.end());
    if (plan.hasGlass)
        worldVariants.push_back("glass");
    worldVariants.push_back("sky");
    if (plan.hasOpaqueBias)
        worldVariants.push_back("opaque_bias");
    for (const std::string &kind : worldVariants)
    {
        const std::string &name = kind;
        Json variant;
        if (name == "opaque_bias")
        {
            Json biased = techset;
            ApplyDepthBias(biased);
            Json biasedMaterial = material;
            biasedMaterial["techset"] = biased.at("name");
            biasedMaterial["techsetDefinition"] = stem + "." + name + ".techset.json";
            variant = {{"definition", stem + "." + name + ".material.json"},
                       {"techsetFile", stem + "." + name + ".techset.json"},
                       {"techset", std::move(biased)},
                       {"material", std::move(biasedMaterial)}};
        }
        else if (name.starts_with("foliage_"))
        {
            variant = CreateVariant(techset, material, stem, map, "foliage");
            if (name.find("_back") != std::string::npos)
            {
                Json &cutout = variant["techset"];
                cutout["name"] = cutout.at("name").get<std::string>() + "_back";
                for (auto &technique : cutout.at("techniques"))
                {
                    auto header = Unhex(technique.at("header").get<std::string>());
                    // The shadow back-face pass keeps its own culling.
                    if (At<std::uint32_t>(header, 8) == 28)
                        continue;
                    Put<std::uint64_t>(header, 0xA0, (At<std::uint64_t>(header, 0xA0) & ~3ull) | 1);
                    technique["header"] = Hex(header);
                    technique["name"] = technique.at("name").get<std::string>() + "_back";
                    PatchStateIdentity(technique, header);
                }
            }
            if (name.ends_with("_bias"))
                ApplyDepthBias(variant["techset"]);
            variant["definition"] = stem + "." + name + ".material.json";
            variant["techsetFile"] = stem + "." + name + ".techset.json";
            variant["material"]["techset"] = variant.at("techset").at("name");
            variant["material"]["techsetDefinition"] = variant.at("techsetFile");
        }
        else
            variant = CreateVariant(techset, material, stem, map, kind);
        WriteJson(mapDirectory / variant.at("definition").get<std::string>(),
                  variant.at("material"));
        WriteJson(mapDirectory / variant.at("techsetFile").get<std::string>(),
                  variant.at("techset"));
        plan.additionalMaterials.push_back({{"schema", 1},
                                            {"material", plan.material + "_" + kind},
                                            {"materialDefinition", variant.at("definition")}});
    }

    // Baked opaque BSP surfaces use Replay's shipped world shader family and
    // the native lightmap table. Keep the generated shader for surfaces whose
    // source semantics are not yet represented by that family.
    const std::set<std::string> neededWorldMaterials(worldMaterials.begin(),
                                                     worldMaterials.end());
    const Json nativeWorldTemplate = Json::parse(ResourceText(IDR_IW3_TECHSET_TEMPLATE));
    std::map<unsigned, std::pair<std::string, std::string>> nativeWorldTechsets;
    for (const SourceMaterial &source : materials)
    {
        if (!neededWorldMaterials.contains(source.name) || source.kind != SurfaceKind::opaque)
            continue;
        auto [entry, inserted] =
            nativeWorldTechsets.try_emplace(source.cullMode | (source.depthBias ? 4u : 0u));
        if (inserted)
        {
            Json nativeTechset = nativeWorldTemplate;
            const std::string suffix =
                "_cull" + std::to_string(source.cullMode) + (source.depthBias ? "_bias" : "");
            nativeTechset["name"] = "tw/mw120r_" + map + "_native_world" + suffix;
            nativeTechset["nativeReplayFixture"] = true;
            for (auto &technique : nativeTechset.at("techniques"))
            {
                auto header = Unhex(technique.at("header").get<std::string>());
                unsigned cull = source.cullMode;
                if (At<std::uint32_t>(header, 8) == 28 && cull)
                    cull = 3 - cull;
                Put<std::uint64_t>(header, 0xA0,
                                   (At<std::uint64_t>(header, 0xA0) & ~3ull) | cull |
                                       (source.depthBias ? 0x10ull : 0ull));
                technique["header"] = Hex(header);
                technique["name"] = technique.at("name").get<std::string>() + "_mw120r_" +
                                    map + suffix;
                PatchStateIdentity(technique, header);
            }
            const std::string file = stem + ".native_world" + suffix + ".techset.json";
            entry->second = {nativeTechset.at("name").get<std::string>(), file};
            WriteJson(mapDirectory / file, nativeTechset);
        }

        const auto sourceBytes = std::span(
            reinterpret_cast<const std::uint8_t *>(source.name.data()), source.name.size());
        const auto nameDigest = Sha256(sourceBytes);
        const std::string id = Hex(std::span(nameDigest).first(8));
        const std::string materialName = "w/mw120r_" + map + "_world_" + id;
        const std::string materialFile = stem + ".world." + id + ".material.json";
        Json direct = Json::parse(ResourceText(IDR_IW3_MATERIAL_TEMPLATE));
        direct["source"] = source.name;
        direct["techset"] = entry->second.first;
        direct["techsetDefinition"] = entry->second.second;
        direct["imageDefinitions"] = Json::array();
        auto pixelConstants = Unhex(direct.at("buffers").at(0).at(3).get<std::string>());
        if (pixelConstants.size() != 64 || pixelConstants[54] != 0x7e)
            throw std::runtime_error("native world normal-strength template changed");
        // The retained pixel shader scales the octahedral G/A normal by
        // (cb3[3].y[16:23] + 1) / 256. Use the full encoded direction.
        pixelConstants[54] = 0xff;
        direct["buffers"][0][3] = Hex(pixelConstants);
        // Shipped Replay world textures pack specular intensity in color alpha
        // ("<c>&<s>", 255 for "&defaultspec") and gloss in the normal map's red
        // channel ("<n>&<g>"). IW3 specular maps carry intensity in RGB and
        // gloss (cosine power) in alpha. Color alpha gets its own mip chain:
        // the color chain weights texels by alpha as coverage.
        Image worldColor = image(source.color).front();
        // Runtime probe: MW120R_DEBUG_MIP_COLORS=<IW3 material> (e.g. wc/zz_grass)
        // paints each mip of that native colour image one flat colour (red,
        // orange, yellow, green, cyan, blue, magenta, white, grey, black), so a
        // screenshot shows which level Replay samples and that the image binds.
        if (const char *probe = std::getenv("MW120R_DEBUG_MIP_COLORS");
            probe && source.name == probe)
        {
            static constexpr std::array<std::array<std::uint8_t, 3>, 10> colors{
                {{255, 0, 0}, {255, 128, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255},
                 {0, 0, 255}, {255, 0, 255}, {255, 255, 255}, {128, 128, 128}, {0, 0, 0}}};
            const auto paint = [&](Image &level, const unsigned index) {
                const auto &color = colors[std::min<std::size_t>(index, colors.size() - 1)];
                for (std::size_t offset = 0; offset < level.rgba.size(); offset += 4)
                    std::copy(color.begin(), color.end(), level.rgba.begin() + offset);
            };
            paint(worldColor, 0);
            worldColor.mips.clear();
            for (unsigned level = 1, width = worldColor.width, height = worldColor.height;
                 width > 1 || height > 1; ++level)
            {
                width = std::max(1u, width / 2);
                height = std::max(1u, height / 2);
                Image mip{width, height,
                          std::vector<std::uint8_t>(std::size_t{width} * height * 4, 255)};
                paint(mip, level);
                worldColor.mips.push_back(std::move(mip));
            }
            zt::warn("iw3: debug mip colours on native world material %s", source.name.c_str());
        }
        for (std::size_t offset = 3; offset < worldColor.rgba.size(); offset += 4)
            worldColor.rgba[offset] = 255;
        for (auto &mip : worldColor.mips)
            for (std::size_t offset = 3; offset < mip.rgba.size(); offset += 4)
                mip.rgba[offset] = 255;
        // Raw IW3 specular bytes left typical surfaces at a quarter of the
        // Replay default and removed their gloss; see ReplaySpecularIntensity.
        const Image specular = source.response.empty()
                                   ? Image{1, 1, std::vector<std::uint8_t>{255, 255, 255, 255}}
                                   : ReplaySpecularIntensity(image(source.response).front());
        residentImage(direct, 0, std::move(worldColor), source.tint, "world", false, &specular);
        const bool hasNormal = !source.normal.empty() && source.normal != "$identitynormalmap";
        Image worldNormal = ReplayNormalGloss(
            hasNormal ? &image(source.normal).front() : nullptr,
            source.response.empty() ? nullptr : &image(source.response).front());
        residentImage(direct, 1, std::move(worldNormal), {1, 1, 1, 1}, "world");
        residentImage(direct, 2,
                      source.response.empty() ? Image{1, 1, std::vector<std::uint8_t>{0, 0, 0, 0}}
                                              : image(source.response).front(),
                      {1, 1, 1, 1}, "world");
        WriteJson(mapDirectory / materialFile, direct);
        const unsigned index = static_cast<unsigned>(plan.additionalMaterials.size() + 1);
        plan.additionalMaterials.push_back(
            {{"schema", 1}, {"material", materialName}, {"materialDefinition", materialFile}});
        plan.materials.at(source.name).worldMaterialIndex = index;
    }

    // FxGlassSystem supplies its own vertices. A BSP glass variant cannot be
    // used here: Replay R_DrawGlassSurf (18F9F00) rejects layouts other than 31.
    for (const std::string &sourceName : surfaceMaterials)
    {
        auto &entry = plan.materials.at(sourceName);
        if (entry.kind != SurfaceKind::glass || !entry.glassMaterial.empty())
            continue;
        const auto sourceBytes = std::span(
            reinterpret_cast<const std::uint8_t *>(sourceName.data()), sourceName.size());
        const auto sourceDigest = Sha256(sourceBytes);
        const std::string kind = "pane_" + std::to_string(entry.tile) + "_" +
                                 std::to_string(entry.flags) + "_" +
                                 Hex(std::span(sourceDigest).first(8)) + "_glass";
        entry.glassMaterial = plan.material + "_" + kind;
        if (std::any_of(plan.assetMaterials.begin(), plan.assetMaterials.end(),
                        [&](const Json &existing) {
                            return existing.at("material") == entry.glassMaterial;
                        }))
            continue;
        Json variant = CreateVariant(techset, material, stem, map, kind);
        Json &paneTechset = variant.at("techset");
        const auto &environment = entry.environment;
        const std::string vertexSource = "#define GLASS_PANE 1\n#define GLASS_TILE " +
            std::to_string(entry.tile) + "\n#define GLASS_FLAGS " +
            std::to_string(entry.flags | 2u) + "\n#define GLASS_ENVIRONMENT float4(" +
            HlslNumber(environment[0]) + "," + HlslNumber(environment[1]) + "," +
            HlslNumber(environment[2]) + "," + HlslNumber(environment[3]) + ")\n" +
            ResourceText(IDR_IW3_VERTEX_SHADER);
        Json vertex = Shader(14, "iw3_glass", "iw3_glass_vertex.hlsl",
                             Compile(vertexSource, "vs_5_0", "iw3_glass_vertex.hlsl"));
        Json &lit = paneTechset.at("techniques").back();
        auto header = Unhex(lit.at("header").get<std::string>());
        header[0x9C] = 31;
        // Shipped Replay glass techsets (techsets_mp_backlot2 g/...) map every
        // pipeline-state slot except 19-21 to their single state. The glass
        // draw selects slots other than 1, and an unmapped 0xFF slot makes
        // the pane draw nothing.
        for (std::size_t slot = 0; slot < 25; ++slot)
            header[0x0F + slot] = slot >= 19 && slot <= 21 ? 0xFF : 0;
        lit["header"] = Hex(header);
        lit["shaders"][0] = vertex.at("name");
        paneTechset["shaders"]["14:" + vertex.at("name").get<std::string>()] = vertex;
        auto args = Unhex(lit.at("args").get<std::string>());
        bool glassBuffer = false;
        for (std::size_t offset = 0; offset < args.size(); offset += 6)
            if (args[offset] == 3 && args[offset + 1] == 2 &&
                At<std::uint16_t>(args, offset + 2) == 13 &&
                At<std::uint16_t>(args, offset + 4) == 45)
            {
                // Signed Replay glass: VS t2 = code buffer 47, stride 28.
                Put<std::uint16_t>(args, offset + 2, 2);
                Put<std::uint16_t>(args, offset + 4, 47);
                glassBuffer = true;
            }
        if (!glassBuffer)
            throw std::runtime_error("Glass technique has no native vertex-buffer binding");
        lit["args"] = Hex(args);
        PatchStateIdentity(lit, Unhex(vertex.at("program").get<std::string>()));
        auto type = Unhex(paneTechset.at("header").get<std::string>());
        Put<std::uint32_t>(type, 0x10, 0x100000u);
        paneTechset["header"] = Hex(type);
        KeepReferencedShaders(paneTechset);
        auto info = Unhex(variant.at("material").at("info").get<std::string>());
        if (sourceName.find("shattered") != std::string::npos)
        {
            // Replay's native shattered pane uses glass contents/surface flags,
            // not the opaque baseboard flags inherited by the material template.
            Put<std::uint32_t>(info, 0x00, 0x2010u);
            Put<std::uint32_t>(info, 0x04, 0x4C4000u);
        }
        else
        {
            // Breakable intact panes use glass contents and surftype_glass_pane
            // (9 << 19). Backlot's opaque tinted pane is glass_solid (39), which
            // Replay treats as bulletproof.
            Put<std::uint32_t>(info, 0x00, 0x10u);
            Put<std::uint32_t>(info, 0x04, 0x480000u);
        }
        Put<std::uint32_t>(info, 0xC, 0x100000u);
        info[0x10] = 3;
        info[0x11] = 26;
        variant["material"]["info"] = Hex(info);
        WriteJson(mapDirectory / variant.at("definition").get<std::string>(),
                  variant.at("material"));
        WriteJson(mapDirectory / variant.at("techsetFile").get<std::string>(), paneTechset);
        plan.assetMaterials.push_back({{"schema", 1}, {"material", entry.glassMaterial},
                                       {"materialDefinition", variant.at("definition")}});
    }

    // Static placements draw through Replay's static-model tables; script,
    // dynamic and effect XModels draw as DObjs, whose shaders take the object
    // from per-draw constants. Shipped Replay keeps a material per usage
    // (tmo/... versus mo/...), so each usage gets its own geometry type.
    const auto emitModelMaterials = [&](const TechniqueGeometry geometry,
                                        const std::vector<std::string> &needed,
                                        std::string MaterialPlan::*target) {
        const bool entity = geometry == TechniqueGeometry::entity;
        const std::string usage = entity ? "entity" : "model";
        Json modelMaterialTemplate = material;
        auto modelInfo = Unhex(modelMaterialTemplate.at("info").get<std::string>());
        Put<std::uint32_t>(modelInfo, 0xC, entity ? 0x80014u : 589844u);
        // Static placements read their IW3 light-grid lighting (one ambient
        // cube per instance) from a shared fourth texture.
        const bool instanceLighting = !entity && staticModelInstances > 0;
        // Entities (DObjs) sample the world-space light-grid volume instead.
        const bool volumeLighting = entity && entityLightVolume;
        if (instanceLighting || volumeLighting)
        {
            modelInfo[0x14] = 4; // texture count
            modelMaterialTemplate["textures"].push_back(
                {{"header", "3d00000000000000"},
                 {"image", instanceLighting ? plan.instanceLightingImage : entityLightVolume->image}});
        }
        modelMaterialTemplate["info"] = Hex(modelInfo);
        Json modelTechset =
            BuildTechset(map, columns, plan.atlasSize, geometry, plan.glassEnvironment,
                         instanceLighting, volumeLighting ? entityLightVolume->defines : std::string{});
        std::array<Json, 5> modelTechsets{};
        modelTechsets[static_cast<unsigned>(SurfaceKind::opaque)] = modelTechset;

        for (const auto &[kind, enabled, surfaceKind] :
             std::array<std::tuple<std::string, bool, SurfaceKind>, 2>{
                 std::tuple{usage + "_foliage", plan.hasCutout, SurfaceKind::cutout},
                 std::tuple{usage + "_glass", plan.hasGlass, SurfaceKind::glass}})
        {
            if (!enabled)
                continue;
            Json variant = CreateVariant(modelTechset, modelMaterialTemplate, stem, map, kind);
            modelTechsets[static_cast<unsigned>(surfaceKind)] = std::move(variant.at("techset"));
        }

        const std::set<std::string> neededMaterials(needed.begin(), needed.end());
        std::map<std::pair<unsigned, unsigned>, std::pair<std::string, std::string>> techsetFiles;
        for (const SourceMaterial &source : materials)
        {
            if (!neededMaterials.contains(source.name) || source.kind == SurfaceKind::skipped)
                continue;
            const unsigned kind = static_cast<unsigned>(source.kind);
            if (kind >= modelTechsets.size() || modelTechsets[kind].empty())
                throw std::runtime_error("IW3 render plan omitted a model technique variant");
            auto [techsetEntry, inserted] = techsetFiles.try_emplace(
                {kind, source.cullMode | (source.depthBias ? 4u : 0u)});
            if (inserted)
            {
                Json variant = modelTechsets[kind];
                const std::string suffix =
                    "_cull" + std::to_string(source.cullMode) + (source.depthBias ? "_bias" : "");
                variant["name"] = variant.at("name").get<std::string>() + suffix;
                for (auto &technique : variant.at("techniques"))
                {
                    auto header = Unhex(technique.at("header").get<std::string>());
                    unsigned cull = source.cullMode;
                    if (At<std::uint32_t>(header, 8) == 28 && cull)
                        cull = 3 - cull; // Native back-face shadow pass reverses authored culling.
                    Put<std::uint64_t>(header, 0xA0,
                                       (At<std::uint64_t>(header, 0xA0) & ~3ull) | cull |
                                           (source.depthBias ? 0x10ull : 0ull));
                    technique["header"] = Hex(header);
                    technique["name"] = technique.at("name").get<std::string>() + suffix;
                    PatchStateIdentity(technique, header);
                }
                const std::string file =
                    stem + "." + usage + "_" + std::to_string(kind) + suffix + ".techset.json";
                techsetEntry->second = {variant.at("name").get<std::string>(), file};
                WriteJson(mapDirectory / file, variant);
            }
            const auto sourceBytes = std::span(
                reinterpret_cast<const std::uint8_t *>(source.name.data()), source.name.size());
            const auto nameDigest = Sha256(sourceBytes);
            const std::string id = Hex(std::span(nameDigest).first(8));
            const std::string materialName = "w/mw120r_" + map + "_" + usage + "_" + id;
            const std::string materialFile = stem + "." + usage + "." + id + ".material.json";
            Json direct = modelMaterialTemplate;
            direct["source"] = source.name;
            if (source.kind == SurfaceKind::glass)
            {
                // Shipped Replay model glass is submitted after opaque surfaces.
                auto info = Unhex(direct.at("info").get<std::string>());
                info[0x10] = 3;
                info[0x11] = 26;
                direct["info"] = Hex(info);
            }
            direct["techset"] = techsetEntry->second.first;
            direct["techsetDefinition"] = techsetEntry->second.second;
            direct["imageDefinitions"] = Json::array();
            // IW3 "ge128" cutouts (flags 8/16 select gt0/lt128) clip at alpha 0.5.
            const bool alphaCoverage =
                source.kind == SurfaceKind::cutout && !(source.flags & 24u);
            residentImage(direct, 0, image(source.color).front(), source.tint, "model",
                          alphaCoverage);
            residentImage(direct, 1,
                          source.normal.empty()
                              ? Image{1, 1, std::vector<std::uint8_t>{128, 128, 128, 128}}
                              : image(source.normal).front(),
                          {1, 1, 1, 1}, "model");
            residentImage(direct, 2,
                          source.response.empty()
                              ? Image{1, 1, std::vector<std::uint8_t>{0, 0, 0, 0}}
                              : image(source.response).front(),
                          {1, 1, 1, 1}, "model");
            if (instanceLighting)
                direct["imageDefinitions"].push_back(
                    {{"name", plan.instanceLightingImage},
                     {"width", kInstanceLightingColumns * 6},
                     {"height", (staticModelInstances + kInstanceLightingColumns - 1) /
                                    kInstanceLightingColumns},
                     {"rgba8", plan.instanceLightingFile},
                     {"format", 6},
                     {"mipCount", 1}});
            if (volumeLighting)
                direct["imageDefinitions"].push_back({{"name", entityLightVolume->image},
                                                      {"width", entityLightVolume->width},
                                                      {"height", entityLightVolume->height},
                                                      {"rgba8", entityLightVolume->file},
                                                      {"format", 6},
                                                      {"mipCount", 1}});
            WriteJson(mapDirectory / materialFile, direct);
            plan.assetMaterials.push_back(
                {{"schema", 1}, {"material", materialName}, {"materialDefinition", materialFile}});
            plan.materials.at(source.name).*target = materialName;
        }
    };
    timing.lap("world variants, native world and glass materials");
    emitModelMaterials(TechniqueGeometry::staticModel, modelMaterials, &MaterialPlan::modelMaterial);
    timing.lap("static model materials");
    emitModelMaterials(TechniqueGeometry::entity, entityMaterials, &MaterialPlan::entityMaterial);
    timing.lap("entity materials");

    for (const std::string &name : fxMaterials)
    {
        const SourceFxMaterial source = ReadFxMaterial(exportRoot, sourcePaths, name);
        if (source.family == FxMaterialFamily::unsupported)
        {
            zt::info("iw3: FX material %s (%s) has no Replay material family", name.c_str(),
                     source.techniqueSet.c_str());
            continue;
        }
        if (source.family == FxMaterialFamily::distortion)
        {
            // IW3 heat haze and shock rings become Replay's own refraction
            // materials (common_mp vfx_dist_heat_1 / _shimmer_blast_1, used by
            // the stock grenade and concussion effects).
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool blast = lower.find("shock") != std::string::npos ||
                               lower.find("blast") != std::string::npos ||
                               lower.find("wave") != std::string::npos;
            const std::string stock =
                blast ? "elcq/vfx_dist_heat_shimmer_blast_1" : "elcq/vfx_dist_heat_1";
            plan.fxDistortionMaterials.insert(name);
            plan.stockMaterials.insert(stock);
            if (!plan.fxMaterialAliases.emplace(name, stock).second)
                throw std::runtime_error("duplicate IW3 FX material alias: " + name);
            continue;
        }
        const std::string materialName = FxMaterialName(map, source);
        const std::string id = materialName.substr(materialName.rfind('_') + 1);
        const std::string materialFile = stem + ".fx." + id + ".material.json";
        Json direct = BuildFxMaterialDefinition(source);
        if (source.family == FxMaterialFamily::decal)
        {
            // Replay decal volume material in the stock 0x44C layout: channel 0
            // "<c>&<s>" (colour, Replay specular intensity in alpha), channel 1
            // "<n>&<g>" (octahedral normal, gloss), channel 2 the colour map
            // whose alpha is the decal's opacity.
            const Image &color = image(source.image).front();
            const Image *response =
                source.response.empty() ? nullptr : &image(source.response).front();
            const Image specular =
                response ? ReplaySpecularIntensity(*response)
                         : Image{1, 1, std::vector<std::uint8_t>{255, 255, 255, 255}};
            Image opaque = color;
            for (std::size_t offset = 3; offset < opaque.rgba.size(); offset += 4)
                opaque.rgba[offset] = 255;
            for (auto &mip : opaque.mips)
                for (std::size_t offset = 3; offset < mip.rgba.size(); offset += 4)
                    mip.rgba[offset] = 255;
            Json packed = {{"textures", Json::array({Json::object(), Json::object()})},
                           {"imageDefinitions", Json::array()}};
            decalImage(packed, 0, std::move(opaque), "fxdecal", 45, &specular);
            decalImage(packed, 1,
                       ReplayNormalGloss(
                           source.normal.empty() ? nullptr : &image(source.normal).front(),
                           response),
                       "fxdecal", 44);
            Json mask = {{"textures", Json::array({Json::object()})},
                         {"imageDefinitions", Json::array()}};
            decalImage(mask, 0, color, "fxdecalmask", 39);
            const std::string volumeName = "i/mw120r_fx_decal_" + id;
            direct["imageDefinitions"] = packed.at("imageDefinitions");
            for (const auto &definition : mask.at("imageDefinitions"))
                direct["imageDefinitions"].push_back(definition);
            direct["decalVolumeMaterial"] = volumeName;
            direct["decalVolumeDefinition"] = {
                {"name", volumeName},
                {"channels", Json::array({packed["textures"][0]["image"],
                                          packed["textures"][1]["image"],
                                          mask["textures"][0]["image"], nullptr, nullptr,
                                          nullptr})},
                {"flags", 1100},
                {"colorTint", Json::array({source.tint[0], source.tint[1], source.tint[2]})},
                {"rows", 1},
                {"columns", 1}};
        }
        else
        {
            const auto &sourceImage = image(source.image);
            if (sourceImage.size() != 1)
                throw std::runtime_error("IW3 FX material color map is a cubemap: " + name);
            if (source.premultiplied)
            {
                Image premultiplied = sourceImage.front();
                const auto multiply = [](std::vector<std::uint8_t> &rgba) {
                    for (std::size_t offset = 0; offset + 3 < rgba.size(); offset += 4)
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            rgba[offset + channel] = static_cast<std::uint8_t>(
                                (rgba[offset + channel] * rgba[offset + 3] + 127) / 255);
                };
                multiply(premultiplied.rgba);
                for (auto &mip : premultiplied.mips)
                    multiply(mip.rgba);
                residentImage(direct, 0, premultiplied, {1, 1, 1, 1}, "fx");
            }
            else
                residentImage(direct, 0, sourceImage.front(), {1, 1, 1, 1}, "fx");
        }
        WriteJson(mapDirectory / materialFile, direct);
        plan.assetMaterials.push_back(
            {{"schema", 1}, {"material", materialName}, {"materialDefinition", materialFile}});
        if (!plan.fxMaterialAliases.emplace(name, materialName).second)
            throw std::runtime_error("duplicate IW3 FX material alias: " + name);
    }
    return plan;
}
std::vector<std::uint8_t> BuildLoadscreenPreview(const std::filesystem::path &exportRoot,
                                                 const std::vector<std::filesystem::path> &sourcePaths,
                                                 const std::string &sourceMap)
{
    const std::string name = "loadscreen_" + sourceMap;
    std::vector<Image> images;
    try
    {
        images = LoadImage(exportRoot, sourcePaths, name);
    }
    catch (const std::exception &)
    {
        return {};
    }
    if (images.size() != 1 || !images.front().width || !images.front().height)
        return {};
    Image image = images.front();
    const unsigned largest = (std::max)(image.width, image.height);
    if (largest > 2048)
        image = Resize(image, (std::max)(1u, image.width * 2048 / largest),
                       (std::max)(1u, image.height * 2048 / largest), true);
    std::vector<std::uint8_t> result(16);
    const std::array<std::uint32_t, 4> header{0x4952574Du, 1u, image.width, image.height};
    std::memcpy(result.data(), header.data(), sizeof(header));
    result.insert(result.end(), image.rgba.begin(), image.rgba.end());
    zt::info("iw3: loading-screen preview %s %ux%u", name.c_str(), image.width, image.height);
    return result;
}
} // namespace iw3
