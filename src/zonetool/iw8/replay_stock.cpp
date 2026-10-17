#include "replay_stock.h"

#include "../../common/json.hpp"
#include "../../common/log.h"
#include "../convert/conv_xsurface.h"
#include "../dumpsrc/xmodel_dump.h"
#include "iw8_focus_structs.h"
#include "replay_render.h"
#include "replay_vfx.h"
#include "write_xsurface.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iw8::stock
{
namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Key = std::pair<IW8_XAssetType, std::string>;

// The shipped map the copies come from; every stock MP map carries the same
// `void` table and CODCaster images, Shipment is the smallest.
constexpr const char *kSourceMap = "mp_shipment";
constexpr const char *kTable = "void";
// Section 19: the CODCaster spectator images, in the language packs stock uses.
const std::vector<std::string> kEnglishImages{"codcaster_playerlist_dead_icon"};
const std::vector<std::string> kWorldwideImages{"codcaster_team_default_6",
                                                "ui_operator_east_yegor_portrait"};
// Loaded with common, so their assets resolve name-only references: stock map
// zones reference worldwide-common effects with a comma.
const std::vector<std::string> kWorldwideCommon{"ww_common", "ww_common_mp", "ww_common_base_mp",
                                                "ww_common_core_mp"};
// Part of every export's cache key: bump when the export arguments change.
constexpr const char *kExportFormat = "stock-export-1";

std::string Hex16(const uint64_t value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(value));
    return text;
}

std::vector<uint8_t> Unhex(const std::string &text)
{
    if (text.size() % 2)
        throw std::runtime_error("stock: odd-length hex field");
    const auto nibble = [](const char c) -> uint8_t {
        if (c >= '0' && c <= '9')
            return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F')
            return static_cast<uint8_t>(c - 'A' + 10);
        throw std::runtime_error("stock: invalid hex digit");
    };
    std::vector<uint8_t> bytes(text.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(nibble(text[2 * index]) << 4 | nibble(text[2 * index + 1]));
    return bytes;
}

// The exact loaded bytes of a struct node (ACTS_NATIVE_BYTES=1), sized to T.
std::vector<uint8_t> Native(const Json &node, const std::size_t size, const char *what)
{
    if (!node.is_object() || !node.contains("$native"))
        throw std::runtime_error(std::string("stock: ") + what +
                                 " has no native bytes (ACTS needs ACTS_NATIVE_BYTES=1)");
    auto bytes = Unhex(node.at("$native").get<std::string>());
    if (bytes.size() != size)
        throw std::runtime_error(std::string("stock: ") + what + " native size " +
                                 std::to_string(bytes.size()) + " is not " + std::to_string(size));
    return bytes;
}

template <typename T> T NativeAs(const Json &node, const char *what)
{
    const auto bytes = Native(node, sizeof(T), what);
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

uint64_t Address(const Json &node)
{
    return std::stoull(node.at("address").get<std::string>(), nullptr, 16);
}

void Store(void *record, const std::size_t offset, const uint64_t value)
{
    std::memcpy(static_cast<uint8_t *>(record) + offset, &value, sizeof(value));
}

uint64_t Load(const void *record, const std::size_t offset)
{
    uint64_t value{};
    std::memcpy(&value, static_cast<const uint8_t *>(record) + offset, sizeof(value));
    return value;
}

// Every runtime address an exported asset names; a copied record must hold
// none of them once its pointer slots are restamped.
void CollectAddresses(const Json &node, std::unordered_set<uint64_t> &addresses)
{
    if (node.is_object())
    {
        if (const auto found = node.find("address"); found != node.end() && found->is_string())
            if (const uint64_t address = std::stoull(found->get<std::string>(), nullptr, 16))
                addresses.insert(address);
        for (const auto &item : node.items())
            CollectAddresses(item.value(), addresses);
    }
    else if (node.is_array())
        for (const auto &item : node)
            CollectAddresses(item, addresses);
}

void RequireNoAddresses(const void *record, const std::size_t size,
                        const std::unordered_set<uint64_t> &addresses, const std::string &what)
{
    for (std::size_t offset = 0; offset + 8 <= size; offset += 8)
        if (const uint64_t value = Load(record, offset); value && addresses.contains(value))
            throw std::runtime_error("stock: " + what + " keeps a runtime pointer at +0x" +
                                     Hex16(offset) + " that the copier does not restamp");
}

// The 8-aligned slot of `address` in a record, searched from `from`.
std::size_t SlotOf(const std::vector<uint8_t> &record, const uint64_t address, const std::size_t from,
                   const std::string &what)
{
    for (std::size_t offset = (from + 7) & ~std::size_t{7}; offset + 8 <= record.size(); offset += 8)
        if (Load(record.data(), offset) == address)
            return offset;
    throw std::runtime_error("stock: pointer slot not found in " + what);
}

// Pointer nodes below a struct node (an object with "address"), in member
// order, skipping union views and the raw union bytes.
void FindPointers(const Json &node, std::vector<const Json *> &pointers)
{
    if (node.is_object())
    {
        if (node.contains("address"))
        {
            pointers.push_back(&node);
            return;
        }
        for (const auto &item : node.items())
        {
            const auto &key = item.key();
            if (key.starts_with('$') || key.starts_with("__") || key == "bytes")
                continue;
            FindPointers(item.value(), pointers);
        }
    }
    else if (node.is_array())
        for (const auto &item : node)
            FindPointers(item, pointers);
}

IW8_XAssetType AssetType(const std::string &schemaType)
{
    static const std::unordered_map<std::string, IW8_XAssetType> types{
        {"Material", ASSET_TYPE_MATERIAL},
        {"XModel", ASSET_TYPE_XMODEL},
        {"XModelSurfs", ASSET_TYPE_XMODELSURFS},
        {"ParticleSystemDef", ASSET_TYPE_VFX},
        {"PhysicsFXPipeline", ASSET_TYPE_PHYSICSFXPIPELINE},
        {"PhysicsFXShape", ASSET_TYPE_PHYSICSFXSHAPE},
        {"PhysicsAsset", ASSET_TYPE_PHYSICSASSET},
        {"VectorField", ASSET_TYPE_VECTORFIELD},
        {"GfxLightDef", ASSET_TYPE_LIGHTDEF},
    };
    const auto found = types.find(schemaType);
    if (found == types.end())
        throw std::runtime_error("stock: unsupported referenced asset type " + schemaType);
    return found->second;
}

// The ACTS pool name of the asset types the copier exports; nullptr otherwise.
const char *Pool(const IW8_XAssetType type)
{
    switch (type)
    {
    case ASSET_TYPE_VFX:
        return "vfx";
    case ASSET_TYPE_PHYSICSFXSHAPE:
        return "physicsfxshape";
    case ASSET_TYPE_LIGHTDEF:
        return "lightdef";
    case ASSET_TYPE_XMODEL:
        return "xmodel";
    case ASSET_TYPE_XMODELSURFS:
        return "xmodelsurfs";
    case ASSET_TYPE_SURFACE_FX:
        return "surfacefx";
    case ASSET_TYPE_IMAGE:
        return "image";
    default:
        return nullptr;
    }
}

// An asset reference read from a shipped record: a leading comma means the
// shipped zone does not own it (a common or worldwide-common zone does).
struct Reference
{
    IW8_XAssetType type{};
    std::string name;
    bool shared{};
};

Reference ReadReference(const Json &node)
{
    auto name = node.at("name").get<std::string>();
    const bool shared = name.starts_with(',');
    if (shared)
        name.erase(0, 1);
    if (name.empty())
        throw std::runtime_error("stock: empty asset reference");
    return {AssetType(node.at("type").get<std::string>()), name, shared};
}

// ---- ACTS exports ----------------------------------------------------------

std::wstring Quote(const std::wstring &value)
{
    std::wstring quoted = L"\"";
    for (const wchar_t c : value)
    {
        if (c == L'"')
            quoted += L'\\';
        quoted += c;
    }
    return quoted + L"\"";
}

std::wstring Widen(const std::string &text)
{
    return std::wstring(text.begin(), text.end());
}

uint64_t Fnv(const std::string &text)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : text)
        hash = (hash ^ c) * 1099511628211ull;
    return hash;
}

// Exported assets of one zone: (pool, name) -> asset JSON file.
struct Export
{
    std::string zone;
    fs::path root;
    std::map<std::pair<std::string, std::string>, fs::path> files;
    std::vector<std::string> scriptStrings;

    const fs::path *find(const IW8_XAssetType type, const std::string &name) const
    {
        const char *pool = Pool(type);
        if (!pool)
            return nullptr;
        const auto found = files.find({pool, name});
        return found == files.end() ? nullptr : &found->second;
    }
};

Json ReadJson(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("stock: cannot read " + path.string());
    return Json::parse(input);
}

// ACTS export of `types` (optionally only `names`) from one zone, cached by the
// zone file's identity and the request. The game's own loaders read the zone,
// so the export is the shipped data as Replay loads it.
Export RunExport(const Options &options, const fs::path &zone, const std::string &types,
                 std::vector<std::string> names)
{
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::error_code error;
    const auto size = fs::file_size(zone, error);
    if (error)
        throw std::runtime_error("stock: missing stock zone " + zone.string());
    const auto stamp = fs::last_write_time(zone, error).time_since_epoch().count();
    std::string request = std::string(kExportFormat) + "|" + zone.filename().string() + "|" +
                          std::to_string(size) + "|" + std::to_string(stamp) + "|" + types;
    for (const auto &name : names)
        request += "|" + name;
    const std::string zoneName = zone.stem().string();
    const fs::path directory =
        options.cacheDirectory / (zoneName + "-" + Hex16(Fnv(request)).substr(0, 12));
    const fs::path marker = directory / "complete.txt";
    const fs::path root = directory / "mw19replay" / zoneName;

    if (!fs::is_regular_file(marker, error))
    {
        fs::remove_all(directory, error);
        fs::create_directories(directory);
        std::vector<std::wstring> arguments{
            options.acts.wstring(), L"--noUpdater", L"fastfile", L"-r", L"mw19replay", L"-g",
            fs::absolute(options.replayExecutable).wstring()};
        const fs::path oodle = options.replayExecutable.parent_path() / "oo2core_7_win64.dll";
        if (fs::is_regular_file(oodle, error))
        {
            arguments.push_back(L"--oodle");
            arguments.push_back(fs::absolute(oodle).wstring());
        }
        arguments.push_back(L"-a");
        arguments.push_back(Widen(types));
        if (!names.empty())
        {
            std::string joined;
            for (const auto &name : names)
                joined += (joined.empty() ? "" : ",") + name;
            arguments.push_back(L"-n");
            arguments.push_back(Widen(joined));
        }
        arguments.push_back(L"-l");
        arguments.push_back(L"-o");
        arguments.push_back(fs::absolute(directory).wstring());
        arguments.push_back(fs::absolute(zone).wstring());
        std::wstring commandLine;
        for (const auto &argument : arguments)
            commandLine += (commandLine.empty() ? L"" : L" ") + Quote(argument);

        const fs::path logPath = directory / "acts.log";
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE)
            throw std::runtime_error("stock: cannot create " + logPath.string());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        // Exact loaded struct bytes, and structured (not binary payload)
        // exports for model surfaces and images.
        SetEnvironmentVariableW(L"ACTS_NATIVE_BYTES", L"1");
        SetEnvironmentVariableW(L"ACTS_REPLAY_STRUCTURED", L"1");
        const BOOL started = CreateProcessW(options.acts.c_str(), commandLine.data(), nullptr,
                                            nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                            &startup, &process);
        SetEnvironmentVariableW(L"ACTS_NATIVE_BYTES", nullptr);
        SetEnvironmentVariableW(L"ACTS_REPLAY_STRUCTURED", nullptr);
        CloseHandle(log);
        if (!started)
            throw std::runtime_error("stock: cannot start ACTS " + options.acts.string());
        const DWORD wait = WaitForSingleObject(process.hProcess, 15u * 60u * 1000u);
        DWORD exitCode = 1;
        if (wait == WAIT_TIMEOUT)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
        }
        else if (wait == WAIT_OBJECT_0)
            GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (wait != WAIT_OBJECT_0 || exitCode != 0 || !fs::is_regular_file(root / "assets.jsonl", error))
            throw std::runtime_error("stock: ACTS export of " + zone.string() + " (" + types +
                                     ") failed; see " + logPath.string());
        std::ofstream(marker) << request << '\n';
        zt::info("stock: exported %s (%s%s) with ACTS", zoneName.c_str(), types.c_str(),
                 names.empty() ? "" : (", " + std::to_string(names.size()) + " named").c_str());
    }

    Export result;
    result.zone = zoneName;
    result.root = root;
    std::ifstream journal(root / "assets.jsonl", std::ios::binary);
    for (std::string line; std::getline(journal, line);)
    {
        if (line.empty())
            continue;
        const auto record = Json::parse(line);
        if (record.value("status", std::string{}) != "ok" || !record.contains("file"))
            continue;
        result.files[{record.at("type").get<std::string>(), record.at("name").get<std::string>()}] =
            root / fs::path(record.at("file").get<std::string>());
    }
    if (fs::is_regular_file(root / "script_strings.json", error))
    {
        const auto strings = ReadJson(root / "script_strings.json");
        for (const auto &text : strings)
            result.scriptStrings.push_back(text.is_string() ? text.get<std::string>() : std::string{});
    }
    for (const auto &name : names)
    {
        bool found = false;
        for (const auto &[key, file] : result.files)
            found = found || key.second == name;
        if (!found && types != "xmodel")
            throw std::runtime_error("stock: ACTS export of " + zoneName + " has no '" + name + "'");
    }
    return result;
}

Json Fields(const fs::path &file)
{
    auto document = ReadJson(file);
    if (!document.contains("asset") || !document.at("asset").contains("fields"))
        throw std::runtime_error("stock: " + file.string() + " is not an ACTS asset export");
    if (document.at("asset").value("read_errors", 0) || document.at("asset").value("unresolved_pointers", 0) ||
        document.at("asset").value("unresolved_unions", 0) || document.at("asset").value("external_payloads", 0))
        throw std::runtime_error("stock: " + file.string() + " has unresolved exported data");
    return std::move(document.at("asset").at("fields"));
}

// ---- parsed copies ---------------------------------------------------------

struct Shape
{
    std::string name;
    std::array<uint8_t, 0x30> native{};
    std::vector<uint8_t> havok;
    std::optional<std::string> sound;
};

struct LightDef
{
    std::string name;
    std::array<uint8_t, iw8sz::LIGHTDEF> native{};
};

struct Model
{
    convert::xmodel::Iw8XModelRecord record;
    std::vector<conv_xsurf::Iw8Surfs> lods;
};

struct Table
{
    std::string name;
    std::array<uint8_t, 0x28> native{};
    std::vector<std::array<std::string, 65>> entries;
    uint32_t poolSize{};
};

struct Image
{
    std::string name;
    std::array<uint8_t, iw8sz::IMAGE> native{};
    std::vector<uint8_t> pixels;
};

iw8_focus::ParticleCurveControlPointDef Point(const Json &node)
{
    return NativeAs<iw8_focus::ParticleCurveControlPointDef>(node, "curve point");
}

// One shipped ParticleSystemDef as the generic imported layout: every record
// keeps its shipped bytes and only pointer slots are rewritten by the writer.
vfx::Effect ParseEffect(const Json &fields, std::vector<Reference> &references)
{
    std::unordered_set<uint64_t> addresses;
    CollectAddresses(fields, addresses);

    vfx::Effect effect;
    effect.name = fields.at("name").at("string").get<std::string>();
    const std::string what = "effect " + effect.name;
    effect.native = NativeAs<iw8_focus::ParticleSystemDef>(fields, what.c_str());
    if (fields.at("numScriptedInputNodes").get<int>() || !fields.at("scriptedInputNodeDefs").is_null())
        throw std::runtime_error("stock: " + what + " has scripted inputs, which the writer omits");
    {
        auto check = effect.native;
        for (const std::size_t offset : {0x00, 0x08, 0x10})
            Store(&check, offset, 0);
        RequireNoAddresses(&check, sizeof(check), addresses, what);
    }

    const auto &emitters = fields.at("emitterDefs");
    if (!emitters.is_null())
        for (const auto &emitterNode : emitters.at("values"))
        {
            vfx::Emitter emitter;
            emitter.native = NativeAs<iw8_focus::ParticleEmitterDef>(emitterNode, "emitter");
            const auto &fade = emitterNode.at("fadeCurveDef");
            if (!fade.at("controlPoints").is_null())
                for (const auto &point : fade.at("controlPoints").at("values"))
                    emitter.fadeCurve.push_back(Point(point));
            if (static_cast<int>(emitter.fadeCurve.size()) != emitter.native.fadeCurveDef.numControlPoints)
                throw std::runtime_error("stock: " + what + " fade curve count mismatch");
            auto check = emitter.native;
            Store(&check, 0x00, 0);
            Store(&check, offsetof(iw8_focus::ParticleEmitterDef, fadeCurveDef), 0);
            RequireNoAddresses(&check, sizeof(check), addresses, what + " emitter");

            const auto &states = emitterNode.at("stateDefs");
            if (!states.is_null())
                for (const auto &stateNode : states.at("values"))
                {
                    vfx::State state;
                    state.native = NativeAs<iw8_focus::ParticleStateDef>(stateNode, "state");
                    auto stateCheck = state.native;
                    Store(&stateCheck, 0x00, 0);
                    RequireNoAddresses(&stateCheck, sizeof(stateCheck), addresses, what + " state");
                    const auto &groups = stateNode.at("moduleGroupDefs").at("values");
                    if (groups.size() != state.groups.size())
                        throw std::runtime_error("stock: " + what + " state does not have three module groups");
                    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex)
                    {
                        const auto group =
                            NativeAs<iw8_focus::ParticleModuleGroupDef>(groups[groupIndex], "module group");
                        if (group.disabled)
                            throw std::runtime_error("stock: " + what + " has a disabled module group");
                        const auto &modules = groups[groupIndex].at("moduleDefs");
                        if (modules.is_null())
                            continue;
                        for (const auto &moduleNode : modules.at("values"))
                        {
                            vfx::Module module;
                            module.imported = true;
                            auto record = Native(moduleNode, sizeof(iw8_focus::ParticleModuleDef), "module");
                            const std::string moduleWhat =
                                what + " module " + std::to_string(Load(record.data(), 0) & 0xFFFF);
                            std::vector<const Json *> pointers;
                            FindPointers(moduleNode.at("moduleData").at("value"), pointers);
                            std::set<uint64_t> seen;
                            for (const Json *pointer : pointers)
                            {
                                const uint64_t address = Address(*pointer);
                                if (!address || !seen.insert(address).second)
                                    continue;
                                const auto type = pointer->value("type", std::string{});
                                if (!pointer->contains("stride"))
                                    throw std::runtime_error("stock: " + moduleWhat +
                                                             " has an unsupported pointer (" + type + ")");
                                vfx::Module::ImportedChild child;
                                child.offset = SlotOf(record, address, 0x10, moduleWhat);
                                static const Json noValues = Json::array();
                                const Json &values = pointer->contains("values") ? pointer->at("values") : noValues;
                                if (values.size() != pointer->at("count").get<std::size_t>())
                                    throw std::runtime_error("stock: " + moduleWhat + " array count mismatch");
                                if (type == "ParticleCurveControlPointDef")
                                {
                                    child.curve = true;
                                    for (const auto &point : values)
                                        child.points.push_back(Point(point));
                                    if (static_cast<int32_t>(Load(record.data(), child.offset + 8)) !=
                                        static_cast<int32_t>(child.points.size()))
                                        throw std::runtime_error("stock: " + moduleWhat + " curve count mismatch");
                                }
                                else if (type == "ParticleLinkedAssetDef")
                                {
                                    for (const auto &entryNode : values)
                                    {
                                        vfx::Module::LinkedEntry entry;
                                        auto bytes = Unhex(entryNode.at("bytes").get<std::string>());
                                        if (bytes.size() != entry.native.size())
                                            throw std::runtime_error("stock: " + moduleWhat + " linked asset size");
                                        std::vector<const Json *> slots;
                                        FindPointers(entryNode.at("value"), slots);
                                        std::size_t next = 0;
                                        for (const Json *slot : slots)
                                        {
                                            const uint64_t target = Address(*slot);
                                            if (!target)
                                                continue;
                                            const std::size_t offset = SlotOf(bytes, target, next, moduleWhat);
                                            next = offset + 8;
                                            if (slot->value("asset_reference", false))
                                            {
                                                auto reference = ReadReference(*slot);
                                                entry.assets.push_back({offset, {reference.type, reference.name}});
                                                references.push_back(std::move(reference));
                                            }
                                            else if (slot->contains("string"))
                                                entry.strings.push_back({offset, slot->at("string").get<std::string>()});
                                            else
                                                throw std::runtime_error("stock: " + moduleWhat +
                                                                         " linked asset has an unsupported pointer");
                                            Store(bytes.data(), offset, 0);
                                        }
                                        RequireNoAddresses(bytes.data(), bytes.size(), addresses,
                                                           moduleWhat + " linked asset");
                                        std::memcpy(entry.native.data(), bytes.data(), bytes.size());
                                        child.entries.push_back(std::move(entry));
                                    }
                                    if (static_cast<int32_t>(Load(record.data(), child.offset + 8)) !=
                                        static_cast<int32_t>(child.entries.size()))
                                        throw std::runtime_error("stock: " + moduleWhat + " linked asset count mismatch");
                                }
                                else
                                    throw std::runtime_error("stock: " + moduleWhat + " has an unsupported array (" +
                                                             type + ")");
                                Store(record.data(), child.offset, 0);
                                module.importedChildren.push_back(std::move(child));
                            }
                            std::sort(module.importedChildren.begin(), module.importedChildren.end(),
                                      [](const auto &a, const auto &b) { return a.offset < b.offset; });
                            RequireNoAddresses(record.data(), record.size(), addresses, moduleWhat);
                            std::memcpy(&module.native, record.data(), record.size());
                            state.groups[groupIndex].push_back(std::move(module));
                        }
                    }
                    emitter.states.push_back(std::move(state));
                }
            effect.emitters.push_back(std::move(emitter));
        }
    return effect;
}

Shape ParseShape(const Json &fields)
{
    std::unordered_set<uint64_t> addresses;
    CollectAddresses(fields, addresses);
    Shape shape;
    shape.name = fields.at("name").at("string").get<std::string>();
    const auto native = Native(fields, shape.native.size(), "physics shape");
    std::memcpy(shape.native.data(), native.data(), native.size());
    if (const auto &havok = fields.at("havokData"); !havok.is_null())
        shape.havok = Unhex(havok.at("bytes").get<std::string>());
    if (static_cast<uint32_t>(Load(shape.native.data(), 8)) != shape.havok.size())
        throw std::runtime_error("stock: physics shape " + shape.name + " havok size mismatch");
    if (const auto &sound = fields.at("soundAlias"); !sound.is_null())
        shape.sound = sound.at("string").get<std::string>();
    for (const std::size_t offset : {0x00, 0x10, 0x18})
        Store(shape.native.data(), offset, 0);
    RequireNoAddresses(shape.native.data(), shape.native.size(), addresses, "physics shape " + shape.name);
    return shape;
}

LightDef ParseLightDef(const Json &fields)
{
    std::unordered_set<uint64_t> addresses;
    CollectAddresses(fields, addresses);
    LightDef light;
    light.name = fields.at("name").at("string").get<std::string>();
    if (!fields.at("iesProfile").is_null())
        throw std::runtime_error("stock: light " + light.name + " has an IES profile the copier does not carry");
    const auto native = Native(fields, light.native.size(), "light");
    std::memcpy(light.native.data(), native.data(), native.size());
    Store(light.native.data(), 0, 0);
    RequireNoAddresses(light.native.data(), light.native.size(), addresses, "light " + light.name);
    return light;
}

// A stock model the XModel writer reproduces: what it cannot carry (detail
// collision, cloth, procedural bones ...) makes the model unsupported, and the
// caller then needs the model from a worldwide-common zone instead.
std::optional<convert::xmodel::Iw8XModelRecord> ParseModel(const Json &fields, const Export &source,
                                                           std::vector<Reference> &references,
                                                           std::string &unsupported)
{
    namespace cx = convert::xmodel;
    for (const char *pointer : {"scriptableMoverDef", "proceduralBones", "dynamicBones", "aimAssistBones",
                                "ikHingeAxis", "reactiveMotionInfo", "physicsFXShape", "detailCollision",
                                "clothAssets", "blendShapeInfo", "mdaoVolumes", "decalVolumesInfo"})
        if (!fields.at(pointer).is_null())
        {
            unsupported = pointer;
            return std::nullopt;
        }
    if (fields.at("numClientBones").get<int>() || fields.at("numAimAssistBones").get<int>() ||
        fields.at("numClothAssets").get<int>() || fields.at("mdaoVolumeCount").get<int>())
    {
        unsupported = "client, aim-assist, cloth or MDAO data";
        return std::nullopt;
    }
    const auto &physics = fields.at("physicsAsset");
    if (!physics.is_null() && !ReadReference(physics).shared)
    {
        unsupported = "a model-local physics asset";
        return std::nullopt;
    }
    // Fields the writer sets itself must already hold the writer's values.
    if (fields.at("characterCollBoundsType").get<int>() != 1 || fields.at("lgvData").get<uint32_t>() != 0x4008040u ||
        fields.at("physicsUseCategory").at("value").get<int>() != (physics.is_null() ? 0 : 1) ||
        fields.at("impactType").at("value").get<int>() || fields.at("mdaoType").at("value").get<int>() ||
        fields.at("edgeLength").get<float>() != 0.0f)
    {
        unsupported = "model scalars the writer does not carry";
        return std::nullopt;
    }
    for (const auto &bits : fields.at("noScalePartBits"))
        if (bits.get<uint32_t>())
        {
            unsupported = "no-scale part bits";
            return std::nullopt;
        }

    cx::Iw8XModelRecord record;
    record.name = fields.at("name").at("string").get<std::string>();
    record.numsurfs = fields.at("numsurfs").get<uint16_t>();
    record.numLods = fields.at("numLods").get<uint8_t>();
    record.collLod = fields.at("collLod").get<uint8_t>();
    record.numBones = fields.at("numBones").get<uint8_t>();
    record.numRootBones = fields.at("numRootBones").get<uint8_t>();
    record.shadowCutoffLod = fields.at("shadowCutoffLod").get<uint8_t>();
    record.flags = fields.at("flags").get<uint32_t>();
    record.contents = fields.at("contents").get<int32_t>();
    record.scale = fields.at("scale").get<float>();
    record.radius = fields.at("radius").get<float>();
    const auto bounds = Native(fields.at("bounds"), 24, "model bounds");
    std::memcpy(record.boundsMid, bounds.data(), 12);
    std::memcpy(record.boundsHalf, bounds.data() + 12, 12);

    if (const auto &names = fields.at("boneNames"); !names.is_null())
        for (const auto &bone : names.at("values"))
        {
            const auto index = bone.at("value").get<std::size_t>();
            if (index >= source.scriptStrings.size())
                throw std::runtime_error("stock: model " + record.name + " bone name is not a zone string");
            record.boneNames.push_back(source.scriptStrings[index]);
        }
    const auto bytesOf = [&](const char *field) {
        const auto &node = fields.at(field);
        return node.is_null() ? std::vector<uint8_t>{} : Unhex(node.at("bytes").get<std::string>());
    };
    record.skeleton.parentList = bytesOf("parentList");
    record.skeleton.partClassification = bytesOf("partClassification");
    const auto eachNative = [&](const char *field, auto &out) {
        const auto &node = fields.at(field);
        if (node.is_null())
            return;
        for (const auto &value : node.at("values"))
            out.push_back(NativeAs<typename std::decay_t<decltype(out)>::value_type>(value, field));
    };
    eachNative("quats", record.skeleton.quats);
    eachNative("trans", record.skeleton.trans);
    eachNative("baseMat", record.skeleton.baseMat);
    eachNative("boneInfo", record.skeleton.boneInfo);

    const auto &lods = fields.at("lodInfo");
    for (std::size_t index = 0; index < record.numLods; ++index)
    {
        const auto &lod = lods.at(index);
        cx::Iw8LodInfo info;
        info.dist = lod.at("dist").get<float>();
        info.numsurfs = lod.at("numsurfs").get<uint16_t>();
        info.surfIndex = lod.at("surfIndex").get<uint16_t>();
        const auto partBits = Native(lod.at("partBits"), 32, "LOD part bits");
        std::memcpy(info.partBits.data(), partBits.data(), 32);
        info.flags = lod.at("flags").at("value").get<uint8_t>();
        if (lod.at("subdivLodValidMask").get<int>())
        {
            unsupported = "subdivision LODs";
            return std::nullopt;
        }
        const auto surfs = ReadReference(lod.at("modelSurfsStaging"));
        if (surfs.shared)
            throw std::runtime_error("stock: model " + record.name + " LOD surfaces live in another zone");
        info.surfsName = surfs.name;
        record.lods.push_back(std::move(info));
    }
    if (const auto &materials = fields.at("materialHandles"); !materials.is_null())
        for (const auto &material : materials.at("values"))
        {
            auto reference = ReadReference(material);
            if (!reference.shared)
                throw std::runtime_error("stock: model " + record.name + " has a zone-local material");
            record.materials.push_back(reference.name);
            references.push_back(std::move(reference));
        }
    if (const auto &radii = fields.at("himipRadiusInvSq"); !radii.is_null())
        for (const auto &radius : radii.at("values"))
            record.himipRadiusInvSq.push_back(radius.get<float>());
    if (!physics.is_null())
    {
        auto reference = ReadReference(physics);
        record.physicsAssetName = reference.name;
        references.push_back(std::move(reference));
    }
    return record;
}

conv_xsurf::Iw8Surfs ParseSurfs(const Json &fields)
{
    conv_xsurf::Iw8Surfs surfs;
    surfs.name = fields.at("name").at("string").get<std::string>();
    const std::string what = "model surfaces " + surfs.name;
    const auto native = Native(fields, iw8sz::XMODELSURFS, what.c_str());
    for (std::size_t offset = 0x10; offset < 0x30; ++offset)
        if (native[offset])
            throw std::runtime_error("stock: " + what + " are streamed (xpak), not resident");
    std::memcpy(surfs.partBits, native.data() + 0x3C, sizeof(surfs.partBits));
    const auto &surfaces = fields.at("surfs").at("values");
    for (std::size_t index = 0; index < surfaces.size(); ++index)
    {
        const auto &surface = surfaces[index];
        for (const char *pointer : {"lmapCoords", "blendVerts", "subdiv"})
            if (surface.contains(pointer) && !surface.at(pointer).is_null())
                throw std::runtime_error("stock: " + what + " use " + pointer + ", which the copier does not carry");
        if (surface.at("blendShapeTargetCount").get<int>() || surface.at("subdivLevelCount").get<int>() ||
            surface.at("blendVertSize").get<int>())
            throw std::runtime_error("stock: " + what + " use blend shapes, subdivision or blend verts");
        for (const char *offset : {"sharedSecondUVDataOffset", "sharedNormalTransformDataOffset",
                                   "sharedTensionAccumTableOffset", "sharedTensionDataOffset"})
            if (surface.at(offset).get<uint32_t>() != UINT32_MAX)
                throw std::runtime_error("stock: " + what + " use " + offset);
        conv_xsurf::Iw8SurfaceCvt converted;
        converted.flags = surface.at("flags").get<uint16_t>();
        converted.vertCount = surface.at("vertCount").get<uint16_t>();
        converted.triCount = surface.at("triCount").get<uint16_t>();
        converted.sharedVertDataOffset = surface.at("sharedVertDataOffset").get<uint32_t>();
        converted.sharedIndexDataOffset = surface.at("sharedIndexDataOffset").get<uint32_t>();
        converted.sharedTriClusterDataOffset = surface.at("sharedTriClusterDataOffset").get<uint32_t>();
        converted.sharedColorDataOffset = surface.at("sharedColorDataOffset").get<uint32_t>();
        const auto &counts = surface.at("blendVertCounts");
        for (std::size_t count = 0; count < 8; ++count)
            converted.blendVertCounts[count] = counts.at(count).get<uint16_t>();
        if (const auto &rigid = surface.at("rigidVertLists"); !rigid.is_null())
            for (const auto &list : rigid.at("values"))
                converted.rigidVertLists.push_back(NativeAs<conv_xsurf::Iw8RigidVertList>(list, "rigid vertex list"));
        if (converted.rigidVertLists.size() != surface.at("rigidVertListCount").get<std::size_t>())
            throw std::runtime_error("stock: " + what + " rigid list count mismatch");
        const auto bounds = Native(surface.at("surfBounds"), 24, "surface bounds");
        std::memcpy(converted.boundsMid, bounds.data(), 12);
        std::memcpy(converted.boundsHalf, bounds.data() + 12, 12);
        const auto partBits = Native(surface.at("partBits"), 32, "surface part bits");
        std::memcpy(converted.partBits, partBits.data(), 32);
        // The resident geometry blob is shared by all surfaces of the asset.
        if (index == 0)
        {
            const auto &shared = surface.at("shared").at("values").at(0);
            const auto &data = shared.at("data");
            if (data.at("active_member").get<std::string>() != "residentData" || shared.at("flags").at("value").get<int>())
                throw std::runtime_error("stock: " + what + " geometry is not resident");
            surfs.sharedBlob = Unhex(data.at("value").at("bytes").get<std::string>());
            if (surfs.sharedBlob.size() != shared.at("dataSize").get<std::size_t>())
                throw std::runtime_error("stock: " + what + " geometry size mismatch");
        }
        surfs.totalVerts += converted.vertCount;
        surfs.totalTris += converted.triCount;
        surfs.totalTriClusters += (converted.triCount + 63u) / 64u;
        surfs.totalRigidRuns += static_cast<uint32_t>(converted.rigidVertLists.size());
        surfs.surfaces.push_back(std::move(converted));
    }
    surfs.ok = !surfs.surfaces.empty() && !surfs.sharedBlob.empty();
    return surfs;
}

Table ParseTable(const Json &fields, std::vector<Reference> &references)
{
    std::unordered_set<uint64_t> addresses;
    CollectAddresses(fields, addresses);
    Table table;
    table.name = fields.at("name").at("string").get<std::string>();
    const auto native = Native(fields, table.native.size(), "surface-fx table");
    std::memcpy(table.native.data(), native.data(), native.size());
    table.poolSize = fields.at("mapPoolBufferSize").get<uint32_t>();
    for (const std::size_t offset : {0x00, 0x08, 0x18})
        Store(table.native.data(), offset, 0);
    RequireNoAddresses(table.native.data(), table.native.size(), addresses, "surface-fx table");
    for (const auto &entryNode : fields.at("table").at("values"))
    {
        std::array<std::string, 65> entry;
        const auto read = [&](const Json &slot, std::string &name) {
            const auto &effect = slot.at("particleSystemDef");
            if (effect.is_null())
                return;
            auto reference = ReadReference(effect);
            if (reference.type != ASSET_TYPE_VFX)
                throw std::runtime_error("stock: surface-fx table entry is not an effect");
            name = reference.name;
            references.push_back(std::move(reference));
        };
        read(entryNode.at("baseEffect"), entry[0]);
        const auto &surfaces = entryNode.at("surfaceEffect");
        if (surfaces.size() != 64)
            throw std::runtime_error("stock: surface-fx table entry does not have 64 surface effects");
        for (std::size_t index = 0; index < 64; ++index)
            read(surfaces[index], entry[index + 1]);
        table.entries.push_back(std::move(entry));
    }
    if (static_cast<uint32_t>(Load(table.native.data(), 0x10)) != table.entries.size())
        throw std::runtime_error("stock: surface-fx table entry count mismatch");
    return table;
}

Image ParseImage(const Json &fields)
{
    std::unordered_set<uint64_t> addresses;
    CollectAddresses(fields, addresses);
    Image image;
    image.name = fields.at("name").at("string").get<std::string>();
    if (!fields.at("packedAtlasData").is_null() || !fields.at("fallback").is_null() ||
        fields.at("streamedPartCount").get<int>())
        throw std::runtime_error("stock: image " + image.name + " is streamed or atlas-packed");
    const auto native = Native(fields, image.native.size(), "image");
    std::memcpy(image.native.data(), native.data(), native.size());
    const auto &pixels = fields.at("pixels");
    if (pixels.at("active_member").get<std::string>() != "residentData")
        throw std::runtime_error("stock: image " + image.name + " pixels are not resident");
    image.pixels = Unhex(pixels.at("value").at("bytes").get<std::string>());
    if (image.pixels.size() != fields.at("totalSize").get<std::size_t>())
        throw std::runtime_error("stock: image " + image.name + " pixel size mismatch");
    Store(image.native.data(), 0x00, 0);
    Store(image.native.data(), 0xE0, 0);
    RequireNoAddresses(image.native.data(), image.native.size(), addresses, "image " + image.name);
    return image;
}

// ---- zone bodies -----------------------------------------------------------

struct StubLayout
{
    std::size_t size;
    uint64_t align;
};

StubLayout Stub(const IW8_XAssetType type)
{
    switch (type)
    {
    case ASSET_TYPE_XMODEL:
        return {iw8sz::XMODEL, 7};
    case ASSET_TYPE_VFX:
        return {iw8sz::VFX, 15};
    case ASSET_TYPE_PHYSICSFXPIPELINE:
    case ASSET_TYPE_PHYSICSFXSHAPE:
        return {0x30, 7};
    case ASSET_TYPE_VECTORFIELD:
        return {0x40, 15};
    case ASSET_TYPE_LIGHTDEF:
        return {iw8sz::LIGHTDEF, 7};
    default:
        throw std::runtime_error("stock: no reference layout for asset type " +
                                 std::to_string(static_cast<int>(type)));
    }
}

// Replay's preload (Preload_*Ptr) loads these top-level headers into block 2
// (TEMP_POSTLOAD), and each pop realigns block 2 to 32; materials, surfaces and
// images stay in block 1. Footprints measured with ACTS's preload stream trace
// of mp_shipment: physics asset 96, model 704, effect 128, physics shape 64,
// light 32 (nested inline assets would add theirs; the copy uses aliases).
std::size_t PreloadFootprint(const IW8_XAssetType type)
{
    std::size_t header = 0;
    switch (type)
    {
    case ASSET_TYPE_PHYSICSASSET:
        header = iw8sz::PHYSICSASSET;
        break;
    case ASSET_TYPE_XMODEL:
        header = iw8sz::XMODEL;
        break;
    case ASSET_TYPE_VFX:
        header = iw8sz::VFX;
        break;
    case ASSET_TYPE_PHYSICSFXPIPELINE:
    case ASSET_TYPE_PHYSICSFXSHAPE:
        header = 0x30;
        break;
    case ASSET_TYPE_VECTORFIELD:
        header = 0x40;
        break;
    case ASSET_TYPE_LIGHTDEF:
        header = iw8sz::LIGHTDEF;
        break;
    case ASSET_TYPE_SURFACE_FX:
        header = 0x28;
        break;
    default:
        return 0;
    }
    return (header + 31) & ~std::size_t{31};
}

// A name-only reference: the zeroed header with its name, which the loader
// resolves against the zone that owns the asset.
void RegisterReference(ZoneWriter &writer, const IW8_XAssetType type, const std::string &name)
{
    if (writer.has(type, name))
        return;
    if (type == ASSET_TYPE_MATERIAL)
        return replayrender::RegisterMaterialReference(writer, name);
    if (type == ASSET_TYPE_PHYSICSASSET)
        return iw8::writePhysicsAssetReference(writer, name);
    const auto layout = Stub(type);
    writer.add(type, name, [layout, name](ZoneWriter &out) {
        std::vector<uint8_t> header(layout.size);
        Store(header.data(), 0, PTR_FOLLOWS);
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(layout.align);
        out.write(header.data(), header.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr("," + name);
        out.popStream();
        out.popStream();
    });
}

void RequireNew(const ZoneWriter &writer, const IW8_XAssetType type, const std::string &name)
{
    if (writer.has(type, name))
        throw std::runtime_error("stock: the map zone already registers " + std::string(iw8sz::type_name(type)) +
                                 " '" + name + "', which the stock copy defines");
}

void RegisterShape(ZoneWriter &writer, const Shape &shape)
{
    RequireNew(writer, ASSET_TYPE_PHYSICSFXSHAPE, shape.name);
    writer.add(ASSET_TYPE_PHYSICSFXSHAPE, shape.name, [shape](ZoneWriter &out) {
        auto header = shape.native;
        Store(header.data(), 0x00, PTR_FOLLOWS);
        Store(header.data(), 0x10, shape.havok.empty() ? PTR_NULL : PTR_FOLLOWS);
        Store(header.data(), 0x18, shape.sound ? PTR_FOLLOWS : PTR_NULL);
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(7);
        out.write(header.data(), header.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(shape.name);
        if (!shape.havok.empty())
        {
            out.align(15);
            out.write(shape.havok.data(), shape.havok.size());
        }
        if (shape.sound)
            out.writeStr(*shape.sound);
        out.popStream();
        out.popStream();
    });
}

void RegisterLight(ZoneWriter &writer, const LightDef &light)
{
    RequireNew(writer, ASSET_TYPE_LIGHTDEF, light.name);
    writer.add(ASSET_TYPE_LIGHTDEF, light.name, [light](ZoneWriter &out) {
        auto header = light.native;
        Store(header.data(), 0x00, PTR_FOLLOWS);
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(7);
        out.write(header.data(), header.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(light.name);
        out.popStream();
        out.popStream();
    });
}

// `preload`: the block-2 bytes Replay's preload needs for every asset of the
// copy (PreloadFootprint), reserved once with the table.
void RegisterTable(ZoneWriter &writer, const Table &table, const std::size_t preload)
{
    RequireNew(writer, ASSET_TYPE_SURFACE_FX, table.name);
    writer.add(ASSET_TYPE_SURFACE_FX, table.name, [table, preload](ZoneWriter &out) {
        auto header = table.native;
        Store(header.data(), 0x00, PTR_FOLLOWS);
        Store(header.data(), 0x08, table.entries.empty() ? PTR_NULL : PTR_FOLLOWS);
        Store(header.data(), 0x18, table.poolSize ? PTR_FOLLOWS : PTR_NULL);
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(7);
        out.write(header.data(), header.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(table.name);
        if (!table.entries.empty())
        {
            out.align(7);
            for (const auto &entry : table.entries)
                for (const auto &effect : entry)
                    out.writeT<uint64_t>(effect.empty() ? PTR_NULL : out.assetAlias(ASSET_TYPE_VFX, effect));
        }
        if (table.poolSize)
        {
            // Runtime pool: zero-filled at load, no file bytes.
            out.pushStream(XFILE_BLOCK_SHARED_STREAM);
            out.align(3);
            out.reserveCalc(table.poolSize);
            out.popStream();
        }
        out.popStream();
        out.popStream();
        out.pushStream(XFILE_BLOCK_TEMP_POSTLOAD);
        out.align(31);
        out.reserveCalc(preload);
        out.popStream();
    });
}

void RegisterImage(ZoneWriter &writer, const Image &image)
{
    RequireNew(writer, ASSET_TYPE_IMAGE, image.name);
    writer.add(ASSET_TYPE_IMAGE, image.name, [image](ZoneWriter &out) {
        // As the converter's resident images (Load_GfxImage E2FD20): header in
        // TEMP_PRELOAD, name in VIRTUAL, pixels in TEMP_PRELOAD, and the header's
        // postload copy reserved in TEMP_POSTLOAD.
        auto header = image.native;
        Store(header.data(), 0x00, PTR_FOLLOWS);
        Store(header.data(), 0xE0, PTR_FOLLOWS);
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(15);
        out.write(header.data(), header.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(image.name);
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(15);
        out.write(image.pixels.data(), image.pixels.size());
        out.popStream();
        out.popStream();
        out.popStream();
        out.pushStream(XFILE_BLOCK_TEMP_POSTLOAD);
        out.align(15);
        out.reserveCalc(iw8sz::IMAGE);
        out.popStream();
    });
}
} // namespace

struct Copier::Impl
{
    std::set<Key> references;
    std::vector<LightDef> lights;
    std::vector<Shape> shapes;
    std::vector<Model> models;
    std::vector<vfx::Effect> effects; // children before parents
    std::optional<Table> table;
    std::vector<Image> englishImages, worldwideImages;
    std::map<std::pair<int, std::string>, std::string> origins;
};

Copier::Copier(const Options &options) : impl_(std::make_unique<Impl>())
{
    if (options.acts.empty() || !fs::is_regular_file(options.acts))
        throw std::runtime_error("stock: ACTS executable not found: " + options.acts.string());
    const fs::path zones = options.replayExecutable.parent_path() / "zone";
    const fs::path shipZone = zones / (std::string(kSourceMap) + ".ff");
    const fs::path worldwideZone = zones / "worldwide" / ("ww_" + std::string(kSourceMap) + ".ff");
    const fs::path englishZone = zones / "english" / ("eng_" + std::string(kSourceMap) + ".ff");
    const std::string ship = kSourceMap;
    const std::string worldwide = "ww_" + ship;

    const auto shipExport = RunExport(options, shipZone, "vfx,physicsfxshape,surfacefx,lightdef", {});
    const auto worldwideExport = RunExport(options, worldwideZone, "vfx,physicsfxshape,lightdef", {});

    auto &impl = *impl_;
    const auto *tableFile = shipExport.find(ASSET_TYPE_SURFACE_FX, kTable);
    if (!tableFile)
        throw std::runtime_error("stock: " + ship + " has no surface-fx table '" + kTable + "'");
    std::vector<Reference> pending;
    impl.table = ParseTable(Fields(*tableFile), pending);
    impl.origins[{ASSET_TYPE_SURFACE_FX, kTable}] =
        "stock surface-fx table (copied from " + ship + ", section 19)";

    // Resolve the closure. What the shipped zone owns is copied; a comma
    // reference is copied when the map's worldwide pack owns it (our ww_<map>
    // stays empty) and otherwise stays a name-only reference to the common or
    // worldwide-common zone that owns it.
    std::map<std::string, vfx::Effect> effects;
    std::map<std::string, std::vector<std::string>> effectChildren;
    std::set<std::string> shapes, lights;
    std::map<std::string, const Export *> localModels; // name -> zone that owns it
    const auto addReference = [&](const Key &key, const std::string &origin) {
        impl.references.insert(key);
        impl.origins.emplace(std::pair{static_cast<int>(key.first), key.second}, origin);
    };
    struct Pending
    {
        Reference ref;
        std::string user;
        const Export *from;
    };
    std::vector<Pending> queue;
    for (auto &ref : pending)
        queue.push_back({std::move(ref), std::string("surface-fx table ") + kTable, &shipExport});
    pending.clear();
    for (std::size_t index = 0; index < queue.size(); ++index)
    {
        const Pending item = queue[index];
        const Reference &ref = item.ref;
        const Export &other = item.from == &shipExport ? worldwideExport : shipExport;
        const Export *owner = nullptr;
        if (!ref.shared)
            owner = item.from->find(ref.type, ref.name) ? item.from
                    : other.find(ref.type, ref.name)    ? &other
                                                        : nullptr;
        else if (item.from == &shipExport && worldwideExport.find(ref.type, ref.name))
            owner = &worldwideExport;
        const std::string common = "name-only reference to the common-zone asset (stock " + std::string(kTable) +
                                   " closure, used by " + item.user + ")";
        switch (ref.type)
        {
        case ASSET_TYPE_VFX:
            if (!owner)
            {
                if (!ref.shared)
                    throw std::runtime_error("stock: effect '" + ref.name + "' is not in the exports");
                addReference({ref.type, ref.name}, common);
                break;
            }
            if (!effects.contains(ref.name))
            {
                std::vector<Reference> dependencies;
                auto effect = ParseEffect(Fields(*owner->find(ref.type, ref.name)), dependencies);
                effects.emplace(ref.name, std::move(effect));
                impl.origins[{ASSET_TYPE_VFX, ref.name}] =
                    "stock effect of surface-fx table " + std::string(kTable) + " (copied from " + owner->zone + ")";
                auto &children = effectChildren[ref.name];
                for (auto &dependency : dependencies)
                {
                    if (dependency.type == ASSET_TYPE_VFX)
                        children.push_back(dependency.name);
                    queue.push_back({std::move(dependency), ref.name, owner});
                }
            }
            break;
        case ASSET_TYPE_PHYSICSFXSHAPE:
        case ASSET_TYPE_LIGHTDEF:
            if (!owner)
            {
                if (!ref.shared)
                    throw std::runtime_error("stock: " + std::string(iw8sz::type_name(ref.type)) + " '" + ref.name +
                                             "' is not in the exports");
                addReference({ref.type, ref.name}, common);
                break;
            }
            if (ref.type == ASSET_TYPE_PHYSICSFXSHAPE && shapes.insert(ref.name).second)
            {
                impl.shapes.push_back(ParseShape(Fields(*owner->find(ref.type, ref.name))));
                impl.origins[{ref.type, ref.name}] =
                    "stock physics shape of effect " + item.user + " (copied from " + owner->zone + ")";
            }
            if (ref.type == ASSET_TYPE_LIGHTDEF && lights.insert(ref.name).second)
            {
                impl.lights.push_back(ParseLightDef(Fields(*owner->find(ref.type, ref.name))));
                impl.origins[{ref.type, ref.name}] = "stock effect light (copied from " + owner->zone + ")";
            }
            break;
        case ASSET_TYPE_XMODEL:
            // Models are exported on demand (the pool is large).
            if (ref.shared)
                addReference({ref.type, ref.name}, common);
            else
                localModels.try_emplace(ref.name, item.from);
            break;
        case ASSET_TYPE_MATERIAL:
        case ASSET_TYPE_PHYSICSFXPIPELINE:
        case ASSET_TYPE_VECTORFIELD:
        case ASSET_TYPE_PHYSICSASSET:
            if (!ref.shared)
                throw std::runtime_error("stock: " + item.user + " uses a zone-local " +
                                         iw8sz::type_name(ref.type) + " '" + ref.name +
                                         "', which the copier does not carry");
            addReference({ref.type, ref.name}, common);
            break;
        default:
            throw std::runtime_error("stock: unsupported reference from " + item.user);
        }
    }

    // Local models: copied when the model writer reproduces them, otherwise
    // they must come from a worldwide-common zone (loaded with common).
    std::map<const Export *, std::vector<std::string>> modelsByZone;
    for (const auto &[name, zone] : localModels)
        modelsByZone[zone].push_back(name);
    std::vector<std::string> unsupportedModels;
    for (const auto &[zoneExport, modelNames] : modelsByZone)
    {
        const fs::path zoneFile = zoneExport == &shipExport ? shipZone : worldwideZone;
        const auto modelExport = RunExport(options, zoneFile, "xmodel", modelNames);
        std::vector<std::string> surfaceNames;
        std::vector<std::pair<convert::xmodel::Iw8XModelRecord, std::vector<Reference>>> records;
        for (const auto &name : modelNames)
        {
            const auto *file = modelExport.find(ASSET_TYPE_XMODEL, name);
            if (!file)
                throw std::runtime_error("stock: model '" + name + "' is not in " + zoneExport->zone);
            std::vector<Reference> dependencies;
            std::string unsupported;
            auto record = ParseModel(Fields(*file), modelExport, dependencies, unsupported);
            if (!record)
            {
                zt::info("stock: model %s uses %s; it is referenced from its worldwide-common copy",
                         name.c_str(), unsupported.c_str());
                unsupportedModels.push_back(name);
                continue;
            }
            for (const auto &lod : record->lods)
                surfaceNames.push_back(lod.surfsName);
            records.push_back({std::move(*record), std::move(dependencies)});
        }
        if (records.empty())
            continue;
        const auto surfaceExport = RunExport(options, zoneFile, "xmodelsurfs", surfaceNames);
        for (auto &[record, dependencies] : records)
        {
            Model model;
            for (const auto &lod : record.lods)
            {
                const auto *file = surfaceExport.find(ASSET_TYPE_XMODELSURFS, lod.surfsName);
                if (!file)
                    throw std::runtime_error("stock: model surfaces '" + lod.surfsName + "' are not in " +
                                             zoneExport->zone);
                model.lods.push_back(ParseSurfs(Fields(*file)));
                impl.origins[{ASSET_TYPE_XMODELSURFS, lod.surfsName}] =
                    "LOD of stock effect model " + record.name + " (copied from " + zoneExport->zone + ")";
            }
            for (const auto &dependency : dependencies)
                addReference({dependency.type, dependency.name},
                             "name-only reference to the common-zone asset (used by stock effect model " +
                                 record.name + ")");
            impl.origins[{ASSET_TYPE_XMODEL, record.name}] =
                "stock effect model (copied from " + zoneExport->zone + ")";
            model.record = std::move(record);
            impl.models.push_back(std::move(model));
        }
    }
    if (!unsupportedModels.empty())
    {
        std::set<std::string> found;
        for (const auto &zone : kWorldwideCommon)
        {
            const auto common = RunExport(options, zones / "worldwide" / (zone + ".ff"), "xmodel", unsupportedModels);
            for (const auto &name : unsupportedModels)
                if (common.find(ASSET_TYPE_XMODEL, name))
                    found.insert(name);
        }
        for (const auto &name : unsupportedModels)
        {
            if (!found.contains(name))
                throw std::runtime_error("stock: model '" + name +
                                         "' is neither copyable nor in a worldwide-common zone");
            addReference({ASSET_TYPE_XMODEL, name},
                         "name-only reference to the worldwide-common model (stock " + std::string(kTable) +
                             " closure; " + ship + " carries a copy the model writer cannot reproduce)");
        }
    }

    // A copy supersedes a name-only reference to the same asset.
    for (const auto &[name, effect] : effects)
        impl.references.erase({ASSET_TYPE_VFX, name});
    for (const auto &name : shapes)
        impl.references.erase({ASSET_TYPE_PHYSICSFXSHAPE, name});
    for (const auto &name : lights)
        impl.references.erase({ASSET_TYPE_LIGHTDEF, name});
    for (const auto &model : impl.models)
        impl.references.erase({ASSET_TYPE_XMODEL, model.record.name});

    // Effects in dependency order: every child effect before its parents.
    std::set<std::string> done, visiting;
    std::function<void(const std::string &)> visit = [&](const std::string &name) {
        if (done.contains(name) || !effects.contains(name))
            return;
        if (!visiting.insert(name).second)
            throw std::runtime_error("stock: effect reference cycle at '" + name + "'");
        for (const auto &child : effectChildren[name])
            visit(child);
        visiting.erase(name);
        done.insert(name);
        impl.effects.push_back(std::move(effects.at(name)));
    };
    std::vector<std::string> names;
    for (const auto &[name, effect] : effects)
        names.push_back(name);
    for (const auto &name : names)
        visit(name);

    // CODCaster images.
    const auto englishExport = RunExport(options, englishZone, "image", kEnglishImages);
    for (const auto &name : kEnglishImages)
    {
        impl.englishImages.push_back(ParseImage(Fields(*englishExport.find(ASSET_TYPE_IMAGE, name))));
        impl.origins[{ASSET_TYPE_IMAGE, name}] = "stock CODCaster image (copied from eng_" + ship + ", section 19)";
    }
    const auto worldwideImages = RunExport(options, worldwideZone, "image", kWorldwideImages);
    for (const auto &name : kWorldwideImages)
    {
        impl.worldwideImages.push_back(ParseImage(Fields(*worldwideImages.find(ASSET_TYPE_IMAGE, name))));
        impl.origins[{ASSET_TYPE_IMAGE, name}] = "stock CODCaster image (copied from " + worldwide + ", section 19)";
    }

    std::size_t modules = 0;
    for (const auto &effect : impl.effects)
        for (const auto &emitter : effect.emitters)
            for (const auto &state : emitter.states)
                for (const auto &group : state.groups)
                    modules += group.size();
    zt::info("stock: %s '%s' closure: %zu effects (%zu modules), %zu physics shapes, %zu lights, %zu models, "
             "%zu name-only references; %zu + %zu CODCaster images",
             "surface-fx table", kTable, impl.effects.size(), modules, impl.shapes.size(), impl.lights.size(),
             impl.models.size(), impl.references.size(), impl.englishImages.size(), impl.worldwideImages.size());
}

Copier::~Copier() = default;

void Copier::RegisterSurfaceEffects(ZoneWriter &main, ZoneWriter &techsets) const
{
    const auto &impl = *impl_;
    std::size_t preload = 0;
    for (const auto &[type, name] : impl.references)
    {
        if (!main.has(type, name))
        {
            RegisterReference(main, type, name);
            preload += PreloadFootprint(type);
        }
        if (type == ASSET_TYPE_MATERIAL)
            RegisterReference(techsets, type, name);
    }
    for (const auto &light : impl.lights)
    {
        RegisterLight(main, light);
        preload += PreloadFootprint(ASSET_TYPE_LIGHTDEF);
    }
    for (const auto &shape : impl.shapes)
    {
        RegisterShape(main, shape);
        preload += PreloadFootprint(ASSET_TYPE_PHYSICSFXSHAPE);
    }
    for (const auto &model : impl.models)
    {
        for (const auto &lod : model.lods)
        {
            RequireNew(main, ASSET_TYPE_XMODELSURFS, lod.name);
            iw8xs_dump::writeXModelSurfs(main, lod.name, lod);
        }
        RequireNew(main, ASSET_TYPE_XMODEL, model.record.name);
        iw8::writeXModel(main, model.record);
        preload += PreloadFootprint(ASSET_TYPE_XMODEL);
    }
    for (const auto &effect : impl.effects)
    {
        RequireNew(main, ASSET_TYPE_VFX, effect.name);
        vfx::Register(main, effect);
        preload += PreloadFootprint(ASSET_TYPE_VFX);
    }
    if (impl.table)
        RegisterTable(main, *impl.table, preload + PreloadFootprint(ASSET_TYPE_SURFACE_FX));
}

void Copier::RegisterEnglishImages(ZoneWriter &english) const
{
    for (const auto &image : impl_->englishImages)
        RegisterImage(english, image);
}

void Copier::RegisterWorldwideImages(ZoneWriter &worldwide) const
{
    for (const auto &image : impl_->worldwideImages)
        RegisterImage(worldwide, image);
}

const std::map<std::pair<int, std::string>, std::string> &Copier::Origins() const
{
    return impl_->origins;
}
} // namespace iw8::stock
