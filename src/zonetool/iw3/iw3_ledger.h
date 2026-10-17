#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>

// Per-asset ledger of the IW3 source assets a conversion consumed. The readers
// every conversion path goes through (LoadImage, ReadMaterial, ReadFxMaterial,
// ReadSourceModel) and the alias/effect/raw-file selections record each name
// they use; AuditSourceAssetDeclarations then accounts for every declaration of
// the map's IW3 zones against it. Thread-safe.
namespace iw3::ledger
{
void Use(std::string_view type, std::string_view name);
std::set<std::pair<std::string, std::string>> Used();
void Clear();
// Where the per-asset ledger (TSV: zone, type, name, fate, detail) is written;
// empty writes none.
void SetReport(const std::filesystem::path &path);
std::filesystem::path Report();
} // namespace iw3::ledger
