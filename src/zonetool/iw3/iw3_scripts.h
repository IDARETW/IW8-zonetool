#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace iw3::scripts
{
struct Source
{
    std::string path; // lowercase, forward slashes, e.g. "maps/mp/_fan.gsc"
    std::string text;
};

struct Translation
{
    std::string gsc;               // IW8 GSC: translated map scripts + IW3 compatibility layer
    std::set<std::string> strings; // string literals the map scripts use (targetnames etc.)
    std::vector<std::string> notes;
    bool translatedMain{};
};

// Translates the map's own IW3 scripts (maps/mp/<map>.gsc and the map-local
// files it uses) into one IW8 compilation unit. IW3 common-script calls go to
// the compatibility layer; IW3-only builtins become iw3compat_noop() calls.
// effectFor maps an IW3 effect name to a loadable IW8 one ("vfx/...").
// generated is appended verbatim (map-specific iw3compat_minimap() and
// iw3compat_generated() definitions).
Translation Translate(const std::string &map, const std::vector<Source> &sources,
                      const std::function<std::string(const std::string &)> &effectFor,
                      const std::string &generated);

// Common MP zone effect standing in for an IW3 effect name.
std::string StockEffectFor(const std::string &iw3Effect);

// Compiles IW8 GSC with gsc-tool (-m comp -g iw8 -s pc) and returns the gscbin.
std::vector<std::uint8_t> Compile(const std::string &gsc, const std::filesystem::path &gscTool,
                                  const std::filesystem::path &workDirectory,
                                  const std::string &stem);
} // namespace iw3::scripts
