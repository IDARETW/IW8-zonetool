#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace convert
{
struct TriggerModelSource
{
    uint32_t collisionModel{};
    bool staticPhysics{true};
};

struct CompassBounds
{
    float northwestX{};
    float northwestY{};
    float southeastX{};
    float southeastY{};
    // IW3 worldspawn northyaw (default 90): the compass image's north direction.
    float northYaw{90.0f};
};

bool iw3KeyToIw8KeyId(const std::string &key, uint32_t &keyId);
std::vector<std::string> iw3ReplayEntityModels(const std::string &input);
bool iw3CompassBounds(const std::string &input, CompassBounds &bounds);
size_t iw3ToIw8EntityString(const std::string &input, std::string &output,
                            std::vector<TriggerModelSource> *triggers = nullptr);
void validateIw8EntityString(const std::string &input);
} // namespace convert
