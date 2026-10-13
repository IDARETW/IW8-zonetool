# Replay 1.20 weapon fastfile layout

96 structures/unions, 2121 named members, 52 enum types.

This is the complete transitive weapon-family schema, including fields absent from the pistol reference.
External asset pointers terminate the record graph and link to their Replay pool identity and size.
A named field is not a claim that its gameplay behavior has been tested. Unmapped ranges and
source-derived pointer-count rules are explicitly reported. Never transfer addresses from another IW8 build.

Target SHA256: `68fb1cbcb2924182724004039de55a4c50152bb6561803c4898b7930b38132f0`

## Serialization

Root records use stream 0; trailing strings and owned records use stream 5. Pointers are 8 bytes;
script strings are 4-byte indices. On-disk pointers are sentinels/packed offsets, not process addresses.
An asset reference is resolved through the native asset list; a name-only dependency starts with a comma.
Member offsets below are relative to their record, not absolute compressed fastfile offsets.

WeaponDef accuracy graph pointers are consumed as name[0], knots[0], name[1], knots[1].
The writer must follow the native loader traversal, which is not always increasing field offset.

## Root assets

| Record | Size | Pool | Known Replay loader RVA |
|---|---:|---:|---|
| WeaponCompleteDef | 0x270 | 43 | 0xD99D00 |
| WeaponDef | 0x14B0 | embedded | 0xD9A580 |
| WeaponAttachment | 0x3C8 | 42 | 0xE0E480 |
| WeaponAnimPackage | 0x40 | 77 | see schema/evidence |
| WeaponSFXPackage | 0xB0 | 78 | see schema/evidence |
| WeaponVFXPackage | 0x10 | 79 | see schema/evidence |

## Complete records

### $38C3DEC81229B66F67FB6D350D75FF5A

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | x | float | "" |
| 0x4 | 4 | y | float | "" |

### $393C16A032292777F0C3725FFB2C0008

Size `0xC`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | x | float | "" |
| 0x4 | 4 | y | float | "" |
| 0x8 | 4 | z | float | "" |

### $43F634250C0E94E2A09AB0840E4770D1

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | r | float | "" |
| 0x4 | 4 | g | float | "" |
| 0x8 | 4 | b | float | "" |
| 0xC | 4 | a | float | "" |

### $91D1B2149FAC90180ECB9AC277F76009

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | x | float | "" |
| 0x4 | 4 | y | float | "" |
| 0x8 | 4 | z | float | "" |
| 0xC | 4 | w | float | "" |

### ADSOverlay

Size `0x70`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | shaderMat | Material * | "" |
| 0x8 | 8 | shaderLowResMat | Material * | "" |
| 0x10 | 8 | shaderEMPMat | Material * | "" |
| 0x18 | 8 | shaderEMPLowResMat | Material * | "" |
| 0x20 | 8 | shaderImg | GfxImage * | 1 |
| 0x28 | 8 | shaderLowResImg | GfxImage * | 1 |
| 0x30 | 8 | shaderEMPImg | GfxImage * | 1 |
| 0x38 | 8 | shaderEMPLowResImg | GfxImage * | 1 |
| 0x40 | 1 | applyVisionSet | bool | "" |
| 0x44 | 4 | reticle | weapOverlayReticle_t | "" |
| 0x48 | 4 | width | float | "" |
| 0x4C | 4 | height | float | "" |
| 0x50 | 4 | widthSplitscreen | float | "" |
| 0x54 | 4 | heightSplitscreen | float | "" |
| 0x58 | 8 | visionSetName | const char * | "" |
| 0x60 | 4 | visionSetADSFraction | float | "" |
| 0x64 | 4 | visionSetBlendInTimeMs | int | "" |
| 0x68 | 4 | visionSetBlendOutTimeMs | int | "" |

Unmapped/padding bit ranges: 520+24, 864+32.

### AdsAltSwitch

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | fovInterpTimeSecMainToAlt | float | "" |
| 0x4 | 4 | fovInterpDelaySecMainToAlt | float | "" |
| 0x8 | 4 | dualFovSwitchRateMainToAlt | float | "" |
| 0xC | 4 | fovInterpTypeMainToAlt | AdsAltSwitchInterpType | "" |
| 0x10 | 4 | fovInterpTimeSecAltToMain | float | "" |
| 0x14 | 4 | fovInterpDelaySecAltToMain | float | "" |
| 0x18 | 4 | dualFovSwitchRateAltToMain | float | "" |
| 0x1C | 4 | fovInterpTypeAltToMain | AdsAltSwitchInterpType | "" |

### AdsSwaySettings

Size `0x2C`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 28 | common | CommonSwaySettings | "" |
| 0x1C | 4 | swayTransitionLerpSpeed | float | "" |
| 0x20 | 12 | adsSwayScale | float[3] | "" |

### AdvancedHipSwaySettings

Size `0xA8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | enabled | bool | "" |
| 0x4 | 4 | torsoGoalSmoothSpeed | float | "" |
| 0x8 | 4 | torsoGoalViewSmoothDurationMs | int | "" |
| 0xC | 8 | torsoGoalDeadzoneAdjustSpeed | vec2_t | "" |
| 0x18 | 8 | torsoGoalViewSpeedToMaxDeadzone_graph | RumbleGraph * | 1 |
| 0x20 | 8 | torsoGoalViewSpeedToMaxDeadzone_viewspeed | vec2_t | "" |
| 0x28 | 8 | torsoGoalViewSpeedToMaxDeadzone_maxDeadzone | vec2_t | "" |
| 0x30 | 8 | torsoMass | vec2_t | "" |
| 0x38 | 8 | torsoSpring | vec2_t | "" |
| 0x40 | 8 | torsoDamper | vec2_t | "" |
| 0x48 | 4 | gunGoalViewSmoothDurationMs | int | "" |
| 0x50 | 8 | gunGoalViewSpeedToOffset_graph | RumbleGraph * | 1 |
| 0x58 | 8 | gunGoalViewSpeedToOffset_viewspeed | vec2_t | "" |
| 0x60 | 8 | gunGoalViewSpeedToOffset_offset | vec2_t | "" |
| 0x68 | 8 | gunMass | vec2_t | "" |
| 0x70 | 8 | gunSpring | vec2_t | "" |
| 0x78 | 8 | gunDamper | vec2_t | "" |
| 0x80 | 12 | gunPivotPoint | vec3_t | "" |
| 0x8C | 4 | gunYawToRollScale | float | "" |
| 0x90 | 4 | fireDurationMs | int | "" |
| 0x94 | 4 | fireStartBlendDurationMs | int | "" |
| 0x98 | 4 | fireFinishBlendDurationMs | int | "" |
| 0x9C | 4 | fireTorsoGoalSmoothSpeed | float | "" |
| 0xA0 | 4 | fireTorsoDeadzoneScale | float | "" |
| 0xA4 | 4 | fireTorsoToGunDirScale | float | "" |

Unmapped/padding bit ranges: 8+24, 160+32, 608+32.

### AdvancedIdleSettings

Size `0x98`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | useAdvancedIdleSettings | bool | "" |
| 0x1 | 1 | useRandomPointsAlgorithm | bool | "" |
| 0x4 | 4 | breathGaspScaleOverride | float | "" |
| 0x8 | 4 | idleSwaySetting1_HipBulletDirScale | float | "" |
| 0xC | 4 | idleSwaySetting1_HipIdleSpeed | float | "" |
| 0x10 | 4 | idleSwaySetting1_HipWeaponMagnitudeX | float | "" |
| 0x14 | 4 | idleSwaySetting1_HipWeaponMagnitudeY | float | "" |
| 0x18 | 4 | idleSwaySetting1_HipWeaponMagnitudeZ | float | "" |
| 0x1C | 4 | idleSwaySetting1_HipWeaponMagnitudeF | float | "" |
| 0x20 | 4 | idleSwaySetting1_HipWeaponRotationOffset | float | "" |
| 0x24 | 4 | idleSwaySetting1_HipViewMagnitudeX | float | "" |
| 0x28 | 4 | idleSwaySetting1_HipViewMagnitudeY | float | "" |
| 0x2C | 4 | idleSwaySetting1_AdsBulletDirScale | float | "" |
| 0x30 | 4 | idleSwaySetting1_AdsIdleSpeed | float | "" |
| 0x34 | 4 | idleSwaySetting1_AdsWeaponMagnitudeX | float | "" |
| 0x38 | 4 | idleSwaySetting1_AdsWeaponMagnitudeY | float | "" |
| 0x3C | 4 | idleSwaySetting1_AdsWeaponMagnitudeZ | float | "" |
| 0x40 | 4 | idleSwaySetting1_AdsWeaponMagnitudeF | float | "" |
| 0x44 | 4 | idleSwaySetting1_AdsWeaponRotationOffset | float | "" |
| 0x48 | 4 | idleSwaySetting1_AdsViewMagnitudeX | float | "" |
| 0x4C | 4 | idleSwaySetting1_AdsViewMagnitudeY | float | "" |
| 0x50 | 4 | idleSwaySetting2_HipBulletDirScale | float | "" |
| 0x54 | 4 | idleSwaySetting2_HipIdleSpeed | float | "" |
| 0x58 | 4 | idleSwaySetting2_HipWeaponMagnitudeX | float | "" |
| 0x5C | 4 | idleSwaySetting2_HipWeaponMagnitudeY | float | "" |
| 0x60 | 4 | idleSwaySetting2_HipWeaponMagnitudeZ | float | "" |
| 0x64 | 4 | idleSwaySetting2_HipWeaponMagnitudeF | float | "" |
| 0x68 | 4 | idleSwaySetting2_HipWeaponRotationOffset | float | "" |
| 0x6C | 4 | idleSwaySetting2_HipViewMagnitudeX | float | "" |
| 0x70 | 4 | idleSwaySetting2_HipViewMagnitudeY | float | "" |
| 0x74 | 4 | idleSwaySetting2_AdsBulletDirScale | float | "" |
| 0x78 | 4 | idleSwaySetting2_AdsIdleSpeed | float | "" |
| 0x7C | 4 | idleSwaySetting2_AdsWeaponMagnitudeX | float | "" |
| 0x80 | 4 | idleSwaySetting2_AdsWeaponMagnitudeY | float | "" |
| 0x84 | 4 | idleSwaySetting2_AdsWeaponMagnitudeZ | float | "" |
| 0x88 | 4 | idleSwaySetting2_AdsWeaponMagnitudeF | float | "" |
| 0x8C | 4 | idleSwaySetting2_AdsWeaponRotationOffset | float | "" |
| 0x90 | 4 | idleSwaySetting2_AdsViewMagnitudeX | float | "" |
| 0x94 | 4 | idleSwaySetting2_AdsViewMagnitudeY | float | "" |

Unmapped/padding bit ranges: 16+16.

### AnimOverride

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | numBindings | unsigned int | "" |
| 0x8 | 8 | bindings | OverrideBinding * | varAnimOverride->numBindings |
| 0x10 | 8 | overrides | WeaponAnimPackage * | 1 |
| 0x18 | 8 | overridesAlt | WeaponAnimPackage * | 1 |

Unmapped/padding bit ranges: 32+32.

### AttADSGunBobSettings

Size `0x1C`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | fAdsGunBobPitchScale | float | "" |
| 0x4 | 4 | fAdsGunBobYawScale | float | "" |
| 0x8 | 4 | fAdsGunBobTiltPitchScale | float | "" |
| 0xC | 4 | fAdsGunBobTiltYawScale | float | "" |
| 0x10 | 4 | fAdsGunBobTiltRollScale | float | "" |
| 0x14 | 4 | fAdsGunBobTiltBulletDirScale | float | "" |
| 0x18 | 4 | fAdsGunBobTiltOffset | float | "" |

### AttADSOverlay

Size `0xA0`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 112 | overlay | ADSOverlay | "" |
| 0x70 | 8 | hybridScopeAttachment | WeaponAttachment * | 1 |
| 0x78 | 4 | hybridScope1To2FovTransBegin | float | "" |
| 0x7C | 4 | hybridScope1To2FovTransEnd | float | "" |
| 0x80 | 4 | hybridScope2To1FovTransBegin | float | "" |
| 0x84 | 4 | hybridScope2To1FovTransEnd | float | "" |
| 0x88 | 4 | hybridScope1To2ScopeFadeTransBegin | float | "" |
| 0x8C | 4 | hybridScope1To2ScopeFadeTransEnd | float | "" |
| 0x90 | 4 | hybridScope2To1ScopeFadeTransBegin | float | "" |
| 0x94 | 4 | hybridScope2To1ScopeFadeTransEnd | float | "" |
| 0x98 | 1 | hybridScope1ShowHipGlassWhenInactive | bool | "" |
| 0x99 | 1 | hybridScope2ShowHipGlassWhenInactive | bool | "" |
| 0x9A | 1 | hybridToggle | bool | "" |
| 0x9B | 1 | thermalScope | bool | "" |
| 0x9C | 1 | thermalToggle | bool | "" |

Unmapped/padding bit ranges: 1256+24.

### AttADSReloadSettings

Size `0x1`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | reloadWhileADS | bool | "" |

### AttADSSettings

Size `0xD8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | adsSpread | float | "" |
| 0x4 | 4 | adsSpreadStartFrac | float | "" |
| 0x8 | 4 | adsSpreadEndFrac | float | "" |
| 0xC | 4 | adsAimPitch | float | "" |
| 0x10 | 4 | adsTransInTime | float | "" |
| 0x14 | 4 | adsTransOutTime | float | "" |
| 0x18 | 4 | adsTransInToOutTime | float | "" |
| 0x1C | 4 | adsTransOutToInTime | float | "" |
| 0x20 | 4 | adsReloadTransTime | int | "" |
| 0x24 | 4 | adsInCrosshairAlphaStart | float | "" |
| 0x28 | 4 | adsInCrosshairAlphaEnd | float | "" |
| 0x2C | 4 | adsOutCrosshairAlphaStart | float | "" |
| 0x30 | 4 | adsOutCrosshairAlphaEnd | float | "" |
| 0x34 | 112 | zoomSettings | ZoomSettings | "" |
| 0xA4 | 4 | adsFovLerpTime | float | "" |
| 0xA8 | 4 | adsBobFactor | float | "" |
| 0xAC | 4 | adsViewBobMult | float | "" |
| 0xB0 | 4 | adsFireRateScale | float | "" |
| 0xB4 | 4 | adsDamageRangeScale | float | "" |
| 0xB8 | 4 | adsFireAnimFrac | float | "" |
| 0xBC | 4 | fireTimerLerpToAdsScale | float | "" |
| 0xC0 | 1 | alwaysFireAtMaxRangeInAds | bool | "" |
| 0xC1 | 1 | adsAlignEnabled | bool | "" |
| 0xC2 | 1 | disableTagAlignX | bool | "" |
| 0xC4 | 12 | adsAlignOffset | vec3_t | "" |
| 0xD0 | 4 | adsAlignOffsetLerpType | AdsOffsetInterpolationType | "" |
| 0xD4 | 4 | adsAlignLerpType | AdsOffsetInterpolationType | "" |

Unmapped/padding bit ranges: 1560+8.

### AttADSStanceScales

Size `0x38`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | adsCrouchViewKickFactorScale | float | "" |
| 0x4 | 4 | adsCrouchGunKickFactorScale | float | "" |
| 0x8 | 4 | adsProneViewKickFactorScale | float | "" |
| 0xC | 4 | adsProneGunKickFactorScale | float | "" |
| 0x10 | 4 | adsCrouchGunBobFactorScale | float | "" |
| 0x18 | 16 | crouchedWeaponOffsetPatternScales | WeaponOffsetPatternScaleInfo | "" |
| 0x28 | 16 | proneWeaponOffsetPatternScales | WeaponOffsetPatternScaleInfo | "" |

Unmapped/padding bit ranges: 160+32.

### AttAddOns

Size `0x2`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | silenced | bool | "" |
| 0x1 | 1 | riotShield | bool | "" |

### AttAdsMovement

Size `0xC`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | forwardOffset | float | "" |
| 0x4 | 4 | roll | float | "" |
| 0x8 | 1 | isOverride | bool | "" |

Unmapped/padding bit ranges: 72+24.

### AttAimAssist

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | autoAimRange | float | "" |
| 0x4 | 4 | aimAssistRange | float | "" |
| 0x8 | 4 | aimAssistRangeAds | float | "" |
| 0xC | 4 | aimAssistPitchSlowdown | float | "" |
| 0x10 | 4 | aimAssistPitchSlowdownAds | float | "" |
| 0x14 | 4 | aimAssistYawSlowdown | float | "" |
| 0x18 | 4 | aimAssistYawSlowdownAds | float | "" |
| 0x1C | 4 | aimAssistLockonStrength | float | "" |

### AttAmmoGeneral

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | fireType | weapFireType_t | "" |
| 0x4 | 4 | burstCount | int | "" |

### AttAmmoTracer

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | tracerType | TracerDef * | 1 |
| 0x8 | 4 | tracerStyle | tracerStyle_t | "" |

Unmapped/padding bit ranges: 96+32.

### AttAmmoType

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | szAmmoName | const char * | "" |

### AttAmmunition

Size `0x1C`; Replay schema override.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | maxAmmo | int | "" |
| 0x4 | 4 | startAmmo | int | "" |
| 0x8 | 4 | clipSize | int | "" |
| 0xC | 4 | shotCount | int | "" |
| 0x10 | 4 | reloadAmmoAdd | int | "" |
| 0x14 | 4 | reloadStartAdd | int | "" |
| 0x18 | 4 | lowAmmoWarningCount | int | "" |

### AttBurst

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | burstFireCooldown | int | "" |
| 0x4 | 1 | burstFireAuto | bool | "" |

Unmapped/padding bit ranges: 40+24.

### AttCharged

Size `0x88`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | chargeType | WeaponChargeType | "" |
| 0x4 | 4 | chargeGain | float | "" |
| 0x8 | 4 | chargeCostPerShot | float | "" |
| 0xC | 4 | chargeLossWhenIdle | float | "" |
| 0x10 | 4 | chargeEmptyCooldown | float | "" |
| 0x14 | 4 | chargeFireAtMaxDamageMultiplier | float | "" |
| 0x18 | 4 | chargeMeterEffectBoneCount | int | "" |
| 0x20 | 8 | chargeMeterEffect | FxCombinedDef | "" |
| 0x28 | 8 | chargeUpSound | SndAliasLookup | "" |
| 0x30 | 8 | chargeDownSound | SndAliasLookup | "" |
| 0x38 | 8 | chargeDownToUpSound | SndAliasLookup | "" |
| 0x40 | 8 | chargeUpToDownSound | SndAliasLookup | "" |
| 0x48 | 8 | chargeMaxSound | SndAliasLookup | "" |
| 0x50 | 8 | chargeUpSoundPlayer | SndAliasLookup | "" |
| 0x58 | 8 | chargeDownSoundPlayer | SndAliasLookup | "" |
| 0x60 | 8 | chargeDownToUpSoundPlayer | SndAliasLookup | "" |
| 0x68 | 8 | chargeUpToDownSoundPlayer | SndAliasLookup | "" |
| 0x70 | 8 | chargeMaxSoundPlayer | SndAliasLookup | "" |
| 0x78 | 1 | chargeHudReveal | bool | "" |
| 0x80 | 8 | chargeRumble | RumbleInfo * | 1 |

Unmapped/padding bit ranges: 224+32, 968+56.

### AttDamage

Size `0x88`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 128 | damageInfo | WeaponDamageInfo | "" |
| 0x80 | 4 | deathAnimDamageType | AnimDamageType | "" |
| 0x84 | 4 | terminateAtRange | float | "" |

### AttDlcOverrides

Size `0x28`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | useFloatOverride | bool[8] | "" |
| 0x8 | 32 | floatOverride | float[8] | "" |

### AttDlcScalars

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 32 | floatScalar | float[8] | "" |

### AttGeneral

Size `0x38`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | boltAction | bool | "" |
| 0x1 | 1 | inheritsPerks | bool | "" |
| 0x2 | 1 | reticleSpin45 | bool | "" |
| 0x3 | 1 | adsShouldShowCrosshair | bool | "" |
| 0x4 | 4 | enemyCrosshairRange | float | "" |
| 0x8 | 4 | reticleType | ReticleType | "" |
| 0x10 | 8 | reticleCenter | Material * | "" |
| 0x18 | 8 | reticleSide | Material * | "" |
| 0x20 | 8 | reticleOnePiece | Material * | "" |
| 0x28 | 4 | reticleCenterSize | int | "" |
| 0x2C | 4 | reticleSideSize | int | "" |
| 0x30 | 8 | szLUICrosshairWidget | const char * | "" |

Unmapped/padding bit ranges: 96+32.

### AttGunKick

Size `0x17C`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | hipGunKickPitchMin | float | "" |
| 0x4 | 4 | hipGunKickPitchMax | float | "" |
| 0x8 | 4 | hipGunKickYawMin | float | "" |
| 0xC | 4 | hipGunKickYawMax | float | "" |
| 0x10 | 4 | hipGunKickMagMin | float | "" |
| 0x14 | 4 | adsGunKickPitchMin | float | "" |
| 0x18 | 4 | adsGunKickPitchMax | float | "" |
| 0x1C | 4 | adsGunKickYawMin | float | "" |
| 0x20 | 4 | adsGunKickYawMax | float | "" |
| 0x24 | 4 | adsGunKickMagMin | float | "" |
| 0x28 | 1 | useAngularGunKick | bool | "" |
| 0x2C | 24 | hipAngularGunKickDir | float[6] | "" |
| 0x44 | 24 | hipAngularGunKickDev | float[6] | "" |
| 0x5C | 24 | hipAngularGunKickStrengthMin | float[6] | "" |
| 0x74 | 24 | hipAngularGunKickStrengthMax | float[6] | "" |
| 0x8C | 24 | hipAngularGunKickPitchScale | float[6] | "" |
| 0xA4 | 24 | adsAngularGunKickDir | float[6] | "" |
| 0xBC | 24 | adsAngularGunKickDev | float[6] | "" |
| 0xD4 | 24 | adsAngularGunKickStrengthMin | float[6] | "" |
| 0xEC | 24 | adsAngularGunKickStrengthMax | float[6] | "" |
| 0x104 | 24 | adsAngularGunKickPitchScale | float[6] | "" |
| 0x11C | 24 | adsAngularGunKickBullet | int[6] | "" |
| 0x134 | 6 | adsAngularGunKickUseSet | bool[6] | "" |
| 0x13A | 1 | useNewGunKick | bool | "" |
| 0x13C | 4 | hipGunKickAccel | float | "" |
| 0x140 | 4 | adsGunKickAccel | float | "" |
| 0x144 | 4 | hipGunKickSpeedMax | float | "" |
| 0x148 | 4 | adsGunKickSpeedMax | float | "" |
| 0x14C | 4 | hipGunKickSpeedDecay | float | "" |
| 0x150 | 4 | adsGunKickSpeedDecay | float | "" |
| 0x154 | 4 | hipGunKickStaticDecay | float | "" |
| 0x158 | 4 | adsGunKickStaticDecay | float | "" |
| 0x15C | 4 | hipGunKickReturnAccelScale | float | "" |
| 0x160 | 4 | adsGunKickReturnAccelScale | float | "" |
| 0x164 | 4 | hipGunKickReturnSpeedCurveScale | float | "" |
| 0x168 | 4 | adsGunKickReturnSpeedCurveScale | float | "" |
| 0x16C | 4 | gunMaxPitch | float | "" |
| 0x170 | 4 | gunMaxYaw | float | "" |
| 0x174 | 4 | bulletDirGunAngleModifierGunKickHip | float | "" |
| 0x178 | 4 | bulletDirGunAngleModifierGunKickAds | float | "" |

Unmapped/padding bit ranges: 328+24, 2520+8.

### AttGunTilt

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | adsGunTiltPitchFactor | float | "" |
| 0x4 | 4 | adsGunTiltYawFactor | float | "" |
| 0x8 | 4 | adsGunTiltRollFactor | float | "" |
| 0xC | 4 | adsGunTiltOffset | float | "" |
| 0x10 | 4 | hipGunTiltPitchFactor | float | "" |
| 0x14 | 4 | hipGunTiltYawFactor | float | "" |
| 0x18 | 4 | hipGunTiltRollFactor | float | "" |
| 0x1C | 4 | hipGunTiltOffset | float | "" |

### AttHeat

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | heatAccumulationPerShot | float | "" |
| 0x4 | 4 | heatDissipationPerSecond | float | "" |
| 0x8 | 4 | heatSmokeStartThreshold | float | "" |
| 0xC | 4 | heatSmokeStopThreshold | float | "" |

### AttHipSpread

Size `0x5C`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | hipSpreadStandMin | float | "" |
| 0x4 | 4 | hipSpreadDuckedMin | float | "" |
| 0x8 | 4 | hipSpreadProneMin | float | "" |
| 0xC | 4 | hipSpreadSprintMin | float | "" |
| 0x10 | 4 | hipSpreadInAirMin | float | "" |
| 0x14 | 4 | hipSpreadMax | float | "" |
| 0x18 | 4 | hipSpreadDuckedMax | float | "" |
| 0x1C | 4 | hipSpreadProneMax | float | "" |
| 0x20 | 4 | hipSpreadSprintMax | float | "" |
| 0x24 | 4 | hipSpreadInAirMax | float | "" |
| 0x28 | 4 | hipSpreadFireAdd | float | "" |
| 0x2C | 4 | hipSpreadTurnAdd | float | "" |
| 0x30 | 4 | hipSpreadMoveAdd | float | "" |
| 0x34 | 4 | hipSpreadDecayRate | float | "" |
| 0x38 | 4 | hipSpreadDuckedDecay | float | "" |
| 0x3C | 4 | hipSpreadProneDecay | float | "" |
| 0x40 | 4 | hipSpreadSprintDecay | float | "" |
| 0x44 | 4 | hipSpreadInAirDecay | float | "" |
| 0x48 | 4 | hipSpreadStandMoveMax | float | "" |
| 0x4C | 4 | hipSpreadDuckedMoveMax | float | "" |
| 0x50 | 4 | hipSpreadProneMoveMax | float | "" |
| 0x54 | 4 | hipSpreadSprintMoveMax | float | "" |
| 0x58 | 4 | hipSpreadInAirMoveMax | float | "" |

### AttHitmarker

Size `0x4`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | hitmarkerType | HitmarkerType | "" |

### AttHoldBreath

Size `0x1`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | canHoldBreath | bool | "" |

### AttIdleSettings

Size `0xB4`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | hipIdleAmount | float | "" |
| 0x4 | 4 | hipIdleSpeed | float | "" |
| 0x8 | 4 | idleCrouchFactor | float | "" |
| 0xC | 4 | idleProneFactor | float | "" |
| 0x10 | 4 | adsIdleLerpStartTime | float | "" |
| 0x14 | 4 | adsIdleLerpTime | float | "" |
| 0x18 | 1 | useUninterruptedAdsIdleMotion | bool | "" |
| 0x1C | 152 | advancedIdleSettings | AdvancedIdleSettings | "" |

Unmapped/padding bit ranges: 200+24.

### AttImpact

Size `0x4`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | vfxImpactType | int | "" |

### AttKickScaling

Size `0x28`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | hipStartingGunKickPercent | float | "" |
| 0x4 | 4 | hipStartingViewKickPercent | float | "" |
| 0x8 | 4 | adsStartingKickBullets | int | "" |
| 0xC | 4 | adsStartingGunKickPercent | float | "" |
| 0x10 | 4 | adsStartingViewKickPercent | float | "" |
| 0x14 | 4 | hipEndingGunKickPercent | float | "" |
| 0x18 | 4 | hipEndingViewKickPercent | float | "" |
| 0x1C | 4 | adsEndingKickBullets | int | "" |
| 0x20 | 4 | adsEndingGunKickPercent | float | "" |
| 0x24 | 4 | adsEndingViewKickPercent | float | "" |

### AttLaser

Size `0x30`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | laserTypeViewModel | LaserDef * | 1 |
| 0x8 | 8 | laserTypeViewModelAlt | LaserDef * | 1 |
| 0x10 | 8 | laserTypeFriendly | LaserDef * | 1 |
| 0x18 | 8 | laserTypeEnemy | LaserDef * | 1 |
| 0x20 | 10 | laserSettings | LaserSettings | "" |

Unmapped/padding bit ranges: 336+48.

### AttMelee

Size `0xC`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | meleeDamage | int | "" |
| 0x4 | 1 | hasMeleeFinisher | bool | "" |
| 0x8 | 4 | meleeCountToFinisher | int | "" |

Unmapped/padding bit ranges: 40+24.

### AttMovement

Size `0xC`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | moveSpeedScale | float | "" |
| 0x4 | 4 | adsMoveSpeedScale | float | "" |
| 0x8 | 1 | fastADS3PAnims | bool | "" |

Unmapped/padding bit ranges: 72+24.

### AttOutline

Size `0x80`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 16 | outlineColor | vec4_t | "" |
| 0x10 | 16 | outlineAltColor | vec4_t | "" |
| 0x20 | 16 | outlineFillColor0 | vec4_t | "" |
| 0x30 | 16 | outlineFillColor1 | vec4_t | "" |
| 0x40 | 16 | outlineOccludedOutlineColor | vec4_t | "" |
| 0x50 | 16 | outlineOccludedInlineColor | vec4_t | "" |
| 0x60 | 16 | outlineOccludedInteriorColor | vec4_t | "" |
| 0x70 | 4 | outlineWidth | float | "" |
| 0x74 | 1 | outlineEnemies | bool | "" |
| 0x75 | 1 | outlineDepthTest | bool | "" |
| 0x76 | 1 | outlineFill | bool | "" |
| 0x77 | 1 | outlineScriptControlled | bool | "" |
| 0x78 | 1 | outlineVehicles | bool | "" |
| 0x79 | 1 | outlineTurrets | bool | "" |
| 0x7A | 1 | outlineCharacters | bool | "" |
| 0x7B | 1 | outlineAgents | bool | "" |
| 0x7C | 1 | outlineScriptMovers | bool | "" |
| 0x7D | 1 | outlineLockOn | bool | "" |

Unmapped/padding bit ranges: 1008+16.

### AttPenetration

Size `0xC`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | penetrateType | PenetrateType | "" |
| 0x4 | 4 | penetrateMultiplier | float | "" |
| 0x8 | 1 | rifleBullet | bool | "" |
| 0x9 | 1 | armorPiercing | bool | "" |
| 0xA | 1 | impaling | bool | "" |

Unmapped/padding bit ranges: 88+8.

### AttPost

Size `0x34`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | adsFireMotionBlur | float | "" |
| 0x4 | 4 | adsFireMotionBlurDurationMs | int | "" |
| 0x8 | 4 | adsFireMotionBlurDecayExponent | float | "" |
| 0xC | 4 | hipFireMotionBlur | float | "" |
| 0x10 | 4 | hipFireMotionBlurDurationMs | int | "" |
| 0x14 | 4 | hipFireMotionBlurDecayExponent | float | "" |
| 0x18 | 4 | dofViewModelPhysicalFstop | float | "" |
| 0x1C | 4 | dofViewModelPhysicalFocusDistance | float | "" |
| 0x20 | 4 | dofPhysicalFstop | float | "" |
| 0x24 | 4 | dofPhysicalFocusDistance | float | "" |
| 0x28 | 4 | adsCameraShakeRotationScale | float | "" |
| 0x2C | 4 | adsCameraShakeTranslationScale | float | "" |
| 0x30 | 1 | dofViewModelPhysicalFocalTag | bool | "" |
| 0x31 | 1 | dofSceneIsOverridden | bool | "" |

Unmapped/padding bit ranges: 400+16.

### AttProjectile

Size `0xF0`; Replay schema override.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | explosionRadius | int | "" |
| 0x4 | 4 | explosionForceRadius | int | "" |
| 0x8 | 4 | explosionInnerDamage | int | "" |
| 0xC | 4 | explosionOuterDamage | int | "" |
| 0x10 | 4 | damageConeAngle | float | "" |
| 0x14 | 4 | projectileSpeed | int | "" |
| 0x18 | 4 | projectileSpeedUp | int | "" |
| 0x1C | 4 | projectileActivateDist | int | "" |
| 0x20 | 4 | projectileLifetime | float | "" |
| 0x24 | 4 | projectileLifetimeStdDeviation | float | "" |
| 0x28 | 8 | projectileModel | XModel * | 1 |
| 0x30 | 4 | projExplosionType | weapProjExposion_t | "" |
| 0x38 | 8 | projExplosionEffect | FxCombinedDef | "" |
| 0x40 | 1 | projExplosionEffectForceNormalUp | bool | "" |
| 0x48 | 8 | projExplosionSound | SndAliasLookup | "" |
| 0x50 | 4 | projExplosionReflClass | unsigned int | "" |
| 0x58 | 8 | projDudEffect | FxCombinedDef | "" |
| 0x60 | 8 | projTimeoutEffect | FxCombinedDef | "" |
| 0x68 | 8 | projDudSound | SndAliasLookup | "" |
| 0x70 | 8 | projTimeoutSound | SndAliasLookup | "" |
| 0x78 | 1 | disableFxOnTimeout | bool | "" |
| 0x79 | 1 | projImpactExplode | bool | "" |
| 0x7A | 1 | disableProjectileCrumpleCheck | bool | "" |
| 0x7C | 4 | destabilizationRateTime | float | "" |
| 0x80 | 4 | destabilizationCurvatureMax | float | "" |
| 0x84 | 4 | destabilizeDistance | int | "" |
| 0x88 | 8 | vmProjBodyEffect | FxCombinedDef | "" |
| 0x90 | 8 | projBodyEffect | FxCombinedDef | "" |
| 0x98 | 8 | projTrailEffect | FxCombinedDef | "" |
| 0xA0 | 8 | projBeaconEffect | FxCombinedDef | "" |
| 0xA8 | 4 | projIgnitionDelay | int | "" |
| 0xB0 | 8 | projIgnitionEffect | FxCombinedDef | "" |
| 0xB8 | 8 | projIgnitionSound | SndAliasLookup | "" |
| 0xC0 | 4 | stickiness | WeapStickinessType | "" |
| 0xC4 | 4 | fuseTime | int | "" |
| 0xC8 | 4 | aiFuseTime | int | "" |
| 0xCC | 1 | stickToPlayers | bool | "" |
| 0xCD | 1 | stickToVehicles | bool | "" |
| 0xCE | 1 | stickToTurrets | bool | "" |
| 0xCF | 1 | stickToNonStick | bool | "" |
| 0xD0 | 1 | timedDetonation | bool | "" |
| 0xD1 | 1 | isPredictedProjectile | bool | "" |
| 0xD2 | 1 | useSteppedExplosionDamage | bool | "" |
| 0xD4 | 4 | clientTrajectoryBlendOutTime | int | "" |
| 0xD8 | 4 | serverTrajectoryBlendInTime | int | "" |
| 0xDC | 4 | reactiveMotionRadiusScale | float | "" |
| 0xE0 | 4 | reactiveMotionFrequencyScale | float | "" |
| 0xE4 | 4 | reactiveMotionAmplitudeScale | float | "" |
| 0xE8 | 4 | reactiveMotionFalloff | float | "" |
| 0xEC | 4 | reactiveMotionLifetime | float | "" |

Unmapped/padding bit ranges: 416+32, 520+56, 672+32, 984+8, 1376+32, 1688+8.

### AttRegeneration

Size `0x14`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | regenerationEnabled | bool | "" |
| 0x4 | 4 | regenerationTimeMs | int | "" |
| 0x8 | 4 | regenerationAddTimeMs | int | "" |
| 0xC | 4 | regenerationAmount | int | "" |
| 0x10 | 1 | regenerationConsumeStock | bool | "" |
| 0x11 | 1 | regenerationDisableWhileFiring | bool | "" |

Unmapped/padding bit ranges: 8+24, 144+16.

### AttReload

Size `0x3`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | noPartialReload | bool | "" |
| 0x1 | 1 | segmentedReload | bool | "" |
| 0x2 | 1 | reloadDisabled | bool | "" |

### AttRumbles

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | fireRumble | RumbleInfo * | 1 |
| 0x8 | 8 | meleeImpactRumble | RumbleInfo * | 1 |

### AttScopeDriftSettings

Size `0x18`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | fScopeDriftDelay | float | "" |
| 0x4 | 4 | fScopeDriftLerpInTime | float | "" |
| 0x8 | 4 | fScopeDriftSteadyTime | float | "" |
| 0xC | 4 | fScopeDriftLerpOutTime | float | "" |
| 0x10 | 4 | fScopeDriftSteadyFactor | float | "" |
| 0x14 | 4 | fScopeDriftUnsteadyFactor | float | "" |

### AttSight

Size `0x88`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | blurSceneAdsInFraction | float | "" |
| 0x4 | 4 | blurSceneAdsOutFraction | float | "" |
| 0x8 | 1 | adsSmoothFade | bool | "" |
| 0xC | 4 | adsSmoothFadeInBegin | float | "" |
| 0x10 | 4 | adsSmoothFadeInEnd | float | "" |
| 0x14 | 4 | adsSmoothFadeOutBegin | float | "" |
| 0x18 | 4 | adsSmoothFadeOutEnd | float | "" |
| 0x1C | 1 | aimDownSight | bool | "" |
| 0x1D | 1 | adsFire | bool | "" |
| 0x1E | 1 | rechamberWhileAds | bool | "" |
| 0x1F | 1 | noAdsWhenMagEmpty | bool | "" |
| 0x20 | 1 | hideRailWithThisScope | bool | "" |
| 0x21 | 1 | forceIronSightDown | bool | "" |
| 0x22 | 1 | useScopeDrift | bool | "" |
| 0x23 | 1 | useDualFOV | bool | "" |
| 0x24 | 1 | showViewModelInDualFOV | bool | "" |
| 0x25 | 1 | syncedFOVInDualFOV | bool | "" |
| 0x26 | 1 | blockNVGADS | bool | "" |
| 0x28 | 4 | scopeLensInnerDisk | float | "" |
| 0x2C | 4 | scopeLensInnerDiskMag | float | "" |
| 0x30 | 4 | scopeLensOuterRing | float | "" |
| 0x34 | 4 | scopeLensOuterRingMag | float | "" |
| 0x38 | 4 | scopeLensRadius | float | "" |
| 0x3C | 4 | scopeLensColorRed | float | "" |
| 0x40 | 4 | scopeLensColorGreen | float | "" |
| 0x44 | 4 | scopeLensColorBlue | float | "" |
| 0x48 | 4 | scopeLensBrightness | float | "" |
| 0x4C | 4 | scopeLensFadeStart | float | "" |
| 0x50 | 4 | scopeLensFadeEnd | float | "" |
| 0x54 | 4 | scopeEyeRelief_focusDistance | float | "" |
| 0x58 | 4 | scopeEyeRelief_focuseUVScale | float | "" |
| 0x5C | 4 | scopeEyeRelief_outOfFocusDistance | float | "" |
| 0x60 | 4 | scopeEyeRelief_outOfFocuseUVScale | float | "" |
| 0x64 | 4 | scopeEyeRelief_idleMovementScale | float | "" |
| 0x68 | 4 | scopeEyeRelief_fullSpeedMovementScale | float | "" |
| 0x6C | 4 | scopeEyeRelief_maxMovement | float | "" |
| 0x70 | 4 | scopeEyeRelief_hipMovementScale | float | "" |
| 0x74 | 4 | scopeEyeRelief_hipMaxMovement | float | "" |
| 0x78 | 4 | scopeEyeRelief_idleSway_freq | float | "" |
| 0x7C | 4 | scopeEyeRelief_idleSway_movement | float | "" |
| 0x80 | 4 | scopeEyeRelief_fullSpeedSway_freq | float | "" |
| 0x84 | 4 | scopeEyeRelief_fullSpeedSway_movement | float | "" |

Unmapped/padding bit ranges: 72+24, 312+8.

### AttSlideSpread

Size `0x18`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | slideSpreadMin | float | "" |
| 0x4 | 4 | slideSpreadMax | float | "" |
| 0x8 | 4 | slideSpreadDecayRate | float | "" |
| 0xC | 4 | slideSpreadFireAdd | float | "" |
| 0x10 | 4 | slideSpreadTurnAdd | float | "" |
| 0x14 | 4 | slideSpreadMoveMax | float | "" |

### AttTargetAssist

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | targetAssistType | targetAssistType_t | "" |
| 0x4 | 4 | targetAssistBehavior | targetAssistBehavior_t | "" |
| 0x8 | 4 | targetAssistRange | float | "" |
| 0xC | 4 | targetAssistAngle | float | "" |
| 0x10 | 4 | targetAssistLosOffsetForward | float | "" |
| 0x14 | 4 | targetAssistLosOffsetRight | float | "" |
| 0x18 | 4 | targetAssistLosOffsetUp | float | "" |
| 0x1C | 1 | targetAssistOnlyAvailableInAds | bool | "" |

Unmapped/padding bit ranges: 232+24.

### AttUI

Size `0x60`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | hudIcon | GfxImage * | 1 |
| 0x8 | 8 | pickupIcon | GfxImage * | 1 |
| 0x10 | 8 | dangerIconImg | GfxImage * | 1 |
| 0x18 | 8 | throwBackIconImg | GfxImage * | 1 |
| 0x20 | 8 | dangerIconMat | Material * | "" |
| 0x28 | 8 | throwBackIconMat | Material * | "" |
| 0x30 | 1 | hideWarningIcons | bool | "" |
| 0x34 | 4 | warningIconsDelay | float | "" |
| 0x38 | 8 | dpadIconImg | GfxImage * | 1 |
| 0x40 | 8 | dpadIconInactiveImg | GfxImage * | 1 |
| 0x48 | 8 | dpadIconMat | Material * | "" |
| 0x50 | 8 | dpadIconInactiveMat | Material * | "" |
| 0x58 | 4 | ammoCounterClip | ammoCounterClipType_t | "" |
| 0x5C | 4 | grenadeDangerIconDistance | float | "" |

Unmapped/padding bit ranges: 392+24.

### AttViewKick

Size `0x168`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | hipViewKickPitchMin | float | "" |
| 0x4 | 4 | hipViewKickPitchMax | float | "" |
| 0x8 | 4 | hipViewKickYawMin | float | "" |
| 0xC | 4 | hipViewKickYawMax | float | "" |
| 0x10 | 4 | hipViewKickMagMin | float | "" |
| 0x14 | 4 | adsViewKickPitchMin | float | "" |
| 0x18 | 4 | adsViewKickPitchMax | float | "" |
| 0x1C | 4 | adsViewKickYawMin | float | "" |
| 0x20 | 4 | adsViewKickYawMax | float | "" |
| 0x24 | 4 | adsViewKickMagMin | float | "" |
| 0x28 | 1 | useAngularViewKick | bool | "" |
| 0x2C | 24 | hipAngularViewKickDir | float[6] | "" |
| 0x44 | 24 | hipAngularViewKickDev | float[6] | "" |
| 0x5C | 24 | hipAngularViewKickStrengthMin | float[6] | "" |
| 0x74 | 24 | hipAngularViewKickStrengthMax | float[6] | "" |
| 0x8C | 24 | hipAngularViewKickPitchScale | float[6] | "" |
| 0xA4 | 24 | adsAngularViewKickDir | float[6] | "" |
| 0xBC | 24 | adsAngularViewKickDev | float[6] | "" |
| 0xD4 | 24 | adsAngularViewKickStrengthMin | float[6] | "" |
| 0xEC | 24 | adsAngularViewKickStrengthMax | float[6] | "" |
| 0x104 | 24 | adsAngularViewKickPitchScale | float[6] | "" |
| 0x11C | 24 | adsAngularViewKickBullet | int[6] | "" |
| 0x134 | 6 | adsAngularViewKickUseSet | bool[6] | "" |
| 0x13A | 1 | useNewViewKick | bool | "" |
| 0x13C | 4 | hipViewKickCenterSpeed | float | "" |
| 0x140 | 4 | adsViewKickCenterSpeed | float | "" |
| 0x144 | 4 | hipViewKickReturnAccelScale | float | "" |
| 0x148 | 4 | adsViewKickReturnAccelScale | float | "" |
| 0x14C | 4 | hipViewKickReturnSpeedCurveScale | float | "" |
| 0x150 | 4 | adsViewKickReturnSpeedCurveScale | float | "" |
| 0x154 | 4 | kickAlignedInputScalar | float | "" |
| 0x158 | 4 | kickOpposedInputScalar | float | "" |
| 0x15C | 4 | viewKickMaintainFraction | float | "" |
| 0x160 | 4 | viewMaxPitch | float | "" |
| 0x164 | 4 | viewMaxYaw | float | "" |

Unmapped/padding bit ranges: 328+24, 2520+8.

### AttachmentList

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | attachmentCount | unsigned int | "" |
| 0x8 | 8 | attachments | WeaponAttachment ** | varAttachmentList->attachmentCount |

Unmapped/padding bit ranges: 32+32.

### BallisticInfo

Size `0x28`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | muzzleVelocity | float | "" |
| 0x4 | 4 | ballisticCoefficient | float | "" |
| 0x8 | 4 | diameter | float | "" |
| 0xC | 4 | mass | float | "" |
| 0x10 | 4 | gravityFactor | float | "" |
| 0x14 | 4 | zeroingDistance | float | "" |
| 0x18 | 8 | calculated | BallisticInfoCalculated * | 1 |
| 0x20 | 1 | enableBallisticTrajectory | bool | "" |
| 0x24 | 4 | lifeTimeMs | int | "" |

Unmapped/padding bit ranges: 264+24.

### BallisticInfoCalculated

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | distances | float * | varBallisticInfoCalculated->numDistanceEntries |
| 0x8 | 4 | zeroingAngle | float | "" |
| 0xC | 4 | numDistanceEntries | int | "" |

### CarryAnimOverride

Size `0x18`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | carryObjectType | CarryObjectType | "" |
| 0x8 | 8 | animPackage | WeaponAnimPackage * | 1 |
| 0x10 | 8 | animPackageAlt | WeaponAnimPackage * | 1 |

Unmapped/padding bit ranges: 32+32.

### CommonSwaySettings

Size `0x1C`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | maxAngle | float | "" |
| 0x4 | 4 | lerpSpeed | float | "" |
| 0x8 | 4 | pitchScale | float | "" |
| 0xC | 4 | yawScale | float | "" |
| 0x10 | 4 | horizScale | float | "" |
| 0x14 | 4 | vertScale | float | "" |
| 0x18 | 4 | gunAngleScale | float | "" |

### FootstepAnim

Size `0x204`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | leftCount | int | "" |
| 0x4 | 256 | step | FootstepTime[32] | "" |
| 0x104 | 256 | movement | MovementTime[32] | "" |

### FootstepTime

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | time | float | "" |
| 0x4 | 1 | isLeft | bool | "" |

Unmapped/padding bit ranges: 40+24.

### FxCombinedDef

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | particleSystemDef | const ParticleSystemDef * | 1 |

### GrenadeRotationParams

Size `0x34`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | initialPitch | float | "" |
| 0x4 | 4 | initialYaw | float | "" |
| 0x8 | 4 | initialRoll | float | "" |
| 0xC | 4 | rotationPitchDir | GrenadeRotationDirection | "" |
| 0x10 | 4 | rotationPitchMin | int | "" |
| 0x14 | 4 | rotationPitchMax | int | "" |
| 0x18 | 4 | rotationYawDir | GrenadeRotationDirection | "" |
| 0x1C | 4 | rotationYawMin | int | "" |
| 0x20 | 4 | rotationYawMax | int | "" |
| 0x24 | 4 | rotationRollDir | GrenadeRotationDirection | "" |
| 0x28 | 4 | rotationRollMin | int | "" |
| 0x2C | 4 | rotationRollMax | int | "" |
| 0x30 | 1 | rotate | bool | "" |

Unmapped/padding bit ranges: 392+24.

### HipSwaySettings

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 28 | common | CommonSwaySettings | "" |
| 0x1C | 4 | maxAngleSteadyAim | float | "" |

### LaserSettings

Size `0xA`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | forceLaserOn | bool | "" |
| 0x1 | 1 | localPlayerADSLaserEnabled | bool | "" |
| 0x2 | 1 | localPlayerHipLaserEnabled | bool | "" |
| 0x3 | 1 | localPlayerNVGADSLaserEnabled | bool | "" |
| 0x4 | 1 | localPlayerNVGHipLaserEnabled | bool | "" |
| 0x5 | 1 | remotePlayerADSLaserEnabled | bool | "" |
| 0x6 | 1 | remotePlayerHipLaserEnabled | bool | "" |
| 0x7 | 1 | remotePlayerNVGADSLaserEnabled | bool | "" |
| 0x8 | 1 | remotePlayerNVGHipLaserEnabled | bool | "" |
| 0x9 | 1 | laserViewCenterInAds | bool | "" |

### MovementTime

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | time | float | "" |
| 0x4 | 1 | isLeadIn | bool | "" |

Unmapped/padding bit ranges: 40+24.

### NoteTrackToSoundEntry

Size `0x18`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | numSoundMappings | unsigned int | "" |
| 0x4 | 4 | attachment | scr_string_t | "" |
| 0x8 | 8 | notetrackSoundMapKeys | scr_string_t * | varNoteTrackToSoundEntry->numSoundMappings |
| 0x10 | 8 | notetrackSoundMapValues | scr_string_t * | varNoteTrackToSoundEntry->numSoundMappings |

### NoteTrackToSuitSoundEntry

Size `0x18`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | numSoundMappings | unsigned int | "" |
| 0x4 | 4 | suitAnimState | SuitAnimType | "" |
| 0x8 | 8 | notetrackSoundMapKeys | scr_string_t * | varNoteTrackToSuitSoundEntry->numSoundMappings |
| 0x10 | 8 | notetrackSoundMapValues | scr_string_t * | varNoteTrackToSuitSoundEntry->numSoundMappings |

### OverrideBinding

Size `0x18`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | numBindingLists | unsigned int | "" |
| 0x8 | 8 | bindingLists | OverrideBindingList * | (68 * varOverrideBinding->numBindingLists) / 68 |
| 0x10 | 4 | attachmentVarIndex | int | "" |

Unmapped/padding bit ranges: 32+32, 160+32.

### OverrideBindingList

Size `0x44`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | slot | AttachmentSlot | "" |
| 0x4 | 64 | attachments | unsigned int[16] | "" |

### SFXOverride

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | numBindings | unsigned int | "" |
| 0x8 | 8 | bindings | OverrideBinding * | varSFXOverride->numBindings |
| 0x10 | 8 | overrides | WeaponSFXPackage * | 1 |
| 0x18 | 8 | overridesAlt | WeaponSFXPackage * | 1 |

Unmapped/padding bit ranges: 32+32.

### SceneZoomSettings

Size `0x40`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | adsZoomCount | int | "" |
| 0x4 | 12 | adsZoomFov | float[3] | "" |
| 0x10 | 12 | adsReloadFov | float[3] | "" |
| 0x1C | 12 | adsZoomFovTransitionTime | int[3] | "" |
| 0x28 | 4 | adsZoomFovLerpType | AdsOffsetInterpolationType | "" |
| 0x2C | 4 | adsZoomInFrac | float | "" |
| 0x30 | 4 | adsZoomOutFrac | float | "" |
| 0x34 | 4 | adsFOVNVG | float | "" |
| 0x38 | 4 | adsReloadFovNVG | float | "" |
| 0x3C | 1 | adsZoomLevelIndexResetOnToggle | bool | "" |

Unmapped/padding bit ranges: 488+24.

### SndAliasLookup

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | name | const char * | "" |

### SwaySettings

Size `0x100`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 32 | hip | HipSwaySettings | "" |
| 0x20 | 44 | ads | AdsSwaySettings | "" |
| 0x50 | 168 | adv | AdvancedHipSwaySettings | "" |
| 0xF8 | 4 | shellShockScale | float | "" |
| 0xFC | 1 | overrideHip | bool | "" |
| 0xFD | 1 | overrideAds | bool | "" |

Unmapped/padding bit ranges: 608+32, 2032+16.

### TriggerDisconnectSoundData

Size `0xC`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | analogTriggerValue | float | "" |
| 0x4 | 6 | enabledForFireType | bool[6] | "" |

Unmapped/padding bit ranges: 80+16.

### VFXOverride

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | numBindings | unsigned int | "" |
| 0x8 | 8 | bindings | OverrideBinding * | varVFXOverride->numBindings |
| 0x10 | 8 | overrides | WeaponVFXPackage * | 1 |
| 0x18 | 8 | overridesAlt | WeaponVFXPackage * | 1 |

Unmapped/padding bit ranges: 32+32.

### WeaponAnimPackage

Size `0x40`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | name | const char * | "" |
| 0x8 | 8 | anims | XAnimParts ** | 0x266 |
| 0x10 | 8 | timers | WeaponAnimPackageStateTimers * | (0x172) / 370 |
| 0x18 | 4 | meleeAnimPrimaryType | MeleeAnimType | "" |
| 0x1C | 4 | meleeAnimPrimaryCount | int | "" |
| 0x20 | 1 | meleeAnimPrimarySet | unsigned __int8 | "" |
| 0x21 | 1 | meleeRandomizeAnims | bool | "" |
| 0x24 | 4 | meleeAnimAltType | MeleeAnimType | "" |
| 0x28 | 4 | meleeAnimAltCount | int | "" |
| 0x2C | 1 | meleeAnimAltSet | unsigned __int8 | "" |
| 0x2D | 1 | altMeleeRandomizeAnims | bool | "" |
| 0x30 | 8 | blendSpaces | BlendSpace2DDef ** | 8 |
| 0x38 | 8 | footstep | FootstepAnim * | 0xD |

Unmapped/padding bit ranges: 272+16, 368+16.

### WeaponAnimPackageStateTimers

Size `0x172`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 2 | iDetonateDelay | unsigned __int16 | "" |
| 0x2 | 2 | iFirstRaiseTime | unsigned __int16 | "" |
| 0x4 | 2 | iAltRaiseTime | unsigned __int16 | "" |
| 0x6 | 2 | iAltRaiseADSTime | unsigned __int16 | "" |
| 0x8 | 2 | iAltRaiseAkimboTime | unsigned __int16 | "" |
| 0xA | 2 | iRechamberTime | unsigned __int16 | "" |
| 0xC | 2 | rechamberTimeOneHanded | unsigned __int16 | "" |
| 0xE | 2 | iRechamberBoltTime | unsigned __int16 | "" |
| 0x10 | 2 | iHoldFireTime | unsigned __int16 | "" |
| 0x12 | 2 | iHoldPrimeTime | unsigned __int16 | "" |
| 0x14 | 2 | iDetonateTime | unsigned __int16 | "" |
| 0x16 | 2 | iMeleeTime | unsigned __int16 | "" |
| 0x18 | 2 | meleeChargeTime | unsigned __int16 | "" |
| 0x1A | 2 | meleeChargeViewAndMovementLockTime | unsigned __int16 | "" |
| 0x1C | 2 | meleeSliceDamageTime | unsigned __int16 | "" |
| 0x1E | 2 | meleeSliceInterruptTime | unsigned __int16 | "" |
| 0x20 | 2 | meleeSliceViewAndMovementLockTime | unsigned __int16 | "" |
| 0x22 | 2 | meleeReSliceInterruptTime | unsigned __int16 | "" |
| 0x24 | 2 | meleeSliceStandardInterruptTime | unsigned __int16 | "" |
| 0x26 | 2 | meleeChargeDamageTime | unsigned __int16 | "" |
| 0x28 | 2 | meleeChargeInterruptTime | unsigned __int16 | "" |
| 0x2A | 2 | meleeReChargeInterruptTime | unsigned __int16 | "" |
| 0x2C | 2 | meleeChargeStandardInterruptTime | unsigned __int16 | "" |
| 0x2E | 2 | altMeleeTime | unsigned __int16 | "" |
| 0x30 | 2 | altMeleeChargeTime | unsigned __int16 | "" |
| 0x32 | 2 | altMeleeChargeViewAndMovementLockTime | unsigned __int16 | "" |
| 0x34 | 2 | altMeleeSliceDamageTime | unsigned __int16 | "" |
| 0x36 | 2 | altMeleeSliceInterruptTime | unsigned __int16 | "" |
| 0x38 | 2 | altMeleeSliceViewAndMovementLockTime | unsigned __int16 | "" |
| 0x3A | 2 | altMeleeReSliceInterruptTime | unsigned __int16 | "" |
| 0x3C | 2 | altMeleeSliceStandardInterruptTime | unsigned __int16 | "" |
| 0x3E | 2 | altMeleeChargeDamageTime | unsigned __int16 | "" |
| 0x40 | 2 | altMeleeChargeInterruptTime | unsigned __int16 | "" |
| 0x42 | 2 | altMeleeReChargeInterruptTime | unsigned __int16 | "" |
| 0x44 | 2 | altMeleeChargeStandardInterruptTime | unsigned __int16 | "" |
| 0x46 | 2 | iReloadTime | unsigned __int16 | "" |
| 0x48 | 2 | iFastReloadTime | unsigned __int16 | "" |
| 0x4A | 2 | reloadShowRocketTime | unsigned __int16 | "" |
| 0x4C | 2 | iReloadEmptyTime | unsigned __int16 | "" |
| 0x4E | 2 | iFastReloadEmptyTime | unsigned __int16 | "" |
| 0x50 | 2 | iReloadAddTime | unsigned __int16 | "" |
| 0x52 | 2 | iFastReloadAddTime | unsigned __int16 | "" |
| 0x54 | 2 | iReloadAddSecondTime | unsigned __int16 | "" |
| 0x56 | 2 | iFastReloadAddSecondTime | unsigned __int16 | "" |
| 0x58 | 2 | iReloadEmptyAddTime | unsigned __int16 | "" |
| 0x5A | 2 | iFastReloadEmptyAddTime | unsigned __int16 | "" |
| 0x5C | 2 | iAltOverride3PInTime | unsigned __int16 | "" |
| 0x5E | 2 | iAltOverride3POutTime | unsigned __int16 | "" |
| 0x60 | 2 | iReloadStartTime | unsigned __int16 | "" |
| 0x62 | 2 | iFastReloadStartTime | unsigned __int16 | "" |
| 0x64 | 2 | iReloadStartAddTime | unsigned __int16 | "" |
| 0x66 | 2 | iFastReloadStartAddTime | unsigned __int16 | "" |
| 0x68 | 2 | iReloadStartAddSecondTime | unsigned __int16 | "" |
| 0x6A | 2 | iFastReloadStartAddSecondTime | unsigned __int16 | "" |
| 0x6C | 2 | iReloadEndTime | unsigned __int16 | "" |
| 0x6E | 2 | iReloadEndInterruptedTime | unsigned __int16 | "" |
| 0x70 | 2 | iReloadEndRechamberTime | unsigned __int16 | "" |
| 0x72 | 2 | iReloadEndRechamberShellEjectTime | unsigned __int16 | "" |
| 0x74 | 2 | iReloadEndRechamberInterruptedTime | unsigned __int16 | "" |
| 0x76 | 2 | iReloadEndRechamberInterruptedShellEjectTime | unsigned __int16 | "" |
| 0x78 | 2 | iFastReloadEndTime | unsigned __int16 | "" |
| 0x7A | 2 | iFastReloadEndInterruptedTime | unsigned __int16 | "" |
| 0x7C | 2 | iFastReloadEndRechamberTime | unsigned __int16 | "" |
| 0x7E | 2 | iFastReloadEndRechamberShellEjectTime | unsigned __int16 | "" |
| 0x80 | 2 | iFastReloadEndRechamberInterruptedTime | unsigned __int16 | "" |
| 0x82 | 2 | iFastReloadEndRechamberInterruptedShellEjectTime | unsigned __int16 | "" |
| 0x84 | 2 | iDropTime | unsigned __int16 | "" |
| 0x86 | 2 | iRaiseTime | unsigned __int16 | "" |
| 0x88 | 2 | iAltDropTime | unsigned __int16 | "" |
| 0x8A | 2 | iAltDropADSTime | unsigned __int16 | "" |
| 0x8C | 2 | iAltDropAkimboTime | unsigned __int16 | "" |
| 0x8E | 2 | quickDropTime | unsigned __int16 | "" |
| 0x90 | 2 | quickRaiseTime | unsigned __int16 | "" |
| 0x92 | 2 | iBreachRaiseTime | unsigned __int16 | "" |
| 0x94 | 2 | iEmptyRaiseTime | unsigned __int16 | "" |
| 0x96 | 2 | iEmptyDropTime | unsigned __int16 | "" |
| 0x98 | 2 | iLadderVaultTime | unsigned __int16 | "" |
| 0x9A | 2 | iLadderClimbRaiseTime | unsigned __int16 | "" |
| 0x9C | 2 | iLadderClimbDropTime | unsigned __int16 | "" |
| 0x9E | 2 | iLadderAimRaiseTime | unsigned __int16 | "" |
| 0xA0 | 2 | iLadderAimRaiseInterruptTime | unsigned __int16 | "" |
| 0xA2 | 2 | iLadderAimAdsRaiseTime | unsigned __int16 | "" |
| 0xA4 | 2 | iLadderAimAdsRaiseInterruptTime | unsigned __int16 | "" |
| 0xA6 | 2 | iLadderAimDropTime | unsigned __int16 | "" |
| 0xA8 | 2 | iLadderAimDropInterruptTime | unsigned __int16 | "" |
| 0xAA | 2 | sprintInTime | unsigned __int16 | "" |
| 0xAC | 2 | sprintLoopTime | unsigned __int16 | "" |
| 0xAE | 2 | sprintOutTime | unsigned __int16 | "" |
| 0xB0 | 2 | walkTime | unsigned __int16 | "" |
| 0xB2 | 2 | gungHoTime | unsigned __int16 | "" |
| 0xB4 | 2 | crawlTime | unsigned __int16 | "" |
| 0xB6 | 2 | stunnedTimeBegin | unsigned __int16 | "" |
| 0xB8 | 2 | stunnedTimeLoop | unsigned __int16 | "" |
| 0xBA | 2 | stunnedTimeEnd | unsigned __int16 | "" |
| 0xBC | 2 | primeTime | unsigned __int16 | "" |
| 0xBE | 1 | bHoldFullPrime | bool | "" |
| 0xBF | 1 | bAltOverride3PADSOnly | bool | "" |
| 0xC0 | 2 | blastFrontTime | unsigned __int16 | "" |
| 0xC2 | 2 | blastRightTime | unsigned __int16 | "" |
| 0xC4 | 2 | blastBackTime | unsigned __int16 | "" |
| 0xC6 | 2 | blastLeftTime | unsigned __int16 | "" |
| 0xC8 | 2 | slideInTime | unsigned __int16 | "" |
| 0xCA | 2 | slideLoopTime | unsigned __int16 | "" |
| 0xCC | 2 | slideOutTime | unsigned __int16 | "" |
| 0xCE | 2 | dodgeTime | unsigned __int16 | "" |
| 0xD0 | 2 | wallRunInTime | unsigned __int16 | "" |
| 0xD2 | 2 | wallRunOutTime | unsigned __int16 | "" |
| 0xD4 | 2 | leapInTime | unsigned __int16 | "" |
| 0xD6 | 2 | leapLoopTime | unsigned __int16 | "" |
| 0xD8 | 2 | leapOutTime | unsigned __int16 | "" |
| 0xDA | 2 | leapCancelTime | unsigned __int16 | "" |
| 0xDC | 2 | chargeInTime | unsigned __int16 | "" |
| 0xDE | 2 | chargeLoopTime | unsigned __int16 | "" |
| 0xE0 | 2 | chargeOutTime | unsigned __int16 | "" |
| 0xE2 | 2 | iRegenerationTime | unsigned __int16 | "" |
| 0xE4 | 2 | iRegenerationAddTime | unsigned __int16 | "" |
| 0xE6 | 2 | preFireTime | unsigned __int16 | "" |
| 0xE8 | 2 | iCycleWalk | unsigned __int16 | "" |
| 0xEA | 2 | iCycleJog | unsigned __int16 | "" |
| 0xEC | 2 | iCycleSprint | unsigned __int16 | "" |
| 0xEE | 2 | iCycleSuperSprint | unsigned __int16 | "" |
| 0xF0 | 2 | iTransitionToWalk | unsigned __int16 | "" |
| 0xF2 | 2 | iTransitionToJog | unsigned __int16 | "" |
| 0xF4 | 2 | iTransitionToSprint | unsigned __int16 | "" |
| 0xF6 | 2 | iTransitionToSuperSprint | unsigned __int16 | "" |
| 0xF8 | 2 | iTransitionDownFromSprint | unsigned __int16 | "" |
| 0xFA | 2 | iTransitionDownFromSuperSprint | unsigned __int16 | "" |
| 0xFC | 2 | skydiveFreefallRaiseTime | unsigned __int16 | "" |
| 0xFE | 2 | scopeToggleOnTime | unsigned __int16 | "" |
| 0x100 | 2 | scopeToggleOffTime | unsigned __int16 | "" |
| 0x102 | 2 | iReloadInterruptTime | unsigned __int16 | "" |
| 0x104 | 2 | iReloadEmptyInterruptTime | unsigned __int16 | "" |
| 0x106 | 2 | iReloadStartInterruptTime | unsigned __int16 | "" |
| 0x108 | 2 | iReloadEndInterruptTime | unsigned __int16 | "" |
| 0x10A | 2 | iReloadEndInterruptedInterruptTime | unsigned __int16 | "" |
| 0x10C | 2 | iReloadEndRechamberInterruptTime | unsigned __int16 | "" |
| 0x10E | 2 | iReloadEndRechamberInterruptedInterruptTime | unsigned __int16 | "" |
| 0x110 | 2 | iReloadMultiple1InterruptTime | unsigned __int16 | "" |
| 0x112 | 2 | iReloadMultiple2InterruptTime | unsigned __int16 | "" |
| 0x114 | 2 | iReloadMultiple3InterruptTime | unsigned __int16 | "" |
| 0x116 | 2 | iReloadMultiple4InterruptTime | unsigned __int16 | "" |
| 0x118 | 2 | iReloadMultiple5InterruptTime | unsigned __int16 | "" |
| 0x11A | 2 | iReloadMultiple6InterruptTime | unsigned __int16 | "" |
| 0x11C | 2 | iReloadMultiple7InterruptTime | unsigned __int16 | "" |
| 0x11E | 2 | iReloadMultiple8InterruptTime | unsigned __int16 | "" |
| 0x120 | 2 | iFastReloadInterruptTime | unsigned __int16 | "" |
| 0x122 | 2 | iFastReloadEmptyInterruptTime | unsigned __int16 | "" |
| 0x124 | 2 | iFastReloadStartInterruptTime | unsigned __int16 | "" |
| 0x126 | 2 | iFastReloadEndInterruptTime | unsigned __int16 | "" |
| 0x128 | 2 | iFastReloadEndInterruptedInterruptTime | unsigned __int16 | "" |
| 0x12A | 2 | iFastReloadEndRechamberInterruptTime | unsigned __int16 | "" |
| 0x12C | 2 | iFastReloadEndRechamberInterruptedInterruptTime | unsigned __int16 | "" |
| 0x12E | 2 | iFastReloadMultiple1InterruptTime | unsigned __int16 | "" |
| 0x130 | 2 | iFastReloadMultiple2InterruptTime | unsigned __int16 | "" |
| 0x132 | 2 | iFastReloadMultiple3InterruptTime | unsigned __int16 | "" |
| 0x134 | 2 | iFastReloadMultiple4InterruptTime | unsigned __int16 | "" |
| 0x136 | 2 | iFastReloadMultiple5InterruptTime | unsigned __int16 | "" |
| 0x138 | 2 | iFastReloadMultiple6InterruptTime | unsigned __int16 | "" |
| 0x13A | 2 | iFastReloadMultiple7InterruptTime | unsigned __int16 | "" |
| 0x13C | 2 | iFastReloadMultiple8InterruptTime | unsigned __int16 | "" |
| 0x13E | 2 | iRaiseStartDisableInterruptTime | unsigned __int16 | "" |
| 0x140 | 2 | iRaiseInterruptTime | unsigned __int16 | "" |
| 0x142 | 2 | iFirstRaiseStartDisableInterruptTime | unsigned __int16 | "" |
| 0x144 | 2 | iFirstRaiseInterruptTime | unsigned __int16 | "" |
| 0x146 | 2 | iQuickRaiseStartDisableInterruptTime | unsigned __int16 | "" |
| 0x148 | 2 | iQuickRaiseInterruptTime | unsigned __int16 | "" |
| 0x14A | 2 | iEmptyRaiseStartDisableInterruptTime | unsigned __int16 | "" |
| 0x14C | 2 | iEmptyRaiseInterruptTime | unsigned __int16 | "" |
| 0x14E | 2 | iAltRaiseInterruptTime | unsigned __int16 | "" |
| 0x150 | 2 | iAltRaiseADSInterruptTime | unsigned __int16 | "" |
| 0x152 | 2 | iAltRaiseAkimboInterruptTime | unsigned __int16 | "" |
| 0x154 | 2 | iSprintOutInterruptTime | unsigned __int16 | "" |
| 0x156 | 2 | holdFireInterruptTime | unsigned __int16 | "" |
| 0x158 | 2 | maxBlendToFire | unsigned __int16 | "" |
| 0x15A | 2 | minBlendToFire | unsigned __int16 | "" |
| 0x15C | 2 | maxBlendAltRaiseToFire | unsigned __int16 | "" |
| 0x15E | 2 | minBlendAltRaiseToFire | unsigned __int16 | "" |
| 0x160 | 2 | iTransitionDownFromSprintInterrupt | unsigned __int16 | "" |
| 0x162 | 2 | iTransitionDownFromSuperSprintInterrupt | unsigned __int16 | "" |
| 0x164 | 2 | scopeToggleOnInterruptTime | unsigned __int16 | "" |
| 0x166 | 2 | scopeToggleOffInterruptTime | unsigned __int16 | "" |
| 0x168 | 2 | inspectTime | unsigned __int16 | "" |
| 0x16A | 2 | iDlcAnim0Time | unsigned __int16 | "" |
| 0x16C | 2 | iDlcAnim1Time | unsigned __int16 | "" |
| 0x16E | 2 | iDlcAnim2Time | unsigned __int16 | "" |
| 0x170 | 2 | iDlcAnim3Time | unsigned __int16 | "" |

### WeaponAttachment

Size `0x3C8`; Replay schema override.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | szInternalName | const char * | "" |
| 0x8 | 8 | szDisplayName | const char * | "" |
| 0x10 | 4 | internalName | scr_string_t | "" |
| 0x14 | 4 | overrideCategoryName | scr_string_t | "" |
| 0x18 | 1 | overrideDisplayName | bool | "" |
| 0x19 | 1 | disableWeaponInspect | bool | "" |
| 0x1C | 4 | type | AttachmentType | "" |
| 0x20 | 4 | altModePlayerAnimType | altModePlayerAnimType_t | "" |
| 0x24 | 4 | weaponType | weapType_t | "" |
| 0x28 | 4 | weapClass | weapClass_t | "" |
| 0x2C | 4 | numReticles | unsigned int | "" |
| 0x30 | 4 | numAltReticles | unsigned int | "" |
| 0x38 | 8 | fallbackWorldModel | XModel * | 1 |
| 0x40 | 8 | fallbackViewModel | XModel * | 1 |
| 0x48 | 4 | numModelVariations | unsigned int | "" |
| 0x50 | 8 | worldModelVariations | XModel ** | varWeaponAttachment->numModelVariations |
| 0x58 | 8 | viewModelVariations | XModel ** | varWeaponAttachment->numModelVariations |
| 0x60 | 4 | attachPoint | scr_string_t | "" |
| 0x64 | 4 | numHideTags | unsigned int | "" |
| 0x68 | 8 | hideTags | scr_string_t * | varWeaponAttachment->numHideTags |
| 0x70 | 4 | numHideTagsNonViewModel | unsigned int | "" |
| 0x78 | 8 | hideTagsNonViewModel | scr_string_t * | varWeaponAttachment->numHideTagsNonViewModel |
| 0x80 | 4 | numShowTags | unsigned int | "" |
| 0x88 | 8 | showTags | scr_string_t * | varWeaponAttachment->numShowTags |
| 0x90 | 8 | reticles | ReticleDef ** | varWeaponAttachment->numReticles |
| 0x98 | 8 | altReticles | ReticleDef ** | varWeaponAttachment->numAltReticles |
| 0xA0 | 8 | ammogeneral | AttAmmoGeneral * | (8) / 8 |
| 0xA8 | 8 | ammotype | AttAmmoType * | (8) / 8 |
| 0xB0 | 8 | impact | AttImpact * | (4) / 4 |
| 0xB8 | 4 | dynamicAttachmentType | DynamicAttachmentType | "" |
| 0xC0 | 8 | dynamicAttachmentAnimateSeq | const char * | "" |
| 0xC8 | 8 | dynamicAttachmentIcon | const char * | "" |
| 0xD0 | 4 | disableInputDrivenViewReturnDampening | WeaponFlagOverride | "" |
| 0xD8 | 8 | tracer | AttAmmoTracer * | (0x10) / 16 |
| 0xE0 | 8 | penetration | AttPenetration * | (0xC) / 12 |
| 0xE8 | 8 | sight | AttSight * | (0x88) / 136 |
| 0xF0 | 8 | holdBreath | AttHoldBreath * | (1) / 1 |
| 0xF8 | 8 | reload | AttReload * | (3) / 3 |
| 0x100 | 8 | addOns | AttAddOns * | (2) / 2 |
| 0x108 | 8 | laser | AttLaser * | 1 |
| 0x110 | 8 | post | AttPost * | (0x34) / 52 |
| 0x118 | 8 | general | AttGeneral * | 1 |
| 0x120 | 8 | aimAssist | AttAimAssist * | (0x20) / 32 |
| 0x128 | 8 | targetAssist | AttTargetAssist * | (0x20) / 32 |
| 0x130 | 8 | ammunition | AttAmmunition * | (0x20) / 32 |
| 0x138 | 8 | damage | AttDamage * | (0x88) / 136 |
| 0x140 | 8 | melee | AttMelee * | (0xC) / 12 |
| 0x148 | 8 | idleSettings | AttIdleSettings * | (0xB4) / 180 |
| 0x150 | 8 | adsSettings | AttADSSettings * | (0xD8) / 216 |
| 0x158 | 8 | adsSettingsMain | AttADSSettings * | (0xD8) / 216 |
| 0x160 | 8 | adsGunBobSettings | AttADSGunBobSettings * | (0x1C) / 28 |
| 0x168 | 8 | adsGunBobSettingsMain | AttADSGunBobSettings * | (0x1C) / 28 |
| 0x170 | 8 | adsReloadSettings | AttADSReloadSettings * | (1) / 1 |
| 0x178 | 8 | scopeDriftSettings | AttScopeDriftSettings * | (0x18) / 24 |
| 0x180 | 8 | scopeDriftSettingsMain | AttScopeDriftSettings * | (0x18) / 24 |
| 0x188 | 8 | adsStanceScales | AttADSStanceScales * | 1 |
| 0x190 | 8 | hipSpread | AttHipSpread * | (0x5C) / 92 |
| 0x198 | 8 | slideSpread | AttSlideSpread * | (0x18) / 24 |
| 0x1A0 | 8 | gunKick | AttGunKick * | (0x17C) / 380 |
| 0x1A8 | 8 | gunTilt | AttGunTilt * | (0x20) / 32 |
| 0x1B0 | 8 | kickScaling | AttKickScaling * | (0x28) / 40 |
| 0x1B8 | 8 | viewKick | AttViewKick * | (0x168) / 360 |
| 0x1C0 | 8 | adsOverlay | AttADSOverlay * | 1 |
| 0x1C8 | 8 | outline | AttOutline * | (0x80) / 128 |
| 0x1D0 | 8 | ui | AttUI * | 1 |
| 0x1D8 | 8 | rumbles | AttRumbles * | 1 |
| 0x1E0 | 8 | projectile | AttProjectile * | 1 |
| 0x1E8 | 8 | charged | AttCharged * | 1 |
| 0x1F0 | 8 | adsAltSwitch | AdsAltSwitch * | (0x20) / 32 |
| 0x1F8 | 8 | regeneration | AttRegeneration * | (0x14) / 20 |
| 0x200 | 8 | heat | AttHeat * | (0x10) / 16 |
| 0x208 | 8 | movement | AttMovement * | (0xC) / 12 |
| 0x210 | 8 | movementScale | AttMovement * | (0xC) / 12 |
| 0x218 | 8 | movementScaleMain | AttMovement * | (0xC) / 12 |
| 0x220 | 8 | burst | AttBurst * | (8) / 8 |
| 0x228 | 8 | hitmarker | AttHitmarker * | (4) / 4 |
| 0x230 | 8 | adsMovement | AttAdsMovement * | (0xC) / 12 |
| 0x238 | 8 | ballisticInfo | BallisticInfo * | 1 |
| 0x240 | 8 | swaySettings | SwaySettings * | 1 |
| 0x248 | 8 | weaponOffsetPatternScaleInfo | WeaponOffsetPatternScaleInfo * | 1 |
| 0x250 | 8 | locationDamage | float * | 0x16 |
| 0x258 | 4 | ammunitionScale | float | "" |
| 0x25C | 4 | ballisticMuzzleVelocityScale | float | "" |
| 0x260 | 4 | damageScale | float | "" |
| 0x264 | 1 | overrideStandOfs | bool | "" |
| 0x268 | 12 | standOfs | vec3_t | "" |
| 0x274 | 12 | standOfsRot | vec3_t | "" |
| 0x280 | 12 | standOfsRotPivot | vec3_t | "" |
| 0x28C | 12 | standOfsAdd | vec3_t | "" |
| 0x298 | 12 | standOfsRotAdd | vec3_t | "" |
| 0x2A4 | 1 | overridehipOffsetLerpType | bool | "" |
| 0x2A8 | 4 | hipOffsetLerpType | AdsOffsetInterpolationType | "" |
| 0x2AC | 4 | stateTimersScale | float | "" |
| 0x2B0 | 4 | reloadTimersScale | float | "" |
| 0x2B4 | 4 | fireTimeOverride | int | "" |
| 0x2B8 | 4 | fireTimeAkimboOverride | int | "" |
| 0x2BC | 4 | fireTimersScale | float | "" |
| 0x2C0 | 4 | burstFireCooldownScale | float | "" |
| 0x2C4 | 4 | idleHipMotionScale | float | "" |
| 0x2C8 | 4 | idleADSMotionScale | float | "" |
| 0x2CC | 4 | idleADSMotionTimeScale | float | "" |
| 0x2D0 | 4 | adsSettingsScale | float | "" |
| 0x2D4 | 4 | adsSettingsScaleMain | float | "" |
| 0x2D8 | 4 | adsSpreadScale | float | "" |
| 0x2DC | 4 | adsSpreadScaleMain | float | "" |
| 0x2E0 | 4 | hipSpreadScale | float | "" |
| 0x2E4 | 4 | slideSpreadScale | float | "" |
| 0x2E8 | 4 | gunKickPitchScale | float | "" |
| 0x2EC | 4 | gunKickYawScale | float | "" |
| 0x2F0 | 4 | gunCenterScale | float | "" |
| 0x2F4 | 4 | gunTiltScale | float | "" |
| 0x2F8 | 4 | viewKickPitchScale | float | "" |
| 0x2FC | 4 | viewKickYawScale | float | "" |
| 0x300 | 4 | viewCenterScale | float | "" |
| 0x304 | 4 | viewKickMaintainFractionScale | float | "" |
| 0x308 | 4 | hipSwayScale | float | "" |
| 0x30C | 4 | adsSwayScale | float | "" |
| 0x310 | 4 | adsSwayScale2 | float | "" |
| 0x314 | 4 | adsSwayScale3 | float | "" |
| 0x318 | 4 | adsSwayMaxAngleScale | float | "" |
| 0x31C | 4 | adsSwayLerpSpeedScale | float | "" |
| 0x320 | 4 | adsSwayScaleScale | float | "" |
| 0x324 | 4 | sprintOutTimerScale | float | "" |
| 0x328 | 4 | iAmmoIndex | int | "" |
| 0x32C | 4 | heatAccumulationScale | float | "" |
| 0x330 | 4 | heatDissipationScale | float | "" |
| 0x334 | 4 | heatSmokeThresholdScale | float | "" |
| 0x338 | 4 | adsFireMotionBlurScale | float | "" |
| 0x33C | 4 | adsFireMotionBlurDurationScale | float | "" |
| 0x340 | 4 | hipFireMotionBlurScale | float | "" |
| 0x344 | 4 | hipFireMotionBlurDurationScale | float | "" |
| 0x348 | 4 | adsCameraShakeRotationScaleScale | float | "" |
| 0x34C | 4 | adsCameraShakeTranslationScaleScale | float | "" |
| 0x350 | 4 | adsGunBobScaleScale | float | "" |
| 0x354 | 4 | adsGunBobScaleScaleMain | float | "" |
| 0x358 | 4 | adsGunBobTiltScaleScale | float | "" |
| 0x35C | 4 | adsGunBobTiltScaleScaleMain | float | "" |
| 0x360 | 4 | adsGunBobTiltBulletDirScaleScale | float | "" |
| 0x364 | 4 | adsGunBobTiltBulletDirScaleScaleMain | float | "" |
| 0x368 | 4 | adsViewBobScaleScale | float | "" |
| 0x36C | 4 | adsBobRateScale | float | "" |
| 0x370 | 4 | meleeDamageScale | float | "" |
| 0x374 | 4 | meleeCountToFinishAdjust | int | "" |
| 0x378 | 4 | loadIndex | int | "" |
| 0x37C | 4 | ammoUsedPerShotOverride | int | "" |
| 0x380 | 2 | transientViewFlags | unsigned __int16 | "" |
| 0x382 | 2 | transientWorldFlags | unsigned __int16 | "" |
| 0x384 | 1 | useAlternateViewModelLaser | bool | "" |
| 0x385 | 1 | ignoreIronSights | bool | "" |
| 0x386 | 1 | hideIronSightsWithThisAttachment | bool | "" |
| 0x387 | 1 | shareAmmoWithAlt | bool | "" |
| 0x388 | 1 | useLeftTriggerAltFire | bool | "" |
| 0x389 | 1 | forceForegripIK | bool | "" |
| 0x38A | 1 | containsIKTag | bool | "" |
| 0x38B | 1 | isAkimboAttachment | bool | "" |
| 0x38C | 1 | attachLeftWeaponAkimbo | bool | "" |
| 0x38D | 1 | attachRightWeaponAkimbo | bool | "" |
| 0x38E | 1 | luiWeaponInfoWidgetUsesScopeStencil | bool | "" |
| 0x390 | 8 | szAltModeName | const char * | "" |
| 0x398 | 8 | szAltModeInactiveName | const char * | "" |
| 0x3A0 | 8 | szLUIWeaponInfoWidgetName | const char * | "" |
| 0x3A8 | 8 | szLUIWeaponInfoWidgetTag | const char * | "" |
| 0x3B0 | 8 | altModeIcon | GfxImage * | 1 |
| 0x3B8 | 8 | dlcOverrides | AttDlcOverrides * | (0x28) / 40 |
| 0x3C0 | 8 | dlcScalars | AttDlcScalars * | (0x20) / 32 |

Unmapped/padding bit ranges: 208+16, 416+32, 608+32, 928+32, 1056+32, 1504+32, 1696+32, 4904+24, 5416+24, 7288+8.

### WeaponCompleteDef

Size `0x270`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | szInternalName | const char * | "" |
| 0x8 | 8 | weapDef | WeaponDef * | 1 |
| 0x10 | 8 | szDisplayName | const char * | "" |
| 0x18 | 8 | szLootTable | const char * | "" |
| 0x20 | 8 | hideTags | scr_string_t * | 0x20 |
| 0x28 | 224 | attachments | AttachmentList[14] | "" |
| 0x108 | 4 | numLootVariants | unsigned int | "" |
| 0x10C | 4 | numNotetrackSoundMappings | unsigned int | "" |
| 0x110 | 8 | notetrackSoundMapKeys | scr_string_t * | varWeaponCompleteDef->numNotetrackSoundMappings |
| 0x118 | 8 | notetrackSoundMapValues | scr_string_t * | varWeaponCompleteDef->numNotetrackSoundMappings |
| 0x120 | 4 | numNotetrackOverrides | unsigned int | "" |
| 0x128 | 8 | notetrackOverrides | NoteTrackToSoundEntry * | (24 * varWeaponCompleteDef->numNotetrackOverrides) / 24 |
| 0x130 | 4 | numNotetrackSuitEntries | unsigned int | "" |
| 0x138 | 8 | notetrackSuitEntries | NoteTrackToSuitSoundEntry * | (24 * varWeaponCompleteDef->numNotetrackSuitEntries) / 24 |
| 0x140 | 112 | zoomSettings | ZoomSettings | "" |
| 0x1B0 | 4 | iAdsTransInTime | int | "" |
| 0x1B4 | 4 | iAdsTransOutTime | int | "" |
| 0x1B8 | 4 | iAdsTransInToOutTime | int | "" |
| 0x1BC | 4 | iAdsTransOutToInTime | int | "" |
| 0x1C0 | 4 | iClipSize | int | "" |
| 0x1C4 | 4 | vfxImpactType | int | "" |
| 0x1C8 | 4 | penetrateMultiplier | float | "" |
| 0x1CC | 4 | fAdsViewKickCenterSpeed | float | "" |
| 0x1D0 | 4 | fHipViewKickCenterSpeed | float | "" |
| 0x1D8 | 8 | killIconMat | Material * | "" |
| 0x1E0 | 8 | dpadIconMat | Material * | "" |
| 0x1E8 | 8 | dpadIconInactiveMat | Material * | "" |
| 0x1F0 | 8 | killIconImg | GfxImage * | 1 |
| 0x1F8 | 8 | dpadIconImg | GfxImage * | 1 |
| 0x200 | 8 | dpadIconInactiveImg | GfxImage * | 1 |
| 0x208 | 4 | ammoDropStockMax | int | "" |
| 0x20C | 1 | useSceneDof | bool | "" |
| 0x210 | 4 | adsDofPhysicalFstop | float | "" |
| 0x214 | 4 | adsDofPhysicalFocusDistance | float | "" |
| 0x218 | 1 | adsDofPhysicalFocalTag | bool | "" |
| 0x21C | 4 | adsDofPhysicalReloadFstop | float | "" |
| 0x220 | 4 | adsDofPhysicalReloadFocusDistance | float | "" |
| 0x224 | 4 | accuracyGraphKnotCount | unsigned __int16[2] | "" |
| 0x228 | 16 | accuracyGraphKnots | vec2_t *[2] | "" |
| 0x238 | 1 | enhanced | bool | "" |
| 0x239 | 1 | dpadIconShowsAmmo | bool | "" |
| 0x23A | 1 | luiWeaponInfoWidgetUsesScopeStencil | bool | "" |
| 0x240 | 8 | szAltModeInactiveName | const char * | "" |
| 0x248 | 8 | dynamicIconAnimationName | const char * | "" |
| 0x250 | 1 | isUsingDynamicIcon | bool | "" |
| 0x258 | 8 | szLUIWeaponInfoWidgetName | const char * | "" |
| 0x260 | 8 | szLUIWeaponInfoWidgetTag | const char * | "" |
| 0x268 | 8 | szLUICrosshairWidget | const char * | "" |

Unmapped/padding bit ranges: 2336+32, 2464+32, 3744+32, 4200+24, 4296+24, 4568+40, 4744+56.

### WeaponDamageCommon

Size `0x28`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | minDamage | int | "" |
| 0x4 | 4 | mid1Damage | int | "" |
| 0x8 | 4 | mid2Damage | int | "" |
| 0xC | 4 | mid3Damage | int | "" |
| 0x10 | 4 | damage | int | "" |
| 0x14 | 4 | maxDamageRange | float | "" |
| 0x18 | 4 | mid1DamageRange | float | "" |
| 0x1C | 4 | mid2DamageRange | float | "" |
| 0x20 | 4 | mid3DamageRange | float | "" |
| 0x24 | 4 | minDamageRange | float | "" |

### WeaponDamageInfo

Size `0x80`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 120 | damageData | WeaponDamageCommon[3] | "" |
| 0x78 | 4 | deathAnimDamageType | AnimDamageType | "" |
| 0x7C | 4 | terminateAtRange | float | "" |

### WeaponDef

Size `0x14B0`; Replay schema override.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | szOverlayName | const char * | "" |
| 0x8 | 8 | gunXModel | XModel * | 1 |
| 0x10 | 8 | gunXModelLeftHand | XModel * | 1 |
| 0x18 | 8 | gunXModelRightHand | XModel * | 1 |
| 0x20 | 8 | defaultViewModel | XModel * | 1 |
| 0x28 | 8 | defaultWorldModelLeftHand | XModel * | 1 |
| 0x30 | 8 | defaultWorldModelRightHand | XModel * | 1 |
| 0x38 | 8 | worldModel | XModel * | 1 |
| 0x40 | 8 | worldXModelLeftHand | XModel * | 1 |
| 0x48 | 8 | worldXModelRightHand | XModel * | 1 |
| 0x50 | 8 | defaultWorldModel | XModel * | 1 |
| 0x58 | 8 | censorshipWorldModel | XModel * | 1 |
| 0x60 | 8 | censorshipWorldModelLeftHand | XModel * | 1 |
| 0x68 | 8 | censorshipWorldModelRightHand | XModel * | 1 |
| 0x70 | 8 | playerShadowModel | XModel * | 1 |
| 0x78 | 8 | playerShadowModelLeftHand | XModel * | 1 |
| 0x80 | 8 | playerShadowModelRightHand | XModel * | 1 |
| 0x88 | 8 | handXModel | XModel * | 1 |
| 0x90 | 8 | szXAnims | WeaponAnimPackage * | 1 |
| 0x98 | 8 | szXAnimsRightHanded | WeaponAnimPackage * | 1 |
| 0xA0 | 8 | szXAnimsLeftHanded | WeaponAnimPackage * | 1 |
| 0xA8 | 8 | turretRaiseAnim | XAnimParts * | 1 |
| 0xB0 | 8 | turretIdleAnim | XAnimParts * | 1 |
| 0xB8 | 8 | turretFireAnim | XAnimParts * | 1 |
| 0xC0 | 128 | damageInfo | WeaponDamageInfo | "" |
| 0x140 | 4 | iFireTime | int | "" |
| 0x144 | 4 | iFireDelay | int | "" |
| 0x148 | 4 | iFireTimeAkimbo | int | "" |
| 0x14C | 4 | iFireDelayAkimbo | int | "" |
| 0x150 | 4 | sprintOutTimerScale | float | "" |
| 0x158 | 8 | szModeName | const char * | "" |
| 0x160 | 8 | notetrackRumbleMapKeys | scr_string_t * | 0x10 |
| 0x168 | 8 | notetrackRumbleMapValues | scr_string_t * | 0x10 |
| 0x170 | 8 | notetrackFXMapKeys | scr_string_t * | 0x10 |
| 0x178 | 8 | notetrackFXMapValues | FxCombinedDef * | 0x10 |
| 0x180 | 8 | notetrackFXMapTagValues | scr_string_t * | 0x10 |
| 0x188 | 4 | playerAnimType | int | "" |
| 0x18C | 4 | scriptedAnimEvent | scriptedAnimEvent_t | "" |
| 0x190 | 4 | scriptedAnimType | int | "" |
| 0x194 | 4 | weapType | weapType_t | "" |
| 0x198 | 4 | weapClass | weapClass_t | "" |
| 0x19C | 4 | penetrateType | PenetrateType | "" |
| 0x1A0 | 4 | inventoryType | weapInventoryType_t | "" |
| 0x1A4 | 4 | fireType | weapFireType_t | "" |
| 0x1A8 | 4 | turretFireType | turretFireType_t | "" |
| 0x1AC | 4 | burstCount | int | "" |
| 0x1B0 | 4 | targetAssistType | targetAssistType_t | "" |
| 0x1B4 | 4 | targetAssistBehavior | targetAssistBehavior_t | "" |
| 0x1B8 | 4 | targetAssistRange | float | "" |
| 0x1BC | 4 | targetAssistAngle | float | "" |
| 0x1C0 | 4 | targetAssistLosOffsetForward | float | "" |
| 0x1C4 | 4 | targetAssistLosOffsetRight | float | "" |
| 0x1C8 | 4 | targetAssistLosOffsetUp | float | "" |
| 0x1CC | 1 | targetAssistOnlyAvailableInAds | bool | "" |
| 0x1D0 | 4 | reloadType | ReloadType | "" |
| 0x1D4 | 4 | burstFireCooldown | float | "" |
| 0x1D8 | 1 | burstFireAuto | bool | "" |
| 0x1DC | 4 | viewPitchMaxSpeed | float | "" |
| 0x1E0 | 4 | viewYawMaxSpeed | float | "" |
| 0x1E4 | 1 | alignBarrelWithTurretBody | bool | "" |
| 0x1E5 | 1 | keepOrientationOnExit | bool | "" |
| 0x1E8 | 4 | kickBackForceMultiplier | float | "" |
| 0x1EC | 1 | autoAdjust | bool | "" |
| 0x1ED | 1 | networkLODRangeOverride | bool | "" |
| 0x1F0 | 4 | networkLODRangeOverrideDistance | float | "" |
| 0x1F4 | 4 | offhandClass | OffhandClass | "" |
| 0x1F8 | 4 | stance | weapStance_t | "" |
| 0x200 | 8 | vfxPackage | WeaponVFXPackage * | 1 |
| 0x208 | 8 | sfxPackage | WeaponSFXPackage * | 1 |
| 0x210 | 1 | disableBreathOffhand | bool | "" |
| 0x214 | 4 | reticleType | ReticleType | "" |
| 0x218 | 4 | hitmarkerType | HitmarkerType | "" |
| 0x220 | 8 | reticleCenter | Material * | "" |
| 0x228 | 8 | reticleSide | Material * | "" |
| 0x230 | 8 | reticleOnePiece | Material * | "" |
| 0x238 | 4 | iReticleCenterSize | int | "" |
| 0x23C | 4 | iReticleSideSize | int | "" |
| 0x240 | 4 | iReticleMinOfs | int | "" |
| 0x244 | 1 | reticleCenterPulse | bool | "" |
| 0x248 | 4 | iReticleShakeDuration | int | "" |
| 0x24C | 4 | iReticleSettleDuration | int | "" |
| 0x250 | 4 | iReticleNumBulletsToRapidFire | int | "" |
| 0x254 | 4 | activeReticleType | activeReticleType_t | "" |
| 0x258 | 1 | useTurretViewmodelAnims | bool | "" |
| 0x259 | 1 | useTurret3pIK | bool | "" |
| 0x25C | 4 | turret3pAnimType | turret3pAnimType_t | "" |
| 0x260 | 4 | turretFireAnimMinTime | float | "" |
| 0x264 | 12 | vStandOfs | vec3_t | "" |
| 0x270 | 12 | vStandOfsRot | vec3_t | "" |
| 0x27C | 12 | vStandOfsRotPivot | vec3_t | "" |
| 0x288 | 12 | vStandMove | vec3_t | "" |
| 0x294 | 12 | vStandRot | vec3_t | "" |
| 0x2A0 | 12 | strafeMove | vec3_t | "" |
| 0x2AC | 12 | strafeRot | vec3_t | "" |
| 0x2B8 | 4 | hipOffsetLerpType | AdsOffsetInterpolationType | "" |
| 0x2BC | 12 | vDuckedOfs | vec3_t | "" |
| 0x2C8 | 12 | vDuckedOfsRot | vec3_t | "" |
| 0x2D4 | 12 | vDuckedOfsRotPivot | vec3_t | "" |
| 0x2E0 | 12 | vDuckedMove | vec3_t | "" |
| 0x2EC | 12 | vDuckedRot | vec3_t | "" |
| 0x2F8 | 12 | vProneOfs | vec3_t | "" |
| 0x304 | 12 | vProneMove | vec3_t | "" |
| 0x310 | 12 | vProneRot | vec3_t | "" |
| 0x31C | 4 | adsForwardOffset | float | "" |
| 0x320 | 4 | adsRoll | float | "" |
| 0x324 | 4 | fPosMoveRate | float | "" |
| 0x328 | 4 | fPosProneMoveRate | float | "" |
| 0x32C | 4 | fStandMoveMinSpeed | float | "" |
| 0x330 | 4 | fDuckedMoveMinSpeed | float | "" |
| 0x334 | 4 | fProneMoveMinSpeed | float | "" |
| 0x338 | 4 | fPosRotRate | float | "" |
| 0x33C | 4 | fPosProneRotRate | float | "" |
| 0x340 | 4 | fDuckedOfsRotRate | float | "" |
| 0x344 | 4 | fStandRotMinSpeed | float | "" |
| 0x348 | 4 | fDuckedRotMinSpeed | float | "" |
| 0x34C | 4 | fProneRotMinSpeed | float | "" |
| 0x350 | 4 | fReticleShakeMagnitude | float | "" |
| 0x354 | 4 | fReticleRapidFireMultiplier | float | "" |
| 0x358 | 8 | worldClipModel | XModel * | 1 |
| 0x360 | 8 | rocketModel | XModel * | 1 |
| 0x368 | 8 | hudIcon | GfxImage * | 1 |
| 0x370 | 8 | pickupIcon | GfxImage * | 1 |
| 0x378 | 8 | dangerIconImg | GfxImage * | 1 |
| 0x380 | 8 | throwBackIconImg | GfxImage * | 1 |
| 0x388 | 8 | dangerIconMat | Material * | "" |
| 0x390 | 8 | throwBackIconMat | Material * | "" |
| 0x398 | 1 | hideWarningIcons | bool | "" |
| 0x39C | 4 | warningIconsDelay | float | "" |
| 0x3A0 | 4 | ammoCounterClip | ammoCounterClipType_t | "" |
| 0x3A4 | 4 | iStartAmmo | int | "" |
| 0x3A8 | 4 | iPerkStartAmmo | int | "" |
| 0x3AC | 4 | iPerkMaxAmmo | int | "" |
| 0x3B0 | 8 | szAmmoName | const char * | "" |
| 0x3B8 | 4 | iAmmoIndex | int | "" |
| 0x3BC | 4 | iMaxAmmo | int | "" |
| 0x3C0 | 4 | shotCount | int | "" |
| 0x3C8 | 8 | szSharedAmmoCapName | const char * | "" |
| 0x3D0 | 4 | iSharedAmmoCapIndex | int | "" |
| 0x3D4 | 4 | iSharedAmmoCap | int | "" |
| 0x3D8 | 4 | iAmmoUsedPerShot | int | "" |
| 0x3DC | 1 | requireAmmoUsedPerShot | bool | "" |
| 0x3DD | 1 | disableNoAmmoWarning | bool | "" |
| 0x3E0 | 4 | lowAmmoWarningThreshold | float | "" |
| 0x3E4 | 4 | lowAmmoWarningCount | int | "" |
| 0x3E8 | 4 | iDamageType | int | "" |
| 0x3EC | 4 | iMeleeDamage | int | "" |
| 0x3F0 | 4 | iMeleeCountToFinisher | int | "" |
| 0x3F4 | 1 | hasMeleeFinisher | bool | "" |
| 0x3F8 | 4 | autoAimRange | float | "" |
| 0x3FC | 4 | aimAssistRange | float | "" |
| 0x400 | 4 | aimAssistRangeAds | float | "" |
| 0x404 | 4 | aimAssistPitchSlowdown | float | "" |
| 0x408 | 4 | aimAssistPitchSlowdownAds | float | "" |
| 0x40C | 4 | aimAssistYawSlowdown | float | "" |
| 0x410 | 4 | aimAssistYawSlowdownAds | float | "" |
| 0x414 | 4 | aimAssistLockonStrength | float | "" |
| 0x418 | 4 | aimPadding | float | "" |
| 0x41C | 4 | enemyCrosshairRange | float | "" |
| 0x420 | 4 | moveSpeedScale | float | "" |
| 0x424 | 4 | adsMoveSpeedScale | float | "" |
| 0x428 | 4 | sprintDurationScale | float | "" |
| 0x42C | 4 | sprintRestoreDelay | float | "" |
| 0x430 | 112 | overlay | ADSOverlay | "" |
| 0x4A0 | 4 | overlayInterface | WeapOverlayInteface_t | "" |
| 0x4A4 | 4 | fAdsBobFactor | float | "" |
| 0x4A8 | 4 | fAdsViewBobMult | float | "" |
| 0x4AC | 4 | fAdsGunBobPitchScale | float | "" |
| 0x4B0 | 4 | fAdsGunBobYawScale | float | "" |
| 0x4B4 | 4 | fAdsGunBobTiltPitchScale | float | "" |
| 0x4B8 | 4 | fAdsGunBobTiltYawScale | float | "" |
| 0x4BC | 4 | fAdsGunBobTiltRollScale | float | "" |
| 0x4C0 | 4 | fAdsGunBobTiltBulletDirScale | float | "" |
| 0x4C4 | 4 | fAdsGunBobTiltOffset | float | "" |
| 0x4C8 | 4 | fAdsGunBobCrouchFactor | float | "" |
| 0x4CC | 4 | adsCrouchViewKickFactor | float | "" |
| 0x4D0 | 4 | adsCrouchGunKickFactor | float | "" |
| 0x4D4 | 4 | adsProneViewKickFactor | float | "" |
| 0x4D8 | 4 | adsProneGunKickFactor | float | "" |
| 0x4DC | 4 | fHipSpreadStandMin | float | "" |
| 0x4E0 | 4 | fHipSpreadDuckedMin | float | "" |
| 0x4E4 | 4 | fHipSpreadProneMin | float | "" |
| 0x4E8 | 4 | hipSpreadSprintMin | float | "" |
| 0x4EC | 4 | hipSpreadInAirMin | float | "" |
| 0x4F0 | 4 | hipSpreadStandMax | float | "" |
| 0x4F4 | 4 | hipSpreadDuckedMax | float | "" |
| 0x4F8 | 4 | hipSpreadProneMax | float | "" |
| 0x4FC | 4 | hipSpreadSprintMax | float | "" |
| 0x500 | 4 | hipSpreadInAirMax | float | "" |
| 0x504 | 4 | fHipSpreadDecayRate | float | "" |
| 0x508 | 4 | fHipSpreadFireAdd | float | "" |
| 0x50C | 4 | fHipSpreadTurnAdd | float | "" |
| 0x510 | 4 | fHipSpreadMoveAdd | float | "" |
| 0x514 | 4 | fHipSpreadDuckedDecay | float | "" |
| 0x518 | 4 | fHipSpreadProneDecay | float | "" |
| 0x51C | 4 | hipSpreadSprintDecay | float | "" |
| 0x520 | 4 | hipSpreadInAirDecay | float | "" |
| 0x524 | 4 | fHipReticleSidePos | float | "" |
| 0x528 | 4 | fAdsIdleAmount | float | "" |
| 0x52C | 4 | fHipIdleAmount | float | "" |
| 0x530 | 4 | adsIdleSpeed | float | "" |
| 0x534 | 4 | hipIdleSpeed | float | "" |
| 0x538 | 4 | fIdleCrouchFactor | float | "" |
| 0x53C | 4 | fIdleProneFactor | float | "" |
| 0x540 | 4 | fGunMaxPitch | float | "" |
| 0x544 | 4 | fGunMaxYaw | float | "" |
| 0x548 | 4 | fViewMaxPitch | float | "" |
| 0x54C | 4 | fViewMaxYaw | float | "" |
| 0x550 | 4 | adsIdleLerpStartTime | float | "" |
| 0x554 | 4 | adsIdleLerpTime | float | "" |
| 0x558 | 1 | useUninterruptedAdsIdleMotion | bool | "" |
| 0x559 | 1 | disableInputDrivenViewReturnDampening | bool | "" |
| 0x55C | 4 | slideSpreadMin | float | "" |
| 0x560 | 4 | slideSpreadMax | float | "" |
| 0x564 | 4 | slideSpreadDecayRate | float | "" |
| 0x568 | 4 | slideSpreadFireAdd | float | "" |
| 0x56C | 4 | slideSpreadTurnAdd | float | "" |
| 0x570 | 256 | swaySettings | SwaySettings | "" |
| 0x670 | 4 | adsFireRateScale | float | "" |
| 0x674 | 4 | adsDamageRangeScale | float | "" |
| 0x678 | 4 | adsFireAnimFrac | float | "" |
| 0x67C | 4 | fireTimerLerpToAdsScale | float | "" |
| 0x680 | 1 | alwaysFireAtMaxRangeInAds | bool | "" |
| 0x681 | 1 | adsAlignEnabled | bool | "" |
| 0x682 | 1 | disableTagAlignX | bool | "" |
| 0x684 | 12 | adsAlignOffset | vec3_t | "" |
| 0x690 | 4 | adsAlignOffsetLerpType | AdsOffsetInterpolationType | "" |
| 0x694 | 4 | adsAlignLerpType | AdsOffsetInterpolationType | "" |
| 0x698 | 4 | adsMovementAnimLerpStart | float | "" |
| 0x69C | 4 | adsMovementAnimLerpEnd | float | "" |
| 0x6A0 | 4 | dualWieldViewModelOffset | float | "" |
| 0x6A4 | 4 | fScopeDriftDelay | float | "" |
| 0x6A8 | 4 | fScopeDriftLerpInTime | float | "" |
| 0x6AC | 4 | fScopeDriftSteadyTime | float | "" |
| 0x6B0 | 4 | fScopeDriftLerpOutTime | float | "" |
| 0x6B4 | 4 | fScopeDriftSteadyFactor | float | "" |
| 0x6B8 | 4 | fScopeDriftUnsteadyFactor | float | "" |
| 0x6BC | 4 | killIconRatio | weaponIconRatioType_t | "" |
| 0x6C0 | 4 | iReloadAmmoAdd | int | "" |
| 0x6C4 | 4 | iReloadStartAdd | int | "" |
| 0x6C8 | 1 | reloadTwoRound | bool | "" |
| 0x6CC | 4 | ammoDropStockMin | int | "" |
| 0x6D0 | 4 | ammoDropClipPercentMin | int | "" |
| 0x6D4 | 4 | ammoDropClipPercentMax | int | "" |
| 0x6D8 | 4 | cameraShakeScale | float | "" |
| 0x6DC | 4 | cameraShakeDuration | int | "" |
| 0x6E0 | 4 | cameraShakeRadius | float | "" |
| 0x6E4 | 4 | iExplosionRadius | int | "" |
| 0x6E8 | 4 | iExplosionRadiusMin | int | "" |
| 0x6EC | 4 | iExplosionForceRadius | int | "" |
| 0x6F0 | 4 | iExplosionInnerDamage | int | "" |
| 0x6F4 | 4 | iExplosionOuterDamage | int | "" |
| 0x6F8 | 4 | explosionForceScalar | float | "" |
| 0x6FC | 4 | damageConeAngle | float | "" |
| 0x700 | 4 | bulletExplDmgMultMin | float | "" |
| 0x704 | 4 | bulletExplDmgMult | float | "" |
| 0x708 | 4 | bulletExplRadiusMult | float | "" |
| 0x70C | 4 | iProjectileSpeed | int | "" |
| 0x710 | 4 | iProjectileSpeedUp | int | "" |
| 0x714 | 4 | iProjectileSpeedForward | int | "" |
| 0x718 | 4 | iProjectileActivateDist | int | "" |
| 0x71C | 4 | iProjectileDetonationRadius | int | "" |
| 0x720 | 4 | projLifetime | float | "" |
| 0x724 | 4 | projLifetimeStdDeviation | float | "" |
| 0x728 | 4 | timeToAccelerate | float | "" |
| 0x72C | 4 | projectileCurvature | float | "" |
| 0x730 | 8 | projectileModel | XModel * | 1 |
| 0x738 | 4 | projExplosion | weapProjExposion_t | "" |
| 0x740 | 8 | projExplosionEffect | FxCombinedDef | "" |
| 0x748 | 8 | projDudEffect | FxCombinedDef | "" |
| 0x750 | 8 | projTimeoutEffect | FxCombinedDef | "" |
| 0x758 | 1 | disableFxOnTimeout | bool | "" |
| 0x75C | 4 | stickiness | WeapStickinessType | "" |
| 0x760 | 4 | ricochetChance | float | "" |
| 0x764 | 1 | riotShieldEnableDamage | bool | "" |
| 0x768 | 4 | riotShieldHealth | int | "" |
| 0x76C | 4 | riotShieldDamageMult | float | "" |
| 0x770 | 8 | parallelBounce | float * | 0x40 |
| 0x778 | 8 | perpendicularBounce | float * | 0x40 |
| 0x780 | 8 | vmProjBodyEffect | FxCombinedDef | "" |
| 0x788 | 8 | projBodyEffect | FxCombinedDef | "" |
| 0x790 | 8 | projTrailEffect | FxCombinedDef | "" |
| 0x798 | 8 | projBeaconEffect | FxCombinedDef | "" |
| 0x7A0 | 12 | vProjectileColor | vec3_t | "" |
| 0x7AC | 4 | guidedMissileType | guidedMissileType_t | "" |
| 0x7B0 | 4 | maxSteeringAccel | float | "" |
| 0x7B4 | 4 | projIgnitionDelay | int | "" |
| 0x7B8 | 8 | projIgnitionEffect | FxCombinedDef | "" |
| 0x7C0 | 4 | fAdsAimPitch | float | "" |
| 0x7C4 | 4 | adsInCrosshairAlphaStart | float | "" |
| 0x7C8 | 4 | adsInCrosshairAlphaEnd | float | "" |
| 0x7CC | 4 | adsOutCrosshairAlphaStart | float | "" |
| 0x7D0 | 4 | adsOutCrosshairAlphaEnd | float | "" |
| 0x7D4 | 1 | adsShouldShowCrosshair | bool | "" |
| 0x7D8 | 4 | fAdsGunKickPitchMin | float | "" |
| 0x7DC | 4 | fAdsGunKickPitchMax | float | "" |
| 0x7E0 | 4 | fAdsGunKickYawMin | float | "" |
| 0x7E4 | 4 | fAdsGunKickYawMax | float | "" |
| 0x7E8 | 4 | fAdsGunKickMagMin | float | "" |
| 0x7EC | 4 | fAdsGunKickAccel | float | "" |
| 0x7F0 | 4 | fAdsGunKickSpeedMax | float | "" |
| 0x7F4 | 4 | fAdsGunKickSpeedDecay | float | "" |
| 0x7F8 | 4 | fAdsGunKickStaticDecay | float | "" |
| 0x7FC | 4 | fAdsViewKickPitchMin | float | "" |
| 0x800 | 4 | fAdsViewKickPitchMax | float | "" |
| 0x804 | 4 | fAdsViewKickYawMin | float | "" |
| 0x808 | 4 | fAdsViewKickYawMax | float | "" |
| 0x80C | 4 | fAdsViewKickMagMin | float | "" |
| 0x810 | 4 | fAdsViewScatterMin | float | "" |
| 0x814 | 4 | fAdsViewScatterMax | float | "" |
| 0x818 | 4 | fAdsSpread | float | "" |
| 0x81C | 4 | fAdsSpreadNVG | float | "" |
| 0x820 | 4 | adsSpreadStartFrac | float | "" |
| 0x824 | 4 | adsSpreadEndFrac | float | "" |
| 0x828 | 4 | iVisibilityAxis | int | "" |
| 0x82C | 4 | fVisibilityUpOffset | float | "" |
| 0x830 | 4 | fHipGunKickPitchMin | float | "" |
| 0x834 | 4 | fHipGunKickPitchMax | float | "" |
| 0x838 | 4 | fHipGunKickYawMin | float | "" |
| 0x83C | 4 | fHipGunKickYawMax | float | "" |
| 0x840 | 4 | fHipGunKickMagMin | float | "" |
| 0x844 | 4 | fHipGunKickAccel | float | "" |
| 0x848 | 4 | fHipGunKickSpeedMax | float | "" |
| 0x84C | 4 | fHipGunKickSpeedDecay | float | "" |
| 0x850 | 4 | fHipGunKickStaticDecay | float | "" |
| 0x854 | 4 | fHipViewKickPitchMin | float | "" |
| 0x858 | 4 | fHipViewKickPitchMax | float | "" |
| 0x85C | 4 | fHipViewKickYawMin | float | "" |
| 0x860 | 4 | fHipViewKickYawMax | float | "" |
| 0x864 | 4 | fHipViewKickMagMin | float | "" |
| 0x868 | 4 | fHipViewScatterMin | float | "" |
| 0x86C | 4 | fHipViewScatterMax | float | "" |
| 0x870 | 4 | multipleReloadClipPercentage | float | "" |
| 0x874 | 4 | hipStartingGunKickPercent | float | "" |
| 0x878 | 4 | hipStartingViewKickPercent | float | "" |
| 0x87C | 4 | adsStartingKickBullets | int | "" |
| 0x880 | 4 | adsStartingGunKickPercent | float | "" |
| 0x884 | 4 | adsStartingViewKickPercent | float | "" |
| 0x888 | 4 | hipEndingGunKickPercent | float | "" |
| 0x88C | 4 | hipEndingViewKickPercent | float | "" |
| 0x890 | 4 | adsEndingKickBullets | int | "" |
| 0x894 | 4 | adsEndingGunKickPercent | float | "" |
| 0x898 | 4 | adsEndingViewKickPercent | float | "" |
| 0x89C | 4 | kickAlignedInputScalar | float | "" |
| 0x8A0 | 4 | kickOpposedInputScalar | float | "" |
| 0x8A4 | 4 | viewKickMaintainFraction | float | "" |
| 0x8A8 | 4 | adsGunTiltPitchFactor | float | "" |
| 0x8AC | 4 | adsGunTiltYawFactor | float | "" |
| 0x8B0 | 4 | adsGunTiltRollFactor | float | "" |
| 0x8B4 | 4 | adsGunTiltOffset | float | "" |
| 0x8B8 | 4 | hipGunTiltPitchFactor | float | "" |
| 0x8BC | 4 | hipGunTiltYawFactor | float | "" |
| 0x8C0 | 4 | hipGunTiltRollFactor | float | "" |
| 0x8C4 | 4 | hipGunTiltOffset | float | "" |
| 0x8C8 | 1 | useNewViewKick | bool | "" |
| 0x8C9 | 1 | useNewGunKick | bool | "" |
| 0x8CA | 1 | useAngularViewKick | bool | "" |
| 0x8CB | 1 | useAngularGunKick | bool | "" |
| 0x8CC | 24 | hipAngularViewKickDir | float[6] | "" |
| 0x8E4 | 24 | hipAngularViewKickDev | float[6] | "" |
| 0x8FC | 24 | hipAngularViewKickStrengthMin | float[6] | "" |
| 0x914 | 24 | hipAngularViewKickStrengthMax | float[6] | "" |
| 0x92C | 24 | hipAngularViewKickPitchScale | float[6] | "" |
| 0x944 | 24 | adsAngularViewKickDir | float[6] | "" |
| 0x95C | 24 | adsAngularViewKickDev | float[6] | "" |
| 0x974 | 24 | adsAngularViewKickStrengthMin | float[6] | "" |
| 0x98C | 24 | adsAngularViewKickStrengthMax | float[6] | "" |
| 0x9A4 | 24 | adsAngularViewKickPitchScale | float[6] | "" |
| 0x9BC | 24 | adsAngularViewKickBullet | int[6] | "" |
| 0x9D4 | 6 | adsAngularViewKickUseSet | bool[6] | "" |
| 0x9DC | 24 | hipAngularGunKickDir | float[6] | "" |
| 0x9F4 | 24 | hipAngularGunKickDev | float[6] | "" |
| 0xA0C | 24 | hipAngularGunKickStrengthMin | float[6] | "" |
| 0xA24 | 24 | hipAngularGunKickStrengthMax | float[6] | "" |
| 0xA3C | 24 | hipAngularGunKickPitchScale | float[6] | "" |
| 0xA54 | 24 | adsAngularGunKickDir | float[6] | "" |
| 0xA6C | 24 | adsAngularGunKickDev | float[6] | "" |
| 0xA84 | 24 | adsAngularGunKickStrengthMin | float[6] | "" |
| 0xA9C | 24 | adsAngularGunKickStrengthMax | float[6] | "" |
| 0xAB4 | 24 | adsAngularGunKickPitchScale | float[6] | "" |
| 0xACC | 24 | adsAngularGunKickBullet | int[6] | "" |
| 0xAE4 | 6 | adsAngularGunKickUseSet | bool[6] | "" |
| 0xAEC | 4 | hipViewKickReturnAccelScale | float | "" |
| 0xAF0 | 4 | adsViewKickReturnAccelScale | float | "" |
| 0xAF4 | 4 | hipViewKickReturnSpeedCurveScale | float | "" |
| 0xAF8 | 4 | adsViewKickReturnSpeedCurveScale | float | "" |
| 0xAFC | 4 | hipGunKickReturnAccelScale | float | "" |
| 0xB00 | 4 | adsGunKickReturnAccelScale | float | "" |
| 0xB04 | 4 | hipGunKickReturnSpeedCurveScale | float | "" |
| 0xB08 | 4 | adsGunKickReturnSpeedCurveScale | float | "" |
| 0xB0C | 4 | hipSpreadStandMoveMax | float | "" |
| 0xB10 | 4 | hipSpreadDuckedMoveMax | float | "" |
| 0xB14 | 4 | hipSpreadProneMoveMax | float | "" |
| 0xB18 | 4 | hipSpreadSprintMoveMax | float | "" |
| 0xB1C | 4 | hipSpreadInAirMoveMax | float | "" |
| 0xB20 | 4 | slideSpreadMoveMax | float | "" |
| 0xB24 | 32 | weaponOffsetCurveHoldFireSlow | WeaponOffsetCurveDescription | "" |
| 0xB44 | 32 | weaponOffsetCurveHoldFireFast | WeaponOffsetCurveDescription | "" |
| 0xB64 | 32 | weaponOffsetCurveAds | WeaponOffsetCurveDescription | "" |
| 0xB84 | 32 | weaponOffsetCurveAlwaysOn | WeaponOffsetCurveDescription | "" |
| 0xBA4 | 32 | weaponOffsetCurveKick | WeaponOffsetCurveDescription | "" |
| 0xBC4 | 32 | weaponOffsetCurveSnapDecay | WeaponOffsetCurveDescription | "" |
| 0xBE4 | 4 | numWeaponOffsetPatterns | int | "" |
| 0xBE8 | 8 | weaponOffsetPatterns | WeaponOffsetPatternDescription * | (72 * varWeaponDef->numWeaponOffsetPatterns) / 72 |
| 0xBF0 | 4 | numWeaponOffsetPatternsKickOrSnapDecay | int | "" |
| 0xBF8 | 8 | weaponOffsetPatternsKickOrSnapDecay | WeaponOffsetPatternDescription ** | (8 * varWeaponDef->numWeaponOffsetPatternsKickOrSnapDecay) / 8 |
| 0xC00 | 4 | fightDist | float | "" |
| 0xC04 | 4 | maxDist | float | "" |
| 0xC08 | 16 | accuracyGraphName | const char *[2] | "" |
| 0xC18 | 16 | originalAccuracyGraphKnots | vec2_t *[2] | "" |
| 0xC28 | 4 | originalAccuracyGraphKnotCount | unsigned __int16[2] | "" |
| 0xC2C | 4 | iPositionReloadTransTime | int | "" |
| 0xC30 | 4 | leftArc | float | "" |
| 0xC34 | 4 | rightArc | float | "" |
| 0xC38 | 4 | topArc | float | "" |
| 0xC3C | 4 | bottomArc | float | "" |
| 0xC40 | 4 | visualPitchLimitTop | float | "" |
| 0xC44 | 4 | visualPitchLimitBottom | float | "" |
| 0xC48 | 1 | softLeftRightArc | bool | "" |
| 0xC4C | 4 | accuracy | float | "" |
| 0xC50 | 4 | aiSpread | float | "" |
| 0xC54 | 4 | playerSpread | float | "" |
| 0xC58 | 8 | minTurnSpeed | float[2] | "" |
| 0xC60 | 8 | maxTurnSpeed | float[2] | "" |
| 0xC68 | 4 | pitchConvergenceTime | float | "" |
| 0xC6C | 4 | yawConvergenceTime | float | "" |
| 0xC70 | 4 | suppressTime | float | "" |
| 0xC74 | 4 | suppressTimeTargetKilled | float | "" |
| 0xC78 | 8 | aiReturnToDefaultSpeed | float[2] | "" |
| 0xC80 | 4 | maxRange | float | "" |
| 0xC84 | 4 | fAnimHorRotateInc | float | "" |
| 0xC88 | 4 | fPlayerPositionDist | float | "" |
| 0xC90 | 8 | szUseHintString | const char * | "" |
| 0xC98 | 8 | dropHintString | const char * | "" |
| 0xCA0 | 4 | iUseHintStringIndex | unsigned int | "" |
| 0xCA4 | 4 | dropHintStringIndex | unsigned int | "" |
| 0xCA8 | 4 | horizViewJitter | float | "" |
| 0xCAC | 4 | vertViewJitter | float | "" |
| 0xCB0 | 1 | enableViewBounceFire | bool | "" |
| 0xCB4 | 4 | viewBounceFireDecay | float | "" |
| 0xCB8 | 4 | viewBounceFireFrequency | float | "" |
| 0xCBC | 4 | viewBounceFirePitchScale | float | "" |
| 0xCC0 | 4 | viewBounceFireRollScale | float | "" |
| 0xCC4 | 4 | scanSpeed | float | "" |
| 0xCC8 | 4 | scanAccel | float | "" |
| 0xCCC | 4 | scanPauseTime | int | "" |
| 0xCD0 | 8 | szScript | const char * | "" |
| 0xCD8 | 8 | adsSpeedMs | float[2] | "" |
| 0xCE0 | 8 | adsAccelSec | float[2] | "" |
| 0xCE8 | 1 | deferDamageToParentVehicle | bool | "" |
| 0xCE9 | 1 | useSteppedExplosionDamage | bool | "" |
| 0xCEC | 4 | destabilizationRateTime | float | "" |
| 0xCF0 | 4 | destabilizationCurvatureMax | float | "" |
| 0xCF4 | 4 | destabilizeDistance | int | "" |
| 0xCF8 | 4 | robotechMaxPitch | float | "" |
| 0xCFC | 4 | robotechMaxYaw | float | "" |
| 0xD00 | 4 | robotechFrequency | float | "" |
| 0xD04 | 4 | robotechVariancePitch | float | "" |
| 0xD08 | 4 | robotechVarianceYaw | float | "" |
| 0xD0C | 4 | robotechVarianceFrequency | float | "" |
| 0xD10 | 8 | locationDamageMultipliers | float * | 0x16 |
| 0xD18 | 8 | hitLocPriorityMap | unsigned __int8 * | 0x16 |
| 0xD20 | 28 | unittypeMultipliers | float[7] | "" |
| 0xD40 | 8 | fireRumble | RumbleInfo * | 1 |
| 0xD48 | 8 | meleeImpactRumble | RumbleInfo * | 1 |
| 0xD50 | 4 | heatAccumulationPerShot | float | "" |
| 0xD54 | 4 | heatDissipationPerSecond | float | "" |
| 0xD58 | 4 | heatSmokeStartThreshold | float | "" |
| 0xD5C | 4 | heatSmokeStopThreshold | float | "" |
| 0xD60 | 1 | heatIsOpenBolt | bool | "" |
| 0xD64 | 4 | tracerStyle | tracerStyle_t | "" |
| 0xD68 | 8 | tracerType | TracerDef * | 1 |
| 0xD70 | 8 | overchargeTracerType | TracerDef * | 1 |
| 0xD78 | 8 | laserTypeViewModel | LaserDef * | 1 |
| 0xD80 | 8 | laserTypeViewModelAlt | LaserDef * | 1 |
| 0xD88 | 8 | laserTypeFriendly | LaserDef * | 1 |
| 0xD90 | 8 | laserTypeEnemy | LaserDef * | 1 |
| 0xD98 | 1 | turretADSEnabled | bool | "" |
| 0xD9C | 4 | turretADSTime | float | "" |
| 0xDA0 | 4 | turretFov | float | "" |
| 0xDA4 | 4 | turretFovADS | float | "" |
| 0xDA8 | 4 | turretScopeZoomRate | float | "" |
| 0xDAC | 4 | turretScopeZoomMin | float | "" |
| 0xDB0 | 4 | turretScopeZoomMax | float | "" |
| 0xDB4 | 4 | turretOverheatPenalty | float | "" |
| 0xDB8 | 8 | turretOverheatSound | SndAliasLookup | "" |
| 0xDC0 | 8 | turretOverheatEffect | FxCombinedDef | "" |
| 0xDC8 | 8 | turretBarrelSpinRumble | RumbleInfo * | 1 |
| 0xDD0 | 4 | turretBarrelSpinSpeed | float | "" |
| 0xDD4 | 4 | turretBarrelSpinUpTime | float | "" |
| 0xDD8 | 4 | turretBarrelSpinDownTime | float | "" |
| 0xDE0 | 8 | turretBarrelSpinMaxSnd | SndAliasLookup | "" |
| 0xDE8 | 32 | turretBarrelSpinUpSnd | SndAliasLookup[4] | "" |
| 0xE08 | 32 | turretBarrelSpinDownSnd | SndAliasLookup[4] | "" |
| 0xE28 | 8 | missileConeSoundAlias | SndAliasLookup | "" |
| 0xE30 | 8 | missileConeSoundAliasAtBase | SndAliasLookup | "" |
| 0xE38 | 4 | missileConeSoundRadiusAtTop | float | "" |
| 0xE3C | 4 | missileConeSoundRadiusAtBase | float | "" |
| 0xE40 | 4 | missileConeSoundHeight | float | "" |
| 0xE44 | 4 | missileConeSoundOriginOffset | float | "" |
| 0xE48 | 4 | missileConeSoundVolumescaleAtCore | float | "" |
| 0xE4C | 4 | missileConeSoundVolumescaleAtEdge | float | "" |
| 0xE50 | 4 | missileConeSoundVolumescaleCoreSize | float | "" |
| 0xE54 | 4 | missileConeSoundPitchAtTop | float | "" |
| 0xE58 | 4 | missileConeSoundPitchAtBottom | float | "" |
| 0xE5C | 4 | missileConeSoundPitchTopSize | float | "" |
| 0xE60 | 4 | missileConeSoundPitchBottomSize | float | "" |
| 0xE64 | 4 | missileConeSoundCrossfadeTopSize | float | "" |
| 0xE68 | 4 | missileConeSoundCrossfadeBottomSize | float | "" |
| 0xE6C | 1 | meleeOverrideValues | bool | "" |
| 0xE70 | 4 | aim_automelee_lerp | float | "" |
| 0xE74 | 4 | aim_automelee_region_height | float | "" |
| 0xE78 | 4 | aim_automelee_region_width | float | "" |
| 0xE7C | 4 | aim_automelee_maxPitchMovement | float | "" |
| 0xE80 | 4 | aim_automelee_maxYawMovement | float | "" |
| 0xE84 | 4 | player_meleeHeight | float | "" |
| 0xE88 | 4 | player_meleeWidth | float | "" |
| 0xE8C | 4 | playerMeleeRangeStanding | float | "" |
| 0xE90 | 4 | playerMeleeRangeCrouched | float | "" |
| 0xE94 | 4 | playerMeleeRangeProne | float | "" |
| 0xE98 | 4 | playerMeleeRangeChargeStanding | float | "" |
| 0xE9C | 4 | playerMeleeRangeChargeCrouched | float | "" |
| 0xEA0 | 4 | playerMeleeChargeHeightTolerance | float | "" |
| 0xEA4 | 1 | shieldAllowFiring | bool | "" |
| 0xEA8 | 4 | shieldMaxSpeed | offhandShieldMaxSpeed_t | "" |
| 0xEAC | 1 | shieldAlwaysDisplay | bool | "" |
| 0xEB0 | 8 | shieldDeployGesture | Gesture * | 1 |
| 0xEB8 | 8 | shieldFireWeapGesture | Gesture * | 1 |
| 0xEC0 | 8 | shieldDeployWhileFiring | Gesture * | 1 |
| 0xEC8 | 8 | shieldRetractWhileFiring | Gesture * | 1 |
| 0xED0 | 8 | shieldBashGesture | Gesture * | 1 |
| 0xED8 | 8 | shieldMeleeFx | FxCombinedDef | "" |
| 0xEE0 | 4 | shieldMeleeFxDelay | float | "" |
| 0xEE4 | 4 | HitEarthquakeScale | float | "" |
| 0xEE8 | 4 | HitEarthquakeDuration | float | "" |
| 0xEEC | 4 | HitEarthquakeRadius | float | "" |
| 0xEF0 | 8 | shieldHitRumble | RumbleInfo * | 1 |
| 0xEF8 | 4 | MissEarthquakeScale | float | "" |
| 0xEFC | 4 | MissEarthquakeDuration | float | "" |
| 0xF00 | 4 | MissEarthquakeRadius | float | "" |
| 0xF08 | 8 | shieldMissRumble | RumbleInfo * | 1 |
| 0xF10 | 4 | shieldDeployButton | offhandShieldButton_t | "" |
| 0xF14 | 1 | shieldUsesEnergy | bool | "" |
| 0xF18 | 4 | shieldMaxEnergy | float | "" |
| 0xF1C | 4 | shieldConsumptionRate | float | "" |
| 0xF20 | 4 | shieldMeleeEnergyCost | float | "" |
| 0xF24 | 4 | shieldMeleeHitEnergyCost | float | "" |
| 0xF28 | 4 | reactiveMotionRadiusScale | float | "" |
| 0xF2C | 4 | reactiveMotionFrequencyScale | float | "" |
| 0xF30 | 4 | reactiveMotionAmplitudeScale | float | "" |
| 0xF34 | 4 | reactiveMotionFalloff | float | "" |
| 0xF38 | 4 | reactiveMotionLifetime | float | "" |
| 0xF3C | 1 | transientBaseViewFlags | unsigned __int8 | "" |
| 0xF3D | 1 | transientBaseWorldFlags | unsigned __int8 | "" |
| 0xF3E | 1 | hasAnyTransientModels | bool | "" |
| 0xF3F | 1 | blendFireAnimations | bool | "" |
| 0xF40 | 1 | sharedAmmo | bool | "" |
| 0xF41 | 1 | lockonSupported | bool | "" |
| 0xF42 | 1 | requireLockonToFire | bool | "" |
| 0xF43 | 1 | isAirburstWeapon | bool | "" |
| 0xF44 | 1 | bigExplosion | bool | "" |
| 0xF45 | 1 | noAdsWhenMagEmpty | bool | "" |
| 0xF46 | 1 | avoidDropCleanup | bool | "" |
| 0xF47 | 1 | allowGrenadeSwitching | bool | "" |
| 0xF48 | 1 | inheritsPerks | bool | "" |
| 0xF49 | 1 | forceUse | bool | "" |
| 0xF4A | 1 | ladderWeapon | bool | "" |
| 0xF4B | 1 | executionVictimHiddenWeapon | bool | "" |
| 0xF4C | 1 | crosshairColorChange | bool | "" |
| 0xF4D | 1 | bRifleBullet | bool | "" |
| 0xF4E | 1 | bEnergyBullet | bool | "" |
| 0xF4F | 1 | bIncendiaryBullet | bool | "" |
| 0xF50 | 1 | armorPiercing | bool | "" |
| 0xF51 | 1 | impaling | bool | "" |
| 0xF52 | 1 | bBoltAction | bool | "" |
| 0xF53 | 1 | aimDownSight | bool | "" |
| 0xF54 | 1 | canHoldBreath | bool | "" |
| 0xF55 | 1 | meleeOnly | bool | "" |
| 0xF56 | 1 | supportsAlternateMelee | bool | "" |
| 0xF57 | 1 | bRechamberWhileAds | bool | "" |
| 0xF58 | 1 | reloadWhileAds | bool | "" |
| 0xF59 | 1 | bBulletExplosiveDamage | bool | "" |
| 0xF5A | 1 | bCookOffHold | bool | "" |
| 0xF5B | 1 | overCookIsNotLethal | bool | "" |
| 0xF5C | 1 | reticleSpin45 | bool | "" |
| 0xF5D | 1 | bClipOnly | bool | "" |
| 0xF5E | 1 | bDoesNotConsumeAmmo | bool | "" |
| 0xF5F | 1 | bRemoveWeaponOnEmpty | bool | "" |
| 0xF60 | 1 | noAmmoPickup | bool | "" |
| 0xF61 | 1 | adsFireOnly | bool | "" |
| 0xF62 | 1 | cancelAutoHolsterWhenEmpty | bool | "" |
| 0xF63 | 1 | disableSwitchToWhenEmpty | bool | "" |
| 0xF64 | 1 | suppressAmmoPrimaryDisplay | bool | "" |
| 0xF65 | 1 | suppressAmmoReserveDisplay | bool | "" |
| 0xF66 | 10 | laserSettings | LaserSettings | "" |
| 0xF70 | 1 | markableViewmodel | bool | "" |
| 0xF71 | 1 | isPredictedProjectile | bool | "" |
| 0xF74 | 4 | clientTrajectoryBlendOutTime | int | "" |
| 0xF78 | 4 | serverTrajectoryBlendInTime | int | "" |
| 0xF7C | 4 | dualWieldType | DualWieldType | "" |
| 0xF80 | 1 | flipKillIcon | bool | "" |
| 0xF81 | 1 | bNoPartialReload | bool | "" |
| 0xF82 | 1 | reloadDisabled | bool | "" |
| 0xF83 | 1 | blocksProne | bool | "" |
| 0xF84 | 1 | silenced | bool | "" |
| 0xF85 | 1 | doNotSuppressAI | bool | "" |
| 0xF86 | 1 | isRollingGrenade | bool | "" |
| 0xF87 | 1 | dropGrenadeHeldOnDeath | bool | "" |
| 0xF88 | 1 | projExplosionEffectForceNormalUp | bool | "" |
| 0xF89 | 1 | projExplosionEffectInheritParentDirection | bool | "" |
| 0xF8A | 1 | bProjImpactExplode | bool | "" |
| 0xF8B | 1 | spawnProjAtMuzzle | bool | "" |
| 0xF8C | 1 | correctBulletTrajectory | bool | "" |
| 0xF90 | 4 | maxCorrectionAngle | float | "" |
| 0xF94 | 1 | disableProjectileCrumpleCheck | bool | "" |
| 0xF95 | 1 | bProjTrajectoryEvents | bool | "" |
| 0xF96 | 1 | bProjWhizByEnabled | bool | "" |
| 0xF97 | 1 | stickToPlayers | bool | "" |
| 0xF98 | 1 | stickToVehicles | bool | "" |
| 0xF99 | 1 | stickToTurrets | bool | "" |
| 0xF9A | 1 | stickToNonStick | bool | "" |
| 0xF9B | 1 | projEnableMissileStickiness | bool | "" |
| 0xF9C | 1 | enableMissileRicochet | bool | "" |
| 0xF9D | 1 | thrownSideways | bool | "" |
| 0xF9E | 1 | disableFiring | bool | "" |
| 0xF9F | 1 | firesOnWeaponSwitch | bool | "" |
| 0xFA0 | 1 | disableHolding | bool | "" |
| 0xFA1 | 1 | timedDetonation | bool | "" |
| 0xFA4 | 4 | missileRicochetMinAngleOfIncidence | float | "" |
| 0xFA8 | 4 | missileCrumpleMinSpeed | float | "" |
| 0xFAC | 4 | detonateType | WeapDetonateType | "" |
| 0xFB0 | 8 | detonatorWorldModel | XModel * | 1 |
| 0xFB8 | 4 | explosiveDamageDelay | int | "" |
| 0xFBC | 4 | fuseTime | int | "" |
| 0xFC0 | 4 | aiFuseTime | int | "" |
| 0xFC4 | 4 | maxHoldTime | int | "" |
| 0xFC8 | 52 | rotationParams | GrenadeRotationParams | "" |
| 0xFFC | 1 | holdButtonToThrow | bool | "" |
| 0xFFD | 1 | autoHold | bool | "" |
| 0xFFE | 1 | infiniteHold | bool | "" |
| 0xFFF | 1 | freezeMovementWhenFiring | bool | "" |
| 0x1000 | 1 | offhandAllowsSprint | bool | "" |
| 0x1001 | 1 | thermalScope | bool | "" |
| 0x1002 | 1 | thermalToggle | bool | "" |
| 0x1003 | 1 | outlineEnemies | bool | "" |
| 0x1004 | 1 | outlineDepthTest | bool | "" |
| 0x1005 | 1 | outlineFill | bool | "" |
| 0x1008 | 4 | enemyOutlineR | float | "" |
| 0x100C | 4 | enemyOutlineG | float | "" |
| 0x1010 | 4 | enemyOutlineB | float | "" |
| 0x1014 | 4 | enemyOutlineA | float | "" |
| 0x1018 | 4 | allyOutlineR | float | "" |
| 0x101C | 4 | allyOutlineG | float | "" |
| 0x1020 | 4 | allyOutlineB | float | "" |
| 0x1024 | 4 | allyOutlineA | float | "" |
| 0x1028 | 1 | depthScan | bool | "" |
| 0x102C | 4 | depthScanThickness | float | "" |
| 0x1030 | 4 | depthScanR | float | "" |
| 0x1034 | 4 | depthScanG | float | "" |
| 0x1038 | 4 | depthScanB | float | "" |
| 0x103C | 4 | depthScanA | float | "" |
| 0x1040 | 4 | depthScanOutlineThickness | float | "" |
| 0x1044 | 4 | depthScanOutlineR | float | "" |
| 0x1048 | 4 | depthScanOutlineG | float | "" |
| 0x104C | 4 | depthScanOutlineB | float | "" |
| 0x1050 | 4 | depthScanOutlineA | float | "" |
| 0x1054 | 1 | depthScanOverlay | bool | "" |
| 0x1058 | 4 | depthScanOverlayStrength | float | "" |
| 0x105C | 4 | depthScanOverlayXTiles | float | "" |
| 0x1060 | 4 | depthScanOverlayYTiles | float | "" |
| 0x1064 | 4 | depthScanOverlayXScroll | float | "" |
| 0x1068 | 4 | depthScanOverlayYScroll | float | "" |
| 0x106C | 4 | blurSceneAdsInFraction | float | "" |
| 0x1070 | 4 | blurSceneAdsOutFraction | float | "" |
| 0x1074 | 1 | altModeSameWeapon | bool | "" |
| 0x1075 | 1 | playAltGesturesForOffhandWeapons | bool | "" |
| 0x1076 | 1 | turretBarrelSpinEnabled | bool | "" |
| 0x1077 | 1 | missileConeSoundEnabled | bool | "" |
| 0x1078 | 1 | missileConeSoundPitchshiftEnabled | bool | "" |
| 0x1079 | 1 | missileConeSoundCrossfadeEnabled | bool | "" |
| 0x107A | 1 | offhandHoldIsCancelable | bool | "" |
| 0x107B | 1 | doNotAllowAttachmentsToOverrideSpread | bool | "" |
| 0x107C | 1 | useScopeDrift | bool | "" |
| 0x107D | 1 | alwaysShatterGlassOnImpact | bool | "" |
| 0x107E | 1 | jumpSpread | bool | "" |
| 0x107F | 1 | noFullViewmodelAnimations | bool | "" |
| 0x1080 | 4 | killcamOffset | float | "" |
| 0x1084 | 1 | useDualFOV | bool | "" |
| 0x1085 | 1 | showViewModelInDualFOV | bool | "" |
| 0x1086 | 1 | syncedFOVInDualFOV | bool | "" |
| 0x1087 | 1 | disableDrop | bool | "" |
| 0x1088 | 1 | preferredDrop | bool | "" |
| 0x1090 | 8 | gestureAnimation | Gesture * | 1 |
| 0x1098 | 4 | gestureFireStateTime | float | "" |
| 0x10A0 | 8 | gesturePullback | Gesture * | 1 |
| 0x10A8 | 8 | gesturePullbackAlt | Gesture * | 1 |
| 0x10B0 | 4 | minPullbackTime | float | "" |
| 0x10B4 | 4 | minPullbackToThrowBlendTime | float | "" |
| 0x10B8 | 4 | maxPullbackToThrowBlendTime | float | "" |
| 0x10C0 | 8 | gestureThrow | Gesture * | 1 |
| 0x10C8 | 8 | gestureThrowAlt | Gesture * | 1 |
| 0x10D0 | 4 | gestureFireTime | float | "" |
| 0x10D8 | 8 | gestureDetonate | Gesture * | 1 |
| 0x10E0 | 8 | gestureDetonateAlt | Gesture * | 1 |
| 0x10E8 | 4 | gestureDetonationTime | float | "" |
| 0x10EC | 1 | gesturesDisablePrimary | bool | "" |
| 0x10F0 | 8 | cameraFireEffect | FxCombinedDef | "" |
| 0x10F8 | 4 | cameraFireEffectDurationSec | float | "" |
| 0x10FC | 4 | changedFireTime | float | "" |
| 0x1100 | 4 | changedFireTimeAkimbo | float | "" |
| 0x1104 | 4 | changedFireTimeNumBullets | int | "" |
| 0x1108 | 4 | chargeType | WeaponChargeType | "" |
| 0x110C | 4 | chargeGain | float | "" |
| 0x1110 | 4 | chargeCostPerShot | float | "" |
| 0x1114 | 4 | chargeLossWhenIdle | float | "" |
| 0x1118 | 4 | chargeEmptyCooldown | float | "" |
| 0x111C | 4 | chargeFireAtMaxDamageMultiplier | float | "" |
| 0x1120 | 4 | chargeMeterEffectBoneCount | int | "" |
| 0x1128 | 8 | chargeMeterEffect | FxCombinedDef | "" |
| 0x1130 | 8 | chargeUpSound | SndAliasLookup | "" |
| 0x1138 | 8 | chargeDownSound | SndAliasLookup | "" |
| 0x1140 | 8 | chargeUpSoundPlayer | SndAliasLookup | "" |
| 0x1148 | 8 | chargeDownSoundPlayer | SndAliasLookup | "" |
| 0x1150 | 8 | chargeDownToUpSound | SndAliasLookup | "" |
| 0x1158 | 8 | chargeDownToUpSoundPlayer | SndAliasLookup | "" |
| 0x1160 | 8 | chargeUpToDownSound | SndAliasLookup | "" |
| 0x1168 | 8 | chargeUpToDownSoundPlayer | SndAliasLookup | "" |
| 0x1170 | 8 | chargeMaxSound | SndAliasLookup | "" |
| 0x1178 | 8 | chargeMaxSoundPlayer | SndAliasLookup | "" |
| 0x1180 | 1 | chargeHudReveal | bool | "" |
| 0x1188 | 8 | chargeRumble | RumbleInfo * | 1 |
| 0x1190 | 4 | stowTag | scr_string_t | "" |
| 0x1198 | 8 | stowOffsetModel | XModel * | 1 |
| 0x11A0 | 4 | stowOffsetAttachTag | scr_string_t | "" |
| 0x11A4 | 4 | slot | WeaponSlot | "" |
| 0x11A8 | 4 | maxTargetAngle | float | "" |
| 0x11AC | 1 | spaceshipSecondaryWeapon | bool | "" |
| 0x11B0 | 4 | impulseFieldRadius | float | "" |
| 0x11B4 | 4 | impulseFieldInitialSpeed | float | "" |
| 0x11B8 | 4 | impulseFieldMaxSpeed | float | "" |
| 0x11BC | 4 | impulseFieldAcceleration | float | "" |
| 0x11C0 | 4 | impulseFieldInAirImpulseMultiplier | float | "" |
| 0x11C4 | 4 | impulseFieldInAirImpulseMultiplierInterpTime | float | "" |
| 0x11C8 | 4 | impulseFieldSlideMultiplier | float | "" |
| 0x11CC | 4 | impulseFieldSlideMultiplierInterpTime | float | "" |
| 0x11D0 | 1 | impulseFieldIsPush | bool | "" |
| 0x11D1 | 1 | impulseFieldAffectsFriendlies | bool | "" |
| 0x11D2 | 1 | impulseFieldAffectsSelf | bool | "" |
| 0x11D3 | 1 | impulseFieldAffectsProne | bool | "" |
| 0x11D4 | 1 | regenerationEnabled | bool | "" |
| 0x11D8 | 4 | regenerationTimeMs | int | "" |
| 0x11DC | 4 | regenerationAddTimeMs | int | "" |
| 0x11E0 | 4 | regenerationAmount | int | "" |
| 0x11E4 | 1 | regenerationConsumeStock | bool | "" |
| 0x11E5 | 1 | regenerationDisableWhileFiring | bool | "" |
| 0x11E8 | 4 | deployType | DeployType | "" |
| 0x11EC | 1 | deployRequireOnWalkableSurface | bool | "" |
| 0x11ED | 1 | deployRequireOnNavmesh | bool | "" |
| 0x11EE | 1 | deployRequireSkyAbove | bool | "" |
| 0x11EF | 1 | deployRequireNoOverhang | bool | "" |
| 0x11F0 | 1 | deployAlwaysUpright | bool | "" |
| 0x11F1 | 1 | deployEdgeSnap | bool | "" |
| 0x11F4 | 4 | deployCylinderRadius | float | "" |
| 0x11F8 | 4 | deployCylinderHeight | float | "" |
| 0x11FC | 4 | deployMaxDistance | float | "" |
| 0x1200 | 4 | deployMaxHeightAboveEye | float | "" |
| 0x1204 | 4 | deployEffectHeightOffset | float | "" |
| 0x1208 | 4 | deployMaxSlope | float | "" |
| 0x1210 | 8 | deployEffect | FxCombinedDef | "" |
| 0x1218 | 4 | deployValidGroupId | ParticleGroupID | "" |
| 0x121C | 4 | deployIndoorGroupId | ParticleGroupID | "" |
| 0x1220 | 4 | deployInvalidGroupId | ParticleGroupID | "" |
| 0x1224 | 4 | deployOutOfRangeGroupId | ParticleGroupID | "" |
| 0x1228 | 4 | numAnimOverrides | unsigned int | "" |
| 0x1230 | 8 | animOverrides | AnimOverride * | (32 * varWeaponDef->numAnimOverrides) / 32 |
| 0x1238 | 120 | carryAnimOverrides | CarryAnimOverride[5] | "" |
| 0x12B0 | 4 | numSfxOverrides | unsigned int | "" |
| 0x12B8 | 8 | sfxOverrides | SFXOverride * | (32 * varWeaponDef->numSfxOverrides) / 32 |
| 0x12C0 | 4 | numVfxOverrides | unsigned int | "" |
| 0x12C8 | 8 | vfxOverrides | VFXOverride * | (32 * varWeaponDef->numVfxOverrides) / 32 |
| 0x12D0 | 4 | reactiveEmitterDelay | float | "" |
| 0x12D4 | 4 | grenadeDangerIconDistance | float | "" |
| 0x12D8 | 4 | bulletDirGunAngleModifierIdleHip | float | "" |
| 0x12DC | 4 | bulletDirGunAngleModifierIdleAds | float | "" |
| 0x12E0 | 4 | bulletDirGunAngleModifierGunKickHip | float | "" |
| 0x12E4 | 4 | bulletDirGunAngleModifierGunKickAds | float | "" |
| 0x12E8 | 4 | bulletDirGunAngleModifierGunTiltHip | float | "" |
| 0x12EC | 4 | bulletDirGunAngleModifierGunTiltAds | float | "" |
| 0x12F0 | 4 | viewClampPitchCatchUpTimeSec | float | "" |
| 0x12F4 | 4 | viewClampYawCatchUpTimeSec | float | "" |
| 0x12F8 | 1 | mountTopEnable | bool | "" |
| 0x12F9 | 1 | mountTopYawClamp | bool | "" |
| 0x12FC | 4 | mountTopYawMax | float | "" |
| 0x1300 | 4 | mountTopGunKickScale | float | "" |
| 0x1304 | 4 | mountTopGunCenterScale | float | "" |
| 0x1308 | 4 | mountTopViewKickScale | float | "" |
| 0x130C | 4 | mountTopViewCenterScale | float | "" |
| 0x1310 | 4 | mountTopGunIdleMotionScale | float | "" |
| 0x1314 | 4 | mountTopViewIdleMotionScale | float | "" |
| 0x1318 | 4 | mountTopEnterDurationMs | int | "" |
| 0x131C | 4 | mountTopExitDurationMs | int | "" |
| 0x1320 | 4 | mountTopEdgeToEyeDistanceForward | float | "" |
| 0x1324 | 4 | mountTopEdgeToEyeDistanceAbove | float | "" |
| 0x1328 | 1 | mountSideEnable | bool | "" |
| 0x1329 | 1 | mountSideTransitionEnable | bool | "" |
| 0x132C | 4 | mountSideRoll | float | "" |
| 0x1330 | 4 | mountSideRollStartFrac | float | "" |
| 0x1334 | 4 | mountSideRollEndFrac | float | "" |
| 0x1338 | 4 | mountSideGunKickScale | float | "" |
| 0x133C | 4 | mountSideGunCenterScale | float | "" |
| 0x1340 | 4 | mountSideViewKickScale | float | "" |
| 0x1344 | 4 | mountSideViewCenterScale | float | "" |
| 0x1348 | 4 | mountSideGunIdleMotionScale | float | "" |
| 0x134C | 4 | mountSideViewIdleMotionScale | float | "" |
| 0x1350 | 4 | mountSideEnterDurationMs | int | "" |
| 0x1354 | 4 | mountSideExitDurationMs | int | "" |
| 0x1358 | 4 | mountSideEdgeToEyeDistanceForward | float | "" |
| 0x135C | 4 | mountSideEdgeToEyeDistanceAbove | float | "" |
| 0x1360 | 4 | mountViewmodelOffset | float | "" |
| 0x1364 | 4 | mountFOVScale | float | "" |
| 0x1368 | 8 | mountRumble | RumbleInfo * | 1 |
| 0x1370 | 16 | mountWeaponOffsetPatternScaleInfo | WeaponOffsetPatternScaleInfo | "" |
| 0x1380 | 4 | adsFireMotionBlur | float | "" |
| 0x1384 | 4 | adsFireMotionBlurDurationMs | int | "" |
| 0x1388 | 4 | adsFireMotionBlurDecayExponent | float | "" |
| 0x138C | 4 | hipFireMotionBlur | float | "" |
| 0x1390 | 4 | hipFireMotionBlurDurationMs | int | "" |
| 0x1394 | 4 | hipFireMotionBlurDecayExponent | float | "" |
| 0x1398 | 4 | adsCameraShakeRotationScale | float | "" |
| 0x139C | 4 | adsCameraShakeTranslationScale | float | "" |
| 0x13A0 | 152 | advancedIdleSettings | AdvancedIdleSettings | "" |
| 0x1438 | 16 | crouchedWeaponOffsetPatternScaleInfo | WeaponOffsetPatternScaleInfo | "" |
| 0x1448 | 16 | proneWeaponOffsetPatternScaleInfo | WeaponOffsetPatternScaleInfo | "" |
| 0x1458 | 40 | ballisticInfo | BallisticInfo | "" |
| 0x1480 | 16 | notifyTypes | WeaponEntityNotify *[2] | "" |
| 0x1490 | 24 | dlcFloat | float[6] | "" |
| 0x14A8 | 3 | dlcBool | bool[3] | "" |
| 0x14AB | 1 | enableWeaponInspect | bool | "" |
| 0x14AC | 1 | stowedOcclusionTestEnabled | bool | "" |
| 0x14AD | 1 | ignoreMinTracerSpawnDistance | bool | "" |
| 0x14AE | 1 | tracerOverrideEnabled | bool | "" |
| 0x14AF | 1 | boltActionReloadIncludesRechamber | bool | "" |

Unmapped/padding bit ranges: 2720+32, 3688+24, 3784+24, 3888+16, 3952+16, 4064+32, 4232+24, 4320+32, 4648+24, 4816+16, 7368+24, 7712+32, 7920+16, 8104+24, 10960+16, 13336+8, 13896+24, 14816+32, 15048+24, 15144+24, 16040+24, 20176+16, 22352+16, 24480+32, 25160+24, 25696+32, 25992+24, 26448+16, 27104+32, 27400+24, 27848+24, 28384+32, 29544+24, 29992+24, 30056+24, 30752+32, 30888+24, 31632+16, 31848+24, 32016+16, 32816+16, 33096+24, 33448+24, 33864+56, 34016+32, 34272+32, 34464+32, 34664+24, 35104+32, 35848+56, 36000+32, 36200+24, 36520+24, 36656+16, 36752+16, 36960+32, 37216+32, 38304+32, 38432+32, 38864+16, 39248+16.

### WeaponEntityNotify

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | flags | WeaponEntityNotifyFlags | "" |
| 0x4 | 4 | radius | float | "" |
| 0x8 | 4 | height | float | "" |
| 0xC | 4 | minSpeed | float | "" |

### WeaponOffsetCurveDescription

Size `0x20`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | blendTime | float | "" |
| 0x4 | 4 | decayTime | float | "" |
| 0x8 | 4 | shotDecayFireTimeFrac | float | "" |
| 0xC | 4 | holdTime | float | "" |
| 0x10 | 4 | adsFractionBegin | float | "" |
| 0x14 | 4 | adsFractionEnd | float | "" |
| 0x18 | 4 | interpType | WeaponOffsetBlendInterpolationType | "" |
| 0x1C | 4 | interpTypeOut | WeaponOffsetBlendInterpolationType | "" |

### WeaponOffsetPatternDescription

Size `0x48`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 1 | active | bool | "" |
| 0x4 | 4 | patternKey | scr_string_t | "" |
| 0x8 | 4 | curveType | WeaponOffsetBlendCurveId | "" |
| 0xC | 4 | patternType | WeaponOffsetPatternId | "" |
| 0x10 | 4 | transformType | WeaponOffsetTransformType | "" |
| 0x14 | 4 | frequency | float | "" |
| 0x18 | 4 | blendTime | float | "" |
| 0x1C | 12 | magnitude | vec3_t | "" |
| 0x28 | 4 | hipScale | float | "" |
| 0x2C | 4 | rotationOffset | float | "" |
| 0x30 | 4 | bulletDirScale | float | "" |
| 0x34 | 4 | fullAutoScale | float | "" |
| 0x38 | 4 | fullAutoBullets | int | "" |
| 0x3C | 4 | fullAutoDecay | float | "" |
| 0x40 | 4 | referenceIndex | int | "" |
| 0x44 | 4 | kickOrSnapDecayIndex | int | "" |

Unmapped/padding bit ranges: 8+24.

### WeaponOffsetPatternScale

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | patternKey | scr_string_t | "" |
| 0x4 | 12 | scale | vec3_t | "" |

### WeaponOffsetPatternScaleInfo

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | numPatternScales | int | "" |
| 0x8 | 8 | patternScales | WeaponOffsetPatternScale * | (16 * varWeaponOffsetPatternScaleInfo->numPatternScales) / 16 |

Unmapped/padding bit ranges: 32+32.

### WeaponSFXPackage

Size `0xB0`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | name | const char * | "" |
| 0x8 | 8 | sounds | WeaponSFXPackageSounds * | 1 |
| 0x10 | 8 | transientFallbackPackage | WeaponSFXPackage * | 1 |
| 0x18 | 8 | detailSoundBankNPC | SndBankTransient * | 1 |
| 0x20 | 8 | detailSoundBankPlayer | SndBankTransient * | 1 |
| 0x28 | 1 | rattleSoundType | unsigned __int8 | "" |
| 0x30 | 8 | szAdsrBaseSetting | const char * | "" |
| 0x38 | 8 | szWeapSndReflectionClass | const char * | "" |
| 0x40 | 8 | szWeapSndReflFrontOverride | const char * | "" |
| 0x48 | 8 | szWeapSndReflDistantOverride | const char * | "" |
| 0x50 | 4 | weapSndFireVolumeShot1 | float | "" |
| 0x54 | 4 | weapSndFireVolumeShot2 | float | "" |
| 0x58 | 4 | weapSndFireVolumeShot3 | float | "" |
| 0x5C | 4 | weapSndProneFireLFEVolume | float | "" |
| 0x60 | 4 | weapSndMediumRangeStart | float | "" |
| 0x64 | 4 | weapSndFarRangeStart | float | "" |
| 0x68 | 4 | weapSndDistantRangeStart | float | "" |
| 0x6C | 4 | projExplosionReflClass | unsigned int | "" |
| 0x70 | 4 | sfxImpactType | int | "" |
| 0x74 | 4 | sfxMeleeImpactType | MeleeImpactType | "" |
| 0x78 | 4 | sfxMaterialType | WeaponMaterialType | "" |
| 0x80 | 8 | szMountEnterExitString | const char * | "" |
| 0x88 | 8 | szMountImpactString | const char * | "" |
| 0x90 | 4 | whizbyType | unsigned int | "" |
| 0x94 | 4 | adsContextType | unsigned int | "" |
| 0x98 | 4 | adsContextValue | unsigned int | "" |
| 0x9C | 4 | speedOfSoundMult | float | "" |
| 0xA0 | 12 | triggerDisconnectSoundData | TriggerDisconnectSoundData | "" |

Unmapped/padding bit ranges: 328+56, 992+32, 1376+32.

### WeaponSFXPackageSounds

Size `0x308`; Replay schema override.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | name | const char * | "" |
| 0x8 | 8 | pickupSound | SndAliasLookup | "" |
| 0x10 | 8 | pickupSoundPlayer | SndAliasLookup | "" |
| 0x18 | 8 | ammoPickupSound | SndAliasLookup | "" |
| 0x20 | 8 | ammoPickupSoundPlayer | SndAliasLookup | "" |
| 0x28 | 8 | projectileSound | SndAliasLookup | "" |
| 0x30 | 8 | pullbackSound | SndAliasLookup | "" |
| 0x38 | 8 | pullbackSoundPlayer | SndAliasLookup | "" |
| 0x40 | 8 | pullbackSoundQuick | SndAliasLookup | "" |
| 0x48 | 8 | pullbackSoundQuickPlayer | SndAliasLookup | "" |
| 0x50 | 8 | preFireSound | SndAliasLookup | "" |
| 0x58 | 8 | preFireSoundPlayer | SndAliasLookup | "" |
| 0x60 | 8 | fireSound | SndAliasLookup | "" |
| 0x68 | 8 | fireSoundPlayer | SndAliasLookup | "" |
| 0x70 | 8 | fireSoundPlayerAkimbo | SndAliasLookup | "" |
| 0x78 | 8 | fireSoundPlayerAkimboRight | SndAliasLookup | "" |
| 0x80 | 8 | fireSoundAtmosphere | SndAliasLookup | "" |
| 0x88 | 8 | fireSoundAtmospherePlayer | SndAliasLookup | "" |
| 0x90 | 8 | fireSoundAtmospherePlayerAkimbo | SndAliasLookup | "" |
| 0x98 | 8 | fireSoundAtmospherePlayerAkimboRight | SndAliasLookup | "" |
| 0xA0 | 8 | fireStartSound | SndAliasLookup | "" |
| 0xA8 | 8 | fireStartSoundPlayer | SndAliasLookup | "" |
| 0xB0 | 8 | fireLoopSound | SndAliasLookup | "" |
| 0xB8 | 8 | fireLoopSoundPlayer | SndAliasLookup | "" |
| 0xC0 | 8 | fireStopSound | SndAliasLookup | "" |
| 0xC8 | 8 | fireStopSoundPlayer | SndAliasLookup | "" |
| 0xD0 | 8 | fireFirstSound | SndAliasLookup | "" |
| 0xD8 | 8 | fireFirstSoundPlayer | SndAliasLookup | "" |
| 0xE0 | 8 | fireFirstSoundPlayerAkimbo | SndAliasLookup | "" |
| 0xE8 | 8 | fireFirstSoundPlayerAkimboRight | SndAliasLookup | "" |
| 0xF0 | 8 | fireHyperBurstSound | SndAliasLookup | "" |
| 0xF8 | 8 | fireHyperBurstSoundPlayer | SndAliasLookup | "" |
| 0x100 | 8 | fireHyperBurstSoundPlayerAkimbo | SndAliasLookup | "" |
| 0x108 | 8 | fireHyperBurstSoundPlayerAkimboRight | SndAliasLookup | "" |
| 0x110 | 8 | fireLastSound | SndAliasLookup | "" |
| 0x118 | 8 | fireLastSoundPlayer | SndAliasLookup | "" |
| 0x120 | 8 | fireLastSoundPlayerAkimbo | SndAliasLookup | "" |
| 0x128 | 8 | fireLastSoundPlayerAkimboRight | SndAliasLookup | "" |
| 0x130 | 8 | regenerateSound | SndAliasLookup | "" |
| 0x138 | 8 | regenerateSoundPlayer | SndAliasLookup | "" |
| 0x140 | 8 | regenerateAddSound | SndAliasLookup | "" |
| 0x148 | 8 | regenerateAddSoundPlayer | SndAliasLookup | "" |
| 0x150 | 8 | emptyFireSound | SndAliasLookup | "" |
| 0x158 | 8 | emptyFireSoundPlayer | SndAliasLookup | "" |
| 0x160 | 8 | emptyRegenFireSound | SndAliasLookup | "" |
| 0x168 | 8 | emptyRegenFireSoundPlayer | SndAliasLookup | "" |
| 0x170 | 8 | rechamberSound | SndAliasLookup | "" |
| 0x178 | 8 | rechamberSoundPlayer | SndAliasLookup | "" |
| 0x180 | 8 | reloadSound | SndAliasLookup | "" |
| 0x188 | 8 | reloadSoundPlayer | SndAliasLookup | "" |
| 0x190 | 8 | reloadEmptySound | SndAliasLookup | "" |
| 0x198 | 8 | reloadEmptySoundPlayer | SndAliasLookup | "" |
| 0x1A0 | 8 | reloadStartSound | SndAliasLookup | "" |
| 0x1A8 | 8 | reloadStartSoundPlayer | SndAliasLookup | "" |
| 0x1B0 | 8 | reloadEndSound | SndAliasLookup | "" |
| 0x1B8 | 8 | reloadEndSoundPlayer | SndAliasLookup | "" |
| 0x1C0 | 8 | reloadEndRechamberSound | SndAliasLookup | "" |
| 0x1C8 | 8 | reloadEndRechamberSoundPlayer | SndAliasLookup | "" |
| 0x1D0 | 8 | detonateSound | SndAliasLookup | "" |
| 0x1D8 | 8 | detonateSoundPlayer | SndAliasLookup | "" |
| 0x1E0 | 8 | nightVisionWearSound | SndAliasLookup | "" |
| 0x1E8 | 8 | nightVisionWearSoundPlayer | SndAliasLookup | "" |
| 0x1F0 | 8 | nightVisionRemoveSound | SndAliasLookup | "" |
| 0x1F8 | 8 | nightVisionRemoveSoundPlayer | SndAliasLookup | "" |
| 0x200 | 8 | altSwitchSound | SndAliasLookup | "" |
| 0x208 | 8 | altSwitchSoundPlayer | SndAliasLookup | "" |
| 0x210 | 8 | raiseSound | SndAliasLookup | "" |
| 0x218 | 8 | raiseSoundPlayer | SndAliasLookup | "" |
| 0x220 | 8 | firstRaiseSound | SndAliasLookup | "" |
| 0x228 | 8 | firstRaiseSoundPlayer | SndAliasLookup | "" |
| 0x230 | 8 | putawaySound | SndAliasLookup | "" |
| 0x238 | 8 | putawaySoundPlayer | SndAliasLookup | "" |
| 0x240 | 8 | enterAdsSound | SndAliasLookup | "" |
| 0x248 | 8 | enterAdsSoundPlayer | SndAliasLookup | "" |
| 0x250 | 8 | leaveAdsSound | SndAliasLookup | "" |
| 0x258 | 8 | leaveAdsSoundPlayer | SndAliasLookup | "" |
| 0x260 | 8 | scanSound | SndAliasLookup | "" |
| 0x268 | 8 | changeVariableZoomSound | SndAliasLookup | "" |
| 0x270 | 8 | bulletRicochetImpact | SndAliasLookup | "" |
| 0x278 | 8 | projExplosionSound | SndAliasLookup | "" |
| 0x280 | 8 | projDudSound | SndAliasLookup | "" |
| 0x288 | 8 | projTimeoutSound | SndAliasLookup | "" |
| 0x290 | 8 | projIgnitionSound | SndAliasLookup | "" |
| 0x298 | 8 | bounceSound | SndAliasLookup | "" |
| 0x2A0 | 8 | bounceSoundHeavy | SndAliasLookup | "" |
| 0x2A8 | 8 | bounceSoundMedium | SndAliasLookup | "" |
| 0x2B0 | 8 | bounceSoundSoft | SndAliasLookup | "" |
| 0x2B8 | 8 | rollingSound | SndAliasLookup | "" |
| 0x2C0 | 8 | turretSpinSnd | SndAliasLookup | "" |
| 0x2C8 | 8 | turretStopSnd | SndAliasLookup | "" |
| 0x2D0 | 8 | triggerDisconnectSound | SndAliasLookup | "" |
| 0x2D8 | 8 | triggerDisconnectSoundPlayerAkimbo | SndAliasLookup | "" |
| 0x2E0 | 8 | triggerDisconnectSoundPlayerAkimboRight | SndAliasLookup | "" |
| 0x2E8 | 8 | dlcSound0 | SndAliasLookup | "" |
| 0x2F0 | 8 | dlcSound0Player | SndAliasLookup | "" |
| 0x2F8 | 8 | dlcSound1 | SndAliasLookup | "" |
| 0x300 | 8 | dlcSound1Player | SndAliasLookup | "" |

### WeaponVFXPackage

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | name | const char * | "" |
| 0x8 | 8 | vfx | WeaponVFXPackageDefs * | 1 |

### WeaponVFXPackageDefs

Size `0xE0`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | viewFlashEffect | FxCombinedDef | "" |
| 0x8 | 8 | worldFlashEffect | FxCombinedDef | "" |
| 0x10 | 8 | viewFlashADSEffect | FxCombinedDef | "" |
| 0x18 | 8 | viewFlashFirstEffect | FxCombinedDef | "" |
| 0x20 | 8 | worldFlashFirstEffect | FxCombinedDef | "" |
| 0x28 | 8 | viewFlashFirstADSEffect | FxCombinedDef | "" |
| 0x30 | 8 | viewFlashOverchargeEffect | FxCombinedDef | "" |
| 0x38 | 8 | viewFlashOverchargeADSEffect | FxCombinedDef | "" |
| 0x40 | 8 | worldFlashOverchargeEffect | FxCombinedDef | "" |
| 0x48 | 8 | viewShellEjectEffect | FxCombinedDef | "" |
| 0x50 | 8 | worldShellEjectEffect | FxCombinedDef | "" |
| 0x58 | 8 | viewLastShotEjectEffect | FxCombinedDef | "" |
| 0x60 | 8 | worldLastShotEjectEffect | FxCombinedDef | "" |
| 0x68 | 8 | viewMagEjectEffect | FxCombinedDef | "" |
| 0x70 | 8 | viewMuzzleSmokeEffect | FxCombinedDef | "" |
| 0x78 | 8 | worldMuzzleSmokeEffect | FxCombinedDef | "" |
| 0x80 | 8 | viewBarrelSmokeEffect | FxCombinedDef | "" |
| 0x88 | 8 | worldBarrelSmokeEffect | FxCombinedDef | "" |
| 0x90 | 8 | viewEjectionPortSmokeEffect | FxCombinedDef | "" |
| 0x98 | 8 | worldEjectionPortSmokeEffect | FxCombinedDef | "" |
| 0xA0 | 8 | viewDlcEffect0 | FxCombinedDef | "" |
| 0xA8 | 8 | worldDlcEffect0 | FxCombinedDef | "" |
| 0xB0 | 8 | viewDlcEffect1 | FxCombinedDef | "" |
| 0xB8 | 8 | worldDlcEffect1 | FxCombinedDef | "" |
| 0xC0 | 8 | viewDlcEffect2 | FxCombinedDef | "" |
| 0xC8 | 8 | worldDlcEffect2 | FxCombinedDef | "" |
| 0xD0 | 8 | viewDlcEffect3 | FxCombinedDef | "" |
| 0xD8 | 8 | worldDlcEffect3 | FxCombinedDef | "" |

### WeaponZoomSettings

Size `0x30`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 4 | adsZoomFov | float | "" |
| 0x4 | 4 | adsZoomFovLerpType | AdsOffsetInterpolationType | "" |
| 0x8 | 4 | adsZoomFovXOffset | float | "" |
| 0xC | 4 | adsReloadZoomFov | float | "" |
| 0x10 | 4 | adsReloadZoomFovXOffset | float | "" |
| 0x14 | 4 | adsNVGZoomFov | float | "" |
| 0x18 | 4 | adsNVGZoomFovXOffset | float | "" |
| 0x1C | 4 | adsNVGReloadZoomFov | float | "" |
| 0x20 | 4 | adsNVGReloadZoomFovXOffset | float | "" |
| 0x24 | 4 | adsZoomInFrac | float | "" |
| 0x28 | 4 | adsZoomOutFrac | float | "" |
| 0x2C | 1 | adsZoomFovXOffsetOverride | bool | "" |
| 0x2D | 1 | adsReloadFovXOffsetOverride | bool | "" |

Unmapped/padding bit ranges: 368+16.

### ZoomSettings

Size `0x70`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 64 | scene | SceneZoomSettings | "" |
| 0x40 | 48 | weapon | WeaponZoomSettings | "" |

### vec2_t

Size `0x8`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 8 | v | float[2] | "" |
| 0x0 | 8 | __s1 | $38C3DEC81229B66F67FB6D350D75FF5A | "" |

### vec3_t

Size `0xC`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 12 | v | float[3] | "" |
| 0x0 | 12 | __s1 | $393C16A032292777F0C3725FFB2C0008 | "" |

### vec4_t

Size `0x10`; shared schema; validate against Replay consumer.

| Offset | Bytes | Field | Type | Pointer count / rule |
|---:|---:|---|---|---|
| 0x0 | 16 | v | float[4] | "" |
| 0x0 | 16 | __s1 | $91D1B2149FAC90180ECB9AC277F76009 | "" |
| 0x0 | 16 | __s2 | $43F634250C0E94E2A09AB0840E4770D1 | "" |
| 0x0 | 12 | xyz | vec3_t | "" |

## Enums

### AdsAltSwitchInterpType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | ADS_ALT_SWITCH_INTERP_TYPE_LINEAR |
| 1 | ADS_ALT_SWITCH_INTERP_TYPE_COSINE |
| 2 | ADS_ALT_SWITCH_INTERP_TYPE_SINE |
| 3 | ADS_ALT_SWITCH_INTERP_TYPE_COUNT |

### AdsOffsetInterpolationType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | ADS_OFFSET_INTERP_TYPE_LINEAR |
| 1 | ADS_OFFSET_INTERP_TYPE_EASEINOUTSINE |
| 2 | ADS_OFFSET_INTERP_TYPE_EASEINOUTSMOOTHERSTEP |
| 3 | ADS_OFFSET_INTERP_TYPE_EASEINCUBIC |
| 4 | ADS_OFFSET_INTERP_TYPE_EASEOUTCUBIC |
| 5 | ADS_OFFSET_INTERP_TYPE_EASEINQUARTIC |
| 6 | ADS_OFFSET_INTERP_TYPE_EASEOUTQUARTIC |
| 7 | ADS_OFFSET_INTERP_TYPE_EASEINEXPO |
| 8 | ADS_OFFSET_INTERP_TYPE_EASEOUTEXPO |
| 9 | ADS_OFFSET_INTERP_TYPE_COUNT |

### AnimDamageType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | ANIM_DAMAGETYPE_NONE |
| 1 | ANIM_DAMAGETYPE_EXPLOSION_LIGHT |
| 2 | ANIM_DAMAGETYPE_EXPLOSION |
| 3 | ANIM_DAMAGETYPE_MELEE |
| 4 | ANIM_DAMAGETYPE_MELEE_DOG |
| 5 | ANIM_DAMAGETYPE_MELEE_ALIEN |
| 6 | ANIM_DAMAGETYPE_ENERGY_BULLET |
| 7 | ANIM_DAMAGETYPE_FIRE_EXPLOSION |
| 8 | ANIM_DAMAGETYPE_BULLET_SM |
| 9 | ANIM_DAMAGETYPE_BULLET_MD |
| 10 | ANIM_DAMAGETYPE_BULLET_LG |
| 11 | ANIM_DAMAGETYPE_FALL |
| 12 | ANIM_DAMAGETYPE_COUNT |

### AttachmentSlot

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | ATT_SLOT_RECEIVER |
| 1 | ATT_SLOT_FRONT_PIECE |
| 2 | ATT_SLOT_BACK_PIECE |
| 3 | ATT_SLOT_MAGAZINE |
| 4 | ATT_SLOT_OVERRIDE |
| 5 | ATT_SLOT_MUZZLE |
| 6 | ATT_SLOT_REAR_GRIP |
| 7 | ATT_SLOT_TRIGGER |
| 8 | ATT_SLOT_EXTRA |
| 9 | ATT_SLOT_SCOPE |
| 10 | ATT_SLOT_UNDERBARREL |
| 11 | ATT_SLOT_MODIFIER |
| 12 | ATT_SLOT_VISUAL |
| 13 | ATT_SLOT_OTHER |
| 14 | ATT_SLOT_COUNT |
| 13 | ATT_SLOT_PRIMARY_COUNT |
| 16 | ATT_SLOT_OTHER_COUNT |

### AttachmentType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | ATTACHMENT_SCOPE |
| 1 | ATTACHMENT_UNDERBARREL |
| 2 | ATTACHMENT_OTHER |
| 3 | ATTACHMENT_COUNT |

### CarryObjectType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | CARRYOBJECTTYPE_GENERIC |
| 1 | CARRYOBJECTTYPE_HUMAN |
| 2 | CARRYOBJECTTYPE_COUNT |

### DeployType

Width: 4 bytes.

| Value | Name |
|---:|---|

### DualWieldType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | DUAL_WIELD_TYPE_DEFAULT |
| 1 | DUAL_WIELD_TYPE_ALT_MODE |
| 2 | DUAL_WIELD_TYPE_COUNT |

### DynamicAttachmentType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | DYNAMIC_ATTACHMENT_TYPE_MUZZLE |
| 1 | DYNAMIC_ATTACHMENT_TYPE_UNDERBARREL |
| 2 | DYNAMIC_ATTACHMENT_TYPE_LASER |
| 3 | DYNAMIC_ATTACHMENT_TYPE_GUARD |
| 4 | DYNAMIC_ATTACHMENT_TYPE_MAGAZINE |
| 5 | DYNAMIC_ATTACHMENT_TYPE_RECEIVER |
| 6 | DYNAMIC_ATTACHMENT_TYPE_SCOPE |
| 7 | DYNAMIC_ATTACHMENT_TYPE_STOCK |
| 8 | DYNAMIC_ATTACHMENT_TYPE_FIRETYPE |
| 9 | DYNAMIC_ATTACHMENT_TYPE_BARREL |
| 10 | DYNAMIC_ATTACHMENT_TYPE_OTHER |
| 11 | DYNAMIC_ATTACHMENT_TYPE_COUNT |

### GrenadeRotationDirection

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | GRD_RANDOM |
| 1 | GRD_CLOCKWISE |
| 2 | GRD_COUNTERCLOCKWISE |
| 3 | GRD_COUNT |

### HitmarkerType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | HITMARKER_TYPE_SMALL |
| 1 | HITMARKER_TYPE_MEDIUM |
| 2 | HITMARKER_TYPE_LARGE |
| 3 | HITMARKER_TYPE_COUNT |

### MeleeAnimType

Width: 4 bytes.

| Value | Name |
|---:|---|

### MeleeImpactType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | MELEE_IMPACT_TYPE_NONE |
| 1 | MELEE_IMPACT_TYPE_GUN_SMALL |
| 2 | MELEE_IMPACT_TYPE_GUN_MEDIUM |
| 3 | MELEE_IMPACT_TYPE_GUN_LARGE |
| 4 | MELEE_IMPACT_TYPE_KNIFE |
| 5 | MELEE_IMPACT_TYPE_SHIELD |
| 6 | MELEE_IMPACT_TYPE_USER1 |
| 7 | MELEE_IMPACT_TYPE_USER2 |
| 8 | MELEE_IMPACT_TYPE_USER3 |
| 9 | MELEE_IMPACT_TYPE_USER4 |
| 10 | MELEE_IMPACT_TYPE_USER5 |
| 11 | MELEE_IMPACT_TYPE_USER6 |
| 12 | MELEE_IMPACT_TYPE_USER7 |
| 13 | MELEE_IMPACT_TYPE_USER8 |
| 14 | MELEE_IMPACT_TYPE_COUNT |

### OffhandClass

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | OFFHAND_CLASS_NONE |
| 1 | OFFHAND_CLASS_FRAG_GRENADE |
| 2 | OFFHAND_CLASS_SMOKE_GRENADE |
| 3 | OFFHAND_CLASS_FLASH_GRENADE |
| 4 | OFFHAND_CLASS_MOLOTOV |
| 5 | OFFHAND_CLASS_THROWINGKNIFE |
| 6 | OFFHAND_CLASS_SHIELD |
| 7 | OFFHAND_CLASS_OTHER |
| 8 | OFFHAND_CLASS_COUNT |

### ParticleGroupID

Width: 4 bytes.

| Value | Name |
|---:|---|

### PenetrateType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | PENETRATE_TYPE_NONE |
| 1 | PENETRATE_TYPE_SMALL |
| 2 | PENETRATE_TYPE_MEDIUM |
| 3 | PENETRATE_TYPE_LARGE |
| 4 | PENETRATE_TYPE_MAXIMUM |
| 4 | PENETRATE_TYPE_TABLE_ENTRY_COUNT |
| 5 | PENETRATE_TYPE_RICOCHET |
| 6 | PENETRATE_TYPE_COUNT |

### ReloadType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | RELOAD_TYPE_NORMAL |
| 1 | RELOAD_TYPE_SEGMENTED |
| 2 | RELOAD_TYPE_MULTIPLE_BULLET |
| 3 | RELOAD_TYPE_MULTIPLE_PERCENTAGE |
| 4 | RELOAD_TYPE_COUNT |

### ReticleType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | RETICLE_TYPE_FOUR_SIDED |
| 1 | RETICLE_TYPE_ONE_PIECE |
| 2 | RETICLE_TYPE_COUNT |

### SuitAnimType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | ANIM_SUIT_HUMAN |
| 1 | ANIM_SUIT_ROBOTIC |
| 2 | ANIM_SUIT_C8 |
| 3 | ANIM_SUIT_REAPER |
| 4 | NUM_ANIM_SUIT_STATE |
| 4 | ANIM_SUIT_INVALID |

### WeapDetonateType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | DETONATE_TYPE_NONE |
| 1 | DETONATE_TYPE_GESTURE |
| 2 | DETONATE_TYPE_VIEWMODEL |
| 3 | DETONATE_TYPE_COUNT |

### WeapOverlayInteface_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPOVERLAYINTERFACE_NONE |
| 1 | WEAPOVERLAYINTERFACE_JAVELIN |
| 2 | WEAPOVERLAYINTERFACE_TURRETSCOPE |
| 3 | WEAPOVERLAYINTERFACE_BINOCULARS |
| 4 | WEAPOVERLAYINTERFACECOUNT |

### WeapStickinessType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPSTICKINESS_NONE |
| 1 | WEAPSTICKINESS_ALL |
| 2 | WEAPSTICKINESS_ALL_ORIENT |
| 3 | WEAPSTICKINESS_GROUND |
| 4 | WEAPSTICKINESS_GROUND_WITH_YAW |
| 5 | WEAPSTICKINESS_GROUND_ORIENT |
| 6 | WEAPSTICKINESS_KNIFE |
| 7 | WEAPSTICKINESS_COUNT |

### WeaponChargeType

Width: 4 bytes.

| Value | Name |
|---:|---|

### WeaponEntityNotifyFlags

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAP_NOTIFY_FLAG_NONE |
| 1 | WEAP_NOFITY_FLAG_CHECK_TEAM |
| 2 | WEAP_NOFITY_FLAG_CHECK_VISIBLE |

### WeaponFlagOverride

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPON_FLAG_OVERRIDE_NO_OVERRIDE |
| 1 | WEAPON_FLAG_OVERRIDE_FALSE |
| 2 | WEAPON_FLAG_OVERRIDE_TRUE |
| 3 | WEAPON_FLAG_OVERRIDE_NUM |

### WeaponMaterialType

Width: 4 bytes.

| Value | Name |
|---:|---|

### WeaponOffsetBlendCurveId

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WOBC_HOLD_FIRE_BLEND_SLOW |
| 1 | WOBC_HOLD_FIRE_BLEND_FAST |
| 2 | WOBC_KICK_BLEND |
| 3 | WOBC_SNAP_DECAY_BLEND |
| 4 | WOBC_ADS_BLEND |
| 5 | WOBC_ALWAYS_ON |
| 6 | NUM_WEAPON_OFFSET_BLEND_CURVES |

### WeaponOffsetBlendInterpolationType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WOBIT_LINEAR |
| 1 | WOBIT_CUBIC_EASE_IN |
| 2 | WOBIT_CUBIC_EASE_OUT |
| 3 | WOBIT_QUARTIC_EASE_IN |
| 4 | WOBIT_QUARTIC_EASE_OUT |
| 5 | WOBIT_EXPONENTIAL_EASE_IN |
| 6 | WOBIT_EXPONENTIAL_EASE_OUT |
| 7 | NUM_WEAPON_OFFSET_BLEND_INTERPOLATION_TYPES |

### WeaponOffsetPatternId

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WOP_KEYFRAME |
| 1 | WOP_NOISY_SINE |
| 2 | WOP_RANDOM_SQUARE |
| 3 | WOP_SINE |
| 4 | NUM_WEAPON_OFFSET_PATTERNS |

### WeaponOffsetTransformType

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WOTT_VIEW_ORIGIN |
| 1 | WOTT_VIEW_ANGLES |
| 2 | WOTT_WEAPON_ORIGIN |
| 3 | WOTT_WEAPON_ANGLES |
| 4 | NUM_WEAPON_OFFSET_TRANSFORM_TYPES |

### WeaponSlot

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPON_SLOT_NONE |
| 1 | WEAPON_SLOT_PRIMARY |
| 2 | WEAPON_SLOT_HEAVY |
| 3 | WEAPON_SLOT_MELEE |
| 4 | WEAPON_SLOT_EXECUTION |
| 5 | WEAPON_SLOT_ACCESSORY |
| 6 | WEAPON_SLOT_NUM |

### activeReticleType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | VEH_ACTIVE_RETICLE_NONE |
| 1 | VEH_ACTIVE_RETICLE_PIP_ON_A_STICK |
| 2 | VEH_ACTIVE_RETICLE_BOUNCING_DIAMOND |
| 3 | VEH_ACTIVE_RETICLE_COUNT |

### altModePlayerAnimType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | ALTMODEPLAYERANIMTYPE_NONE |
| 1 | ALTMODEPLAYERANIMTYPE_LAUNCHER |
| 2 | ALTMODEPLAYERANIMTYPE_SHOTGUN |
| 3 | ALTMODEPLAYERANIMTYPE_FIRESELECTOR |
| 4 | NUM_ALTMODEPLAYERANIMTYPES |

### ammoCounterClipType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | AMMO_COUNTER_CLIP_NONE |
| 1 | AMMO_COUNTER_CLIP_MAGAZINE |
| 2 | AMMO_COUNTER_CLIP_SHORTMAGAZINE |
| 3 | AMMO_COUNTER_CLIP_SHOTGUN |
| 4 | AMMO_COUNTER_CLIP_ROCKET |
| 5 | AMMO_COUNTER_CLIP_BELTFED |
| 6 | AMMO_COUNTER_CLIP_ALTWEAPON |
| 7 | AMMO_COUNTER_CLIP_COUNT |

### guidedMissileType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | MISSILE_GUIDANCE_NONE |
| 1 | MISSILE_GUIDANCE_SIDEWINDER |
| 2 | MISSILE_GUIDANCE_HELLFIRE |
| 3 | MISSILE_GUIDANCE_JAVELIN |
| 4 | MISSILE_GUIDANCE_ROBOTECH |
| 5 | MISSILE_GUIDANCE_SOFTLOCKON |
| 6 | MISSILE_GUIDANCE_COUNT |

### offhandShieldButton_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAP_OFFHAND_SHIELD_BUTTON_DEFAULT |
| 1 | WEAP_OFFHAND_SHIELD_BUTTON_ADS |
| 2 | WEAP_OFFHAND_SHIELD_BUTTON_COUNT |

### offhandShieldMaxSpeed_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAP_OFFHAND_SHIELD_SPEED_WALK |
| 1 | WEAP_OFFHAND_SHIELD_SPEED_RUN |
| 2 | WEAP_OFFHAND_SHIELD_SPEED_SPRINT |
| 3 | WEAP_OFFHAND_SHIELD_SPEED_COUNT |

### scr_string_t

Width: 4 bytes.

| Value | Name |
|---:|---|

### scriptedAnimEvent_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | SCRIPTEDANIMEVENT_POWER_ACTIVE |
| 1 | SCRIPTEDANIMEVENT_POWER_ACTIVE_CP |
| 2 | SCRIPTEDANIMEVENT_COUNT |

### targetAssistBehavior_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | TARGET_ASSISTBEHAVE_DEFAULT |
| 1 | TARGET_ASSISTBEHAVE_DIRECT_DAMAGE |
| 2 | TARGET_ASSISTBEHAVE_TARGET_ONLY |
| 3 | TARGET_ASSISTBEHAVE_BEAM |
| 4 | TARGET_ASSISTBEHAVE_SMART_PELLET |
| 5 | TARGET_ASSISTBEHAVE_COUNT |

### targetAssistType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | TARGET_ASSISTTYPPE_DEFAULT |
| 1 | TARGET_ASSISTTYPPE_CONE |
| 2 | TARGET_ASSISTTYPPE_COUNT |

### tracerStyle_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | TRACERSTYLE_TRACER |
| 1 | TRACERSTYLE_BEAM |
| 2 | TRACERSTYLE_NUM |

### turret3pAnimType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | TURRET_3PANIM_MOUNTED |
| 1 | TURRET_3PANIM_MOUNTED_INPLACE |
| 2 | TURRET_3PANIM_MOUNTED_INPLACE_LEAN |
| 3 | TURRET_3PANIM_COUNT |

### turretFireType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | TURRET_SINGLE_FIRE |
| 1 | TURRET_ALTERNATE_FIRE |
| 2 | TURRET_FIRE_TYPE_COUNT |

### weapClass_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPCLASS_RIFLE |
| 1 | WEAPCLASS_SNIPER |
| 2 | WEAPCLASS_MG |
| 3 | WEAPCLASS_SMG |
| 4 | WEAPCLASS_SPREAD |
| 5 | WEAPCLASS_PISTOL |
| 6 | WEAPCLASS_GRENADE |
| 7 | WEAPCLASS_ROCKETLAUNCHER |
| 8 | WEAPCLASS_TURRET |
| 9 | WEAPCLASS_THROWINGKNIFE |
| 10 | WEAPCLASS_NON_PLAYER |
| 11 | WEAPCLASS_ITEM |
| 12 | WEAPCLASS_BEAM |
| 13 | WEAPCLASS_BALL |
| 14 | WEAPCLASS_NONE |
| 15 | WEAPCLASS_NUM |

### weapFireType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPON_FIRETYPE_FULLAUTO |
| 1 | WEAPON_FIRETYPE_SINGLESHOT |
| 2 | WEAPON_FIRETYPE_BURST |
| 3 | WEAPON_FIRETYPE_DOUBLEBARREL |
| 4 | WEAPON_FIRETYPE_DOUBLEBARREL_DUALTRIGGER |
| 5 | WEAPON_FIRETYPE_BEAM |
| 6 | WEAPON_FIRETYPECOUNT |

### weapInventoryType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPINVENTORY_PRIMARY |
| 1 | WEAPINVENTORY_OFFHAND |
| 2 | WEAPINVENTORY_ITEM |
| 3 | WEAPINVENTORY_ALTMODE |
| 4 | WEAPINVENTORY_EXCLUSIVE |
| 5 | WEAPINVENTORY_SCAVENGER |
| 6 | WEAPINVENTORY_MODEL_ONLY |
| 7 | WEAPINVENTORYCOUNT |

### weapOverlayReticle_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPOVERLAYRETICLE_NONE |
| 1 | WEAPOVERLAYRETICLE_CROSSHAIR |
| 2 | WEAPOVERLAYRETICLE_NUM |

### weapProjExposion_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPPROJEXP_GRENADE |
| 1 | WEAPPROJEXP_ROCKET |
| 2 | WEAPPROJEXP_FLASHBANG |
| 3 | WEAPPROJEXP_MOLOTOV |
| 4 | WEAPPROJEXP_NONE |
| 5 | WEAPPROJEXP_DUD |
| 6 | WEAPPROJEXP_SMOKE |
| 7 | WEAPPROJEXP_HEAVY |
| 8 | WEAPPROJEXP_NUM |

### weapStance_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPSTANCE_STAND |
| 1 | WEAPSTANCE_DUCK |
| 2 | WEAPSTANCE_PRONE |
| 3 | WEAPSTANCE_NUM |

### weapType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPTYPE_NONE |
| 1 | WEAPTYPE_MODEL_ONLY |
| 2 | WEAPTYPE_BULLET |
| 3 | WEAPTYPE_GRENADE |
| 4 | WEAPTYPE_PROJECTILE |
| 5 | WEAPTYPE_RIOTSHIELD |
| 6 | WEAPTYPE_SCRIPT |
| 7 | WEAPTYPE_SHIELD |
| 8 | WEAPTYPE_CHARGE_SHIELD |
| 9 | WEAPTYPE_LOCATION_SELECT |
| 10 | WEAPTYPE_EQUIP_DEPLOY |
| 11 | WEAPTYPE_NUM |

### weaponIconRatioType_t

Width: 4 bytes.

| Value | Name |
|---:|---|
| 0 | WEAPON_ICON_RATIO_1TO1 |
| 1 | WEAPON_ICON_RATIO_2TO1 |
| 2 | WEAPON_ICON_RATIO_4TO1 |
| 3 | WEAPON_ICON_RATIO_COUNT |

## External asset boundaries

| Type | Replay pool | Size |
|---|---:|---:|
| BlendSpace2DDef | 97 (blendspace2d) | 0x40 |
| Gesture | 40 (gesture) | 0x290 |
| GfxImage | 19 (image) | 0xE8 |
| LaserDef | 68 (laser) | 0xA8 |
| Material | 11 (material) | 0x78 |
| ParticleSystemDef | 44 (vfx) | 0x80 |
| ReticleDef | 86 (reticle) | 0x48 |
| RumbleGraph | 76 (rumblegraph) | 0x90 |
| RumbleInfo | 75 (rumble) | 0x38 |
| SndBankTransient | 22 (soundbanktransient) | 0x200 |
| TracerDef | 58 (tracer) | 0x90 |
| XAnimParts | 7 (xanim) | 0xA0 |
| XModel | 9 (xmodel) | 0x2B0 |
