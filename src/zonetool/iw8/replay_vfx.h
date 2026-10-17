#pragma once

#include "iw8_focus_structs.h"
#include "iw8_zone.h"

#include <array>
#include <string>
#include <vector>

namespace iw8::vfx
{
struct Module
{
    struct Curve
    {
        std::vector<iw8_focus::ParticleCurveControlPointDef> points;
    };
    iw8_focus::ParticleModuleDef native{};
    std::vector<iw8_focus::ParticleCurveControlPointDef> spawnCurve;
    std::vector<Curve> curves;
    std::vector<std::string> models;
    std::vector<std::string> materials;
    std::vector<std::string> lightDefs;
    // INIT_RUNNER and TEST_* modules both serialize ParticleSystemDef aliases.
    std::vector<std::string> childEffects;
    std::vector<std::array<std::string, 3>> decalMaterials;

    // Imported stock modules (iw8::stock): `native` is the shipped record and
    // every pointer it carries is one of these children, at its byte offset in
    // the record, in load order: a ParticleCurveDef's control points, or a
    // ParticleLinkedAssetListDef whose entries keep their shipped bytes and
    // name the asset (or, for INIT_SOUND, the inline alias string) each
    // pointer slot holds.
    struct LinkedEntry
    {
        std::array<uint8_t, 0x20> native{};
        std::vector<std::pair<std::size_t, std::pair<IW8_XAssetType, std::string>>> assets;
        std::vector<std::pair<std::size_t, std::string>> strings;
    };
    struct ImportedChild
    {
        std::size_t offset{};
        bool curve{};
        std::vector<iw8_focus::ParticleCurveControlPointDef> points;
        std::vector<LinkedEntry> entries;
    };
    bool imported = false;
    std::vector<ImportedChild> importedChildren;
};

struct State
{
    iw8_focus::ParticleStateDef native{};
    std::array<std::vector<Module>, 3> groups;
};

struct Emitter
{
    iw8_focus::ParticleEmitterDef native{};
    std::vector<State> states;
    std::vector<iw8_focus::ParticleCurveControlPointDef> fadeCurve;
};

struct Effect
{
    std::string name;
    iw8_focus::ParticleSystemDef native{};
    std::vector<Emitter> emitters;
};

// Source graph creation is separate. The writer accepts only selector payloads
// whose Replay 1.20 pointer layout is pinned in iw8_focus_structs.h.
void Register(ZoneWriter &writer, Effect effect);
void RegisterLightDef(ZoneWriter &writer, std::string name);
} // namespace iw8::vfx
