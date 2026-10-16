#pragma once
// Simulator export: the converter state the zone writer serializes but the
// kept intermediate (render.json, images) does not carry: XModel geometry
// blobs, static placements, dynamic entities and the sun. Written beside the
// render data when IW8_ZONETOOL_KEEP_SCRATCH is set, read by `iw8-zonetool sim`.
#include <string>

namespace iw3
{
struct PreparedMap;
}
namespace iw8
{
struct MapSun;
}

namespace sim
{
// Writes <dumpDirectory>/<assetName>.sim.json and <assetName>.sim.bin.
void Export(const std::string &dumpDirectory, const std::string &assetName,
            const std::string &map, const iw3::PreparedMap &prepared, const iw8::MapSun &sun,
            const std::string &style);
} // namespace sim
