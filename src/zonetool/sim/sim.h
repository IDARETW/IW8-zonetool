#pragma once
// iw8-zonetool sim: renders a converted map from the converter's kept
// intermediate with the exact shader bytecode the zone carries, emulating
// only what the engine supplies at runtime (camera, sun constants, AO, probe
// tables). Subcommands: render (PNG), panorama (equirectangular JPEGs for the
// web walk-through), serve (local web viewer with a free camera), check
// (load-time validation through Replay's own loaders), info.
namespace sim
{
int Main(int argc, char **argv);
}
