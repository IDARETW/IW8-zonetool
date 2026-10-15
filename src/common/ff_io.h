#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zt
{
struct Iw8WriteParams
{
    uint64_t blockSize[11]{};
    uint64_t totalDecompressed{};
    uint64_t calcSize{};
    uint8_t transientFileType{};
    uint32_t xfileVersion{0xFF7};
    // XFile +0x08: capacity of the postload stream plan Replay records while it
    // preloads the zone (0xF65F70 appends [block][int32 delta]... 0 per recorded
    // pointer, unbounded). Stock headers carry the exact size (mp_shipment 34597,
    // mp_donetsk 804228); 0 overflowed a 16-byte buffer and crashed bigger maps in
    // postload. Validation measures the real plan through Replay's own preload.
    uint64_t preloadWalkSize{256 * 1024};
};

bool iw8_write(const std::string &outputPath, const std::vector<uint8_t> &zoneBody,
               const Iw8WriteParams &params);
bool inspect_ff(const std::string &fastfilePath);
} // namespace zt
