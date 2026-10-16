// Replay static-world inputs. Metadata and lightmap UVs use separate vertex attributes.
#define MW120R_ATLAS_VERTEX_V2 1
#ifndef MAP_DEBUG_MODE
#define MAP_DEBUG_MODE 0
#endif
#ifndef MAP_INDIRECT_GAIN
#define MAP_INDIRECT_GAIN 1.0
#endif
#ifndef MAP_UNBAKED_GAIN
#define MAP_UNBAKED_GAIN 1.0
#endif
#ifndef MAP_SOURCE_SUN_MASK
#define MAP_SOURCE_SUN_MASK 0
#endif
#ifndef MAP_LIGHTING_ROWS
#define MAP_LIGHTING_ROWS 84
#endif
// The stock lit forward+ shaders declare 187 lighting rows (probe clusters
// cb7[48..52], decal clusters cb7[116..118], probe roughness fallback cb7[186]).
#if MAP_LIGHTING_ROWS < 187
#undef MAP_LIGHTING_ROWS
#define MAP_LIGHTING_ROWS 187
#endif
#ifndef MAP_MAX_ANISO
#define MAP_MAX_ANISO 8
#endif
// Slight sharpening for Replay's temporal anti-aliasing; the same value in
// every pass keeps alpha-tested coverage identical between depth and colour.
#ifndef MAP_COLOR_LOD_BIAS
#define MAP_COLOR_LOD_BIAS -0.5
#endif
struct Input {
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
#ifdef STATIC_MODEL
    nointerpolation uint instance : INSTANCE0;
#endif
};
#ifdef STATIC_MODEL
Texture2D<float4> sourceAtlas : register(t0);
#else
Texture2D<float4> sourceAtlas : register(t1);
#endif
SamplerState colorSampler : register(s3);
Texture2D<float4> sunVisibility : register(t85);
#ifdef STATIC_MODEL
// Replay's static-model forward shader binds the sun tile classifications at
// t4. World surfaces use t5; sampling t5 here reads the wrong screen resource.
StructuredBuffer<uint2> lightingTiles : register(t4);
#else
StructuredBuffer<uint2> lightingTiles : register(t5);
#endif
SamplerState shadowSampler : register(s5);
// TECHNIQUE_LIT_FORWARDPLUS_BITMASK binds code image 0 to the GTAO slot.
// The native Replay static-model pixel shader consumes that image at t95
// with the point sampler s8. Keep this as an engine code image: GTAO is a
// per-frame render target and is not a converted map asset.
Texture2D<float4> gtaoImage : register(t95);
SamplerState gtaoSampler : register(s8);
cbuffer ReplayView : register(b2) {
    float4 replayView[8];
};
cbuffer ReplayLighting : register(b7) {
    float4 replayLighting[MAP_LIGHTING_ROWS];
};

// Blinn-Phong power p as Beckmann roughness^2 = 2 / (p + 2), widened by the
// normal variance and converted back.
float FilteredPower(float power, float variance) {
    return max(2 / (2 / (power + 2) + variance) - 2, 1);
}
float FilteredGloss(float gloss, float variance) {
    return log2(FilteredPower(exp2(13 * gloss), variance)) / 13;
}

#ifndef MAP_SOURCE_CHANNELS
#define MAP_SOURCE_CHANNELS 0
#endif
#if MAP_SOURCE_CHANNELS
#ifdef STATIC_MODEL
Texture2D<float4> sourceNormalAtlas : register(t1);
Texture2D<float4> sourceResponseAtlas : register(t2);
#else
Texture2D<float4> sourceNormalAtlas : register(t2);
Texture2D<float4> sourceResponseAtlas : register(t3);
#endif
float3 SourceData(float3 value) {
    return lerp(value * 12.92, 1.055 * pow(max(value, 0), 1.0 / 2.4) - .055, step(.0031308, value));
}
// Inverse of SourceData (IEC 61966-2-1 decode).
float3 DecodeSrgb(float3 value) {
    return lerp(value / 12.92, pow((value + .055) / 1.055, 2.4), step(.04045, value));
}
#ifdef MAP_DETAIL
// IW3 detailMap, material texture slot 0x3E (models only; worlds use it for lightmaps).
Texture2D<float4> detailMap : register(t10);
#endif
float3
SourceWorldNormal(Input input, uint flags, float2 atlasUV, float lod, out float3 tangentNormal) {
    tangentNormal = float3(0, 0, 1);
    if ((flags & 32) != 0) {
#ifdef STATIC_MODEL
        float4 raw = sourceNormalAtlas.SampleBias(colorSampler, atlasUV, replayView[7].y);
#else
        float4 raw = sourceNormalAtlas.SampleLevel(colorSampler, atlasUV, lod);
#endif
        // The converter stores IW3 slopes as .xy; DXT5 sources (Y in RGB,
        // X in alpha) are swizzled to that layout when they are loaded.
        float2 slope = raw.rg * float2(4.08, 4.06451607) - float2(2.08, 2.06451607);
        tangentNormal = normalize(float3(slope, 1));
    }
    float3 normal = normalize(input.normal);
    float3 tangent = normalize(input.tangent.xyz);
    float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
    return normalize(tangentNormal.x * tangent + tangentNormal.y * bitangent +
                     tangentNormal.z * normal);
}
#ifdef MAP_LIGHTMAP_TEXTURES
// IW3's two lightmap layers, uncompressed over the atlas region the lightmaps
// occupy: primary = A colour + direction X, secondary = B colour + direction Y.
Texture2D<float4> lightmapPrimary : register(t7);
Texture2D<float4> lightmapSecondary : register(t8);
#endif
float3 SourceIndirect(float2 uv, float3 first, float3 tangentNormal) {
#ifdef MAP_LIGHTMAP_TEXTURES
    float2 lightmapUV = (uv * ATLAS_SIZE.0 - MAP_LIGHTMAP_ORIGIN) / MAP_LIGHTMAP_SIZE;
    float4 primary = lightmapPrimary.SampleLevel(colorSampler, lightmapUV, 0);
    float4 secondary = lightmapSecondary.SampleLevel(colorSampler, lightmapUV, 0);
    first = primary.rgb;
    float3 second = secondary.rgb;
    float2 raw = float2(primary.a, secondary.a);
#else
    float4 responseData = sourceResponseAtlas.SampleLevel(colorSampler, uv, 0);
    float4 normalData = sourceNormalAtlas.SampleLevel(colorSampler, uv, 0);
    first = float3(normalData.ba, responseData.a);
    float3 second = responseData.rgb;
    float2 raw = normalData.rg;
#endif
    float2 slope = raw * float2(4.08, 4.06451607) - float2(2.08, 2.06451607);
    float response = saturate(dot(normalize(float3(slope, 1)), tangentNormal));
    return first * tangentNormal.z + second * response;
}
float3 SourceSunSpecular(Input input,
                         uint flags,
                         float2 uv,
                         float lod,
                         float3 normal,
                         float3 sunDirection,
                         float3 sun,
                         float visibility,
                         float variance) {
    if ((flags & 64) == 0 || visibility <= 0)
        return 0;
#ifdef STATIC_MODEL
    float4 raw = sourceResponseAtlas.SampleBias(colorSampler, uv, replayView[7].y);
#else
    float4 raw = sourceResponseAtlas.SampleLevel(colorSampler, uv, lod);
#endif
    float3 view = normalize(-input.relativePosition);
    float3 reflected = reflect(-view, normal);
    float sourceExponent = exp2(raw.a * 9.3775177) + 7;
    // A widened lobe keeps its energy: scale by the normalisation ratio.
    float exponent = FilteredPower(sourceExponent, variance);
    float lobe = saturate(exp((dot(reflected, sunDirection) - .99925) * exponent)) *
                 (exponent + 2) / (sourceExponent + 2);
    float fresnel = pow(saturate(1 - abs(dot(view, normal))), input.materialParameters.z);
    float response = lerp(input.materialParameters.x, input.materialParameters.y, fresnel);
    return raw.rgb * response * input.materialParameters.w * lobe * sun * visibility;
}
#endif

#ifdef MAP_BAKED_SUN
// Baked world-space sun depth (replaysunshadow::BakeDepthImage): row 0 holds
// the projection as raw floats; from row 1 the depths in row-major order, two
// 16-bit occluder depths per texel in a 4096-texel-wide image.
Texture2D<float4> bakedSun : register(t9);
float BakedSunHeader(uint index) {
    uint4 bytes = (uint4)round(bakedSun.Load(int3(index, 0, 0)) * 255);
    return asfloat(bytes.r | (bytes.g << 8) | (bytes.b << 16) | (bytes.a << 24));
}
float BakedSunDepth(int2 texel, uint stride) {
    uint index = (uint)texel.y * stride + (uint)texel.x;
    uint pairIndex = index >> 1;
    float4 pair = bakedSun.Load(int3(pairIndex & 4095, (pairIndex >> 12) + 1, 0));
    float2 packed = (index & 1) != 0 ? pair.ba : pair.rg;
    return dot(round(packed * 255), float2(256, 1)) / 65535;
}
// Sun visibility from every static caster, whatever the camera sees: 3x3
// bilinear PCF over the stored depths (lit where the receiver is at least the
// stored depth, or nothing casts). -1 outside the baked region.
float BakedSunVisibility(float3 world, float3 normal) {
    if (BakedSunHeader(16) != 1)
        return -1;
    float3 right = float3(BakedSunHeader(0), BakedSunHeader(1), BakedSunHeader(2));
    float3 up = float3(BakedSunHeader(3), BakedSunHeader(4), BakedSunHeader(5));
    float3 axis = float3(BakedSunHeader(6), BakedSunHeader(7), BakedSunHeader(8));
    float texel = BakedSunHeader(11);
    float inverseSpan = BakedSunHeader(13);
    float2 size = float2(BakedSunHeader(14), BakedSunHeader(15));
    uint stride = (uint)BakedSunHeader(17);
    // A normal offset of one and a half texels keeps sloped receivers clear
    // of their neighbours' occluder depths.
    float3 p = world + normal * (1.5 * texel);
    float2 position =
        float2(dot(right, p) - BakedSunHeader(9), -dot(up, p) - BakedSunHeader(10)) / texel - .5;
    if (any(position < 2) || any(position >= size - 4))
        return -1;
    float depth = (dot(axis, p) + BakedSunHeader(12)) * inverseSpan - .5 * inverseSpan;
    // Tent filter of radius 2.5 texels over 6x6 stored depths: a low sun
    // stretches each texel along the ground, and a 3x3 kernel left visible
    // steps on shadow edges (Discovery's moon at 21 degrees, owner test
    // 2026-10-15).
    int2 base = (int2)floor(position) - 2;
    float lit = 0, total = 0;
    [unroll] for (int y = 0; y < 6; ++y) {
        float wy = max(0, 2.5 - abs(base.y + y - position.y));
        [unroll] for (int x = 0; x < 6; ++x) {
            float w = wy * max(0, 2.5 - abs(base.x + x - position.x));
            float stored = BakedSunDepth(base + int2(x, y), stride);
            lit += w * (stored == 0 || depth >= stored ? 1 : 0);
            total += w;
        }
    }
    return lit / total;
}
#endif

#ifdef MAP_INSTANCE_LIGHTING
// IW3 lights static models from its light grid at the model's lighting origin.
// The converter stores one ambient cube per placement (+X,-X,+Y,-Y,+Z,-Z, IW3
// grid shading values / 2, 256 placements per row), indexed by the placement.
Texture2D<float4> instanceLighting : register(t6);
float3 InstanceAmbient(uint instance, float3 n) {
    int x = (instance % 256) * 6, y = instance / 256;
    float3 square = n * n;
    float3 ambient =
        square.x * instanceLighting.Load(int3(x + (n.x >= 0 ? 0 : 1), y, 0)).rgb +
        square.y * instanceLighting.Load(int3(x + (n.y >= 0 ? 2 : 3), y, 0)).rgb +
        square.z * instanceLighting.Load(int3(x + (n.z >= 0 ? 4 : 5), y, 0)).rgb;
    return ambient * 2;
}
// IW3's baked sun visibility at the instance (light-grid primary light = sun).
float InstanceSunVisibility(uint instance) {
    return instanceLighting.Load(int3((instance % 256) * 6, instance / 256, 0)).a;
}
// The instance's IW3 reflection probe as a converted slot (-1: none), stored
// as slot + 1 in the alpha of face 1.
int InstanceReflectionProbe(uint instance) {
    return (int)round(instanceLighting.Load(int3((instance % 256) * 6 + 1, instance / 256, 0)).a * 255) - 1;
}
#endif

#ifdef MAP_LIGHT_VOLUME
// Entities (DObjs): the IW3 light grid as a world-space L1 SH volume, packed as
// an atlas of Z-slice tiles (three coefficient groups x slices, one-texel
// borders); see iw3_lightgrid.h LightGridVolume for the layout.
Texture2D<float4> lightVolume : register(t6);
float4 LightVolumeTile(float tile, float2 cell) {
    float2 origin = float2(fmod(tile, MAP_LIGHT_VOLUME_TILES) * (MAP_LIGHT_VOLUME_DIMS.x + 2),
                           floor(tile / MAP_LIGHT_VOLUME_TILES) * (MAP_LIGHT_VOLUME_DIMS.y + 2));
    return lightVolume.SampleLevel(colorSampler, (origin + 1.5 + cell) / MAP_LIGHT_VOLUME_SIZE, 0);
}
float3 LightVolumeAmbient(float3 world, float3 n) {
    float3 c = clamp((world - MAP_LIGHT_VOLUME_ORIGIN) / MAP_LIGHT_VOLUME_CELL, 0,
                     MAP_LIGHT_VOLUME_DIMS - 1);
    float z0 = floor(c.z), z1 = min(z0 + 1, MAP_LIGHT_VOLUME_DIMS.z - 1), fz = c.z - z0;
    float4 g[3];
    [unroll] for (int group = 0; group < 3; ++group)
        g[group] = lerp(LightVolumeTile(group * MAP_LIGHT_VOLUME_DIMS.z + z0, c.xy),
                        LightVolumeTile(group * MAP_LIGHT_VOLUME_DIMS.z + z1, c.xy), fz);
    float3 l0 = g[0].rgb * 8;
    float3 l1x = (float3(g[0].a, g[1].b, g[2].g) - .5) * 8; // basis -0.488603 y
    float3 l1y = (float3(g[1].r, g[1].a, g[2].b) - .5) * 8; // basis 0.488603 z
    float3 l1z = (float3(g[1].g, g[2].r, g[2].a) - .5) * 8; // basis -0.488603 x
    return max(0, l0 * 0.282095 + l1x * (-0.488603 * n.y) + l1y * (0.488603 * n.z) +
                      l1z * (-0.488603 * n.x));
}
#endif

#ifdef MAP_IW8_LIGHTING
// --lighting-style iw8: Replay's clustered reflection probes, read the way the
// stock lit forward+ shaders read them (the engine binds these for every lit
// pass): z-bin probe ranges t47, per-cluster probe bitmasks t46, probe
// parameters t44 (camera-relative projection origin, image index, inverse
// feather, box-projection planes), probe boxes t45 (world centre, axes, half
// size), the octahedral probe array t124 and the split-sum environment BRDF t123.
struct ProbeParameters {
    float v[17];
};
struct ProbeBox {
    float v[15];
};
StructuredBuffer<ProbeParameters> probeParameters : register(t44);
StructuredBuffer<ProbeBox> probeBoxes : register(t45);
StructuredBuffer<uint> probeClusterMasks : register(t46);
StructuredBuffer<uint> probeDepthBins : register(t47);
Texture3D<float4> specularOcclusionLut : register(t121);
Texture2D<float4> environmentBrdf : register(t123);
Texture2DArray<float4> probeImages : register(t124);
SamplerState probeSampler : register(s11);

float2 ProbeOctahedral(float3 d) {
    float sum = abs(d.x) + abs(d.y) + abs(d.z);
    d.xy /= sum == 0 ? 1 : sum;
    if (d.z <= 0)
        d.xy = (1 - abs(d.yx)) * (d.xy >= 0 ? 1 : -1);
    return d.xy * .5 + .5;
}

// Probe radiance around this pixel, weighted by each probe box's feathered
// coverage (weights sum to at most 1) and box-projected; the probes are
// normalised, so the caller scales the sum by the local irradiance luminance.
// Every probe whose box holds the point, weighted by its containment over the
// distance to its box centre and normalised. The stock path walks Replay's
// per-tile cluster lists, whose order follows view-depth bins: where probe
// boxes overlap, the first listed probe won, so ice and snow switched probes
// as the camera turned (Discovery, owner test 2026-10-15). This depends on
// the world position alone. Converted maps hold at most a few dozen probes.
// A surface with an IW3 probe (fixedProbe >= 0, the converted slot) reflects
// that probe alone, as IW3 did: the blend let large outdoor boxes darken the
// Discovery ice cave with night sky until the reflection pointed at the snow
// (owner test 2026-10-16).
float3 ProbeReflection(float2 screenPixel, float viewDepth, float3 relativePosition,
                       float3 reflected, float gloss, int fixedProbe) {
    float mip = floor(5 - 5 * gloss);
    float blend = frac(-5 * gloss);
    float2 border = float2(4u << (uint)clamp(mip, 0, 3), 4u << (uint)clamp(mip + 1, 0, 3));
    float2 scale = 1 - border / 128.0;
    float2 bias = border / 256.0;
    uint count, stride;
    probeBoxes.GetDimensions(count, stride);
    float3 sum = 0;
    float total = 0;
    bool fixedSlot = fixedProbe >= 0 && (uint)fixedProbe < min(count, 256u);
    [loop] for (uint probe = fixedSlot ? (uint)fixedProbe : 0;
                probe < (fixedSlot ? (uint)fixedProbe + 1 : min(count, 256u)); ++probe) {
        ProbeParameters p = probeParameters[probe];
        ProbeBox box = probeBoxes[probe];
        float3 offset = relativePosition - (float3(box.v[0], box.v[1], box.v[2]) - replayView[4].xyz);
        float3 xAxis = float3(box.v[3], box.v[4], box.v[5]);
        float3 yAxis = float3(box.v[6], box.v[7], box.v[8]);
        float3 zAxis = float3(box.v[9], box.v[10], box.v[11]);
        float3 local = float3(dot(offset, xAxis), dot(offset, yAxis), dot(offset, zAxis));
        float3 inside = (float3(box.v[12], box.v[13], box.v[14]) - abs(local)) *
                        float3(p.v[4], p.v[5], p.v[6]);
        float containment = saturate(min(min(inside.z, inside.y), inside.x));
        if (containment <= 0 && !fixedSlot)
            continue;
        float weight = fixedSlot ? 1 : containment / (length(offset) + 64);
        float3 direction = reflected;
        // IW3 samples a surface's own probe as a distant cubemap. Box projection
        // of that probe (whose box need not hold the surface) bent the ray
        // toward "probe centre to point" and broke flat-faceted ice into
        // per-triangle patches that lost the reflection looking up (Discovery,
        // owner test 2026-10-16).
        if (!fixedSlot && asint(p.v[0]) != -1) {
            // Distance along the ray to the box's (expanded) planes.
            float rx = dot(reflected, -xAxis), ox = dot(offset, -xAxis);
            float ry = dot(reflected, -yAxis), oy = dot(offset, -yAxis);
            float rz = dot(reflected, -zAxis), oz = dot(offset, -zAxis);
            float tx = (rx > 0 ? -(p.v[11] + ox) : p.v[12] + ox) / abs(rx);
            float ty = (ry > 0 ? -(p.v[13] + oy) : p.v[14] + oy) / abs(ry);
            float tz = (rz > 0 ? -(p.v[15] + oz) : p.v[16] + oz) / abs(rz);
            float t = min(min(max(tz, 0), max(ty, 0)), max(tx, 0));
            direction = reflected * t + relativePosition - float3(p.v[0], p.v[1], p.v[2]);
        }
        // Converted IW3 probes are axis-aligned (identity probe rotation).
        float2 uv = ProbeOctahedral(direction);
        float slot = asuint(p.v[3]) & 0xffff;
        float3 a = probeImages.SampleLevel(probeSampler, float3(uv * scale.x + bias.x, slot), mip).rgb;
        float3 b = probeImages.SampleLevel(probeSampler, float3(uv * scale.y + bias.y, slot), mip + 1).rgb;
        sum += lerp(a, b, blend) * weight;
        total += weight;
    }
    return total > 0 ? sum / total : 0;
}
#endif

// Replay decal volumes (bullet holes, blast marks, converted IW3 FX decals),
// ported from the stock world lit forward+ pixel shader (2026-10-14): the
// stock shaders blend them into albedo and normal themselves, so surfaces
// drawn by this shader showed none. Clusters: cb7[117].xyz dims, cb7[116].zw
// view-depth slice scale/bias, t57 per-cluster [count:10][first:22] lists of
// t56 indices. A 256-byte volume: rows 0-2 camera-relative position -> box
// (x, y in [0,1), |z| < 1), row 3 xyz projection direction and w flags (512 off,
// 32/64 masks, 128 opacity mode, 4 albedo, 8 normal), row 4 mask atlas rects,
// row 5 fade parameters, rows 6-7 tiling, rows 8-9 tangent frame, row 10 tint,
// row 11 albedo/normal atlas rects (uint16 origin and size / 4096). cb7[116].x
// is the visibility threshold, cb7[118] the angle fade and edge feather.
// The colour, normal and mask atlases are sampled with the material sampler.
struct DecalVolume {
    uint4 rows[16];
};
StructuredBuffer<DecalVolume> decalVolumes : register(t56);
StructuredBuffer<uint> decalLists : register(t57);
Texture2D<float4> decalColorAtlas : register(t116);
Texture2D<float4> decalNormalAtlas : register(t117);
Texture2D<float4> decalMaskAtlas : register(t118);

float2 DecalRectUv(uint2 rect, float2 uv, out float2 size) {
    size = float2(rect >> 16) * (1.0 / 4096);
    return float2(rect & 0xffff) * (1.0 / 4096) + uv * size;
}

void ApplyDecals(float3 relativePosition, float3 vertexNormal, float2 screenUV, float viewDepth,
                 inout float3 albedo, inout float3 shadingNormal) {
    float3 dims = replayLighting[117].xyz;
    if (dims.x < 1 || dims.y < 1 || dims.z < 1)
        return;
    uint2 cell = min((uint2)max(screenUV * dims.xy, 0), (uint2)dims.xy - 1);
    uint slice = (uint)clamp(viewDepth * replayLighting[116].z - replayLighting[116].w, 0, dims.z - 1);
    uint list = decalLists[(slice * (uint)dims.y + cell.y) * (uint)dims.x + cell.x];
    uint count = list & 1023, first = list >> 10;
    float4 position = float4(relativePosition, 1);
    float3 positionDx = ddx(relativePosition), positionDy = ddy(relativePosition);
    for (uint i = 0; i < count && i < 64; ++i) {
        DecalVolume v = decalVolumes[decalLists[first + i]];
        uint flags = v.rows[3].w;
        if (flags & 512)
            continue;
        float4 r0 = asfloat(v.rows[0]), r1 = asfloat(v.rows[1]), r2 = asfloat(v.rows[2]);
        float3 box = float3(dot(r0, position), dot(r1, position), dot(r2, position));
        if (abs(floor(box.x)) + abs(floor(box.y)) + floor(abs(box.z)) != 0)
            continue;
        float3 projection = asfloat(v.rows[3].xyz);
        float facing = saturate(dot(projection, vertexNormal) + replayLighting[118].z);
        if (!(replayLighting[118].x < facing))
            continue;
        float2 boxDx = float2(dot(r0.xyz, positionDx), dot(r1.xyz, positionDx));
        float2 boxDy = float2(dot(r0.xyz, positionDy), dot(r1.xyz, positionDy));
        float2 size;
        float mask = 1;
        if (flags & 32) {
            float2 uv = DecalRectUv(v.rows[4].xy, box.xy, size);
            mask = decalMaskAtlas.SampleGrad(colorSampler, uv, boxDx * size, boxDy * size).r;
        }
        float2 edge = saturate((abs(box.xy * 2 - 1) - replayLighting[118].w) * 256);
        float fade = (1 - max(edge.x, edge.y)) * mask;
        float4 r5 = asfloat(v.rows[5]);
        fade *= saturate(fade * r5.x + r5.y);
        if (!(replayLighting[116].x < fade))
            continue;
        float4 r6 = asfloat(v.rows[6]), r7 = asfloat(v.rows[7]);
        float2 tiled = frac(float2(dot(float3(r6.xy, r7.x), float3(box.xy, 1)),
                                   dot(float3(r6.zw, r7.y), float3(box.xy, 1))));
        float2 tiling = float2(r6.x, r6.w) * replayLighting[116].y;
        float2 tiledDx = tiling * boxDx, tiledDy = tiling * boxDy;
        float mask2 = 1;
        if (flags & 64) {
            float2 uv = DecalRectUv(v.rows[4].zw, tiled, size);
            mask2 = decalMaskAtlas.SampleGrad(colorSampler, uv, tiledDx * size, tiledDy * size).r;
        }
        fade *= mask2 * saturate(abs(box.z) * r5.z - r5.z);
        fade = (flags & 128) ? saturate(fade * r7.z + r5.w * r7.z - r7.z) : r5.w * fade;
        fade *= saturate((facing - replayLighting[118].x) * replayLighting[118].y);
        if (!(replayLighting[116].x < fade))
            continue;
        float3 tint = asfloat(v.rows[10].xyz);
        if (flags & 4) {
            float2 uv = DecalRectUv(v.rows[11].xy, tiled, size);
            float3 color = decalColorAtlas.SampleGrad(colorSampler, uv, tiledDx * size, tiledDy * size).rgb;
            albedo = lerp(albedo, color * tint, fade);
        }
        if (flags & 8) {
            float2 uv = DecalRectUv(v.rows[11].zw, tiled, size);
            float4 packed = decalNormalAtlas.SampleGrad(colorSampler, uv, tiledDx * size, tiledDy * size);
            float scale = (((v.rows[8].w >> 16) & 255) + 1) * (1.0 / 256);
            float2 n = ((packed.yw - .5) * scale + .5) * 2 - 1;
            float3 o = float3((n.x + n.y) * .5, (n.x - n.y) * .5, 0);
            o.z = 1 - abs(o.x) - abs(o.y);
            o = normalize(o);
            float3 world = normalize(asfloat(v.rows[8].xyz) * o.x + projection * o.z +
                                     asfloat(v.rows[9].xyz) * o.y);
            shadingNormal = normalize(lerp(shadingNormal, world, fade));
        }
    }
}

#ifdef MAP_GLASS_SH
// Linear radiance SH9 (RGB) averaged over the map's IW3 reflection captures.
static const float3 glassSh[9] = MAP_GLASS_SH;
float3 GlassEnvironment(float3 d) {
    return max(0, glassSh[0] * 0.282095 + glassSh[1] * (0.488603 * d.y) +
                      glassSh[2] * (0.488603 * d.z) + glassSh[3] * (0.488603 * d.x) +
                      glassSh[4] * (1.092548 * d.x * d.y) + glassSh[5] * (1.092548 * d.y * d.z) +
                      glassSh[6] * (0.315392 * (3 * d.z * d.z - 1)) +
                      glassSh[7] * (1.092548 * d.x * d.z) +
                      glassSh[8] * (0.546274 * (d.x * d.x - d.y * d.y)));
}
#endif

// Replay local lights (converted IW3 primary lights: spots and omnis), ported
// from the stock lit forward+ pixel shader (techsets_mp_rust, 2026-10-15).
// Lists: 8x8 screen tile x view-depth bin, as the probe clusters: t52[depth >> 5]
// = [first:16][last word:11 at bit 5]; t50[tile * cb7[52].x + word] light bits.
// t49 record (96 bytes): +0 colour, +16 spot axis (pointing back toward the
// light), shadow slot in the low byte of +28, +28 flags (type at bits 8-9, 2 = spot) and line axis, +48
// camera-relative origin and 1 / radius, +64 cone scale and bias, apex offset
// and window, +92 the falloff curve range (two halves) into t122.
// Shadows: the screen-space mask t94 when cb7[109].x is negative (bytes for
// the first four shadowed lights in .x, then bits of .y); otherwise the spot
// shadow array t93 through t41's 96-byte records (projection rows +32..+80,
// tile origin +4/+8, size +8, slice +12, ESM +16..+28).
ByteAddressBuffer localLightShadows : register(t41);
ByteAddressBuffer localLights : register(t49);
StructuredBuffer<uint> localLightMasks : register(t50);
StructuredBuffer<uint> localLightBins : register(t52);
Texture2DArray<float> localLightShadowMaps : register(t93);
Texture2D<uint2> localLightShadowMask : register(t94);
Texture1D<float> localLightFalloff : register(t122);

float LocalLightFloat(uint light, uint offset) {
    return asfloat(localLights.Load(light * 96 + offset));
}
float3 LocalLightFloat3(uint light, uint offset) {
    return asfloat(localLights.Load3(light * 96 + offset));
}
float LocalShadowFloat(uint slot, uint offset) {
    return asfloat(localLightShadows.Load(slot * 96 + offset));
}
float4 LocalShadowRow(uint slot, uint offset) {
    return asfloat(localLightShadows.Load4(slot * 96 + offset));
}

// Diffuse irradiance (to multiply by albedo) and specular radiance.
void LocalLighting(float2 screenPixel, float viewDepth, float3 relativePosition, float3 normal,
                   float3 view, float3 f0, float gloss, out float3 diffuse, out float3 specular) {
    diffuse = 0;
    specular = 0;
    uint words = asuint(replayLighting[52].x);
    if (words == 0)
        return;
    uint2 tile = (uint2)screenPixel >> 3;
    uint tileColumns = ((uint)replayLighting[54].x + 7) >> 3;
    uint tileIndex = tile.y * tileColumns + tile.x;
    uint bin = localLightBins[min((uint)max(viewDepth, 0) >> 5, 8191)];
    uint first = bin >> 16;
    uint lastWord = min((bin >> 5) & 0x7ff, words - 1);
    int count = clamp((int)(bin & 0xffff) - (int)first + 1, 0, 32);
    uint mask = count == 32 ? 0xffffffff : ~(0xffffffffu << count);
    // Resources the engine may leave unbound for a generated technique report
    // zero size: fall back to a flat falloff and no shadow then.
    uint falloffWidth, maskWidth, maskHeight, mapWidth, mapHeight, mapSlices;
    localLightFalloff.GetDimensions(falloffWidth);
    localLightShadowMask.GetDimensions(maskWidth, maskHeight);
    localLightShadowMaps.GetDimensions(mapWidth, mapHeight, mapSlices);
    bool screenMask = (asuint(replayLighting[109].x) >> 31) != 0;
    bool shadowsBound = screenMask ? maskWidth > 0 : mapSlices > 0;
    uint2 shadowMask = screenMask && maskWidth > 0 ? localLightShadowMask.Load(int3(screenPixel, 0))
                                                   : uint2(0, 0);
    uint shadowed = 0;
    float power = exp2(13 * min(gloss, .98));
    for (uint word = first >> 5; word <= lastWord && word < 2048; ++word) {
        uint bits = localLightMasks[tileIndex * words + word];
        uint shift = (uint)clamp((int)first - (int)(word << 5), 0, 31);
        bits &= count == 32 ? 0xffffffff : mask << shift;
        while (bits != 0) {
            uint bit = firstbitlow(bits);
            bits ^= 1u << bit;
            uint light = bit + (word << 5);
            float3 origin = LocalLightFloat3(light, 48);
            float inverseRadius = LocalLightFloat(light, 60);
            float3 spotDirection = LocalLightFloat3(light, 16);
            uint flags = localLights.Load(light * 96 + 28);
            bool spot = ((flags >> 8) & 3) == 2;
            uint shadowIndex = flags & 255;
            float4 cone = asfloat(localLights.Load4(light * 96 + 64));
            uint falloffRange = localLights.Load(light * 96 + 92);
            float3 toLight = origin - relativePosition;
            float distance = length(toLight);
            float3 direction = toLight / max(distance, 1e-4);
            float spotFactor = 1;
            float facing = 1;
            float profileCoordinate = .5;
            if (spot) {
                float3 apex = origin + spotDirection * cone.z;
                float c = dot(spotDirection, normalize(apex - relativePosition));
                spotFactor = saturate(c * cone.x + cone.y);
                facing = dot(origin - relativePosition, spotDirection) > 0 ? 1 : 0;
                float t = 1 - min(c * c, 1);
                profileCoordinate = c > 0 ? t * .5 : 1 - t * .5;
            }
            float profile = falloffWidth == 0 ? 1 : localLightFalloff.SampleLevel(
                gtaoSampler,
                f16tof32(falloffRange & 0xffff) + profileCoordinate * f16tof32(falloffRange >> 16), 0);
            float window = saturate(cone.w * (1 - distance * inverseRadius)) * spotFactor;
            float attenuation = profile * facing * window * window * (3 - 2 * window);
            if (attenuation <= 0)
                continue;
            float shadow = 1;
            if (shadowIndex != 255 && shadowsBound) {
                if (screenMask) {
                    shadow = shadowed < 4 ? ((shadowMask.x >> (shadowed * 8)) & 255) / 255.0
                                          : ((shadowMask.y >> (shadowed - 4)) & 1);
                    ++shadowed;
                } else {
                    float4 position = float4(relativePosition, 1);
                    float4 projected = float4(dot(position, LocalShadowRow(shadowIndex, 32)),
                                              dot(position, LocalShadowRow(shadowIndex, 48)),
                                              dot(position, LocalShadowRow(shadowIndex, 64)),
                                              dot(position, LocalShadowRow(shadowIndex, 80)));
                    float tileScale = frac(LocalShadowFloat(shadowIndex, 12));
                    float halfTexel = .5 / max(floor(LocalShadowFloat(shadowIndex, 8)), 1);
                    float2 uv = projected.xy / projected.w * tileScale;
                    uv = clamp(uv, halfTexel, tileScale - halfTexel) +
                         float2(LocalShadowFloat(shadowIndex, 4),
                                frac(LocalShadowFloat(shadowIndex, 8)));
                    float stored = localLightShadowMaps.SampleLevel(
                        gtaoSampler, float3(uv, (float)(uint)LocalShadowFloat(shadowIndex, 12)), 0);
                    float exponent = (projected.z * LocalShadowFloat(shadowIndex, 28) - stored) *
                                     LocalShadowFloat(shadowIndex, 16);
                    shadow = saturate(exp2(exponent * 1.442695) * LocalShadowFloat(shadowIndex, 20) +
                                      LocalShadowFloat(shadowIndex, 24));
                }
                shadow = lerp(1, shadow, spot ? 1 : spotFactor);
            }
            float ndotl = saturate(dot(normal, direction));
            float3 radiance = LocalLightFloat3(light, 0) * attenuation * shadow;
            diffuse += radiance * ndotl;
            float3 halfway = normalize(view + direction);
            float distribution = (power + 2) * .5 * pow(saturate(dot(normal, halfway)), power);
            float3 fresnel = f0 + (1 - f0) * pow(1 - saturate(dot(view, halfway)), 5);
            specular += fresnel * distribution * .25 * radiance * ndotl;
        }
    }
}

float4 main(Input input) : SV_TARGET0 {
    // World surfaces: atlas tile in bits 0-15, IW3 reflection probe slot + 1
    // above (0: none).
    uint tileWord = (uint)input.metadata.x;
    uint tile = tileWord & 0xffff;
    int fixedProbe = (int)(tileWord >> 16) - 1;
    uint flags = (uint)input.metadata.y;
    uint kind = flags % 4;
    bool sky = kind == 1;
    float2 mappedUv = input.uv.xy;
    float2 local = sky ? saturate(mappedUv) : frac(mappedUv);
#ifdef STATIC_MODEL
    // Replay's shipped static-model shaders use the material sampler, screen
    // gradients and the view mip bias. Preserve the unwrapped model UV so the
    // sampler can calculate gradients across repeating texture coordinates.
    // Coverage passes bind no view mip bias and use the gradients alone.
#ifndef MAP_MIP_BIAS
#define MAP_MIP_BIAS replayView[7].y
#endif
    float lod = 0;
    float2 sampleUV = input.uv.xy;
    float4 texel = sourceAtlas.SampleBias(colorSampler, sampleUV, MAP_MIP_BIAS);
#ifdef MAP_DETAIL
    // IW3 detail materials (lp_*r0c0d0*): colour + detail - 0.5 in IW3's gamma
    // space before the vertex colour, the detail map tiled by detailScale.xy
    // (disassembled ps_lp_r0c0d0n0_sm3). The detail image is stored UNORM, so
    // it reads IW3's raw bytes; the colour map is sRGB and is re-encoded here.
    float3 detail = detailMap.SampleBias(colorSampler, sampleUV * MAP_DETAIL_SCALE, MAP_MIP_BIAS).rgb;
    float3 gammaColor = SourceData(texel.rgb) + detail - .5;
    texel.rgb = DecodeSrgb(max(gammaColor, 0));
#endif
#else
    float2 cellSize = ATLAS_SIZE.0 / ATLAS_COLUMNS;
    float2 tileOrigin = float2(tile % ATLAS_COLUMNS, tile / ATLAS_COLUMNS) * cellSize;
    float maxLod = log2(cellSize.x) - 2;
    // Derivatives must use the continuous UV, before frac introduces tile seams.
    float2 uvDx = ddx(mappedUv);
    float2 uvDy = ddy(mappedUv);
    float lengthX = length(uvDx * cellSize);
    float lengthY = length(uvDy * cellSize);
    float footprint = max(lengthX, lengthY);
    // Isotropic level for the normal and response channels.
    float lod = sky ? 0 : clamp(log2(max(1.0, footprint)), 0, maxLod);
    float border = .5 * exp2(ceil(lod));
    float2 pixel = tileOrigin + clamp(.5 + local * (cellSize - 1), border, cellSize - border);
    float2 sampleUV = pixel / ATLAS_SIZE.0;
    // Colour uses anisotropic filtering, as IW3 did: the minor axis of the
    // screen footprint picks the mip and taps spread along the major axis.
    // Hardware anisotropy would read neighbouring atlas tiles, so every tap
    // wraps and clamps inside this material's tile. Ground seen at a grazing
    // angle otherwise drops to a mip that is several levels too blurry.
    float ratio = sky ? 1 : clamp(footprint / max(min(lengthX, lengthY), 1e-6), 1, MAP_MAX_ANISO);
    float colorLod = sky ? 0 : clamp(log2(max(1.0, footprint / ratio)) + MAP_COLOR_LOD_BIAS,
                                     0, maxLod);
    float colorBorder = .5 * exp2(ceil(colorLod));
    uint taps = (uint)ceil(ratio - .01);
    float2 axis = lengthX >= lengthY ? uvDx : uvDy;
    float4 texel = 0;
    for (uint tap = 0; tap < taps; ++tap)
    {
        float2 tapUv = mappedUv + axis * ((tap + .5) / taps - .5);
        float2 tapLocal = sky ? saturate(tapUv) : frac(tapUv);
        float2 tapPixel = tileOrigin +
                          clamp(.5 + tapLocal * (cellSize - 1), colorBorder, cellSize - colorBorder);
        texel += sourceAtlas.SampleLevel(colorSampler, tapPixel / ATLAS_SIZE.0, colorLod);
    }
    texel /= taps;
#endif
    texel *= input.color;
    uint alphaTest = (flags >> 3) & 3;
    if (kind == 3 && alphaTest == 1)
        texel.a = texel.a > 0 ? 1 : 0;
    if (kind == 3 && alphaTest == 2)
        texel.a = texel.a < .5 ? 1 : 0;
    // The generated depth prepass uses this same sampling code and cutoff.
    if (kind == 3)
        clip(texel.a - .5);
    float3 tangentNormal = float3(0, 0, 1);
    float3 shadingNormal = normalize(input.normal);
#if MAP_SOURCE_CHANNELS
    shadingNormal = SourceWorldNormal(input, flags, sampleUV, lod, tangentNormal);
#endif
    // Geometric specular antialiasing (Tokuyoshi and Kaplanyan 2019): the
    // shading normal's variance over the pixel widens the highlight lobe.
    // Without it, distant normal-mapped and thin glossy surfaces (fence poles,
    // building trim) flickered as sub-pixel highlights came and went (owner
    // tests 2026-10-15; the flicker stopped up close).
    float3 normalDx = ddx(shadingNormal), normalDy = ddy(shadingNormal);
    float specularVariance =
        min(.5 * (dot(normalDx, normalDx) + dot(normalDy, normalDy)), .18);
    // The stock forward pass uses these same scaled screen coordinates, sun
    // direction, irradiance and sun visibility texture.
    float2 screenPixel = input.position.xy * replayView[7].x;
    float2 screenUV = screenPixel * replayView[6].zw;
    // IW3 additive (256) and multiply (512) layers draw their colour map as is.
    bool layer = (flags & 768) != 0;
    // Blended non-glass layers (2048) are lit like the surface under them.
    bool decalLayer = kind == 2 && (flags & 2048) != 0;
    bool surfaceLit = kind == 0 || kind == 3 || decalLayer;
    if (!sky && !layer && (kind == 0 || kind == 3))
        ApplyDecals(input.relativePosition, normalize(input.normal), screenUV, input.position.w,
                    texel.rgb, shadingNormal);
    float3 direction = replayLighting[56].xyz;
    float3 sun = max(0, replayLighting[57].xyz);
    bool nativeSun = replayLighting[56].w != 0;
    float ndotl = nativeSun ? saturate(dot(shadingNormal, direction)) : 0;
    float visibility = 1;
    // Glass has no opaque depth coverage. Solid and masked surfaces both
    // contribute matching depth and normals before the shadow resolve.
    if (!sky && surfaceLit) {
        if (nativeSun) {
            // Replay's forward shader skips t85 for fully shadowed 8x8 tiles.
            // The resolve can leave those texels unwritten; sampling them
            // directly exposes stale data as camera-dependent blocks of light.
            uint2 tilePosition = (uint2)screenPixel >> 3;
            uint tileColumns = ((uint)replayLighting[54].x + 7) >> 3;
            uint tileClass = lightingTiles[tilePosition.y * tileColumns + tilePosition.x].y >> 28;
            // Class 11 marks an 8x8 screen tile fully shadowed (static or moving
            // casters) and leaves its t85 texels unwritten; the stock shader
            // takes it as shadow. Taking it as lit (zt-42..45) punched the
            // insides out of player and car shadows (owner test 2026-10-15).
            if (replayLighting[83].w > 0 && tileClass == 11)
                visibility = 0;
            else
                visibility = saturate(abs(sunVisibility.SampleLevel(shadowSampler, screenUV, 0).x));
        }
    }
    // Every static caster's shadow comes from the baked sun depth, at any
    // distance and whatever the camera sees; the native result (which drops
    // casters outside its cascades and view, owner tests 2026-10-15) adds the
    // shadows of moving casters: players, script models and dynamic props.
    float bakedVisibility = -1;
#ifdef MAP_BAKED_SUN
    if (!sky && surfaceLit && nativeSun && ndotl > 0)
        bakedVisibility =
            BakedSunVisibility(input.relativePosition + replayView[4].xyz, normalize(input.normal));
    if (bakedVisibility >= 0)
        visibility = min(visibility, bakedVisibility);
#endif
    // Soft hemispherical sky fill for props without a baked lightmap. Preserve
    // authored lightmap occlusion on architecture instead of a global exposure lift.
    float hemi = saturate(normalize(input.normal).z * .5 + .5);
    float3 indirect = lerp(float3(.22, .23, .24), float3(.38, .41, .44), hemi) * MAP_UNBAKED_GAIN;
#if defined(MAP_IW8_LIGHTING) && defined(MAP_GLASS_SH)
    // iw8: direction and colour of the unbaked fill from the map's averaged
    // reflection-probe SH, convolved to irradiance (bands x 1, 2/3, 1/4),
    // at the fill's average brightness (.31) so the exposure stays put.
    {
        float3 n = shadingNormal;
        float3 irradiance = max(0, glassSh[0] * 0.282095 +
                                       (glassSh[1] * n.y + glassSh[2] * n.z + glassSh[3] * n.x) *
                                           (0.488603 * 2.0 / 3.0) +
                                       (glassSh[4] * (1.092548 * n.x * n.y) +
                                        glassSh[5] * (1.092548 * n.y * n.z) +
                                        glassSh[6] * (0.315392 * (3 * n.z * n.z - 1)) +
                                        glassSh[7] * (1.092548 * n.x * n.z) +
                                        glassSh[8] * (0.546274 * (n.x * n.x - n.y * n.y))) *
                                           0.25);
        float average = dot(glassSh[0] * 0.282095, float3(.2126, .7152, .0722));
        if (average > 1e-4)
            indirect = irradiance / average * .31 * MAP_UNBAKED_GAIN;
    }
#endif
#ifdef MAP_INSTANCE_LIGHTING
    // Static placements: IW3's own light-grid lighting (lightmap units).
    if (input.instance != 0xffffffff) {
        indirect = InstanceAmbient(input.instance, shadingNormal) * MAP_INDIRECT_GAIN;
#ifdef MAP_IW8_LIGHTING
        // --style iw8: outside the baked sun depth, IW3's light-grid sun
        // visibility for the model keeps models IW3 kept out of the sun
        // (indoors, under cover) out of it.
        if (bakedVisibility < 0)
            visibility = min(visibility, InstanceSunVisibility(input.instance));
#endif
    }
#endif
#ifdef MAP_LIGHT_VOLUME
    // Entities: the light grid at this point (camera position cb2[4], as the
    // stock lit shaders read it).
    if (input.instance == 0xffffffff)
        indirect = LightVolumeAmbient(input.relativePosition + replayView[4].xyz, shadingNormal) *
                   MAP_INDIRECT_GAIN;
#endif
    if ((flags & 4) != 0) {
        float4 lm = sourceAtlas.SampleLevel(colorSampler, saturate(input.lightmapUV), 0);
        // Both styles keep IW3's baked sun visibility: native shadows alone
        // let sun leak through roofs and ceilings whose outer faces IW3 never
        // drew (caulk), lighting interiors and floors (owner test 2026-10-15).
#if MAP_SOURCE_SUN_MASK
        // IW3 lightmap alpha is the visibility of the surface's own primary
        // light: the sun for most surfaces, a spotlight's mask for those IW3
        // assigned to one (Discovery's cave mouth). Read as sun visibility it
        // took the moonlight, and so the player's shadow, off that ground
        // (owner test 2026-10-15). The baked sun depth decides wherever it
        // covers the point; the mask remains the fallback outside it.
        if (bakedVisibility < 0)
            visibility = min(visibility, lm.a);
#endif
        // CoD4 lightmap coefficients are linear bytes, but the shared color
        // atlas is an sRGB image. Undo its hardware transfer for this data tile
        // only; otherwise indirect lighting is incorrectly crushed toward black.
        float3 baked = lerp(lm.rgb * 12.92, 1.055 * pow(max(lm.rgb, 0), 1.0 / 2.4) - .055,
                            step(.0031308, lm.rgb));
#if MAP_SOURCE_CHANNELS
        indirect =
            SourceIndirect(saturate(input.lightmapUV), baked, tangentNormal) * MAP_INDIRECT_GAIN;
#else
            indirect = baked * 2 * MAP_INDIRECT_GAIN;
#endif
    }
    // The shipped world forward shader scales baked irradiance by cb7[43].z and
    // occludes only that indirect term with GTAO, using the multi-bounce form
    // ao / (1 - min(albedo, .85) * (1 - ao)). Direct sun is never occluded by AO.
    float gtaoSample = saturate(gtaoImage.SampleLevel(gtaoSampler, screenUV, 0).x *
                                replayLighting[47].y);
    float gtao = lerp(1.0, gtaoSample, replayLighting[47].x);
    float3 bounce = 1 - min(texel.rgb, .85) * (1 - gtao);
#ifdef MAP_IW8_LIGHTING
    // Stock environment specular: probe radiance scaled by the local baked
    // irradiance luminance (rough surfaces fall back to that irradiance,
    // cb7[186]), split-sum Fresnel from the environment BRDF (N.V, gloss),
    // scaled by cb7[43].w; indirect diffuse keeps 1 - max(F). IW3 specular
    // maps give F0 (sRGB reflectance) and gloss; other surfaces are rough
    // dielectrics.
    float3 environmentSpecular = 0;
    // Rough dielectric unless an IW3 specular map gives F0 and gloss (owner
    // test: .25 left untextured ground reflective at grazing angles).
    float3 f0 = .04;
    float gloss = .15;
    // Grazing reflectance limit (f90 = saturate(16.5 (r + g + b)) of F0): with
    // Schlick to 1 Nuketown's near-black grass specular (21/255) read wet toward
    // the sun (owner test 2026-10-14).
    float f90 = 1;
#if MAP_SOURCE_CHANNELS
    if ((flags & 64) != 0) {
#ifdef STATIC_MODEL
        float4 response = sourceResponseAtlas.SampleBias(colorSampler, sampleUV, replayView[7].y);
#else
        float4 response = sourceResponseAtlas.SampleLevel(colorSampler, sampleUV, lod);
#endif
        // IW3 paints its specular maps far brighter than physical reflectance
        // (Nuketown wall paint 158/255 = .34 linear, garage door .23) and tamed
        // them with envMapParms and dim LDR probe captures. Read as F0 they made
        // painted walls and ceilings mirror-like indoors (owner test 2026-10-14,
        // zt-33); a quarter keeps ordinary surfaces near .04-.09 and bright
        // (metal) specular visibly reflective.
        f0 = .25 * lerp(response.rgb / 12.92, pow(max((response.rgb + .055) / 1.055, 0), 2.4),
                        step(.04045, response.rgb));
        // IW3 stores gloss as the exponent 2^(9.3775 a) + 7; Replay's is
        // 2^(13 gloss). Read directly, asphalt (a .68) drew a ~5x tighter, hotter
        // highlight than IW3's.
        gloss = log2(exp2(response.a * 9.3775177) + 7) / 13;
        f90 = saturate(dot(f0, 16.5));
    }
#endif
    gloss = FilteredGloss(gloss, specularVariance);
    if (!sky && (flags & 128) == 0 && surfaceLit) {
        float3 view = normalize(-input.relativePosition);
        float ndotv = saturate(dot(shadingNormal, view));
        float3 reflected = reflect(-view, shadingNormal);
        float gate = saturate(gloss * replayLighting[186].x + replayLighting[186].y);
        float3 environment = indirect;
#ifdef MAP_DIAG_NO_PROBES
        // Diagnostic build (IW8_ZONETOOL_DIAG_NO_PROBES): no probe images.
        gate = 1;
#endif
        if (gate < 1) {
            float luminance = dot(indirect, float3(.2126, .7152, .0722));
#if defined(STATIC_MODEL) && defined(MAP_INSTANCE_LIGHTING)
            if (input.instance != 0xffffffff)
                fixedProbe = InstanceReflectionProbe(input.instance);
#endif
            float3 probes = ProbeReflection(screenPixel, input.position.w, input.relativePosition,
                                            reflected, gloss, fixedProbe) * luminance;
            environment = lerp(probes, indirect, gate);
        }
        // Stock specular occlusion: the 3D LUT t121 at (sqrt(occlusion),
        // 2 / (e^(20 gloss) + 2), N.R) with an 18% floor of the blended
        // irradiance. Converted surfaces carry no baked self-visibility (the
        // stock SELFVIS bent normal), so GTAO is the visibility and the
        // shading normal the bent normal.
        float3 lutCoordinate = float3(sqrt(saturate(1 - gtao)), 2 / (exp(20 * gloss) + 2),
                                      saturate(dot(shadingNormal, reflected)));
        float occlusion = specularOcclusionLut.SampleLevel(gtaoSampler, lutCoordinate, 0).r;
        float3 floorLight = lerp(environment, indirect, .85) * .18;
        environment = floorLight + occlusion * (environment - floorLight);
        float2 brdf = environmentBrdf.SampleLevel(gtaoSampler, float2(ndotv, gloss) * .984375 + .0078125, 0).xy;
        float3 fresnel = f0 * brdf.x + f90 * brdf.y;
        environmentSpecular = fresnel * environment * replayLighting[43].w;
        indirect *= 1 - max(fresnel.r, max(fresnel.g, fresnel.b));
    }
#endif
    indirect *= replayLighting[43].z * gtao / max(bounce, 1e-3);
    float3 light = indirect + visibility * ndotl * sun;
    // Sky and IW3 unlit materials (flag 128) show their colour unlit.
    bool unlit = sky || layer || (flags & 128) != 0;
    if (unlit)
        light = 1;
    // Build-time diagnostics keep depth coverage and native postprocessing.
#if MAP_DEBUG_MODE == 1
    light = 1;
#elif MAP_DEBUG_MODE == 2
    light = indirect;
#elif MAP_DEBUG_MODE == 3
    texel.rgb = visibility;
    light = 1;
#elif MAP_DEBUG_MODE == 4
    texel.rgb = shadingNormal * .5 + .5;
    light = 1;
#endif
    float3 specular = 0;
    float3 color = texel.rgb * light;
#if MAP_DEBUG_MODE == 0
    // Converted IW3 primary lights (spots, omnis) through Replay's local
    // light lists and shadows: areas IW3 lit only with them were black.
    if (!sky && !unlit && surfaceLit) {
        float3 localDiffuse, localSpecular;
#ifdef MAP_IW8_LIGHTING
        LocalLighting(screenPixel, input.position.w, input.relativePosition, shadingNormal,
                      normalize(-input.relativePosition), f0, gloss, localDiffuse, localSpecular);
#else
        LocalLighting(screenPixel, input.position.w, input.relativePosition, shadingNormal,
                      normalize(-input.relativePosition), .04, .3, localDiffuse, localSpecular);
#endif
        color += texel.rgb * localDiffuse + localSpecular;
    }
#endif
#if defined(MAP_IW8_LIGHTING) && defined(MAP_SUN_SPRITE) && MAP_DEBUG_MODE == 0
    // --style iw8: the map's IW3 sun sprite (sun/<map>.sun r_sun_fx_position and
    // r_sunsprite_size) as a disc in the sun's colour, and its flare
    // (r_sunflare_max_alpha) as a halo of a few degrees. The wide glow of
    // zt-34..38 brightened a quarter of every sky (owner test 2026-10-15).
    if (sky) {
        float angle = acos(saturate(dot(normalize(input.relativePosition), MAP_SUN_SPRITE.xyz)));
        float radius = MAP_SUN_SPRITE.w;
        float disc = 1 - smoothstep(radius * .8, radius * 1.2, angle);
        float flare = MAP_SUN_FLARE * exp(-angle * angle / (2 * .07 * .07));
        float3 sunColor = nativeSun ? sun : 1;
        color += sunColor * (disc * 12 + flare);
    }
#endif
#if defined(MAP_IW8_LIGHTING) && MAP_DEBUG_MODE == 0
    // --style iw8: direct sun specular in Call of Duty's physically based form
    // (normalised Blinn-Phong, power 2^(13 gloss), Schlick Fresnel and the
    // matching visibility term), from the environment specular's F0 and gloss.
    if (!unlit && nativeSun && surfaceLit) {
        float3 view = normalize(-input.relativePosition);
        float3 halfway = normalize(view + direction);
        float ndotv = max(dot(shadingNormal, view), 1e-3);
        float power = exp2(13 * min(gloss, .98));
        float distribution = (power + 2) * .5 * pow(saturate(dot(shadingNormal, halfway)), power);
        float k = 2 / sqrt(3.14159265 * (power + 2));
        float visibilityTerm = .25 / ((ndotl * (1 - k) + k) * (ndotv * (1 - k) + k));
        float3 fresnel = f0 + (f90 - f0) * pow(1 - saturate(dot(view, halfway)), 5);
        specular = fresnel * distribution * visibilityTerm * ndotl * sun * visibility;
    }
    specular += environmentSpecular;
    color += specular;
#elif MAP_SOURCE_CHANNELS && MAP_DEBUG_MODE == 0
    // --style iw3: IW3 added its sun specular in the gamma-encoded framebuffer
    // (like its fog); added linearly it over-brightened dark surfaces.
    if (!unlit && nativeSun) {
        specular = SourceSunSpecular(input, flags, sampleUV, lod, shadingNormal, direction,
                                     pow(sun, 1.0 / 2.2), visibility, specularVariance);
        color = pow(pow(max(color, 0), 1.0 / 2.2) + specular, 2.2);
    }
#endif
    float alpha = kind == 2 ? texel.a : 1;
    // IW3 blended surfaces (glass, ground decals) draw ONE/INVSRCALPHA with a
    // premultiplied lit colour plus their environment reflection, which only
    // adds light. Reflection as coverage (zt-33..45) replaced the ground under
    // a decal's transparent strip with the dark environment at grazing angles
    // (Nuketown's black curb strip, owner test 2026-10-15).
    float3 reflection = 0;
#ifdef MAP_GLASS_SH
    // Translucent surfaces get no deferred probe reflection in Replay, so
    // evaluate the map's captured environment here.
    if (kind == 2 && !layer && !decalLayer)
    {
        float3 view = normalize(-input.relativePosition);
        float fresnel = pow(1 - saturate(abs(dot(view, shadingNormal))),
                            max(input.materialParameters.z, 1));
        float response = 1;
#if MAP_SOURCE_CHANNELS
        if ((flags & 64) != 0)
            response = sourceResponseAtlas.SampleLevel(colorSampler, sampleUV, lod).g;
#endif
        // IW3 reflects only on materials with a specular map (s0 techsets):
        // blended decals, carpets and posters get no environment film.
        if ((flags & 64) == 0)
            response = 0;
        float reflectivity = saturate(response * input.materialParameters.w * lerp(.08, 1, fresnel));
        float3 environment =
            GlassEnvironment(reflect(-view, shadingNormal)) * replayLighting[43].z * 2;
        reflection = environment * reflectivity;
    }
#endif
    // IW3 setExpFog: saturate(exp(-density * (distance - start))), with density
    // ln2 / halfway. The sky is not fogged in IW3.
    float fogFactor = 1;
#if defined(MAP_FOG) && MAP_DEBUG_MODE == 0
    if (!sky)
        fogFactor = saturate(exp(-MAP_FOG_DENSITY * (length(input.relativePosition) - MAP_FOG_START)));
#endif
    // IW3 falloff (1024; vertcol_simple_fog_foa, zfeather_foa_nf_eo):
    // f = saturate((N.V)^2 * parms.x + parms.y), colour *= f *
    // lerp(falloffEndColor, falloffBeginColor, f), the end colour as a fraction
    // of the begin colour (parms.z) folded into the layer's tint. God rays and
    // window hotspots fade to nothing edge on; a linear ramp that stopped at
    // the end colour left Gulag's god-ray cones as flat sheets.
    if ((flags & 1024) != 0)
    {
        float facing = dot(normalize(-input.relativePosition), normalize(input.normal));
        float ramp = saturate(facing * facing * input.materialParameters.x +
                              input.materialParameters.y);
        color *= ramp * lerp(input.materialParameters.z, 1, ramp);
    }
    // Multiply layers scale what is behind them by their colour; fog fades
    // them to neutral. Additive layers add light that fog fades out (IW3
    // fogged additive techniques toward black).
    // IW3 ps_mul: lerp(1, texture * vertex colour, vertex alpha).
    if ((flags & 512) != 0)
        return float4(lerp(1, texel.rgb, input.color.a * fogFactor), 1);
    // IW3's additive pixel shaders (vertcol_simple_add, zfeather_add_nf) output
    // colour * texture alpha * vertex alpha (texel carries both): god-ray cards
    // fade along their length through vertex alpha, and drew as uniform
    // hard-ended sheets without it (Gulag, owner test 2026-10-16). Additive
    // layers blend ONE/ONE, srcalpha-additive ones included.
    if ((flags & 256) != 0)
        return float4(min(color * alpha * fogFactor * replayLighting[42].x, 32255.0), alpha);
#if defined(MAP_FOG) && MAP_DEBUG_MODE == 0
    // Blended toward the fog colour in CoD4's gamma-encoded framebuffer.
    if (!sky)
    {
        float3 encoded = pow(max(color, 0), 1.0 / 2.2);
        color = pow(lerp(MAP_FOG_COLOR, encoded, fogFactor), 2.2);
    }
#endif
    if (kind == 2)
        color = color * alpha + reflection * fogFactor;
    return float4(min(color * replayLighting[42].x, 32255.0), alpha);
}
