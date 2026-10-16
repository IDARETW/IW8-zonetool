// Headless Direct3D 11 renderer for converted Replay maps (see sim.h).
//
// Exactness contract: world surfaces, static models, dynamic-entity models and
// glass panes are drawn with the DXBC vertex/pixel shaders embedded in the
// map's technique sets, fed the same buffers the zone carries (surface
// records, vertex streams, packed model vertices, placements, material images,
// lightmaps, baked sun depth, probe array). Emulated: the engine's per-frame
// constants (camera rows, sun direction/colour, exposure, screen size), its
// runtime-only textures (GTAO = 1, native sun visibility = 1, tile classes =
// 0, decal clusters empty, no local lights) and the probe tables it builds
// from the world's probe list.
#include "sim/sim_render.h"

#include "common/fs_util.h"
#include "common/json.hpp"
#include "common/log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#pragma comment(lib, "windowscodecs")

using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using zt::path_join;
using zt::read_file;
using zt::read_file_str;
using zt::write_file;
using zt::write_file_str;

namespace sim
{
namespace
{
constexpr float kPi = std::numbers::pi_v<float>;
constexpr unsigned kLightingRows = 187;
constexpr unsigned kViewRows = 12;
constexpr unsigned kEntityRows = 2 + 128 * 4;
constexpr uint32_t kModelAttributeBase = 0x02bc0000u; // unifiedGeometry attribute region
constexpr uint32_t kColorPageTableBase = 0x00240000u; // drawSurfaces colour page starts

void Check(const HRESULT hr, const char *what)
{
    if (FAILED(hr))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}

template <typename T> T At(const std::vector<uint8_t> &bytes, const size_t offset)
{
    T value{};
    if (offset + sizeof(T) > bytes.size())
        throw std::runtime_error("sim: record read outside its bytes");
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

struct Float3
{
    float x{}, y{}, z{};
};
Float3 operator-(const Float3 a, const Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 operator+(const Float3 a, const Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Float3 operator*(const Float3 a, const float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(const Float3 a, const Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 Cross(const Float3 a, const Float3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Float3 Normalize(const Float3 a)
{
    const float length = std::sqrt(Dot(a, a));
    return length > 0 ? a * (1.0f / length) : Float3{0, 0, 1};
}
// CoD angles (pitch, yaw, roll) -> quaternion: yaw about Z, then pitch about
// Y, then roll about X (AnglesToAxis order).
std::array<float, 4> QuaternionFromAngles(const std::array<float, 3> &angles)
{
    const auto axis = [](const float degrees, const int which) {
        const float half = degrees * kPi / 360.0f;
        std::array<float, 4> q{0, 0, 0, std::cos(half)};
        q[which] = std::sin(half);
        return q;
    };
    const auto multiply = [](const std::array<float, 4> &a, const std::array<float, 4> &b) {
        return std::array<float, 4>{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                                    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                                    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                                    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    };
    return multiply(multiply(axis(angles[1], 2), axis(angles[0], 1)), axis(angles[2], 0));
}
Float3 Rotate(const Float3 v, const std::array<float, 4> &q)
{
    // v + 2 q.xyz x (q.xyz x v + q.w v)
    const Float3 u{q[0], q[1], q[2]};
    const Float3 t = Cross(u, Cross(u, v) + v * q[3]);
    return v + t * 2.0f;
}

DXGI_FORMAT ReplayFormat(const unsigned format)
{
    switch (format)
    {
    case 6: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 32: return DXGI_FORMAT_R11G11B10_FLOAT;
    case 39: return DXGI_FORMAT_BC4_UNORM;
    case 40: return DXGI_FORMAT_BC5_UNORM;
    case 42: return DXGI_FORMAT_BC6H_UF16;
    case 44: return DXGI_FORMAT_BC7_UNORM;
    case 45: return DXGI_FORMAT_BC7_UNORM_SRGB;
    default: throw std::runtime_error("sim: unsupported Replay image format " + std::to_string(format));
    }
}
bool BlockFormat(const unsigned format)
{
    return format == 39 || format == 40 || format == 42 || format == 44 || format == 45;
}
size_t BlockBytes(const unsigned format)
{
    return format == 39 ? 8 : 16;
}

// Technique header bits (offset 0xA0 "other" state, 0xA8 blend state) as the
// converter writes them; see ADR notes in replay-iw3-blend-layers.
struct PassState
{
    unsigned cull = 1;       // 0 none, 1 back, 2 front
    bool depthBias = false;  // 0x10
    bool depthWrite = false; // 0x200
    unsigned depthTest = 0;  // bits 0x400 GE, 0x800 EQ, 0xC00 LE, 0 = always
    uint64_t blendBits = 0;
};
PassState DecodePass(const replayrender::Technique &technique)
{
    PassState state;
    const uint64_t other = At<uint64_t>(technique.header, 0xA0);
    state.cull = static_cast<unsigned>(other & 3);
    state.depthBias = (other & 0x10) != 0;
    state.depthWrite = (other & 0x200) != 0;
    state.depthTest = static_cast<unsigned>(other & 0xC00);
    state.blendBits = At<uint64_t>(technique.header, 0xA8);
    return state;
}
uint32_t TechniqueType(const replayrender::Technique &technique)
{
    return At<uint32_t>(technique.header, 8);
}

D3D11_BLEND SourceFactor(const unsigned code)
{
    switch (code)
    {
    case 0: return D3D11_BLEND_ONE;
    case 1: return D3D11_BLEND_ZERO;
    case 2: return D3D11_BLEND_SRC_COLOR;
    case 4: return D3D11_BLEND_SRC_ALPHA;
    case 5: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8: return D3D11_BLEND_DEST_COLOR;
    case 9: return D3D11_BLEND_INV_DEST_COLOR;
    default: return D3D11_BLEND_ONE;
    }
}
D3D11_BLEND DestFactor(const unsigned code)
{
    switch (code)
    {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 2: return D3D11_BLEND_SRC_COLOR;
    case 5: return D3D11_BLEND_INV_SRC_ALPHA;
    default: return D3D11_BLEND_ONE;
    }
}
D3D11_BLEND AlphaFactor(const D3D11_BLEND color)
{
    switch (color)
    {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return color;
    }
}

// Debug pixel shaders sharing the zone vertex shaders' output signature.
const char *kIdShader = R"(
cbuffer Ids : register(b10) { uint4 ids; };
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
#ifdef HAS_INSTANCE
    nointerpolation uint instance : INSTANCE0;
#endif
};
uint4 main(Input input) : SV_TARGET0 {
#ifdef HAS_INSTANCE
    uint instance = input.instance;
#else
    uint instance = ids.y;
#endif
    float3 n = normalize(input.normal) * .5 + .5;
    uint packedNormal = (uint)(saturate(n.x) * 255) | ((uint)(saturate(n.y) * 255) << 8) |
                        ((uint)(saturate(n.z) * 255) << 16);
    return uint4(ids.x, instance, ids.z, packedNormal);
}
)";

std::vector<uint8_t> CompileDebug(const char *source, const char *defines)
{
    std::string text = std::string(defines) + source;
    ComPtr<ID3DBlob> program, errors;
    const HRESULT hr = D3DCompile(text.data(), text.size(), "sim_ids.hlsl", nullptr, nullptr, "main",
                                  "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &program, &errors);
    if (FAILED(hr))
        throw std::runtime_error(std::string("sim: debug shader: ") +
                                 (errors ? static_cast<const char *>(errors->GetBufferPointer()) : "?"));
    return {static_cast<const uint8_t *>(program->GetBufferPointer()),
            static_cast<const uint8_t *>(program->GetBufferPointer()) + program->GetBufferSize()};
}

struct ShaderBinding
{
    struct Resource
    {
        unsigned bindPoint{};
        D3D_SHADER_INPUT_TYPE type{};
        std::string name;
    };
    std::vector<Resource> resources;
};
ShaderBinding Reflect(const std::vector<uint8_t> &program)
{
    ShaderBinding binding;
    ComPtr<ID3D11ShaderReflection> reflection;
    Check(D3DReflect(program.data(), program.size(), IID_PPV_ARGS(&reflection)), "D3DReflect");
    D3D11_SHADER_DESC desc{};
    reflection->GetDesc(&desc);
    for (unsigned i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC input{};
        reflection->GetResourceBindingDesc(i, &input);
        binding.resources.push_back({input.BindPoint, input.Type, input.Name});
    }
    return binding;
}

struct Pipeline
{
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ShaderBinding vsBinding, psBinding;
    PassState state;
    ComPtr<ID3D11RasterizerState> rasterizer[2]; // [flipWinding]
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11BlendState> blend;
    uint32_t type{};
};

// Shader register -> image name, from a technique's type-5 arguments
// (byte 2 = register, bytes 4-5 = material texture slot) resolved through the
// material's 8-byte texture headers (byte 0 = slot). Pipelines are shared by
// every material of a technique set, so this map belongs to the material.
using TextureMap = std::map<unsigned, std::string>;
TextureMap MaterialTextures(const replayrender::Material &material, const replayrender::Technique &technique)
{
    TextureMap textures;
    for (size_t at = 0; at + 6 <= technique.args.size(); at += 6)
    {
        if (technique.args[at] != 5)
            continue;
        const unsigned reg = technique.args[at + 2];
        const unsigned slot = technique.args[at + 4] | (technique.args[at + 5] << 8);
        for (size_t i = 0; i * 8 < material.textureHeaders.size() && i < material.images.size(); ++i)
            if (material.textureHeaders[i * 8] == slot)
                textures[reg] = material.images[i];
    }
    return textures;
}

struct MaterialGpu
{
    std::string name;
    unsigned id{};
    bool translucent{};
    uint8_t sortKey{}; // Material +0x19 (materialInfo starts at +0x08)
    std::shared_ptr<Pipeline> prepass, lit;
    TextureMap prepassTextures, litTextures;
    const replayrender::Material *source{};
};

struct RigidRun
{
    unsigned bone{}, vertCount{}, triOffset{}, triCount{};
};
struct ModelSurfaceGpu
{
    std::vector<RigidRun> runs; // bone-space vertex runs (entity models)
    std::array<float, 3> mid{}; // position quantisation bounds (per surface)
    float scale{1};
    unsigned boundsIndex{};    // surfaceBounds[] entry
    unsigned pageStart{};      // vertexPages[] start for this surface
    unsigned colorPageStart{}; // colorPages[] start
    unsigned indexFirst{};     // into modelIndices (u16 local)
    unsigned triCount{};
    std::string material;
    uint8_t cullType{0xEF};
};
struct ModelLodGpu
{
    float distance{};
    std::vector<ModelSurfaceGpu> surfaces;
};
struct BindPose
{
    std::string name;
    std::array<float, 4> quat{0, 0, 0, 1};
    std::array<float, 3> trans{};
    bool hidden{}; // damaged-state part the destructible script hides at spawn
};
struct ModelGpu
{
    std::string name;
    std::vector<ModelLodGpu> lods;
    std::vector<BindPose> bones; // model-space bind pose per bone
    std::array<float, 3> mid{}, half{};
    bool entity{}; // draws through the DObj path (entity materials)
};

struct Placement
{
    unsigned model{};
    std::array<float, 3> origin{};
    std::array<float, 4> quaternion{0, 0, 0, 1};
    float scale{1};
    bool dynamic{};
};

struct Batch
{
    const MaterialGpu *material{};
    uint8_t cullType{0xEF};
    bool entity{};
    unsigned placement{};  // entity draws: placement index
    unsigned modelIndex{}; // entity draws
    const ModelSurfaceGpu *surface{}; // entity draws: quantisation bounds
    std::vector<uint32_t> indices;
};
struct BatchRange
{
    Batch *batch;
    unsigned first, count;
};
// Model draw list for one LOD selection; rebuilt only when a placement's LOD
// (or the model/dynamic toggles) changes, not every frame.
struct DrawCache
{
    bool valid{};
    std::vector<const void *> selection;
    std::map<std::pair<std::string, uint8_t>, Batch> staticBatches;
    std::vector<Batch> entityBatches;
    std::vector<BatchRange> ranges;
};

constexpr char kTonemapShader[] = R"(
Texture2D<float4> hdr : register(t0);
cbuffer Tonemap : register(b0) { float4 exposure; };
float4 main(float4 position : SV_Position) : SV_Target
{
    float3 v = max(hdr.Load(int3(position.xy, 0)).rgb * exposure.x, 0);
    v = v / (1 + v * 0.15); // the CPU Tonemap's filmic roll-off
    float3 low = v * 12.92, high = 1.055 * pow(v, 1 / 2.4) - 0.055;
    return float4(saturate(v <= 0.0031308 ? low : high), 1);
}
)";
// Horizon-style AO from the depth prepass (an approximation of Replay's GTAO:
// same input, the depth buffer, and the same view dependence).
constexpr char kGtaoShader[] = R"(
Texture2D<float> depthImage : register(t0);
cbuffer Gtao : register(b0) { float4 projection; float4 size; };
// projection: sx, sy, near plane, world radius; size: width, height, 1/width, 1/height
float3 ViewPosition(int2 pixel)
{
    float d = depthImage.Load(int3(pixel, 0));
    float z = d > 0 ? projection.z / d : 1e6;
    float2 ndc = float2((pixel.x + .5) * size.z * 2 - 1, 1 - (pixel.y + .5) * size.w * 2);
    return float3(ndc.x / projection.x, ndc.y / projection.y, 1) * z;
}
float main(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float3 p = ViewPosition(pixel);
    if (p.z >= 1e5)
        return 1;
    float3 px = ViewPosition(pixel + int2(1, 0)) - p, py = ViewPosition(pixel + int2(0, 1)) - p;
    float3 nx = ViewPosition(pixel - int2(1, 0)) - p, ny = ViewPosition(pixel - int2(0, 1)) - p;
    float3 dx = abs(px.z) < abs(nx.z) ? px : -nx;
    float3 dy = abs(py.z) < abs(ny.z) ? py : -ny;
    float3 n = normalize(cross(dy, dx));
    if (dot(n, p) > 0) n = -n;
    float radiusPixels = projection.w * projection.x * size.x * .5 / p.z;
    radiusPixels = clamp(radiusPixels, 2, 96);
    float occlusion = 0, count = 0;
    [unroll] for (int d = 0; d < 8; ++d)
    {
        float angle = d * 0.7853982 + 0.3;
        float2 dir = float2(cos(angle), sin(angle));
        [unroll] for (int s = 1; s <= 4; ++s)
        {
            int2 sample = pixel + int2(dir * radiusPixels * s / 4);
            if (any(sample < 0) || sample.x >= (int)size.x || sample.y >= (int)size.y)
                continue;
            float3 v = ViewPosition(sample) - p;
            float len = length(v);
            float falloff = saturate(1 - len / projection.w);
            occlusion += saturate(dot(n, v) / max(len, 1e-3) - .1) * falloff;
            count += 1;
        }
    }
    return saturate(1 - 2 * occlusion / max(count, 1));
}
)";
constexpr char kFullscreenShader[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";
} // namespace

struct Simulator::Impl
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    replayrender::Mesh mesh;
    std::string directory;
    std::string style;
    Float3 sunDirection{0, 0, 1};
    Float3 sunColor{1, 1, 1};

    // Materials by name (world base material, additional, asset materials).
    std::map<std::string, MaterialGpu> materials;
    std::map<std::string, ComPtr<ID3D11ShaderResourceView>> images;
    std::map<std::string, std::shared_ptr<Pipeline>> pipelines;
    ComPtr<ID3D11ShaderResourceView> white, black, ones3d, brdf, probeImages, zeroTiles,
        probeParameters, probeBoxes, probeMasks, probeBins, decalVolumes, decalLists;
    ComPtr<ID3D11ShaderResourceView> worldSurfaces, worldPositions, worldAttributes;
    ComPtr<ID3D11Buffer> worldIndexBuffer;
    ComPtr<ID3D11ShaderResourceView> drawSurfaces, unifiedGeometry, vertexPages, vertexColors,
        colorPages, surfaceBounds, modelPlacements, glassVertices;
    ComPtr<ID3D11Buffer> drawSurfacesBuffer, modelIndexBuffer, placementsBuffer;
    ComPtr<ID3D11Buffer> cbView, cbLighting, cbSurface, cbEntity, cbIds;
    ComPtr<ID3D11SamplerState> samplers[16];
    ComPtr<ID3D11PixelShader> idShaderWorld, idShaderModel;
    ComPtr<ID3D11Texture2D> colorTarget, idTarget, depthTarget, colorStaging, idStaging,
        depthStaging, normalTarget;
    ComPtr<ID3D11RenderTargetView> colorView, idView, normalView;
    ComPtr<ID3D11DepthStencilView> depthView;
    unsigned targetWidth{}, targetHeight{};

    std::vector<ModelGpu> models;
    std::map<std::string, unsigned> modelByName;
    std::vector<Placement> placements;
    unsigned staticPlacementCount{};
    struct ScriptModel
    {
        std::string model;
        std::array<float, 3> origin{};
        std::array<float, 3> angles{}; // pitch, yaw, roll (degrees)
    };
    std::vector<ScriptModel> scriptModels;
    std::vector<uint16_t> modelIndices; // local indices of every model surface
    unsigned modelVertexPages{};
    unsigned sharedLods{}; // padded LOD copies that reuse an earlier LOD's pages
    DrawCache drawCache;
    struct LocalLight
    {
        Float3 origin, color, direction;
        float radius{};
        bool spot{};
        float cosOuter{}, cosInner{};
    };
    std::vector<LocalLight> localLights;
    ComPtr<ID3D11ShaderResourceView> localLightRecords, localLightBins, localLightMasks;
    ComPtr<ID3D11Buffer> localLightRecordsBuffer;
    unsigned localLightMaskTiles{};
    std::vector<std::string> missingModels; // placed but without exported geometry
    unsigned droppedPlacements{};
    ComPtr<ID3D11VertexShader> fullscreenVS;
    ComPtr<ID3D11PixelShader> tonemapPS, gtaoPS;
    ComPtr<ID3D11Buffer> cbGtao;
    ComPtr<ID3D11Texture2D> gtaoTarget;
    ComPtr<ID3D11RenderTargetView> gtaoView;
    ComPtr<ID3D11ShaderResourceView> gtaoResource, depthResource;
    bool gtaoActive{};
    ComPtr<ID3D11Buffer> cbTonemap;
    ComPtr<ID3D11Texture2D> ldrTarget, ldrStaging;
    ComPtr<ID3D11RenderTargetView> ldrView;
    ComPtr<ID3D11ShaderResourceView> colorResource;
    std::vector<unsigned> worldSurfaceMaterial; // material id per world surface
    struct GlassDraw
    {
        std::string material;
        unsigned firstVertex{}, vertexCount{};
    };
    std::vector<GlassDraw> glass;
    std::vector<uint32_t> glassIndices;
    ComPtr<ID3D11Buffer> glassIndexBuffer;
    Stats stats;

    // ---- resource helpers ------------------------------------------------
    ComPtr<ID3D11ShaderResourceView> StructuredBuffer(const void *data, const size_t bytes,
                                                      const unsigned stride, ComPtr<ID3D11Buffer> *out = nullptr,
                                                      const bool dynamic = false)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(std::max<size_t>(bytes, stride));
        desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = stride;
        std::vector<uint8_t> zero;
        if (!data || !bytes)
        {
            zero.assign(desc.ByteWidth, 0);
            data = zero.data();
        }
        D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        Check(device->CreateBuffer(&desc, &initial, &buffer), "structured buffer");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        view.Buffer.NumElements = desc.ByteWidth / stride;
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(buffer.Get(), &view, &srv), "structured SRV");
        if (out)
            *out = buffer;
        return srv;
    }
    ComPtr<ID3D11ShaderResourceView> RawBuffer(const void *data, size_t bytes,
                                               ComPtr<ID3D11Buffer> *out = nullptr)
    {
        bytes = std::max<size_t>((bytes + 3) & ~size_t{3}, 16);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(bytes);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        std::vector<uint8_t> padded(bytes, 0);
        if (data)
            std::memcpy(padded.data(), data, std::min(bytes, padded.size()));
        D3D11_SUBRESOURCE_DATA initial{padded.data(), 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        Check(device->CreateBuffer(&desc, &initial, &buffer), "raw buffer");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        view.BufferEx.NumElements = static_cast<UINT>(bytes / 4);
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(buffer.Get(), &view, &srv), "raw SRV");
        if (out)
            *out = buffer;
        return srv;
    }
    ComPtr<ID3D11Buffer> IndexBuffer(const void *data, const size_t bytes, const bool dynamic)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(std::max<size_t>(bytes, 4));
        desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
        std::vector<uint8_t> zero;
        if (!data || !bytes)
        {
            zero.assign(desc.ByteWidth, 0);
            data = zero.data();
        }
        D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        Check(device->CreateBuffer(&desc, &initial, &buffer), "index buffer");
        return buffer;
    }
    ComPtr<ID3D11Buffer> ConstantBuffer(const size_t bytes)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>((bytes + 15) & ~size_t{15});
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer;
        Check(device->CreateBuffer(&desc, nullptr, &buffer), "constant buffer");
        return buffer;
    }
    void Update(ID3D11Buffer *buffer, const void *data, const size_t bytes)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map");
        std::memcpy(mapped.pData, data, bytes);
        context->Unmap(buffer, 0);
    }
    ComPtr<ID3D11ShaderResourceView> Texture(const replayrender::Image &image)
    {
        const DXGI_FORMAT format = ReplayFormat(image.format);
        const bool blocks = BlockFormat(image.format);
        const unsigned slices = (image.flags & 0x38000u) == 0x8000u ? 6u : image.numElements;
        std::vector<D3D11_SUBRESOURCE_DATA> initial(static_cast<size_t>(slices) * image.mipCount);
        size_t cursor = 0;
        unsigned width = image.width, height = image.height;
        for (unsigned level = 0; level < image.mipCount; ++level)
        {
            const size_t pitch = blocks ? size_t((width + 3) / 4) * BlockBytes(image.format)
                                        : size_t(width) * 4;
            const size_t rows = blocks ? (height + 3) / 4 : height;
            const size_t subresource = (pitch * rows + 15) & ~size_t{15};
            for (unsigned slice = 0; slice < slices; ++slice)
            {
                auto &entry = initial[static_cast<size_t>(slice) * image.mipCount + level];
                entry.pSysMem = image.pixels.data() + cursor;
                entry.SysMemPitch = static_cast<UINT>(pitch);
                entry.SysMemSlicePitch = 0;
                cursor += subresource;
            }
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        if (cursor > image.pixels.size())
            throw std::runtime_error("sim: image pixels shorter than its mip chain: " + image.name);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image.width;
        desc.Height = image.height;
        desc.MipLevels = image.mipCount;
        desc.ArraySize = slices;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&desc, initial.data(), &texture), ("texture " + image.name).c_str());
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        if (slices > 1)
        {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipLevels = image.mipCount;
            view.Texture2DArray.ArraySize = slices;
        }
        else
        {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = image.mipCount;
        }
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(texture.Get(), &view, &srv), "texture SRV");
        return srv;
    }
    ComPtr<ID3D11ShaderResourceView> SolidTexture(const float r, const float g, const float b,
                                                  const float a)
    {
        const uint8_t pixel[4] = {uint8_t(r * 255), uint8_t(g * 255), uint8_t(b * 255),
                                  uint8_t(a * 255)};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 1;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixel, 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&desc, &initial, &texture), "solid texture");
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "solid SRV");
        return srv;
    }
    ComPtr<ID3D11ShaderResourceView> OnesTexture3D()
    {
        std::vector<uint16_t> ones(4 * 4 * 4 * 4, 0x3C00); // half 1.0
        D3D11_TEXTURE3D_DESC desc{};
        desc.Width = desc.Height = desc.Depth = 4;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{ones.data(), 4 * 8, 4 * 4 * 8};
        ComPtr<ID3D11Texture3D> texture;
        Check(device->CreateTexture3D(&desc, &initial, &texture), "3D LUT");
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "3D SRV");
        return srv;
    }
    // Split-sum environment BRDF (scale, bias) over (N.V, gloss): the stock
    // engine LUT t123; computed with the GGX roughness Replay's gloss implies.
    ComPtr<ID3D11ShaderResourceView> EnvironmentBrdf()
    {
        constexpr unsigned size = 64;
        std::vector<float> table(size * size * 2);
        for (unsigned y = 0; y < size; ++y)
        {
            const float gloss = (y + .5f) / size;
            const float roughness = std::clamp(1.0f - gloss, 0.02f, 1.0f);
            const float alpha = roughness * roughness;
            for (unsigned x = 0; x < size; ++x)
            {
                const float ndotv = std::max((x + .5f) / size, 1e-3f);
                const Float3 view{std::sqrt(1 - ndotv * ndotv), 0, ndotv};
                float a = 0, b = 0;
                constexpr unsigned samples = 128;
                for (unsigned i = 0; i < samples; ++i)
                {
                    // Hammersley GGX importance sample.
                    const float u = (i + .5f) / samples;
                    uint32_t bits = i;
                    bits = (bits << 16) | (bits >> 16);
                    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
                    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
                    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
                    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
                    const float v = bits * 2.3283064365386963e-10f;
                    const float phi = 2 * kPi * u;
                    const float cosTheta = std::sqrt((1 - v) / (1 + (alpha * alpha - 1) * v));
                    const float sinTheta = std::sqrt(1 - cosTheta * cosTheta);
                    const Float3 h{sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
                    const float vdoth = Dot(view, h);
                    const Float3 l = h * (2 * vdoth) - view;
                    const float ndotl = l.z, ndoth = h.z;
                    if (ndotl > 0)
                    {
                        const float k = alpha / 2;
                        const float g1v = ndotv / (ndotv * (1 - k) + k);
                        const float g1l = ndotl / (ndotl * (1 - k) + k);
                        const float gVis = g1v * g1l * vdoth / (ndoth * ndotv);
                        const float fc = std::pow(1 - vdoth, 5.0f);
                        a += (1 - fc) * gVis;
                        b += fc * gVis;
                    }
                }
                table[(y * size + x) * 2] = a / samples;
                table[(y * size + x) * 2 + 1] = b / samples;
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = size;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{table.data(), size * 8, 0};
        ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&desc, &initial, &texture), "BRDF LUT");
        ComPtr<ID3D11ShaderResourceView> srv;
        Check(device->CreateShaderResourceView(texture.Get(), nullptr, &srv), "BRDF SRV");
        return srv;
    }

    // ---- loading -------------------------------------------------------
    void RegisterImages(const replayrender::Material &material)
    {
        for (const auto &image : material.imageDefinitions)
            if (!images.contains(image.name))
                images[image.name] = Texture(image);
    }

    std::shared_ptr<Pipeline> BuildPipeline(const replayrender::Material &material,
                                            const replayrender::Technique &technique)
    {
        const std::string key = material.techset + "|" + technique.name;
        if (auto found = pipelines.find(key); found != pipelines.end())
            return found->second;
        auto pipeline = std::make_shared<Pipeline>();
        pipeline->type = TechniqueType(technique);
        pipeline->state = DecodePass(technique);
        const auto program = [&](const unsigned type, const std::string &name) -> const replayrender::Shader * {
            for (const auto &shader : material.shaders)
                if (shader.type == type && shader.name == name)
                    return &shader;
            return nullptr;
        };
        const auto *vs = program(14, technique.shaders[0]);
        const auto *ps = program(17, technique.shaders[3]);
        if (!vs || !ps || vs->program.empty() || ps->program.empty())
            throw std::runtime_error("sim: technique without shaders: " + technique.name);
        Check(device->CreateVertexShader(vs->program.data(), vs->program.size(), nullptr, &pipeline->vs),
              "vertex shader");
        Check(device->CreatePixelShader(ps->program.data(), ps->program.size(), nullptr, &pipeline->ps),
              "pixel shader");
        pipeline->vsBinding = Reflect(vs->program);
        pipeline->psBinding = Reflect(ps->program);
        for (unsigned flip = 0; flip < 2; ++flip)
        {
            D3D11_RASTERIZER_DESC rasterizer{};
            rasterizer.FillMode = D3D11_FILL_SOLID;
            rasterizer.CullMode = pipeline->state.cull == 0   ? D3D11_CULL_NONE
                                  : pipeline->state.cull == 1 ? D3D11_CULL_BACK
                                                              : D3D11_CULL_FRONT;
            // Replay front faces are clockwise (D3D default); --flip-winding is a diagnostic.
            rasterizer.FrontCounterClockwise = flip ? TRUE : FALSE;
            rasterizer.DepthClipEnable = TRUE;
            if (pipeline->state.depthBias)
            {
                // Replay r_polygonOffsetBias -1 / r_polygonOffsetScale -1.0 /
                // r_polygonOffsetClamp 0 (registered at RVA 0x192BF6E); with
                // reversed depth a positive bias moves the layer toward the
                // camera. SIM_POLYGON_OFFSET="bias,slope" overrides (diagnostic).
                rasterizer.DepthBias = 1;
                rasterizer.SlopeScaledDepthBias = 1.0f;
                if (const char *custom = std::getenv("SIM_POLYGON_OFFSET"); custom && *custom)
                    if (std::sscanf(custom, "%d,%f", &rasterizer.DepthBias, &rasterizer.SlopeScaledDepthBias) < 1)
                        rasterizer.DepthBias = 1;
            }
            Check(device->CreateRasterizerState(&rasterizer, &pipeline->rasterizer[flip]), "rasterizer");
        }
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = pipeline->state.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        switch (pipeline->state.depthTest)
        {
        case 0x400: depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; break;
        case 0x800: depth.DepthFunc = D3D11_COMPARISON_EQUAL; break;
        case 0xC00: depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL; break;
        default: depth.DepthFunc = D3D11_COMPARISON_ALWAYS; break;
        }
        // Reversed depth: the zone's LESS_EQUAL/GREATER_EQUAL are expressed
        // against Replay's reversed buffer; map them onto ours (near = 1).
        if (depth.DepthFunc == D3D11_COMPARISON_LESS_EQUAL)
            depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        else if (depth.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL)
            depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        Check(device->CreateDepthStencilState(&depth, &pipeline->depth), "depth state");
        D3D11_BLEND_DESC blend{};
        const uint64_t bits = pipeline->state.blendBits;
        auto &target = blend.RenderTarget[0];
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (bits)
        {
            target.BlendEnable = TRUE;
            target.SrcBlend = SourceFactor(static_cast<unsigned>(bits & 0xF));
            target.DestBlend = DestFactor(static_cast<unsigned>((bits >> 4) & 0xF));
            target.BlendOp = D3D11_BLEND_OP_ADD;
            target.SrcBlendAlpha = AlphaFactor(SourceFactor(static_cast<unsigned>((bits >> 11) & 0xF)));
            target.DestBlendAlpha = AlphaFactor(DestFactor(static_cast<unsigned>((bits >> 15) & 0xF)));
            target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
        Check(device->CreateBlendState(&blend, &pipeline->blend), "blend state");
        pipelines[key] = pipeline;
        return pipeline;
    }

    void RegisterMaterial(const replayrender::Material &material)
    {
        if (materials.contains(material.material) || material.techniques.empty())
            return;
        RegisterImages(material);
        MaterialGpu gpu;
        gpu.name = material.material;
        gpu.id = static_cast<unsigned>(materials.size() + 1);
        gpu.source = &material;
        if (material.materialInfo.size() > 0x11)
            gpu.sortKey = material.materialInfo[0x11];
        for (const auto &technique : material.techniques)
        {
            const auto type = TechniqueType(technique);
            if (type == 34)
            {
                gpu.lit = BuildPipeline(material, technique);
                gpu.litTextures = MaterialTextures(material, technique);
            }
            // Type 0 writes depth; the DObj type-9 variant (no depth write)
            // only stands in when a techset lacks type 0.
            else if (type == 0 || (type == 9 && !gpu.prepass))
            {
                gpu.prepass = BuildPipeline(material, technique);
                gpu.prepassTextures = MaterialTextures(material, technique);
            }
        }
        if (!gpu.lit)
            return; // effect quads and other non-geometry materials
        gpu.translucent = gpu.lit->state.blendBits != 0 || !gpu.prepass;
        materials[material.material] = std::move(gpu);
    }

    void BuildWorld()
    {
        worldSurfaces = StructuredBuffer(mesh.surfData.data(), mesh.surfData.size(), 88);
        worldPositions = RawBuffer(mesh.positions.data(), mesh.positions.size());
        worldAttributes = RawBuffer(mesh.aux.data(), mesh.aux.size());
        worldIndexBuffer = IndexBuffer(mesh.indices.data(), mesh.indices.size(), false);
        worldSurfaceMaterial.resize(mesh.count);
        for (unsigned i = 0; i < mesh.count; ++i)
        {
            const unsigned index = mesh.surfaceMaterials[i];
            const auto &material = index ? mesh.additionalMaterials[index - 1]
                                         : static_cast<const replayrender::Material &>(mesh);
            RegisterMaterial(material);
            const auto found = materials.find(material.material);
            worldSurfaceMaterial[i] = found == materials.end() ? 0 : found->second.id;
        }
    }

    void BuildProbes()
    {
        const auto &probes = mesh.reflectionProbes;
        std::vector<float> parameters, boxes;
        for (const auto &probe : probes)
        {
            // ProbeParameters (17 floats): projection origin (-1 bits = none),
            // image slot, inverse feather, planes; ProbeBox (15): centre, axes,
            // half size. The world-space centre is re-based per frame by the
            // shader through replayView[4]; the parameters here are static.
            const auto &half = probe.volume.halfSize;
            parameters.insert(parameters.end(), {probe.origin[0], probe.origin[1], probe.origin[2]});
            uint32_t slot = static_cast<uint32_t>(&probe - probes.data());
            float slotBits;
            std::memcpy(&slotBits, &slot, 4);
            parameters.push_back(slotBits);
            for (unsigned k = 0; k < 3; ++k)
                parameters.push_back(half[k] > 0 ? 4.0f / half[k] : 0.0f);
            parameters.insert(parameters.end(), {0, 0, 0, 0}); // v[7..10] unused
            for (unsigned k = 0; k < 3; ++k)
            {
                parameters.push_back(half[k]); // v[11],v[13],v[15]
                parameters.push_back(half[k]); // v[12],v[14],v[16]
            }
            boxes.insert(boxes.end(), {probe.volume.midpoint[0], probe.volume.midpoint[1],
                                       probe.volume.midpoint[2], 1, 0, 0, 0, 1, 0, 0, 0, 1,
                                       half[0], half[1], half[2]});
        }
        probeParameters = StructuredBuffer(parameters.data(), parameters.size() * 4, 17 * 4);
        probeBoxes = StructuredBuffer(boxes.data(), boxes.size() * 4, 15 * 4);
        std::vector<uint32_t> ones(64, 0xFFFFFFFFu);
        probeMasks = StructuredBuffer(ones.data(), ones.size() * 4, 4);
        probeBins = StructuredBuffer(ones.data(), ones.size() * 4, 4);
        if (!mesh.reflectionProbeArrayImage.name.empty())
            probeImages = Texture(mesh.reflectionProbeArrayImage);
    }

    void BuildModels(const Json &simJson, const std::vector<uint8_t> &blob)
    {
        std::map<std::string, const Json *> xmodels;
        for (const auto &model : simJson.at("xmodels"))
            xmodels[model.at("name").get<std::string>()] = &model;
        // Static models and dynamic entity models share the model table.
        std::vector<std::pair<std::string, bool>> wanted;
        for (const auto &model : simJson.at("staticModels"))
            wanted.emplace_back(model.at("name").get<std::string>(), false);
        for (const auto &entity : simJson.at("dynamicEntities"))
            if (!entity.value("brush", false))
                wanted.emplace_back(entity.at("model").get<std::string>(), true);
        for (const auto &scripted : scriptModels)
            if (xmodels.contains(scripted.model))
                wanted.emplace_back(scripted.model, true);
        std::vector<uint8_t> geometry; // unifiedGeometry: positions pages then attributes
        std::vector<uint32_t> pages, colorPagesTable;
        std::vector<uint8_t> colors;
        std::vector<float> bounds;
        std::vector<std::array<uint8_t, 12>> attributes;
        const auto staticMaterials = [&](const std::string &name) -> const Json * {
            for (const auto &model : simJson.at("staticModels"))
                if (model.at("name") == name)
                    return &model;
            return nullptr;
        };
        for (const auto &[name, entity] : wanted)
        {
            if (modelByName.contains(name))
                continue;
            const auto source = xmodels.find(name);
            if (source == xmodels.end())
            {
                zt::warn("sim: model without geometry: %s", name.c_str());
                missingModels.push_back(name);
                continue;
            }
            const Json &record = *source->second;
            const Json *staticRecord = entity ? nullptr : staticMaterials(name);
            ModelGpu model;
            model.name = name;
            model.entity = entity;
            for (unsigned k = 0; k < 3; ++k)
            {
                model.mid[k] = record.at("mid").at(k).get<float>();
                model.half[k] = record.at("half").at(k).get<float>();
            }
            const auto &materialsList = record.at("materials");
            for (const auto &bone : record.value("bones", Json::array()))
            {
                BindPose pose;
                pose.name = bone.value("name", "");
                // IW3 maps\mp\_destructible.gsc hides every damage-state tag
                // ("<tag>_d": shattered glass, broken lights) until the part is
                // destroyed; the converted scripts keep that behaviour.
                pose.hidden = pose.name.size() > 2 && pose.name.ends_with("_d");
                for (unsigned k = 0; k < 4; ++k)
                    pose.quat[k] = bone.at("quat").at(k).get<float>();
                for (unsigned k = 0; k < 3; ++k)
                    pose.trans[k] = bone.at("trans").at(k).get<float>();
                model.bones.push_back(pose);
            }
            unsigned surfaceIndex = 0;
            for (size_t lodIndex = 0; lodIndex < record.at("lods").size(); ++lodIndex)
            {
                const Json &lod = record.at("lods").at(lodIndex);
                ModelLodGpu lodGpu;
                lodGpu.distance = lod.at("distance").get<float>();
                const uint64_t blobOffset = lod.at("blobOffset").get<uint64_t>();
                const uint64_t blobSize = lod.at("blobSize").get<uint64_t>();
                if (blobOffset + blobSize > blob.size())
                    throw std::runtime_error("sim: model blob outside sim.bin: " + name);
                const uint8_t *lodBlob = blob.data() + blobOffset;
                // The converter pads single-LOD IW3 models to six LODs with copies
                // of the last one (r_forceLod); share a byte-identical earlier
                // LOD's pages instead of uploading the copy again.
                bool shared = false;
                for (size_t earlier = 0; earlier < lodIndex && !shared; ++earlier)
                {
                    const Json &other = record.at("lods").at(earlier);
                    if (other.at("blobSize").get<uint64_t>() != blobSize ||
                        other.at("surfaces").size() != lod.at("surfaces").size() ||
                        std::memcmp(blob.data() + other.at("blobOffset").get<uint64_t>(), lodBlob, blobSize) != 0)
                        continue;
                    lodGpu.surfaces = model.lods[earlier].surfaces;
                    surfaceIndex += static_cast<unsigned>(lod.at("surfaces").size());
                    ++sharedLods;
                    shared = true;
                }
                if (shared)
                {
                    model.lods.push_back(std::move(lodGpu));
                    continue;
                }
                for (size_t s = 0; s < lod.at("surfaces").size(); ++s, ++surfaceIndex)
                {
                    const Json &surface = lod.at("surfaces").at(s);
                    ModelSurfaceGpu gpu;
                    const unsigned vertCount = surface.at("vertCount").get<unsigned>();
                    const unsigned triCount = surface.at("triCount").get<unsigned>();
                    const uint64_t vertOffset = surface.at("vertOffset").get<uint64_t>();
                    const uint64_t indexOffset = surface.at("indexOffset").get<uint64_t>();
                    const uint64_t colorOffset = surface.at("colorOffset").get<uint64_t>();
                    if (vertOffset + size_t(vertCount) * 20 > blobSize ||
                        indexOffset + size_t(triCount) * 6 > blobSize)
                        throw std::runtime_error("sim: model surface outside its blob: " + name);
                    // Pages of 64 vertices: positions (8 B) at page * 512, attributes
                    // (12 B) at the fixed attribute region, as the zone's vertex
                    // shader addresses the engine's unified geometry buffer.
                    const unsigned pageCount = (vertCount + 63) / 64;
                    gpu.pageStart = static_cast<unsigned>(pages.size());
                    for (unsigned p = 0; p < pageCount; ++p)
                        pages.push_back(modelVertexPages + p);
                    const size_t positionBase = size_t(modelVertexPages) * 512;
                    geometry.resize(std::max(geometry.size(), positionBase + size_t(pageCount) * 512), 0);
                    attributes.resize(size_t(modelVertexPages + pageCount) * 64);
                    for (unsigned v = 0; v < vertCount; ++v)
                    {
                        const uint8_t *vertex = lodBlob + vertOffset + size_t(v) * 20;
                        std::memcpy(geometry.data() + positionBase + size_t(v) * 8, vertex, 8);
                        auto &attribute = attributes[size_t(modelVertexPages) * 64 + v];
                        std::memcpy(attribute.data(), vertex + 8, 4);       // metadata
                        std::memcpy(attribute.data() + 4, vertex + 16, 4);  // tangent frame
                        std::memcpy(attribute.data() + 8, vertex + 12, 4);  // texcoord halves
                    }
                    modelVertexPages += pageCount;
                    // Colour pages of 256 vertices (1 KB).
                    const unsigned colorPageCount = (vertCount + 255) / 256;
                    gpu.colorPageStart = static_cast<unsigned>(colorPagesTable.size());
                    const size_t colorBase = colors.size();
                    for (unsigned p = 0; p < colorPageCount; ++p)
                        colorPagesTable.push_back(static_cast<uint32_t>(colorBase / 1024 + p));
                    colors.resize(colorBase + size_t(colorPageCount) * 1024, 0xFF);
                    if (colorOffset != UINT32_MAX && colorOffset + size_t(vertCount) * 4 <= blobSize)
                        std::memcpy(colors.data() + colorBase, lodBlob + colorOffset, size_t(vertCount) * 4);
                    gpu.indexFirst = static_cast<unsigned>(modelIndices.size());
                    gpu.triCount = triCount;
                    for (const auto &run : surface.value("runs", Json::array()))
                        gpu.runs.push_back({run.at("bone").get<unsigned>(), run.at("vertCount").get<unsigned>(),
                                            run.at("triOffset").get<unsigned>(), run.at("triCount").get<unsigned>()});
                    const uint16_t *tris = reinterpret_cast<const uint16_t *>(lodBlob + indexOffset);
                    modelIndices.insert(modelIndices.end(), tris, tris + size_t(triCount) * 3);
                    gpu.boundsIndex = static_cast<unsigned>(bounds.size() / 8);
                    const auto &mid = surface.at("mid");
                    const auto &half = surface.at("half");
                    const float scale = std::max({half.at(0).get<float>(), half.at(1).get<float>(),
                                                  half.at(2).get<float>()});
                    bounds.insert(bounds.end(), {mid.at(0).get<float>(), mid.at(1).get<float>(),
                                                 mid.at(2).get<float>(), scale, 0, 0, 0, 0});
                    gpu.mid = {mid.at(0).get<float>(), mid.at(1).get<float>(), mid.at(2).get<float>()};
                    gpu.scale = scale;
                    if (staticRecord && lodIndex < staticRecord->at("lods").size() &&
                        s < staticRecord->at("lods").at(lodIndex).at("surfaces").size())
                    {
                        const Json &ss = staticRecord->at("lods").at(lodIndex).at("surfaces").at(s);
                        gpu.material = ss.at("material").get<std::string>();
                        gpu.cullType = static_cast<uint8_t>(ss.value("cullType", 0xEF));
                    }
                    else if (surfaceIndex < materialsList.size())
                        gpu.material = materialsList.at(surfaceIndex).get<std::string>();
                    lodGpu.surfaces.push_back(std::move(gpu));
                }
                model.lods.push_back(std::move(lodGpu));
            }
            modelByName[name] = static_cast<unsigned>(models.size());
            models.push_back(std::move(model));
        }
        const size_t attributeBytes = attributes.size() * 12;
        zt::info("sim: model geometry %u position pages (%.1f MB of the %.1f MB pool), %u padded LOD copies shared",
                 modelVertexPages, modelVertexPages * 512.0 / 1048576.0, kModelAttributeBase / 1048576.0, sharedLods);
        if (size_t(modelVertexPages) * 512 > kModelAttributeBase)
            throw std::runtime_error("sim: model positions exceed the unified geometry attribute base");
        geometry.resize(kModelAttributeBase + attributeBytes, 0);
        std::memcpy(geometry.data() + kModelAttributeBase, attributes.data(), attributeBytes);
        unifiedGeometry = RawBuffer(geometry.data(), geometry.size());
        vertexPages = RawBuffer(pages.data(), pages.size() * 4);
        vertexColors = RawBuffer(colors.data(), colors.size());
        colorPages = RawBuffer(colorPagesTable.data(), colorPagesTable.size() * 4);
        surfaceBounds = StructuredBuffer(bounds.data(), bounds.size() * 4, 32);
        for (const auto &instance : simJson.at("instances"))
        {
            Placement placement;
            placement.model = instance.at("model").get<unsigned>();
            for (unsigned k = 0; k < 3; ++k)
                placement.origin[k] = instance.at("origin").at(k).get<float>();
            for (unsigned k = 0; k < 4; ++k)
                placement.quaternion[k] = instance.at("quaternion").at(k).get<float>();
            placement.scale = instance.value("scale", 1.0f);
            // The instance refers to the static-model table; map to ours.
            const std::string name = simJson.at("staticModels").at(placement.model).at("name");
            const auto found = modelByName.find(name);
            if (found == modelByName.end())
            {
                ++droppedPlacements;
                continue;
            }
            placement.model = found->second;
            placements.push_back(placement);
        }
        staticPlacementCount = static_cast<unsigned>(placements.size());
        for (const auto &entity : simJson.at("dynamicEntities"))
        {
            if (entity.value("brush", false))
                continue;
            const auto found = modelByName.find(entity.at("model").get<std::string>());
            if (found == modelByName.end())
                continue;
            Placement placement;
            placement.model = found->second;
            placement.dynamic = true;
            for (unsigned k = 0; k < 3; ++k)
                placement.origin[k] = entity.at("origin").at(k).get<float>();
            for (unsigned k = 0; k < 4; ++k)
                placement.quaternion[k] = entity.at("quaternion").at(k).get<float>();
            placements.push_back(placement);
        }
        for (const auto &scripted : scriptModels)
        {
            const auto found = modelByName.find(scripted.model);
            if (found == modelByName.end())
                continue;
            Placement placement;
            placement.model = found->second;
            placement.dynamic = true;
            placement.origin = scripted.origin;
            placement.quaternion = QuaternionFromAngles(scripted.angles);
            placements.push_back(placement);
        }
        // Placements (t76): int origin * 4096, packed quaternion, scale.
        std::vector<uint8_t> packed(placements.size() * 24);
        for (size_t i = 0; i < placements.size(); ++i)
        {
            auto *p = packed.data() + i * 24;
            for (unsigned k = 0; k < 3; ++k)
            {
                const int32_t v = static_cast<int32_t>(std::lround(placements[i].origin[k] * 4096.0));
                std::memcpy(p + k * 4, &v, 4);
            }
            const auto &q = placements[i].quaternion;
            const auto u16 = [](const float v) {
                return static_cast<uint32_t>(std::lround(std::clamp(v * .5f + .5f, 0.0f, 1.0f) * 65535.0f));
            };
            const uint32_t xy = u16(q[0]) | (u16(q[1]) << 16), zw = u16(q[2]) | (u16(q[3]) << 16);
            std::memcpy(p + 12, &xy, 4);
            std::memcpy(p + 16, &zw, 4);
            std::memcpy(p + 20, &placements[i].scale, 4);
        }
        modelPlacements = StructuredBuffer(packed.data(), packed.size(), 24, &placementsBuffer);
        for (const auto &model : models)
            for (const auto &lod : model.lods)
                for (const auto &surface : lod.surfaces)
                    for (const auto &material : mesh.assetMaterials)
                        if (material.material == surface.material)
                            RegisterMaterial(material);
    }

    // Replay's native glass vertex (28 bytes): position, half2 texcoord,
    // packed tangent frame, colour, lighting handle. Panes are quads in the
    // pane frame (x across the width, y up the height, z the normal).
    static uint32_t PackTangentFrame(const std::array<float, 4> &quaternion, const bool negative)
    {
        std::array<float, 4> q = quaternion;
        unsigned largest = 0;
        for (unsigned k = 1; k < 4; ++k)
            if (std::abs(q[k]) > std::abs(q[largest]))
                largest = k;
        if (q[largest] < 0)
            for (auto &v : q)
                v = -v;
        std::array<float, 3> rest{};
        for (unsigned k = 0, j = 0; k < 4; ++k)
            if (k != largest)
                rest[j++] = q[k];
        const auto quantize = [](const float v, const float bits) {
            return static_cast<uint32_t>(std::lround(std::clamp((v / .7071067811865475f) * .5f + .5f, 0.0f, 1.0f) * bits));
        };
        return quantize(rest[0], 1023) | (quantize(rest[1], 1023) << 10) |
               (quantize(rest[2], 511) << 20) | (negative ? 1u << 29 : 0u) | (largest << 30);
    }
    static uint32_t Half2(const float u, const float v)
    {
        const auto half = [](float f) {
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            const uint32_t sign = (bits >> 16) & 0x8000;
            int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
            uint32_t mantissa = bits & 0x7FFFFF;
            if (exponent <= 0)
                return sign;
            if (exponent >= 31)
                return sign | 0x7C00;
            return sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
        };
        return half(u) | (half(v) << 16);
    }
    void BuildGlass()
    {
        std::vector<uint8_t> vertices;
        for (const auto &pane : mesh.glassPanes)
        {
            GlassDraw draw;
            draw.material = pane.material;
            draw.firstVertex = static_cast<unsigned>(vertices.size() / 28);
            const uint32_t frame = PackTangentFrame(pane.quaternion, false);
            const std::array<std::array<float, 2>, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
            for (const auto &corner : corners)
            {
                const float x = corner[0] * pane.halfWidth, y = corner[1] * pane.halfHeight;
                const Float3 world = Rotate({x, y, 0}, pane.quaternion) +
                                     Float3{pane.origin[0], pane.origin[1], pane.origin[2]};
                const float u = pane.texCoordOrigin[0] + pane.texVecs[0] * x + pane.texVecs[2] * y;
                const float v = pane.texCoordOrigin[1] + pane.texVecs[1] * x + pane.texVecs[3] * y;
                uint8_t vertex[28]{};
                std::memcpy(vertex, &world, 12);
                const uint32_t uv = Half2(u, v), color = 0xFFFFFFFFu, handle = 0;
                std::memcpy(vertex + 12, &uv, 4);
                std::memcpy(vertex + 16, &frame, 4);
                std::memcpy(vertex + 20, &color, 4);
                std::memcpy(vertex + 24, &handle, 4);
                vertices.insert(vertices.end(), vertex, vertex + 28);
            }
            const uint32_t base = draw.firstVertex;
            glassIndices.insert(glassIndices.end(), {base, base + 1, base + 2, base, base + 2, base + 3,
                                                     base, base + 2, base + 1, base, base + 3, base + 2});
            draw.vertexCount = 12;
            glass.push_back(draw);
            for (const auto &material : mesh.assetMaterials)
                if (material.material == pane.material)
                    RegisterMaterial(material);
        }
        if (!vertices.empty())
        {
            glassVertices = StructuredBuffer(vertices.data(), vertices.size(), 28);
            glassIndexBuffer = IndexBuffer(glassIndices.data(), glassIndices.size() * 4, false);
        }
    }

    void EnsureTargets(const unsigned width, const unsigned height)
    {
        if (targetWidth == width && targetHeight == height)
            return;
        targetWidth = width;
        targetHeight = height;
        const auto make = [&](const DXGI_FORMAT format, const UINT bind, ComPtr<ID3D11Texture2D> &texture,
                              const bool staging) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
            desc.BindFlags = staging ? 0 : bind;
            desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
            Check(device->CreateTexture2D(&desc, nullptr, &texture), "render target");
        };
        make(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, colorTarget,
             false);
        make(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, ldrTarget, false);
        make(DXGI_FORMAT_R8G8B8A8_UNORM, 0, ldrStaging, true);
        make(DXGI_FORMAT_R16G16B16A16_FLOAT, 0, colorStaging, true);
        make(DXGI_FORMAT_R32G32B32A32_UINT, D3D11_BIND_RENDER_TARGET, idTarget, false);
        make(DXGI_FORMAT_R32G32B32A32_UINT, 0, idStaging, true);
        make(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET, normalTarget, false);
        make(DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, depthTarget, false);
        make(DXGI_FORMAT_R32_TYPELESS, 0, depthStaging, true);
        make(DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, gtaoTarget, false);
        Check(device->CreateRenderTargetView(colorTarget.Get(), nullptr, &colorView), "RTV");
        Check(device->CreateRenderTargetView(ldrTarget.Get(), nullptr, &ldrView), "LDR RTV");
        Check(device->CreateShaderResourceView(colorTarget.Get(), nullptr, &colorResource), "HDR SRV");
        Check(device->CreateRenderTargetView(idTarget.Get(), nullptr, &idView), "id RTV");
        Check(device->CreateRenderTargetView(normalTarget.Get(), nullptr, &normalView), "normal RTV");
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        Check(device->CreateDepthStencilView(depthTarget.Get(), &dsv, &depthView), "DSV");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        Check(device->CreateShaderResourceView(depthTarget.Get(), &srv, &depthResource), "depth SRV");
        Check(device->CreateRenderTargetView(gtaoTarget.Get(), nullptr, &gtaoView), "GTAO RTV");
        Check(device->CreateShaderResourceView(gtaoTarget.Get(), nullptr, &gtaoResource), "GTAO SRV");
    }

    // ---- per-frame binding ------------------------------------------------
    ID3D11ShaderResourceView *EngineResource(const std::string &name)
    {
        if (name == "gtaoImage" && gtaoActive)
            return gtaoResource.Get();
        static const std::map<std::string, ComPtr<ID3D11ShaderResourceView> Impl::*> table = {
            {"sunVisibility", &Impl::white},        {"gtaoImage", &Impl::white},
            {"localLights", &Impl::localLightRecords}, {"localLightBins", &Impl::localLightBins},
            {"localLightMasks", &Impl::localLightMasks},
            {"lightingTiles", &Impl::zeroTiles},    {"probeParameters", &Impl::probeParameters},
            {"probeBoxes", &Impl::probeBoxes},      {"probeClusterMasks", &Impl::probeMasks},
            {"probeDepthBins", &Impl::probeBins},   {"specularOcclusionLut", &Impl::ones3d},
            {"environmentBrdf", &Impl::brdf},       {"probeImages", &Impl::probeImages},
            {"decalVolumes", &Impl::decalVolumes},  {"decalLists", &Impl::decalLists},
            {"decalColorAtlas", &Impl::white},      {"decalNormalAtlas", &Impl::white},
            {"decalMaskAtlas", &Impl::white},       {"surfaces", &Impl::worldSurfaces},
            {"positions", &Impl::worldPositions},   {"attributes", &Impl::worldAttributes},
            {"drawSurfaces", &Impl::drawSurfaces},  {"unifiedGeometry", &Impl::unifiedGeometry},
            {"vertexPages", &Impl::vertexPages},    {"vertexColors", &Impl::vertexColors},
            {"colorPages", &Impl::colorPages},      {"surfaceBounds", &Impl::surfaceBounds},
            {"modelPlacements", &Impl::modelPlacements}, {"glassVertices", &Impl::glassVertices},
        };
        const auto found = table.find(name);
        return found == table.end() ? nullptr : (this->*found->second).Get();
    }
    void BindShaderResources(const Pipeline &pipeline, const bool pixel, const TextureMap &materialTextures)
    {
        const ShaderBinding &binding = pixel ? pipeline.psBinding : pipeline.vsBinding;
        for (const auto &resource : binding.resources)
        {
            if (resource.type == D3D_SIT_SAMPLER)
            {
                ID3D11SamplerState *sampler = samplers[resource.bindPoint & 15].Get();
                if (pixel)
                    context->PSSetSamplers(resource.bindPoint, 1, &sampler);
                else
                    context->VSSetSamplers(resource.bindPoint, 1, &sampler);
                continue;
            }
            if (resource.type != D3D_SIT_TEXTURE && resource.type != D3D_SIT_STRUCTURED &&
                resource.type != D3D_SIT_BYTEADDRESS)
                continue;
            ID3D11ShaderResourceView *view = nullptr;
            if (const auto material = materialTextures.find(resource.bindPoint);
                material != materialTextures.end() && resource.type == D3D_SIT_TEXTURE)
            {
                const auto image = images.find(material->second);
                view = image == images.end() ? white.Get() : image->second.Get();
            }
            else
                view = EngineResource(resource.name);
            // Local-light falloff curve and shadow maps stay unbound: the zone
            // shader then uses a flat falloff and no shadow (its documented
            // fallback for generated techniques).
            if (!view && resource.type == D3D_SIT_TEXTURE && resource.name.rfind("localLight", 0) != 0)
                view = white.Get();
            if (pixel)
                context->PSSetShaderResources(resource.bindPoint, 1, &view);
            else
                context->VSSetShaderResources(resource.bindPoint, 1, &view);
        }
    }
    void UnbindAll()
    {
        ID3D11ShaderResourceView *nulls[128]{};
        context->PSSetShaderResources(0, 128, nulls);
        context->VSSetShaderResources(0, 128, nulls);
    }
};

Simulator::Simulator() : impl_(std::make_unique<Impl>())
{
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &impl_->device, &level, &impl_->context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
                               D3D11_SDK_VERSION, &impl_->device, &level, &impl_->context);
    Check(hr, "D3D11CreateDevice");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}
Simulator::~Simulator() = default;

void Simulator::Load(const std::string &simJsonPath)
{
    auto &impl = *impl_;
    std::ifstream input(simJsonPath, std::ios::binary);
    if (!input)
        throw std::runtime_error("sim: cannot read " + simJsonPath);
    const Json simJson = Json::parse(input);
    impl.directory = std::filesystem::path(simJsonPath).parent_path().string();
    map_ = simJson.at("map").get<std::string>();
    impl.style = simJson.value("style", "iw3");
    const auto &sun = simJson.at("sun");
    impl.sunDirection = Normalize({sun.at("direction").at(0).get<float>(), sun.at("direction").at(1).get<float>(),
                                   sun.at("direction").at(2).get<float>()});
    const float intensity = sun.value("intensity", 1.0f);
    const auto &color = sun.contains("primaryColor") ? sun.at("primaryColor") : sun.at("color");
    impl.sunColor = {color.at(0).get<float>() * intensity, color.at(1).get<float>() * intensity,
                     color.at(2).get<float>() * intensity};

    const auto t0 = std::chrono::steady_clock::now();
    impl.mesh = replayrender::Load(path_join(impl.directory, simJson.at("render").get<std::string>()));
    zt::info("sim: render data loaded in %.1fs", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::vector<uint8_t> blob;
    if (!read_file(path_join(impl.directory, simJson.at("bin").get<std::string>()), blob))
        throw std::runtime_error("sim: cannot read the model geometry blob");

    // Spawn points and bounds from the entity string.
    std::string ents;
    read_file_str(path_join(impl.directory, simJson.at("ents").get<std::string>()), ents);
    std::set<std::string> oldSchoolTargets;
    for (size_t at = 0; (at = ents.find('{', at)) != std::string::npos;)
    {
        const size_t end = ents.find('}', at);
        if (end == std::string::npos)
            break;
        const std::string block = ents.substr(at, end - at);
        at = end + 1;
        const auto value = [&](const char *key) -> std::string {
            const std::string needle = std::string("\"") + key + "\" \"";
            const size_t k = block.find(needle);
            if (k == std::string::npos)
                return {};
            const size_t v = k + needle.size();
            return block.substr(v, block.find('"', v) - v);
        };
        if (value("targetname") == "oldschool_pickup" && !value("target").empty())
            oldSchoolTargets.insert(value("target"));
    }
    {
        size_t at = 0;
        while ((at = ents.find('{', at)) != std::string::npos)
        {
            const size_t end = ents.find('}', at);
            if (end == std::string::npos)
                break;
            const std::string block = ents.substr(at, end - at);
            at = end + 1;
            const auto value = [&](const char *key) -> std::string {
                const std::string needle = std::string("\"") + key + "\" \"";
                const size_t k = block.find(needle);
                if (k == std::string::npos)
                    return {};
                const size_t v = k + needle.size();
                return block.substr(v, block.find('"', v) - v);
            };
            const std::string classname = value("classname");
            if (classname == "script_model")
            {
                // The entity string here is the IW3 source; draw only what the
                // converted map keeps in TDM/FFA. convert/maps.cpp drops old
                // school pickups (and the triggers they target); it strips the
                // gameobject name from HQ radios (targetname hq_hardpoint ->
                // hqloc); iw3_compat.gsc iw3compat_remove_gameobjects() deletes
                // every other script_model that has a script_gameobjectname.
                const std::string targetName = value("targetname");
                if (targetName == "oldschool_pickup" || oldSchoolTargets.contains(targetName))
                    continue;
                if (!value("script_gameobjectname").empty() && targetName != "hq_hardpoint")
                    continue;
                Impl::ScriptModel scripted;
                scripted.model = value("model");
                std::istringstream origin(value("origin")), angles(value("angles"));
                origin >> scripted.origin[0] >> scripted.origin[1] >> scripted.origin[2];
                angles >> scripted.angles[0] >> scripted.angles[1] >> scripted.angles[2];
                if (!scripted.model.empty() && scripted.model.front() != '*')
                    impl.scriptModels.push_back(scripted);
                continue;
            }
            if (classname.find("_spawn") == std::string::npos || classname.find("mp_") != 0)
                continue;
            Spawn spawn;
            spawn.classname = classname;
            std::istringstream origin(value("origin")), angles(value("angles"));
            origin >> spawn.origin[0] >> spawn.origin[1] >> spawn.origin[2];
            float pitch = 0;
            angles >> pitch >> spawn.yaw;
            spawns_.push_back(spawn);
        }
    }
    boundsMin_ = {impl.mesh.sceneBounds.midpoint[0] - impl.mesh.sceneBounds.halfSize[0],
                  impl.mesh.sceneBounds.midpoint[1] - impl.mesh.sceneBounds.halfSize[1],
                  impl.mesh.sceneBounds.midpoint[2] - impl.mesh.sceneBounds.halfSize[2]};
    boundsMax_ = {impl.mesh.sceneBounds.midpoint[0] + impl.mesh.sceneBounds.halfSize[0],
                  impl.mesh.sceneBounds.midpoint[1] + impl.mesh.sceneBounds.halfSize[1],
                  impl.mesh.sceneBounds.midpoint[2] + impl.mesh.sceneBounds.halfSize[2]};

    impl.white = impl.SolidTexture(1, 1, 1, 1);
    impl.black = impl.SolidTexture(0, 0, 0, 0);
    impl.ones3d = impl.OnesTexture3D();
    impl.brdf = impl.EnvironmentBrdf();
    {
        std::vector<uint32_t> zeros(2 * 2048 * 1024 / 64, 0);
        impl.zeroTiles = impl.StructuredBuffer(zeros.data(), zeros.size() * 4, 8);
        impl.decalVolumes = impl.StructuredBuffer(nullptr, 256, 256);
        impl.decalLists = impl.StructuredBuffer(nullptr, 64, 4);
    }
    for (unsigned s = 0; s < 16; ++s)
    {
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = s == 3 ? D3D11_FILTER_ANISOTROPIC : s == 8 ? D3D11_FILTER_MIN_MAG_MIP_POINT
                                                                : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        const auto mode = (s == 5 || s == 8 || s == 11) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressU = desc.AddressV = desc.AddressW = mode;
        desc.MaxAnisotropy = 8;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        Check(impl.device->CreateSamplerState(&desc, &impl.samplers[s]), "sampler");
    }
    impl.cbView = impl.ConstantBuffer(kViewRows * 16);
    impl.cbLighting = impl.ConstantBuffer(kLightingRows * 16);
    impl.cbSurface = impl.ConstantBuffer(16);
    impl.cbEntity = impl.ConstantBuffer(kEntityRows * 16);
    impl.cbIds = impl.ConstantBuffer(16);
    {
        const auto compile = [](const char *source, const char *profile) {
            ComPtr<ID3DBlob> program, errors;
            if (FAILED(D3DCompile(source, std::strlen(source), "sim_tonemap.hlsl", nullptr, nullptr, "main", profile,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &program, &errors)))
                throw std::runtime_error(std::string("sim: tonemap shader: ") +
                                         (errors ? static_cast<const char *>(errors->GetBufferPointer()) : "?"));
            return program;
        };
        const auto vs = compile(kFullscreenShader, "vs_5_0");
        const auto ps = compile(kTonemapShader, "ps_5_0");
        const auto ao = compile(kGtaoShader, "ps_5_0");
        Check(impl.device->CreatePixelShader(ao->GetBufferPointer(), ao->GetBufferSize(), nullptr, &impl.gtaoPS),
              "GTAO PS");
        impl.cbGtao = impl.ConstantBuffer(32);
        Check(impl.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &impl.fullscreenVS),
              "fullscreen VS");
        Check(impl.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &impl.tonemapPS),
              "tonemap PS");
        impl.cbTonemap = impl.ConstantBuffer(16);
        const auto world = CompileDebug(kIdShader, "");
        const auto model = CompileDebug(kIdShader, "#define HAS_INSTANCE 1\n");
        Check(impl.device->CreatePixelShader(world.data(), world.size(), nullptr, &impl.idShaderWorld), "id PS");
        Check(impl.device->CreatePixelShader(model.data(), model.size(), nullptr, &impl.idShaderModel), "id PS");
    }
    {
        std::string lightingPath = simJsonPath;
        if (const auto at = lightingPath.rfind(".sim.json"); at != std::string::npos)
            lightingPath.replace(at, 9, ".lighting.json");
        std::string text;
        if (read_file_str(lightingPath, text))
        {
            const Json lighting = Json::parse(text);
            const int sunIndex = lighting.value("sun_primary_light_index", -1);
            const auto &primary = lighting.value("primary_lights", Json::array());
            for (size_t i = 0; i < primary.size(); ++i)
            {
                const Json &light = primary[i];
                const int type = light.value("type", 0);
                // IW3 primary light types: 2 omni, 3 spot (1 is the sun).
                if (int(i) == sunIndex || (type != 2 && type != 3) || light.value("radius", 0.0f) <= 0)
                    continue;
                Impl::LocalLight local;
                const auto vec = [&](const char *key) {
                    const auto &v = light.at(key);
                    return Float3{v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()};
                };
                local.origin = vec("origin");
                local.color = vec("color");
                local.direction = vec("direction");
                local.radius = light.at("radius").get<float>();
                local.spot = type == 3;
                local.cosOuter = light.value("cos_half_fov_outer", 0.0f);
                local.cosInner = light.value("cos_half_fov_inner", 1.0f);
                impl.localLights.push_back(local);
            }
            if (impl.localLights.size() > 32)
                impl.localLights.resize(32); // one mask word; converted maps hold a handful
        }
        // Every light in every view-depth bin: [first:16][last:16].
        std::vector<uint32_t> bins(8192, impl.localLights.empty() ? 0u : uint32_t(impl.localLights.size() - 1));
        impl.localLightBins = impl.StructuredBuffer(bins.data(), bins.size() * 4, 4);
        impl.localLightMaskTiles = (3840 / 8) * (2160 / 8);
        const uint32_t all = impl.localLights.empty() ? 0u
                             : impl.localLights.size() == 32 ? 0xFFFFFFFFu
                                                              : (1u << impl.localLights.size()) - 1;
        std::vector<uint32_t> masks(impl.localLightMaskTiles, all);
        impl.localLightMasks = impl.StructuredBuffer(masks.data(), masks.size() * 4, 4);
        if (!impl.localLights.empty())
            zt::info("sim: %zu local light(s)", impl.localLights.size());
    }
    impl.BuildWorld();
    impl.BuildProbes();
    impl.BuildModels(simJson, blob);
    impl.BuildGlass();
    stats_.worldSurfaces = impl.mesh.count;
    stats_.staticInstances = impl.staticPlacementCount;
    stats_.staticModels = static_cast<unsigned>(impl.models.size());
    stats_.dynamicEntities = static_cast<unsigned>(impl.placements.size() - impl.staticPlacementCount);
    stats_.glassPanes = static_cast<unsigned>(impl.glass.size());
    stats_.materials = static_cast<unsigned>(impl.materials.size());
    stats_.pipelines = static_cast<unsigned>(impl.pipelines.size());
    zt::info("sim: %u world surfaces, %u static placements of %u models, %u dynamic entities, %u glass panes, %u materials, %u pipelines",
             stats_.worldSurfaces, stats_.staticInstances, stats_.staticModels, stats_.dynamicEntities,
             stats_.glassPanes, stats_.materials, stats_.pipelines);
}

namespace
{
struct ViewRows
{
    float rows[kViewRows][4]{};
};
ViewRows BuildView(const Camera &camera, const unsigned width, const unsigned height)
{
    const float yaw = camera.yaw * kPi / 180, pitch = camera.pitch * kPi / 180;
    const Float3 forward{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
    const Float3 right = Normalize(Cross(forward, {0, 0, 1}));
    const Float3 up = Cross(right, forward);
    const float aspect = float(width) / float(height);
    const float tanHalf = std::tan(camera.fov * kPi / 360);
    const float sx = 1 / tanHalf, sy = aspect / tanHalf;
    constexpr float nearPlane = 4.0f;
    ViewRows view;
    // Clip = rows . relative (camera-relative world position); reversed
    // infinite depth: z = near, w = view depth.
    view.rows[0][0] = right.x * sx; view.rows[0][1] = right.y * sx; view.rows[0][2] = right.z * sx;
    view.rows[1][0] = up.x * sy; view.rows[1][1] = up.y * sy; view.rows[1][2] = up.z * sy;
    view.rows[2][3] = nearPlane;
    view.rows[3][0] = forward.x; view.rows[3][1] = forward.y; view.rows[3][2] = forward.z;
    for (unsigned k = 0; k < 3; ++k)
    {
        view.rows[4][k] = camera.origin[k];
        const int32_t fixed = static_cast<int32_t>(std::lround(camera.origin[k] * 4096.0));
        std::memcpy(&view.rows[5][k], &fixed, 4);
        view.rows[9][k] = view.rows[5][k];
    }
    view.rows[6][2] = 1.0f / width;
    view.rows[6][3] = 1.0f / height;
    view.rows[7][0] = 1; // resolution scale
    view.rows[7][1] = 0; // mip bias
    return view;
}
} // namespace

Frame Simulator::Render(const Camera &camera, const RenderOptions &options)
{
    auto &impl = *impl_;
    ID3D11DeviceContext &ctx = *impl.context.Get();
    const auto t0 = std::chrono::steady_clock::now();
    impl.EnsureTargets(options.width, options.height);
    stats_.drawCalls = stats_.triangles = 0;

    const ViewRows view = BuildView(camera, options.width, options.height);
    impl.Update(impl.cbView.Get(), &view, sizeof(view));
    float lighting[kLightingRows][4]{};
    lighting[42][0] = 1;
    lighting[43][2] = 1;
    lighting[43][3] = 1;
    lighting[47][1] = 1;
    impl.gtaoActive = options.gtao && options.view == View::exact;
    if (impl.gtaoActive)
        lighting[47][0] = options.gtaoStrength;
    lighting[54][0] = float(options.width);
    if (!impl.localLights.empty())
    {
        const uint32_t words = 1;
        std::memcpy(&lighting[52][0], &words, 4);
        std::vector<uint8_t> records(impl.localLights.size() * 96, 0);
        for (size_t i = 0; i < impl.localLights.size(); ++i)
        {
            const auto &light = impl.localLights[i];
            uint8_t *r = records.data() + i * 96;
            const auto put = [&](size_t offset, float value) { std::memcpy(r + offset, &value, 4); };
            put(0, light.color.x);
            put(4, light.color.y);
            put(8, light.color.z);
            // Spot axis points back toward the light.
            put(16, -light.direction.x);
            put(20, -light.direction.y);
            put(24, -light.direction.z);
            const uint32_t flags = (light.spot ? 2u << 8 : 1u << 8) | 255u; // no shadow slot
            std::memcpy(r + 28, &flags, 4);
            put(48, light.origin.x - camera.origin[0]);
            put(52, light.origin.y - camera.origin[1]);
            put(56, light.origin.z - camera.origin[2]);
            put(60, 1.0f / light.radius);
            // Cone: saturate(c * scale + bias) from the outer to the inner angle.
            const float span = std::max(light.cosInner - light.cosOuter, 1e-3f);
            put(64, light.spot ? 1.0f / span : 0.0f);
            put(68, light.spot ? -light.cosOuter / span : 1.0f);
            put(72, 0.0f);
            put(76, 1.0f); // window: linear falloff to the radius (light_point_linear)
        }
        impl.localLightRecords = impl.RawBuffer(records.data(), records.size(), &impl.localLightRecordsBuffer);
    }
    lighting[56][0] = impl.sunDirection.x;
    lighting[56][1] = impl.sunDirection.y;
    lighting[56][2] = impl.sunDirection.z;
    lighting[56][3] = 1;
    lighting[57][0] = impl.sunColor.x;
    lighting[57][1] = impl.sunColor.y;
    lighting[57][2] = impl.sunColor.z;
    // Diagnostic: SIM_LIGHTING="row.component=value;..." overrides cb7 rows
    // (e.g. "56.3=0" turns the sun off) to bisect a shading problem.
    if (const char *overrides = std::getenv("SIM_LIGHTING"); overrides && *overrides)
    {
        const char *cursor = overrides;
        int row = 0, component = 0, consumed = 0;
        float value = 0;
        while (std::sscanf(cursor, "%d.%d=%f%n", &row, &component, &value, &consumed) == 3)
        {
            if (row >= 0 && row < int(std::size(lighting)) && component >= 0 && component < 4)
                lighting[row][component] = value;
            cursor += consumed;
            while (*cursor == ';' || *cursor == ' ')
                ++cursor;
        }
    }
    impl.Update(impl.cbLighting.Get(), lighting, sizeof(lighting));

    const bool idPass = options.view != View::exact;
    D3D11_VIEWPORT viewport{0, 0, float(options.width), float(options.height), 0, 1};
    ctx.RSSetViewports(1, &viewport);
    const float clear[4] = {0, 0, 0, 0};
    ctx.ClearRenderTargetView(impl.colorView.Get(), clear);
    const float zeroes[4]{};
    ctx.ClearRenderTargetView(impl.idView.Get(), zeroes);
    ctx.ClearDepthStencilView(impl.depthView.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
    ID3D11Buffer *cbs[] = {impl.cbEntity.Get(), nullptr, impl.cbView.Get(), nullptr, nullptr, nullptr, nullptr,
                           impl.cbLighting.Get(), nullptr, impl.cbSurface.Get(), impl.cbIds.Get()};
    ctx.VSSetConstantBuffers(0, 11, cbs);
    ctx.PSSetConstantBuffers(0, 11, cbs);
    ctx.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx.IASetInputLayout(nullptr);
    const float blendFactor[4] = {1, 1, 1, 1};

    // ---- static and dynamic model batches for this camera ----------------
    const auto lodFor = [&](const ModelGpu &model, const Placement &placement) -> const ModelLodGpu * {
        if (model.lods.empty())
            return nullptr;
        if (options.forceLod >= 0)
            return &model.lods[std::min<size_t>(options.forceLod, model.lods.size() - 1)];
        const Float3 delta = Float3{placement.origin[0], placement.origin[1], placement.origin[2]} -
                             Float3{camera.origin[0], camera.origin[1], camera.origin[2]};
        const float distance = std::sqrt(Dot(delta, delta));
        for (const auto &lod : model.lods)
            if (lod.distance <= 0 || distance < lod.distance)
                return &lod;
        return nullptr; // beyond the last LOD distance: culled, as in the game
    };
    // The draw list depends only on each placement's LOD and the toggles.
    std::vector<const void *> selection;
    selection.reserve(impl.placements.size() + 1);
    selection.push_back(reinterpret_cast<const void *>(
        uintptr_t((options.drawModels ? 1 : 0) | (options.drawDynamic ? 2 : 0) | ((options.forceLod + 1) << 2))));
    if (options.drawModels || options.drawDynamic)
        for (const Placement &placement : impl.placements)
            selection.push_back(placement.dynamic ? (options.drawDynamic ? lodFor(impl.models[placement.model], placement)
                                                                          : nullptr)
                                                  : (options.drawModels ? lodFor(impl.models[placement.model], placement)
                                                                        : nullptr));
    DrawCache &cache = impl.drawCache;
    if (!cache.valid || cache.selection != selection)
    {
    std::vector<uint32_t> drawSurfaceTable; // 4 uints per draw surface
    std::vector<uint32_t> colorStarts;
    auto &staticBatches = cache.staticBatches;
    auto &entityBatches = cache.entityBatches;
    staticBatches.clear();
    entityBatches.clear();
    std::vector<uint32_t> allIndices;
        if (options.drawModels || options.drawDynamic)
        {
            for (unsigned p = 0; p < impl.placements.size(); ++p)
            {
                const Placement &placement = impl.placements[p];
                if (placement.dynamic ? !options.drawDynamic : !options.drawModels)
                    continue;
                const ModelGpu &model = impl.models[placement.model];
                const ModelLodGpu *lod = lodFor(model, placement);
                if (!lod)
                    continue;
                for (const auto &surface : lod->surfaces)
                {
                    const auto material = impl.materials.find(surface.material);
                    if (material == impl.materials.end())
                        continue;
                    if (drawSurfaceTable.size() / 4 >= 65535)
                        break;
                    const uint16_t *local = impl.modelIndices.data() + surface.indexFirst;
                    if (model.entity)
                    {
                        // DObj path: one draw surface per rigid run, its bone in x.
                        std::vector<RigidRun> runs = surface.runs;
                        if (runs.empty())
                            runs.push_back({0, 0, 0, surface.triCount});
                        entityBatches.push_back({});
                        Batch &batch = entityBatches.back();
                        batch.entity = true;
                        batch.placement = p;
                        batch.modelIndex = placement.model;
                        batch.surface = &surface;
                        batch.material = &material->second;
                        batch.cullType = surface.cullType;
                        for (const auto &run : runs)
                        {
                            if (drawSurfaceTable.size() / 4 >= 65535)
                                break;
                            if (run.bone < model.bones.size() && model.bones[run.bone].hidden)
                                continue;
                            const uint32_t ds = static_cast<uint32_t>(drawSurfaceTable.size() / 4);
                            drawSurfaceTable.insert(drawSurfaceTable.end(),
                                                    {run.bone, surface.boundsIndex, surface.pageStart, 0});
                            colorStarts.push_back(surface.colorPageStart);
                            const unsigned end = std::min(surface.triCount, run.triOffset + run.triCount);
                            for (unsigned t = run.triOffset * 3; t < end * 3; ++t)
                                batch.indices.push_back((ds << 16) | local[t]);
                        }
                        continue;
                    }
                    const uint32_t ds = static_cast<uint32_t>(drawSurfaceTable.size() / 4);
                    drawSurfaceTable.insert(drawSurfaceTable.end(), {p, surface.boundsIndex, surface.pageStart, 0});
                    colorStarts.push_back(surface.colorPageStart);
                    Batch *batch = &staticBatches[{surface.material, surface.cullType}];
                    batch->material = &material->second;
                    batch->cullType = surface.cullType;
                    for (unsigned i = 0; i < surface.triCount * 3; ++i)
                        batch->indices.push_back((ds << 16) | local[i]);
                }
            }
        }
        // Draw-surface table (t63): records at 0, colour page starts at the
        // fixed table base the vertex shader reads.
        {
            std::vector<uint8_t> table(kColorPageTableBase + colorStarts.size() * 4 + 16, 0);
            std::memcpy(table.data(), drawSurfaceTable.data(), drawSurfaceTable.size() * 4);
            std::memcpy(table.data() + kColorPageTableBase, colorStarts.data(), colorStarts.size() * 4);
            impl.drawSurfaces = impl.RawBuffer(table.data(), table.size(), &impl.drawSurfacesBuffer);
        }
        auto &ranges = cache.ranges;
        ranges.clear();
        for (auto &[key, batch] : staticBatches)
        {
            ranges.push_back({&batch, static_cast<unsigned>(allIndices.size()), static_cast<unsigned>(batch.indices.size())});
            allIndices.insert(allIndices.end(), batch.indices.begin(), batch.indices.end());
        }
        for (auto &batch : entityBatches)
        {
            ranges.push_back({&batch, static_cast<unsigned>(allIndices.size()), static_cast<unsigned>(batch.indices.size())});
            allIndices.insert(allIndices.end(), batch.indices.begin(), batch.indices.end());
        }
        if (!allIndices.empty())
            impl.modelIndexBuffer = impl.IndexBuffer(allIndices.data(), allIndices.size() * 4, false);
        cache.selection = std::move(selection);
        cache.valid = true;
    }
    auto &ranges = cache.ranges;

    const auto applyPipeline = [&](const Pipeline &pipeline, const TextureMap &textures, const bool ids,
                                   const bool model) {
        ctx.VSSetShader(pipeline.vs.Get(), nullptr, 0);
        ctx.PSSetShader(ids ? (model ? impl.idShaderModel.Get() : impl.idShaderWorld.Get()) : pipeline.ps.Get(),
                        nullptr, 0);
        ctx.RSSetState(pipeline.rasterizer[options.flipWinding ? 1 : 0].Get());
        ctx.OMSetDepthStencilState(pipeline.depth.Get(), 0);
        ctx.OMSetBlendState(ids ? nullptr : pipeline.blend.Get(), blendFactor, 0xFFFFFFFF);
        impl.BindShaderResources(pipeline, false, textures);
        if (!ids)
            impl.BindShaderResources(pipeline, true, textures);
    };
    const auto setIds = [&](const uint32_t material, const uint32_t surface, const uint32_t extra) {
        const uint32_t ids[4] = {material, surface, extra, 0};
        impl.Update(impl.cbIds.Get(), ids, sizeof(ids));
    };
    const auto entityConstants = [&](const Placement &placement, const ModelGpu &model, const ModelSurfaceGpu &surface) {
        static float rows[kEntityRows][4];
        std::memset(rows, 0, sizeof(rows));
        // cb0[0]: position quantisation bounds. The converter packs every
        // surface against its own AABB (conv_xsurface computeBounds), and the
        // DObj path draws one surface per call, so this row is per surface.
        rows[0][0] = surface.mid[0];
        rows[0][1] = surface.mid[1];
        rows[0][2] = surface.mid[2];
        rows[0][3] = surface.scale;
        const auto &q = placement.quaternion;
        const float m[3][3] = {
            {1 - 2 * (q[1] * q[1] + q[2] * q[2]), 2 * (q[0] * q[1] - q[2] * q[3]), 2 * (q[0] * q[2] + q[1] * q[3])},
            {2 * (q[0] * q[1] + q[2] * q[3]), 1 - 2 * (q[0] * q[0] + q[2] * q[2]), 2 * (q[1] * q[2] - q[0] * q[3])},
            {2 * (q[0] * q[2] - q[1] * q[3]), 2 * (q[1] * q[2] + q[0] * q[3]), 1 - 2 * (q[0] * q[0] + q[1] * q[1])}};
        // Rigid vertices are model space (the converter writes vertex.position)
        // and the DObj bone matrices are relative to the bind pose, so every
        // bone of a resting entity carries the placement transform. The rigid
        // runs still pick their own row block (ds.x = bone), as in the game.
        const unsigned boneCount = std::max<unsigned>(1, static_cast<unsigned>(model.bones.size()));
        for (unsigned bone = 0; bone < std::min(128u, boneCount); ++bone)
        {
            for (unsigned r = 0; r < 3; ++r)
            {
                for (unsigned c = 0; c < 3; ++c)
                    rows[2 + bone * 4 + r][c] = m[r][c];
                const int32_t fixed = static_cast<int32_t>(std::lround(placement.origin[r] * 4096.0));
                std::memcpy(&rows[2 + bone * 4 + r][3], &fixed, 4);
            }
            rows[2 + bone * 4 + 3][3] = 1;
        }
        impl.Update(impl.cbEntity.Get(), rows, sizeof(rows));
    };

    for (int pass = 0; pass < 2; ++pass)
    {
        const bool prepass = pass == 0;
        if (!prepass && impl.gtaoActive)
        {
            ID3D11RenderTargetView *aoTargets[1] = {impl.gtaoView.Get()};
            ctx.OMSetRenderTargets(1, aoTargets, nullptr);
            ctx.RSSetState(nullptr);
            ctx.OMSetDepthStencilState(nullptr, 0);
            ctx.OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx.VSSetShader(impl.fullscreenVS.Get(), nullptr, 0);
            ctx.PSSetShader(impl.gtaoPS.Get(), nullptr, 0);
            const float tanHalf = std::tan(camera.fov * kPi / 360);
            const float aoConstants[8] = {1 / tanHalf, (float(options.width) / options.height) / tanHalf, 4.0f, 48.0f,
                                          float(options.width), float(options.height), 1.0f / options.width,
                                          1.0f / options.height};
            impl.Update(impl.cbGtao.Get(), aoConstants, sizeof(aoConstants));
            ID3D11Buffer *aoBuffers[1] = {impl.cbGtao.Get()};
            ctx.PSSetConstantBuffers(0, 1, aoBuffers);
            ID3D11ShaderResourceView *depth[1] = {impl.depthResource.Get()};
            ctx.PSSetShaderResources(0, 1, depth);
            ctx.Draw(3, 0);
            ID3D11ShaderResourceView *none[1] = {};
            ctx.PSSetShaderResources(0, 1, none);
            // Restore the scene constant buffers for the lit pass.
            ctx.VSSetConstantBuffers(0, 11, cbs);
            ctx.PSSetConstantBuffers(0, 11, cbs);
        }
        ID3D11RenderTargetView *targets[2] = {idPass ? impl.idView.Get() : impl.colorView.Get(),
                                              impl.normalView.Get()};
        ctx.OMSetRenderTargets(2, targets, impl.depthView.Get());
        // Draw list in the engine's order: the depth prepass and the opaque lit
        // pass (depth EQUAL) for world then models; then every blended surface
        // (no prepass technique: decal layers, translucent) after all opaque,
        // ordered by material sort key. reverseOrder reverses only draws the
        // engine may reorder: each pass's opaque list and each equal-key run.
        struct Item
        {
            bool model;
            unsigned index; // world surface or model range
            const MaterialGpu *material;
        };
        std::vector<Item> items;
        if (options.drawWorld)
            for (unsigned i = 0; i < impl.mesh.count; ++i)
            {
                const unsigned index = impl.mesh.surfaceMaterials[i];
                const auto &source = index ? impl.mesh.additionalMaterials[index - 1]
                                           : static_cast<const replayrender::Material &>(impl.mesh);
                const auto found = impl.materials.find(source.material);
                if (found != impl.materials.end())
                    items.push_back({false, i, &found->second});
            }
        for (unsigned r = 0; r < ranges.size(); ++r)
            items.push_back({true, r, ranges[r].batch->material});
        const auto group = [&](const Item &item) -> int {
            return item.material->prepass ? -1 : item.material->sortKey;
        };
        if (!prepass)
            std::stable_sort(items.begin(), items.end(),
                             [&](const Item &a, const Item &b) { return group(a) < group(b); });
        if (options.reverseOrder)
            for (size_t first = 0; first < items.size();)
            {
                size_t last = first;
                while (last < items.size() && (prepass || group(items[last]) == group(items[first])))
                    ++last;
                // World surfaces keep their fixed surface order (the engine draws
                // the BSP ranges in it); only model draws of the group, whose
                // order the engine may vary, are reversed in their own slots.
                std::vector<size_t> slots;
                for (size_t i = first; i < last; ++i)
                    if (items[i].model)
                        slots.push_back(i);
                for (size_t a = 0, b = slots.size(); a + 1 < b; ++a, --b)
                    std::swap(items[slots[a]], items[slots[b - 1]]);
                first = last;
            }
        int boundIndexBuffer = -1;
        for (const Item &item : items)
        {
            const MaterialGpu &material = *item.material;
            const Pipeline *pipeline = prepass ? material.prepass.get() : material.lit.get();
            if (!pipeline)
                continue;
            if (options.opaqueOnly && !material.prepass)
                continue;
            if (idPass && !prepass && material.prepass)
                continue; // opaque already resolved by the id prepass
            const bool ids = idPass && (prepass || !material.prepass);
            if (boundIndexBuffer != int(item.model))
            {
                if (item.model)
                    ctx.IASetIndexBuffer(impl.modelIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
                else
                    ctx.IASetIndexBuffer(impl.worldIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
                boundIndexBuffer = int(item.model);
            }
            applyPipeline(*pipeline, prepass ? material.prepassTextures : material.litTextures, ids, item.model);
            if (!item.model)
            {
                const unsigned i = item.index;
                const uint32_t surfaceIndex[4] = {i, 0, 0, 0};
                impl.Update(impl.cbSurface.Get(), surfaceIndex, sizeof(surfaceIndex));
                setIds(material.id, i, 0);
                const uint32_t baseIndex = At<uint32_t>(impl.mesh.surfaces, 40 * i + 12);
                const uint16_t triCount = At<uint16_t>(impl.mesh.surfaces, 40 * i + 10);
                ctx.DrawIndexed(triCount * 3, baseIndex, 0);
                ++stats_.drawCalls;
                stats_.triangles += triCount;
                continue;
            }
            const auto &range = ranges[item.index];
            if (range.batch->entity)
            {
                const Placement &placement = impl.placements[range.batch->placement];
                const ModelGpu &model = impl.models[range.batch->modelIndex];
                entityConstants(placement, model, *range.batch->surface);
                setIds(material.id, 0x80000000u | range.batch->placement, range.batch->cullType);
            }
            else
                setIds(material.id, 0, range.batch->cullType);
            ctx.DrawIndexed(range.count, range.first, 0);
            ++stats_.drawCalls;
            stats_.triangles += range.count / 3;
        }
        // Glass panes (lit pass only; no prepass technique).
        if (!prepass && options.drawGlass && impl.glassIndexBuffer)
        {
            ctx.IASetIndexBuffer(impl.glassIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
            for (size_t g = 0; g < impl.glass.size(); ++g)
            {
                const auto found = impl.materials.find(impl.glass[g].material);
                if (found == impl.materials.end() || !found->second.lit)
                    continue;
                applyPipeline(*found->second.lit, found->second.litTextures, idPass, false);
                setIds(found->second.id, 0x40000000u | static_cast<uint32_t>(g), 0);
                ctx.DrawIndexed(impl.glass[g].vertexCount, static_cast<UINT>(g * 12), 0);
                ++stats_.drawCalls;
            }
        }
        impl.UnbindAll();
    }
    ctx.Flush();

    Frame frame;
    frame.width = options.width;
    frame.height = options.height;
    if (!(options.gpuTonemap && options.view == View::exact))
        frame.rgba.resize(size_t(options.width) * options.height * 4);
    if (idPass)
    {
        ctx.CopyResource(impl.idStaging.Get(), impl.idTarget.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(ctx.Map(impl.idStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "id readback");
        frame.ids.resize(size_t(options.width) * options.height * 2);
        for (unsigned y = 0; y < options.height; ++y)
        {
            const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch);
            for (unsigned x = 0; x < options.width; ++x)
            {
                const uint32_t material = row[x * 4], instance = row[x * 4 + 1], cull = row[x * 4 + 2],
                               normal = row[x * 4 + 3];
                frame.ids[(size_t(y) * options.width + x) * 2] = material;
                frame.ids[(size_t(y) * options.width + x) * 2 + 1] = instance;
                float *out = frame.rgba.data() + (size_t(y) * options.width + x) * 4;
                uint32_t key = 0;
                switch (options.view)
                {
                case View::materialId: key = material; break;
                case View::surfaceId: key = instance * 2654435761u + material; break;
                case View::cullClass: key = cull * 0x9E3779B1u; break;
                default: break;
                }
                if (options.view == View::normals)
                {
                    out[0] = (normal & 255) / 255.0f;
                    out[1] = ((normal >> 8) & 255) / 255.0f;
                    out[2] = ((normal >> 16) & 255) / 255.0f;
                }
                else if (material == 0)
                    out[0] = out[1] = out[2] = 0;
                else
                {
                    const uint32_t h = key * 2246822519u + 3266489917u;
                    out[0] = .25f + .75f * ((h & 255) / 255.0f);
                    out[1] = .25f + .75f * (((h >> 8) & 255) / 255.0f);
                    out[2] = .25f + .75f * (((h >> 16) & 255) / 255.0f);
                }
                out[3] = 1;
            }
        }
        ctx.Unmap(impl.idStaging.Get(), 0);
    }
    else if (options.gpuTonemap && options.view == View::exact)
    {
        ID3D11RenderTargetView *ldr[1] = {impl.ldrView.Get()};
        ctx.OMSetRenderTargets(1, ldr, nullptr);
        ctx.RSSetState(nullptr);
        ctx.OMSetDepthStencilState(nullptr, 0);
        ctx.OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx.IASetInputLayout(nullptr);
        ctx.IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx.VSSetShader(impl.fullscreenVS.Get(), nullptr, 0);
        ctx.PSSetShader(impl.tonemapPS.Get(), nullptr, 0);
        const float exposure[4] = {options.exposure, 0, 0, 0};
        impl.Update(impl.cbTonemap.Get(), exposure, sizeof(exposure));
        ID3D11Buffer *tonemapConstants[1] = {impl.cbTonemap.Get()};
        ctx.PSSetConstantBuffers(0, 1, tonemapConstants);
        ID3D11ShaderResourceView *hdr[1] = {impl.colorResource.Get()};
        ctx.PSSetShaderResources(0, 1, hdr);
        ctx.Draw(3, 0);
        ID3D11ShaderResourceView *none[1] = {};
        ctx.PSSetShaderResources(0, 1, none);
        ctx.OMSetRenderTargets(0, nullptr, nullptr);
        ctx.CopyResource(impl.ldrStaging.Get(), impl.ldrTarget.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(ctx.Map(impl.ldrStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "LDR readback");
        frame.rgba8.resize(size_t(options.width) * options.height * 4);
        for (unsigned y = 0; y < options.height; ++y)
            std::memcpy(frame.rgba8.data() + size_t(y) * options.width * 4,
                        static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(options.width) * 4);
        ctx.Unmap(impl.ldrStaging.Get(), 0);
    }
    else
    {
        ctx.CopyResource(impl.colorStaging.Get(), impl.colorTarget.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(ctx.Map(impl.colorStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "colour readback");
        for (unsigned y = 0; y < options.height; ++y)
        {
            const uint16_t *row = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch);
            for (unsigned x = 0; x < options.width * 4; ++x)
            {
                const uint16_t h = row[x];
                const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
                uint32_t bits;
                if (exponent == 0)
                    bits = sign | (mantissa ? static_cast<uint32_t>((mantissa / 1024.0f) * std::ldexp(1.0f, -14) * 0 + 0) : 0);
                else if (exponent == 31)
                    bits = sign | 0x7F800000u;
                else
                    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
                float value;
                std::memcpy(&value, &bits, 4);
                if (exponent == 0 && mantissa)
                    value = (sign ? -1.0f : 1.0f) * (mantissa / 1024.0f) * std::ldexp(1.0f, -14);
                frame.rgba[size_t(y) * options.width * 4 + x] = value;
            }
        }
        ctx.Unmap(impl.colorStaging.Get(), 0);
    }
    if (options.view == View::depth)
    {
        ctx.CopyResource(impl.depthStaging.Get(), impl.depthTarget.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(ctx.Map(impl.depthStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "depth readback");
        for (unsigned y = 0; y < options.height; ++y)
        {
            const float *row = reinterpret_cast<const float *>(static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch);
            for (unsigned x = 0; x < options.width; ++x)
            {
                const float depth = row[x] > 0 ? 4.0f / row[x] : 1e6f; // reversed: z = near / depth
                const float shade = std::clamp(1.0f - std::log2(depth / 32.0f) / 10.0f, 0.0f, 1.0f);
                float *out = frame.rgba.data() + (size_t(y) * options.width + x) * 4;
                out[0] = out[1] = out[2] = shade;
                out[3] = 1;
            }
        }
        ctx.Unmap(impl.depthStaging.Get(), 0);
    }
    stats_.frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return frame;
}

std::vector<uint8_t> Simulator::Tonemap(const Frame &frame, const float exposure)
{
    std::vector<uint8_t> out(size_t(frame.width) * frame.height * 4);
    for (size_t i = 0; i < size_t(frame.width) * frame.height; ++i)
    {
        for (unsigned c = 0; c < 3; ++c)
        {
            float v = std::max(frame.rgba[i * 4 + c] * exposure, 0.0f);
            // Simple filmic roll-off so bright sun-lit surfaces keep their hue.
            v = v / (1.0f + v * 0.15f);
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
            out[i * 4 + c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255 + .5f);
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}

namespace
{
std::vector<uint8_t> EncodeWic(const uint8_t *rgba, const unsigned width, const unsigned height,
                               const GUID &container, const int quality)
{
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "WIC factory");
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "stream");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(container, nullptr, &encoder), "WIC encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "encoder init");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    Check(encoder->CreateNewFrame(&frame, &properties), "frame");
    if (container == GUID_ContainerFormatJpeg && properties)
    {
        PROPBAG2 option{};
        wchar_t name[] = L"ImageQuality";
        option.pstrName = name;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = quality / 100.0f;
        properties->Write(1, &option, &value);
    }
    Check(frame->Initialize(properties.Get()), "frame init");
    Check(frame->SetSize(width, height), "size");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    Check(frame->SetPixelFormat(&format), "format");
    std::vector<uint8_t> converted;
    if (format == GUID_WICPixelFormat32bppRGBA)
        Check(frame->WritePixels(height, width * 4, width * height * 4, const_cast<uint8_t *>(rgba)), "pixels");
    else if (format == GUID_WICPixelFormat32bppBGRA || format == GUID_WICPixelFormat32bppBGR)
    {
        converted.resize(size_t(width) * height * 4);
        for (size_t i = 0; i < size_t(width) * height; ++i)
        {
            converted[i * 4] = rgba[i * 4 + 2];
            converted[i * 4 + 1] = rgba[i * 4 + 1];
            converted[i * 4 + 2] = rgba[i * 4];
            converted[i * 4 + 3] = rgba[i * 4 + 3];
        }
        Check(frame->WritePixels(height, width * 4, static_cast<UINT>(converted.size()), converted.data()), "pixels");
    }
    else if (format == GUID_WICPixelFormat24bppBGR)
    {
        converted.resize(size_t(width) * height * 3);
        for (size_t i = 0; i < size_t(width) * height; ++i)
        {
            converted[i * 3] = rgba[i * 4 + 2];
            converted[i * 3 + 1] = rgba[i * 4 + 1];
            converted[i * 3 + 2] = rgba[i * 4];
        }
        Check(frame->WritePixels(height, width * 3, static_cast<UINT>(converted.size()), converted.data()), "pixels");
    }
    else
        throw std::runtime_error("sim: unsupported WIC pixel format negotiated");
    Check(frame->Commit(), "frame commit");
    Check(encoder->Commit(), "encoder commit");
    HGLOBAL global{};
    Check(GetHGlobalFromStream(stream.Get(), &global), "hglobal");
    const size_t size = GlobalSize(global);
    const auto *data = static_cast<const uint8_t *>(GlobalLock(global));
    std::vector<uint8_t> out(data, data + size);
    GlobalUnlock(global);
    return out;
}
} // namespace

std::vector<uint8_t> Simulator::EncodeJpeg(const uint8_t *rgba, const unsigned width, const unsigned height,
                                           const int quality)
{
    return EncodeWic(rgba, width, height, GUID_ContainerFormatJpeg, quality);
}
std::vector<uint8_t> Simulator::EncodePng(const uint8_t *rgba, const unsigned width, const unsigned height)
{
    return EncodeWic(rgba, width, height, GUID_ContainerFormatPng, 100);
}

MaterialInfo Simulator::Material(const uint32_t id) const
{
    MaterialInfo info;
    for (const auto &[name, gpu] : impl_->materials)
        if (gpu.id == id)
        {
            info.name = name;
            info.techset = gpu.source ? gpu.source->techset : "";
            info.translucent = gpu.translucent;
            info.opaque = gpu.prepass != nullptr;
            break;
        }
    return info;
}

std::string Simulator::InstanceModel(const uint32_t instance) const
{
    const auto &impl = *impl_;
    if (instance & 0x40000000u && !(instance & 0x80000000u))
        return "glass";
    const unsigned p = instance & 0x3FFFFFFFu;
    if ((instance & 0x80000000u) && p < impl.placements.size())
        return impl.models[impl.placements[p].model].name;
    return {};
}

std::string Simulator::Audit() const
{
    const auto &impl = *impl_;
    Json report;
    // Geometry whose material never registered (no lit technique) is not drawn.
    std::map<std::string, unsigned> undrawnWorld, undrawnModel;
    for (unsigned i = 0; i < impl.mesh.count; ++i)
    {
        const unsigned index = impl.mesh.surfaceMaterials[i];
        const auto &source = index ? impl.mesh.additionalMaterials[index - 1]
                                   : static_cast<const replayrender::Material &>(impl.mesh);
        if (!impl.materials.contains(source.material))
            ++undrawnWorld[source.material];
    }
    std::set<std::string> usedMaterials;
    for (const auto &model : impl.models)
        for (const auto &lod : model.lods)
            for (const auto &surface : lod.surfaces)
            {
                usedMaterials.insert(surface.material);
                if (!impl.materials.contains(surface.material))
                    ++undrawnModel[model.name + " | " + surface.material];
            }
    report["undrawnWorldSurfaces"] = undrawnWorld;
    report["undrawnModelSurfaces"] = undrawnModel;
    // Material images the map data does not define (white in the simulator;
    // stock images resolve in the game, anything else is a missing asset).
    std::map<std::string, std::vector<std::string>> missingImages;
    std::map<std::string, unsigned> engineInputs;
    for (const auto &[name, gpu] : impl.materials)
    {
        for (const auto *textures : {&gpu.litTextures, &gpu.prepassTextures})
            for (const auto &[reg, image] : *textures)
                if (!impl.images.contains(image))
                    missingImages[image].push_back(name);
        if (gpu.lit)
            for (const auto *binding : {&gpu.lit->psBinding, &gpu.lit->vsBinding})
                for (const auto &resource : binding->resources)
                {
                    if (resource.type == D3D_SIT_SAMPLER || resource.type == D3D_SIT_CBUFFER)
                        continue;
                    if (gpu.litTextures.contains(resource.bindPoint) && resource.type == D3D_SIT_TEXTURE)
                        continue;
                    const bool known = const_cast<Impl &>(impl).EngineResource(resource.name) != nullptr;
                    ++engineInputs[(known ? "emulated: " : "MISSING (white/null): ") + resource.name];
                }
    }
    Json images = Json::object();
    for (auto &[image, users] : missingImages)
    {
        std::sort(users.begin(), users.end());
        users.erase(std::unique(users.begin(), users.end()), users.end());
        images[image] = {{"materials", users.size()}, {"example", users.front()}};
    }
    report["imagesNotInMapData"] = images;
    report["shaderInputs"] = engineInputs;
    report["modelsWithoutGeometry"] = impl.missingModels;
    report["droppedStaticPlacements"] = impl.droppedPlacements;
    // Bounds sanity for every model surface.
    unsigned badBounds = 0;
    for (const auto &model : impl.models)
        for (const auto &lod : model.lods)
            for (const auto &surface : lod.surfaces)
                if (!std::isfinite(surface.scale) || surface.scale <= 0 || surface.scale > 65536 ||
                    !std::isfinite(surface.mid[0]) || !std::isfinite(surface.mid[1]) || !std::isfinite(surface.mid[2]))
                    ++badBounds;
    report["modelSurfacesWithBadBounds"] = badBounds;
    report["counts"] = {{"materials", impl.materials.size()},
                        {"images", impl.images.size()},
                        {"models", impl.models.size()},
                        {"placements", impl.placements.size()},
                        {"worldSurfaces", impl.mesh.count}};
    return report.dump(2);
}

std::string Simulator::Describe(const Camera &camera, const RenderOptions &options, const unsigned x,
                                const unsigned y)
{
    RenderOptions ids = options;
    ids.view = View::materialId;
    auto &impl = *impl_;
    Render(camera, ids);
    ID3D11DeviceContext &ctx = *impl.context.Get();
    ctx.CopyResource(impl.idStaging.Get(), impl.idTarget.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(ctx.Map(impl.idStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "id readback");
    const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(mapped.pData) +
                                                             size_t(std::min(y, options.height - 1)) * mapped.RowPitch);
    const uint32_t material = row[std::min(x, options.width - 1) * 4], instance = row[std::min(x, options.width - 1) * 4 + 1],
                   cull = row[std::min(x, options.width - 1) * 4 + 2];
    ctx.Unmap(impl.idStaging.Get(), 0);
    Json out;
    out["material"] = nullptr;
    for (const auto &[name, gpu] : impl.materials)
        if (gpu.id == material)
        {
            out["material"] = name;
            out["source"] = gpu.source && gpu.source->material == name ? gpu.source->techset : "";
            out["translucent"] = gpu.translucent;
            break;
        }
    if (instance & 0x80000000u)
    {
        const unsigned p = instance & 0x7FFFFFFFu;
        if (p < impl.placements.size())
        {
            out["dynamicEntity"] = p - impl.staticPlacementCount;
            out["model"] = impl.models[impl.placements[p].model].name;
            out["origin"] = impl.placements[p].origin;
        }
    }
    else if (instance & 0x40000000u)
        out["glassPane"] = instance & 0x3FFFFFFFu;
    else if (material && instance < impl.placements.size() && cull)
    {
        out["instance"] = instance;
        out["model"] = impl.models[impl.placements[instance].model].name;
        out["origin"] = impl.placements[instance].origin;
        out["cullType"] = cull;
    }
    else if (material)
        out["worldSurface"] = instance;
    return out.dump();
}
} // namespace sim
