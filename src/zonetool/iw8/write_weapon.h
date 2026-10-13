#pragma once
#include <string>

namespace iw8
{
// Native Replay melee package; input carries a prepared reference and OBJ geometry.
bool buildWeapon(const std::string &input, const std::string &outputDirectory);
// Build named Replay-native placeholder weapons from a pinned native template.
bool buildWeaponPlaceholders(const std::string &manifest, const std::string &outputDirectory);
// Separate from map argument parsing: weapon flags describe native asset fields.
int weaponMain(int argc, char **argv);
// Reuse the native model/material/animation writers for scene props.
int buildSceneAssets(const std::string &project, const std::string &outputDirectory);
} // namespace iw8
