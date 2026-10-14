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
// cb7[48..52], probe roughness fallback cb7[186]).
#if defined(MAP_IW8_LIGHTING) && MAP_LIGHTING_ROWS < 187
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
float3 SourceIndirect(float2 uv, float3 first, float3 tangentNormal) {
    float4 responseData = sourceResponseAtlas.SampleLevel(colorSampler, uv, 0);
    float4 normalData = sourceNormalAtlas.SampleLevel(colorSampler, uv, 0);
    first = float3(normalData.ba, responseData.a);
    float3 second = responseData.rgb;
    float2 raw = normalData.rg;
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
                         float visibility) {
    if ((flags & 64) == 0 || visibility <= 0)
        return 0;
#ifdef STATIC_MODEL
    float4 raw = sourceResponseAtlas.SampleBias(colorSampler, uv, replayView[7].y);
#else
    float4 raw = sourceResponseAtlas.SampleLevel(colorSampler, uv, lod);
#endif
    float3 view = normalize(-input.relativePosition);
    float3 reflected = reflect(-view, normal);
    float exponent = exp2(raw.a * 9.3775177) + 7;
    float lobe = saturate(exp((dot(reflected, sunDirection) - .99925) * exponent));
    float fresnel = pow(saturate(1 - abs(dot(view, normal))), input.materialParameters.z);
    float response = lerp(input.materialParameters.x, input.materialParameters.y, fresnel);
    return raw.rgb * response * input.materialParameters.w * lobe * sun * visibility;
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
float3 ProbeReflection(float2 screenPixel, float viewDepth, float3 relativePosition,
                       float3 reflected, float gloss) {
    uint2 tile = (uint2)(screenPixel * replayLighting[50].zw);
    float4 slices = viewDepth - 4 - float4(0, replayLighting[48].x,
                                           replayLighting[48].x + replayLighting[48].y,
                                           replayLighting[48].x + replayLighting[48].y +
                                               replayLighting[48].z);
    slices = saturate(slices * replayLighting[49]);
    uint slice = min((uint)(dot(slices, .25) * 32), 31);
    uint2 grid = (uint2)((replayLighting[50].xy + replayLighting[51].xy - 1) * replayLighting[50].zw);
    uint cluster = slice * grid.x * grid.y + tile.y * grid.x + tile.x;
    uint words = asuint(replayLighting[52].z);
    uint bin = probeDepthBins[min((uint)max(viewDepth, 0) >> 5, 8191)];
    uint first = bin >> 16;
    uint lastWord = min((bin >> 5) & 0x7ff, (uint)max((int)words - 1, 0));
    int count = clamp((int)(bin & 0xffff) - (int)first + 1, 0, 32);
    uint mask = count == 32 ? 0xffffffff : ~(0xffffffffu << count);
    float mip = floor(5 - 5 * gloss);
    float blend = frac(-5 * gloss);
    float2 border = float2(4u << (uint)clamp(mip, 0, 3), 4u << (uint)clamp(mip + 1, 0, 3));
    float2 scale = 1 - border / 128.0;
    float2 bias = border / 256.0;
    float3 sum = 0;
    float remaining = 1;
    for (uint word = first >> 5; word <= lastWord && word < 2048; ++word) {
        uint bits = probeClusterMasks[cluster * words + word];
        uint shift = (uint)clamp((int)first - (int)(word << 5), 0, 31);
        bits &= count == 32 ? 0xffffffff : mask << shift;
        while (bits != 0) {
            uint bit = firstbitlow(bits);
            bits ^= 1u << bit;
            uint probe = bit + (word << 5);
            ProbeParameters p = probeParameters[probe];
            ProbeBox box = probeBoxes[probe];
            float3 offset = relativePosition - (float3(box.v[0], box.v[1], box.v[2]) - replayView[4].xyz);
            float3 xAxis = float3(box.v[3], box.v[4], box.v[5]);
            float3 yAxis = float3(box.v[6], box.v[7], box.v[8]);
            float3 zAxis = float3(box.v[9], box.v[10], box.v[11]);
            float3 local = float3(dot(offset, xAxis), dot(offset, yAxis), dot(offset, zAxis));
            float3 inside = (float3(box.v[12], box.v[13], box.v[14]) - abs(local)) *
                            float3(p.v[4], p.v[5], p.v[6]);
            float weight = min(saturate(min(min(inside.z, inside.y), inside.x)), remaining);
            if (weight <= 0)
                continue;
            float3 direction = reflected;
            if (asint(p.v[0]) != -1) {
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
            remaining -= weight;
        }
    }
    return sum;
}
#endif

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

float4 main(Input input) : SV_TARGET0 {
    uint tile = (uint)input.metadata.x;
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
    // The stock forward pass uses these same scaled screen coordinates, sun
    // direction, irradiance and sun visibility texture.
    float2 screenPixel = input.position.xy * replayView[7].x;
    float2 screenUV = screenPixel * replayView[6].zw;
    float3 direction = replayLighting[56].xyz;
    float3 sun = max(0, replayLighting[57].xyz);
    bool nativeSun = replayLighting[56].w != 0;
    float ndotl = nativeSun ? saturate(dot(shadingNormal, direction)) : 0;
    float visibility = 1;
    // Glass has no opaque depth coverage. Solid and masked surfaces both
    // contribute matching depth and normals before the shadow resolve.
    if (!sky && (kind == 0 || kind == 3)) {
        if (nativeSun) {
            // Replay's forward shader skips t85 for fully shadowed 8x8 tiles.
            // The resolve can leave those texels unwritten; sampling them
            // directly exposes stale data as camera-dependent blocks of light.
            uint2 tilePosition = (uint2)screenPixel >> 3;
            uint tileColumns = ((uint)replayLighting[54].x + 7) >> 3;
            uint tileClass = lightingTiles[tilePosition.y * tileColumns + tilePosition.x].y >> 28;
            if (replayLighting[83].w > 0 && tileClass == 11)
                visibility = 0;
            else
                visibility = saturate(abs(sunVisibility.SampleLevel(shadowSampler, screenUV, 0).x));
        }
    }
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
    if (input.instance != 0xffffffff)
        indirect = InstanceAmbient(input.instance, shadingNormal) * MAP_INDIRECT_GAIN;
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
#if MAP_SOURCE_SUN_MASK
        // The source profile preserves the direction used to bake this mask.
        // Keep static occlusion at every distance; native shadows add moving
        // casters and nearby detail without erasing the authored building shade.
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
    if (!sky && (flags & 128) == 0 && (kind == 0 || kind == 3)) {
        float3 view = normalize(-input.relativePosition);
        float ndotv = saturate(dot(shadingNormal, view));
        float3 reflected = reflect(-view, shadingNormal);
        float3 f0 = .04;
        float gloss = .25;
#if MAP_SOURCE_CHANNELS
        if ((flags & 64) != 0) {
#ifdef STATIC_MODEL
            float4 response = sourceResponseAtlas.SampleBias(colorSampler, sampleUV, replayView[7].y);
#else
            float4 response = sourceResponseAtlas.SampleLevel(colorSampler, sampleUV, lod);
#endif
            f0 = lerp(response.rgb / 12.92, pow(max((response.rgb + .055) / 1.055, 0), 2.4),
                      step(.04045, response.rgb));
            gloss = response.a;
        }
#endif
        float gate = saturate(gloss * replayLighting[186].x + replayLighting[186].y);
        float3 environment = indirect;
        if (gate < 1) {
            float luminance = dot(indirect, float3(.2126, .7152, .0722));
            float3 probes = ProbeReflection(screenPixel, input.position.w, input.relativePosition,
                                            reflected, gloss) * luminance;
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
        float3 fresnel = f0 * brdf.x + brdf.y;
        environmentSpecular = fresnel * environment * replayLighting[43].w;
        indirect *= 1 - max(fresnel.r, max(fresnel.g, fresnel.b));
    }
#endif
    indirect *= replayLighting[43].z * gtao / max(bounce, 1e-3);
    float3 light = indirect + visibility * ndotl * sun;
    // Sky and IW3 unlit materials (flag 128) show their colour unlit.
    bool unlit = sky || (flags & 128) != 0;
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
#if MAP_SOURCE_CHANNELS && MAP_DEBUG_MODE == 0
    if (!unlit && nativeSun)
        specular = SourceSunSpecular(input, flags, sampleUV, lod, shadingNormal, direction,
                                     sun, visibility);
#endif
#if defined(MAP_IW8_LIGHTING) && MAP_DEBUG_MODE == 0
    specular += environmentSpecular;
#endif
    float3 color = texel.rgb * light + specular;
    float alpha = kind == 2 ? texel.a : 1;
#ifdef MAP_GLASS_SH
    // IW3 clear glass is a nearly transparent colour map; its visibility comes
    // from the envMapParms-weighted reflection. Translucent surfaces get no
    // deferred probe reflection in Replay, so evaluate the map's captured
    // environment here. Reflection adds coverage, as it does on real glass.
    if (kind == 2)
    {
        float3 view = normalize(-input.relativePosition);
        float fresnel = pow(1 - saturate(abs(dot(view, shadingNormal))),
                            max(input.materialParameters.z, 1));
        float response = 1;
#if MAP_SOURCE_CHANNELS
        if ((flags & 64) != 0)
            response = sourceResponseAtlas.SampleLevel(colorSampler, sampleUV, lod).g;
#endif
        float reflectivity = saturate(response * input.materialParameters.w * lerp(.08, 1, fresnel));
        float3 environment =
            GlassEnvironment(reflect(-view, shadingNormal)) * replayLighting[43].z * 2;
        float coverage = saturate(alpha + reflectivity * (1 - alpha));
        color = (color * alpha + environment * reflectivity * (1 - alpha)) / max(coverage, 1e-3);
        alpha = coverage;
    }
#endif
#if defined(MAP_FOG) && MAP_DEBUG_MODE == 0
    // IW3 setExpFog: saturate(exp(-density * (distance - start))), with density
    // ln2 / halfway, blended toward the fog colour in CoD4's gamma-encoded
    // framebuffer. The sky is not fogged in IW3.
    if (!sky)
    {
        float fogFactor = saturate(exp(-MAP_FOG_DENSITY *
                                       (length(input.relativePosition) - MAP_FOG_START)));
        float3 encoded = pow(max(color, 0), 1.0 / 2.2);
        color = pow(lerp(MAP_FOG_COLOR, encoded, fogFactor), 2.2);
    }
#endif
    return float4(min(color * replayLighting[42].x, 32255.0), alpha);
}
