#pragma once

#include "iw8_zone.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iw8::rawfile
{
void Register(ZoneWriter &writer, const std::string &name, const std::vector<uint8_t> &data);
}

namespace iw8::localize
{
// LocalizeEntry (0x10: name, value; Preload_LocalizeEntry). Names are the
// lowercase string key ("mp_chyron/mp_shipment_axis_1"), as stock.
void Register(ZoneWriter &writer, const std::string &name, const std::string &value);
}
