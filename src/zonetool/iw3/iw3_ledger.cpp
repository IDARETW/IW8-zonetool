#include "iw3_ledger.h"

#include <mutex>

namespace iw3::ledger
{
namespace
{
std::mutex mutex;
std::set<std::pair<std::string, std::string>> used;
std::filesystem::path report;
} // namespace

void Use(const std::string_view type, const std::string_view name)
{
    const std::lock_guard lock(mutex);
    used.emplace(std::string(type), std::string(name));
}

std::set<std::pair<std::string, std::string>> Used()
{
    const std::lock_guard lock(mutex);
    return used;
}

void SetReport(const std::filesystem::path &path)
{
    const std::lock_guard lock(mutex);
    report = path;
}

std::filesystem::path Report()
{
    const std::lock_guard lock(mutex);
    return report;
}

void Clear()
{
    const std::lock_guard lock(mutex);
    used.clear();
}
} // namespace iw3::ledger
