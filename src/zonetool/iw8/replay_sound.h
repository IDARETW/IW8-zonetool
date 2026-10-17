#pragma once

#include "iw8_zone.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iw8::sound
{
// Replay SND hash (0x14125B000): 65599 * h + lowercase byte from 5381; zero maps
// to one. Alias, secondary alias, curve and SAB entry keys all use it.
uint32_t HashName(std::string_view name);

// A 16-bit PCM sound stored in the bank's loaded SAB as codec 8 (FLAC frames).
struct Sample
{
    std::string name; // SAB asset name; HashName(name) is the entry key / alias assetId
    uint32_t rate{};
    uint8_t channels{};
    bool looping{};
    std::vector<int16_t> pcm; // interleaved frames
};

// One 232-byte SndAlias. Fields left at their defaults keep the stock values
// shared by every Replay map-bank alias ("default" curves, no context, no duck).
struct Alias
{
    std::string assetName;
    std::string secondaryName; // empty for none
    uint32_t flags{};          // bit0 loop, bits 7..9 load type, bits 10..17 channel
    int32_t volModIndex{};
    float volMin{1}, volMax{1};
    float pitchMin{1}, pitchMax{1};
    float distMin{}, distMax{};
    float probability{1};
    float lfePercentage{};
    float centerPercentage{};                       // +124
    float envelopMin{}, envelopMax{}, envelopPercentage{}; // +152..+160 (IW3 envelope)
    float reverbMultiplier{1};
    float farReverbMultiplier{1};
    int32_t startDelay{};
    int32_t startDelayMax{};
    uint32_t volumeFalloffCurve{};
    float stereoSpreadMinDist{}, stereoSpreadMaxDist{}, stereoSpreadMidPoint{};
    uint8_t stereoSpreadMaxAngle{};
    int16_t maxDurationDsec{};
};

struct AliasList
{
    std::string name;
    std::vector<Alias> aliases;
};

// SndCurve: name, id and up to 16 (x, y) points.
struct Curve
{
    std::string name;
    std::vector<std::array<float, 2>> points;
};

// Replay SAB v10 container (magic 2UX#, build 16, 44-byte entries) with every
// sample as fixed 1024-sample FLAC frames behind its 18-byte seek table.
std::vector<uint8_t> BuildSab(const std::vector<Sample> &samples, const std::string &zone);

struct Bank
{
    std::string name;      // SndBankResident asset name
    std::string zone;      // the loader opens zone\<zone>.sabs (and patch_<zone>.sabs)
    std::string streamKey; // StreamKey asset holding the loaded SAB
    std::vector<AliasList> lists;
    std::vector<Curve> volumeCurves;
    std::vector<uint8_t> sab;
};

// Registers the StreamKey (type 102, resident data) and the always-loaded
// SndBankResident (type 21) that references it.
void RegisterBank(ZoneWriter &writer, const Bank &bank);
} // namespace iw8::sound
