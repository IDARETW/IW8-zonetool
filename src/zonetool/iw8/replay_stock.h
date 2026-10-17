#pragma once

#include "iw8_zone.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace iw8::stock
{
// Stock Replay assets that every shipped map carries and IW3 has no source for
// (checklist section 19): the surface-effect table `void` (per-surface effects
// for brass, equipment explosions and killstreak impacts) with every effect,
// physics shape and model it needs, and the CODCaster spectator images. They
// are read from the game install through ACTS exports at build time; nothing
// stock is stored in this repository.
struct Options
{
    std::filesystem::path acts;             // ACTS acts.exe with ACTS_NATIVE_BYTES support
    std::filesystem::path replayExecutable; // game_dx12_ship_replay.exe
    std::filesystem::path cacheDirectory;   // export cache, reused across builds
};

class Copier
{
  public:
    // Exports (or reuses the cached exports of) mp_shipment and its eng_/ww_
    // companions and resolves everything `void` needs.
    explicit Copier(const Options &options);
    ~Copier();
    Copier(const Copier &) = delete;
    Copier &operator=(const Copier &) = delete;

    // `void` and its closure into the map zone. Assets the shipped zone names
    // with a comma (owned by a common or worldwide-common zone) become name-only
    // references; materials also get their techset-zone peer.
    void RegisterSurfaceEffects(ZoneWriter &main, ZoneWriter &techsets) const;
    // CODCaster images where stock keeps them: eng_<map> and ww_<map>.
    void RegisterEnglishImages(ZoneWriter &english) const;
    void RegisterWorldwideImages(ZoneWriter &worldwide) const;
    // Output-ledger origin of every asset the copier registers.
    const std::map<std::pair<int, std::string>, std::string> &Origins() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace iw8::stock
