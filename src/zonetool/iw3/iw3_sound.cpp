#include "iw3_sound.h"

#include "common/fs_util.h"
#include "common/json.hpp"
#include "common/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace iw3::sound
{
namespace
{
using Json = nlohmann::json;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(const std::string &text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// IW3 sound paths are relative to sound/; zone dumps prefix referenced
// loaded sounds with ','.
std::string NormalizeSoundPath(std::string path)
{
    path = Lower(Trim(path));
    while (!path.empty() && path.front() == ',')
        path.erase(path.begin());
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.starts_with("sound/"))
        path.erase(0, 6);
    return path;
}

// ------------------------------------------------------------- inflate
// RFC 1951 decoder for IWD (zip) members, after Mark Adler's puff.
class Inflater
{
  public:
    Inflater(const uint8_t *data, const size_t size, std::vector<uint8_t> &out)
        : data_(data)
        , size_(size)
        , out_(out)
    {
    }

    void Run()
    {
        int last = 0;
        do
        {
            last = Bits(1);
            const int type = Bits(2);
            if (type == 0)
                Stored();
            else if (type == 1)
                Fixed();
            else if (type == 2)
                Dynamic();
            else
                throw std::runtime_error("invalid deflate block type");
        } while (!last);
    }

  private:
    struct Huffman
    {
        std::array<int16_t, 16> count{};
        std::array<int16_t, 320> symbol{};
    };

    int Bits(const int need)
    {
        uint32_t value = bitBuffer_;
        while (bitCount_ < need)
        {
            if (position_ >= size_)
                throw std::runtime_error("truncated deflate stream");
            value |= static_cast<uint32_t>(data_[position_++]) << bitCount_;
            bitCount_ += 8;
        }
        bitBuffer_ = value >> need;
        bitCount_ -= need;
        return static_cast<int>(value & ((1u << need) - 1));
    }

    void Stored()
    {
        bitBuffer_ = 0;
        bitCount_ = 0;
        if (position_ + 4 > size_)
            throw std::runtime_error("truncated stored deflate block");
        const unsigned length = data_[position_] | (data_[position_ + 1] << 8);
        const unsigned check = data_[position_ + 2] | (data_[position_ + 3] << 8);
        position_ += 4;
        if (length != (~check & 0xFFFF) || position_ + length > size_)
            throw std::runtime_error("invalid stored deflate block");
        out_.insert(out_.end(), data_ + position_, data_ + position_ + length);
        position_ += length;
    }

    int Decode(const Huffman &h)
    {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length <= 15; ++length)
        {
            code |= Bits(1);
            const int count = h.count[length];
            if (code - count < first)
                return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("invalid deflate Huffman code");
    }

    static void Construct(Huffman &h, const int16_t *length, const int n)
    {
        h.count.fill(0);
        for (int symbol = 0; symbol < n; ++symbol)
            ++h.count[length[symbol]];
        std::array<int16_t, 16> offsets{};
        for (int len = 1; len < 15; ++len)
            offsets[len + 1] = static_cast<int16_t>(offsets[len] + h.count[len]);
        for (int symbol = 0; symbol < n; ++symbol)
            if (length[symbol] != 0)
                h.symbol[offsets[length[symbol]]++] = static_cast<int16_t>(symbol);
    }

    void Codes(const Huffman &lengthCode, const Huffman &distanceCode)
    {
        static constexpr int16_t lengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                                   15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                                   67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr int16_t lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr int16_t distanceBase[30] = {
            1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
            193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr int16_t distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;)
        {
            int symbol = Decode(lengthCode);
            if (symbol < 256)
            {
                out_.push_back(static_cast<uint8_t>(symbol));
                continue;
            }
            if (symbol == 256)
                return;
            symbol -= 257;
            if (symbol >= 29)
                throw std::runtime_error("invalid deflate length symbol");
            const size_t length = lengthBase[symbol] + Bits(lengthExtra[symbol]);
            const int distanceSymbol = Decode(distanceCode);
            if (distanceSymbol >= 30)
                throw std::runtime_error("invalid deflate distance symbol");
            const size_t distance = distanceBase[distanceSymbol] + Bits(distanceExtra[distanceSymbol]);
            if (distance > out_.size())
                throw std::runtime_error("deflate distance exceeds output");
            const size_t from = out_.size() - distance;
            for (size_t i = 0; i < length; ++i)
                out_.push_back(out_[from + i]);
        }
    }

    void Fixed()
    {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> result;
            int16_t lengths[288];
            for (int symbol = 0; symbol < 144; ++symbol)
                lengths[symbol] = 8;
            for (int symbol = 144; symbol < 256; ++symbol)
                lengths[symbol] = 9;
            for (int symbol = 256; symbol < 280; ++symbol)
                lengths[symbol] = 7;
            for (int symbol = 280; symbol < 288; ++symbol)
                lengths[symbol] = 8;
            Construct(result.first, lengths, 288);
            for (int symbol = 0; symbol < 30; ++symbol)
                lengths[symbol] = 5;
            Construct(result.second, lengths, 30);
            return result;
        }();
        Codes(tables.first, tables.second);
    }

    void Dynamic()
    {
        static constexpr int16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int lengthCount = Bits(5) + 257;
        const int distanceCount = Bits(5) + 1;
        const int codeCount = Bits(4) + 4;
        if (lengthCount > 286 || distanceCount > 30)
            throw std::runtime_error("invalid dynamic deflate counts");
        int16_t lengths[320]{};
        for (int i = 0; i < codeCount; ++i)
            lengths[order[i]] = static_cast<int16_t>(Bits(3));
        Huffman codeLengths;
        Construct(codeLengths, lengths, 19);
        int index = 0;
        while (index < lengthCount + distanceCount)
        {
            int symbol = Decode(codeLengths);
            if (symbol < 16)
            {
                lengths[index++] = static_cast<int16_t>(symbol);
                continue;
            }
            int16_t repeat = 0;
            int times = 0;
            if (symbol == 16)
            {
                if (index == 0)
                    throw std::runtime_error("deflate repeat without a previous length");
                repeat = lengths[index - 1];
                times = 3 + Bits(2);
            }
            else if (symbol == 17)
                times = 3 + Bits(3);
            else
                times = 11 + Bits(7);
            if (index + times > lengthCount + distanceCount)
                throw std::runtime_error("deflate code lengths overflow");
            while (times--)
                lengths[index++] = repeat;
        }
        Huffman lengthCode, distanceCode;
        Construct(lengthCode, lengths, lengthCount);
        Construct(distanceCode, lengths + lengthCount, distanceCount);
        Codes(lengthCode, distanceCode);
    }

    const uint8_t *data_;
    size_t size_;
    size_t position_{};
    uint32_t bitBuffer_{};
    int bitCount_{};
    std::vector<uint8_t> &out_;
};

uint32_t Crc32(const std::vector<uint8_t> &data)
{
    static const auto table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            result[i] = c;
        }
        return result;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : data)
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------------ IWD
class Iwd
{
  public:
    explicit Iwd(const std::filesystem::path &path)
        : path_(path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot open " + path.string());
        file.seekg(0, std::ios::end);
        const uint64_t size = static_cast<uint64_t>(file.tellg());
        const uint64_t tail = std::min<uint64_t>(size, 65536 + 22);
        std::vector<uint8_t> end(static_cast<size_t>(tail));
        file.seekg(static_cast<std::streamoff>(size - tail));
        file.read(reinterpret_cast<char *>(end.data()), static_cast<std::streamsize>(end.size()));
        size_t eocd = std::string::npos;
        for (size_t i = end.size() >= 22 ? end.size() - 22 : 0; i != size_t(-1); --i)
            if (Read32(end, i) == 0x06054B50u)
            {
                eocd = i;
                break;
            }
        if (eocd == std::string::npos)
            throw std::runtime_error("no zip directory in " + path.string());
        const uint32_t directorySize = Read32(end, eocd + 12);
        const uint32_t directoryOffset = Read32(end, eocd + 16);
        std::vector<uint8_t> directory(directorySize);
        file.seekg(directoryOffset);
        file.read(reinterpret_cast<char *>(directory.data()), directorySize);
        if (!file)
            throw std::runtime_error("cannot read zip directory of " + path.string());
        for (size_t at = 0; at + 46 <= directory.size();)
        {
            if (Read32(directory, at) != 0x02014B50u)
                break;
            Member member;
            member.method = Read16(directory, at + 10);
            member.crc = Read32(directory, at + 16);
            member.compressed = Read32(directory, at + 20);
            member.size = Read32(directory, at + 24);
            const uint16_t nameLength = Read16(directory, at + 28);
            const uint16_t extraLength = Read16(directory, at + 30);
            const uint16_t commentLength = Read16(directory, at + 32);
            member.header = Read32(directory, at + 42);
            std::string name(reinterpret_cast<const char *>(directory.data() + at + 46), nameLength);
            std::replace(name.begin(), name.end(), '\\', '/');
            members_[Lower(name)] = member;
            at += 46 + nameLength + extraLength + commentLength;
        }
    }

    bool Contains(const std::string &name) const
    {
        return members_.contains(name);
    }

    std::vector<std::string> Names() const
    {
        std::vector<std::string> names;
        for (const auto &[name, member] : members_)
            names.push_back(name);
        return names;
    }

    std::vector<uint8_t> Read(const std::string &name) const
    {
        const Member &member = members_.at(name);
        std::ifstream file(path_, std::ios::binary);
        std::vector<uint8_t> local(30);
        file.seekg(member.header);
        file.read(reinterpret_cast<char *>(local.data()), 30);
        if (!file || Read32(local, 0) != 0x04034B50u)
            throw std::runtime_error("bad zip member header for " + name);
        file.seekg(member.header + 30 + Read16(local, 26) + Read16(local, 28));
        std::vector<uint8_t> packed(member.compressed);
        file.read(reinterpret_cast<char *>(packed.data()), static_cast<std::streamsize>(packed.size()));
        if (!file)
            throw std::runtime_error("cannot read zip member " + name);
        std::vector<uint8_t> data;
        if (member.method == 0)
            data = std::move(packed);
        else if (member.method == 8)
        {
            data.reserve(member.size);
            Inflater(packed.data(), packed.size(), data).Run();
        }
        else
            throw std::runtime_error("unsupported zip method for " + name);
        if (data.size() != member.size || Crc32(data) != member.crc)
            throw std::runtime_error("zip member failed its CRC check: " + name);
        return data;
    }

  private:
    struct Member
    {
        uint16_t method{};
        uint32_t crc{};
        uint32_t compressed{};
        uint32_t size{};
        uint32_t header{};
    };
    static uint16_t Read16(const std::vector<uint8_t> &b, const size_t at)
    {
        return static_cast<uint16_t>(b.at(at) | (b.at(at + 1) << 8));
    }
    static uint32_t Read32(const std::vector<uint8_t> &b, const size_t at)
    {
        return Read16(b, at) | (static_cast<uint32_t>(Read16(b, at + 2)) << 16);
    }
    std::filesystem::path path_;
    std::unordered_map<std::string, Member> members_;
};

// ------------------------------------------------------------------ WAV
struct Pcm
{
    uint32_t rate{};
    uint8_t channels{};
    std::vector<int16_t> samples;
};

std::optional<Pcm> ReadWav(const std::vector<uint8_t> &file, std::string &error)
{
    const auto read16 = [&](const size_t at) { return static_cast<uint16_t>(file[at] | (file[at + 1] << 8)); };
    const auto read32 = [&](const size_t at) { return read16(at) | (static_cast<uint32_t>(read16(at + 2)) << 16); };
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) || std::memcmp(file.data() + 8, "WAVE", 4))
    {
        error = "not a RIFF WAVE file";
        return std::nullopt;
    }
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = nullptr;
    size_t dataSize = 0;
    for (size_t at = 12; at + 8 <= file.size();)
    {
        const uint32_t size = read32(at + 4);
        const size_t body = at + 8;
        const size_t available = std::min<size_t>(size, file.size() - body);
        if (!std::memcmp(file.data() + at, "fmt ", 4) && available >= 16)
        {
            format = read16(body);
            channels = read16(body + 2);
            rate = read32(body + 4);
            bits = read16(body + 14);
            if (format == 0xFFFE && available >= 26)
                format = read16(body + 24);
        }
        else if (!std::memcmp(file.data() + at, "data", 4))
        {
            data = file.data() + body;
            dataSize = available;
        }
        at = body + size + (size & 1);
    }
    if (!data || !channels || channels > 2 || !rate)
    {
        error = "missing fmt/data chunk or unsupported channel count";
        return std::nullopt;
    }
    Pcm pcm;
    pcm.rate = rate;
    pcm.channels = static_cast<uint8_t>(channels);
    const size_t width = bits / 8;
    if (!width || (format == 1 && width > 3) || (format == 3 && bits != 32) || (format != 1 && format != 3))
    {
        error = "unsupported WAV encoding (format " + std::to_string(format) + ", " +
                std::to_string(bits) + " bits)";
        return std::nullopt;
    }
    const size_t count = dataSize / width / channels * channels;
    pcm.samples.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        const uint8_t *p = data + i * width;
        int32_t value = 0;
        if (format == 3)
        {
            float f;
            std::memcpy(&f, p, 4);
            value = static_cast<int32_t>(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f));
        }
        else if (width == 1)
            value = (static_cast<int32_t>(p[0]) - 128) << 8;
        else if (width == 2)
            value = static_cast<int16_t>(p[0] | (p[1] << 8));
        else
            value = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                         (static_cast<uint32_t>(p[2]) << 24)) >> 16;
        pcm.samples[i] = static_cast<int16_t>(value);
    }
    if (pcm.samples.empty())
    {
        error = "WAV has no samples";
        return std::nullopt;
    }
    return pcm;
}

class SoundFiles
{
  public:
    SoundFiles(const std::filesystem::path &exportRoot, const std::vector<std::filesystem::path> &searchPaths)
    {
        roots_.push_back(exportRoot / "sound");
        for (const auto &path : searchPaths)
            roots_.push_back(path / "sound");
        for (const auto &path : searchPaths)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(path, error))
                continue;
            std::vector<std::filesystem::path> archives;
            for (const auto &entry : std::filesystem::directory_iterator(path, error))
                if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".iwd")
                    archives.push_back(entry.path());
            // IW3 searches later archives first (iw_13 overrides iw_00).
            std::sort(archives.rbegin(), archives.rend());
            for (const auto &archive : archives)
            {
                try
                {
                    iwds_.push_back(std::make_unique<Iwd>(archive));
                }
                catch (const std::exception &failure)
                {
                    zt::warn("iw3 sound: skipped %s: %s", archive.string().c_str(), failure.what());
                }
            }
        }
    }

    std::optional<std::vector<uint8_t>> Find(const std::string &path, std::string &source) const
    {
        for (const auto &root : roots_)
        {
            std::vector<uint8_t> bytes;
            std::error_code error;
            const auto candidate = root / path;
            if (std::filesystem::is_regular_file(candidate, error) && zt::read_file(candidate.string(), bytes))
            {
                source = candidate.string();
                return bytes;
            }
        }
        const std::string member = "sound/" + path;
        for (const auto &iwd : iwds_)
            if (iwd->Contains(member))
            {
                source = member + " (IWD)";
                return iwd->Read(member);
            }
        return std::nullopt;
    }

  private:
    std::vector<std::filesystem::path> roots_;
    std::vector<std::unique_ptr<Iwd>> iwds_;
};

// --------------------------------------------------------------- aliases
struct Iw3Alias
{
    std::string file; // relative to sound/
    float volMin{1}, volMax{1};
    float pitchMin{1}, pitchMax{1};
    float distMin{120}, distMax{1000};
    float probability{1};
    float lfe{};
    int startDelay{};
    bool looping{};
    std::string secondary;
    std::string curve; // IW3 volume falloff curve (soundaliases/<curve>.vfcurve)
};

std::map<std::string, std::vector<Iw3Alias>> ReadZoneAliases(const std::filesystem::path &exportRoot)
{
    std::map<std::string, std::vector<Iw3Alias>> result;
    const auto directory = exportRoot / "soundaliases";
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return result;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error))
    {
        if (!entry.is_regular_file() || !entry.path().string().ends_with(".iw3.json"))
            continue;
        std::string text;
        if (!zt::read_file_str(entry.path().string(), text))
            continue;
        const Json json = Json::parse(text, nullptr, false);
        if (json.is_discarded() || !json.contains("aliases"))
            continue;
        const std::string name = Lower(json.value("name", std::string{}));
        for (const Json &source : json.at("aliases"))
        {
            Iw3Alias alias;
            const Json &file = source.at("file");
            alias.file = file.value("name", std::string{});
            if (file.contains("directory") && file.at("directory").is_string())
                alias.file = file.at("directory").get<std::string>() + "/" + alias.file;
            alias.file = NormalizeSoundPath(alias.file);
            alias.volMin = source.at("volume").value("min", 1.0f);
            alias.volMax = source.at("volume").value("max", 1.0f);
            alias.pitchMin = source.at("pitch").value("min", 1.0f);
            alias.pitchMax = source.at("pitch").value("max", 1.0f);
            alias.distMin = source.at("distance").value("min", 120.0f);
            alias.distMax = source.at("distance").value("max", 1000.0f);
            alias.probability = source.value("probability", 1.0f);
            alias.lfe = source.value("lfe_percentage", 0.0f);
            alias.startDelay = source.value("start_delay", 0);
            alias.looping = (source.value("flags", 0) & 1) != 0;
            if (source.contains("secondary_alias") && source.at("secondary_alias").is_string())
                alias.secondary = Lower(source.at("secondary_alias").get<std::string>());
            if (source.contains("volume_falloff_curve") && source.at("volume_falloff_curve").is_string())
            {
                // Curves from another zone carry IW3's ',' reference prefix.
                alias.curve = Lower(source.at("volume_falloff_curve").get<std::string>());
                while (!alias.curve.empty() && alias.curve.front() == ',')
                    alias.curve.erase(alias.curve.begin());
            }
            if (!alias.file.empty())
                result[name].push_back(std::move(alias));
        }
    }
    return result;
}

std::vector<std::string> SplitCsv(const std::string &line)
{
    std::vector<std::string> fields;
    std::string field;
    for (const char c : line)
    {
        if (c == ',')
        {
            fields.push_back(Trim(field));
            field.clear();
        }
        else
            field += c;
    }
    fields.push_back(Trim(field));
    return fields;
}

// IW3 raw soundaliases/*.csv rows that apply to an MP map: a blank loadspec
// applies everywhere, a list only to the named levels ("all_mp" = every MP map)
// and a "!"-list to every level not named.
std::map<std::string, std::vector<Iw3Alias>> ReadCsvAliases(const std::vector<std::filesystem::path> &searchPaths,
                                                            const std::string &map)
{
    std::map<std::string, std::vector<Iw3Alias>> result;
    for (const auto &root : searchPaths)
    {
        const auto directory = root / "soundaliases";
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
            continue;
        std::map<std::string, float> volumeMods;
        {
            std::string text;
            if (zt::read_file_str((directory / "volumemodgroups.def").string(), text))
            {
                std::istringstream lines(text);
                std::string line;
                while (std::getline(lines, line))
                {
                    std::istringstream words(line);
                    std::string key;
                    float value = 0;
                    if (words >> key >> value && !key.starts_with("//"))
                        volumeMods[Lower(key)] = value;
                }
            }
        }
        for (const auto &entry : std::filesystem::directory_iterator(directory, error))
        {
            if (!entry.is_regular_file() || Lower(entry.path().extension().string()) != ".csv")
                continue;
            std::string text;
            if (!zt::read_file_str(entry.path().string(), text))
                continue;
            std::istringstream lines(text);
            std::string line;
            std::map<std::string, size_t> columns;
            while (std::getline(lines, line))
            {
                const auto fields = SplitCsv(line);
                if (fields.empty() || fields[0].empty() || fields[0].starts_with("#") || fields[0].starts_with("//"))
                    continue;
                if (Lower(fields[0]) == "name")
                {
                    columns.clear();
                    for (size_t i = 0; i < fields.size(); ++i)
                        columns[Lower(fields[i])] = i;
                    continue;
                }
                if (columns.empty())
                    continue;
                const auto get = [&](const char *column) -> std::string {
                    const auto found = columns.find(column);
                    return found != columns.end() && found->second < fields.size() ? fields[found->second] : std::string{};
                };
                const auto number = [&](const char *column, const float fallback) {
                    const std::string value = get(column);
                    if (value.empty())
                        return fallback;
                    try
                    {
                        return std::stof(value);
                    }
                    catch (...)
                    {
                        return fallback;
                    }
                };
                std::istringstream specWords(Lower(get("loadspec")));
                std::vector<std::string> spec;
                for (std::string word; specWords >> word;)
                    spec.push_back(word);
                bool applies = spec.empty();
                if (!spec.empty())
                {
                    const bool negated = spec.front().starts_with("!");
                    bool named = false;
                    for (std::string word : spec)
                    {
                        if (word.starts_with("!"))
                            word.erase(0, 1);
                        named = named || word == map || word == "all_mp";
                    }
                    applies = negated ? !named : named;
                }
                if (!applies || get("file").empty())
                    continue;
                Iw3Alias alias;
                alias.file = NormalizeSoundPath(get("file"));
                const auto mod = volumeMods.find(Lower(get("vol_mod")));
                const float scale = mod != volumeMods.end() ? mod->second : 1.0f;
                alias.volMin = number("vol_min", 1.0f) * scale;
                alias.volMax = number("vol_max", alias.volMin / scale) * scale;
                alias.pitchMin = number("pitch_min", 1.0f);
                alias.pitchMax = number("pitch_max", alias.pitchMin);
                alias.distMin = number("dist_min", 120.0f);
                alias.distMax = number("dist_max", 1000.0f);
                alias.probability = number("probability", 1.0f);
                alias.lfe = number("lfe percentage", 0.0f);
                alias.startDelay = static_cast<int>(number("startdelay", 0.0f));
                alias.looping = Lower(get("loop")).find("looping") != std::string::npos;
                alias.secondary = Lower(get("secondaryaliasname"));
                result[Lower(fields[0])].push_back(std::move(alias));
            }
        }
    }
    return result;
}

// Replay stock aliases whose fields the converted IW3 aliases share. Volumes
// scale the IW3 values to the level of the stock analog in the Replay mix.
enum class Role
{
    explosion,   // stock "o2_barrel_explode"
    fireBurst,   // stock "dst_propane_fire_whoosh"
    loopEmitter, // stock "vehicle_fire_small" (IW8's port of the IW3 alias)
    breakable,   // stock "dst_personal_computer"
};

struct RoleTemplate
{
    uint32_t flags; // bit19 = precached (G_CStringMP_SoundAliasIndex), bits 10..17 channel
    int32_t volModIndex;
    const char *curve;
    float reverb, farReverb;
    float spreadMin, spreadMax, spreadMid;
    uint8_t spreadAngle;
    float gain;
};

const RoleTemplate &TemplateFor(const Role role)
{
    static const RoleTemplate explosion{0x92808, 38, "standard_concave", 0.0f, 1.0f, 200, 600, 600, 60, 1.0f};
    static const RoleTemplate fireBurst{0x92400, 37, "standard_concave", 1.0f, 1.0f, 50, 300, 300, 30, 0.35f};
    static const RoleTemplate loopEmitter{0x2A003, 75, "default", 1.0f, 1.0f, 0, 0, 0, 0, 0.18f};
    static const RoleTemplate breakable{0x92400, 36, "default", 1.0f, 1.0f, 0, 0, 0, 0, 0.45f};
    switch (role)
    {
    case Role::explosion:
        return explosion;
    case Role::fireBurst:
        return fireBurst;
    case Role::loopEmitter:
        return loopEmitter;
    default:
        return breakable;
    }
}

Role RoleFor(const std::string &name, const bool looping)
{
    if (looping)
        return Role::loopEmitter;
    const auto has = [&](const char *word) { return name.find(word) != std::string::npos; };
    if (has("glass") || has("break") || has("shatter") || has("tv_") || has("door"))
        return Role::breakable;
    if (has("fire") || has("flare") || has("flame") || has("burn"))
        return Role::fireBurst;
    return Role::explosion;
}

// Stock Replay map-bank curves (mp_shipment.all vfCurves), copied by value.
// IW3 SNDCURVE text: "SNDCURVE", the knot count, then "distance volume" knots,
// both normalized; Replay vfCurves use the same (distance, volume) points.
std::optional<iw8::sound::Curve> ReadIw3Curve(const std::filesystem::path &exportRoot,
                                              const std::string &name)
{
    std::string text;
    if (name.empty() || name.find_first_of("/\\.") != std::string::npos ||
        !zt::read_file_str((exportRoot / "soundaliases" / (name + ".vfcurve")).string(), text))
        return std::nullopt;
    std::istringstream input(text);
    std::string magic;
    std::size_t count = 0;
    if (!(input >> magic >> count) || magic != "SNDCURVE" || count < 2 || count > 16)
        return std::nullopt;
    iw8::sound::Curve curve{"mw120r_" + name, {}};
    float previous = -1.0f;
    for (std::size_t index = 0; index < count; ++index)
    {
        float distance{}, volume{};
        if (!(input >> distance >> volume) || !std::isfinite(distance) || !std::isfinite(volume) ||
            distance < 0.0f || distance > 1.0f || distance < previous || volume < 0.0f ||
            volume > 1.0f)
            return std::nullopt;
        previous = distance;
        curve.points.push_back({distance, volume});
    }
    return curve;
}

iw8::sound::Curve CurveNamed(const std::string &name)
{
    if (name == "standard_concave")
        return {name, {{0.0f, 1.0f}, {0.1047f, 0.6834f}, {0.2982f, 0.4123f}, {0.6039f, 0.1756f}, {1.0f, 0.0f}}};
    return {"default", {{0.0f, 1.0f}, {1.0f, 0.0f}}};
}
} // namespace

std::set<std::string> ReadScriptSoundNames(const std::filesystem::path &scriptDirectory)
{
    std::set<std::string> names;
    std::error_code error;
    if (!std::filesystem::is_directory(scriptDirectory, error))
        return names;
    static const std::regex direct(
        R"re(\b(?:playsound|playloopsound|playsoundasmaster|soundexists|ambientplay)\s*\(\s*"([^"]+)")re", std::regex::icase);
    static const std::regex positional(R"re(\bplaysoundatpos\s*\([^;]*?,\s*"([^"]+)"\s*\))re", std::regex::icase);
    // createfx loop sounds: ent.v[ "soundalias" ] = "alias";
    static const std::regex createfx(R"re(\[\s*"soundalias"\s*\]\s*=\s*"([^"]+)")re", std::regex::icase);
    for (const auto &entry : std::filesystem::directory_iterator(scriptDirectory, error))
    {
        if (Lower(entry.path().extension().string()) != ".gsc")
            continue;
        std::string text;
        if (!zt::read_file_str(entry.path().string(), text))
            continue;
        for (const std::regex *pattern : {&direct, &positional, &createfx})
            for (auto match = std::sregex_iterator(text.begin(), text.end(), *pattern);
                 match != std::sregex_iterator(); ++match)
                names.insert(Lower((*match)[1].str()));
    }
    return names;
}

std::optional<ConvertedSounds> Convert(const std::filesystem::path &exportRoot,
                                       const std::vector<std::filesystem::path> &searchPaths,
                                       const std::string &map, const std::set<std::string> &requested)
{
    if (requested.empty())
        return std::nullopt;
    const auto zoneAliases = ReadZoneAliases(exportRoot);
    const auto csvAliases = ReadCsvAliases(searchPaths, Lower(map));
    const auto definition = [&](const std::string &name) -> const std::vector<Iw3Alias> * {
        if (const auto found = zoneAliases.find(name); found != zoneAliases.end())
            return &found->second;
        if (const auto found = csvAliases.find(name); found != csvAliases.end())
            return &found->second;
        return nullptr;
    };

    // Requested aliases and their secondary chains (IW3 plays each secondary
    // alongside its primary; Replay follows SndAlias.secondaryId the same way).
    std::vector<std::string> order;
    std::set<std::string> visited;
    std::vector<std::string> pending(requested.begin(), requested.end());
    while (!pending.empty())
    {
        const std::string name = Lower(pending.back());
        pending.pop_back();
        if (!visited.insert(name).second)
            continue;
        const auto *aliases = definition(name);
        if (!aliases)
        {
            zt::warn("iw3 sound: alias %s has no IW3 definition; left silent", name.c_str());
            continue;
        }
        order.push_back(name);
        for (const auto &alias : *aliases)
            if (!alias.secondary.empty())
                pending.push_back(alias.secondary);
    }

    SoundFiles files(exportRoot, searchPaths);
    std::map<std::string, iw8::sound::Sample> samples; // by IW3 path
    std::map<std::string, bool> missing;
    const std::string prefix = "mw120r\\" + Lower(map) + "\\";
    const auto sampleFor = [&](const std::string &path, const bool looping) -> const iw8::sound::Sample * {
        if (auto found = samples.find(path); found != samples.end())
        {
            found->second.looping = found->second.looping || looping;
            return &found->second;
        }
        if (missing.contains(path))
            return nullptr;
        std::string source, failure;
        const auto bytes = files.Find(path, source);
        std::optional<Pcm> pcm;
        if (bytes)
            pcm = ReadWav(*bytes, failure);
        else
            failure = "not found in the extracted zone, raw sound folders or IWDs";
        if (!pcm)
        {
            zt::warn("iw3 sound: %s: %s", path.c_str(), failure.c_str());
            missing[path] = true;
            return nullptr;
        }
        iw8::sound::Sample sample;
        std::string stem = path.ends_with(".wav") ? path.substr(0, path.size() - 4) : path;
        std::replace(stem.begin(), stem.end(), '/', '\\');
        sample.name = prefix + stem;
        sample.rate = pcm->rate;
        sample.channels = pcm->channels;
        sample.looping = looping;
        sample.pcm = std::move(pcm->samples);
        zt::info("iw3 sound: %s <- %s (%u Hz, %u ch, %.2f s)", sample.name.c_str(), source.c_str(), sample.rate,
                 static_cast<unsigned>(sample.channels),
                 double(sample.pcm.size() / sample.channels) / sample.rate);
        return &samples.emplace(path, std::move(sample)).first->second;
    };

    // First pass: load audio so lists whose files are all missing drop out
    // (and are not used as secondaries).
    std::set<std::string> converted;
    for (const auto &name : order)
    {
        bool any = false;
        for (const auto &alias : *definition(name))
            any = sampleFor(alias.file, alias.looping) != nullptr || any;
        if (any)
            converted.insert(name);
        else
            zt::warn("iw3 sound: alias %s has no usable audio; left silent", name.c_str());
    }
    if (converted.empty())
        return std::nullopt;

    ConvertedSounds result;
    result.bank.name = Lower(map) + ".all";
    // The loader opens zone\<zone>.sabs (and patch_<zone>.sabs) even for a
    // fully resident bank and fails the whole bank when it cannot. These two
    // stock files are the 4096-byte "no streamed sounds" placeholders, so the
    // bank loads in every Replay install without shipping another file.
    result.bank.zone = "vehicle_engine_type4_npc";
    result.bank.streamKey = Lower(map) + "_all_loaded";
    std::set<std::string> curves{"default"};
    std::map<std::string, std::optional<iw8::sound::Curve>> iw3Curves;
    for (const auto &name : order)
    {
        if (!converted.contains(name))
            continue;
        iw8::sound::AliasList list;
        list.name = name;
        bool looping = false;
        for (const auto &source : *definition(name))
        {
            const auto *sample = sampleFor(source.file, source.looping);
            if (!sample)
                continue;
            looping = looping || source.looping;
            const RoleTemplate &role = TemplateFor(RoleFor(name, source.looping));
            iw8::sound::Alias alias;
            alias.assetName = sample->name;
            if (!source.secondary.empty() && converted.contains(source.secondary))
                alias.secondaryName = source.secondary;
            alias.flags = role.flags;
            alias.volModIndex = role.volModIndex;
            alias.volMin = std::clamp(source.volMin * role.gain, 0.0f, 1.0f);
            alias.volMax = std::clamp(std::max(source.volMax, source.volMin) * role.gain, 0.0f, 1.0f);
            alias.pitchMin = std::clamp(source.pitchMin, 0.25f, 4.0f);
            alias.pitchMax = std::clamp(std::max(source.pitchMax, source.pitchMin), 0.25f, 4.0f);
            alias.distMin = std::max(0.0f, source.distMin);
            alias.distMax = std::max(source.distMax, alias.distMin + 1.0f);
            alias.probability = source.probability > 0 ? source.probability : 1.0f;
            alias.lfePercentage = std::clamp(source.lfe, 0.0f, 1.0f);
            alias.startDelay = std::max(0, source.startDelay);
            // The alias's own IW3 falloff curve when it converts, else the role's.
            auto found = iw3Curves.find(source.curve);
            if (!source.curve.empty() && found == iw3Curves.end())
                found = iw3Curves.emplace(source.curve, ReadIw3Curve(exportRoot, source.curve)).first;
            if (!source.curve.empty() && found->second)
                alias.volumeFalloffCurve = iw8::sound::HashName(found->second->name);
            else
            {
                alias.volumeFalloffCurve = iw8::sound::HashName(role.curve);
                curves.insert(role.curve);
            }
            alias.reverbMultiplier = role.reverb;
            alias.farReverbMultiplier = role.farReverb;
            alias.stereoSpreadMinDist = role.spreadMin;
            alias.stereoSpreadMaxDist = role.spreadMax;
            alias.stereoSpreadMidPoint = role.spreadMid;
            alias.stereoSpreadMaxAngle = role.spreadAngle;
            const double seconds = double(sample->pcm.size() / sample->channels) / sample->rate;
            alias.maxDurationDsec = static_cast<int16_t>(std::clamp(std::ceil(seconds * 10.0), 1.0, 32767.0));
            list.aliases.push_back(std::move(alias));
        }
        result.aliases[name] = looping;
        zt::info("iw3 sound: alias %s -> %zu variant(s)%s", name.c_str(), list.aliases.size(),
                 looping ? " (looping)" : "");
        result.bank.lists.push_back(std::move(list));
    }
    for (const auto &name : curves)
        result.bank.volumeCurves.push_back(CurveNamed(name));
    std::size_t convertedCurves = 0;
    for (const auto &[name, curve] : iw3Curves)
        if (curve)
        {
            result.bank.volumeCurves.push_back(*curve);
            ++convertedCurves;
        }
        else
            zt::warn("iw3 sound: falloff curve %s did not convert; its aliases use the role curve",
                     name.c_str());
    if (convertedCurves)
        zt::info("iw3 sound: converted %zu IW3 falloff curve(s)", convertedCurves);
    std::vector<iw8::sound::Sample> ordered;
    for (auto &[path, sample] : samples)
        ordered.push_back(std::move(sample));
    result.bank.sab = iw8::sound::BuildSab(ordered, Lower(map));
    zt::info("iw3 sound: bank %s: %zu alias list(s), %zu sound(s), %zu-byte loaded SAB", result.bank.name.c_str(),
             result.bank.lists.size(), ordered.size(), result.bank.sab.size());
    // Diagnostic: IW8_ZONETOOL_SAB_DUMP=<file> keeps a copy for offline checks.
    if (const char *dump = std::getenv("IW8_ZONETOOL_SAB_DUMP"); dump && *dump)
    {
        std::ofstream file(dump, std::ios::binary);
        file.write(reinterpret_cast<const char *>(result.bank.sab.data()),
                   static_cast<std::streamsize>(result.bank.sab.size()));
        zt::info("iw3 sound: wrote SAB copy to %s", dump);
    }
    return result;
}
std::size_t ExtractIwdMembers(const std::filesystem::path &directory,
                              const std::vector<std::string> &prefixes,
                              const std::filesystem::path &destination)
{
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return 0;
    std::vector<std::filesystem::path> archives;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error))
        if (entry.is_regular_file() && Lower(entry.path().extension().string()) == ".iwd")
            archives.push_back(entry.path());
    // IW3 searches later archives first, so they extract first and win.
    std::sort(archives.rbegin(), archives.rend());
    std::size_t extracted = 0;
    for (const auto &archive : archives)
    {
        try
        {
            const Iwd iwd(archive);
            for (const auto &name : iwd.Names())
            {
                if (name.empty() || name.back() == '/' ||
                    std::ranges::none_of(prefixes, [&](const std::string &prefix) {
                        return name.starts_with(prefix);
                    }))
                    continue;
                // Archive member names are untrusted: keep each one inside
                // the destination.
                const std::filesystem::path relative(name);
                if (relative.is_absolute() || relative.has_root_name() ||
                    std::ranges::any_of(relative, [](const std::filesystem::path &part) {
                        return part == "..";
                    }))
                    continue;
                const auto target = destination / relative;
                if (std::filesystem::exists(target, error))
                    continue;
                std::filesystem::create_directories(target.parent_path(), error);
                const auto data = iwd.Read(name);
                std::ofstream file(target, std::ios::binary);
                file.write(reinterpret_cast<const char *>(data.data()),
                           static_cast<std::streamsize>(data.size()));
                if (file)
                    ++extracted;
            }
        }
        catch (const std::exception &failure)
        {
            zt::warn("iw3: skipped IWD %s: %s", archive.string().c_str(), failure.what());
        }
    }
    return extracted;
}
} // namespace iw3::sound
