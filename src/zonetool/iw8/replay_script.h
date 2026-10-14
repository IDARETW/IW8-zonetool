#pragma once

#include "iw8_zone.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iw8::replay_script
{
// Encode bytes as a zlib stream using stored DEFLATE blocks. Replay accepts
// this representation for compressed ScriptFile and RawFile payloads.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t> &input);

// Emits the small map-startup ScriptFile used by the stock IW8 compass setup.
void emitCompassStartup(ZoneWriter &writer, const std::string &assetName,
                        const std::string &mapName, float northwestX, float northwestY,
                        float southeastX, float southeastY);

// gsc-tool -g iw8 writes local call, thread-call and function-reference offsets
// and endswitch case offsets as byte distances; Replay 1.20 stores them doubled
// (its VM decodes (operand << 8) >> 9, and every one of 755 stock scripts does so).
// Rewrites those operands in a bytecode block, checking every target.
std::vector<uint8_t> toReplayCodeOffsets(std::vector<uint8_t> bytecode);

// Emits a ScriptFile from a gsc-tool iw8 .gscbin ("GSC\0", compressed stack
// length, stack length, bytecode length, zlib stack, bytecode). The bytecode
// goes through toReplayCodeOffsets; the stack is rewritten so names gsc-tool
// knows above Replay's compiled-in strings (0xE2C0) are stored by name, and
// checked against the game loader's reads.
void emitCompiledScript(ZoneWriter &writer, const std::string &assetName,
                        const std::vector<uint8_t> &gscbin);
} // namespace iw8::replay_script
