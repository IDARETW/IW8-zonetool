#include "replay_rawfile.h"

#include "replay_script.h"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace iw8::rawfile
{
void Register(ZoneWriter &writer, const std::string &name, const std::vector<uint8_t> &data)
{
    if (name.empty() || name.find('\0') != std::string::npos)
        throw std::invalid_argument("Replay RawFile name is invalid");
    if (data.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("Replay RawFile payload is too large");

    const auto compressed = replay_script::zlibStored(data);
    if (compressed.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("Replay RawFile compressed payload is too large");
    const uint32_t compressedLength = static_cast<uint32_t>(compressed.size());
    const uint32_t rawLength = static_cast<uint32_t>(data.size());

    writer.add(ASSET_TYPE_RAWFILE, name,
               [name, compressed, compressedLength, rawLength](ZoneWriter &output) {
                   output.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
                   output.align(7);
                   std::array<uint8_t, 0x18> header{};
                   std::memcpy(header.data(), &PTR_FOLLOWS, sizeof(PTR_FOLLOWS));
                   std::memcpy(header.data() + 8, &compressedLength, sizeof(compressedLength));
                   std::memcpy(header.data() + 12, &rawLength, sizeof(rawLength));
                   std::memcpy(header.data() + 16, &PTR_FOLLOWS, sizeof(PTR_FOLLOWS));
                   output.write(header.data(), header.size());
                   output.popStream();

                   output.pushStream(XFILE_BLOCK_VIRTUAL);
                   output.writeStr(name);
                   output.write(compressed.data(), compressed.size());
                   output.popStream();
               });
}
} // namespace iw8::rawfile

namespace iw8::localize
{
void Register(ZoneWriter &writer, const std::string &name, const std::string &value)
{
    if (name.empty() || name.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
        throw std::invalid_argument("Replay LocalizeEntry name or value is invalid");

    writer.add(ASSET_TYPE_LOCALIZE, name, [name, value](ZoneWriter &output) {
        output.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        output.align(7);
        std::array<uint8_t, 0x10> header{};
        std::memcpy(header.data(), &PTR_FOLLOWS, sizeof(PTR_FOLLOWS));
        std::memcpy(header.data() + 8, &PTR_FOLLOWS, sizeof(PTR_FOLLOWS));
        output.write(header.data(), header.size());
        output.popStream();

        output.pushStream(XFILE_BLOCK_VIRTUAL);
        output.writeStr(name);
        output.writeStr(value);
        output.popStream();
        // The postload pass copies the 0x10 entry into stream 2, and every pop of
        // stream 2 realigns it to 32 bytes, so each entry takes 0x20. Without this
        // reservation it overran the next stream (round 100: crash in memcpy at
        // match start; ACTS_REPLAY_PRELOAD "block 2 ... requested 16, reserved 0").
        output.pushStream(XFILE_BLOCK_TEMP_POSTLOAD);
        output.align(31);
        output.reserveCalc(0x20);
        output.popStream();
    });
}
} // namespace iw8::localize
