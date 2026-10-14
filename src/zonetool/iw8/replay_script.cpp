#include "replay_script.h"

#include "iw8_zonebuffer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace iw8::replay_script
{
namespace
{
// Signed Replay 1.20 Backlot map-main stack/export evidence: first export id ref_0254.
constexpr uint32_t kStartupExport = 0x254;

// Replay 1.20's loader (ProcessScriptFile 0x141322350) takes a field operand as
// a compiled-in canonical string id only up to 0xE2C0. Above that it reads a
// stack entry: a nonzero u32 is used as the canonical id, 0 is followed by the
// name. Function-header and far-call names use the same {u32, name} entries.
constexpr uint32_t kReplayMaxCanonicalId = 0xE2C0;
// gsc-tool -g iw8 writes a stack name only above its own table (max_string_id),
// so a name it knows between the two limits ("stage" = 0x10FC6) had no entry.
// The loader then read the next entries one slot early and every later string
// in the script came out wrong (Nuketown fixes-30..32: minimap material names,
// the IW3 effect table keys).
constexpr uint32_t kGscToolMaxStringId = 0x1472F;

struct GscToken
{
    uint32_t id;
    std::string_view name;
};
#include "gsc_tool_iw8_tokens.inc"

std::string_view gscToolTokenName(const uint32_t id)
{
    const auto found = std::lower_bound(kGscToolIw8LateTokens.begin(), kGscToolIw8LateTokens.end(), id,
                                        [](const GscToken &token, const uint32_t value) { return token.id < value; });
    if (found == kGscToolIw8LateTokens.end() || found->id != id)
        throw std::invalid_argument("script names gsc-tool token " + std::to_string(id) +
                                    ", which Replay needs by name and the converter's table lacks");
    return found->name;
}

// Minimal zlib (RFC 1950/1951) decoder for gsc-tool's compressed stack.
class Inflater
{
  public:
    Inflater(const uint8_t *data, const size_t size) : data_(data), size_(size) {}

    std::vector<uint8_t> run()
    {
        if (size_ < 6 || (data_[0] & 0x0F) != 8 || ((data_[0] << 8) | data_[1]) % 31 != 0 || (data_[1] & 0x20))
            throw std::invalid_argument("script stack is not a zlib stream");
        at_ = 2;
        bool last = false;
        while (!last)
        {
            last = bits(1) != 0;
            switch (bits(2))
            {
            case 0: stored(); break;
            case 1: fixed(); break;
            case 2: dynamic(); break;
            default: throw std::invalid_argument("script stack has an invalid deflate block");
            }
        }
        if (at_ + 4 > size_)
            throw std::invalid_argument("script stack zlib stream is truncated");
        uint32_t a = 1, b = 0;
        for (const uint8_t byte : out_)
        {
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        const uint32_t expected = (uint32_t(data_[at_]) << 24) | (uint32_t(data_[at_ + 1]) << 16) |
                                  (uint32_t(data_[at_ + 2]) << 8) | data_[at_ + 3];
        if (((b << 16) | a) != expected)
            throw std::invalid_argument("script stack zlib checksum mismatch");
        return std::move(out_);
    }

  private:
    struct Huffman
    {
        std::array<uint16_t, 16> count{};
        std::array<uint16_t, 288> symbol{};
    };

    uint32_t bits(const int need)
    {
        uint64_t value = bitBuffer_;
        while (bitCount_ < need)
        {
            if (at_ >= size_)
                throw std::invalid_argument("script stack deflate data is truncated");
            value |= uint64_t(data_[at_++]) << bitCount_;
            bitCount_ += 8;
        }
        bitBuffer_ = value >> need;
        bitCount_ -= need;
        return uint32_t(value & ((uint64_t(1) << need) - 1));
    }

    static int construct(Huffman &table, const uint16_t *lengths, const int count)
    {
        table.count.fill(0);
        for (int symbol = 0; symbol < count; ++symbol)
            ++table.count[lengths[symbol]];
        if (table.count[0] == count)
            return 0;
        int left = 1;
        for (int length = 1; length < 16; ++length)
        {
            left = (left << 1) - table.count[length];
            if (left < 0)
                return left;
        }
        std::array<uint16_t, 16> offsets{};
        for (int length = 1; length < 15; ++length)
            offsets[length + 1] = uint16_t(offsets[length] + table.count[length]);
        for (int symbol = 0; symbol < count; ++symbol)
            if (lengths[symbol])
                table.symbol[offsets[lengths[symbol]]++] = uint16_t(symbol);
        return left;
    }

    int decode(const Huffman &table)
    {
        int code = 0, first = 0, index = 0;
        for (int length = 1; length < 16; ++length)
        {
            code |= int(bits(1));
            const int count = table.count[length];
            if (code - count < first)
                return table.symbol[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw std::invalid_argument("script stack has an invalid Huffman code");
    }

    void stored()
    {
        bitBuffer_ = 0;
        bitCount_ = 0;
        if (at_ + 4 > size_)
            throw std::invalid_argument("script stack stored block is truncated");
        const uint32_t length = data_[at_] | (data_[at_ + 1] << 8);
        const uint32_t inverse = data_[at_ + 2] | (data_[at_ + 3] << 8);
        at_ += 4;
        if (length != (~inverse & 0xFFFF) || at_ + length > size_)
            throw std::invalid_argument("script stack stored block is invalid");
        out_.insert(out_.end(), data_ + at_, data_ + at_ + length);
        at_ += length;
    }

    void codes(const Huffman &lengthCode, const Huffman &distanceCode)
    {
        static constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19,  23, 27,
                                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                     2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr uint16_t kDistanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                       33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;)
        {
            int symbol = decode(lengthCode);
            if (symbol < 256)
            {
                out_.push_back(uint8_t(symbol));
                continue;
            }
            if (symbol == 256)
                return;
            symbol -= 257;
            if (symbol >= 29)
                throw std::invalid_argument("script stack has an invalid deflate length");
            const size_t length = kLengthBase[symbol] + bits(kLengthExtra[symbol]);
            symbol = decode(distanceCode);
            if (symbol >= 30)
                throw std::invalid_argument("script stack has an invalid deflate distance");
            const size_t distance = kDistanceBase[symbol] + bits(kDistanceExtra[symbol]);
            if (distance > out_.size())
                throw std::invalid_argument("script stack deflate distance is too far back");
            for (size_t i = 0; i < length; ++i)
                out_.push_back(out_[out_.size() - distance]);
        }
    }

    void fixed()
    {
        std::array<uint16_t, 288 + 30> lengths{};
        std::fill(lengths.begin(), lengths.begin() + 144, uint16_t(8));
        std::fill(lengths.begin() + 144, lengths.begin() + 256, uint16_t(9));
        std::fill(lengths.begin() + 256, lengths.begin() + 280, uint16_t(7));
        std::fill(lengths.begin() + 280, lengths.begin() + 288, uint16_t(8));
        std::fill(lengths.begin() + 288, lengths.end(), uint16_t(5));
        Huffman lengthCode, distanceCode;
        construct(lengthCode, lengths.data(), 288);
        construct(distanceCode, lengths.data() + 288, 30);
        codes(lengthCode, distanceCode);
    }

    void dynamic()
    {
        static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int lengthCount = int(bits(5)) + 257;
        const int distanceCount = int(bits(5)) + 1;
        const int codeCount = int(bits(4)) + 4;
        if (lengthCount > 286 || distanceCount > 30)
            throw std::invalid_argument("script stack has invalid deflate code counts");
        std::array<uint16_t, 286 + 30> lengths{};
        for (int index = 0; index < codeCount; ++index)
            lengths[kOrder[index]] = uint16_t(bits(3));
        Huffman lengthCode, distanceCode;
        if (construct(lengthCode, lengths.data(), 19) != 0)
            throw std::invalid_argument("script stack has an invalid code-length code");
        int index = 0;
        while (index < lengthCount + distanceCount)
        {
            int symbol = decode(lengthCode);
            if (symbol < 16)
            {
                lengths[index++] = uint16_t(symbol);
                continue;
            }
            uint16_t repeated = 0;
            if (symbol == 16)
            {
                if (index == 0)
                    throw std::invalid_argument("script stack repeats a missing code length");
                repeated = lengths[index - 1];
                symbol = 3 + int(bits(2));
            }
            else
                symbol = symbol == 17 ? 3 + int(bits(3)) : 11 + int(bits(7));
            if (index + symbol > lengthCount + distanceCount)
                throw std::invalid_argument("script stack code lengths overflow");
            while (symbol--)
                lengths[index++] = repeated;
        }
        if (lengths[256] == 0)
            throw std::invalid_argument("script stack block has no end code");
        int error = construct(lengthCode, lengths.data(), lengthCount);
        if (error && (error < 0 || lengthCount != lengthCode.count[0] + lengthCode.count[1]))
            throw std::invalid_argument("script stack has an incomplete length code");
        error = construct(distanceCode, lengths.data() + lengthCount, distanceCount);
        if (error && (error < 0 || distanceCount != distanceCode.count[0] + distanceCode.count[1]))
            throw std::invalid_argument("script stack has an incomplete distance code");
        codes(lengthCode, distanceCode);
    }

    const uint8_t *data_;
    size_t size_;
    size_t at_ = 0;
    uint64_t bitBuffer_ = 0;
    int bitCount_ = 0;
    std::vector<uint8_t> out_;
};

class StackReader
{
  public:
    explicit StackReader(const std::vector<uint8_t> &data) : data_(data) {}

    uint32_t u32()
    {
        if (at_ + 4 > data_.size())
            throw std::invalid_argument("script stack ends early");
        uint32_t value = 0;
        std::memcpy(&value, data_.data() + at_, 4);
        at_ += 4;
        return value;
    }

    std::string_view cstr()
    {
        if (at_ >= data_.size())
            throw std::invalid_argument("script stack ends early");
        const auto *begin = data_.data() + at_;
        const auto *end = static_cast<const uint8_t *>(std::memchr(begin, 0, data_.size() - at_));
        if (!end)
            throw std::invalid_argument("script stack string is unterminated");
        at_ += size_t(end - begin) + 1;
        return {reinterpret_cast<const char *>(begin), size_t(end - begin)};
    }

    size_t position() const { return at_; }

  private:
    const std::vector<uint8_t> &data_;
    size_t at_ = 0;
};

bool isFieldTokenOpcode(const uint8_t opcode)
{
    switch (opcode)
    {
    case 4: case 7: case 8: case 24: case 37: case 44: case 56: case 60: case 76: case 109: case 123: case 141:
        return true;
    default:
        return false;
    }
}

bool isFarCallOpcode(const uint8_t opcode)
{
    switch (opcode)
    {
    case 5: case 23: case 101: case 138: case 96: case 117: case 146: case 150:
        return true;
    default:
        return false;
    }
}

void appendU32(std::vector<uint8_t> &out, const uint32_t value)
{
    const size_t offset = out.size();
    out.resize(offset + sizeof(value));
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

void appendString(std::vector<uint8_t> &out, const std::string &value)
{
    if (value.find('\0') != std::string::npos)
        throw std::invalid_argument("Replay startup script string contains a NUL");
    out.insert(out.end(), value.begin(), value.end());
    out.push_back(0);
}

void appendF32(std::vector<uint8_t> &out, const float value)
{
    const size_t offset = out.size();
    out.resize(offset + sizeof(value));
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

// Operand bytes after each opcode, as Replay 1.20's script loader walks them
// (ProcessScriptFile 0x141322350).
size_t replayOperandSize(const std::vector<uint8_t> &code, const size_t at)
{
    switch (code[at])
    {
    case 1: case 3: case 20: case 22: case 40: case 48: case 52: case 68: case 69: case 70:
    case 71: case 81: case 82: case 91: case 98: case 99: case 104: case 107: case 113:
    case 116: case 140: case 142: case 147: case 161: case 185: case 186: case 187: case 188:
        return 1;
    case 11: case 14: case 25: case 27: case 28: case 29: case 30: case 31: case 32: case 33:
    case 34: case 50: case 80: case 85: case 102: case 114: case 126: case 127: case 128:
    case 129: case 130: case 131:
        return 2;
    case 35: case 51: case 79: case 96: case 117: case 132: case 137: case 146: case 150:
    case 152: case 169: case 170: case 171: case 172: case 177: case 178: case 179: case 180:
        return 3;
    case 4: case 5: case 7: case 8: case 23: case 24: case 37: case 41: case 44: case 53:
    case 56: case 60: case 72: case 73: case 76: case 95: case 101: case 109: case 110:
    case 112: case 122: case 123: case 134: case 138: case 141: case 148: case 173: case 174:
    case 175: case 176: case 181: case 182: case 183: case 184:
        return 4;
    case 145:
        return 8;
    case 160:
        return 5;
    case 19:
        return 12;
    case 166: // FormalParams, 189
    case 189:
        if (at + 1 >= code.size())
            throw std::invalid_argument("script bytecode ends inside an operand");
        return size_t(1) + code[at + 1];
    case 86: // endswitch: count, then {4-byte value, 3-byte offset} per case
        if (at + 2 >= code.size())
            throw std::invalid_argument("script bytecode ends inside an operand");
        return 2 + size_t(7) * (code[at + 1] | (code[at + 2] << 8));
    default:
        return 0;
    }
}

// Local function call, thread call and function-reference opcodes. Their
// 3-byte target offset (relative to the operand) is stored doubled in Replay.
bool isLocalCodeReference(const uint8_t opcode)
{
    switch (opcode)
    {
    case 0x29: case 0x33: case 0x35: case 0x48: case 0x4F: case 0x6E: case 0x89: case 0x98:
        return true;
    default:
        return false;
    }
}

int32_t readS24(const uint8_t *at)
{
    int32_t value = at[0] | (at[1] << 8) | (at[2] << 16);
    return value & 0x800000 ? value - 0x1000000 : value;
}

void writeS24(uint8_t *at, const int32_t value)
{
    if (value < -0x800000 || value > 0x7FFFFF)
        throw std::length_error("Replay script code offset does not fit 24 bits");
    at[0] = uint8_t(value);
    at[1] = uint8_t(value >> 8);
    at[2] = uint8_t(value >> 16);
}

// Walks the stack in the loader's order. gsc-tool's stack is read with its own
// rules and written back with Replay's; with toReplay false the stack is only
// read with Replay's rules. Returns the stack bytes the walk consumed.
size_t walkScriptStack(const std::vector<uint8_t> &code, const std::vector<uint8_t> &stack, const bool toReplay,
                       std::vector<uint8_t> *out)
{
    StackReader in(stack);
    const auto writeString = [&](const std::string_view text) {
        if (!out)
            return;
        out->insert(out->end(), text.begin(), text.end());
        out->push_back(0);
    };
    const auto copyString = [&] { writeString(in.cstr()); };
    const auto copyU32 = [&] {
        const uint32_t value = in.u32();
        if (out)
            appendU32(*out, value);
        return value;
    };
    const auto writeName = [&](const std::string_view name) {
        if (out)
            appendU32(*out, 0);
        writeString(name);
    };
    // {u32 id, or 0 then the name}; an id Replay lacks is written by name.
    const auto copyName = [&] {
        const uint32_t id = in.u32();
        if (id == 0)
        {
            if (out)
                appendU32(*out, 0);
            copyString();
        }
        else if (toReplay && id > kReplayMaxCanonicalId)
            writeName(gscToolTokenName(id));
        else if (out)
            appendU32(*out, id);
    };

    size_t nextFunction = 1;
    for (size_t at = 1; at < code.size(); at += 1 + replayOperandSize(code, at))
    {
        if (at == nextFunction)
        {
            const uint32_t size = copyU32();
            if (size == 0 || size > code.size() - at)
                throw std::invalid_argument("script function size does not match the bytecode");
            copyName();
            nextFunction += size;
        }
        const uint8_t opcode = code[at];
        if (isFieldTokenOpcode(opcode))
        {
            uint32_t id = 0;
            std::memcpy(&id, &code[at + 1], 4);
            if (id <= kReplayMaxCanonicalId)
                continue;
            if (!toReplay || id > kGscToolMaxStringId)
                copyName();
            else
                writeName(gscToolTokenName(id));
        }
        else if (isFarCallOpcode(opcode))
        {
            // Script file id (a nonzero id names a numbered script) and function name.
            if (copyU32() == 0)
                copyString();
            copyName();
        }
        else if (opcode == 86)
        {
            const size_t cases = code[at + 1] | (code[at + 2] << 8);
            for (size_t index = 0; index < cases; ++index)
            {
                uint32_t value = 0;
                std::memcpy(&value, &code[at + 3 + 7 * index], 4);
                if (value < 0x80000)
                    copyString();
            }
        }
        else if (opcode == 95 || opcode == 122 || opcode == 98)
            copyString();
        else if (opcode == 145)
        {
            copyString(); // animtree
            copyString(); // animation
        }
        else if (opcode >= 177 && opcode <= 184)
            copyU32();
    }
    return in.position();
}

// Rewrites gsc-tool's stack in Replay's encoding and checks that the game's
// loader consumes exactly the result.
std::vector<uint8_t> toReplayStack(const std::vector<uint8_t> &code, const std::vector<uint8_t> &stack)
{
    std::vector<uint8_t> replay;
    replay.reserve(stack.size() + 256);
    if (walkScriptStack(code, stack, true, &replay) != stack.size())
        throw std::invalid_argument("gsc-tool's script stack does not match its bytecode");
    if (walkScriptStack(code, replay, false, nullptr) != replay.size())
        throw std::logic_error("rewritten script stack does not match Replay's loader");
    return replay;
}

void writeScriptHeader(ZoneWriter &writer, const uint32_t compressedLength,
                       const uint32_t stackLength, const uint32_t bytecodeLength)
{
    uint8_t header[0x28]{};
    std::memcpy(header + 0x00, &PTR_FOLLOWS, sizeof(PTR_FOLLOWS));
    std::memcpy(header + 0x08, &compressedLength, sizeof(compressedLength));
    std::memcpy(header + 0x0C, &stackLength, sizeof(stackLength));
    std::memcpy(header + 0x10, &bytecodeLength, sizeof(bytecodeLength));
    std::memcpy(header + 0x18, &PTR_FOLLOWS, sizeof(PTR_FOLLOWS));
    std::memcpy(header + 0x20, &PTR_FOLLOWS, sizeof(PTR_FOLLOWS));

    writer.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
    writer.align(7);
    writer.write(header, sizeof(header));
    writer.popStream();
}
} // namespace

// The native ScriptFile and RawFile assets store zlib-wrapped DEFLATE data.
// Stored blocks keep the converter self-contained while preserving the exact
// decompressed bytes.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t> &input)
{
    if (input.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("Replay zlib payload is too large");

    std::vector<uint8_t> output{0x78, 0x01};
    uint32_t adlerA = 1;
    uint32_t adlerB = 0;
    size_t cursor = 0;
    do
    {
        const size_t remaining = input.size() - cursor;
        const uint16_t count = static_cast<uint16_t>((remaining > 0xFFFF) ? 0xFFFF : remaining);
        const bool final = cursor + count == input.size();
        output.push_back(final ? 0x01 : 0x00);
        output.push_back(static_cast<uint8_t>(count));
        output.push_back(static_cast<uint8_t>(count >> 8));
        const uint16_t inverse = static_cast<uint16_t>(~count);
        output.push_back(static_cast<uint8_t>(inverse));
        output.push_back(static_cast<uint8_t>(inverse >> 8));
        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(cursor),
                      input.begin() + static_cast<std::ptrdiff_t>(cursor + count));
        for (size_t i = 0; i < count; ++i)
        {
            adlerA = (adlerA + input[cursor + i]) % 65521;
            adlerB = (adlerB + adlerA) % 65521;
        }
        cursor += count;
    } while (cursor != input.size());

    const uint32_t adler = (adlerB << 16) | adlerA;
    output.push_back(static_cast<uint8_t>(adler >> 24));
    output.push_back(static_cast<uint8_t>(adler >> 16));
    output.push_back(static_cast<uint8_t>(adler >> 8));
    output.push_back(static_cast<uint8_t>(adler));
    return output;
}

void emitCompassStartup(ZoneWriter &writer, const std::string &assetName,
                        const std::string &mapName, const float northwestX,
                        const float northwestY, const float southeastX,
                        const float southeastY)
{
    if (assetName.empty() || mapName.empty())
        throw std::invalid_argument("Replay startup script asset and map names are required");
    if (!std::isfinite(northwestX) || !std::isfinite(northwestY) ||
        !std::isfinite(southeastX) || !std::isfinite(southeastY))
        throw std::invalid_argument("Replay compass bounds must be finite");

    const std::string compass = "compass_map_" + mapName;

    // Replay reads worldspawn northyaw into configstring 9 before this
    // startup call. Its native span check expects the source northwest and
    // southeast corners in that rotated frame, in this order.
    const float firstX = northwestX;
    const float firstY = northwestY;
    const float secondX = southeastX;
    const float secondY = southeastY;

    // Invoke Replay's native seven-argument setminimap builtin (0x1D3).
    // Builtin calls consume direct arguments; PreScriptCall (0x15) is only
    // emitted before script calls and would leave a marker on the VM stack.
    // Values are pushed in reverse argument order by the IW8 VM.
    std::vector<uint8_t> bytecode{0x3B, 0x4B, 0x7A, 0x00, 0x00, 0x00, 0x00,
                                  0x16, 0x01, 0x70};
    appendF32(bytecode, secondY);
    bytecode.push_back(0x70);
    appendF32(bytecode, secondX);
    bytecode.push_back(0x70);
    appendF32(bytecode, firstY);
    bytecode.push_back(0x70);
    appendF32(bytecode, firstX);
    bytecode.insert(bytecode.end(), {0x7A, 0x00, 0x00, 0x00, 0x00,
                                     0x23, 0x07, 0xD3, 0x01, 0x58, 0x3B});
    const uint32_t functionSize = static_cast<uint32_t>(bytecode.size() - 1);

    std::vector<uint8_t> stack;
    stack.reserve(10 + compass.size() * 2);
    appendU32(stack, functionSize);
    appendU32(stack, kStartupExport);
    // Strings appear in bytecode execution order: alternate image, then primary.
    appendString(stack, compass);
    appendString(stack, compass);

    const auto compressed = zlibStored(stack);
    if (compressed.size() > std::numeric_limits<uint32_t>::max() ||
        stack.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("Replay startup script payload is too large");

    writeScriptHeader(writer, static_cast<uint32_t>(compressed.size()),
                      static_cast<uint32_t>(stack.size()),
                      static_cast<uint32_t>(bytecode.size()));

    writer.pushStream(XFILE_BLOCK_VIRTUAL);
    writer.writeStr(assetName);
    writer.popStream();

    writer.pushStream(XFILE_BLOCK_RUNTIME);
    writer.write(compressed.data(), compressed.size());
    writer.write(bytecode.data(), bytecode.size());
    writer.popStream();
}
std::vector<uint8_t> toReplayCodeOffsets(std::vector<uint8_t> code)
{
    if (code.empty())
        throw std::invalid_argument("empty script bytecode");
    // The loader starts after the leading byte; collect instruction starts first.
    std::vector<bool> instruction(code.size() + 1, false);
    size_t at = 1;
    while (at < code.size())
    {
        instruction[at] = true;
        at += 1 + replayOperandSize(code, at);
    }
    if (at != code.size())
        throw std::invalid_argument("script bytecode does not end on an instruction boundary");
    instruction[code.size()] = true;

    for (at = 1; at < code.size(); at += 1 + replayOperandSize(code, at))
    {
        const uint8_t opcode = code[at];
        if (isLocalCodeReference(opcode))
        {
            const int32_t offset = readS24(&code[at + 1]);
            const int64_t target = int64_t(at + 1) + offset;
            if (target < 1 || target >= int64_t(code.size()) || !instruction[size_t(target)] ||
                (code[size_t(target)] != 0x4B && code[size_t(target)] != 0xA6))
                throw std::invalid_argument("script call does not target a function start");
            writeS24(&code[at + 1], offset * 2);
        }
        else if (opcode == 86)
        {
            const size_t cases = code[at + 1] | (code[at + 2] << 8);
            for (size_t index = 0; index < cases; ++index)
            {
                uint8_t *operand = &code[at + 3 + 7 * index + 4];
                const int32_t offset = readS24(operand);
                const int64_t target = int64_t(operand - code.data()) + offset;
                if (target < 1 || target > int64_t(code.size()) || !instruction[size_t(target)])
                    throw std::invalid_argument("script switch case does not target an instruction");
                writeS24(operand, offset * 2);
            }
        }
    }
    return code;
}

void emitCompiledScript(ZoneWriter &writer, const std::string &assetName,
                        const std::vector<uint8_t> &gscbin)
{
    if (assetName.empty())
        throw std::invalid_argument("Replay script asset name is required");
    if (gscbin.size() < 16 || std::memcmp(gscbin.data(), "GSC\0", 4) != 0)
        throw std::invalid_argument("not a gsc-tool iw8 gscbin");
    uint32_t compressed = 0, stack = 0, bytecode = 0;
    std::memcpy(&compressed, gscbin.data() + 4, 4);
    std::memcpy(&stack, gscbin.data() + 8, 4);
    std::memcpy(&bytecode, gscbin.data() + 12, 4);
    if (size_t(16) + compressed + bytecode != gscbin.size() || !stack || !bytecode)
        throw std::invalid_argument("gscbin lengths do not match its payload");

    const std::vector<uint8_t> gscCode(gscbin.begin() + 16 + compressed,
                                       gscbin.begin() + 16 + compressed + bytecode);
    const std::vector<uint8_t> gscStack = Inflater(gscbin.data() + 16, compressed).run();
    if (gscStack.size() != stack)
        throw std::invalid_argument("gscbin stack length does not match its payload");
    const std::vector<uint8_t> replayStack = toReplayStack(gscCode, gscStack);
    const std::vector<uint8_t> replayCompressed = zlibStored(replayStack);
    const std::vector<uint8_t> code = toReplayCodeOffsets(gscCode);
    if (replayCompressed.size() > std::numeric_limits<uint32_t>::max() ||
        replayStack.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("Replay script payload is too large");

    writeScriptHeader(writer, static_cast<uint32_t>(replayCompressed.size()),
                      static_cast<uint32_t>(replayStack.size()), bytecode);
    writer.pushStream(XFILE_BLOCK_VIRTUAL);
    writer.writeStr(assetName);
    writer.popStream();
    writer.pushStream(XFILE_BLOCK_RUNTIME);
    writer.write(replayCompressed.data(), replayCompressed.size());
    writer.write(code.data(), code.size());
    writer.popStream();
}
} // namespace iw8::replay_script
