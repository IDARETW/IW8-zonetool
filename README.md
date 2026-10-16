# IW8-zonetool

A map compiler that converts Call of Duty 4: Modern Warfare (**IW3**) multiplayer maps into native
fastfiles for Call of Duty: Modern Warfare (2019) (**IW8**), build **1.20 Replay**.

One command reads a CoD4 map fastfile and writes the map's IW8 zones: world, lighting, collision,
models, effects, sounds, map scripts, spawns and bot navigation.

## Supported Games

* **IW3** (*Call of Duty 4: Modern Warfare*) - source, multiplayer maps
* **IW8** (*Call of Duty: Modern Warfare (2019)*, 1.20 Replay) - target

## Requirements

- Windows x64.
- **Call of Duty 4: Modern Warfare** installed; shared models, materials and sounds are read from it.
- **Modern Warfare (2019) 1.20 Replay** (`game_dx12_ship_replay.exe`); it is used to bake the map's
  Havok collision.
- **OpenAssetTools `Unlinker.exe`**, built with this project's IW3 map exporters
  (see [docs/IW3_FASTFILE.md](docs/IW3_FASTFILE.md#openassettools-unlinker)).
- **[gsc-tool](https://github.com/xensik/gsc-tool)** to compile the converted map scripts. Without it
  the map still builds, but only its minimap setup runs; scripted features such as moving parts,
  destructibles and ambient sounds are left out.
- To build from source: Visual Studio 2022 or newer with the *Desktop development with C++*
  workload, and [XMake](https://xmake.io/).

## Build

```bat
xmake build -y iw8-zonetool
```

The executable is written to `xmake-out\x64\Release\iw8-zonetool.exe`.

## How to use

```powershell
iw8-zonetool.exe build-iw3 C:\Maps\mp_example\mp_example.ff mp_example `
    -o C:\Maps\mp_example\converted `
    --replay    'C:\Games\MW2019\game_dx12_ship_replay.exe' `
    --unlinker  'C:\Tools\OpenAssetTools\Unlinker.exe' `
    --iw3-root  'C:\Games\Call of Duty 4 Modern Warfare' `
    --gsc-tool  'C:\Tools\gsc-tool.exe' `
    --metadata  C:\Maps\mp_example\map.json
```

- The second argument is the map id. It must start with `mp_`, use only lower-case letters, digits
  and underscores, and be at most 15 characters. It defaults to the fastfile name.
- Keep the map's `.ff`, its `_load.ff` and any `.iwd` files together; all of them are read.
- Extraction happens in a temporary folder next to the output and is deleted afterwards. Large maps
  need a few GB of free space on that drive.

### Output

| File | Contents |
| --- | --- |
| `<id>.ff` | The map. |
| `techsets_<id>.ff` | Its shaders and materials. |
| `srv_<id>.ff`, `eng_<id>.ff`, `ww_<id>.ff` | Companion zones (empty). |
| `map.json` | Map id, title and description. |
| `preview.rgba` | Loading-screen image, when the CoD4 map has one. |

### Options

| Option | Meaning |
| --- | --- |
| `-o <folder>` | Output folder. Default: `<map id>_iw8` next to the source fastfile. |
| `--replay <exe>` | Path to `game_dx12_ship_replay.exe` (required). |
| `--unlinker <exe>` | Path to OpenAssetTools `Unlinker.exe`. Also found next to the converter or through `IW8_ZONETOOL_UNLINKER`. |
| `--iw3-root <folder>` | CoD4 installation folder. Also read from `IW3_GAME_ROOT` or `COD4_ROOT`. |
| `--search-path <folder>` | Extra folder to search for CoD4 assets (repeatable). |
| `--gsc-tool <exe>` | gsc-tool used to compile the map scripts. Also read from `IW8_GSC_TOOL`. |
| `--metadata <map.json>` | Map title, description and minimap rotation (see below). |
| `--style iw3` / `--style iw8` | Visual style, see [Styles](#styles). Default: `iw3`. |
| `--no-navigation` | Skip the generated bot navigation. |
| `--iw3-mode-assets on\|off` | Keep CoD4's Domination flag and Headquarters laptop models. Default: `on`. |
| `-v` / `-q` | More / less console output. |

### Map metadata

```json
{
  "id": "mp_example",
  "name": "Example",
  "description": "Converted from Call of Duty 4",
  "compassRotation": 90
}
```

All fields are optional. `name` (or `title`) is the map's display name. `compassRotation`
(0, 90, 180 or 270, clockwise) turns the minimap image when it was drawn at a different orientation
than the map.

### Styles

| | `--style iw3` (classic) | `--style iw8` (modern) |
| --- | --- | --- |
| Sun lighting and shadows | IW3 baked sun | IW8 real-time sun shadows |
| Specular and reflections | IW3 specular | Reflection probes, physically based specular |
| Sky | IW3 sky | IW3 sky with a sun disc and glow |
| Effects | IW3 effects | IW8 effects for vehicles, explosions, smoke and glass |
| Bullet impacts | IW3 impact effects | IW8 impact effects and marks |
| Fog and colour grading | IW3 | IW3 |

## Commands

* `build-iw3 <map.ff> [map id] [options]`: Converts an IW3 multiplayer map (see above)
* `build-map-images -o <folder> <map id>=<preview.rgba>...`: Builds one zone holding the loading-screen
  images of several converted maps, as `loading_<map id>_bg` materials
* `translate-iw3-script <maps\mp folder> <map> -o <folder>`: Translates an IW3 map script to IW8 GSC
  without building the map
* `validate-output <folder> <map id>`: Checks a converted map
* `inspect <file.ff>`: Prints a fastfile's header and contents
* `build-weapon <weapon.json>`: Builds a custom weapon, see [docs/CUSTOM_WEAPONS.md](docs/CUSTOM_WEAPONS.md)
* `sim <map.sim.json> <command>`: Renders and checks a converted map without starting the game, see
  [Map simulator](#map-simulator)

## Map simulator

The simulator renders a converted map with the IW8 shaders and data the converter wrote into its
zones, so a map can be looked at and checked without loading it in the game. It runs headless on
Direct3D 11 and can also serve a live web viewer with a free-flying camera.

**Exact:** the map's own compiled IW8 vertex and pixel shaders, its materials, textures, geometry
(world, static models, dynamic and script models, glass), baked lighting, sun, reflection probes and
fog, drawn in the engine's pass order (depth prepass, opaque, then blended layers by sort key) with
the engine's depth and polygon-offset settings.
**Approximated:** inputs the engine computes every frame are held constant: screen-space ambient
occlusion, real-time shadow cascades, screen-space reflections, local lights, decals and texture
streaming. Effects, animation and physics are not run.

### Getting the simulator data

Build the map with the environment variable `IW8_ZONETOOL_KEEP_SCRATCH=1`. The converter then keeps
its intermediate folder (`iw3*.tmp` next to the output) and writes
`prepared\maps\mp\<map id>.d3dbsp.sim.json` with its `.sim.bin` there.

```powershell
$env:IW8_ZONETOOL_KEEP_SCRATCH = 1
iw8-zonetool.exe build-iw3 C:\Maps\mp_example\mp_example.ff mp_example -o C:\Maps\mp_example\converted ...
iw8-zonetool.exe sim C:\Maps\mp_example\iw3XXXX.tmp\prepared\maps\mp\mp_example.d3dbsp.sim.json render -o shot.png --spawn 0
```

### Simulator commands

| Command | Meaning |
| --- | --- |
| `render -o <file.png\|jpg>` | One frame from `--camera` or `--spawn N`. |
| `pick --at x,y` | Material, model and placement under a pixel. |
| `info` | Map statistics, bounds and spawns as JSON. |
| `serve [--port 8765] [--auth-file <file>]` | Web viewer: fly with WASD and the mouse, Q/E up and down, debug views, picking. `--auth-file` holds `user:password` for HTTP basic auth. |
| `panorama -o <folder>` | 360° images from every spawn and a viewer page for them. |
| `check --package <folder>` | Load checks on the built map, see [Map checks](#map-checks). |
| `flicker [-o report.json]` | Draws each view in normal and reversed order and reports surfaces that swap, the cause of flickering layers. |
| `rotation [-o report.json]` | Reports surfaces whose shading changes when only the camera turns. |

| Option | Meaning |
| --- | --- |
| `--camera x,y,z,yaw,pitch[,fov]` | Camera position and angles (degrees, CoD convention). |
| `--spawn N` | Camera at spawn point N, at eye height. |
| `--size WxH` / `--fov F` | Image size and horizontal field of view. |
| `--view <name>` | `exact` (default), `albedo`, `normals`, `surface`, `material`, `lightmap`, `sun`, `cull`, `depth`. |
| `--exposure E` / `--lod N` | Exposure; force a model LOD. |
| `--no-world`, `--no-models`, `--no-dynamic`, `--no-glass` | Leave a part of the map out. |
| `--points spawns\|x,y,z;...` | Viewpoints for `panorama`, `flicker` and `rotation`. Default: every spawn. |
| `--opaque-only` | Leave out blended layers (picks the surface under a decal). |
| `--reverse-order` | Draw in reverse order within each pass. |

### Map checks

`tools\mapcheck.py <built map folder>` (or `sim ... check --package <folder>`) loads the built map
through the game's own loaders using ACTS and reports:

* **load**: every asset of the map zone loads, with failures listed.
* **preload**: each zone's streamed data fits the space its header reserves (misaligned or
  overrunning streams fail here).
* **script**: the compiled map script's bytecode and string table are consistent, and every call into
  the map's own script names a function it defines.
* **entities**: spawn and objective counts for each game mode.

Pass `--json` for a machine-readable report. ACTS, the game executable and its Oodle DLL are taken
from `--acts`, `--game` and `--oodle`, or `IW8_SIM_ACTS`, `IW8_SIM_GAME` and `IW8_SIM_OODLE`.

## Asset Conversion Support

Legend:
- ✔️ Supported
- ⚠️ Partially supported
- ❌ Not supported

| **Asset type** | **IW3 → IW8** | **Notes** |
|----------------|:-------------:|-----------|
| GfxWorld | ✔️ | Geometry, lightmaps, light grid, reflection probes, sun and shadows, sky, visibility, static model placements, fog |
| clipMap_t | ✔️ | Havok collision with surface types, brush models, triggers, ladders, stair ramps |
| ComWorld | ✔️ | Sun and primary lights |
| GameWorldMp | ✔️ | Including breakable glass |
| MapEnts | ✔️ | Spawns, game mode objectives, script entities, triggers |
| XModel | ✔️ | All LODs, cull distances, skeletons, dynamic and destructible models, model collision |
| Material | ✔️ | Rebuilt on IW8 shaders |
| GfxImage | ✔️ | Recompressed for IW8 |
| FxImpactTable | ✔️ | |
| FxEffectDef | ⚠️ | Effects the map uses. Trail, spot light and sound elements are not supported |
| snd_alias_list_t | ⚠️ | Aliases the map uses, built into the map's sound bank |
| SndCurve | ⚠️ | Falloff curves of converted aliases |
| LoadedSound | ⚠️ | Audio of converted aliases |
| PhysPreset | ⚠️ | Presets used by dynamic models |
| GfxLightDef | ⚠️ | `light_point_linear` only |
| RawFile | ⚠️ | Map GSC (including createfx and createart) is translated to IW8 and compiled; other map raw files are embedded |
| MaterialTechniqueSet | ❌ | Replaced by generated IW8 shaders |
| XAnimParts | ❌ | Animated models stay in their resting pose |
| XModelPieces | ❌ | |
| WeaponDef | ❌ | |
| MenuList | ❌ | |
| menuDef_t | ❌ | |
| Font_s | ❌ | |
| LocalizeEntry | ❌ | |
| StringTable | ❌ | |
| SndDriverGlobals | ❌ | |
| XModelAlias | ❌ | |
| AiType | ❌ | |
| Character | ❌ | |
| MpType | ❌ | |
| uiMap | ❌ | |
| GameWorldSp | ❌ | Multiplayer maps only |
| clipMap_t (single-player) | ❌ | Multiplayer maps only |

Generated for every map (no IW3 equivalent):

* Bot navigation: navmesh, path nodes and tactical graph
* Colour grading and vision settings from the map's IW3 film and fog settings
* Minimap setup from the map's `minimap_corner` entities and compass image

### Game modes

| **Mode** | **IW3 → IW8** | **Notes** |
|----------|:-------------:|-----------|
| Team Deathmatch | ✔️ | |
| Free-for-all | ✔️ | |
| Domination | ✔️ | Spawns and flags |
| Headquarters / Hardpoint | ✔️ | Spawns and zones |
| Capture the Flag | ✔️ | From the map's CTF or Sabotage layout |
| Search and Destroy | ⚠️ | Spawns only; bomb sites are not converted |
| Sabotage | ❌ | Not available in IW8 |

## Known Issues

* Only multiplayer maps can be converted.
* With `--style iw3`, bullet holes do not appear on surfaces.
* Animated props (swinging lamps, flags) do not move.

## More

* [docs/USAGE.md](docs/USAGE.md) and [docs/IW3_FASTFILE.md](docs/IW3_FASTFILE.md): detailed usage
* [THIRD_PARTY.md](THIRD_PARTY.md): third-party code and licences
