#pragma once
#include "iw8_structs.h"
#include "iw8_zonebuffer.h"
#include "replay_havok.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace iw8
{

// Both the server map writer and ZoneWriter use the same native PhysicsAsset body.
// The caller supplies the top-level XAsset entry and its DB_InsertPointer slot.
template <typename Writer>
void emitPhysicsAssetBody(Writer &writer, const havok::PhysicsAsset &asset)
{
    if (asset.name.empty() || asset.havokData.empty() ||
        asset.havokData.size() > std::numeric_limits<std::uint32_t>::max() || !asset.contents ||
        !((asset.useCategory == 0 && asset.simulationCategory == 0) ||
          (asset.useCategory == 3 && asset.simulationCategory == 1) ||
          (asset.useCategory == 7 && asset.simulationCategory == 9)))
        throw std::runtime_error("emitPhysicsAssetBody: invalid native physics asset");

    std::array<std::uint8_t, iw8sz::PHYSICSASSET> body{};
    const auto put = [&](const std::size_t offset, const auto value) {
        if (offset + sizeof(value) > body.size())
            throw std::runtime_error("emitPhysicsAssetBody: field exceeds Replay layout");
        std::memcpy(body.data() + offset, &value, sizeof(value));
    };
    put(0x00, PTR_FOLLOWS);
    put(0x08, PTR_FOLLOWS);
    put(0x10, static_cast<std::uint32_t>(asset.havokData.size()));
    put(0x14, asset.useCategory);
    put(0x18, std::uint32_t{1});
    put(0x20, PTR_FOLLOWS);
    put(0x28, PTR_FOLLOWS);
    put(0x30, std::uint32_t{1});
    put(0x34, std::uint32_t{1});
    put(0x38, PTR_FOLLOWS);
    put(0x40, PTR_FOLLOWS);
    put(0x54, std::uint32_t{asset.useCategory == 0 ? 0u : 1u});

    writer.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
    writer.align(7);
    writer.write(body.data(), body.size());
    writer.pushStream(XFILE_BLOCK_VIRTUAL);
    writer.writeStr(asset.name.c_str());
    writer.align(15);
    writer.write(asset.havokData.data(), asset.havokData.size());
    writer.align(3);
    writer.writeT(asset.simulationCategory);
    writer.align(3);
    writer.writeT(asset.contents);
    writer.align(7);
    writer.writeT(std::uint64_t{0});
    writer.align(7);
    writer.writeT(std::uint64_t{0});
    writer.popStream();
    writer.popStream();
}

template <typename Writer>
void emitPhysicsAssetReferenceBody(Writer &writer, const std::string &name)
{
    if (name.empty() || name.front() == ',' || name.find('\0') != std::string::npos)
        throw std::runtime_error("invalid PhysicsAsset reference name");
    std::array<std::uint8_t, iw8sz::PHYSICSASSET> body{};
    const std::uint64_t follows = PTR_FOLLOWS;
    std::memcpy(body.data(), &follows, sizeof(follows));
    writer.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
    writer.align(7);
    writer.write(body.data(), body.size());
    writer.pushStream(XFILE_BLOCK_VIRTUAL);
    writer.writeStr(("," + name).c_str());
    writer.popStream();
    writer.popStream();
}

} // namespace iw8
