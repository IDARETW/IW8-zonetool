#include "common/ff_io.h"
#include "common/fs_util.h"
#include "common/json.hpp"
#include "common/log.h"
#include "convert/maps_convert.h"
#include "dumpsrc/dump_source.h"
#include "dumpsrc/image_dump.h"
#include "dumpsrc/material_dumpsrc.h"
#include "dumpsrc/xmodel_dump.h"
#include "iw3/iw3_fastfile.h"
#include "iw3/iw3_vision.h"
#include "iw3/iw3_scripts.h"
#include "iw7/iw7_fastfile.h"
#include "iw8/iw8_ffheader.h"
#include "iw8/iw8_zone.h"
#include "iw8/map_zone.h"
#include "iw8/maps_write.h"
#include "iw8/replay_dynentity.h"
#include "iw8/replay_havok.h"
#include "iw8/replay_impact.h"
#include "iw8/replay_navigation.h"
#include "iw8/replay_rawfile.h"
#include "iw8/replay_grading.h"
#include "iw8/replay_render.h"
#include "iw8/replay_script.h"
#include "iw8/replay_vfx.h"
#include "iw8/write_xsurface.h"
#include "iw8/write_weapon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>

using namespace zt;

namespace
{
struct Args
{
    std::string command;
    std::vector<std::string> positional;
    std::string outputDirectory;
    std::string replayExecutable;
    std::string collisionPath;
    std::string footstepsPath;
    std::string unlinkerExecutable;
    std::vector<std::string> iw3Roots;
    std::vector<std::string> searchPaths;
    std::string metadataPath;
    std::string lightingProfile = "source";
    float sunIntensityScale = 1.0f;
    bool navigation = true;
    bool iw3ModeAssets = true;
    std::string gscTool;
};

struct MapMetadata
{
    bool present{};
    std::string id;
    std::string title;
    std::string description;
    unsigned compassRotation{};
    bool compassRotationSet{};
    // Contraction of the IW3 minimap region about its centre (see the compass
    // bounds below); build-time only, not written to the package map.json.
    float minimapScale{0.875f};
};

void printUsage()
{
    std::printf("iw8-zonetool - IW8 Replay 1.20 map compiler\n"
                "usage:\n"
                "  iw8-zonetool build-map <dump> <map> [-o <output>] [options]\n"
                "  iw8-zonetool build-iw3 <map.ff> [map] [-o <output>] [options]\n"
                "  iw8-zonetool build-weapon <weapon.json> [-o <output>]\n"
                "  iw8-zonetool build-weapon-placeholders <manifest.json> [-o <output>]\n"
                "  iw8-zonetool inspect-iw7 <file.ff>\n"
                "  iw8-zonetool inspect <file.ff>\n"
                "  iw8-zonetool validate-output <map_output> <map>\n"
                "  iw8-zonetool translate-iw3-script <maps/mp dir> <map> -o <out dir> [--gsc-tool <exe>]\n"
                "options:\n"
                "  --replay <game_dx12_ship_replay.exe>\n"
                "  --collision <native collision input>\n"
                "  --footsteps <native footstep input>\n"
                "  --unlinker <OpenAssetTools\\Unlinker.exe>\n"
                "  --iw3-root <CoD4 installation>\n"
                "  --search-path <IW3 asset directory>\n"
                "  --metadata <map.json>\n"
                "  --lighting-profile source|aniyah-incursion\n"
                "  --sun-intensity-scale <positive multiplier>\n"
                "  --no-navigation  (skip generated bot aipaths/navmesh/tacgraph)\n"
                "  --gsc-tool <gsc-tool.exe>  (compile converted IW3 map scripts; or IW8_GSC_TOOL)\n"
                "  -v  -q\n");
}

bool parseArgs(const int argc, char **argv, Args &args)
{
    if (argc < 2)
    {
        return false;
    }

    args.command = argv[1];
    for (int index = 2; index < argc; ++index)
    {
        const std::string value = argv[index];
        if (value == "-o" && index + 1 < argc)
        {
            args.outputDirectory = argv[++index];
        }
        else if (value == "--replay" && index + 1 < argc)
        {
            args.replayExecutable = argv[++index];
        }
        else if (value == "--collision" && index + 1 < argc)
        {
            args.collisionPath = argv[++index];
        }
        else if (value == "--footsteps" && index + 1 < argc)
        {
            args.footstepsPath = argv[++index];
        }
        else if (value == "--unlinker" && index + 1 < argc)
        {
            args.unlinkerExecutable = argv[++index];
        }
        else if (value == "--iw3-root" && index + 1 < argc)
        {
            args.iw3Roots.emplace_back(argv[++index]);
        }
        else if (value == "--search-path" && index + 1 < argc)
        {
            args.searchPaths.emplace_back(argv[++index]);
        }
        else if (value == "--metadata" && index + 1 < argc)
        {
            args.metadataPath = argv[++index];
        }
        else if (value == "--lighting-profile" && index + 1 < argc)
        {
            args.lightingProfile = argv[++index];
        }
        else if (value == "--sun-intensity-scale" && index + 1 < argc)
        {
            const std::string scale = argv[++index];
            size_t consumed = 0;
            try
            {
                args.sunIntensityScale = std::stof(scale, &consumed);
            }
            catch (const std::exception &)
            {
                return false;
            }
            if (consumed != scale.size() || !std::isfinite(args.sunIntensityScale) ||
                args.sunIntensityScale <= 0.0f)
            {
                return false;
            }
        }
        else if (value == "--gsc-tool" && index + 1 < argc)
        {
            args.gscTool = argv[++index];
        }
        else if (value == "--no-navigation")
        {
            args.navigation = false;
        }
        else if (value == "--iw3-mode-assets" && index + 1 < argc)
        {
            const std::string mode = argv[++index];
            if (mode != "on" && mode != "off")
                return false;
            args.iw3ModeAssets = mode == "on";
        }
        else if (value == "-v")
        {
            g_logLevel = 3;
        }
        else if (value == "-q")
        {
            g_logLevel = 0;
        }
        else
        {
            args.positional.push_back(value);
        }
    }
    return true;
}

bool isMapId(const std::string &map)
{
    if (map.size() < 4 || map.size() > 15 || !map.starts_with("mp_"))
    {
        return false;
    }
    return std::all_of(map.begin(), map.end(), [](const unsigned char character) {
        return character == '_' || (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    });
}

bool requireMapId(const std::string &map)
{
    if (isMapId(map))
    {
        return true;
    }
    err("map id '%s' is invalid; use at most 15 lower-case characters in mp_<name>", map.c_str());
    return false;
}

std::array<std::string, 5> zoneNames(const std::string &map)
{
    return {map, "srv_" + map, "eng_" + map, "ww_" + map, "techsets_" + map};
}

std::vector<std::string> expectedFiles(const std::string &map)
{
    std::vector<std::string> files;
    for (const std::string &zone : zoneNames(map))
    {
        files.push_back(zone + ".ff");
    }
    files.emplace_back("map.json");
    std::sort(files.begin(), files.end());
    return files;
}

bool readMetadata(const std::string &path, MapMetadata &metadata)
{
    metadata = {};
    if (path.empty())
    {
        return true;
    }

    std::ifstream input(path);
    if (!input)
    {
        err("metadata: cannot read %s", path.c_str());
        return false;
    }

    try
    {
        const nlohmann::json data = nlohmann::json::parse(input);
        if (!data.is_object())
        {
            throw std::runtime_error("the root must be an object");
        }
        const auto readString = [&data](const char *key, std::string &value) {
            const auto item = data.find(key);
            if (item == data.end())
            {
                return true;
            }
            if (!item->is_string())
            {
                return false;
            }
            value = item->get<std::string>();
            return true;
        };
        std::string name;
        if (!readString("id", metadata.id) || !readString("title", metadata.title) ||
            !readString("name", name) || !readString("description", metadata.description))
        {
            throw std::runtime_error("id, name, title, and description must be strings");
        }
        if (!name.empty() && !metadata.title.empty() && name != metadata.title)
            throw std::runtime_error("name and title must match when both are present");
        if (metadata.title.empty())
            metadata.title = std::move(name);
        if (const auto rotation = data.find("compassRotation"); rotation != data.end())
        {
            if (!rotation->is_number_unsigned())
                throw std::runtime_error("compassRotation must be 0, 90, 180, or 270 clockwise");
            metadata.compassRotation = rotation->get<unsigned>();
            metadata.compassRotationSet = true;
            if (metadata.compassRotation != 0 && metadata.compassRotation != 90 &&
                metadata.compassRotation != 180 && metadata.compassRotation != 270)
                throw std::runtime_error("compassRotation must be 0, 90, 180, or 270 clockwise");
        }
        if (const auto scale = data.find("minimapScale"); scale != data.end())
        {
            if (!scale->is_number() || scale->get<double>() < 0.5 || scale->get<double>() > 1.5)
                throw std::runtime_error("minimapScale must be a number from 0.5 to 1.5");
            metadata.minimapScale = scale->get<float>();
        }
        metadata.present = true;
    }
    catch (const std::exception &exception)
    {
        err("metadata: %s is invalid: %s", path.c_str(), exception.what());
        return false;
    }
    return true;
}

bool validateMetadata(const std::string &directory, const std::string &map, bool &present)
{
    const std::filesystem::path path = std::filesystem::path(directory) / "map.json";
    present = std::filesystem::exists(path);
    if (!present)
    {
        err("metadata: package is missing map.json");
        return false;
    }

    MapMetadata metadata;
    if (!readMetadata(path.string(), metadata))
    {
        return false;
    }
    if (!metadata.id.empty() && metadata.id != map)
    {
        err("metadata: id '%s' does not match output map '%s'", metadata.id.c_str(), map.c_str());
        return false;
    }
    return true;
}

bool writeMetadata(const std::string &directory, const MapMetadata &metadata)
{
    nlohmann::json data = nlohmann::json::object();
    if (!metadata.id.empty())
    {
        data["id"] = metadata.id;
    }
    if (!metadata.title.empty())
    {
        data["title"] = metadata.title;
    }
    if (!metadata.description.empty())
    {
        data["description"] = metadata.description;
    }
    if (metadata.compassRotation)
    {
        data["compassRotation"] = metadata.compassRotation;
    }
    std::ofstream output(std::filesystem::path(directory) / "map.json", std::ios::binary);
    if (!output)
    {
        err("metadata: cannot write map.json");
        return false;
    }
    output << data.dump(2) << '\n';
    return output.good();
}

// Optional package file: the map's loading-screen artwork for the custom-map
// UI ("MWRI", version 1, width, height, then RGBA8; 1..2048 on each side).
constexpr char kPreviewFile[] = "preview.rgba";

bool validPreview(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const auto size = static_cast<std::uint64_t>(input.tellg());
    input.seekg(0);
    std::uint32_t header[4]{};
    if (!input.read(reinterpret_cast<char *>(header), sizeof(header)) || header[0] != 0x4952574Du ||
        header[1] != 1 || header[2] < 1 || header[2] > 2048 || header[3] < 1 || header[3] > 2048)
        return false;
    return size == sizeof(header) + std::uint64_t(header[2]) * header[3] * 4;
}

bool prepareOutputDirectory(const std::string &outputDirectory, const std::string &map)
{
    std::error_code error;
    if (!std::filesystem::exists(outputDirectory, error))
    {
        return mkdirs(outputDirectory);
    }
    if (error || !std::filesystem::is_directory(outputDirectory, error))
    {
        err("build-map: output is not a directory: %s", outputDirectory.c_str());
        return false;
    }

    const std::vector<std::string> expected = expectedFiles(map);
    for (const auto &entry : std::filesystem::directory_iterator(outputDirectory, error))
    {
        if (error || !entry.is_regular_file(error) ||
            (std::find(expected.begin(), expected.end(), entry.path().filename().string()) ==
                 expected.end() &&
             entry.path().filename() != kPreviewFile))
        {
            err("build-map: output directory must be empty or contain only this map's files");
            return false;
        }
    }
    return !error;
}

struct ZoneValidation
{
    uint32_t assetCount{};
    std::array<uint32_t, ASSET_TYPE_COUNT> typeCounts{};
};

bool validateZoneFile(const std::string &path, ZoneValidation *details = nullptr)
{
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes) || bytes.size() < 0x8C + sizeof(IW8_XAssetList))
    {
        err("validate: cannot read a complete IW8 header from %s", path.c_str());
        return false;
    }

    IW8_DB_FFHeader header{};
    std::memcpy(&header, bytes.data(), (std::min)(bytes.size(), sizeof(header)));
    if (std::memcmp(header.magic, iw8ff::kMagicUnsec, 8) != 0 ||
        header.headerVersion != iw8ff::kHeaderVersion ||
        header.xfileVersion != iw8ff::kXFileVersion || header.dashCompressBuild != 0 ||
        header.dashEncryptBuild != 0)
    {
        err("validate: %s has an unsupported Replay header", path.c_str());
        return false;
    }
    if (std::memcmp(bytes.data() + 0x88, "\x01IWC", 4) != 0 ||
        header.xfileHeader.size != bytes.size() - 0x8C ||
        header.residentPartSize != bytes.size() - 0x88 ||
        header.alwaysLoadedPartSize != header.xfileHeader.size)
    {
        err("validate: %s has invalid resident framing", path.c_str());
        return false;
    }

    if (header.transientFileType != 0 || header.residentHash != 0 ||
        header.xfileHeader.preloadWalkSize != 0 ||
        header.xfileHeader.blockSize[iw8::XFILE_BLOCK_TEMP] != sizeof(IW8_XAssetList) ||
        header.xfileHeader.blockSize[iw8::XFILE_BLOCK_CALLBACK] != 0 ||
        header.xfileHeader.blockSize[iw8::XFILE_BLOCK_SCRIPT] != 0 ||
        header.xfileHeader.blockSize[iw8::XFILE_BLOCK_UNK10] != 0)
    {
        err("validate: %s has invalid Replay stream/header policy", path.c_str());
        return false;
    }

    uint64_t streamTotal = 0;
    for (const uint64_t size : header.xfileHeader.blockSize)
    {
        if (size > (std::numeric_limits<uint64_t>::max)() - streamTotal)
        {
            err("validate: %s has overflowing stream sizes", path.c_str());
            return false;
        }
        streamTotal += size;
    }
    if (streamTotal < header.xfileHeader.size)
    {
        err("validate: %s has invalid stream sizes", path.c_str());
        return false;
    }

    IW8_XAssetList root{};
    std::memcpy(&root, bytes.data() + 0x8C, sizeof(root));
    const bool hasStrings = root.stringList.count != 0;
    const bool hasAssets = root.assetCount != 0;
    if (root.stringList.count < 0 || root.stringList.loaded != 0 || root.assetReadPos != 0 ||
        root.stringList.strings !=
            (hasStrings ? iw8::PTR_FOLLOWS : iw8::PTR_NULL) ||
        root.assets != (hasAssets ? iw8::PTR_FOLLOWS : iw8::PTR_NULL) ||
        (hasAssets && root.assetCount >
                          header.xfileHeader.blockSize[iw8::XFILE_BLOCK_VIRTUAL] /
                              sizeof(IW8_XAsset)))
    {
        err("validate: %s has an invalid root asset list", path.c_str());
        return false;
    }

    size_t cursor = 0x8C + sizeof(root);
    if (hasStrings)
    {
        const size_t stringCount = static_cast<size_t>(root.stringList.count);
        if (stringCount > (bytes.size() - cursor) / sizeof(uint64_t))
        {
            err("validate: %s has a truncated script-string pointer list", path.c_str());
            return false;
        }
        for (size_t index = 0; index < stringCount; ++index)
        {
            uint64_t pointer{};
            std::memcpy(&pointer, bytes.data() + cursor, sizeof(pointer));
            cursor += sizeof(pointer);
            if (pointer != (index == 0 ? iw8::PTR_NULL : iw8::PTR_FOLLOWS))
            {
                err("validate: %s has an invalid script-string pointer", path.c_str());
                return false;
            }
        }
        for (size_t index = 1; index < stringCount; ++index)
        {
            const auto end = std::find(bytes.begin() + cursor, bytes.end(), uint8_t{});
            if (end == bytes.end())
            {
                err("validate: %s has an unterminated script string", path.c_str());
                return false;
            }
            cursor = static_cast<size_t>(end - bytes.begin()) + 1;
        }
    }

    if (root.assetCount > (bytes.size() - cursor) / sizeof(IW8_XAsset))
    {
        err("validate: %s has a truncated top-level asset list", path.c_str());
        return false;
    }
    std::array<uint32_t, ASSET_TYPE_COUNT> typeCounts{};
    for (uint32_t index = 0; index < root.assetCount; ++index)
    {
        IW8_XAsset asset{};
        std::memcpy(&asset, bytes.data() + cursor, sizeof(asset));
        cursor += sizeof(asset);
        if (asset.type < 0 || asset.type >= ASSET_TYPE_COUNT || asset._pad04 != 0 ||
            (asset.header != iw8::PTR_INSERT &&
             !(asset.type == ASSET_TYPE_LIGHTDEF && asset.header == iw8::PTR_FOLLOWS)))
        {
            err("validate: %s has an invalid top-level asset descriptor", path.c_str());
            return false;
        }
        ++typeCounts[asset.type];
    }
    std::string summary;
    for (size_t type = 0; type < typeCounts.size(); ++type)
    {
        if (!typeCounts[type])
            continue;
        if (!summary.empty())
            summary += ", ";
        const char *name = iw8sz::type_name(static_cast<int>(type));
        summary += std::strcmp(name, "unknown") == 0 ? "type" + std::to_string(type) : name;
        summary += '=';
        summary += std::to_string(typeCounts[type]);
    }
    info("validate: %s contains %u top-level asset(s)%s%s", path.c_str(), root.assetCount,
         summary.empty() ? "" : ": ", summary.c_str());
    if (details)
    {
        details->assetCount = root.assetCount;
        details->typeCounts = typeCounts;
    }
    return true;
}

bool validatePackage(const std::string &packageDirectory, const std::string &map)
{
    if (!requireMapId(map))
    {
        return false;
    }

    bool hasMetadata = false;
    if (!validateMetadata(packageDirectory, map, hasMetadata))
    {
        return false;
    }

    std::vector<std::string> actual;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(packageDirectory, error))
    {
        if (error || !entry.is_regular_file(error))
        {
            err("validate: package contains an unreadable or non-file entry");
            return false;
        }
        actual.push_back(entry.path().filename().string());
    }
    if (error)
    {
        err("validate: cannot enumerate %s", packageDirectory.c_str());
        return false;
    }
    std::sort(actual.begin(), actual.end());
    if (const auto preview = std::find(actual.begin(), actual.end(), kPreviewFile);
        preview != actual.end())
    {
        if (!validPreview(std::filesystem::path(packageDirectory) / kPreviewFile))
        {
            err("validate: preview.rgba is not a valid MWRI image");
            return false;
        }
        actual.erase(preview);
    }
    if (actual != expectedFiles(map))
    {
        err("validate: output must contain exactly five map fastfiles and map.json "
            "(and optionally preview.rgba)");
        return false;
    }

    const auto zones = zoneNames(map);
    std::array<ZoneValidation, 5> validation{};
    bool valid = true;
    for (size_t index = 0; index < zones.size(); ++index)
    {
        valid = validateZoneFile(path_join(packageDirectory, zones[index] + ".ff"),
                                 &validation[index]) &&
                valid;
    }
    if (valid)
    {
        const auto &main = validation[0].typeCounts;
        const auto &server = validation[1].typeCounts;
        const auto &techsets = validation[4].typeCounts;
        if (main[ASSET_TYPE_GFX_MAP] != 1 || main[ASSET_TYPE_GLASS_MAP] != 1 ||
            main[ASSET_TYPE_IMPACT_FX] != 1 || main[ASSET_TYPE_COL_MAP] != 1 ||
            main[ASSET_TYPE_COM_MAP] != 1 || main[ASSET_TYPE_MAP_ENTS] != 1 ||
            main[ASSET_TYPE_NET_CONST_STRINGS] != replayncs::Count ||
            std::any_of(server.begin(), server.end(), [](const auto count) { return count; }))
        {
            err("validate: package is missing a required native Replay map asset");
            valid = false;
        }
        if (!techsets[ASSET_TYPE_MATERIAL] ||
            main[ASSET_TYPE_MATERIAL] != techsets[ASSET_TYPE_MATERIAL] ||
            !techsets[ASSET_TYPE_TECHSET] ||
            !techsets[ASSET_TYPE_VERTEXSHADER] || !techsets[ASSET_TYPE_PIXELSHADER] ||
            techsets[ASSET_TYPE_IMAGE] || main[ASSET_TYPE_TECHSET] ||
            main[ASSET_TYPE_VERTEXSHADER] || main[ASSET_TYPE_PIXELSHADER])
        {
            err("validate: map render definitions must reside in the techset zone and images in the main zone");
            valid = false;
        }
        for (size_t index = 2; index < 4; ++index)
        {
            if (validation[index].assetCount)
            {
                err("validate: generated localization companions must be empty");
                valid = false;
                break;
            }
        }
    }
    if (valid)
    {
        info("validate: map output '%s' passes structural checks", map.c_str());
    }
    return valid;
}

const char *defaultEntities =
    "{ 212 \"worldspawn\" }\n"
    "{ 212 \"info_player_start\" 709 \"0 0 64\" 80 \"0 0 0\" }\n"
    "{ 212 \"mp_tdm_spawn\" 709 \"0 0 64\" 80 \"0 0 0\" }\n"
    "{ 212 \"mp_tdm_spawn_allies_start\" 709 \"-64 0 64\" 80 \"0 0 0\" }\n"
    "{ 212 \"mp_tdm_spawn_axis_start\" 709 \"64 0 64\" 80 \"0 180 0\" }\n"
    "{ 212 \"script_model\" 709 \"0 0 16\" 80 \"0 0 0\" }\n";

struct AssetTally
{
    int materials{};
    int images{};
    int models{};
    int surfaces{};
    int failures{};
    std::string firstFailure;
    bool hasCompass{};
};

AssetTally addMapAssets(iw8::ZoneWriter &writer, iw8::ZoneWriter &techsets,
                        const std::string &dumpDirectory,
                        const dumpsrc::DumpSource &source, const std::string &map,
                        const unsigned compassRotation)
{
    AssetTally tally;
    for (const std::string &relativePath : source.listMaterials())
    {
        const std::string path = path_join(path_join(dumpDirectory, "materials"), relativePath);
        std::string materialName;
        if (convdump::mtl::emitMaterialFromDump(techsets, path, relativePath, &materialName))
        {
            replayrender::RegisterMaterialReference(writer, materialName);
            ++tally.materials;
        }
        else
        {
            ++tally.failures;
            if (tally.firstFailure.empty())
                tally.firstFailure = "material " + path;
        }
    }

    const std::string compassName = "compass_map_" + map;
    bool hasCompass = false;
    for (const std::string &name : dumpimg::listImageDumps(dumpDirectory))
    {
        const dumpimg::ImageDumpFile input = dumpimg::readImageDump(dumpDirectory, name);
        const unsigned rotation = input.name == compassName ? compassRotation : 0;
        if (!input.loaded || !dumpimg::addImageAsset(writer, dumpimg::convertImage(input, rotation)))
        {
            ++tally.failures;
            if (tally.firstFailure.empty())
                tally.firstFailure = "image " + name + " in " + dumpDirectory;
            continue;
        }
        ++tally.images;
        hasCompass = hasCompass || input.name == compassName;
    }
    if (hasCompass)
    {
        tally.hasCompass = true;
        if (!convdump::mtl::writeCompassMaterial(techsets, compassName))
        {
            ++tally.failures;
            if (tally.firstFailure.empty())
                tally.firstFailure = "compass material " + compassName;
        }
        else
        {
            replayrender::RegisterMaterialReference(writer, compassName);
        }
    }
    else
    {
        warn("assets: no images/%s.dds, .iwi, or .ffImg; HUD compass will use the default material",
             compassName.c_str());
    }

    std::vector<std::string> surfaceFiles;
    if (list_dir(path_join(dumpDirectory, "XSurface"), surfaceFiles))
    {
        for (const std::string &file : surfaceFiles)
        {
            if (!file.ends_with(".xse"))
            {
                continue;
            }
            const std::string name = file.substr(0, file.size() - 4);
            if (iw8xs_dump::writeXModelSurfsFromDump(writer, dumpDirectory, name))
            {
                ++tally.surfaces;
            }
            else
            {
                ++tally.failures;
                if (tally.firstFailure.empty())
                    tally.firstFailure = "XModelSurfs " + name + " in " + dumpDirectory;
            }
        }
    }

    // XModel bodies use packed references, so their XModelSurfs dependencies must precede them.
    for (const std::string &name : source.listXModels())
    {
        if (iw8::addXModelFromDump(writer, dumpDirectory, name))
        {
            ++tally.models;
        }
        else
        {
            ++tally.failures;
            if (tally.firstFailure.empty())
                tally.firstFailure = "XModel " + name + " in " + dumpDirectory;
        }
    }

    info("assets: %d materials, %d images, %d models, %d model surfaces, %d failures",
         tally.materials, tally.images, tally.models, tally.surfaces, tally.failures);
    return tally;
}

template <typename Buffer> Iw8WriteParams writeParams(const Buffer &buffer)
{
    Iw8WriteParams params;
    for (int index = 0; index < iw8::IW8_MAX_XFILE_COUNT; ++index)
    {
        params.blockSize[index] = buffer.blockSize(index);
        params.totalDecompressed += params.blockSize[index];
    }
    params.calcSize = buffer.calcSize();
    return params;
}

template <> Iw8WriteParams writeParams(const iw8::ZoneBuffer &buffer)
{
    Iw8WriteParams params;
    for (int index = 0; index < iw8::IW8_MAX_XFILE_COUNT; ++index)
    {
        params.blockSize[index] = buffer.streamSize(index);
        params.totalDecompressed += params.blockSize[index];
    }
    params.calcSize = buffer.calcSize();
    return params;
}

iw8::MapSun loadLighting(const Args &args, const std::string &dumpDirectory,
                         const std::string &assetName)
{
    if (args.lightingProfile != "source" && args.lightingProfile != "aniyah-incursion")
    {
        throw std::runtime_error("unknown lighting profile: " + args.lightingProfile);
    }

    iw8::MapSun lighting;
    if (args.lightingProfile == "source")
    {
        const std::string path = path_join(dumpDirectory, assetName + ".lighting.json");
        if (!file_exists(path))
        {
            throw std::runtime_error("source lighting profile needs .lighting.json");
        }

        std::ifstream input(path);
        const nlohmann::json data = nlohmann::json::parse(input);
        if (data.at("schema") != 1)
        {
            throw std::runtime_error("invalid map lighting schema");
        }
        lighting.intensity = data.at("intensity").get<float>();

        float directionLength = 0.0f;
        float upLength = 0.0f;
        float dot = 0.0f;
        for (size_t index = 0; index < 3; ++index)
        {
            lighting.color[index] = data.at("color").at(index).get<float>();
            lighting.direction[index] = data.at("direction").at(index).get<float>();
            lighting.up[index] = data.at("up").at(index).get<float>();
            if (!std::isfinite(lighting.color[index]) || lighting.color[index] < 0.0f)
            {
                throw std::runtime_error("invalid sun color");
            }
            directionLength += lighting.direction[index] * lighting.direction[index];
            upLength += lighting.up[index] * lighting.up[index];
            dot += lighting.direction[index] * lighting.up[index];
        }
        if (!std::isfinite(lighting.intensity) || lighting.intensity < 0.0f ||
            !std::isfinite(directionLength + upLength + dot) ||
            std::abs(directionLength - 1.0f) > 0.001f ||
            (upLength != 0.0f && std::abs(upLength - 1.0f) > 0.001f) || std::abs(dot) > 0.001f)
        {
            throw std::runtime_error("invalid sun lighting data");
        }

        if (data.contains("primary_lights"))
        {
            const auto &sourceLights = data.at("primary_lights");
            if (!sourceLights.is_array() || sourceLights.size() < 2 || sourceLights.size() > 65535)
            {
                throw std::runtime_error("invalid source primary-light array");
            }
            lighting.sunPrimaryLightIndex = data.at("sun_primary_light_index").get<uint32_t>();
            if (lighting.sunPrimaryLightIndex >= sourceLights.size())
            {
                throw std::runtime_error("invalid source sun primary-light index");
            }

            const auto readVector = [](const nlohmann::json &value, const char *name) {
                if (!value.is_array() || value.size() != 3)
                    throw std::runtime_error(std::string("invalid primary-light ") + name);
                std::array<float, 3> result{};
                for (size_t component = 0; component < result.size(); ++component)
                {
                    result[component] = value.at(component).get<float>();
                    if (!std::isfinite(result[component]))
                        throw std::runtime_error(std::string("invalid primary-light ") + name);
                }
                return result;
            };

            lighting.primaryLights.reserve(sourceLights.size());
            for (const auto &source : sourceLights)
            {
                iw8::MapSun::PrimaryLight light;
                const unsigned type = source.at("type").get<unsigned>();
                const unsigned exponent = source.at("exponent").get<unsigned>();
                if (type > 3 || exponent > 255)
                    throw std::runtime_error("invalid IW3 primary-light type or exponent");
                light.type = static_cast<uint8_t>(type);
                light.exponent = static_cast<uint8_t>(exponent);
                light.canUseShadowMap = source.at("can_use_shadow_map").get<bool>();

                const auto color = readVector(source.at("color"), "color");
                const auto direction = readVector(source.at("direction"), "direction");
                const auto origin = readVector(source.at("origin"), "origin");
                if (std::ranges::any_of(color, [](const float value) { return value < 0.0f; }))
                    throw std::runtime_error("invalid primary-light color");
                std::copy(color.begin(), color.end(), light.color);
                std::copy(origin.begin(), origin.end(), light.origin);

                const float sourceDirectionLength =
                    std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                              direction[2] * direction[2]);
                if (type != 0 && sourceDirectionLength <= 0.000001f)
                    throw std::runtime_error("primary-light direction has zero length");
                if (sourceDirectionLength > 0.000001f)
                    for (size_t component = 0; component < direction.size(); ++component)
                        light.direction[component] = direction[component] / sourceDirectionLength;

                if (type >= 2)
                {
                    const std::array<float, 3> reference =
                        std::abs(light.direction[2]) < 0.999f
                            ? std::array<float, 3>{0.0f, 0.0f, 1.0f}
                            : std::array<float, 3>{0.0f, 1.0f, 0.0f};
                    const float projection = reference[0] * light.direction[0] +
                                             reference[1] * light.direction[1] +
                                             reference[2] * light.direction[2];
                    float sourceUpLength = 0.0f;
                    for (size_t component = 0; component < reference.size(); ++component)
                    {
                        light.up[component] =
                            reference[component] - projection * light.direction[component];
                        sourceUpLength += light.up[component] * light.up[component];
                    }
                    sourceUpLength = std::sqrt(sourceUpLength);
                    for (float &component : light.up)
                        component /= sourceUpLength;
                }

                light.radius = source.at("radius").get<float>();
                light.cosHalfFovOuter = source.at("cos_half_fov_outer").get<float>();
                light.cosHalfFovInner = source.at("cos_half_fov_inner").get<float>();
                light.rotationLimit = source.at("rotation_limit").get<float>();
                light.translationLimit = source.at("translation_limit").get<float>();
                light.definition = source.at("definition").get<std::string>();
                if (!std::isfinite(light.radius) || light.radius < 0.0f ||
                    !std::isfinite(light.cosHalfFovOuter) ||
                    !std::isfinite(light.cosHalfFovInner) || !std::isfinite(light.rotationLimit) ||
                    !std::isfinite(light.translationLimit) || light.definition.size() > 255)
                {
                    throw std::runtime_error("invalid primary-light scalar data");
                }
                if (type == 2 && (light.cosHalfFovOuter <= 0.0f ||
                                  light.cosHalfFovOuter >= light.cosHalfFovInner ||
                                  light.cosHalfFovInner > 1.0f))
                {
                    throw std::runtime_error("invalid IW3 spot-light field of view");
                }
                lighting.primaryLights.push_back(std::move(light));
            }
        }
    }

    if (lighting.primaryLights.empty())
        lighting.primaryLights.resize(2);

    lighting.intensity *= args.sunIntensityScale;
    if (!std::isfinite(lighting.intensity))
    {
        throw std::runtime_error("scaled sun intensity is not finite");
    }
    info("lighting: %s, scale %.6f, native intensity %.6f", args.lightingProfile.c_str(),
         args.sunIntensityScale, lighting.intensity);
    return lighting;
}

iw8::havok::BakeResult loadCollision(const Args &args, const std::string &dumpDirectory,
                                     const std::string &assetName)
{
    if (!args.collisionPath.empty())
    {
        if (args.replayExecutable.empty())
        {
            throw std::runtime_error("--replay is required with --collision");
        }
        return iw8::havok::BakeCollision(
            {args.replayExecutable, args.collisionPath, args.footstepsPath});
    }

    iw8::havok::BakeResult collision;
    const std::string path = path_join(dumpDirectory, assetName + ".havok");
    if (!read_file(path, collision.world) || collision.world.size() < 16 ||
        collision.world.size() > 256 * 1024 * 1024 ||
        std::memcmp(collision.world.data() + 4, "TAG0", 4) != 0)
    {
        throw std::runtime_error("native collision is missing; pass --replay and --collision");
    }
    return collision;
}

int writeMapPackage(const Args &args, const std::string &map, const std::string &outputDirectory,
                    const std::string &entities, const iw8::MapBounds &bounds,
                    const std::string &dumpDirectory, const MapMetadata &metadata,
                    const std::vector<convert::TriggerModelSource> &triggers,
                    const iw3::PreparedMap *prepared,
                    const convert::CompassBounds *compassBounds)
{
    if (!prepareOutputDirectory(outputDirectory, map))
    {
        return 2;
    }

    LapTimer timing("package");
    const std::string assetName = "maps/mp/" + map + ".d3dbsp";
    const iw8::MapSun lighting = loadLighting(args, dumpDirectory, assetName);
    const std::string renderPath = path_join(dumpDirectory, assetName + ".render.json");
    if (!file_exists(renderPath))
    {
        throw std::runtime_error("native render data is missing: " + renderPath);
    }
    const auto renderMesh = replayrender::Load(renderPath);
    timing.lap("render load");
    // Use the final ComWorld sun vector, including any requested profile.
    // Replay rejects compressed shadows whose stored direction differs from
    // the active sun. Moving entities and breakable panes are not baked.
    const auto sunShadow = prepared
        ? replaysunshadow::Bake(prepared->shadowScene,
                                {lighting.direction[0], lighting.direction[1], lighting.direction[2]},
                                renderMesh.sceneBounds)
        : replaysunshadow::Data{};
    if (!sunShadow.bytes.empty())
        info("sun-shadow: baked %u cells at %u resolution, %zu native compressed bytes",
             sunShadow.parameters.forestSize, sunShadow.parameters.resolution,
             sunShadow.bytes.size());
    timing.lap("sun shadow bake");
    const auto glassInitCount = static_cast<uint32_t>(renderMesh.glassPanes.size());
    iw8::havok::PhysicsAsset glassPhysics;
    if (glassInitCount)
    {
        if (args.replayExecutable.empty())
            throw std::runtime_error("native glass physics requires --replay");
        iw8::havok::PrepareCollisionBaker(args.replayExecutable);
        iw8::havok::PhysicsMesh dummy;
        dummy.contents = 45440; // shipped Replay glasschunkdummydefault bodyContents
        dummy.vertices = {{{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f},
                           {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
                           {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
                           {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}}};
        dummy.triangles = {{{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                            {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                            {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}}};
        glassPhysics = iw8::havok::BakePhysicsAsset(
            iw8maps::kGlassPhysicsName, dummy, 7, 9);
    }
    const auto glassPieceLimit = glassInitCount ? glassInitCount + 512u : 0u;
    iw8::navigation::Result navigation;
    if (prepared && args.navigation)
    {
        navigation = iw8::navigation::Build(prepared->navigation);
        timing.lap("bot navigation");
    }
    // Replay spawns each fixed pathnode from a node_* MapEnts record, in order.
    std::string mapEntities = entities;
    if (navigation)
    {
        uint32_t classnameKey = 0;
        uint32_t originKey = 0;
        if (!convert::iw3KeyToIw8KeyId("classname", classnameKey) ||
            !convert::iw3KeyToIw8KeyId("origin", originKey))
            throw std::runtime_error("Replay entity key table lacks classname or origin");
        const auto origins = iw8::navigation::FixedPathNodes(navigation);
        for (const auto &origin : origins)
        {
            char line[192];
            std::snprintf(line, sizeof(line), "{ %u \"node_pathnode\" %u \"%.3f %.3f %.3f\" }\n",
                          classnameKey, originKey, origin[0], origin[1], origin[2]);
            mapEntities += line;
        }
        info("navigation: appended %zu node_pathnode MapEnts records", origins.size());
    }
    int result = 0;
    auto collision = loadCollision(args, dumpDirectory, assetName);
    timing.lap("collision bake");
    {
        if (collision.models.empty())
        {
            iw8::havok::CollisionModel worldModel;
            if (bounds.valid)
            {
                std::copy_n(bounds.mn, 3, worldModel.minimum.begin());
                std::copy_n(bounds.mx, 3, worldModel.maximum.begin());
            }
            collision.models.push_back(worldModel);
        }
        collision.triggers.reserve(triggers.size());
        for (const auto &trigger : triggers)
        {
            if (trigger.collisionModel >= collision.models.size())
                throw std::runtime_error("IW3 trigger references a missing collision model");
            if (collision.models[trigger.collisionModel].shapeIndex ==
                std::numeric_limits<uint16_t>::max())
                throw std::runtime_error("IW3 trigger collision model has no native Havok shape");
            collision.triggers.push_back({trigger.collisionModel, trigger.staticPhysics});
        }
        info("collision: serialized native world %zu bytes, entities %zu bytes, %zu brush models",
             collision.world.size(), collision.entities.size(), collision.models.size());
    }

    {
        iw8::ZoneWriter writer;
        iw8::ZoneWriter techsets;
        const auto dynamicCounts = prepared ? iw8::CountDynamicEntities(prepared->dynamicEntities)
                                            : iw8::DynamicEntityCounts{};
        static const std::vector<iw8::staticmodelcollision::Model> noStaticCollisionModels;
        const auto worldStrings = iw8::internMapWorldStrings(
            writer, mapEntities, lighting,
            prepared ? prepared->staticModelCollisionModels : noStaticCollisionModels);
        replayrender::RegisterMapMaterials(writer, techsets, renderMesh);
        replayrender::RegisterReflectionProbeImage(writer, renderMesh);
        if (glassInitCount)
            iw8::writePhysicsAsset(writer, glassPhysics);
        // Models and effects this zone registers; only these go into the level
        // NetConstStrings (stock names already sit in the common tables).
        std::set<std::string> zoneModels, zoneEffects, scriptModels, scriptEffects;
        if (prepared)
        {
            for (const auto &model : prepared->xmodels)
                zoneModels.insert(model.model.name);
            for (const auto &effect : prepared->vfxEffects)
                zoneEffects.insert(effect.name);
        }
        if (prepared)
        {
            if (!prepared->gradingClutName.empty())
                iw8::grading::RegisterGradingClut(writer, prepared->gradingClutName,
                                                  iw3::GradingClutSize,
                                                  prepared->gradingClutPixels);
            for (const auto &rawFile : prepared->rawFiles)
                iw8::rawfile::Register(writer, rawFile.name, rawFile.data);
            if (prepared->soundBank)
                iw8::sound::RegisterBank(writer, *prepared->soundBank);
            for (const auto &model : prepared->xmodels)
            {
                if (!model.physicsAsset.name.empty())
                {
                    iw8::writePhysicsAsset(writer, model.physicsAsset);
                }
                for (const auto &lod : model.lods)
                    iw8xs_dump::writeXModelSurfs(writer, lod.name, lod);
                iw8::writeXModel(writer, model.model);
            }
            if (!prepared->vfxLightDef.empty())
                iw8::vfx::RegisterLightDef(writer, prepared->vfxLightDef);
            for (auto &effect : prepared->vfxEffects)
                iw8::vfx::Register(writer, std::move(effect));
            iw8::RegisterDynamicEntityList(writer, prepared->dynamicEntities);
        }
        iw8::impact::Register(writer, map,
                              prepared ? prepared->smallGlassEffect : std::string{},
                              prepared ? prepared->impactOverrides
                                       : iw8::impact::EffectOverrides{});
        if (!renderMesh.glassPanes.empty())
        {
            writer.add(ASSET_TYPE_FX_MAP, assetName,
                       [assetName, &renderMesh](iw8::ZoneWriter &output) {
                           iw8maps::emitFxMapBody(output, assetName.c_str(), renderMesh);
                       });
        }
        const auto primaryLightCount = static_cast<uint32_t>(lighting.primaryLights.size());
        const auto sunPrimaryLightIndex = lighting.sunPrimaryLightIndex;
        const replayrender::StaticModels staticModels =
            prepared ? prepared->staticModels : replayrender::StaticModels{};
        writer.add(ASSET_TYPE_GFX_MAP, assetName,
                   [assetName, renderPath, &renderMesh, primaryLightCount,
                    sunPrimaryLightIndex, staticModels, dynamicCounts,
                    &sunShadow](iw8::ZoneWriter &output) {
                       iw8maps::emitGfxMapBody(output, assetName.c_str(), renderPath,
                                               renderMesh, primaryLightCount, sunPrimaryLightIndex,
                                               staticModels, dynamicCounts.models,
                                               dynamicCounts.brushes, sunShadow);
                   });
        writer.add(ASSET_TYPE_GLASS_MAP, assetName,
                   [assetName, &renderMesh](iw8::ZoneWriter &output) {
                       iw8maps::emitGlassMapBody(output, assetName.c_str(), renderMesh);
                   });
        iw8::navigation::Register(writer, assetName, navigation);
        const dumpsrc::DumpSource source(dumpDirectory, map);
        const AssetTally assets =
            addMapAssets(writer, techsets, dumpDirectory, source, map, metadata.compassRotation);
        if (assets.failures)
            throw std::runtime_error("failed to import " + std::to_string(assets.failures) +
                                     " map assets; first: " + assets.firstFailure);
        std::string gscTool = args.gscTool;
        if (gscTool.empty())
            if (const char *value = std::getenv("IW8_GSC_TOOL"); value && *value)
                gscTool = value;
        bool scriptCompiled = false;
        if (prepared && !prepared->mapScript.empty() && !gscTool.empty())
        {
            // Converted IW3 map scripts plus the IW3 compatibility layer, with
            // the minimap call generated from the squared compass bounds.
            // setminimap( material, x1, y1, x2, y2, 1, material ) is the
            // argument order of the proven hand-assembled startup bytecode.
            std::string generated = "iw3compat_minimap()\n{\n";
            if (assets.hasCompass && compassBounds)
            {
                char line[320];
                std::snprintf(line, sizeof(line),
                              "    setminimap( \"compass_map_%s\", %.3f, %.3f, %.3f, %.3f, 1, "
                              "\"compass_map_%s\" );\n",
                              map.c_str(), compassBounds->northwestX, compassBounds->northwestY,
                              compassBounds->southeastX, compassBounds->southeastY, map.c_str());
                generated += line;
            }
            generated += "}\n";
            const std::string scriptSource = prepared->mapScript + "\n" + generated;
            // Names the script resolves at runtime: precachemodel/setmodel through the
            // model table, loadfx through the effect table.
            static const std::regex scriptModel(R"re((?:precachemodel|setmodel)\s*\(\s*"([^"]+)")re",
                                                std::regex::icase);
            static const std::regex scriptEffect(R"re(loadfx\s*\(\s*"([^"]+)")re", std::regex::icase);
            for (auto match = std::sregex_iterator(scriptSource.begin(), scriptSource.end(),
                                                   scriptModel);
                 match != std::sregex_iterator(); ++match)
                scriptModels.insert((*match)[1].str());
            for (auto match = std::sregex_iterator(scriptSource.begin(), scriptSource.end(),
                                                   scriptEffect);
                 match != std::sregex_iterator(); ++match)
                scriptEffects.insert((*match)[1].str());
            const std::filesystem::path work =
                std::filesystem::path(path_dir(outputDirectory)) / ".iw3-scripts" / map;
            try
            {
                const auto gscbin = iw3::scripts::Compile(scriptSource, gscTool, work, map);
                const std::string scriptName = "scripts/mp/maps/" + map + "/" + map + ".gsc";
                writer.add(ASSET_TYPE_SCRIPTFILE, scriptName,
                           [scriptName, gscbin](iw8::ZoneWriter &output) {
                               iw8::replay_script::emitCompiledScript(output, scriptName, gscbin);
                           });
                scriptCompiled = true;
                info("scripts: compiled converted map script (%zu bytes gscbin, source %s)",
                     gscbin.size(), (work / (map + ".gsc")).string().c_str());
            }
            catch (const std::exception &error)
            {
                warn("scripts: converted map script did not compile, using the minimap-only "
                     "startup script: %s", error.what());
            }
        }
        else if (prepared && !prepared->mapScript.empty())
            warn("scripts: pass --gsc-tool (or set IW8_GSC_TOOL) to compile the converted IW3 map "
                 "scripts; using the minimap-only startup script");
        if (!scriptCompiled && assets.hasCompass && compassBounds)
        {
            const std::string scriptName = "scripts/mp/maps/" + map + "/" + map + ".gsc";
            const convert::CompassBounds compass = *compassBounds;
            writer.add(ASSET_TYPE_SCRIPTFILE, scriptName,
                       [scriptName, map, compass](iw8::ZoneWriter &output) {
                           iw8::replay_script::emitCompassStartup(
                               output, scriptName, map, compass.northwestX,
                               compass.northwestY, compass.southeastX, compass.southeastY);
                       });
        }
        else if (!scriptCompiled && assets.hasCompass)
        {
            warn("assets: compass image has no valid pair of IW3 minimap_corner entities");
        }
        // Map entity models (MapEnts key 652 = "model"; '*' values are brush models).
        iw8::LevelNetConstStrings levelStrings;
        {
            static const std::regex entityModel(R"re((?:^|[\s{])652 "([^"*][^"]*)")re");
            std::set<std::string> models;
            for (auto match = std::sregex_iterator(mapEntities.begin(), mapEntities.end(),
                                                   entityModel);
                 match != std::sregex_iterator(); ++match)
                if (zoneModels.contains((*match)[1].str()))
                    models.insert((*match)[1].str());
            for (const auto &model : scriptModels)
                if (zoneModels.contains(model))
                    models.insert(model);
            std::set<std::string> effects;
            for (const auto &effect : scriptEffects)
                if (zoneEffects.contains(effect))
                    effects.insert(effect);
            levelStrings[0].assign(models.begin(), models.end());  // "mdl"
            levelStrings[6].assign(effects.begin(), effects.end()); // "vfx"
            info("assets: level NetConstStrings: %zu model(s), %zu effect(s)", models.size(),
                 effects.size());
        }
        iw8::registerMapWorldAssets(
            writer, worldStrings, assetName, mapEntities, bounds, lighting, collision,
            glassInitCount, glassPieceLimit, dynamicCounts.models, dynamicCounts.brushes,
            prepared && prepared->vfxLightDef == "light_point_linear", levelStrings);
        timing.lap("asset registration");
        techsets.build();
        writer.build();
        timing.lap("zone build");

        if (!iw8_write(path_join(outputDirectory, "techsets_" + map + ".ff"),
                       techsets.body(), writeParams(techsets)))
        {
            result = 1;
        }

        if (!iw8_write(path_join(outputDirectory, map + ".ff"), writer.body(), writeParams(writer)))
        {
            result = 1;
        }
        timing.lap("zone write");
        info("build-map: main zone contains %zu assets", writer.assetCount());
        info("build-map: techset zone contains %zu assets", techsets.assetCount());
    }

    const auto writeEmpty = [&](const std::string &name) {
        iw8::ZoneBuffer buffer;
        iw8::buildEmptyZone(buffer);
        if (!iw8_write(path_join(outputDirectory, name), buffer.data(), writeParams(buffer)))
        {
            result = 1;
        }
    };
    // Shipped srv_<map> zones hold only server extras; the world lives in the main zone.
    writeEmpty("srv_" + map + ".ff");
    writeEmpty("eng_" + map + ".ff");
    writeEmpty("ww_" + map + ".ff");

    if (result == 0 && !writeMetadata(outputDirectory, metadata))
    {
        result = 1;
    }
    if (const auto preview = std::filesystem::path(dumpDirectory) / kPreviewFile;
        result == 0 && std::filesystem::is_regular_file(preview))
    {
        std::error_code copyError;
        std::filesystem::copy_file(preview, std::filesystem::path(outputDirectory) / kPreviewFile,
                                   std::filesystem::copy_options::overwrite_existing, copyError);
        if (copyError)
        {
            err("build-map: cannot copy preview.rgba: %s", copyError.message().c_str());
            result = 1;
        }
    }

    if (result == 0 && !validatePackage(outputDirectory, map))
    {
        result = 1;
    }
    timing.lap("metadata and validation");
    if (result == 0)
    {
        info("build-map: wrote map output to %s", outputDirectory.c_str());
    }
    return result;
}

int buildMap(const Args &args, const iw3::PreparedMap *prepared = nullptr)
{
    if (args.positional.size() < 2)
    {
        printUsage();
        return 2;
    }

    const std::string dumpDirectory = args.positional[0];
    const std::string map = args.positional[1];
    if (!requireMapId(map))
    {
        return 2;
    }

    // The Replay Havok image needs its preferred address. Reserve it before
    // render/lighting preparation in standalone build-map as build-iw3 does.
    // Otherwise a map with native collision can fail late at BakeCollision.
    if (!args.replayExecutable.empty())
        iw8::havok::PrepareCollisionBaker(args.replayExecutable);

    MapMetadata metadata;
    if (!readMetadata(args.metadataPath, metadata))
    {
        return 2;
    }
    if (!metadata.id.empty() && metadata.id != map)
    {
        err("metadata: id '%s' does not match map '%s'", metadata.id.c_str(), map.c_str());
        return 2;
    }
    metadata.present = true;
    if (metadata.id.empty())
        metadata.id = map;
    if (metadata.title.empty())
        metadata.title = map;
    const std::string outputDirectory = args.outputDirectory.empty()
                                            ? path_join(dumpDirectory, map + "_out")
                                            : args.outputDirectory;

    const dumpsrc::DumpSource source(dumpDirectory, map);
    if (!source.hasMapFiles())
    {
        err("build-map: no maps/mp/%s.d3dbsp.* files under %s", map.c_str(), dumpDirectory.c_str());
        return 1;
    }

    std::string entities;
    std::vector<convert::TriggerModelSource> triggers;
    bool foundEntities = false;
    for (const std::string &path : {path_join(dumpDirectory, map + "_iw8_ents.txt"),
                                    path_join(path_dir(dumpDirectory), map + "_iw8_ents.txt")})
    {
        if (file_exists(path) && read_file_str(path, entities) && !entities.empty())
        {
            info("entities: using %s", path.c_str());
            foundEntities = true;
            break;
        }
    }

    const dumpsrc::EntsDump sourceEntities = source.loadEnts();
    convert::CompassBounds compassBounds;
    const bool hasCompassBounds = sourceEntities.loaded && !sourceEntities.text.empty() &&
                                  convert::iw3CompassBounds(sourceEntities.text, compassBounds);
    if (hasCompassBounds)
    {
        info("compass: northwest %.3f %.3f, southeast %.3f %.3f", compassBounds.northwestX,
             compassBounds.northwestY, compassBounds.southeastX, compassBounds.southeastY);
        // IW3 compass images are square, uniformly scaled maps. IW3 stretched
        // them over the minimap_corner rectangle, but Replay's map keeps the
        // world aspect, so a non-square corner pair squashes the image (Nuketown:
        // 6136 x 2904 units). Square the region around its center.
        const float spanX = std::abs(compassBounds.southeastX - compassBounds.northwestX);
        const float spanY = std::abs(compassBounds.northwestY - compassBounds.southeastY);
        if (std::abs(spanX - spanY) > 1.0f)
        {
            const float half = std::max(spanX, spanY) * 0.5f;
            const float centerX = (compassBounds.northwestX + compassBounds.southeastX) * 0.5f;
            const float centerY = (compassBounds.northwestY + compassBounds.southeastY) * 0.5f;
            const float signX = compassBounds.southeastX >= compassBounds.northwestX ? 1.0f : -1.0f;
            const float signY = compassBounds.northwestY >= compassBounds.southeastY ? 1.0f : -1.0f;
            compassBounds.northwestX = centerX - signX * half;
            compassBounds.southeastX = centerX + signX * half;
            compassBounds.northwestY = centerY + signY * half;
            compassBounds.southeastY = centerY - signY * half;
            info("compass: squared minimap region to northwest %.3f %.3f, southeast %.3f %.3f",
                 compassBounds.northwestX, compassBounds.northwestY, compassBounds.southeastX,
                 compassBounds.southeastY);
        }
        // Replay draws a converted IW3 minimap too large about the map centre:
        // the player's position on it drifts toward the centre with distance.
        // Calibrated on Nuketown (fixes-25): a player located from the scene at
        // (1206, 630) was shown on the art for (1072, 545), i.e. at 0.86-0.89 of
        // the true distance from the centre. Contracting the declared region by
        // that factor puts the art under the player again; the centre (where
        // the minimap already lined up) and the orientation are unchanged.
        if (metadata.minimapScale != 1.0f)
        {
            const float centerX = (compassBounds.northwestX + compassBounds.southeastX) * 0.5f;
            const float centerY = (compassBounds.northwestY + compassBounds.southeastY) * 0.5f;
            const float scale = metadata.minimapScale;
            compassBounds.northwestX = centerX + (compassBounds.northwestX - centerX) * scale;
            compassBounds.southeastX = centerX + (compassBounds.southeastX - centerX) * scale;
            compassBounds.northwestY = centerY + (compassBounds.northwestY - centerY) * scale;
            compassBounds.southeastY = centerY + (compassBounds.southeastY - centerY) * scale;
            info("compass: scaled minimap region by %.3f to northwest %.3f %.3f, southeast %.3f %.3f",
                 scale, compassBounds.northwestX, compassBounds.northwestY,
                 compassBounds.southeastX, compassBounds.southeastY);
        }
        // IW3 draws the compass image with northyaw up; Replay's minimap wants
        // it turned clockwise by northyaw (Office, default 90, was verified at 90).
        if (!metadata.compassRotationSet)
        {
            const float yaw = std::fmod(std::fmod(compassBounds.northYaw, 360.0f) + 360.0f, 360.0f);
            const float quarter = std::round(yaw / 90.0f);
            if (std::abs(yaw - quarter * 90.0f) < 0.01f)
            {
                metadata.compassRotation = static_cast<unsigned>(quarter * 90.0f) % 360u;
                info("compass: rotating the IW3 minimap %u degrees for northyaw %.1f",
                     metadata.compassRotation, compassBounds.northYaw);
            }
            else
                warn("compass: northyaw %.3f is not a quarter turn; minimap left unrotated",
                     compassBounds.northYaw);
        }
    }
    if (!foundEntities && sourceEntities.loaded && !sourceEntities.text.empty())
    {
        const size_t converted =
            convert::iw3ToIw8EntityString(sourceEntities.text, entities, &triggers);
        if (converted == 0)
        {
            err("build-map: could not convert any source entities");
            return 1;
        }
        info("entities: converted %zu source entities", converted);
        foundEntities = true;
    }
    if (!foundEntities)
    {
        entities = defaultEntities;
        warn("entities: using the built-in Replay TDM spawn set");
    }
    convert::validateIw8EntityString(entities);

    iw8::MapBounds bounds;
    const dumpsrc::ClipMapDump clipMap = source.loadClipMap();
    if (clipMap.loaded && clipMap.boundsFromVerts)
    {
        for (size_t index = 0; index < 3; ++index)
        {
            bounds.mn[index] = clipMap.boundsMin[index];
            bounds.mx[index] = clipMap.boundsMax[index];
        }
        bounds.valid = true;
    }

    const std::string boundsPath =
        path_join(dumpDirectory, "maps/mp/" + map + ".d3dbsp.bounds.json");
    if (file_exists(boundsPath))
    {
        std::ifstream file(boundsPath);
        const nlohmann::json data = nlohmann::json::parse(file);
        if (data.at("schema") != 1 || data.at("min").size() != 3 || data.at("max").size() != 3)
        {
            throw std::runtime_error("invalid map bounds schema");
        }
        for (size_t index = 0; index < 3; ++index)
        {
            bounds.mn[index] = data.at("min").at(index).get<float>();
            bounds.mx[index] = data.at("max").at(index).get<float>();
            if (!std::isfinite(bounds.mn[index]) || !std::isfinite(bounds.mx[index]) ||
                bounds.mn[index] >= bounds.mx[index] || std::abs(bounds.mn[index]) > 100100.0f ||
                std::abs(bounds.mx[index]) > 100100.0f)
            {
                throw std::runtime_error("invalid map bounds");
            }
        }
        bounds.valid = true;
    }

    const dumpsrc::ComWorldDump commonWorld = source.loadComWorld();
    if (commonWorld.loaded)
    {
        info("common world: %d primary lights", commonWorld.primaryLightCount);
    }

    // Build the complete family beside the requested output, then publish the
    // directory only after all five zones and map.json pass validation.
    const std::filesystem::path target =
        std::filesystem::absolute(outputDirectory).lexically_normal();
    const std::filesystem::path parent = target.parent_path();
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error)
    {
        err("build-map: cannot create output parent %s: %s", parent.string().c_str(),
            error.message().c_str());
        return 1;
    }
    const bool initialTarget = std::filesystem::exists(target, error);
    if (error ||
        (initialTarget &&
         (!prepareOutputDirectory(target.string(), map) ||
          (GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))))
    {
        err("build-map: output directory is unavailable or contains unexpected files: %s",
            target.string().c_str());
        return 1;
    }

    const std::string tag = std::to_string(GetCurrentProcessId()) + "-" +
                            std::to_string(GetTickCount64());
    const std::filesystem::path stage =
        parent / ("." + map + ".stage-" + tag);
    const std::filesystem::path backup =
        parent / ("." + map + ".backup-" + tag);
    const bool backupExists = std::filesystem::exists(backup, error);
    if (error || stage.parent_path() != parent || backup.parent_path() != parent ||
        backupExists || !std::filesystem::create_directory(stage, error) || error)
    {
        err("build-map: could not create a private output stage beside %s",
            target.string().c_str());
        return 1;
    }
    const auto cleanKnownFiles = [&](const std::filesystem::path &directory) {
        if (directory.parent_path() != parent ||
            (GetFileAttributesW(directory.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
            return;
        std::error_code cleanupError;
        for (const std::string &name : expectedFiles(map))
        {
            std::filesystem::remove(directory / name, cleanupError);
            cleanupError.clear();
            std::filesystem::remove(directory / (name + ".partial"), cleanupError);
            cleanupError.clear();
        }
        std::filesystem::remove(directory, cleanupError);
        if (cleanupError)
            warn("build-map: could not clean %s: %s", directory.string().c_str(),
                 cleanupError.message().c_str());
    };

    int result = 1;
    try
    {
        result = writeMapPackage(args, map, stage.string(), entities, bounds,
                                 dumpDirectory, metadata, triggers, prepared,
                                 hasCompassBounds ? &compassBounds : nullptr);
    }
    catch (...)
    {
        cleanKnownFiles(stage);
        throw;
    }
    if (result)
    {
        cleanKnownFiles(stage);
        return result;
    }
    const bool hadTarget = std::filesystem::exists(target, error);
    if (error ||
        (hadTarget &&
         ((GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) ||
          !prepareOutputDirectory(target.string(), map))))
    {
        cleanKnownFiles(stage);
        err("build-map: output changed while conversion was running");
        return 1;
    }
    if (hadTarget)
    {
        std::filesystem::rename(target, backup, error);
        if (error)
        {
            cleanKnownFiles(stage);
            err("build-map: could not preserve previous output: %s",
                error.message().c_str());
            return 1;
        }
    }
    std::filesystem::rename(stage, target, error);
    if (error)
    {
        const std::string publishError = error.message();
        std::string recovery = "the validated stage remains at " + stage.string();
        if (hadTarget)
        {
            error.clear();
            std::filesystem::rename(backup, target, error);
            recovery += error ? "; previous output remains at " + backup.string()
                              : "; previous output was restored";
        }
        err("build-map: publish failed (%s); %s", publishError.c_str(), recovery.c_str());
        return 1;
    }
    if (hadTarget)
        cleanKnownFiles(backup);
    info("build-map: published validated map output to %s", target.string().c_str());
    return 0;
}

int buildIw3(const Args &args)
{
    if (args.positional.empty() || args.positional.size() > 2 || args.replayExecutable.empty())
    {
        printUsage();
        return 2;
    }

    const bool hasExplicitMap = args.positional.size() == 2;
    std::string map = hasExplicitMap ? args.positional[1]
                                     : std::filesystem::path(args.positional[0]).stem().string();
    MapMetadata metadata;
    if (!readMetadata(args.metadataPath, metadata))
    {
        return 2;
    }
    if (!metadata.id.empty())
    {
        if (hasExplicitMap && metadata.id != map)
        {
            err("metadata: id '%s' does not match map '%s'", metadata.id.c_str(), map.c_str());
            return 2;
        }
        map = metadata.id;
    }
    if (!requireMapId(map))
    {
        return 2;
    }

    // Direct conversion produces a complete installable package. Always emit
    // the one supported loose configuration file, even when the caller does
    // not provide a custom title or description.
    metadata.present = true;
    if (metadata.id.empty())
    {
        metadata.id = map;
    }
    if (metadata.title.empty())
    {
        metadata.title = map;
    }

    iw8::havok::PrepareCollisionBaker(args.replayExecutable);

    iw3::ImportOptions options;
    options.fastfile = args.positional[0];
    options.map = map;
    options.unlinker = args.unlinkerExecutable;
    options.iw3ModeAssets = args.iw3ModeAssets;
    const auto requestedOutput = args.outputDirectory.empty()
                                     ? std::filesystem::path(args.positional[0]).parent_path() /
                                           (map + "_iw8")
                                     : std::filesystem::path(args.outputDirectory);
    options.scratchRoot = std::filesystem::absolute(requestedOutput).parent_path();
    for (const std::string &path : args.searchPaths)
    {
        options.searchPaths.emplace_back(path);
    }
    // The fastfile directory is always a valid source root. A user may set
    // IW3_GAME_ROOT or COD4_ROOT once for the matching main/raw installation;
    // explicit --search-path values remain repeatable for custom asset packs.
    options.searchPaths.emplace_back(options.fastfile.parent_path());
    for (const char *name : {"IW3_GAME_ROOT", "COD4_ROOT"})
    {
        if (const char *value = std::getenv(name); value && *value)
        {
            const std::filesystem::path root(value);
            options.searchPaths.push_back(root);
            options.searchPaths.push_back(root / "main");
            options.searchPaths.push_back(root / "raw");
        }
    }
    for (const std::string &value : args.iw3Roots)
    {
        const std::filesystem::path root(value);
        options.searchPaths.push_back(root);
        options.searchPaths.push_back(root / "main");
        options.searchPaths.push_back(root / "raw");
    }
    iw3::PreparedMap prepared = [&] {
        StageTimer timer("iw3 conversion");
        return iw3::PrepareFastfile(options);
    }();

    Args build = args;
    build.command = "build-map";
    build.positional = {prepared.root.string(), map};
    build.collisionPath = prepared.collision.string();
    build.footstepsPath = prepared.footsteps.string();
    if (!writeMetadata(prepared.root.string(), metadata))
    {
        return 1;
    }
    build.metadataPath = (prepared.root / "map.json").string();
    if (build.outputDirectory.empty())
    {
        build.outputDirectory =
            (std::filesystem::path(args.positional[0]).parent_path() / (map + "_iw8")).string();
    }
    return buildMap(build, &prepared);
}

int translateIw3Script(const Args &args)
{
    if (args.positional.size() != 2 || args.outputDirectory.empty())
    {
        printUsage();
        return 2;
    }
    const std::filesystem::path directory = args.positional[0];
    const std::string map = args.positional[1];
    std::vector<iw3::scripts::Source> sources;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".gsc")
            continue;
        std::string text;
        if (!read_file_str(entry.path().string(), text))
            return 1;
        std::string name = entry.path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        sources.push_back({"maps/mp/" + name, std::move(text)});
    }
    const auto translation = iw3::scripts::Translate(
        map, sources, [](const std::string &) { return std::string{}; },
        "iw3compat_generated()\n{\n    level.mw120r_models = [];\n}\n");
    for (const auto &note : translation.notes)
        info("translate: %s", note.c_str());
    // Standalone translation has no converted effects: stock stand-ins only.
    const std::string source = translation.gsc + "\niw3compat_minimap()\n{\n}\n" +
                               "\niw3compat_iw3fx()\n{\n    level.mw120r_iw3fx = [];\n}\n";
    std::filesystem::create_directories(args.outputDirectory);
    if (!write_file_str(path_join(args.outputDirectory, map + ".gsc"), source))
        return 1;
    if (!args.gscTool.empty())
    {
        const auto gscbin = iw3::scripts::Compile(source, args.gscTool, args.outputDirectory, map);
        info("translate: %s compiled to %zu bytes", map.c_str(), gscbin.size());
    }
    return 0;
}

int inspect(const Args &args)
{
    if (args.positional.empty())
    {
        printUsage();
        return 2;
    }
    return inspect_ff(args.positional[0]) ? 0 : 1;
}

int inspectIw7(const Args &args)
{
    if (args.positional.empty())
    {
        printUsage();
        return 2;
    }

    iw7::FastfileInfo fastfile;
    std::string failure;
    if (!iw7::inspectFastfile(args.positional[0], fastfile, failure))
    {
        err("inspect-iw7: %s", failure.c_str());
        return 1;
    }

    info("inspect-iw7: version %u, %s", fastfile.version,
         fastfile.signedFile ? "signed" : "unsigned");
    info("inspect-iw7: %u script strings, %u assets, %llu decompressed bytes",
         fastfile.scriptStringCount, fastfile.assetCount,
         static_cast<unsigned long long>(fastfile.uncompressedSize));
    info("inspect-iw7: %u shared streams, %u image streams", fastfile.sharedStreamCount,
         fastfile.imageStreamCount);

    const std::vector<std::string> packages = iw7::requiredPackages(fastfile);
    if (packages.empty())
    {
        info("inspect-iw7: no external image packages");
    }
    else
    {
        std::string list;
        for (const std::string &package : packages)
        {
            if (!list.empty())
            {
                list += ", ";
            }
            list += package;
        }
        info("inspect-iw7: external packages: %s", list.c_str());
    }
    return 0;
}

int validate(const Args &args)
{
    if (args.positional.size() < 2)
    {
        printUsage();
        return 2;
    }
    return validatePackage(args.positional[0], args.positional[1]) ? 0 : 1;
}

} // namespace

int main(const int argc, char **argv)
try
{
    if (argc > 1 && !std::strcmp(argv[1], "build-weapon"))
        return iw8::weaponMain(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "build-weapon-placeholders"))
    {
        if (argc != 3 && argc != 5)
        {
            std::fprintf(stderr, "usage: iw8-zonetool build-weapon-placeholders <manifest.json> [-o <output>]\n");
            return 2;
        }
        if (argc == 5 && std::strcmp(argv[3], "-o"))
        {
            std::fprintf(stderr, "expected -o before output directory\n");
            return 2;
        }
        return iw8::buildWeaponPlaceholders(argv[2], argc == 5 ? argv[4] : "") ? 0 : 1;
    }
    if (argc == 4 && !std::strcmp(argv[1], "build-scene-assets"))
        return iw8::buildSceneAssets(argv[2], argv[3]);
    Args args;
    if (!parseArgs(argc, argv, args))
    {
        printUsage();
        return 2;
    }
    if (args.command == "--help" || args.command == "-h" || args.command == "-?")
    {
        printUsage();
        return 0;
    }
    if (args.command == "build-map")
    {
        return buildMap(args);
    }
    if (args.command == "translate-iw3-script")
    {
        return translateIw3Script(args);
    }
    if (args.command == "build-weapon")
    {
        if (args.positional.size() != 1) { printUsage(); return 2; }
        return iw8::buildWeapon(args.positional[0], args.outputDirectory) ? 0 : 1;
    }
    if (args.command == "build-iw3")
    {
        return buildIw3(args);
    }
    if (args.command == "inspect")
    {
        return inspect(args);
    }
    if (args.command == "inspect-iw7")
    {
        return inspectIw7(args);
    }
    if (args.command == "validate-output" || args.command == "validate-package")
    {
        return validate(args);
    }
    err("unknown command '%s'", args.command.c_str());
    printUsage();
    return 2;
}
catch (const std::exception &error)
{
    err("conversion failed: %s", error.what());
    return 1;
}
