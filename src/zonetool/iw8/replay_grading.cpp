#include "replay_grading.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace iw8::grading
{
namespace
{
template <class T> void Store(uint8_t *record, const size_t offset, const T value)
{
    std::memcpy(record + offset, &value, sizeof(value));
}

// GfxPixelFormat 12 (format table 0x1423E25F0 -> DXGI_FORMAT_R16G16B16A16_UNORM), the
// format of stock mp_shipment's "gradingclutmp_shipment_clut".
constexpr uint32_t kHdrClutFormat = 12;
// Stock HDR CLUT image flags; 0x10000 marks the volume texture.
constexpr uint32_t kHdrClutFlags = 0x00010003;
} // namespace

void RegisterGradingClut(ZoneWriter &writer, const std::string &name, const unsigned clutSize,
                         const std::vector<uint8_t> &pixels)
{
    if (name.empty() || clutSize < 2 || clutSize > 64 ||
        pixels.size() != size_t(clutSize) * clutSize * clutSize * 8)
        throw std::invalid_argument("Replay grading CLUT definition is invalid");
    const std::string imageName = "gradingclut" + name;

    // Exact Replay Load_GfxImage layout used by replay_render's image writer, with
    // every header field matching the stock mp_shipment HDR CLUT image.
    writer.add(ASSET_TYPE_IMAGE, imageName, [imageName, clutSize, pixels](ZoneWriter &out) {
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(15);
        std::array<uint8_t, iw8sz::IMAGE> image{};
        Store(image.data(), 0x00, PTR_FOLLOWS);
        Store(image.data(), 0x14, kHdrClutFormat);
        Store(image.data(), 0x18, kHdrClutFlags);
        Store(image.data(), 0x1C, static_cast<uint32_t>(pixels.size()));
        Store(image.data(), 0x24, static_cast<uint16_t>(clutSize));
        Store(image.data(), 0x26, static_cast<uint16_t>(clutSize));
        Store(image.data(), 0x28, static_cast<uint16_t>(clutSize));
        Store(image.data(), 0x2A, uint16_t{1});
        image[0x2E] = 1; // semantic
        image[0x2F] = 1; // category
        image[0x30] = 1; // levelCount
        image[0x31] = 8; // streamedPartCount, as stock
        Store(image.data(), 0xE0, PTR_FOLLOWS);
        out.write(image.data(), image.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(imageName.c_str());
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

    // GfxGradingClut (Preload_GfxGradingClut 0x140DCD850): 24 bytes {name,
    // inputColorimetry, outputColorimetry, clutSize, hdrClutImage}; the name and
    // image pointer load from the virtual stream, and postload advances 24 bytes.
    writer.add(ASSET_TYPE_GRADINGCLUT, name, [name, imageName, clutSize](ZoneWriter &out) {
        // The image body precedes this one, so its alias slot exists now.
        const uint64_t image = out.assetAlias(ASSET_TYPE_IMAGE, imageName);
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(7);
        std::array<uint8_t, 0x18> clut{};
        Store(clut.data(), 0x00, PTR_FOLLOWS);
        clut[0x08] = 1; // inputColorimetry: BT.2020 PQ, as stock
        clut[0x09] = 1; // outputColorimetry
        Store(clut.data(), 0x0C, static_cast<uint32_t>(clutSize));
        Store(clut.data(), 0x10, image);
        out.write(clut.data(), clut.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(name.c_str());
        out.popStream();
        out.popStream();
        out.pushStream(XFILE_BLOCK_TEMP_POSTLOAD);
        out.align(7);
        out.reserveCalc(clut.size());
        out.popStream();
    });
}
} // namespace iw8::grading
