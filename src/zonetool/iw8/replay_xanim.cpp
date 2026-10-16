#include "replay_xanim.h"

#include "../../common/json.hpp"
#include "iw8_structs.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace iw8::xanim
{
namespace
{
template <class T> void Store(uint8_t *record, const size_t offset, const T value)
{
    std::memcpy(record + offset, &value, sizeof(value));
}

// Replay XAnimParts (0xA0; Load_XAnimParts 0xDE0B10, ACTS schema).
constexpr size_t kName = 0x00, kNames = 0x08, kDataByte = 0x10, kDataShort = 0x18,
                 kDataInt = 0x20, kRandomDataShort = 0x28, kRandomDataByte = 0x30,
                 kRandomDataInt = 0x38, kIndices = 0x40, kNotify = 0x48, kDeltaPart = 0x50,
                 kRandomDataShortCount = 0x58, kRandomDataByteCount = 0x5C, kIndexCount = 0x60,
                 kFramerate = 0x64, kFrequency = 0x68, kDataByteCount = 0x6C,
                 kDataShortCount = 0x70, kDataIntCount = 0x72, kRandomDataIntCount = 0x74,
                 kNumFrames = 0x76, kFlags = 0x78, kBoneCount = 0x79, kNotifyCount = 0x83,
                 kAssetType = 0x84;
constexpr unsigned kPartTypeAll = 9;
// flags bit 0 = loop (shipped looping fans 3, their non-looping stop anims 2).
// Non-looping prop anims played on script models ship flags 2 and assetType 7
// (equipment_box_fan_anim_stop); assetType is read only by a script query
// (0xE8FF68 -> XAnimGetAssetType), never by playback. Bit 1 is inferred.
constexpr uint8_t kFlagLoop = 1, kFlagShipped = 2, kAssetTypeProp = 7;

template <class T> std::vector<T> Array(const nlohmann::json &source, const char *field)
{
    std::vector<T> out;
    for (const auto &value : source.at(field))
    {
        const auto number = value.get<int64_t>();
        if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max())
            throw std::runtime_error(std::string("IW3 XAnim value out of range: ") + field);
        out.push_back(static_cast<T>(number));
    }
    return out;
}

template <class T> void Write(ZoneWriter &writer, const std::vector<T> &data, const unsigned alignment)
{
    if (data.empty())
        return;
    writer.align(alignment);
    writer.write(data.data(), data.size() * sizeof(T));
}
} // namespace

Anim LoadIw3(const std::filesystem::path &path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot read IW3 XAnim dump " + path.string());
    const auto source = nlohmann::json::parse(input);
    if (source.value("schema", 0) != 1)
        throw std::runtime_error("unsupported IW3 XAnim dump schema: " + path.string());
    if (!source.at("delta_part").is_null())
    {
        const auto &delta = source.at("delta_part");
        if (delta.contains("trans") || delta.contains("quat"))
            throw std::runtime_error("IW3 XAnim root-motion (delta) parts are not converted yet: " +
                                     source.at("name").get<std::string>());
    }
    Anim anim;
    anim.name = source.at("name");
    anim.numframes = source.at("numframes");
    anim.framerate = source.at("framerate");
    anim.frequency = source.at("frequency");
    anim.loop = source.at("loop");
    const auto counts = source.at("bone_count");
    if (counts.size() != anim.boneCount.size())
        throw std::runtime_error("IW3 XAnim bone-count table has the wrong size");
    for (size_t i = 0; i < anim.boneCount.size(); ++i)
        anim.boneCount[i] = counts[i].get<uint8_t>();
    for (const auto &name : source.at("names"))
        anim.names.push_back(name);
    for (const auto &note : source.at("notify"))
        anim.notify.emplace_back(note.at("name").get<std::string>(), note.at("time").get<float>());
    anim.dataByte = Array<uint8_t>(source, "data_byte");
    anim.dataShort = Array<int16_t>(source, "data_short");
    anim.dataInt = Array<int32_t>(source, "data_int");
    anim.randomDataByte = Array<uint8_t>(source, "random_data_byte");
    anim.randomDataShort = Array<int16_t>(source, "random_data_short");
    anim.randomDataInt = Array<int32_t>(source, "random_data_int");
    anim.indices = Array<uint16_t>(source, "indices");
    if (anim.names.size() != anim.boneCount[kPartTypeAll] || anim.notify.size() > 255 ||
        anim.dataShort.size() > UINT16_MAX || anim.dataInt.size() > UINT16_MAX ||
        anim.randomDataInt.size() > UINT16_MAX || !anim.numframes ||
        (anim.numframes < 256 && std::any_of(anim.indices.begin(), anim.indices.end(),
                                             [](uint16_t index) { return index > 255; })))
        throw std::runtime_error("inconsistent IW3 XAnim dump: " + anim.name);
    return anim;
}

void Register(ZoneWriter &writer, const Anim &anim)
{
    std::vector<uint32_t> names;
    for (const auto &name : anim.names)
        names.push_back(writer.internScriptString(name));
    std::vector<std::pair<uint32_t, float>> notify;
    for (const auto &[name, time] : anim.notify)
        notify.emplace_back(writer.internScriptString(name), time);

    writer.add(ASSET_TYPE_XANIM, anim.name, [anim, names, notify](ZoneWriter &out) {
        std::array<uint8_t, iw8sz::XANIM> parts{};
        const auto follows = [](const bool present) { return present ? PTR_FOLLOWS : PTR_NULL; };
        Store(parts.data(), kName, PTR_FOLLOWS);
        Store(parts.data(), kNames, follows(!names.empty()));
        Store(parts.data(), kDataByte, follows(!anim.dataByte.empty()));
        Store(parts.data(), kDataShort, follows(!anim.dataShort.empty()));
        Store(parts.data(), kDataInt, follows(!anim.dataInt.empty()));
        Store(parts.data(), kRandomDataShort, follows(!anim.randomDataShort.empty()));
        Store(parts.data(), kRandomDataByte, follows(!anim.randomDataByte.empty()));
        Store(parts.data(), kRandomDataInt, follows(!anim.randomDataInt.empty()));
        Store(parts.data(), kIndices, follows(!anim.indices.empty()));
        Store(parts.data(), kNotify, follows(!notify.empty()));
        // Shipped anims carry a delta part even without root motion (all three
        // members null); match them.
        Store(parts.data(), kDeltaPart, PTR_FOLLOWS);
        Store(parts.data(), kRandomDataShortCount, static_cast<uint32_t>(anim.randomDataShort.size()));
        Store(parts.data(), kRandomDataByteCount, static_cast<uint32_t>(anim.randomDataByte.size()));
        Store(parts.data(), kIndexCount, static_cast<uint32_t>(anim.indices.size()));
        Store(parts.data(), kFramerate, anim.framerate);
        Store(parts.data(), kFrequency, anim.frequency);
        Store(parts.data(), kDataByteCount, static_cast<uint32_t>(anim.dataByte.size()));
        Store(parts.data(), kDataShortCount, static_cast<uint16_t>(anim.dataShort.size()));
        Store(parts.data(), kDataIntCount, static_cast<uint16_t>(anim.dataInt.size()));
        Store(parts.data(), kRandomDataIntCount, static_cast<uint16_t>(anim.randomDataInt.size()));
        Store(parts.data(), kNumFrames, anim.numframes);
        parts[kFlags] = static_cast<uint8_t>(kFlagShipped | (anim.loop ? kFlagLoop : 0));
        std::memcpy(parts.data() + kBoneCount, anim.boneCount.data(), anim.boneCount.size());
        parts[kNotifyCount] = static_cast<uint8_t>(notify.size());
        parts[kAssetType] = kAssetTypeProp;

        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(7);
        out.write(parts.data(), parts.size());
        // Load_XAnimParts order, all in the virtual stream (DB_PushStreamPos 5).
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(anim.name);
        Write(out, names, 3);
        if (!notify.empty())
        {
            out.align(3);
            for (const auto &[name, time] : notify)
            {
                out.writeT<uint32_t>(name);
                out.writeT<float>(time);
            }
        }
        out.align(7);
        const std::array<uint8_t, 24> delta{}; // trans, quat2, quat: none
        out.write(delta.data(), delta.size());
        Write(out, anim.dataByte, 0);
        Write(out, anim.dataShort, 1);
        Write(out, anim.dataInt, 3);
        Write(out, anim.randomDataShort, 1);
        Write(out, anim.randomDataByte, 0);
        Write(out, anim.randomDataInt, 3);
        if (anim.numframes < 256)
        {
            std::vector<uint8_t> bytes(anim.indices.begin(), anim.indices.end());
            Write(out, bytes, 0);
        }
        else
            Write(out, anim.indices, 1);
        out.popStream();
        out.popStream();
        // Postload_XAnimPartsPtr (0xDEA2D0) re-allocates the struct in stream 2.
        out.pushStream(XFILE_BLOCK_TEMP_POSTLOAD);
        out.align(7);
        out.reserveCalc(iw8sz::XANIM);
        out.popStream();
    });
}
} // namespace iw8::xanim
