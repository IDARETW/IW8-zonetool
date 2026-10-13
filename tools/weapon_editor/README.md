# Weapon editor

This directory contains the local Replay 1.20 custom weapon workbench described in
[`docs/CUSTOM_WEAPONS.md`](../../docs/CUSTOM_WEAPONS.md).

Run it from the repository root:

```powershell
python .\iw8-zonetool\tools\weapon_editor\server.py --port 8766
```

Open `http://127.0.0.1:8766/`. The server keeps the editor library, projects, imports, build
outputs, checks, and its isolated ZoneTool binary under
`%LOCALAPPDATA%\ReplayWeaponEditor`.

Its browser UI, Three.js viewport, uploads, mutations, and builds are served by `server.py` on
loopback. `web/app.js` owns the authoring workflow and
`web/viewer.js` owns model, attachment, rig, operator-arms, and animation preview behavior.
`sound.py` writes the resident SAB used by the native C++ SndBank writer.

Every import prompt can use either the browser device or the server machine. Server browsing is
limited to `server_file_roots` in `config.local.json`, to supported weapon source extensions, and
to the same 128 MiB source-file limit as browser uploads (512 MiB for converted OBJ files). Paths are resolved beneath the configured
root before a directory is listed or a file is read; junctions and `..` paths cannot escape it.
Large downloads use byte ranges and uploads use 4 MiB chunks, with progress shown in the editor.
GLB/glTF conversion runs in a module worker and preserves indexed vertices. Custom-weapon
viewport previews use a compact GLB derived from the exact OBJ read by the native fastfile
writer. Mesh part names, triangle count, and model placement therefore match the compiled
geometry. The editor transports material previews at up to 1024 pixels per side while the
fastfile writer continues to use the original full-resolution RGBA images. Embedded GLB
materials are not imported automatically;
use the material controls for color, normal, and emissive textures.

For a protected Cloudflare Quick Tunnel, create a random token file, start the server with
`--access-token-file <file>`, then point `cloudflared tunnel --url http://127.0.0.1:8766` at it and
set `--http-host-header 127.0.0.1:8766`. Open
`https://<tunnel-host>/access/<token>`. The server validates the key and serves the editor directly
with its credential embedded in the non-cacheable response. There is no authentication redirect
or cookie requirement. The protected path stays in the address bar, so reloads and bookmarks work
even with browser storage disabled. The editor authenticates API and asset requests explicitly.
The older `#access=<token>` and `?token=<token>` links remain accepted. Opening the plain URL
without a valid session shows an unlock form that accepts either the key or a complete protected
link; the editor waits for successful authentication before loading project data.
Keep the complete protected URL private. The origin server remains bound only to `127.0.0.1`;
unauthenticated static editor code contains no project or server-file data. Server request logs
redact access keys in paths and query strings.

The `check_*.py` scripts are focused native regression tools. Use `--help` on a script for its
required local Replay, ACTS, compiler, library, project, and output paths.

The Animation page previews clips referenced by copied and stock animation packages. Choose a
package and clip, then use Play, Pause/Resume, the timeline, speed, and Loop. The viewport's single
operator-arms toggle applies to playback as well as model and rig editing. Reset pose returns to
the authored placement. The Model page has numeric position, rotation, and scale controls, plus
optional move/rotate/scale snapping. The reference-wireframe toolbar button loads the selected
weapon's stock view/world geometry as a non-exported placement guide. Model and attachment transform saves preserve the live mesh and gizmo,
so consecutive drags do not reload geometry or require selecting the transform tool again.
Imported base models use one placement for view and world by default. The Model page can unlink
them when a weapon needs distinct poses. Importing a complete replacement model removes borrowed
stock attachment slots, which otherwise draw the reference receiver, grip, magazine, and sights
over custom geometry; explicitly owned custom attachments remain available.
With operator arms shown, the left and right hand targets on the Rig page move the preview arms
live through the editable native `tag_ik_loc_le` and `tag_ik_loc_ri` bones emitted by the build.
Their gizmos and numeric position/rotation fields also work while a native animation is paused.
Normal animation playback shows the native arm pose; enabling paused pose editing activates the
hand-target solver, and leaving it restores the same native frame.
At a paused resting-grip frame, **Set tags from hands** copies the native animation's actual
left and right wrist transforms into the custom weapon tags before entering placement mode.
The editor rejects this capture when a wrist is away from the compiled model, such as mid-reload.
This is a starting pose, not a replacement for checking reload, fire, and idle poses in game.
All tag markers use the same native coordinate space as the compiled mesh. A new custom model
starts with generic placeholder tags, so use **Pick on model** to place a selected tag directly
on its surface, then refine its position and rotation with the gizmo or number fields. Saving
a tag leaves the loaded model and paused frame in place. The operator-arm IK is an editor preview;
check the built weapon in Replay to confirm the game's animation and grip behavior.
On the Animation page, pause a stock clip and enable paused pose editing to select any imported bone or tag;
the gizmo and numeric fields update the frozen frame immediately and save the adjustment in the
weapon's native bind space, without baking the animation's current root motion into the tag.
Other viewport reloads prepare a complete replacement before swapping it into the scene. Slow or
failed requests retain the displayed model; superseded loads are discarded. Immutable source
resources are cached for repeated material and view changes.

Native playback uses local ACTS `mw19replay` XAnimParts exports. Set `animation_export` in
`config.local.json` to the exported zone directory containing `script_strings.json` and
`assets/xanim/*.asset.json`; the default is
`%LOCALAPPDATA%/ReplayWeaponEditor/native-animation-export/mw19replay/common_mp`.
`animation.py` decodes bounded packed streams into sparse tracks and checks complete stream
consumption. Missing clip payloads are disabled in the picker. This preview cache does not
replace or modify the game's animation assets or the project's package bindings.

Native clips animate the operator skeleton and matching weapon bones. Weapon placement uses
`tag_weapon`, with the initial clip pose centered on the authoring origin. Separately moving
magazines, bolts, and other parts need matching bone names and named-part assignments. External
GLB/FBX skin weights are not native Replay bind-space data, so custom imports default to rigid
parts until native-space weights are authored. Additive layers are previewed alone, root-motion
deltas are omitted, and animation notetracks do not yet trigger sound/effect playback.

`node check_preview.mjs <ReplayWeaponEditor directory>` checks real Three.js native reload motion,
pause/seek/reset, three consecutive model transform saves retaining geometry, and concurrent arm
loading. It uses the local Futuristic Rifle and `tunnel/native-reload-preview.json` fixture without
changing the project file.


### Load stock weapons

Use **Load stock weapon** in the header, search a native weapon name (for example,
`kilo433`, `mike1911`, or `akilo47`), then load it. This creates a separate local reference
project containing native weapon data and an assembled view/world model. The loader follows
stock default attachment tokens and native attachment tags, merges the component skeletons,
and retains original vertex skin weights, including client bones. Shared streamed geometry is
read from the installed `zone/pak_*.xpak` archives when needed. The Rig page lists native
bone/tag names; the rig overlay follows paused or playing animation poses. Selecting a tag for
editing resets playback to the bind pose. Animation uses the existing package/clip controls;
the single arms toggle applies throughout. Stock assemblies use the native `j_gun` driver.

Set `game` to the local Replay 1.20 install, `stock_exporter` to a local ACTS build with Replay
geometry **and skin weight** export, and `stock_cache` to a folder with sufficient free space.
The first request extracts model payloads from the relevant companion zones and caches the
assembled result. Extraction runs as a background job with short status requests so the tunnel
does not time out on a slow first load. No game process is needed. Native textures are not imported: the stock
weapon uses a neutral inspection material. Missing animation payloads remain visibly disabled.

The assembled stock geometry is for inspection. Builds of these projects retain native model
and attachment references; preview rig/material changes are not compiled into those stock
assets. Import a custom model to switch to the existing custom-geometry compilation workflow.
Weapon data, owned attachment, sound, and animation edits retain their existing build paths.

`python check_stock.py --library <library> --output <results.json>` extracts all selectable
weapons and checks vertex/skin correspondence and bone names. `--weapon <native-name>` narrows
that check. `node check_stock_preview.mjs <ReplayWeaponEditor directory>` checks the local
AK-47 inspection fixture for native gun alignment and independent magazine motion.
`node check_glb.mjs <ReplayWeaponEditor directory> <Scorned-revolver.glb>` checks the large GLB
fixture's indexed import and preview vertex correspondence.
