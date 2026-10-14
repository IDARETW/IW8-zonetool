#pragma once

#include "../iw8/replay_sound.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace iw3::sound
{
struct ConvertedSounds
{
    iw8::sound::Bank bank;
    // Converted IW3 alias names -> true when looping (playloopsound only).
    std::map<std::string, bool> aliases;
};

// Alias names the map's own IW3 scripts pass as literals to playsound,
// playloopsound, playsoundasmaster, soundexists and playsoundatpos.
std::set<std::string> ReadScriptSoundNames(const std::filesystem::path &scriptDirectory);

// Every identifier-like string literal in the scripts: maps pass aliases
// through their own helpers (global_FX( ..., "emt_fly_loop" )). Convert keeps
// only the ones the map defines as aliases.
std::set<std::string> ReadScriptStringLiterals(const std::filesystem::path &scriptDirectory);

// Converts the requested IW3 aliases and their secondary chains into one
// always-loaded Replay map bank. Definitions come from the extracted map zone
// (soundaliases/*.iw3.json), else from IW3 raw soundaliases/*.csv rows that
// apply to MP maps; audio from the extracted zone, loose raw/sound files or
// the IWD archives on the search paths. Returns nothing when no alias converts.
std::optional<ConvertedSounds> Convert(const std::filesystem::path &exportRoot,
                                       const std::vector<std::filesystem::path> &searchPaths,
                                       const std::string &map,
                                       const std::set<std::string> &requested,
                                       const std::set<std::string> &candidates = {});
// Extracts the members of the IWD archives in `directory` whose lowercase path
// starts with one of `prefixes` into `destination`. Existing files are kept and
// later archives take priority. Returns the number of files written.
std::size_t ExtractIwdMembers(const std::filesystem::path &directory,
                              const std::vector<std::string> &prefixes,
                              const std::filesystem::path &destination);
} // namespace iw3::sound
