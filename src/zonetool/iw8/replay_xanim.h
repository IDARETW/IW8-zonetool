#pragma once

#include "iw8_zone.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace iw8::xanim
{
// An XAnimParts in the stream encoding IW3 and Replay 1.20 share: part types
// (boneCount[10]: no/half/full quat, small/full/no trans, all), key counts in
// dataShort, frame indices in dataByte/dataShort/indices, keys in the random
// data streams. Verified on shipped desktop_fan_fn_01_loop: Replay decodes
// these streams exactly as IW3 does, so conversion is a repack, not a re-encode.
struct Anim
{
    std::string name;
    uint16_t numframes = 0;
    float framerate = 30;
    float frequency = 0;
    bool loop = false;
    std::array<uint8_t, 10> boneCount{};
    std::vector<std::string> names;
    std::vector<std::pair<std::string, float>> notify;
    std::vector<uint8_t> dataByte;
    std::vector<int16_t> dataShort;
    std::vector<int32_t> dataInt;
    std::vector<uint8_t> randomDataByte;
    std::vector<int16_t> randomDataShort;
    std::vector<int32_t> randomDataInt;
    std::vector<uint16_t> indices; // bytes on disk when numframes < 256
};

// Reads an Unlinker replay_export::XAnim dump (xanim/<name>.iw3.json). Delta
// (root-motion) parts are rejected until their Replay layout is mapped.
Anim LoadIw3(const std::filesystem::path &path);
// Registers a top-level XAnimParts (asset type 7). Bone and notify names are
// interned as zone script strings.
void Register(ZoneWriter &writer, const Anim &anim);
} // namespace iw8::xanim
