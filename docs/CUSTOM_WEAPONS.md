# Replay 1.20 custom weapon workbench

The weapon workbench is a local web editor backed by ZoneTool's native Replay 1.20 weapon
writer. It creates standalone custom weapon packages for every stock multiplayer weapon
reference in the supported build. The reference contributes the engine-compatible default
graph; the project owns only the weapon data, models, materials, attachment packages,
animations, effects, and audio that the author changes.

The editor is in [`tools/weapon_editor`](../tools/weapon_editor). Start it with:

```powershell
python .\iw8-zonetool\tools\weapon_editor\server.py --port 8766
```

Then open `http://127.0.0.1:8766/`. Projects and imported source files are stored under
`%LOCALAPPDATA%\ReplayWeaponEditor`; they are not served outside the local machine. The editor
uses `%LOCALAPPDATA%\ReplayWeaponEditor\bin\iw8-zonetool.exe` when building.

## Authoring workflow

1. Create a weapon and choose the closest stock reference. The catalog covers all 167 stock
   multiplayer weapon definitions and every weapon family represented by them.
2. Import an OBJ, FBX, GLB, or glTF model. The editor converts exchange formats to the native
   OBJ/rig inputs used by the writer. Assign rigid parts or vertex weights to Replay bones.
3. Enable the operator-arms button in the viewport. This is one persistent toggle shared by
   model, rig, attachment, and animation views, so the same hands remain visible while placing
   the grip, IK locators, moving parts, and animated bones.
4. Import the model's companion material files and images with the model in the same file picker. Embedded FBX and glTF images are extracted before the material maps appear. OBJ materials load
   from the selected .mtl file; FBX and glTF material slots are retained separately. In
   **Imported material maps**, review each surface and select its base-color, normal, roughness,
   metallic, ambient-occlusion, specular, and emissive images. Filename matching ignores a
   .jpg/.jpeg extension mismatch, and every map can be reassigned manually.
5. Add compatible stock attachments or clone an attachment package. A cloned attachment can
   own custom view/world geometry, per-surface PBR maps, its bone, and an independent placement
   matrix. The assembled weapon and custom attachments are visible and selectable in the same
   viewport; attachment texture maps can be reassigned in the attachment inspector.
6. Import animated FBX or GLB clips, clone the relevant animation package, and bind each clip to
   one of that package's native XAnim event slots. The editor replaces the package's stock source
   wherever the weapon used it and retains the displaced animation for undo or remapping. Configure
   looping, notetracks, IK type, and finger pose, then preview the result on the weapon and visible
   operator arms. Unbound clips remain available as preview-only XAnim assets and produce a build
   warning until they are mapped.
7. Import PCM WAV files on the Sound page. Each source becomes a native alias in a resident SAB;
   map it to any exposed SFX event and set player/world/mechanical/UI defaults, volume, pitch,
   distance, and looping.
8. Clone native animation, SFX, and VFX packages when the weapon needs owned package records.
   The complete field inspector can change scalar, enum, string, script-string, record, and asset
   references while preserving the reference's native relocation graph.
9. Build from the Builds page. Validation errors identify the source file, field, asset, or graph
   that cannot be emitted safely.

The Data layout page documents 269 native types, 96 serialized record layouts, and 2,121 mapped
members from the supported Replay schema. It also shows every preserved padding or still-unmapped
range at its exact byte offset, accounting for the full record without inventing semantics.
Layout/runtime fields remain visible but read-only. This prevents pointer values, counts derived
from owned arrays, and internal runtime state from being authored as ordinary gameplay values.

## Native package layout

`build-weapon --project <build.json>` emits a base group and a common group. Each group follows
Replay's normal companion-zone contract:

```text
<weapon>.ff
techsets_<weapon>.ff
ww_<weapon>.ff
eng_<weapon>.ff
<weapon>_common.ff
techsets_<weapon>_common.ff
ww_<weapon>_common.ff
eng_<weapon>_common.ff
<weapon>.weapon.json
```

The base group owns the `WeaponCompleteDef`, loadout registration strings and tables, localized
UI strings, custom base models, materials, images, attachment definitions, and attachment models.
The common group owns the full SFX package, animation/effect packages, custom XAnim assets, the
transient SndBank, and its resident StreamKey/SAB data. Keeping those assets in their stock
lifetime prevents weapon references from surviving the zones that own their backing data.

The registration metadata tells mw120rproxy which base and common groups to request. The proxy
uses the normal fastfile loader and loadout enumeration path. It does not substitute models at
render time. Existing stock weapons remain present; a custom weapon receives its own reserved
loadout ordinal.

## Model, rig, and attachment rules

- View and world models are separate native XModels generated from the same source unless the
  project supplies different transforms.
- When custom geometry is built, the compiler redirects every base-weapon render slot in
  `WeaponDef` to that custom view/world pair, including streamed defaults and left/right or
  censorship variants. The stock inspection rig is normalized again at import and build time, so
  older projects cannot leave a reference fallback mesh attached beside the custom model. Hand,
  shadow, collision, stow, and projectile models are separate native assets and retain their own
  roles.
- The template wireframe is an editor-only alignment aid. It is not written to the weapon fastfiles.
- Imported materials keep their per-surface assignments through OBJ conversion and native XModel
  writing. The Replay weapon profile stores color/specular, normal/gloss, and emissive images;
  ambient occlusion is multiplied into color, roughness is inverted into gloss, and metallic maps
  contribute to the available specular channel because this profile has no separate metal slot.
- Custom attachment materials use the same conversion, per-face material keys, project-local image
  storage, and native techset registration as the main weapon. The fastfile names each attachment's
  material and images independently so two attachments may reuse a source material key safely.
- Embedded FBX and glTF images are extracted into the project. Missing or incorrectly linked images can
  be reassigned per material in **Imported material maps** before building.
- The rig stores Replay bone names, parent-relative bind transforms, optional per-part rigid
  assignments, per-vertex weights, material references, and view/world placement matrices.
- Imported attachment geometry is emitted as `<attachment>/custom_vm` and
  `<attachment>/custom_wm`. The owned attachment's native model-variation dependencies are
  rewritten to those assets; attachments without model dependencies are rejected rather than
  silently producing unused geometry.
- Operator arms are editor preview data. They provide the stock viewhands skeleton and mesh for
  placement and animation review; they are not duplicated into every custom weapon package.
- A source reference may still require weapon-specific animation or attachment tuning. Choosing
  the closest stock reference gives the safest starting graph.

## Audio and animation rules

PCM WAV input supports mono or stereo, 8/16/24/32-bit integer samples. The authoring server
normalizes it to signed 16-bit samples and writes Replay's resident raw-FLAC framing, SAB header,
entry hashes, and checksums. The native SndBank owns aliases which reference those SAB entry IDs;
the SFX package points weapon events at the aliases.

Custom animation clips are emitted as native XAnimParts with Replay script strings, bone tracks,
short or long frame indices, notetracks, frequency, looping state, IK type, and finger-pose type.
Animation preview drives matching weapon and operator-hand bones from the same imported tracks.

## Validation

The following focused checks live beside the editor:

```powershell
python .\iw8-zonetool\tools\weapon_editor\check_animation.py --help
python .\iw8-zonetool\tools\weapon_editor\check_animation_binding.py --help
python .\iw8-zonetool\tools\weapon_editor\check_sound.py --help
python .\iw8-zonetool\tools\weapon_editor\check_native_sound.py --help
python .\iw8-zonetool\tools\weapon_editor\check_native_attachment.py --help
python .\iw8-zonetool\tools\weapon_editor\check_native.py --help
python .\iw8-zonetool\tools\weapon_editor\check_all_weapon_reload.py --help
python .\iw8-zonetool\tools\weapon_editor\check_registration.py --help
python .\iw8-zonetool\tools\weapon_editor\check_assets.py --help
```

The native checks build fastfiles and reload them through the matching ACTS `mw19replay` reader.
They cover short/long animation indices, a weapon-to-owned-package-to-custom-XAnim gameplay
binding, attachment-owned view/world XModels, resident SAB decode, SndBank aliases, SFX event
mapping, every stock weapon reference, and imported native package graphs.
`check_all_weapon_reload.py` also reloads every generated stock-reference weapon zone
and verifies its weapon, NetConstStrings, and StringTable asset counts. This establishes
serialization and loader compatibility. `check_registration.py` exercises loadout registration,
Gunsmith tables, native attachment maps, and custom attachment rows across every reference.
Final gameplay behavior,
first-person placement, third-person/world placement, animation event timing, and sound mix still
require a live Replay test.

Use only the supported Replay executable and assets extracted from that exact build. Numeric
layouts, asset IDs, and loader behavior from another IW8 build are not interchangeable.
