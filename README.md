# IW8-zonetool

Builds custom multiplayer map fastfiles for the Call of Duty: Modern Warfare (2019) **1.20 Replay** build.
Its main use is converting Call of Duty 4 (IW3) maps.

From an IW3 map fastfile it produces native Replay zones covering:
- world geometry, materials, lightmaps, lightgrid and reflection probes;
- Havok collision, static and dynamic models, breakable glass and ladders;
- bot navigation: navmesh, pathnodes and tactical graph.

## Build

Requirements: Visual Studio 2022 (or newer) with the C++ desktop workload, and [XMake](https://xmake.io/).

```bat
xmake build -y iw8-zonetool
```

The executable lands in `xmake-out\x64\Release\iw8-zonetool.exe`.

## Convert an IW3 map

```powershell
iw8-zonetool.exe build-iw3 <path\to\mp_map.ff> mp_map -o <output dir> `
    --replay <game_dx12_ship_replay.exe> `
    --unlinker <OpenAssetTools Unlinker.exe> `
    --iw3-root <CoD4 install>
```

The output folder holds the map's five fastfiles plus `map.json`.

Options:
- `--no-navigation` skips the generated bot data.
- `--metadata <map.json>` sets the map title and description.

More detail is in [docs/USAGE.md](docs/USAGE.md) and [docs/IW3_FASTFILE.md](docs/IW3_FASTFILE.md).

## Status

Work in progress. Nuketown is the main test map, and in-game results are still being verified.
