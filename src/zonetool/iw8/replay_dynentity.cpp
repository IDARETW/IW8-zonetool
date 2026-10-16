#include "replay_dynentity.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace iw8
{
namespace
{
constexpr char AssetName[] = "dynentitylist0";
constexpr std::uint32_t InvalidNode = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t SpatialPopulationSize = 0x38;
// Clutter moves when shot; the leaf bounds the spawn origins with some slack.
constexpr float SpatialLeafPadding = 128.0f;

template <typename T> void Put(std::uint8_t *bytes, const std::size_t offset, const T &value)
{
    std::memcpy(bytes + offset, &value, sizeof(value));
}

using EntityPartition = std::array<std::vector<const DynamicEntity *>, 2>;

EntityPartition Partition(const std::vector<DynamicEntity> &entities)
{
    EntityPartition result;
    for (const auto &entity : entities)
    {
        const auto basis = static_cast<std::size_t>(entity.basis);
        if (basis >= result.size())
            throw std::runtime_error("Replay dynamic entity has an invalid basis");
        result[basis].push_back(&entity);
    }
    return result;
}

// Stock mp_hideout clutter (verified ACTS dump 2026-10-16): type CLUTTER, priority
// DYNENT_RADIAL_PRIORITY_LEVEL_LOW, spawnActive false, clientId 0xFFFF/0xFFFF
// (unassigned), scriptableMapIndex 0, linked into its leaf's bucket chain
// (head -> ... -> tail, tail next -1). Stock keeps only scriptable physics on the
// no-spatial list (ALWAYS_ACTIVE, spawnActive, clientId 0); clutter placed there
// was never activated, so no prop rendered.
void WriteDefinitions(ZoneWriter &writer, const std::vector<const DynamicEntity *> &entities,
                      const DynamicEntityBasis basis, const std::uint32_t globalBase)
{
    if (entities.empty())
        return;

    writer.align(7);
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const auto &entity = *entities[index];
        const auto globalIndex = globalBase + static_cast<std::uint32_t>(index);
        std::uint8_t definition[0x50]{};
        if (basis == DynamicEntityBasis::model)
            Put(definition, 0x00, writer.assetAlias(ASSET_TYPE_XMODEL, entity.model));
        std::memcpy(definition + 0x10, entity.quaternion.data(), sizeof(entity.quaternion));
        std::memcpy(definition + 0x20, entity.origin.data(), sizeof(entity.origin));
        // Each chain runs from its last entry down to its basis' first.
        Put(definition, 0x2C, globalIndex == globalBase ? InvalidNode : globalIndex - 1);
        definition[0x31] = static_cast<std::uint8_t>(basis);
        definition[0x32] = 1; // DYNENT_TYPE_CLUTTER
        if (basis == DynamicEntityBasis::model)
        {
            definition[0x33] = 0;                            // DYNENT_RADIAL_PRIORITY_LEVEL_LOW
            Put<std::uint32_t>(definition, 0x34, 0);         // spatial leaf 0
            Put<std::uint16_t>(definition, 0x38, 0xFFFF);    // clientId: none yet
            Put<std::uint16_t>(definition, 0x3A, 0xFFFF);
            Put<std::uint32_t>(definition, 0x3C, 0);         // scriptableMapIndex (stock 0)
        }
        else
        {
            definition[0x33] = 0xFF;                           // always active
            Put<std::uint32_t>(definition, 0x3C, InvalidNode); // no ScriptableMap index
            Put(definition, 0x42, entity.brushModel);
            definition[0x49] = 1; // spawnActive
        }
        Put<std::uint16_t>(definition, 0x40, 0); // dynentitylist0
        definition[0x48] = entity.rigidBodies;  // physicsAssetRigidBodiesCount
        definition[0x4A] = entity.noPhysics ? 1 : 0;
        definition[0x4D] = entity.collisionFlags;
        writer.write(definition, sizeof(definition));
    }
}

void EmitBody(ZoneWriter &writer, const std::vector<DynamicEntity> &entities)
{
    if (entities.empty())
        throw std::runtime_error("invalid Replay dynamic-entity count");
    const EntityPartition partition = Partition(entities);
    const auto counts = CountDynamicEntities(entities);
    const auto total = counts.models + counts.brushes;

    std::uint8_t list[iw8sz::DYNENTITYLIST]{};
    Put(list, 0x00, PTR_FOLLOWS);
    Put<std::uint16_t>(list, 0x08, 0);
    Put(list, 0x0C, counts.models);
    Put(list, 0x10, counts.brushes);
    Put(list, 0x14, total);
    Put(list, 0x18, counts.models ? PTR_FOLLOWS : PTR_NULL);
    Put(list, 0x20, counts.brushes ? PTR_FOLLOWS : PTR_NULL);
    Put(list, 0x28, PTR_FOLLOWS);
    Put(list, 0x30, PTR_FOLLOWS);
    // dynEntNoSpatialList head/tail: brush dynents only.
    Put(list, 0x38, counts.brushes ? total - 1 : InvalidNode);
    Put(list, 0x3C, counts.brushes ? counts.models : InvalidNode);

    writer.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
    writer.align(7);
    writer.write(list, sizeof(list));
    writer.pushStream(XFILE_BLOCK_VIRTUAL);
    writer.writeStr(AssetName);
    WriteDefinitions(writer, partition[0], DynamicEntityBasis::model, 0);
    WriteDefinitions(writer, partition[1], DynamicEntityBasis::brush, counts.models);
    // q_Load_SpatialPartition_TransientPopulation DDFF90: the 0x38 struct, then
    // buckets, bucketTails and partitionIds (partitionCount u32 each, 4-aligned).
    // Population 0 = radial clutter, one leaf; population 1 (sparse occluders) empty.
    std::uint8_t radial[SpatialPopulationSize]{};
    if (counts.models)
    {
        Put(radial, 0x00, PTR_FOLLOWS);
        Put(radial, 0x08, PTR_FOLLOWS);
        Put(radial, 0x10, PTR_FOLLOWS);
        Put<std::uint32_t>(radial, 0x30, 1);
        Put<std::uint32_t>(radial, 0x34, counts.models);
    }
    writer.align(7);
    writer.write(radial, sizeof(radial));
    if (counts.models)
    {
        writer.align(3);
        writer.writeT<std::uint32_t>(counts.models - 1); // bucket head
        writer.align(3);
        writer.writeT<std::uint32_t>(0); // bucket tail
        writer.align(3);
        writer.writeT<std::uint32_t>(0); // partition id
    }
    const std::uint8_t emptyPopulation[SpatialPopulationSize]{};
    writer.align(7);
    writer.write(emptyPopulation, sizeof(emptyPopulation));
    writer.popStream();
    writer.popStream();
}
} // namespace

DynamicEntityCounts CountDynamicEntities(const std::vector<DynamicEntity> &entities)
{
    DynamicEntityCounts counts;
    counts.modelMins.fill(std::numeric_limits<float>::max());
    counts.modelMaxs.fill(std::numeric_limits<float>::lowest());
    for (const auto &entity : entities)
    {
        std::uint32_t *count = nullptr;
        switch (entity.basis)
        {
        case DynamicEntityBasis::model:
            count = &counts.models;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                counts.modelMins[axis] =
                    std::min(counts.modelMins[axis], entity.origin[axis] - SpatialLeafPadding);
                counts.modelMaxs[axis] =
                    std::max(counts.modelMaxs[axis], entity.origin[axis] + SpatialLeafPadding);
            }
            break;
        case DynamicEntityBasis::brush:
            count = &counts.brushes;
            break;
        default:
            throw std::runtime_error("Replay dynamic entity has an invalid basis");
        }
        if (*count == std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("Replay dynamic-entity basis exceeds MapEnts limits");
        ++*count;
    }
    if (!counts.models)
    {
        counts.modelMins.fill(0.0f);
        counts.modelMaxs.fill(0.0f);
    }
    return counts;
}

void RegisterDynamicEntityList(ZoneWriter &writer, const std::vector<DynamicEntity> &entities)
{
    for (const auto &entity : entities)
    {
        if (entity.basis == DynamicEntityBasis::model && entity.model.empty())
            throw std::runtime_error("Replay model dynamic entity has no XModel");
        if (entity.basis == DynamicEntityBasis::brush && !entity.model.empty())
            throw std::runtime_error("Replay brush dynamic entity unexpectedly names an XModel");
        float length = 0.0f;
        for (const float value : entity.quaternion)
        {
            if (!std::isfinite(value))
                throw std::runtime_error("Replay dynamic entity has an invalid quaternion");
            length += value * value;
        }
        if (std::abs(length - 1.0f) > 0.01f)
            throw std::runtime_error("Replay dynamic entity has a non-unit quaternion");
        for (const float value : entity.origin)
            if (!std::isfinite(value))
                throw std::runtime_error("Replay dynamic entity has an invalid origin");
    }
    if (!entities.empty())
    {
        writer.add(ASSET_TYPE_DYNENTITYLIST, AssetName,
                   [entities](ZoneWriter &output) { EmitBody(output, entities); });
    }
}
} // namespace iw8
