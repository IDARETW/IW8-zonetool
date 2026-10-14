#include "replay_sound.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>

namespace iw8::sound
{
namespace
{
template <class T> void Store(std::vector<uint8_t> &bytes, const size_t offset, const T value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// ------------------------------------------------------------------- FLAC
// Replay decodes codec 8 with a complete FLAC decoder: stock banks mix LPC,
// FIXED, CONSTANT and VERBATIM subframes and every stereo decorrelation mode.
// This encoder writes the stock framing (fixed 1024-sample blocks, 16-bit,
// explicit rate code, short final block) with FIXED predictors and
// partitioned Rice residuals, which every FLAC decoder accepts.
constexpr unsigned kBlockSamples = 1024;
constexpr unsigned kSeekInterval = 16384;
constexpr unsigned kMaxPartitionOrder = 6;
constexpr unsigned kMaxRiceParameter = 14; // 4-bit parameters; 15 is the escape code

class BitWriter
{
  public:
    explicit BitWriter(std::vector<uint8_t> &out)
        : out_(out)
    {
    }
    void Put(const uint32_t value, const unsigned count)
    {
        if (!count)
            return;
        const uint64_t masked = count == 32 ? value : (value & ((1u << count) - 1));
        accumulator_ = (accumulator_ << count) | masked;
        bits_ += count;
        while (bits_ >= 8)
        {
            bits_ -= 8;
            out_.push_back(static_cast<uint8_t>(accumulator_ >> bits_));
        }
        accumulator_ &= (uint64_t{1} << bits_) - 1;
    }
    void PutSigned(const int32_t value, const unsigned count)
    {
        Put(static_cast<uint32_t>(value), count);
    }
    void Unary(uint32_t zeros)
    {
        while (zeros >= 31)
        {
            Put(0, 31);
            zeros -= 31;
        }
        Put(1, zeros + 1);
    }
    void Align()
    {
        if (bits_)
            Put(0, 8 - bits_);
    }

  private:
    std::vector<uint8_t> &out_;
    uint64_t accumulator_{};
    unsigned bits_{};
};

uint8_t Crc8(const uint8_t *data, const size_t size)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

uint16_t Crc16(const uint8_t *data, const size_t size)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= static_cast<uint16_t>(data[i] << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

uint32_t Zigzag(const int32_t value)
{
    return value >= 0 ? static_cast<uint32_t>(value) << 1
                      : (static_cast<uint32_t>(-(value + 1)) << 1) | 1u;
}

struct Subframe
{
    enum class Type
    {
        constant,
        verbatim,
        fixed,
    } type{Type::verbatim};
    unsigned order{};
    unsigned partitionOrder{};
    std::vector<uint8_t> parameters;
    std::vector<int32_t> residual; // indexed by sample; [0, order) unused
    uint64_t bits{};
};

void FixedResidual(const std::vector<int32_t> &x, const unsigned order, std::vector<int32_t> &r)
{
    r.assign(x.size(), 0);
    for (size_t i = order; i < x.size(); ++i)
    {
        int64_t value = x[i];
        switch (order)
        {
        case 1:
            value -= x[i - 1];
            break;
        case 2:
            value -= 2ll * x[i - 1] - x[i - 2];
            break;
        case 3:
            value -= 3ll * x[i - 1] - 3ll * x[i - 2] + x[i - 3];
            break;
        case 4:
            value -= 4ll * x[i - 1] - 6ll * x[i - 2] + 4ll * x[i - 3] - x[i - 4];
            break;
        default:
            break;
        }
        r[i] = static_cast<int32_t>(value);
    }
}

// Best Rice partitioning of a residual whose first `order` samples are warm-up.
void PlanResidual(const std::vector<int32_t> &r, const unsigned order, Subframe &plan)
{
    const size_t n = r.size();
    unsigned maxOrder = 0;
    while (maxOrder < kMaxPartitionOrder && n % (size_t{1} << (maxOrder + 1)) == 0 &&
           (n >> (maxOrder + 1)) > order)
        ++maxOrder;
    const size_t parts = size_t{1} << maxOrder;
    const size_t partSize = n >> maxOrder;
    // cost[q][k] = sum over partition q of (u >> k); counts[q] = residuals in q.
    std::vector<std::array<uint64_t, kMaxRiceParameter + 1>> sums(parts);
    std::vector<uint64_t> counts(parts);
    for (size_t q = 0; q < parts; ++q)
    {
        sums[q].fill(0);
        for (size_t i = std::max<size_t>(q * partSize, order); i < (q + 1) * partSize; ++i)
        {
            const uint32_t u = Zigzag(r[i]);
            for (unsigned k = 0; k <= kMaxRiceParameter; ++k)
                sums[q][k] += u >> k;
            ++counts[q];
        }
    }
    uint64_t bestBits = std::numeric_limits<uint64_t>::max();
    for (unsigned p = maxOrder + 1; p-- > 0;)
    {
        const size_t level = size_t{1} << p;
        const size_t merge = parts / level;
        uint64_t bits = 2 + 4;
        std::vector<uint8_t> parameters(level);
        for (size_t q = 0; q < level; ++q)
        {
            std::array<uint64_t, kMaxRiceParameter + 1> merged{};
            uint64_t count = 0;
            for (size_t m = 0; m < merge; ++m)
            {
                for (unsigned k = 0; k <= kMaxRiceParameter; ++k)
                    merged[k] += sums[q * merge + m][k];
                count += counts[q * merge + m];
            }
            uint64_t best = std::numeric_limits<uint64_t>::max();
            for (unsigned k = 0; k <= kMaxRiceParameter; ++k)
            {
                const uint64_t cost = count * (k + 1) + merged[k];
                if (cost < best)
                {
                    best = cost;
                    parameters[q] = static_cast<uint8_t>(k);
                }
            }
            bits += 4 + best;
        }
        if (bits < bestBits)
        {
            bestBits = bits;
            plan.partitionOrder = p;
            plan.parameters = std::move(parameters);
        }
    }
    plan.bits += bestBits;
}

Subframe PlanSubframe(const std::vector<int32_t> &x, const unsigned bps)
{
    const size_t n = x.size();
    Subframe best;
    best.type = Subframe::Type::verbatim;
    best.bits = 8 + uint64_t(n) * bps;
    if (std::all_of(x.begin(), x.end(), [&](const int32_t value) { return value == x.front(); }))
    {
        best.type = Subframe::Type::constant;
        best.bits = 8 + bps;
        return best;
    }
    std::vector<int32_t> residual;
    for (unsigned order = 0; order <= 4 && order < n; ++order)
    {
        FixedResidual(x, order, residual);
        Subframe plan;
        plan.type = Subframe::Type::fixed;
        plan.order = order;
        plan.bits = 8 + uint64_t(order) * bps;
        PlanResidual(residual, order, plan);
        if (plan.bits < best.bits)
        {
            plan.residual = residual;
            best = std::move(plan);
        }
    }
    return best;
}

void WriteSubframe(BitWriter &writer, const std::vector<int32_t> &x, const unsigned bps,
                   const Subframe &plan)
{
    writer.Put(0, 1);
    switch (plan.type)
    {
    case Subframe::Type::constant:
        writer.Put(0, 6);
        writer.Put(0, 1);
        writer.PutSigned(x.front(), bps);
        return;
    case Subframe::Type::verbatim:
        writer.Put(1, 6);
        writer.Put(0, 1);
        for (const int32_t value : x)
            writer.PutSigned(value, bps);
        return;
    case Subframe::Type::fixed:
        break;
    }
    writer.Put(8 + plan.order, 6);
    writer.Put(0, 1);
    for (unsigned i = 0; i < plan.order; ++i)
        writer.PutSigned(x[i], bps);
    writer.Put(0, 2); // partitioned Rice, 4-bit parameters
    writer.Put(plan.partitionOrder, 4);
    const size_t partSize = x.size() >> plan.partitionOrder;
    for (size_t q = 0; q < plan.parameters.size(); ++q)
    {
        const unsigned k = plan.parameters[q];
        writer.Put(k, 4);
        for (size_t i = std::max<size_t>(q * partSize, plan.order); i < (q + 1) * partSize; ++i)
        {
            const uint32_t u = Zigzag(plan.residual[i]);
            writer.Unary(u >> k);
            writer.Put(u, k);
        }
    }
}

unsigned RateCode(const uint32_t rate)
{
    switch (rate)
    {
    case 88200:
        return 1;
    case 176400:
        return 2;
    case 192000:
        return 3;
    case 8000:
        return 4;
    case 16000:
        return 5;
    case 22050:
        return 6;
    case 24000:
        return 7;
    case 32000:
        return 8;
    case 44100:
        return 9;
    case 48000:
        return 10;
    case 96000:
        return 11;
    default:
        break;
    }
    if (rate % 1000 == 0 && rate / 1000 <= 255)
        return 12;
    if (rate <= 65535)
        return 13;
    if (rate % 10 == 0 && rate / 10 <= 65535)
        return 14;
    throw std::runtime_error("FLAC cannot code sample rate " + std::to_string(rate));
}

void PutUtf8(std::vector<uint8_t> &out, const uint32_t value)
{
    if (value < 0x80)
    {
        out.push_back(static_cast<uint8_t>(value));
        return;
    }
    unsigned extra = value < 0x800 ? 1 : value < 0x10000 ? 2 : value < 0x200000 ? 3 : value < 0x4000000 ? 4 : 5;
    const uint8_t lead = static_cast<uint8_t>((0xFF00u >> (extra + 1)) & 0xFF);
    out.push_back(static_cast<uint8_t>(lead | (value >> (6 * extra))));
    while (extra--)
        out.push_back(static_cast<uint8_t>(0x80 | ((value >> (6 * extra)) & 0x3F)));
}

struct EncodedSample
{
    std::vector<uint8_t> frames;
    std::vector<uint32_t> frameOffsets;
};

EncodedSample EncodeFlac(const Sample &sample)
{
    const unsigned channels = sample.channels;
    if (channels < 1 || channels > 2)
        throw std::runtime_error("sound " + sample.name + " must be mono or stereo");
    const size_t frames = sample.pcm.size() / channels;
    if (!frames || sample.pcm.size() % channels)
        throw std::runtime_error("sound " + sample.name + " has no complete PCM frames");
    const unsigned rateCode = RateCode(sample.rate);
    EncodedSample encoded;
    std::vector<int32_t> left, right, mid, side;
    for (size_t start = 0, number = 0; start < frames; start += kBlockSamples, ++number)
    {
        const size_t n = std::min<size_t>(kBlockSamples, frames - start);
        encoded.frameOffsets.push_back(static_cast<uint32_t>(encoded.frames.size()));
        std::vector<uint8_t> &out = encoded.frames;
        const size_t frameStart = out.size();
        left.resize(n);
        for (size_t i = 0; i < n; ++i)
            left[i] = sample.pcm[(start + i) * channels];
        unsigned assignment = 0;
        std::vector<std::pair<const std::vector<int32_t> *, unsigned>> subframes;
        std::vector<Subframe> plans;
        if (channels == 1)
        {
            plans.push_back(PlanSubframe(left, 16));
            subframes.push_back({&left, 16});
        }
        else
        {
            right.resize(n);
            mid.resize(n);
            side.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                right[i] = sample.pcm[(start + i) * 2 + 1];
                mid[i] = (left[i] + right[i]) >> 1;
                side[i] = left[i] - right[i];
            }
            Subframe l = PlanSubframe(left, 16), r = PlanSubframe(right, 16),
                     m = PlanSubframe(mid, 16), s = PlanSubframe(side, 17);
            const uint64_t independent = l.bits + r.bits, leftSide = l.bits + s.bits,
                           rightSide = s.bits + r.bits, midSide = m.bits + s.bits;
            const uint64_t best = std::min({independent, leftSide, rightSide, midSide});
            if (best == independent)
            {
                assignment = 1;
                plans = {std::move(l), std::move(r)};
                subframes = {{&left, 16}, {&right, 16}};
            }
            else if (best == leftSide)
            {
                assignment = 8;
                plans = {std::move(l), std::move(s)};
                subframes = {{&left, 16}, {&side, 17}};
            }
            else if (best == rightSide)
            {
                assignment = 9;
                plans = {std::move(s), std::move(r)};
                subframes = {{&side, 17}, {&right, 16}};
            }
            else
            {
                assignment = 10;
                plans = {std::move(m), std::move(s)};
                subframes = {{&mid, 16}, {&side, 17}};
            }
        }
        const unsigned blockCode = n == kBlockSamples ? 10 : n <= 256 ? 6 : 7;
        out.push_back(0xFF);
        out.push_back(0xF8); // fixed block size
        out.push_back(static_cast<uint8_t>((blockCode << 4) | rateCode));
        out.push_back(static_cast<uint8_t>((assignment << 4) | (4 << 1))); // 16-bit samples
        PutUtf8(out, static_cast<uint32_t>(number));
        if (blockCode == 6)
            out.push_back(static_cast<uint8_t>(n - 1));
        else if (blockCode == 7)
        {
            out.push_back(static_cast<uint8_t>((n - 1) >> 8));
            out.push_back(static_cast<uint8_t>(n - 1));
        }
        if (rateCode == 12)
            out.push_back(static_cast<uint8_t>(sample.rate / 1000));
        else if (rateCode == 13 || rateCode == 14)
        {
            const uint32_t value = rateCode == 13 ? sample.rate : sample.rate / 10;
            out.push_back(static_cast<uint8_t>(value >> 8));
            out.push_back(static_cast<uint8_t>(value));
        }
        out.push_back(Crc8(out.data() + frameStart, out.size() - frameStart));
        BitWriter writer(out);
        for (size_t c = 0; c < plans.size(); ++c)
            WriteSubframe(writer, *subframes[c].first, subframes[c].second, plans[c]);
        writer.Align();
        const uint16_t crc = Crc16(out.data() + frameStart, out.size() - frameStart);
        out.push_back(static_cast<uint8_t>(crc >> 8));
        out.push_back(static_cast<uint8_t>(crc));
    }
    return encoded;
}

// ------------------------------------------------------------------- SAB
std::array<uint8_t, 16> Md5(const uint8_t *data, const size_t size)
{
    std::array<uint8_t, 16> digest{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) >= 0 &&
                    BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
                    BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0 &&
                    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        throw std::runtime_error("cannot compute MD5");
    return digest;
}

size_t Align4k(const size_t value)
{
    return (value + 4095) & ~size_t{4095};
}

// Native 0x1413975E0 reads entry bytes 35..42 as a loudness envelope: levels
// (0..100, dB + 100) at the start, at two positions (0..65535 of the length)
// and at the end. Stock banks put the first point at the loudest instant.
std::array<uint8_t, 9> Envelope(const Sample &sample)
{
    const size_t frames = sample.pcm.size() / sample.channels;
    const size_t window = std::max<size_t>(1, frames / 256);
    std::vector<uint8_t> levels;
    for (size_t start = 0; start < frames; start += window)
    {
        int peak = 0;
        const size_t end = std::min(frames, start + window);
        for (size_t i = start * sample.channels; i < end * sample.channels; ++i)
            peak = std::max(peak, std::abs(int(sample.pcm[i])));
        const double db = peak ? 20.0 * std::log10(peak / 32768.0) : -100.0;
        levels.push_back(static_cast<uint8_t>(std::clamp(std::lround(100.0 + db), 0l, 100l)));
    }
    const size_t loudest = std::max_element(levels.begin(), levels.end()) - levels.begin();
    const size_t after = loudest + std::max<size_t>(1, (levels.size() - 1 - loudest) / 4);
    const size_t second = std::min(levels.size() - 1, after);
    const auto position = [&](const size_t index) {
        return static_cast<uint16_t>(std::min<size_t>(65535, index * window * 65535 / frames));
    };
    uint16_t first = position(loudest);
    uint16_t last = position(second);
    if (last <= first)
        last = static_cast<uint16_t>(std::min(65535, first + 1));
    std::array<uint8_t, 9> tail{};
    std::memcpy(tail.data(), &first, 2);
    std::memcpy(tail.data() + 2, &last, 2);
    tail[4] = levels.front();
    tail[5] = levels[loudest];
    tail[6] = levels[second];
    tail[7] = levels.back();
    return tail;
}

// SndIndexEntry table: bucket id % count, collisions chained through `next`.
struct IndexEntry
{
    uint16_t value = UINT16_MAX;
    uint16_t next = UINT16_MAX;
};

std::vector<IndexEntry> MakeAliasIndex(const std::vector<uint32_t> &ids)
{
    const size_t count = ids.size();
    std::vector<IndexEntry> result(count);
    const auto bucketFor = [&](const uint16_t value) { return static_cast<uint16_t>(ids[value] % count); };
    const auto empty = [&]() -> uint16_t {
        for (uint16_t i = 0; i < result.size(); ++i)
            if (result[i].value == UINT16_MAX)
                return i;
        throw std::runtime_error("sound alias index table overflow");
    };
    for (uint16_t value = 0; value < count; ++value)
    {
        const uint16_t bucket = bucketFor(value);
        if (result[bucket].value == UINT16_MAX)
        {
            result[bucket].value = value;
            continue;
        }
        if (bucketFor(result[bucket].value) == bucket)
        {
            uint16_t tail = bucket;
            while (result[tail].next != UINT16_MAX)
                tail = result[tail].next;
            const auto slot = empty();
            result[slot].value = value;
            result[tail].next = slot;
            continue;
        }
        // A prior collision borrowed this home bucket: move that chain node.
        const uint16_t displaced = result[bucket].value;
        uint16_t predecessor = bucketFor(displaced);
        while (result[predecessor].next != bucket)
        {
            if (result[predecessor].next == UINT16_MAX)
                throw std::runtime_error("invalid sound alias collision chain");
            predecessor = result[predecessor].next;
        }
        const auto slot = empty();
        result[slot] = result[bucket];
        result[predecessor].next = slot;
        result[bucket] = {value, UINT16_MAX};
    }
    return result;
}

// Replay sound.h curve hash of "default" (the fallback every map bank defines).
constexpr uint32_t kDefaultHash = 4135636924u;
} // namespace

uint32_t HashName(const std::string_view name)
{
    uint32_t hash = 5381;
    for (const char c : name)
    {
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
        hash = 65599u * hash + static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(lower)));
    }
    return hash ? hash : 1;
}

std::vector<uint8_t> BuildSab(const std::vector<Sample> &samples, const std::string &zone)
{
    struct Item
    {
        const Sample *sample;
        uint32_t key;
        EncodedSample encoded;
        std::vector<uint8_t> seek;
        uint64_t offset;
    };
    std::vector<Item> items;
    items.reserve(samples.size());
    for (const Sample &sample : samples)
    {
        if (sample.name.empty() || sample.name.size() >= 128)
            throw std::runtime_error("SAB sample name must be 1..127 characters: " + sample.name);
        Item item{&sample, HashName(sample.name), EncodeFlac(sample), {}, 0};
        const size_t frames = sample.pcm.size() / sample.channels;
        // Seek points every 16384 samples, as stock: {u64 sample, u64 byte, u16 1024}.
        for (size_t point = 0; point < frames / kSeekInterval; ++point)
        {
            const uint64_t sampleNumber = uint64_t(point) * kSeekInterval;
            const uint64_t byte = item.encoded.frameOffsets.at(sampleNumber / kBlockSamples);
            const uint16_t blockSamples = kBlockSamples;
            const size_t at = item.seek.size();
            item.seek.resize(at + 18);
            std::memcpy(item.seek.data() + at, &sampleNumber, 8);
            std::memcpy(item.seek.data() + at + 8, &byte, 8);
            std::memcpy(item.seek.data() + at + 16, &blockSamples, 2);
        }
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.key < b.key; });
    for (size_t i = 1; i < items.size(); ++i)
        if (items[i - 1].key == items[i].key)
            throw std::runtime_error("SAB key collision: " + items[i - 1].sample->name + " / " +
                                     items[i].sample->name);

    // Stock layout: 688-byte header page, packed sample data from 4096, then the
    // entry index, entry digests, name digests and 128-byte names, each 4 KiB aligned.
    std::vector<uint8_t> sab(4096, 0);
    uint64_t dataBytes = 0;
    for (Item &item : items)
    {
        item.offset = sab.size();
        sab.insert(sab.end(), item.seek.begin(), item.seek.end());
        sab.insert(sab.end(), item.encoded.frames.begin(), item.encoded.frames.end());
        dataBytes += item.seek.size() + item.encoded.frames.size();
    }
    const size_t count = items.size();
    const size_t indexOffset = Align4k(sab.size());
    const size_t digestOffset = Align4k(indexOffset + count * 44);
    const size_t nameDigestOffset = Align4k(digestOffset + count * 16);
    const size_t namesOffset = Align4k(nameDigestOffset + count * 16);
    sab.resize(namesOffset + count * 128, 0);
    for (size_t i = 0; i < count; ++i)
    {
        const Item &item = items[i];
        const Sample &sample = *item.sample;
        const size_t entry = indexOffset + i * 44;
        Store(sab, entry + 0, item.key);
        Store(sab, entry + 4, static_cast<uint32_t>(item.encoded.frames.size()));
        Store(sab, entry + 8, static_cast<uint32_t>(item.seek.size()));
        Store(sab, entry + 12, static_cast<uint32_t>(sample.pcm.size() / sample.channels));
        Store(sab, entry + 16, uint32_t{0}); // no hybrid PCM tail
        Store(sab, entry + 20, item.offset);
        Store(sab, entry + 28, sample.rate);
        sab[entry + 32] = sample.channels;
        sab[entry + 33] = sample.looping ? 1 : 0;
        sab[entry + 34] = 8; // FLAC
        const auto tail = Envelope(sample);
        std::memcpy(sab.data() + entry + 35, tail.data(), tail.size());
        const auto digest = Md5(item.encoded.frames.data(), item.encoded.frames.size());
        std::memcpy(sab.data() + digestOffset + i * 16, digest.data(), 16);
        std::string lower = sample.name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto nameDigest = Md5(reinterpret_cast<const uint8_t *>(lower.data()), lower.size());
        std::memcpy(sab.data() + nameDigestOffset + i * 16, nameDigest.data(), 16);
        std::memcpy(sab.data() + namesOffset + i * 128, sample.name.data(), sample.name.size());
    }
    // Header (SND_AssetBankValidateHeader 0x141B85AB0).
    Store(sab, 0x00, uint32_t{0x23585532});
    Store(sab, 0x04, uint32_t{10});
    Store(sab, 0x08, uint32_t{44});
    Store(sab, 0x0C, uint32_t{16});
    Store(sab, 0x10, uint32_t{64}); // zone name capacity, as stock
    Store(sab, 0x14, static_cast<uint32_t>(count));
    Store(sab, 0x18, uint32_t{8}); // platform name capacity, as stock
    Store(sab, 0x1C, uint32_t{16});
    Store(sab, 0x20, static_cast<uint64_t>(sab.size()));
    Store(sab, 0x28, static_cast<uint64_t>(indexOffset));
    Store(sab, 0x30, static_cast<uint64_t>(digestOffset));
    const auto link = Md5(sab.data() + indexOffset, count * 44);
    std::memcpy(sab.data() + 0x38, link.data(), link.size());
    Store(sab, 0x248, static_cast<uint64_t>(nameDigestOffset));
    Store(sab, 0x250, static_cast<uint64_t>(namesOffset));
    std::memcpy(sab.data() + 0x258, zone.data(), std::min<size_t>(zone.size(), 63));
    std::memcpy(sab.data() + 0x298, "pc_dx12", 7);
    std::memcpy(sab.data() + 0x2A0, "all", 3);
    sab[0x2A3] = 0x0A;
    if (dataBytes > UINT32_MAX)
        throw std::runtime_error("SAB sample data exceeds 4 GiB");
    Store(sab, 0x2A7, static_cast<uint32_t>(dataBytes));
    return sab;
}

void RegisterBank(ZoneWriter &writer, const Bank &source)
{
    // Asset bodies run during ZoneWriter::build(); keep a copy alive until then.
    const auto shared = std::make_shared<const Bank>(source);
    const Bank &bank = *shared;
    if (bank.lists.empty() || bank.lists.size() >= UINT16_MAX || bank.sab.empty())
        throw std::runtime_error("sound bank " + bank.name + " has no aliases or data");
    if (bank.sab.size() > UINT32_MAX)
        throw std::runtime_error("sound bank " + bank.name + " SAB exceeds 4 GiB");

    // StreamKey (Preload_StreamKey 0x140E33FB0): 64 bytes, name at 0, data at 40,
    // size at 56. Byte 60 = 2 selects the sound behaviour whose first callback
    // (0x141394D60 -> SNDL_LoadBank 0x141B86070) indexes the loaded SAB; byte 61
    // bit 1 = resident data loaded inline (sub_140E34050). Stock "mp_shipment_all_loaded"
    // carries the same 2 / 3.
    writer.add(ASSET_TYPE_STREAMKEY, bank.streamKey, [shared](ZoneWriter &out) {
        const Bank &bank = *shared;
        std::vector<uint8_t> root(64, 0);
        Store(root, 0, PTR_FOLLOWS);
        Store(root, 40, PTR_FOLLOWS);
        Store(root, 56, static_cast<uint32_t>(bank.sab.size()));
        root[60] = 2;
        root[61] = 3;
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(7);
        out.write(root.data(), root.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(bank.streamKey);
        out.align(15); // AllocLoad_byte16
        out.write(bank.sab.data(), bank.sab.size());
        out.popStream();
        out.popStream();
    });

    // SndBankResident (Load_SndBankResidentPtr 0x140E3A020 / Load_SndBank
    // 0x140E39A80): 0x200 header in stream 1, contents in stream 5.
    writer.add(ASSET_TYPE_SOUNDBANK, bank.name, [shared](ZoneWriter &out) {
        const Bank &bank = *shared;
        std::vector<uint32_t> ids;
        for (const AliasList &list : bank.lists)
            ids.push_back(HashName(list.name));
        const auto index = MakeAliasIndex(ids);
        std::vector<uint8_t> header(0x200, 0);
        for (size_t offset : {size_t(0), size_t(8), size_t(16), size_t(24), size_t(40), size_t(48)})
            Store(header, offset, PTR_FOLLOWS);
        Store(header, 32, static_cast<uint32_t>(bank.lists.size()));
        // soundTable.id stays 0: SND_GetSoundTableById never selects this bank.
        if (!bank.volumeCurves.empty())
        {
            Store(header, 400, static_cast<uint32_t>(bank.volumeCurves.size()));
            Store(header, 408, PTR_FOLLOWS);
        }
        Store(header, 496, out.assetAlias(ASSET_TYPE_STREAMKEY, bank.streamKey));
        header[504] = 1; // streamInfo.alwaysLoaded: SNDL_AddBank uses a resident slot
        out.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
        out.align(7);
        out.write(header.data(), header.size());
        out.pushStream(XFILE_BLOCK_VIRTUAL);
        out.writeStr(bank.name);
        out.writeStr(bank.zone);
        out.writeStr("eng");
        out.writeStr("all");
        out.align(7); // AllocLoad_SndAliasList
        for (const AliasList &list : bank.lists)
        {
            if (list.aliases.empty())
                throw std::runtime_error("sound alias list " + list.name + " is empty");
            std::vector<uint8_t> record(32, 0);
            Store(record, 0, PTR_FOLLOWS);
            Store(record, 8, HashName(list.name));
            Store(record, 16, PTR_FOLLOWS);
            Store(record, 24, static_cast<int32_t>(list.aliases.size()));
            out.write(record.data(), record.size());
        }
        for (const AliasList &list : bank.lists)
        {
            out.writeStr(list.name);
            out.align(7); // Load_SndAliasList head array
            for (const Alias &alias : list.aliases)
            {
                std::vector<uint8_t> head(232, 0);
                Store(head, 0, PTR_FOLLOWS);
                if (!alias.secondaryName.empty())
                    Store(head, 16, PTR_FOLLOWS);
                Store(head, 24, PTR_FOLLOWS);
                Store(head, 32, HashName(list.name));
                Store(head, 36, alias.secondaryName.empty() ? 0u : HashName(alias.secondaryName));
                Store(head, 40, HashName(alias.assetName));
                Store(head, 76, alias.volMin);
                Store(head, 80, alias.volMax);
                Store(head, 84, alias.volModIndex);
                Store(head, 88, alias.pitchMin);
                Store(head, 92, alias.pitchMax);
                Store(head, 96, alias.distMin); // donutFadeEnd: stock equals distMin
                Store(head, 100, alias.distMin);
                Store(head, 104, alias.distMax);
                Store(head, 112, alias.flags);
                Store(head, 116, alias.probability);
                Store(head, 120, alias.lfePercentage);
                Store(head, 128, alias.startDelay);
                Store(head, 132, alias.startDelayMax);
                Store(head, 136, alias.volumeFalloffCurve ? alias.volumeFalloffCurve : kDefaultHash);
                Store(head, 140, kDefaultHash); // lpfCurve
                Store(head, 144, kDefaultHash); // hpfCurve
                Store(head, 148, kDefaultHash); // reverbSendCurve
                Store(head, 164, kDefaultHash); // speakerMap
                Store(head, 168, alias.reverbMultiplier);
                Store(head, 172, alias.farReverbMultiplier);
                Store(head, 176, kDefaultHash); // occlusionShape
                Store(head, 180, kDefaultHash); // dopplerPreset
                Store(head, 192, 1.0f);         // smartPanAttenuation2d
                Store(head, 196, alias.stereoSpreadMinDist);
                Store(head, 200, alias.stereoSpreadMaxDist);
                Store(head, 204, alias.stereoSpreadMidPoint);
                head[208] = alias.stereoSpreadMaxAngle;
                Store(head, 216, 1.0f);        // masterSlavePercentage
                Store(head, 220, 0.25118864f); // masterThreshold (-12 dB)
                Store(head, 224, alias.maxDurationDsec);
                out.write(head.data(), head.size());
            }
            for (const Alias &alias : list.aliases)
            {
                out.writeStr(list.name);
                if (!alias.secondaryName.empty())
                    out.writeStr(alias.secondaryName);
                out.writeStr(alias.assetName);
            }
        }
        out.align(1);
        out.write(index.data(), index.size() * sizeof(IndexEntry));
        if (!bank.volumeCurves.empty())
        {
            out.align(3);
            for (const Curve &curve : bank.volumeCurves)
            {
                if (curve.name.empty() || curve.name.size() >= 64 || curve.points.empty() ||
                    curve.points.size() > 16)
                    throw std::runtime_error("invalid sound curve " + curve.name);
                std::vector<uint8_t> record(200, 0);
                std::memcpy(record.data(), curve.name.data(), curve.name.size());
                Store(record, 64, HashName(curve.name));
                Store(record, 68, static_cast<int32_t>(curve.points.size()));
                for (size_t i = 0; i < curve.points.size(); ++i)
                {
                    Store(record, 72 + i * 8, curve.points[i][0]);
                    Store(record, 76 + i * 8, curve.points[i][1]);
                }
                out.write(record.data(), record.size());
            }
        }
        out.popStream();
        out.popStream();
    });
}
} // namespace iw8::sound
