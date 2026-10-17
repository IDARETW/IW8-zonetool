#pragma once

#include "iw8_zone.h"

namespace iw8::ies
{
// The world's IES lookup strip (GfxWorldDraw +0xF0) and the IES LightDefs that
// index it, in the layout of shipped Replay MP maps (mp_hideout, mp_shipment):
//  - "*ieslookup": 512x1 R8 (GfxPixelFormat 1), 8 levels, one 128-texel segment
//    per profile: 0 = white (lights without a profile), then IES_16 and
//    ies_nvg_flashlight, then an empty segment (the strip is a power of two).
//  - LightDefs "flashlight" (IES_16) and "light_ies_nvg_flashlight": stock maps
//    carry them for the weapon flashlight and the NVG illuminator, which look
//    them up by name in the loaded map. IW3 has no IES lights, so converted
//    local lights keep their profile-less definitions (segment 0).
constexpr const char *kLookupName = "*ieslookup";

// Registers the strip and both LightDefs; must precede the gfx_map asset.
void Register(ZoneWriter &writer);
} // namespace iw8::ies
