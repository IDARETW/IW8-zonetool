#include "sim/sim_export.h"

#include "common/fs_util.h"
#include "common/json.hpp"
#include "common/log.h"
#include "convert/conv_xsurface.h"
#include "dumpsrc/xmodel_dump.h"
#include "iw3/iw3_fastfile.h"
#include "iw8/map_zone.h"
#include "iw8/replay_dynentity.h"
#include "iw8/replay_render.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace sim
{
using Json = nlohmann::json;
using zt::path_join;

void Export(const std::string &dumpDirectory, const std::string &assetName,
            const std::string &map, const iw3::PreparedMap &prepared, const iw8::MapSun &sun,
            const std::string &style)
{
    const std::string jsonPath = path_join(dumpDirectory, assetName + ".sim.json");
    const std::string binPath = path_join(dumpDirectory, assetName + ".sim.bin");
    std::ofstream bin(binPath, std::ios::binary);
    if (!bin)
        throw std::runtime_error("sim: cannot write " + binPath);
    std::uint64_t cursor = 0;
    const auto blob = [&](const void *data, const std::size_t size) {
        const std::uint64_t at = cursor;
        bin.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        cursor += size;
        return at;
    };

    Json xmodels = Json::array();
    for (const auto &model : prepared.xmodels)
    {
        Json lods = Json::array();
        for (std::size_t lodIndex = 0; lodIndex < model.lods.size(); ++lodIndex)
        {
            const conv_xsurf::Iw8Surfs &surfs = model.lods[lodIndex];
            Json surfaces = Json::array();
            for (const auto &surface : surfs.surfaces)
            {
                // Rigid vertex runs: bone-space vertex ranges drawn with that
                // bone's matrix (triOffset/triCount in triangles).
                Json runs = Json::array();
                for (const auto &run : surface.rigidVertLists)
                    runs.push_back({{"bone", run.boneIndex},
                                    {"vertCount", run.vertCount},
                                    {"triOffset", run.triOffset},
                                    {"triCount", run.triCount}});
                surfaces.push_back(
                    {{"runs", std::move(runs)},
                     {"vertCount", surface.vertCount},
                     {"triCount", surface.triCount},
                     {"vertOffset", surface.sharedVertDataOffset},
                     {"indexOffset", surface.sharedIndexDataOffset},
                     {"colorOffset", surface.sharedColorDataOffset},
                     {"mid", {surface.boundsMid[0], surface.boundsMid[1], surface.boundsMid[2]}},
                     {"half",
                      {surface.boundsHalf[0], surface.boundsHalf[1], surface.boundsHalf[2]}}});
            }
            const float distance =
                lodIndex < model.model.lods.size() ? model.model.lods[lodIndex].dist : 0.0f;
            lods.push_back({{"name", surfs.name},
                            {"distance", distance},
                            {"blobOffset", blob(surfs.sharedBlob.data(), surfs.sharedBlob.size())},
                            {"blobSize", surfs.sharedBlob.size()},
                            {"surfaces", std::move(surfaces)}});
        }
        // Model-space bind pose per bone (XModel baseMat): bone-space vertices
        // of multi-part models (vehicles) need it.
        Json bones = Json::array();
        for (std::size_t bone = 0; bone < model.model.skeleton.baseMat.size(); ++bone)
        {
            const auto &pose = model.model.skeleton.baseMat[bone];
            bones.push_back({{"name", bone < model.model.boneNames.size() ? model.model.boneNames[bone] : ""},
                             {"quat", pose.quat},
                             {"trans", pose.trans}});
        }
        xmodels.push_back({{"name", model.model.name},
                           {"bones", std::move(bones)},
                           {"materials", model.model.materials},
                           {"mid", {model.model.boundsMid[0], model.model.boundsMid[1],
                                    model.model.boundsMid[2]}},
                           {"half", {model.model.boundsHalf[0], model.model.boundsHalf[1],
                                     model.model.boundsHalf[2]}},
                           {"lods", std::move(lods)}});
    }

    Json staticModels = Json::array();
    for (const auto &model : prepared.staticModels.models)
    {
        Json lods = Json::array();
        for (const auto &lod : model.lods)
        {
            Json surfaces = Json::array();
            for (const auto &surface : lod.surfaces)
                surfaces.push_back({{"material", surface.material}, {"cullType", surface.cullType}});
            lods.push_back({{"distance", lod.distance}, {"surfaces", std::move(surfaces)}});
        }
        staticModels.push_back({{"name", model.name},
                                {"mid", model.bounds.midpoint},
                                {"half", model.bounds.halfSize},
                                {"lods", std::move(lods)}});
    }
    Json instances = Json::array();
    for (const auto &instance : prepared.staticModels.instances)
        instances.push_back({{"model", instance.model},
                             {"origin", instance.origin},
                             {"quaternion", instance.quaternion},
                             {"scale", instance.scale}});
    Json dynamic = Json::array();
    for (const auto &entity : prepared.dynamicEntities)
        dynamic.push_back({{"model", entity.model},
                           {"origin", entity.origin},
                           {"quaternion", entity.quaternion},
                           {"brush", entity.basis == iw8::DynamicEntityBasis::brush}});

    Json root = {
        {"schema", 1},
        {"map", map},
        {"assetName", assetName},
        {"style", style},
        {"bin", assetName.substr(assetName.find_last_of('/') + 1) + ".sim.bin"},
        {"render", assetName.substr(assetName.find_last_of('/') + 1) + ".render.json"},
        {"ents", assetName.substr(assetName.find_last_of('/') + 1) + ".ents"},
        {"sun",
         {{"direction", {sun.direction[0], sun.direction[1], sun.direction[2]}},
          {"color", {sun.color[0], sun.color[1], sun.color[2]}},
          {"intensity", sun.intensity},
          {"primaryLightIndex", sun.sunPrimaryLightIndex}}},
        {"xmodels", std::move(xmodels)},
        {"staticModels", std::move(staticModels)},
        {"instances", std::move(instances)},
        {"dynamicEntities", std::move(dynamic)},
    };
    if (sun.sunPrimaryLightIndex < sun.primaryLights.size())
    {
        const auto &light = sun.primaryLights[sun.sunPrimaryLightIndex];
        root["sun"]["primaryColor"] = {light.color[0], light.color[1], light.color[2]};
    }
    std::ofstream json(jsonPath, std::ios::binary);
    json << root.dump();
    zt::info("sim: exported %zu xmodels, %zu placements, %zu dynamic entities to %s",
             prepared.xmodels.size(), prepared.staticModels.instances.size(),
             prepared.dynamicEntities.size(), jsonPath.c_str());
}
} // namespace sim
