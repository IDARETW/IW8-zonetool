#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr unsigned kResolution = 512;

void check(const HRESULT result, const char *operation)
{
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(static_cast<unsigned>(result)));
}

struct Vertex
{
    float x, y, depth;
};

std::vector<Vertex> readTriangles(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 8)
        throw std::runtime_error("cannot read projected triangles");
    const auto length = static_cast<std::size_t>(input.tellg());
    input.seekg(0);
    std::array<std::uint32_t, 2> header{};
    if (!input.read(reinterpret_cast<char *>(header.data()), 8) ||
        header[0] != 0x54535343u || header[1] > 1000000u ||
        length != 8 + std::size_t(header[1]) * 3 * sizeof(Vertex))
        throw std::runtime_error("invalid projected triangle file");
    std::vector<Vertex> vertices(std::size_t(header[1]) * 3);
    if (!input.read(reinterpret_cast<char *>(vertices.data()),
                    static_cast<std::streamsize>(vertices.size() * sizeof(Vertex))))
        throw std::runtime_error("truncated projected triangle file");
    for (const auto &vertex : vertices)
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) ||
            !std::isfinite(vertex.depth))
            throw std::runtime_error("nonfinite projected triangle vertex");
    return vertices;
}

std::vector<float> drawDepth(ID3D11Device *device, ID3D11DeviceContext *context,
                             ID3D11VertexShader *vertexShader,
                             ID3D11InputLayout *layout, ID3D11Buffer *vertices,
                             const unsigned vertexCount, const float slopeBias,
                             const bool maximum)
{
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = texture.Height = kResolution;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R32_TYPELESS;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> target;
    check(device->CreateTexture2D(&texture, nullptr, &target), "create depth target");

    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_D32_FLOAT;
    view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> depthView;
    check(device->CreateDepthStencilView(target.Get(), &view, &depthView),
          "create depth view");

    texture.Usage = D3D11_USAGE_STAGING;
    texture.BindFlags = 0;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    check(device->CreateTexture2D(&texture, nullptr, &readback),
          "create depth readback");

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.SlopeScaledDepthBias = slopeBias;
    ComPtr<ID3D11RasterizerState> rasterState;
    check(device->CreateRasterizerState(&raster, &rasterState),
          "create slope-bias rasterizer");

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = maximum ? D3D11_COMPARISON_GREATER_EQUAL
                              : D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;
    check(device->CreateDepthStencilState(&depth, &depthState),
          "create depth state");

    const D3D11_VIEWPORT viewport{0, 0, float(kResolution), float(kResolution), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(rasterState.Get());
    context->OMSetRenderTargets(0, nullptr, depthView.Get());
    context->OMSetDepthStencilState(depthState.Get(), 0);
    context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH,
                                   maximum ? 0.0f : 1.0f, 0);
    context->IASetInputLayout(layout);
    const unsigned stride = sizeof(Vertex), offset = 0;
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader, nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->Draw(vertexCount, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->CopyResource(readback.Get(), target.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped),
          "map depth readback");
    std::vector<float> result(std::size_t(kResolution) * kResolution);
    for (unsigned y = 0; y != kResolution; ++y)
        std::memcpy(result.data() + std::size_t(y) * kResolution,
                    static_cast<const std::uint8_t *>(mapped.pData) +
                        std::size_t(y) * mapped.RowPitch,
                    kResolution * sizeof(float));
    context->Unmap(readback.Get(), 0);
    return result;
}

void writeDepth(const std::filesystem::path &path, const std::vector<float> &values)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.write(reinterpret_cast<const char *>(values.data()),
                      static_cast<std::streamsize>(values.size() * sizeof(float))))
        throw std::runtime_error("cannot write raster depth " + path.string());
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: css-raster <projected-triangles.bin> <output-directory> "
                     "<signed-slope-bias>\n";
        return 2;
    }
    try
    {
        const auto vertices = readTriangles(argv[1]);
        if (vertices.empty())
            throw std::runtime_error("empty projected triangle list");
        const float slopeBias = std::stof(argv[3]);
        if (!std::isfinite(slopeBias))
            throw std::runtime_error("nonfinite slope bias");

        constexpr char source[] = R"(
struct Input { float3 position : POSITION; };
float4 main(Input input) : SV_POSITION
{
    return float4(input.position.x / 256.0 - 1.0,
                  1.0 - input.position.y / 256.0,
                  input.position.z, 1.0);
})";
        ComPtr<ID3DBlob> code, errors;
        const HRESULT compile = D3DCompile(source, sizeof(source) - 1, nullptr,
                                            nullptr, nullptr, "main", "vs_5_0",
                                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                            &code, &errors);
        if (FAILED(compile))
            throw std::runtime_error(errors
                ? std::string(static_cast<const char *>(errors->GetBufferPointer()),
                              errors->GetBufferSize())
                : "depth vertex shader compilation failed");

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL featureLevel{};
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                nullptr, 0, D3D11_SDK_VERSION, &device,
                                &featureLevel, &context), "create D3D11 WARP device");
        ComPtr<ID3D11VertexShader> vertexShader;
        check(device->CreateVertexShader(code->GetBufferPointer(),
                                         code->GetBufferSize(), nullptr,
                                         &vertexShader), "create depth vertex shader");
        const D3D11_INPUT_ELEMENT_DESC element{
            "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
            D3D11_INPUT_PER_VERTEX_DATA, 0};
        ComPtr<ID3D11InputLayout> layout;
        check(device->CreateInputLayout(&element, 1, code->GetBufferPointer(),
                                        code->GetBufferSize(), &layout),
              "create projected triangle layout");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = static_cast<unsigned>(vertices.size() * sizeof(Vertex));
        buffer.Usage = D3D11_USAGE_IMMUTABLE;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{vertices.data(), 0, 0};
        ComPtr<ID3D11Buffer> vertexBuffer;
        check(device->CreateBuffer(&buffer, &data, &vertexBuffer),
              "create projected triangle buffer");

        auto front = drawDepth(device.Get(), context.Get(), vertexShader.Get(),
                               layout.Get(), vertexBuffer.Get(),
                               static_cast<unsigned>(vertices.size()),
                               slopeBias, true);
        auto back = drawDepth(device.Get(), context.Get(), vertexShader.Get(),
                              layout.Get(), vertexBuffer.Get(),
                              static_cast<unsigned>(vertices.size()),
                              slopeBias, false);
        std::size_t covered = 0;
        for (std::size_t pixel = 0; pixel != front.size(); ++pixel)
            if (front[pixel] <= 0)
                back[pixel] = 0;
            else
                ++covered;

        const std::filesystem::path output(argv[2]);
        std::filesystem::create_directories(output);
        writeDepth(output / "t0-max.r32", front);
        writeDepth(output / "t1-min.r32", back);
        std::cout << "WARP rasterized " << vertices.size() / 3
                  << " projected triangles, " << covered << " covered pixels, "
                  << "slope bias " << slopeBias << "\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
