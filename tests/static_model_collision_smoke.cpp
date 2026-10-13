#include "common/json.hpp"
#include "zonetool/iw8/map_zone.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Json = nlohmann::json;
using Axis = iw8::staticmodelcollision::Axis;

void require(const bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

uint64_t read64(const std::vector<uint8_t> &bytes, const size_t offset)
{
    uint64_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

template <typename T> T readValue(const std::vector<uint8_t> &bytes, const size_t offset)
{
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::string readString(const std::vector<uint8_t> &bytes, size_t &offset)
{
    std::string value;
    while (offset < bytes.size() && bytes[offset])
        value.push_back(static_cast<char>(bytes[offset++]));
    require(offset < bytes.size(), "serialized string is not NUL-terminated");
    ++offset;
    return value;
}

Axis normalizedAxis(const Json &source)
{
    Axis axis{};
    for (size_t row = 0; row < 3; ++row)
    {
        float lengthSquared = 0.0f;
        for (size_t column = 0; column < 3; ++column)
        {
            axis[row][column] = source.at(row).at(column).get<float>();
            lengthSquared += axis[row][column] * axis[row][column];
        }
        const float length = std::sqrt(lengthSquared);
        require(std::isfinite(length) && length > 0.0f, "source axis row is invalid");
        for (float &component : axis[row])
            component /= length;
    }
    return axis;
}

void testSourceAxisRoundTrips(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "cannot read the source-world audit JSON");
    Json world;
    file >> world;
    const auto &instances = world.at("models");
    require(instances.is_array() && !instances.empty(), "source world has no static instances");

    size_t checked = 0;
    for (const auto &instance : instances)
    {
        const Axis source = normalizedAxis(instance.at("axis"));
        const auto angles = iw8::staticmodelcollision::anglesFromAxis(source);
        const Axis reconstructed = iw8::staticmodelcollision::axisFromAngles(angles);
        require(iw8::staticmodelcollision::axesNearlyEqual(source, reconstructed, 2.0e-4f),
                "source static-model axis did not round-trip through native Euler angles");
        for (const auto value : instance.at("origin"))
            require(std::isfinite(value.get<float>()), "source static-model origin is invalid");
        const float scale = instance.at("scale").get<float>();
        require(std::isfinite(scale) && scale > 0.0f, "source static-model scale is invalid");
        ++checked;
    }
    std::cout << "source_axis_round_trips=" << checked << '\n';
}

void testStringTableAndListEmission()
{
    const std::string entities =
        "{\n212 \"mp_test_spawn\"\n709 \"1 2 3\"\n80 \"0 0 0\"\n}";
    iw8::ReplaySpawns strings(entities);
    require(strings.records.size() == 1, "fixture spawn was not parsed");
    const uint32_t spawnName = strings.records.front().name;
    const std::string spawnText = strings.strings.at(spawnName);

    iw8::staticmodelcollision::Model first;
    first.name = "mw120r/mp_4doffice/smodel_7";
    first.physicsAssetName = "mw120r/mp_4doffice/smodel_7";
    first.instances = {{{170.0f, 80.0f, 0.0f}, {0.0f, 310.0f, 0.0f}, 1.0f},
                       {{162.0f, 36.0f, 0.0f}, {0.0f, 44.0f, 0.0f}, 1.0f}};
    iw8::staticmodelcollision::Model second;
    second.name = "mw120r/mp_4doffice/smodel_8";
    second.physicsAssetName = first.physicsAssetName; // exercise a shared-PA alias
    second.instances = {{{208.0f, 10.0f, 0.0f}, {0.0f, 90.0f, 0.0f}, 1.0f}};
    const std::vector<iw8::staticmodelcollision::Model> models{first, second};
    const auto plan = iw8::staticmodelcollision::makePlan(models, strings);
    require(plan.models.size() == 2 && plan.totalInstances == 3,
            "static-model collision plan dropped a row or placement");
    require(strings.strings.at(spawnName) == spawnText,
            "interning static-model identities changed an existing spawn string");
    require(strings.strings.at(plan.models[0].scriptString) == first.name &&
                strings.strings.at(plan.models[1].scriptString) == second.name,
            "static-model collision identity does not resolve through ScriptStringList");
    require(plan.structureMemoryCost == 24 + 2 * 40 + 3 * 28,
            "native structureMemoryCost does not match the Backlot layout formula");

    iw8::ReplaySpawns collisionOnlyStrings("");
    const auto collisionOnlyPlan = iw8::staticmodelcollision::makePlan(models,
                                                                        collisionOnlyStrings);
    require(collisionOnlyStrings.records.empty() && collisionOnlyPlan.models.size() == 2 &&
                collisionOnlyStrings.strings.size() == 4,
            "collision-only ScriptStringList did not intern static model identities");
    iw8::ZoneBuffer collisionOnlyBuffer;
    collisionOnlyBuffer.pushStream(iw8::XFILE_BLOCK_VIRTUAL);
    collisionOnlyStrings.writeStrings(collisionOnlyBuffer);
    collisionOnlyBuffer.popStream();
    const auto &collisionOnlyBytes = collisionOnlyBuffer.data();
    require(read64(collisionOnlyBytes, 0) == iw8::PTR_NULL &&
                read64(collisionOnlyBytes, sizeof(uint64_t)) == iw8::PTR_FOLLOWS,
            "collision-only ScriptStringList omitted its reserved string entries");
    size_t collisionOnlyOffset = collisionOnlyStrings.strings.size() * sizeof(uint64_t);
    for (size_t index = 1; index < collisionOnlyStrings.strings.size(); ++index)
        require(readString(collisionOnlyBytes, collisionOnlyOffset) ==
                    collisionOnlyStrings.strings[index],
                "collision-only ScriptStringList does not serialize static identities");
    require(collisionOnlyOffset == collisionOnlyBytes.size(),
            "collision-only ScriptStringList has trailing bytes");

    iw8::ZoneBuffer stringBuffer;
    stringBuffer.pushStream(iw8::XFILE_BLOCK_VIRTUAL);
    strings.writeStrings(stringBuffer);
    stringBuffer.popStream();
    const auto &stringBytes = stringBuffer.data();
    require(read64(stringBytes, 0) == iw8::PTR_NULL,
            "ScriptStringList slot zero must remain the null string");
    const size_t stringCount = strings.strings.size();
    for (size_t index = 1; index < stringCount; ++index)
        require(read64(stringBytes, index * sizeof(uint64_t)) == iw8::PTR_FOLLOWS,
                "ScriptStringList lost a non-null string pointer");
    size_t stringOffset = stringCount * sizeof(uint64_t);
    for (size_t index = 1; index < stringCount; ++index)
        require(readString(stringBytes, stringOffset) == strings.strings[index],
                "ScriptStringList strings do not round-trip in index order");
    require(stringOffset == stringBytes.size(), "ScriptStringList has unexpected trailing bytes");

    iw8::ZoneBuffer buffer;
    buffer.pushStream(iw8::XFILE_BLOCK_VIRTUAL);
    iw8::emitStaticModelCollisionModelLists(buffer, plan);
    buffer.popStream();
    const auto &bytes = buffer.data();
    require(bytes.size() >= sizeof(iw8::maps::StaticModelCollisionModelList) +
                                2 * sizeof(iw8::maps::StaticModelCollisionModel),
            "serialized static-model collision list is truncated");
    const auto list = readValue<iw8::maps::StaticModelCollisionModelList>(bytes, 0);
    require(list.numModels == 2 && list.structureMemoryCost == plan.structureMemoryCost &&
                reinterpret_cast<uintptr_t>(list.models) == iw8::PTR_FOLLOWS,
            "serialized native list header differs from the prepared plan");
    const size_t rowOffset = sizeof(list);
    const auto row0 = readValue<iw8::maps::StaticModelCollisionModel>(bytes, rowOffset);
    const auto row1 = readValue<iw8::maps::StaticModelCollisionModel>(
        bytes, rowOffset + sizeof(row0));
    require(row0.name == plan.models[0].scriptString && row1.name == plan.models[1].scriptString,
            "serialized model identity indices do not match ScriptStringList");
    require(reinterpret_cast<uintptr_t>(row0.physicsAsset) == iw8::PTR_INSERT &&
                reinterpret_cast<uintptr_t>(row1.physicsAsset) ==
                    ((uint64_t(iw8::XFILE_BLOCK_VIRTUAL) << 32) | (rowOffset + 2 * sizeof(row0) + 1)),
            "PhysicsAsset insertion and alias fields do not match native pointer rules");
    require(reinterpret_cast<uintptr_t>(row0.detailCollision) == iw8::PTR_NULL &&
                reinterpret_cast<uintptr_t>(row1.detailCollision) == iw8::PTR_NULL &&
                row0.numInstances == 2 && row1.numInstances == 1 &&
                reinterpret_cast<uintptr_t>(row0.instances) == iw8::PTR_FOLLOWS &&
                reinterpret_cast<uintptr_t>(row1.instances) == iw8::PTR_FOLLOWS,
            "serialized per-model instance pointers or counts are invalid");

    size_t offset = rowOffset + 2 * sizeof(row0);
    const uint64_t paStubName = read64(bytes, offset);
    require(paStubName == iw8::PTR_FOLLOWS, "PhysicsAsset ref stub has no following name");
    offset += iw8sz::PHYSICSASSET;
    require(readString(bytes, offset) == "," + first.physicsAssetName,
            "PhysicsAsset ref stub must use Replay's comma-prefixed reference name");
    for (const auto &instance : first.instances)
    {
        const auto wire = readValue<iw8::maps::StaticModelCollisionInstance>(bytes, offset);
        for (size_t axis = 0; axis < 3; ++axis)
        {
            require(wire.origin.v[axis] == instance.origin[axis] &&
                        wire.angles.v[axis] == instance.angles[axis],
                    "serialized instance origin or angle changed");
        }
        require(wire.scale == instance.scale, "serialized instance scale changed");
        offset += sizeof(wire);
    }
    const auto finalInstance = readValue<iw8::maps::StaticModelCollisionInstance>(bytes, offset);
    require(finalInstance.origin.v[0] == second.instances[0].origin[0] &&
                finalInstance.origin.v[1] == second.instances[0].origin[1] &&
                finalInstance.origin.v[2] == second.instances[0].origin[2] &&
                finalInstance.angles.v[1] == second.instances[0].angles[1],
            "aliased PhysicsAsset row lost its per-instance transform");
    std::cout << "serialized_models=" << list.numModels
              << " serialized_instances=" << plan.totalInstances
              << " script_strings=" << strings.strings.size() << '\n';
}
} // namespace

int main(const int argc, char **argv)
{
    try
    {
        if (argc != 2)
            throw std::runtime_error("usage: static-model-collision-smoke <source-world.json>");
        testSourceAxisRoundTrips(argv[1]);
        testStringTableAndListEmission();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
