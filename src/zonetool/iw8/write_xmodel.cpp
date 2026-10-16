#include "../../common/fs_util.h"
#include "../../common/log.h"
#include "../dumpsrc/xmodel_dump.h"
#include "iw8_focus_structs.h"
#include "iw8_structs.h"
#include "iw8_zone.h"
#include "physics_asset_serialization.h"
#include "replay_havok.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace iw8
{

namespace
{

namespace cx = convert::xmodel;

// stamp a tagged sentinel (or any 64-bit value) into a struct pointer slot without UB.
template <typename P> inline void stampPtr(P &slot, uint64_t v)
{
    std::memcpy(&slot, &v, sizeof(slot));
}

// Build the fixed iw8_focus::XModel struct (0x2B0) from the converted record. All //PTR fields get
// tagged sentinels; scalar fields get the real values. Returns the struct by value.
iw8_focus::XModel buildStruct(const cx::Iw8XModelRecord &rec)
{
    iw8_focus::XModel xm;
    std::memset(&xm, 0, sizeof(xm));

    // ---- scalar fields ----
    xm.numsurfs = rec.numsurfs;
    xm.numLods = rec.numLods;
    xm.collLod = rec.collLod;
    xm.shadowCutoffLod = rec.shadowCutoffLod;
    xm.numBones = rec.numBones;
    xm.numRootBones = rec.numRootBones;
    xm.numClientBones = rec.numClientBones;
    xm.flags = rec.flags;
    xm.contents = rec.contents;
    xm.scale = (rec.scale != 0.f) ? rec.scale : 1.f;
    xm.radius = rec.radius;
    xm.bounds.midPoint = {rec.boundsMid[0], rec.boundsMid[1], rec.boundsMid[2]};
    xm.bounds.halfSize = {rec.boundsHalf[0], rec.boundsHalf[1], rec.boundsHalf[2]};
    xm.physicsUseCategory = rec.physicsAssetName.empty() ? 0 : 1;
    // CharCollBoundsType_Human (game-test PDB enum): the model compiler's default,
    // carried by 98% of shipped Replay XModels (mp_hideout); read only when the
    // model is used as a character. flags stay as the source sets them: shipped
    // 0x40 (SELF_VISIBILITY) and 0x2000000 (HAS_ANY_STREAMED_IMAGES) describe data
    // converted models do not carry.
    xm.characterCollBoundsType = 1;
    // lgvData (light-grid-volume packing): 0x4008040 on all 1902 shipped mp_hideout
    // XModels; the value is tool-baked (absent from the exe), so every model gets it.
    xm.lgvData = 0x4008040;
    // numAimAssistBones / numClothAssets / mdaoVolumeCount / physicsUseCategory / impactType /
    // mdaoType / edgeLength / physicsUsageCounter /
    // noScalePartBits: 0 (no IW5 source) -> all their //PTR/CNT pairs are null/zero (load-safe).

    // ---- pointer fields -> sentinels ----
    stampPtr(xm.name, PTR_FOLLOWS); // name follows
    stampPtr(xm.scriptableMoverDef, PTR_NULL);
    stampPtr(xm.proceduralBones, PTR_NULL);
    stampPtr(xm.dynamicBones, PTR_NULL);
    stampPtr(xm.aimAssistBones, PTR_NULL);                         // numAimAssistBones=0
    stampPtr(xm.boneNames, rec.numBones ? PTR_FOLLOWS : PTR_NULL); // [numBones] follows
    stampPtr(xm.parentList, rec.skeleton.parentList.empty() ? PTR_NULL : PTR_FOLLOWS);
    stampPtr(xm.quats, rec.skeleton.quats.empty() ? PTR_NULL : PTR_FOLLOWS);
    stampPtr(xm.trans, rec.skeleton.trans.empty() ? PTR_NULL : PTR_FOLLOWS);
    stampPtr(xm.partClassification, rec.numBones ? PTR_FOLLOWS : PTR_NULL);
    stampPtr(xm.baseMat, rec.numBones ? PTR_FOLLOWS : PTR_NULL);
    stampPtr(xm.ikHingeAxis, PTR_NULL);
    stampPtr(xm.reactiveMotionInfo, PTR_NULL);
    stampPtr(xm.materialHandles, rec.numsurfs ? PTR_FOLLOWS : PTR_NULL); // [numsurfs] follows
    stampPtr(xm.boneInfo, rec.numBones ? PTR_FOLLOWS : PTR_NULL);
    stampPtr(xm.himipRadiusInvSq,
             rec.himipRadiusInvSq.empty() ? PTR_NULL : PTR_FOLLOWS);
    stampPtr(xm.physicsAsset, PTR_NULL);
    stampPtr(xm.physicsFXShape, PTR_NULL);
    stampPtr(xm.detailCollision, PTR_NULL);
    stampPtr(xm.clothAssets, PTR_NULL); // numClothAssets=0
    stampPtr(xm.blendShapeInfo, PTR_NULL);
    stampPtr(xm.mdaoVolumes, PTR_NULL); // mdaoVolumeCount=0
    stampPtr(xm.decalVolumesInfo, PTR_NULL);
    // runtime_0x298 stays zero; Replay copies it but does not traverse it during asset loading.

    // ---- lodInfo[6] (inline in the struct) ----
    for (int i = 0; i < 6; ++i)
    {
        iw8_focus::XModelLodInfo &li = xm.lodInfo[i];
        std::memset(&li, 0, sizeof(li));
        li.dist = 1000000.0f;
        if (i < (int)rec.lods.size())
        {
            const cx::Iw8LodInfo &s = rec.lods[i];
            li.dist = s.dist;
            li.numsurfs = s.numsurfs;
            li.surfIndex = s.surfIndex;
            li.flags = s.flags;
            std::memcpy(li.partBits, s.partBits.data(),
                        sizeof(uint32_t) * s.partBits.size()); // 8 u32 = 32B == partBits[32]
        }
        stampPtr(li.modelSurfsStaging, PTR_NULL); // XModelSurfs cross-ref (verify-phase alias)
        stampPtr(li.surfs, PTR_NULL);             // XSurface*    cross-ref (verify-phase alias)
    }
    return xm;
}

// The asset body callback: struct -> TEMP_PRELOAD, trailing name+arrays -> VIRTUAL.
void emitBody(ZoneWriter &zw, const cx::Iw8XModelRecord &rec,
              const std::vector<uint32_t> &boneNames)
{
    iw8_focus::XModel xm = buildStruct(rec);
    if (!rec.physicsAssetName.empty())
        stampPtr(xm.physicsAsset, zw.assetAlias(ASSET_TYPE_PHYSICSASSET, rec.physicsAssetName));
    for (size_t index = 0; index < rec.lods.size() && index < std::size(xm.lodInfo); ++index)
    {
        if (!rec.lods[index].surfsName.empty())
        {
            stampPtr(xm.lodInfo[index].modelSurfsStaging,
                     zw.assetAlias(ASSET_TYPE_XMODELSURFS, rec.lods[index].surfsName));
        }
    }
    static_assert(sizeof(iw8_focus::XModel) == iw8sz::XMODEL, "XModel writer struct must be 0x2B0");

    zw.pushStream(XFILE_BLOCK_TEMP_PRELOAD);
    zw.align(7);               // 8-align the struct
    zw.write(&xm, sizeof(xm)); // exactly 0x2B0 bytes

    zw.pushStream(XFILE_BLOCK_VIRTUAL);
    // name (follows)
    zw.writeStr(rec.name);

    // Replay remaps these zone-local script-string indices during Load_XModel.
    if (rec.numBones)
    {
        zw.align(3);
        zw.write(boneNames.data(), boneNames.size() * sizeof(uint32_t));
    }
    const auto writeArray = [&](const auto &array, uint64_t alignment) {
        if (!array.empty())
        {
            zw.align(alignment);
            zw.write(array.data(), array.size() * sizeof(array[0]));
        }
    };
    // Replay Load_XModel E29820: parent bytes, short4 quats, float3 translations,
    // classification bytes, then 32-byte bind matrices aligned to 16.
    writeArray(rec.skeleton.parentList, 0);
    writeArray(rec.skeleton.quats, 1);
    writeArray(rec.skeleton.trans, 3);
    writeArray(rec.skeleton.partClassification, 0);
    writeArray(rec.skeleton.baseMat, 15);
    // materialHandles[numsurfs] : packed references to the previously emitted Material assets.
    if (rec.numsurfs)
    {
        zw.align(7);
        for (size_t i = 0; i < rec.numsurfs; ++i)
        {
            const uint64_t v = rec.materials[i].empty()
                                   ? PTR_NULL
                                   : zw.assetAlias(ASSET_TYPE_MATERIAL, rec.materials[i]);
            zw.write(&v, sizeof(v));
        }
    }
    // Bone bounds follow all six inline LOD dependency visits.
    writeArray(rec.skeleton.boneInfo, 3);
    // Replay rebuilds its model-to-material streaming table after loading a zone and indexes this
    // array for every material surface before it checks the material handle. Keep the native array
    // present even when the source engine has no high-mip radius metadata (all-zero is valid).
    writeArray(rec.himipRadiusInvSq, 3);
    zw.popStream(); // VIRTUAL -> TEMP_PRELOAD
    zw.popStream(); // TEMP_PRELOAD -> (caller's stream, VIRTUAL)
}

} // namespace

void writePhysicsAsset(ZoneWriter &zw, const havok::PhysicsAsset &asset)
{
    zw.add(ASSET_TYPE_PHYSICSASSET, asset.name, [asset](ZoneWriter &writer) {
        emitPhysicsAssetBody(writer, asset);
    });
}

void writePhysicsAssetReference(ZoneWriter &zw, const std::string &name)
{
    zw.add(ASSET_TYPE_PHYSICSASSET, name, [name](ZoneWriter &writer) {
        emitPhysicsAssetReferenceBody(writer, name);
    });
}

void writeXModel(ZoneWriter &zw, const cx::Iw8XModelRecord &rec)
{
    if (rec.name.empty())
    {
        throw std::runtime_error("writeXModel: empty record name");
    }
    if (rec.numClientBones || rec.numRootBones > rec.numBones ||
        rec.boneNames.size() != rec.numBones || rec.materials.size() != rec.numsurfs ||
        rec.himipRadiusInvSq.size() != rec.numsurfs ||
        rec.numLods > 6 || rec.lods.size() != rec.numLods)
        throw std::runtime_error("writeXModel: inconsistent model counts for " + rec.name);
    if (!std::isfinite(rec.scale) || rec.scale <= 0 || !std::isfinite(rec.radius) ||
        rec.radius < 0 || (rec.collLod != UINT8_MAX && rec.collLod >= rec.numLods) ||
        rec.shadowCutoffLod > 6)
        throw std::runtime_error("writeXModel: invalid model scalars for " + rec.name);
    float previousDistance = 0;
    for (const auto &lod : rec.lods)
    {
        if (!std::isfinite(lod.dist) || lod.dist < previousDistance || !lod.numsurfs ||
            lod.surfsName.empty() || size_t(lod.surfIndex) + lod.numsurfs > rec.numsurfs)
            throw std::runtime_error("writeXModel: invalid LOD for " + rec.name);
        previousDistance = lod.dist;
    }
    const auto &skeleton = rec.skeleton;
    const size_t childBones = rec.numBones - rec.numRootBones;
    if (skeleton.parentList.size() != childBones || skeleton.quats.size() != childBones ||
        skeleton.trans.size() != childBones || skeleton.partClassification.size() != rec.numBones ||
        skeleton.baseMat.size() != rec.numBones || skeleton.boneInfo.size() != rec.numBones)
        throw std::runtime_error("writeXModel: incomplete skeleton for " + rec.name);
    for (size_t index = 0; index < childBones; ++index)
        if (!skeleton.parentList[index] || skeleton.parentList[index] > index + rec.numRootBones)
            throw std::runtime_error("writeXModel: invalid parent bone for " + rec.name);
    const auto finite = [&](const auto &values) {
        for (const float value : values)
            if (!std::isfinite(value))
                throw std::runtime_error("writeXModel: non-finite skeleton for " + rec.name);
    };
    for (const auto &position : skeleton.trans)
        finite(position);
    for (const auto &pose : skeleton.baseMat)
    {
        finite(pose.quat);
        finite(pose.trans);
        if (!std::isfinite(pose.transWeight))
            throw std::runtime_error("writeXModel: invalid bind weight for " + rec.name);
    }
    for (const auto &bone : skeleton.boneInfo)
    {
        finite(bone.midPoint);
        finite(bone.halfSize);
        for (const float extent : bone.halfSize)
            if (extent < 0)
                throw std::runtime_error("writeXModel: negative bone extent for " + rec.name);
        if (!std::isfinite(bone.radiusSquared) || bone.radiusSquared < 0)
            throw std::runtime_error("writeXModel: invalid bone radius for " + rec.name);
    }
    finite(rec.boundsMid);
    finite(rec.boundsHalf);
    for (const float extent : rec.boundsHalf)
        if (extent < 0)
            throw std::runtime_error("writeXModel: negative model extent for " + rec.name);
    std::vector<uint32_t> boneNames;
    boneNames.reserve(rec.boneNames.size());
    for (const auto &name : rec.boneNames)
        boneNames.push_back(zw.internScriptString(name));
    // capture rec by value so the deferred body callback owns its data.
    cx::Iw8XModelRecord r = rec;
    zw.add(ASSET_TYPE_XMODEL, r.name, [r, boneNames](ZoneWriter &w) { emitBody(w, r, boneNames); });
    zt::info("writeXModel: registered '%s' (0x2B0; surfs=%u lods=%u bones=%u)", r.name.c_str(),
             r.numsurfs, r.numLods, r.numBones);
}

bool addXModelFromDump(ZoneWriter &zw, const std::string &dumpDir, const std::string &name)
{
    dumpsrc::XModelDumpFull dump;
    if (!dumpsrc::readXModel(dumpDir, name, dump))
    {
        zt::warn("addXModelFromDump: '%s' not read — skipped (asset)", name.c_str());
        return false;
    }
    cx::Iw8XModelRecord rec;
    if (!cx::toIw8(dump, rec))
    {
        zt::warn("addXModelFromDump: '%s' convert failed — skipped", name.c_str());
        return false;
    }
    writeXModel(zw, rec);
    return true;
}

} // namespace iw8
