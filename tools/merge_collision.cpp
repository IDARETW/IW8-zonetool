// Repackage shipped Replay collision tiles without changing their Havok payloads.
// Replay loader evidence: CollisionTile E0A180, compressed list E0E200/model
// E0E100, PhysicsAsset DF3880, detail E2A3A0, MapEdgeList E0B1F0.
#include "common/ff_io.h"
#include "common/fs_util.h"
#include "common/log.h"
#include "zonetool/iw8/iw8_ffheader.h"
#include "zonetool/iw8/iw8_zone.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Bytes = std::vector<uint8_t>;
using Key = std::pair<int, std::string>;
constexpr uint64_t follows = iw8::PTR_FOLLOWS;
constexpr uint64_t virtualTag = uint64_t{5} << 32;

// All serialized fields are retained, including currently unnamed runtime fields.
// These sizes/offsets come from the Replay loaders, not the later game-test ABI.
struct CollisionTile
{
    uint64_t name;
    uint32_t index;
    uint8_t gridFields[4];
    float origin[3];
    uint32_t pad1c;
    uint64_t models;
    uint32_t havokSize, pad2c;
    uint64_t havokData, cachedShape;
    uint8_t trailing[8];
};
struct CompressedModelList
{
    uint32_t count, pad04;
    uint64_t models;
    uint8_t trailing[16];
};
struct CompressedModel
{
    uint32_t name, pad04;
    uint64_t physics, detail, runtimeShape, runtimeDetailShape;
    uint16_t shapeTag, detailShapeTag;
    uint32_t instanceCount;
    uint64_t instances;
};
static_assert(sizeof(CollisionTile) == 0x48 && offsetof(CollisionTile, models) == 0x20);
static_assert(sizeof(CompressedModelList) == 0x20);
static_assert(sizeof(CompressedModel) == 0x38 && offsetof(CompressedModel, instances) == 0x30);

template <typename T> T get(const Bytes &data, size_t offset)
{
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("field exceeds serialized data");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}
template <typename T> void put(Bytes &data, size_t offset, T value)
{
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("patch exceeds serialized data");
    std::memcpy(data.data() + offset, &value, sizeof(value));
}
uint32_t u32(const Bytes &b, size_t o) { return get<uint32_t>(b, o); }
uint64_t u64(const Bytes &b, size_t o) { return get<uint64_t>(b, o); }
void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Piece
{
    uint64_t alignment{};
    Bytes bytes;
    std::map<size_t, Key> references;
    bool operator==(const Piece &) const = default;
};
struct Asset
{
    int type{};
    std::string name;
    Bytes root;
    std::vector<Piece> pieces;
    bool operator==(const Asset &) const = default;
};
struct Assets
{
    std::map<Key, Asset> values;
    std::vector<std::string> strings;
    std::map<std::string, uint32_t> stringIds;
    uint64_t duplicates{}, references{}, instances{}, havokBytes{};

    uint32_t intern(const std::string &s)
    {
        const auto [it, added] = stringIds.try_emplace(s, uint32_t(strings.size() + 1));
        if (added) strings.push_back(s);
        return it->second;
    }
    Key add(Asset asset)
    {
        const bool reference = asset.name.starts_with(',');
        Key key{asset.type, reference ? asset.name.substr(1) : asset.name};
        require(!key.second.empty(), "empty asset name");
        auto it = values.find(key);
        if (it == values.end()) values.emplace(key, std::move(asset));
        else if (it->second.name.starts_with(',') && !reference) it->second = std::move(asset);
        else if (!reference)
        {
            require(it->second == asset, "conflicting tile dependency: " + key.second);
            ++duplicates;
        }
        if (reference) ++references;
        return key;
    }
};

using Decompress = int64_t(__cdecl *)(const void *, int64_t, void *, int64_t,
                                     int, int, int, void *, int64_t, void *, void *, void *, int64_t, int);

Bytes decode(const std::filesystem::path &path, Decompress oodle, IW8_DB_FFHeader &header)
{
    Bytes file;
    require(zt::read_file(path.string(), file) && file.size() >= 0x8c, "cannot read " + path.string());
    std::memcpy(&header, file.data(), 0x88);
    const bool secured = std::memcmp(header.magic, iw8ff::kMagic, 8) == 0;
    require(secured || std::memcmp(header.magic, iw8ff::kMagicUnsec, 8) == 0, "not a Replay fastfile");
    require(header.headerVersion == 11 &&
                (header.xfileVersion == 0xff3 || header.xfileVersion == 0xff7) &&
                header.dashEncryptBuild == 0 && header.transientFileType == 3,
            "unsupported collision fastfile header");
    const size_t end = 0x88ull + header.residentPartSize;
    require(end == file.size(), "resident fastfile size mismatch");
    Bytes compressed;
    if (secured)
    {
        // Skip the signed header/master hashes, then each 16-KiB hash page.
        for (size_t at = 0x8088; at < end; at += 0x804000)
            compressed.insert(compressed.end(), file.begin() + at,
                              file.begin() + (std::min)(at + 0x800000, end));
    }
    else compressed.assign(file.begin() + 0x88, file.end());
    require(compressed.size() >= 4 && compressed[1] == 'I' && compressed[2] == 'W' &&
                compressed[3] == 'C', "invalid IWC frame");
    if (compressed[0] == 1)
    {
        require(header.xfileHeader.size == compressed.size() - 4, "stored frame size mismatch");
        return Bytes(compressed.begin() + 4, compressed.end());
    }
    require(compressed[0] == 2 && compressed.size() >= 12 && oodle, "unsupported IWC encoding");
    Bytes result;
    require(header.xfileHeader.size <= UINT32_MAX, "zone too large");
    result.reserve(size_t(header.xfileHeader.size));
    size_t pos = 12;
    while (result.size() < header.xfileHeader.size)
    {
        const uint32_t packed = u32(compressed, pos), raw = u32(compressed, pos + 4);
        require(raw && raw <= 0x10000 && packed && packed <= 0x10100 &&
                    u32(compressed, pos + 8) == 0 && pos + 12ull + packed <= compressed.size() &&
                    raw <= header.xfileHeader.size - result.size(), "invalid Oodle block");
        const size_t start = result.size();
        result.resize(start + raw);
        require(oodle(compressed.data() + pos + 12, packed, result.data() + start, raw,
                      1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3) == raw,
                "Oodle decompression failed");
        pos += 12 + ((packed + 3ull) & ~3ull);
    }
    return result;
}

struct Reader
{
    const Bytes &data;
    Assets &assets;
    size_t pos{32};
    uint64_t virtualPos{}, assetBase{};
    unsigned depth{};
    std::map<uint64_t, std::string> names;
    std::map<uint64_t, Key> aliases;
    std::vector<uint32_t> scriptStrings;
    std::set<uint32_t> tiles;
    std::set<std::string> edges;

    Bytes take(size_t n, uint64_t alignment = 0, bool virt = true)
    {
        require(pos <= data.size() && n <= data.size() - pos, "truncated asset body");
        if (virt) virtualPos = (virtualPos + alignment) & ~alignment;
        Bytes out(data.begin() + pos, data.begin() + pos + n);
        pos += n;
        if (virt) virtualPos += n;
        return out;
    }
    uint64_t packed(uint64_t p) const
    {
        require(((p >> 32) & 15) == 5, "unsupported packed stream reference");
        return (p & ~0x1000000000ull) + ((p & 0x1000000000ull) ? assetBase : 0);
    }
    std::string string(uint64_t p)
    {
        if (!p) return {};
        if (p != follows && p != iw8::PTR_SHARED)
        {
            auto it = names.find(packed(p));
            require(it != names.end(), "unresolved shared name");
            return it->second;
        }
        const auto end = std::find(data.begin() + pos, data.end(), uint8_t{});
        require(end != data.end(), "unterminated asset name");
        const uint64_t slot = virtualTag + virtualPos + 1;
        auto bytes = take(size_t(end - data.begin()) - pos + 1);
        std::string s(reinterpret_cast<const char *>(bytes.data()), bytes.size() - 1);
        names.emplace(slot, s);
        return s;
    }
    void scripts(Bytes &b, size_t offset, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t old = u32(b, offset + i * 4);
            require(old < scriptStrings.size(), "script string index exceeds source table");
            put(b, offset + i * 4, scriptStrings[old]);
        }
    }
    Key pointer(int type, uint64_t p, uint64_t slot)
    {
        require(p != 0, "unexpected null asset pointer");
        if (p != follows && p != iw8::PTR_SHARED && p != iw8::PTR_INSERT)
        {
            auto it = aliases.find(packed(p));
            require(it != aliases.end() && it->second.first == type, "unresolved asset alias");
            aliases[slot] = it->second;
            return it->second;
        }
        uint64_t inserted{};
        if (p == iw8::PTR_INSERT)
        {
            virtualPos = (virtualPos + 7) & ~uint64_t{7};
            inserted = virtualTag + virtualPos + 1;
            virtualPos += 8;
        }
        auto key = asset(type);
        aliases[slot] = key;
        if (inserted) aliases[inserted] = key;
        return key;
    }
    Key asset(int type)
    {
        if (!depth) assetBase = virtualPos;
        require(++depth <= 8, "asset nesting exceeds collision schema");
        size_t size{};
        switch (type)
        {
        case 3: size = 0x58; break;
        case 101: size = 0x28; break;
        case 107: size = sizeof(CollisionTile); break;
        case 116: size = 0xa0; break;
        default: throw std::runtime_error("unsupported collision dependency type " + std::to_string(type));
        }
        Asset a{type, {}, take(size, 0, false), {}};
        a.name = string(u64(a.root, 0));
        put(a.root, 0, follows);
        a.pieces.push_back({0, Bytes(a.name.begin(), a.name.end()), {}});
        a.pieces.back().bytes.push_back(0);
        const auto blob = [&](size_t offset, size_t bytes, uint64_t alignment) {
            if (u64(a.root, offset))
            {
                put(a.root, offset, follows);
                a.pieces.push_back({alignment, take(bytes, alignment), {}});
            }
            else require(!bytes, "missing array in " + a.name);
        };
        if (type == 3)
        {
            blob(8, u32(a.root, 16), 15);
            blob(32, 4ull * u32(a.root, 24), 3);
            blob(40, 4ull * u32(a.root, 24), 3);
            for (auto [offset, count] : {std::pair{56u, 48u}, std::pair{64u, 52u}})
            {
                if (!u64(a.root, offset)) continue;
                blob(offset, 8ull * u32(a.root, count), 7);
                require(std::all_of(a.pieces.back().bytes.begin(), a.pieces.back().bytes.end(),
                                    [](uint8_t c) { return !c; }),
                        "non-null physics event dependency in " + a.name);
            }
        }
        else if (type == 101)
        {
            blob(16, u32(a.root, 8), 15);
            if (u64(a.root, 32))
            {
                blob(32, 4ull * u32(a.root, 24), 3);
                scripts(a.pieces.back().bytes, 0, u32(a.root, 24));
            }
        }
        else if (type == 116)
        {
            // MapEdgeList: lines, metadata, octree metadata/nodes, indices,
            // adjacency metadata, adjacency records. All seven arrays are retained.
            constexpr size_t fields[][4] = {{64,56,32,31}, {72,56,8,3}, {88,80,368,15},
                {104,96,128,127}, {120,112,4,3}, {136,128,4,3}, {152,144,12,3}};
            for (const auto &f : fields) blob(f[0], f[2] * u32(a.root, f[1]), f[3]);
            require(edges.insert(a.name).second, "duplicate edge list in source");
        }
        else
        {
            const auto index = u32(a.root, 8);
            require(index < 1024 && tiles.insert(index).second, "invalid or duplicate collision tile");
            if (u64(a.root, 32))
            {
                put(a.root, 32, follows);
                Bytes list = take(sizeof(CompressedModelList), 7);
                const uint32_t count = u32(list, 0);
                const bool present = u64(list, 8) != 0;
                if (present) put(list, 8, follows);
                else require(!count, "missing compressed models");
                a.pieces.push_back({7, std::move(list), {}});
                if (present)
                {
                    Piece models{7, take(sizeof(CompressedModel) * size_t(count), 7), {}};
                    const uint64_t start = virtualPos - models.bytes.size();
                    // Keep instance arrays separate until every inline dependency is consumed.
                    std::vector<Piece> instances;
                    for (size_t i = 0; i < count; ++i)
                    {
                        const size_t at = i * sizeof(CompressedModel);
                        scripts(models.bytes, at, 1);
                        for (auto [offset, dep] : {std::pair{8u,3}, std::pair{16u,101}})
                        {
                            const uint64_t p = u64(models.bytes, at + offset);
                            if (!p) continue;
                            models.references.emplace(at + offset,
                                pointer(dep, p, virtualTag + start + at + offset + 1));
                            put(models.bytes, at + offset, follows);
                        }
                        const size_t n = u32(models.bytes, at + 44);
                        if (u64(models.bytes, at + 48))
                        {
                            put(models.bytes, at + 48, follows);
                            instances.push_back({1, take(14 * n, 1), {}});
                        }
                        else require(!n, "missing model instances");
                        assets.instances += n;
                    }
                    a.pieces.push_back(std::move(models));
                    for (auto &i : instances) a.pieces.push_back(std::move(i));
                }
            }
            blob(48, u32(a.root, 40), 15);
            assets.havokBytes += u32(a.root, 40);
            // Replay's loader reads one char for this runtime shape placeholder.
            if (u64(a.root, 56)) blob(56, 1, 0);
        }
        --depth;
        return assets.add(std::move(a));
    }
    void read(const IW8_DB_FFHeader &header)
    {
        require(data.size() >= 32 && !u32(data, 4) && !u32(data, 20), "invalid XAssetList");
        const size_t strings = u32(data, 0), count = u32(data, 16);
        require((strings == 0) == (u64(data, 8) == 0) &&
                    (count == 0) == (u64(data, 24) == 0), "invalid XAssetList pointers");
        if (strings)
        {
            const auto pointers = take(8 * strings, 7);
            for (size_t i = 0; i < strings; ++i)
            {
                const auto p = u64(pointers, i * 8);
                scriptStrings.push_back(p ? assets.intern(string(p)) : 0);
            }
        }
        const auto table = take(count * 16, 7);
        const uint64_t start = virtualPos - table.size();
        for (size_t i = 0; i < count; ++i)
            pointer(int(u32(table, i * 16)), u64(table, i * 16 + 8), virtualTag + start + i * 16 + 9);
        require(pos == data.size(), "unconsumed bytes after collision assets");
        require(virtualPos == header.xfileHeader.blockSize[5], "virtual stream reservation mismatch");
    }
};

void write(const std::filesystem::path &path, Assets &assets)
{
    iw8::ZoneWriter writer;
    for (size_t i = 0; i < assets.strings.size(); ++i)
        require(writer.internScriptString(assets.strings[i]) == i + 1, "script string remap mismatch");
    // Shipped collision zones include 64 bytes of root/preload framing, then
    // 32-byte-rounded asset bodies in stream 2. Load uses scratch stream 1;
    // Preload/Postload_PhysicsAssetPtr and the other pointer loaders use 2.
    // This gives exactly 413664 for shipped Quarry and 19744 for tile 00500.
    writer.pushStream(iw8::XFILE_BLOCK_TEMP_POSTLOAD);
    writer.reserveCalc(64);
    writer.popStream();
    // Preserve the full-collision ordering: dependencies, edges, then tiles.
    std::vector<const std::pair<const Key, Asset> *> ordered;
    for (int type : {3, 101, 116, 107})
        for (const auto &entry : assets.values)
            if (entry.first.first == type) ordered.push_back(&entry);
    for (const auto *entry : ordered)
    {
        const auto &[key, a] = *entry;
        writer.add(IW8_XAssetType(a.type), key.second, [&a](iw8::ZoneWriter &w) {
            w.pushStream(iw8::XFILE_BLOCK_TEMP_PRELOAD);
            w.align(7);
            w.write(a.root.data(), a.root.size());
            w.pushStream(iw8::XFILE_BLOCK_VIRTUAL);
            for (const auto &piece : a.pieces)
            {
                w.align(piece.alignment);
                if (piece.references.empty()) w.write(piece.bytes.data(), piece.bytes.size());
                else
                {
                    auto bytes = piece.bytes;
                    for (const auto &[offset, ref] : piece.references)
                        put(bytes, offset, w.assetAlias(IW8_XAssetType(ref.first), ref.second));
                    w.write(bytes.data(), bytes.size());
                }
            }
            w.popStream();
            w.popStream();
            w.pushStream(iw8::XFILE_BLOCK_TEMP_POSTLOAD);
            w.align(7);
            w.reserveCalc(a.root.size());
            w.align(31);
            w.popStream();
        });
    }
    writer.build();
    zt::Iw8WriteParams params;
    for (int i = 0; i < 11; ++i) params.blockSize[i] = writer.blockSize(i);
    params.totalDecompressed = writer.totalDecompressed();
    params.calcSize = writer.calcSize();
    params.transientFileType = 3;
    // Shipped collision tiles and this Replay executable require 4083. The
    // shared custom-map writer defaults to 4087; do not inherit it for imports.
    params.xfileVersion = 0xff3;
    require(zt::iw8_write(path.string(), writer.body(), params), "fastfile write failed");
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc != 5)
        {
            std::fprintf(stderr, "usage: iw8-collision-transient <zone directory or reference.ff> <map> <output.ff> <oodle.dll>\n");
            return 2;
        }
        const std::filesystem::path source = argv[1], output = argv[3];
        const std::string map = argv[2];
        require(map.starts_with("mp_") && map.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos,
                "invalid map name");
        require(output.stem().string() == map + "_cg_ls_tr", "output must use the native full-collision zone name");
        const HMODULE module = LoadLibraryW(std::filesystem::absolute(argv[4]).c_str());
        const auto oodle = module ? reinterpret_cast<Decompress>(GetProcAddress(module, "OodleLZ_Decompress")) : nullptr;
        require(oodle != nullptr, "cannot load the supplied game's Oodle decoder");
        Assets assets;
        std::set<uint32_t> tiles;
        std::set<std::string> edges;
        const bool reference = std::filesystem::is_regular_file(source);
        const unsigned files = reference ? 1 : 1024;
        for (unsigned i = 0; i < files; ++i)
        {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "_cg_%05u_tr.ff", i);
            const auto input = reference ? source : source / (map + suffix);
            try
            {
                IW8_DB_FFHeader header{};
                const auto bytes = decode(input, oodle, header);
                Reader reader{bytes, assets};
                reader.read(header);
                require(reference || (reader.tiles.size() == 1 && reader.tiles.contains(i)), "source tile index mismatch");
                for (auto tile : reader.tiles) require(tiles.insert(tile).second, "duplicate source tile");
                for (const auto &edge : reader.edges) require(edges.insert(edge).second, "duplicate source edge list");
            }
            catch (const std::exception &e) { throw std::runtime_error(input.filename().string() + ": " + e.what()); }
            if ((i + 1) % 64 == 0 || reference)
                zt::info("read %u/%u files; %zu unique assets", i + 1, files, assets.values.size());
        }
        require(tiles.size() == 1024, "full collision requires all 1024 tile slots");
        std::map<int, size_t> counts;
        size_t external{};
        for (const auto &[key, a] : assets.values)
        {
            ++counts[a.type];
            if (a.name.starts_with(',')) ++external;
        }
        zt::info("merge: tiles=%zu edges=%zu physics=%zu detail=%zu base-zone references=%zu strings=%zu instances=%llu world-Havok=%llu bytes",
                 tiles.size(), edges.size(), counts[3], counts[101], external, assets.strings.size(),
                 static_cast<unsigned long long>(assets.instances), static_cast<unsigned long long>(assets.havokBytes));
        // Validate a candidate before the atomic publish. An existing test file is
        // never replaced by a failed import, conflicting asset, or partial write.
        const std::filesystem::path candidate = output.string() + ".candidate";
        write(candidate, assets);
        IW8_DB_FFHeader header{};
        const auto bytes = decode(candidate, oodle, header);
        Assets check;
        // Retain intern order so the normalized comparison includes every script ID.
        for (const auto &s : assets.strings) check.intern(s);
        Reader reader{bytes, check};
        reader.read(header);
        require(reader.tiles == tiles && reader.edges == edges && check.values == assets.values &&
                    check.instances == assets.instances && check.havokBytes == assets.havokBytes,
                "semantic round-trip verification failed");
        require(MoveFileExW(std::filesystem::absolute(candidate).c_str(),
                            std::filesystem::absolute(output).c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "cannot publish verified collision fastfile");
        zt::info("verified: every asset, script string, model instance, edge array and Havok payload survived the round trip");
        FreeLibrary(module);
        return 0;
    }
    catch (const std::exception &e)
    {
        zt::err("collision merge: %s", e.what());
        return 1;
    }
}
