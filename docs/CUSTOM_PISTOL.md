# Standalone custom pistol fastfiles

The Replay 1.20 compiler emits a separate base fastfile with native companions per custom gun. The proxy adds a new pistol
entry without replacing any stock weapon. This first profile has no attachments.

## Pocket Blaster test package

The CC0 [Kenney Blaster Kit](https://kenney.nl/assets/blaster-kit) supplies `blaster-a.obj`.
Its view/world models have 878 triangles each and a 30-bone skeleton joined from the matching
Replay 1911 receiver, slide and magazine skeletons. Those are bones in one model, not attachments.
The base name is `iw8_cw_pocket_blaster`; its native weapon is `iw8_cw_pocket_blaster_mp`.

The mesh uses its own `iw8_cw_pocket_blaster/material` asset with Kenney's palette and imported
OBJ UVs, plus owned normal/gloss and black emissive textures. It uses a native single-UV lit
model shader. Animations remain stock. Grip placement, muzzle
alignment, animation and audio behavior need the game test.

## Build flags

From the repository root, using the isolated compiler provided with this test:

```powershell
& .\zt_task_notes\custom-pistol\iw8-zonetool.exe build-weapon `
    --reference .\custom_weapon_sources\kenney_blaster\reference.json `
    --rig .\custom_weapon_sources\kenney_blaster\rig.json `
    --model .\custom_weapon_sources\kenney_blaster\blaster-a.obj `
    --material .\custom_weapon_sources\kenney_blaster\material\material.json `
    --name iw8_cw_pocket_blaster --display-name 'Pocket Blaster' `
    --loadout-slot 61 --rpm 400 --mag-size 12 --start-ammo 36 --max-ammo 72 --damage 30 `
    --fire-sound weap_decho_fire_npc --fire-sound-player weap_decho_fire_plr `
    --sound-bank weapon_pi_decho.all -o .\output_ff\custom_pistol
```

This emits `iw8_cw_pocket_blaster.ff`, `techsets_iw8_cw_pocket_blaster.ff`,
`ww_iw8_cw_pocket_blaster.ff`, `eng_iw8_cw_pocket_blaster.ff`, and registration metadata
`iw8_cw_pocket_blaster.weapon.json`. It also emits `iw8_cw_pocket_blaster_common.ff` and that
zone's `techsets_`, `ww_`, and `eng_` companions: eight fastfiles total, using metadata format
`replay-custom-weapon-v2`. The English global zone contains the native localized title and
description. The global shader companion owns the custom material and three resident images,
referencing the verified stock single-UV shader. The other shader/worldwide companions are
valid empty zones. Replay requests and manages both groups through its normal loader.
The global file contains a name-only reference to the custom SFX package. The full SFX package
and its bank dependency are in the common file, loaded with `common_mp` ownership. This follows
the stock weapon/SFX split and prevents a global bank reference surviving common-zone unload.
The base zone also contains `mp/gunsmith/cw_pocket_blaster_progression.csv`: one level-zero
row (displayed as level one), with no unlocks. The selection menu requires this table even
when attachments are disabled. Localize asset names are lowercase, matching native UI lookup.
It also includes `mp/gunsmith/cw_pocket_blaster_variants.csv`, containing variant zero,
the custom base/asset names and empty attachment columns. The native match script resolves
the loot table through this variant table even for a base-only gun.
The writer clears inherited transient-model flags because both custom meshes are resident.
`--material` accepts `replay-weapon-material-v1`, with three adjacent RGBA8 image files and
the verified native parameter layout. `tools/prepare_weapon_material.py` prepares this from
a local matching material export and the source palette. Textured models require valid OBJ
texture coordinates on every face corner. The importer preserves UV seams and flips V for
native texture sampling. Borrowed self-visibility materials (`mo/`, `mco/`, `m2o/`, `m2co/`)
are rejected for this simple surface layout; they require a different vertex declaration.
Replay model triangles use clockwise winding relative to their outward normals. The OBJ
importer reverses corner order for orientation-preserving transforms, retaining vertex/UV
associations; reflected transforms already reverse the winding. Singular transforms fail.
The base also contains `ncs_wep_zz_iw8_cw_pocket_blaster`, a native NetConstStrings asset
listing `iw8_cw_pocket_blaster_mp`. Loading the weapon asset alone does not register a runtime
weapon index: Replay's normal client/server setup enumerates these lists. The custom list
uses Replay weapon type 14, common source 0, flags 0 and sorts after stock common lists.
The example
uses semi-automatic fire, a 150 ms interval, and Desert Eagle firing sounds. Other damage
locations/distances retain the stock 1911 settings. `--min-damage` changes its minimum body damage.

Names must start with `iw8_cw_`, use lowercase letters/digits/underscores, omit `_mp`, and fit
48 characters. Each installed gun needs a unique `--loadout-slot` from 61 through 254.
Keep that slot when rebuilding; do not reuse a saved gun's slot. The default is 61.

Sound flags select existing native aliases/banks; they do not import WAV/FLAC. Explicit firing
aliases also replace inherited first/last/akimbo alternatives and clear inherited atmosphere
tails. An exact `--set` can restore a desired tail.

```powershell
& .\zt_task_notes\custom-pistol\iw8-zonetool.exe build-weapon `
    --reference .\custom_weapon_sources\kenney_blaster\reference.json --list-fields
```

The listing shows field names, types, record-relative offsets and values. `--set exact.field=value`
edits scalar, enum, string and script-string fields. Examples:

```text
--set weapon.weapDef[0].lowAmmoWarningThreshold=0.25
--set weapon.weapDef[0].damageInfo.damageData[0].maxDamageRange=700
--set sfx.sounds[0].fireSoundAtmospherePlayer.name=weap_decho_fire_plr_atmo
```

Array counts, runtime indices, attachment counts and compiler-owned identity fields are read-only.
Pointer topology comes from the prepared reference. Model names are remapped by the rig's
`replace` lists. The full field listing is schema-derived; not every obscure field's gameplay
meaning has been independently verified.

## Confirmed reference fields

Offsets below are hexadecimal within the named native record, not process/file addresses.

| Control | Record and field | Offset | Stock 1911 |
|---|---|---:|---|
| Internal name | WeaponCompleteDef.szInternalName | 0x000 | iw8_pi_mike1911_mp |
| Display name | WeaponCompleteDef.szDisplayName | 0x010 | WEAPON/PI_MIKE1911 |
| Magazine | WeaponCompleteDef.iClipSize | 0x1C0 | 7 |
| View model | WeaponDef.gunXModel | 0x008 | weapon_vm_pi_mike1911 |
| World model | WeaponDef.worldModel | 0x038 | weapon_wm_pi_mike1911 |
| Near body damage | WeaponDef.damageInfo.damageData[0].damage | 0x0D0 | 33 |
| Minimum body damage | WeaponDef.damageInfo.damageData[0].minDamage | 0x0C0 | 23 |
| Fire interval | WeaponDef.iFireTime | 0x140 | 210 ms |
| Akimbo interval | WeaponDef.iFireTimeAkimbo | 0x148 | 210 ms |
| VFX package | WeaponDef.vfxPackage | 0x200 | stock VFX |
| SFX package | WeaponDef.sfxPackage | 0x208 | stock SFX |
| Starting ammo | WeaponDef.iStartAmmo | 0x3A4 | 21 |
| Ammo name | WeaponDef.szAmmoName | 0x3B0 | weapon/ammo_45_pist |
| Maximum ammo | WeaponDef.iMaxAmmo | 0x3BC | 42 |
| NPC firing alias | WeaponSFXPackageSounds.fireSound.name | 0x060 | weap_mike1911_fire_npc |
| Player firing alias | WeaponSFXPackageSounds.fireSoundPlayer.name | 0x068 | weap_mike1911_fire_plr |
| Detail bank | WeaponSFXPackage.detailSoundBankPlayer | 0x020 | weapon_pi_mike1911.all |

Replay sizes: WeaponCompleteDef 0x270, WeaponDef 0x14B0, WeaponSFXPackage 0xB0,
WeaponSFXPackageSounds 0x308. Other IW8 builds differ. The base pistol XModels are empty
placeholders; use the component transients to obtain the real skeleton.

`tools/prepare_weapon_reference.py` imports the matching local weapon/SFX/schema/script-string
exports. The 1911 weapon lives in `global_stream_mp.ff`; its SFX package lives in `common_mp.ff`.
`prepare_pistol_rig.py` joins local receiver/slide/magazine skeleton exports. Generated stock
references are ignored by Git and must not be redistributed.

## Proxy integration and saved data

At startup, the proxy discovers `zone/iw8_cw_*.weapon.json` and verifies all eight package fastfiles.
It appends global weapon requests to `global_stream_mp` loads and common SFX requests to
`common_mp` loads, inheriting each corresponding owner's flags, priority and failure policy.
Existing disk-routing and unsigned-reader
hooks recognize these packages. The native loader owns the assets and their rendering/lifetime.

The existing asset-lookup hook returns extended copies of `mp/statsTable.csv` and
`loot/weapon_ids.csv`, preserving every stock row. Localized titles come from the English fastfile.
The new pistol's attachment/customization editor is disabled; its weapon-selection entry remains.
Stat IDs continue consecutively after the stock table (71 for this package). Replay's `16`
script initializes its weapon data by looking up IDs 1, 2, ... until the first missing ID;
a sparse high ID makes the gun invisible to match loadout validation and triggers the 1911
fallback. These transient stat IDs are separate from stable loot IDs and saved DDL ordinals.

`LoadoutWeapon` also indexes a fixed `weapon_xp[61]` array in private/ranked saves. The proxy
preserves it and adds `CustomLoadoutWeapon` only for 8-bit scalar weapon-choice fields. Stock
IDs, bit offsets/widths, GUIDs, versions, XP arrays and archived schemas remain unchanged.
Custom weapon XP progression is not added in this first profile; the native UI progression
table describes a single available level with no attachments or cosmetic unlocks.

The native pistol-selection restriction check also reads `ddl/mp/recipes.ddl`. Its stock
61-bit `CommonOptions.weaponRestricted` array is extended to 256 independent bits using
the same custom IDs. Following fields and the root's offsets are adjusted; the recipe is
4024 bytes, within Replay's existing 4096-byte buffer. Custom recipe version 33231 has a
distinct layout GUID and retains stock version 463 in its definition chain. Replay's own
`DDL_CreateContext` converts older recipes by field name. Stock loadout/XP enum arrays remain
unchanged; only the restriction array and scalar weapon choices use the extended enum.
The fixed 256-entry recipe layout remains stable as installed weapons change.

The local `custom-weapon-tests` target accepts an optional executable-path argument naming
the exact Replay executable. It maps the image without launching its entrypoint and exercises
its native DDL initialization, stock-to-custom conversion, name lookup and restriction reads.
The current fixture checks all 1834 existing recipe fields and independent custom restrictions.
It also calls Replay's native weapon-name sort comparator and common-weapon enumerator.
With the extracted 167-entry global-stream list, all original ordinals remain intact and
the custom entry is visited next. This is an isolated enumeration test, not live gameplay proof.
Bounded weapon lookup diagnostics use the normal `mw120rproxy.log` and record requested/resolved
weapon names, clip size, model names and caller RVA, independently of verbose dvar tracing.

## Deploy and test

With Replay closed:

```powershell
.\mw120rproxy\tools\deploy_custom_weapon.ps1 -GameRoot 'E:\IW8\Builds\1.20-replay' `
    -PackageDir .\output_ff\custom_pistol `
    -ProxyPath .\zt_task_notes\custom-pistol\XInput9_1_0.dll
```

The helper verifies the executable, stages files, backs up previous files, checks hashes and
rolls back copy failures. It preserves the INI, maps and stock fastfiles and never starts/stops
Replay. Specify `-WeaponName` when the authoring directory contains multiple packages.

Test Local Multiplayer → weapon selection → Pistols → **Pocket Blaster**, save/reopen the
loadout, then enter a match. Check first/third-person views, muzzle position, firing, reloading,
12-round magazine and sounds. Confirm the stock 1911 and Combat Knife remain available.
Offline validation is not live menu/rendering proof.

If loading fails, retain `mw120rproxy.log`, `mw120rproxy.engine.log`, and
`mw120rproxy.exceptions.log`. The normal log includes weapon identity and zone ownership;
verbose tracing is optional. The old knife replacement remains disabled.
To disable this test, move its `.weapon.json` and eight `.ff` files outside `zone` with Replay closed.
Keep a newer DLL from the map session; disabling the package does not require reverting it.

The September 16 failure on entering Local Multiplayer occurred after the base FF loaded:
missing `ww_`/`eng_` companions were recorded as optional failures against zone flags `0x2`,
marking `global_stream_mp,global_mp` failed. The frontend subsequently raised
`MENU/CONTENT_NOT_AVAILABLE`. Generate/deploy the complete package; companion requests and
native load-failure handling must not be suppressed.
