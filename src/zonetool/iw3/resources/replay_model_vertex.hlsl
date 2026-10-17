// Replay 1.20 model UGB layout. SV_VertexID carries the draw-surface
// index in its upper 16 bits and the surface-local vertex in its lower bits.
ByteAddressBuffer drawSurfaces : register(t63);
ByteAddressBuffer unifiedGeometry : register(t64);
ByteAddressBuffer vertexPages : register(t65);
ByteAddressBuffer vertexColors : register(t66);
ByteAddressBuffer colorPages : register(t67);

// IW3 falloff layers (god rays, lamp glows), six values per row: falloffParms
// .z and .w, end/begin colour, unused, the near fade (featherParms.x) and the
// eye offset (eyeOffsetParms.x). Row 0 is the stock ramp; the converter writes
// the map's rows and stores each surface's row in metadata bits 10-15.
#ifndef MAP_FALLOFF_TABLE
#define MAP_FALLOFF_TABLE {2.128, -.064, .5, 0, 0, 0}
#define MAP_FALLOFF_ROWS 1
#endif
static const float falloffTable[MAP_FALLOFF_ROWS * 6] = MAP_FALLOFF_TABLE;

#ifdef MAP_ENTITY
// Replay's XModel (DObj) shaders take the object from the per-draw constants:
// cb0[0] holds the surface bounds and each bone owns object-matrix rows from
// cb0[2] (four rows, or eight with the previous frame for the VELRGD prepass).
// Row .w is the integer 1/4096-unit translation. Skinned surfaces read the
// GPU-skinned vertex cache instead of the unified geometry.
#ifdef MAP_ENTITY_VELOCITY
#define MAP_BONE_ROWS 8
#define MAP_VIEW_ROWS 12
#define MAP_VIEW_ORIGIN 11
#else
#define MAP_BONE_ROWS 4
#define MAP_VIEW_ROWS 6
#define MAP_VIEW_ORIGIN 5
#endif
cbuffer EntityObject : register(b0) {
    float4 entityObject[2 + 128 * MAP_BONE_ROWS];
};
ByteAddressBuffer skinnedVertices : register(t68);
#else
#define MAP_VIEW_ROWS 6
#define MAP_VIEW_ORIGIN 5
struct SurfaceBounds {
    float4 midpointAndScale;
    float4 unused;
};
StructuredBuffer<SurfaceBounds> surfaceBounds : register(t75);

struct ModelPlacement {
    int3 origin;
    uint quaternionXY;
    uint quaternionZW;
    float scale;
};
StructuredBuffer<ModelPlacement> modelPlacements : register(t76);
#endif

cbuffer ReplayView : register(b2) {
    float4 replayView[MAP_VIEW_ROWS];
};

struct Output {
    float4 position : SV_POSITION;
    float4 uv : TEXCOORDS0;
    float2 lightmapUV : LMAPCOORDS0;
    float3 normal : NORMAL0;
    float4 tangent : TANGENT0;
    float4 color : COLOR0;
    nointerpolation float2 metadata : MAPDATA0;
    nointerpolation float4 materialParameters : MATERIALPARMS0;
    float3 relativePosition : WORLDPOS0;
#ifdef MAP_ENTITY_VELOCITY
    float4 clipPosition : CLIP_POS0;
    float4 previousClipPosition : PREV_CLIP_POS0;
#endif
    // Static placement index (GfxWorld instance order) for per-instance
    // lighting; 0xffffffff for entities.
    nointerpolation uint instance : INSTANCE0;
#ifdef MAP_ENTITY
    // Camera-relative lighting origin (the surface bounds centre through its
    // bone), read by the entity grid lighting (map_surface_realtime MAP_ENTITY_GRID).
    nointerpolation float3 lightingOrigin : LIGHTORIGIN0;
#endif
};

float4 DecodePlacementQuaternion(uint xy, uint zw) {
    float4 quaternion =
        float4(xy & 0xffff, xy >> 16, zw & 0xffff, zw >> 16) * (2.0 / 65535.0) - 1.0;
    return quaternion;
}

float3 Rotate(float3 value, float4 quaternion) {
    return value + 2.0 * cross(quaternion.xyz,
                               cross(quaternion.xyz, value) + quaternion.w * value);
}

void DecodeTangentFrame(uint packed, out float3 normal, out float3 tangent,
                        out float binormalSign) {
    float3 xyz =
        (float3(packed & 1023, (packed >> 10) & 1023, (packed >> 20) & 511) *
             float3(2.0 / 1023, 2.0 / 1023, 2.0 / 511) -
         1) *
        .7071067811865475;
    float largest = sqrt(1 - min(dot(xyz, xyz), 1));
    uint largestIndex = packed >> 30;
    float4 q = largestIndex == 0   ? float4(largest, xyz)
               : largestIndex == 1 ? float4(xyz.x, largest, xyz.yz)
               : largestIndex == 2 ? float4(xyz.xy, largest, xyz.z)
                                   : float4(xyz, largest);
    normal = float3(2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x),
                    1 - 2 * (q.x * q.x + q.y * q.y));
    tangent = float3(1 - 2 * (q.y * q.y + q.z * q.z),
                     2 * (q.x * q.y + q.w * q.z),
                     2 * (q.x * q.z - q.w * q.y));
    binormalSign = (packed & (1u << 29)) ? -1 : 1;
}

#ifdef MAP_ENTITY
// Camera-relative position of a local point through one bone's matrix rows.
float4 EntityPosition(uint row, float3 local, int3 viewOrigin) {
    precise float3 translation =
        (float3)(asint(float3(entityObject[row].w, entityObject[row + 1].w,
                              entityObject[row + 2].w)) - viewOrigin) / 4096.0;
    return float4(dot(local, entityObject[row].xyz) + translation.x,
                  dot(local, entityObject[row + 1].xyz) + translation.y,
                  dot(local, entityObject[row + 2].xyz) + translation.z,
                  dot(float4(local, 1), entityObject[row + 3]));
}
#endif

// IW3 polygon offset as a pull toward the camera (fraction of the distance).
// IW3 (R_ForceSetPolygonOffset) sets D3D9 DEPTHBIAS level * r_polygonOffsetBias
// (-1) / 65536 and SLOPESCALEDEPTHBIAS level * r_polygonOffsetScale (-1) on a
// [0, 1] depth range with r_znear 4: the constant term brings a decal
// level * d^2 / 262144 units nearer (0.34 units at 300, 3.8 at 1000): along
// the view ray that is a constant depth-buffer offset, exact per pixel. The
// slope term is left to the hardware bias of the _bias techniques: estimated
// per vertex from each vertex's view angle it tilted large decal triangles
// through their wall, so stains showed in patches that moved as the camera
// turned (Greenband, owner test 2026-10-16). Mapper decals sit on or slightly behind their
// wall and relied on it (see the world vertex shader).
float Iw3PolygonOffset(uint level, float3 relative, float3 normal) {
    if (level == 0)
        return 0;
    float viewDistance = max(dot(relative, replayView[3].xyz), 0);
    return level * min(viewDistance * (1.0 / 262144.0), .03);
}

Output main(uint id : SV_VertexID) {
    uint drawSurface = id >> 16;
    // Replay's model shadow pass uses the second draw-surface table in t63.
    // The lit and depth-prepass paths use the table at byte offset zero.
#ifdef MAP_MODEL_SHADOW_PASS
    uint4 surface = drawSurfaces.Load4(0x002c0000 + drawSurface * 16);
#else
    uint4 surface = drawSurfaces.Load4(drawSurface * 16);
#endif
    uint page = vertexPages.Load((surface.z + ((id >> 6) & 1023)) * 4);

    uint attributeVertex = (id & 63) | (page << 6);
    uint3 attributes = unifiedGeometry.Load3(0x02bc0000 + attributeVertex * 12);
    uint positionAddress = ((id * 8) & 511) | (page << 9);
    uint2 packedPosition = unifiedGeometry.Load2(positionAddress);
#ifdef MAP_ENTITY
    if ((surface.y >> 16) & 1) {
        uint skinned = (id & 0xffff) + surface.w;
        packedPosition = skinnedVertices.Load2(skinned * 8);
        attributes.y = skinnedVertices.Load(0x00ca8000 + skinned * 4);
    }
#endif

    uint3 quantized;
    quantized.x = packedPosition.x & 0x1fffff;
    quantized.y = (packedPosition.x >> 21) | ((packedPosition.y & 0x3ff) << 11);
    quantized.z = (packedPosition.y >> 10) & 0x1fffff;
#ifdef MAP_ENTITY
    float4 bounds = entityObject[0];
#else
    float4 bounds = surfaceBounds[surface.y & 0xffff].midpointAndScale;
#endif
    precise float3 local = bounds.xyz +
                           ((float3)quantized * (2.0 / 2097151.0) - 1.0) * bounds.w;

    float3 modelNormal;
    float3 modelTangent;
    float binormalSign;
    DecodeTangentFrame(attributes.y, modelNormal, modelTangent, binormalSign);
#if defined(MAP_FLAG_WAVE) && !defined(MAP_ENTITY)
    // IW3 flag cloth (mc_l_sm_flag_*, disassembled common_mp vs_3_0 flag
    // shaders): with u = 1 - texcoord and t = gameTime.w seconds, the vertex
    // moves along its model normal by d(u, t) and the normal tilts by 0.025 *
    // (dd/du.x * T + dd/du.y * B), B = cross(N, T) * sign. Replay's static-model
    // view constants carry the game time in row 4 .w (stock wind shaders read it).
    {
        float2 u = 1 - float2(f16tof32(attributes.z & 0xffff), f16tof32(attributes.z >> 16));
        float t = replayView[4].w;
        float3 phase = float3(19.577713 * u.x + 5.24583244 * u.y + 7.53982258 * t,
                              3.62305188 * u.x + 0.25334847 * u.y + 5.6548667 * t,
                              8.25030613 * u.x - 1.45475161 * u.y + 9.42477798 * t);
        // IW3 wraps each phase to [-pi, pi) before sincos.
        phase = frac(phase * 0.159154937 + 0.5) * 6.28318548 - 3.14159274;
        float3 s, c;
        sincos(phase, s, c);
        float d = 1.7 * s.x + 7.5 * s.y + 3 * s.z;
        float dx = 33.2821121 * c.x + 27.1728897 * c.y + 24.7509193 * c.z;
        float dy = 8.91791534 * c.x + 1.90011346 * c.y - 4.36425447 * c.z;
        float3 binormal = cross(modelNormal, modelTangent) * binormalSign;
        local += d * modelNormal;
        modelNormal = normalize(modelNormal + 0.025 * (dx * modelTangent + dy * binormal));
    }
#endif
#ifdef MAP_ENTITY
    uint row = 2 + surface.x * MAP_BONE_ROWS;
    precise float4 relative =
        EntityPosition(row, local, asint(replayView[MAP_VIEW_ORIGIN].xyz));
    float3x3 rotation = float3x3(entityObject[row].xyz, entityObject[row + 1].xyz,
                                 entityObject[row + 2].xyz) *
                        rsqrt(dot(float3(entityObject[row].x, entityObject[row + 1].x,
                                         entityObject[row + 2].x),
                                  float3(entityObject[row].x, entityObject[row + 1].x,
                                         entityObject[row + 2].x)));
    float3 normal = mul(rotation, modelNormal);
    float3 tangent = mul(rotation, modelTangent);
#else
    ModelPlacement placement = modelPlacements[surface.x];
    float4 placementQuaternion =
        DecodePlacementQuaternion(placement.quaternionXY, placement.quaternionZW);
    precise float3 translation =
        (float3)(placement.origin - asint(replayView[MAP_VIEW_ORIGIN].xyz)) / 4096.0;
    precise float4 relative =
        float4(Rotate(local * placement.scale, placementQuaternion) + translation, 1);
    float3 normal = Rotate(modelNormal, placementQuaternion);
    float3 tangent = Rotate(modelTangent, placementQuaternion);
#endif

    Output o;
    o.relativePosition = relative.xyz;
    // Bits 0-3 of the attribute word: blend and falloff flags (256-2048);
    // bits 4-9: the IW3 layer rank. A ranked surface (a decal or blended
    // layer) moves toward the camera along its own view ray by rank * 1.2e-5 of
    // its distance: same screen position, IW3's layer order in depth.
    uint extended = attributes.x & 0xfff;
    bool falloff = (extended & 4) != 0;
    uint falloffRow = min((attributes.x >> 10) & 63, MAP_FALLOFF_ROWS - 1) * 6;
    precise float4 projected = relative;
#ifndef MAP_MODEL_SHADOW_PASS
    projected.xyz *= 1 - ((extended >> 4) & 63) * 1.2e-5;
    // IW3 polygon offset (bits 10-11 of a surface without falloff: offset1,
    // offset2), as the world vertex shader.
    projected.xyz *= 1 - Iw3PolygonOffset(falloff ? 0 : (attributes.x >> 10) & 3, projected.xyz, normal);
#endif
    // IW3 eye offset (zfeather_foa_nf_eo): the vertex moves along its view ray
    // until it is eyeOffsetParms.x units nearer in view depth.
    float viewDepth = dot(projected.xyz, replayView[3].xyz);
    float eyeOffset = falloff ? falloffTable[falloffRow + 5] : 0;
    if (eyeOffset > 0 && viewDepth > eyeOffset)
        projected.xyz *= (viewDepth - eyeOffset) / viewDepth;
    o.position.x = dot(projected, replayView[0]);
    o.position.y = dot(projected, replayView[1]);
    o.position.z = dot(projected, replayView[2]);
    o.position.w = dot(projected, replayView[3]);
#ifdef MAP_ENTITY_VELOCITY
    // The previous-frame rows and view origin feed Replay's velocity target.
    float4 previous = EntityPosition(row + 4, local, asint(replayView[9].xyz));
    o.clipPosition = o.position;
    o.previousClipPosition = previous.x * replayView[4] + previous.y * replayView[5] +
                             previous.z * replayView[6] + previous.w * replayView[7];
#endif
    o.uv = float4(f16tof32(attributes.z & 0xffff), f16tof32(attributes.z >> 16), 0, 0);
    o.lightmapUV = 0;
    o.normal = normal;
    o.tangent = float4(tangent, binormalSign);

    // Models sample their own textures: no atlas tile.
    o.metadata = float2(0, ((attributes.x >> 16) & 0xff) | ((extended & 15) << 8) |
                               (((extended >> 4) & 63) << 12));
    o.materialParameters = float4(.8, 4, 2.5, .625);
    // IW3 falloff layers on models: the material's own falloff row.
    if (falloff)
        o.materialParameters =
            float4(falloffTable[falloffRow], falloffTable[falloffRow + 1],
                   falloffTable[falloffRow + 2], falloffTable[falloffRow + 3]);
    uint colorPageStart = drawSurfaces.Load(0x00240000 + drawSurface * 4);
    uint colorByte = (id & 0xffff) * 4;
    uint colorPage = colorPages.Load((colorPageStart + (colorByte >> 10)) * 4);
    uint packedColor = vertexColors.Load((colorPage << 10) | (colorByte & 1023));
    float4 color = float4(packedColor & 255, (packedColor >> 8) & 255,
                         (packedColor >> 16) & 255, packedColor >> 24) / 255.0;
    // Replay decodes authored model RGB from sRGB; alpha remains linear.
    o.color = float4(lerp(color.rgb / 12.92, pow((color.rgb + .055) / 1.055, 2.4),
                          step(.04045, color.rgb)), color.a);
    // IW3 zfeather near fade: alpha *= saturate(view depth * featherParms.x);
    // its additive pixel shader scales the colour by that alpha.
    float nearFade = falloff ? falloffTable[falloffRow + 4] : 0;
    if (nearFade > 0)
        o.color *= saturate(viewDepth * nearFade);
#ifdef MAP_ENTITY
    o.instance = 0xffffffff;
    // IW3 lights a DObj once per entity, from its origin (CG_General: script
    // models at origin + 4 up; dynents at the pose origin): the root bone (row 2,
    // the entity transform). The pixel shader adds the class offset or finds the
    // entity's precomputed IW3 volume by this position.
    o.lightingOrigin =
        EntityPosition(2, float3(0, 0, 0), asint(replayView[MAP_VIEW_ORIGIN].xyz)).xyz;
#else
    o.instance = surface.x;
#endif
    return o;
}
