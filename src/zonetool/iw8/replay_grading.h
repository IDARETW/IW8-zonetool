#pragma once

#include "iw8_zone.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iw8::grading
{
// Registers a GfxGradingClut (asset type 35) and its HDR CLUT image
// ("gradingclut" + name). The pixels are clutSize^3 R16G16B16A16_UNORM texels.
void RegisterGradingClut(ZoneWriter &writer, const std::string &name, unsigned clutSize,
                         const std::vector<uint8_t> &pixels);
} // namespace iw8::grading
