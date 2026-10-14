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

// Converts the requested IW3 aliases and their secondary chains into one
// always-loaded Replay map bank. Definitions come from the extracted map zone
// (soundaliases/*.iw3.json), else from IW3 raw soundaliases/*.csv rows that
// apply to MP maps; audio from the extracted zone, loose raw/sound files or
// the IWD archives on the search paths. Returns nothing when no alias converts.
std::optional<ConvertedSounds> Convert(const std::filesystem::path &exportRoot,
                                       const std::vector<std::filesystem::path> &searchPaths,
                                       const std::string &map,
                                       const std::set<std::string> &requested);
} // namespace iw3::sound
