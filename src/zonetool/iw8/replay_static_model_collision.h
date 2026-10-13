#pragma once

#include "iw8_map_structs.h"
#include "replay_spawns.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace iw8::staticmodelcollision
{
using Vec3 = std::array<float, 3>;
using Axis = std::array<Vec3, 3>;

struct Instance
{
    Vec3 origin{};
    Vec3 angles{};
    float scale{1.0f};
};

struct Model
{
    // name is the emitted XModel asset identity and is also interned in the zone's
    // ScriptStringList for StaticModelCollisionModel.name.
    std::string name;
    // Keep this exactly equal to the emitted PhysicsAsset asset name. The current
    // IW3 static-model converter uses the XModel identity for both names.
    std::string physicsAssetName;
    std::vector<Instance> instances;
};

struct PlannedModel
{
    const Model *source{};
    uint32_t scriptString{};
};

struct Plan
{
    std::vector<PlannedModel> models;
    uint32_t structureMemoryCost{};
    uint64_t totalInstances{};
};

inline Axis axisFromAngles(const Vec3 &angles)
{
    constexpr float radians = 0.01745329251994329577f;
    const float pitch = angles[0] * radians;
    const float yaw = angles[1] * radians;
    const float roll = angles[2] * radians;
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sr = std::sin(roll), cr = std::cos(roll);
    return {{{cp * cy, cp * sy, -sp},
             {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp},
             {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp}}};
}

inline Vec3 anglesFromAxis(const Axis &axis)
{
    constexpr float degrees = 57.2957795130823208768f;
    const float sinePitch = std::clamp(-axis[0][2], -1.0f, 1.0f);
    const float pitch = std::asin(sinePitch);
    const float cosinePitch = std::cos(pitch);
    float yaw{};
    float roll{};
    if (std::abs(cosinePitch) > 1.0e-6f)
    {
        yaw = std::atan2(axis[0][1], axis[0][0]);
        roll = std::atan2(axis[1][2], axis[2][2]);
    }
    else
    {
        // At +/-90 degrees pitch, yaw and roll are coupled. Choosing zero roll
        // preserves the matrix while keeping a stable single-valued result.
        yaw = std::atan2(-axis[1][0], axis[1][1]);
    }

    auto wrap = [degrees](const float radians) {
        float value = std::fmod(radians * degrees, 360.0f);
        if (value < 0.0f)
            value += 360.0f;
        if (value >= 360.0f - 1.0e-5f)
            value = 0.0f;
        return value;
    };
    return {wrap(pitch), wrap(yaw), wrap(roll)};
}

inline bool axesNearlyEqual(const Axis &left, const Axis &right, const float tolerance)
{
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            if (!std::isfinite(left[row][column]) || !std::isfinite(right[row][column]) ||
                std::abs(left[row][column] - right[row][column]) > tolerance)
                return false;
    return true;
}

inline Plan makePlan(const std::vector<Model> &source, ReplaySpawns &strings)
{
    Plan plan;
    std::unordered_set<std::string> identities;
    uint64_t structureMemoryCost = sizeof(maps::StaticModelCollisionModelList);
    for (const auto &model : source)
    {
        if (model.physicsAssetName.empty())
            continue;
        if (model.name.empty() || model.name.find('\0') != std::string::npos ||
            model.physicsAssetName.find('\0') != std::string::npos || model.instances.empty() ||
            !identities.insert(model.name).second)
            throw std::runtime_error("invalid or duplicate static-model collision identity");
        if (model.instances.size() > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("static-model collision instance count exceeds 32 bits");

        const uint32_t scriptString = strings.intern(model.name);
        if (scriptString == 0 || strings.strings.at(scriptString) != model.name)
            throw std::runtime_error("static-model collision name did not intern exactly");
        plan.models.push_back({&model, scriptString});
        if (structureMemoryCost > std::numeric_limits<uint32_t>::max() -
                                      sizeof(maps::StaticModelCollisionModel))
            throw std::runtime_error("static-model collision structure cost exceeds 32 bits");
        structureMemoryCost += sizeof(maps::StaticModelCollisionModel);
        const uint64_t instanceBytes =
            static_cast<uint64_t>(model.instances.size()) *
            sizeof(maps::StaticModelCollisionInstance);
        if (instanceBytes > std::numeric_limits<uint32_t>::max() - structureMemoryCost)
            throw std::runtime_error("static-model collision structure cost exceeds 32 bits");
        structureMemoryCost += instanceBytes;
        plan.totalInstances += model.instances.size();
        if (plan.totalInstances > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("static-model collision total instances exceed 32 bits");
    }
    if (plan.models.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("static-model collision model count exceeds 32 bits");
    if (!plan.models.empty())
        plan.structureMemoryCost = static_cast<uint32_t>(structureMemoryCost);
    return plan;
}

} // namespace iw8::staticmodelcollision
