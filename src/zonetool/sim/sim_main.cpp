// iw8-zonetool sim: command line, local web viewer, panoramas, check.
#include "sim/sim.h"
#include "sim/sim_render.h"
#include "sim/sim_viewer_html.h"

#include "common/fs_util.h"
#include "common/json.hpp"
#include "common/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32")

namespace sim
{
using zt::path_join;
using zt::read_file;
using zt::read_file_str;
using zt::write_file;
using zt::write_file_str;
namespace
{
using Json = nlohmann::json;

struct Options
{
    std::string simJson, command = "render", output;
    Camera camera;
    bool cameraSet = false;
    int spawn = -1;
    RenderOptions render;
    int port = 8765;
    int quality = 88;
    unsigned panoramaSize = 1024;
    std::string points = "spawns";
    std::string package;
    std::string label;
    unsigned pickX{}, pickY{};
    std::string authFile; // "user:password" for HTTP basic auth on serve
};

View ParseView(const std::string &name)
{
    static const std::map<std::string, View> views = {
        {"exact", View::exact},       {"albedo", View::albedo},   {"normals", View::normals},
        {"surface", View::surfaceId}, {"material", View::materialId}, {"lightmap", View::lightmap},
        {"sun", View::sunVisibility}, {"cull", View::cullClass},  {"depth", View::depth}};
    const auto found = views.find(name);
    return found == views.end() ? View::exact : found->second;
}

bool Parse(const int argc, char **argv, Options &options)
{
    if (argc < 1)
        return false;
    options.simJson = argv[0];
    int index = 1;
    if (index < argc && argv[index][0] != '-')
        options.command = argv[index++];
    for (; index < argc; ++index)
    {
        const std::string value = argv[index];
        const auto next = [&]() -> std::string {
            if (index + 1 >= argc)
                throw std::runtime_error("sim: missing value after " + value);
            return argv[++index];
        };
        if (value == "-o" || value == "--out")
            options.output = next();
        else if (value == "--camera")
        {
            // x,y,z,yaw,pitch[,fov]
            std::string text = next();
            std::replace(text.begin(), text.end(), ',', ' ');
            std::istringstream in(text);
            in >> options.camera.origin[0] >> options.camera.origin[1] >> options.camera.origin[2] >>
                options.camera.yaw >> options.camera.pitch;
            float fov;
            if (in >> fov)
                options.camera.fov = fov;
            options.cameraSet = true;
        }
        else if (value == "--spawn")
            options.spawn = std::stoi(next());
        else if (value == "--size")
        {
            std::string text = next();
            std::replace(text.begin(), text.end(), 'x', ' ');
            std::istringstream in(text);
            in >> options.render.width >> options.render.height;
        }
        else if (value == "--fov")
            options.camera.fov = std::stof(next());
        else if (value == "--view")
            options.render.view = ParseView(next());
        else if (value == "--exposure")
            options.render.exposure = std::stof(next());
        else if (value == "--flip-winding")
            options.render.flipWinding = true;
        else if (value == "--reverse-order")
            options.render.reverseOrder = true;
        else if (value == "--opaque-only")
            options.render.opaqueOnly = true;
        else if (value == "--gtao")
            options.render.gtao = true;
        else if (value == "--no-world")
            options.render.drawWorld = false;
        else if (value == "--no-models")
            options.render.drawModels = false;
        else if (value == "--no-dynamic")
            options.render.drawDynamic = false;
        else if (value == "--no-glass")
            options.render.drawGlass = false;
        else if (value == "--lod")
            options.render.forceLod = std::stoi(next());
        else if (value == "--port")
            options.port = std::stoi(next());
        else if (value == "--quality")
            options.quality = std::stoi(next());
        else if (value == "--panorama-size")
            options.panoramaSize = static_cast<unsigned>(std::stoul(next()));
        else if (value == "--points")
            options.points = next();
        else if (value == "--package")
            options.package = next();
        else if (value == "--label")
            options.label = next();
        else if (value == "--auth-file")
            options.authFile = next();
        else if (value == "--at")
        {
            std::string text = next();
            std::replace(text.begin(), text.end(), ',', ' ');
            std::istringstream in(text);
            in >> options.pickX >> options.pickY;
        }
        else
            throw std::runtime_error("sim: unknown option " + value);
    }
    return true;
}

Camera SpawnCamera(const Simulator &simulator, const int index)
{
    const auto &spawns = simulator.Spawns();
    Camera camera;
    if (spawns.empty())
    {
        const auto mn = simulator.BoundsMin(), mx = simulator.BoundsMax();
        camera.origin = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
        return camera;
    }
    const Spawn &spawn = spawns[static_cast<size_t>(std::clamp<int>(index, 0, int(spawns.size()) - 1))];
    camera.origin = {spawn.origin[0], spawn.origin[1], spawn.origin[2] + 60};
    camera.yaw = spawn.yaw;
    return camera;
}

std::string UrlDecode(const std::string &text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        }
        else if (text[i] == '+')
            out.push_back(' ');
        else
            out.push_back(text[i]);
    }
    return out;
}
std::map<std::string, std::string> Query(const std::string &target)
{
    std::map<std::string, std::string> out;
    const size_t q = target.find('?');
    if (q == std::string::npos)
        return out;
    std::istringstream in(target.substr(q + 1));
    std::string pair;
    while (std::getline(in, pair, '&'))
    {
        const size_t eq = pair.find('=');
        if (eq != std::string::npos)
            out[pair.substr(0, eq)] = UrlDecode(pair.substr(eq + 1));
    }
    return out;
}
float Get(const std::map<std::string, std::string> &query, const char *key, const float fallback)
{
    const auto found = query.find(key);
    if (found == query.end() || found->second.empty())
        return fallback;
    try
    {
        return std::stof(found->second);
    }
    catch (...)
    {
        return fallback;
    }
}

Json InfoJson(const Simulator &simulator)
{
    Json spawns = Json::array();
    for (const auto &spawn : simulator.Spawns())
        spawns.push_back({{"classname", spawn.classname}, {"origin", spawn.origin}, {"yaw", spawn.yaw}});
    const auto &stats = simulator.LastStats();
    return {{"map", simulator.MapName()},
            {"spawns", spawns},
            {"boundsMin", simulator.BoundsMin()},
            {"boundsMax", simulator.BoundsMax()},
            {"stats",
             {{"worldSurfaces", stats.worldSurfaces},
              {"staticInstances", stats.staticInstances},
              {"staticModels", stats.staticModels},
              {"dynamicEntities", stats.dynamicEntities},
              {"glassPanes", stats.glassPanes},
              {"materials", stats.materials},
              {"pipelines", stats.pipelines},
              {"drawCalls", stats.drawCalls},
              {"triangles", stats.triangles},
              {"frameMs", stats.frameMs}}}};
}

// Equirectangular panorama from six 90-degree faces.
std::vector<uint8_t> Equirect(Simulator &simulator, const Camera &at, const RenderOptions &base,
                              const unsigned faceSize, const unsigned width, const unsigned height)
{
    struct Face
    {
        float yaw, pitch;
        std::vector<uint8_t> rgba;
        float f[3], r[3], u[3];
    };
    std::vector<Face> faces = {{0, 0}, {90, 0}, {180, 0}, {270, 0}, {0, -90}, {0, 90}};
    RenderOptions options = base;
    options.width = options.height = faceSize;
    for (auto &face : faces)
    {
        Camera camera = at;
        camera.yaw = face.yaw;
        camera.pitch = face.pitch;
        camera.fov = 90;
        face.rgba = Simulator::Tonemap(simulator.Render(camera, options), base.exposure);
        const float yaw = face.yaw * std::numbers::pi_v<float> / 180, pitch = face.pitch * std::numbers::pi_v<float> / 180;
        face.f[0] = std::cos(pitch) * std::cos(yaw);
        face.f[1] = std::cos(pitch) * std::sin(yaw);
        face.f[2] = -std::sin(pitch);
        // Same basis as the renderer: right = forward x up, degenerate at the poles.
        float rx = face.f[1], ry = -face.f[0], rz = 0;
        const float length = std::sqrt(rx * rx + ry * ry);
        if (length < 1e-4f)
        {
            rx = std::sin(yaw);
            ry = -std::cos(yaw);
        }
        else
        {
            rx /= length;
            ry /= length;
        }
        face.r[0] = rx; face.r[1] = ry; face.r[2] = rz;
        face.u[0] = ry * face.f[2] - rz * face.f[1];
        face.u[1] = rz * face.f[0] - rx * face.f[2];
        face.u[2] = rx * face.f[1] - ry * face.f[0];
    }
    std::vector<uint8_t> out(size_t(width) * height * 4, 255);
    for (unsigned y = 0; y < height; ++y)
    {
        const float lat = (0.5f - (y + .5f) / height) * std::numbers::pi_v<float>;
        for (unsigned x = 0; x < width; ++x)
        {
            const float lon = ((x + .5f) / width - 0.5f) * 2 * std::numbers::pi_v<float>;
            const float d[3] = {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
            const Face *best = nullptr;
            float bestDot = -2;
            for (const auto &face : faces)
            {
                const float dot = d[0] * face.f[0] + d[1] * face.f[1] + d[2] * face.f[2];
                if (dot > bestDot)
                {
                    bestDot = dot;
                    best = &face;
                }
            }
            const float px = (d[0] * best->r[0] + d[1] * best->r[1] + d[2] * best->r[2]) / bestDot;
            const float py = (d[0] * best->u[0] + d[1] * best->u[1] + d[2] * best->u[2]) / bestDot;
            const float fx = std::clamp((px + 1) * .5f * faceSize - .5f, 0.0f, faceSize - 1.0f);
            const float fy = std::clamp((1 - py) * .5f * faceSize - .5f, 0.0f, faceSize - 1.0f);
            const unsigned x0 = static_cast<unsigned>(fx), y0 = static_cast<unsigned>(fy);
            const unsigned x1 = std::min(x0 + 1, faceSize - 1), y1 = std::min(y0 + 1, faceSize - 1);
            const float tx = fx - x0, ty = fy - y0;
            for (unsigned c = 0; c < 3; ++c)
            {
                const auto sample = [&](const unsigned sx, const unsigned sy) {
                    return float(best->rgba[(size_t(sy) * faceSize + sx) * 4 + c]);
                };
                const float v = (sample(x0, y0) * (1 - tx) + sample(x1, y0) * tx) * (1 - ty) +
                                (sample(x0, y1) * (1 - tx) + sample(x1, y1) * tx) * ty;
                out[(size_t(y) * width + x) * 4 + c] = static_cast<uint8_t>(v + .5f);
            }
        }
    }
    return out;
}

std::vector<Camera> PanoramaPoints(const Simulator &simulator, const std::string &spec)
{
    std::vector<Camera> points;
    if (spec == "spawns")
    {
        // One point per distinct spawn location (the gametype duplicates share origins).
        for (size_t i = 0; i < simulator.Spawns().size(); ++i)
        {
            const Camera camera = SpawnCamera(simulator, static_cast<int>(i));
            const bool duplicate = std::any_of(points.begin(), points.end(), [&](const Camera &other) {
                const float dx = other.origin[0] - camera.origin[0], dy = other.origin[1] - camera.origin[1];
                return dx * dx + dy * dy < 96 * 96;
            });
            if (!duplicate)
                points.push_back(camera);
        }
        return points;
    }
    std::string text = spec;
    std::replace(text.begin(), text.end(), ';', ' ');
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream in(text);
    Camera camera;
    while (in >> camera.origin[0] >> camera.origin[1] >> camera.origin[2])
        points.push_back(camera);
    return points;
}

int Panorama(Simulator &simulator, const Options &options)
{
    if (options.output.empty())
        throw std::runtime_error("sim panorama: pass -o <directory>");
    std::filesystem::create_directories(options.output);
    const auto points = PanoramaPoints(simulator, options.points);
    Json index = {{"map", simulator.MapName()}, {"label", options.label}, {"points", Json::array()}};
    const unsigned width = options.panoramaSize * 4, height = options.panoramaSize * 2;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto rgba = Equirect(simulator, points[i], options.render, options.panoramaSize, width, height);
        const auto jpeg = Simulator::EncodeJpeg(rgba.data(), width, height, options.quality);
        const std::string file = "point_" + std::to_string(i) + ".jpg";
        write_file(path_join(options.output, file), jpeg);
        index["points"].push_back({{"file", file},
                                   {"origin", points[i].origin},
                                   {"yaw", points[i].yaw},
                                   {"name", "Point " + std::to_string(i + 1)}});
        zt::info("sim: panorama %zu/%zu at (%.0f, %.0f, %.0f) in %.1fs (%zu bytes)", i + 1, points.size(),
                 points[i].origin[0], points[i].origin[1], points[i].origin[2],
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), jpeg.size());
    }
    index["boundsMin"] = simulator.BoundsMin();
    index["boundsMax"] = simulator.BoundsMax();
    write_file_str(path_join(options.output, "index.json"), index.dump(2));
    write_file_str(path_join(options.output, "index.html"), kPanoramaHtml);
    return 0;
}

int RenderOnce(Simulator &simulator, Options options)
{
    const Camera camera = options.cameraSet ? options.camera : SpawnCamera(simulator, std::max(options.spawn, 0));
    const Frame frame = simulator.Render(camera, options.render);
    const auto rgba = Simulator::Tonemap(frame, options.render.exposure);
    const std::string output = options.output.empty() ? simulator.MapName() + ".png" : options.output;
    const bool jpeg = output.size() > 4 && (output.ends_with(".jpg") || output.ends_with(".jpeg"));
    const auto encoded = jpeg ? Simulator::EncodeJpeg(rgba.data(), frame.width, frame.height, options.quality)
                              : Simulator::EncodePng(rgba.data(), frame.width, frame.height);
    if (!write_file(output, encoded))
        throw std::runtime_error("sim: cannot write " + output);
    const auto &stats = simulator.LastStats();
    zt::info("sim: wrote %s (%ux%u, camera %.0f %.0f %.0f yaw %.0f pitch %.0f, %u draws, %u triangles, %.0f ms)",
             output.c_str(), frame.width, frame.height, camera.origin[0], camera.origin[1], camera.origin[2],
             camera.yaw, camera.pitch, stats.drawCalls, stats.triangles, stats.frameMs);
    return 0;
}

// ---- minimal HTTP server -----------------------------------------------
struct Request
{
    std::string method, target, authorization;
};
std::string Base64Decode(const std::string &text)
{
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned buffer = 0, bits = 0;
    for (const char c : text)
    {
        const size_t value = alphabet.find(c);
        if (value == std::string::npos)
            break;
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}
// Constant-time comparison for the basic-auth secret.
bool SameSecret(const std::string &a, const std::string &b)
{
    unsigned difference = static_cast<unsigned>(a.size() ^ b.size());
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        difference |= static_cast<unsigned>((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    return difference == 0;
}
bool ReadRequest(const SOCKET client, Request &request)
{
    std::string buffer;
    char chunk[4096];
    while (buffer.find("\r\n\r\n") == std::string::npos)
    {
        const int got = recv(client, chunk, sizeof(chunk), 0);
        if (got <= 0)
            return false;
        buffer.append(chunk, static_cast<size_t>(got));
        if (buffer.size() > 65536)
            return false;
    }
    std::istringstream line(buffer.substr(0, buffer.find("\r\n")));
    line >> request.method >> request.target;
    std::istringstream headers(buffer.substr(buffer.find("\r\n") + 2));
    std::string header;
    while (std::getline(headers, header))
    {
        if (!header.empty() && header.back() == '\r')
            header.pop_back();
        std::string lower = header;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.rfind("authorization:", 0) == 0)
        {
            request.authorization = header.substr(14);
            request.authorization.erase(0, request.authorization.find_first_not_of(' '));
        }
    }
    return !request.method.empty();
}
void Respond(const SOCKET client, const char *status, const char *type, const void *body, const size_t size)
{
    std::string header = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
                         "\r\nContent-Length: " + std::to_string(size) +
                         "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\n";
    send(client, header.data(), static_cast<int>(header.size()), 0);
    size_t sent = 0;
    while (sent < size)
    {
        const int n = send(client, static_cast<const char *>(body) + sent, static_cast<int>(std::min<size_t>(size - sent, 1 << 16)), 0);
        if (n <= 0)
            break;
        sent += static_cast<size_t>(n);
    }
}

// Viewer request parameters shared by /frame, /pick and /cam.
void ApplyViewQuery(const std::map<std::string, std::string> &query, const Options &options, Camera &camera,
                    RenderOptions &render)
{
    camera.origin[0] = Get(query, "x", camera.origin[0]);
    camera.origin[1] = Get(query, "y", camera.origin[1]);
    camera.origin[2] = Get(query, "z", camera.origin[2]);
    camera.yaw = Get(query, "yaw", camera.yaw);
    camera.pitch = Get(query, "pitch", camera.pitch);
    camera.fov = Get(query, "fov", camera.fov);
    render.width = static_cast<unsigned>(std::clamp(Get(query, "w", 1280.0f), 64.0f, 3840.0f));
    render.height = static_cast<unsigned>(std::clamp(Get(query, "h", 720.0f), 64.0f, 2160.0f));
    render.exposure = Get(query, "exposure", render.exposure);
    render.forceLod = static_cast<int>(Get(query, "lod", -1));
    if (const auto view = query.find("view"); view != query.end())
        render.view = ParseView(view->second);
    render.drawWorld = Get(query, "world", 1) != 0;
    render.drawModels = Get(query, "models", 1) != 0;
    render.drawDynamic = Get(query, "dynamic", 1) != 0;
    render.drawGlass = Get(query, "glass", 1) != 0;
    render.flipWinding = Get(query, "flip", options.render.flipWinding ? 1.0f : 0.0f) != 0;
    render.gtao = Get(query, "gtao", options.render.gtao ? 1.0f : 0.0f) != 0;
}

// One rendered JPEG of the current view (the simulator lock must be held).
std::vector<uint8_t> RenderJpeg(Simulator &simulator, const Camera &camera, RenderOptions render, const int quality)
{
    render.gpuTonemap = true;
    const Frame frame = simulator.Render(camera, render);
    if (!frame.rgba8.empty())
        return Simulator::EncodeJpeg(frame.rgba8.data(), frame.width, frame.height, quality);
    const auto rgba = Simulator::Tonemap(frame, render.exposure);
    return Simulator::EncodeJpeg(rgba.data(), frame.width, frame.height, quality);
}

// Live viewer. Every connection gets its own thread. The page keeps one
// /stream response open (multipart JPEG frames) and posts camera changes to
// /cam; a single render thread owns the GPU and renders the latest requested
// view whenever it changes, so frame throughput no longer waits for a network
// round trip per frame (the remote tunnel adds most of a second to each).
int Serve(Simulator &simulator, const Options &options)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        throw std::runtime_error("sim: WSAStartup failed");
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(options.port));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    const BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener, 64))
        throw std::runtime_error("sim: cannot listen on port " + std::to_string(options.port));
    std::string secret;
    if (!options.authFile.empty())
    {
        if (!read_file_str(options.authFile, secret))
            throw std::runtime_error("sim: cannot read the auth file");
        while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r' || secret.back() == ' '))
            secret.pop_back();
        if (secret.find(':') == std::string::npos)
            throw std::runtime_error("sim: the auth file must hold user:password");
    }

    struct Shared
    {
        std::mutex mutex;
        std::condition_variable changed;
        Camera camera;
        RenderOptions render;
        int quality = 80;
        uint64_t requested = 1, rendered = 0; // view versions
        std::shared_ptr<const std::vector<uint8_t>> jpeg;
        uint64_t frameId = 0;
        double renderMs = 0;
        int streams = 0;
        double lastSeq = -1; // /cam updates may overtake each other in flight
    };
    auto shared = std::make_shared<Shared>();
    shared->camera = options.cameraSet ? options.camera : SpawnCamera(simulator, 0);
    shared->render = options.render;
    shared->render.width = 960;
    shared->render.height = 540;
    std::mutex simulatorLock; // one D3D11 immediate context

    std::thread([&simulator, &simulatorLock, shared]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            Camera camera;
            RenderOptions render;
            int quality;
            uint64_t version;
            {
                std::unique_lock lock(shared->mutex);
                shared->changed.wait(lock, [&] { return shared->streams > 0 && shared->requested != shared->rendered; });
                camera = shared->camera;
                render = shared->render;
                quality = shared->quality;
                version = shared->requested;
            }
            std::shared_ptr<const std::vector<uint8_t>> jpeg;
            const auto t0 = std::chrono::steady_clock::now();
            try
            {
                std::lock_guard simulatorGuard(simulatorLock);
                jpeg = std::make_shared<const std::vector<uint8_t>>(RenderJpeg(simulator, camera, render, quality));
            }
            catch (const std::exception &error)
            {
                zt::warn("sim: viewer frame failed: %s", error.what());
            }
            {
                std::lock_guard lock(shared->mutex);
                if (jpeg)
                {
                    shared->jpeg = std::move(jpeg);
                    ++shared->frameId;
                    shared->renderMs =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                }
                shared->rendered = version;
            }
            shared->changed.notify_all();
        }
    }).detach();

    zt::info("sim: viewer at http://localhost:%d/  (free camera: WASD + mouse, Q/E up/down, Shift fast)%s",
             options.port, secret.empty() ? "" : ", HTTP basic auth required");
    for (;;)
    {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;
        std::thread([&simulator, &simulatorLock, &options, shared, secret, client]() {
            const BOOL noDelay = TRUE;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            // Keep-alive: one connection carries many /cam requests (a tunnel
            // round trip per new connection otherwise).
            for (bool keep = true; keep;)
            {
            Request request;
            if (!ReadRequest(client, request))
                break;
            if (!secret.empty())
            {
                const bool basic = request.authorization.rfind("Basic ", 0) == 0;
                if (!basic || !SameSecret(Base64Decode(request.authorization.substr(6)), secret))
                {
                    Sleep(400); // slow down guessing
                    const char *body = "authentication required";
                    const std::string header =
                        std::string("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"map simulator\", "
                                    "charset=\"UTF-8\"\r\nContent-Type: text/plain\r\nContent-Length: ") +
                        std::to_string(std::strlen(body)) + "\r\nConnection: close\r\n\r\n" + body;
                    send(client, header.data(), static_cast<int>(header.size()), 0);
                    break;
                }
            }
            try
            {
                const std::string path = request.target.substr(0, request.target.find('?'));
                const auto query = Query(request.target);
                if (path == "/")
                    Respond(client, "200 OK", "text/html; charset=utf-8", kViewerHtml, std::strlen(kViewerHtml));
                else if (path == "/info")
                {
                    std::string body;
                    {
                        std::lock_guard guard(simulatorLock);
                        body = InfoJson(simulator).dump();
                    }
                    Respond(client, "200 OK", "application/json", body.data(), body.size());
                }
                else if (path == "/cam")
                {
                    {
                        std::lock_guard lock(shared->mutex);
                        const double seq = Get(query, "seq", -1);
                        if (seq < 0 || seq > shared->lastSeq)
                        {
                            shared->lastSeq = seq < 0 ? shared->lastSeq : seq;
                            ApplyViewQuery(query, options, shared->camera, shared->render);
                            shared->quality = static_cast<int>(std::clamp(Get(query, "q", 80), 20.0f, 100.0f));
                            ++shared->requested;
                        }
                    }
                    shared->changed.notify_all();
                    std::string body;
                    {
                        std::lock_guard lock(shared->mutex);
                        body = "{\"frame\":" + std::to_string(shared->frameId) +
                               ",\"renderMs\":" + std::to_string(shared->renderMs) + "}";
                    }
                    Respond(client, "200 OK", "application/json", body.data(), body.size());
                }
                else if (path == "/stream")
                {
                    const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                                               "Cache-Control: no-store\r\nX-Accel-Buffering: no\r\nConnection: close\r\n\r\n";
                    bool open = send(client, header.data(), static_cast<int>(header.size()), 0) > 0;
                    uint64_t sent = 0;
                    {
                        std::lock_guard lock(shared->mutex);
                        ++shared->streams;
                        ++shared->requested; // a fresh frame for the new viewer
                    }
                    shared->changed.notify_all();
                    while (open)
                    {
                        std::shared_ptr<const std::vector<uint8_t>> jpeg;
                        {
                            std::unique_lock lock(shared->mutex);
                            // Wake at least every 10 s: an empty part keeps idle
                            // tunnels from closing the response.
                            shared->changed.wait_for(lock, std::chrono::seconds(10),
                                                     [&] { return shared->frameId != sent; });
                            if (shared->frameId != sent)
                            {
                                jpeg = shared->jpeg;
                                sent = shared->frameId;
                            }
                        }
                        const std::string part =
                            jpeg ? "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg->size()) +
                                       "\r\nX-Frame: " + std::to_string(sent) + "\r\n\r\n"
                                 : std::string("--frame\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n\r\n");
                        open = send(client, part.data(), static_cast<int>(part.size()), 0) > 0;
                        size_t offset = 0;
                        while (open && jpeg && offset < jpeg->size())
                        {
                            const int n = send(client, reinterpret_cast<const char *>(jpeg->data()) + offset,
                                               static_cast<int>(std::min<size_t>(jpeg->size() - offset, 1 << 16)), 0);
                            open = n > 0;
                            offset += n > 0 ? static_cast<size_t>(n) : 0;
                        }
                        if (open && jpeg)
                            open = send(client, "\r\n", 2, 0) > 0;
                    }
                    std::lock_guard lock(shared->mutex);
                    --shared->streams;
                    keep = false;
                }
                else if (path == "/frame" || path == "/pick")
                {
                    Camera camera = options.cameraSet ? options.camera : SpawnCamera(simulator, 0);
                    RenderOptions render = options.render;
                    ApplyViewQuery(query, options, camera, render);
                    if (path == "/pick")
                    {
                        std::string body;
                        {
                            std::lock_guard guard(simulatorLock);
                            body = simulator.Describe(camera, render, static_cast<unsigned>(Get(query, "px", 0)),
                                                      static_cast<unsigned>(Get(query, "py", 0)));
                        }
                        Respond(client, "200 OK", "application/json", body.data(), body.size());
                    }
                    else
                    {
                        std::vector<uint8_t> jpeg;
                        {
                            std::lock_guard guard(simulatorLock);
                            jpeg = RenderJpeg(simulator, camera, render,
                                              static_cast<int>(Get(query, "q", float(options.quality))));
                        }
                        Respond(client, "200 OK", "image/jpeg", jpeg.data(), jpeg.size());
                    }
                }
                else if (path == "/check")
                {
                    std::string report;
                    if (options.package.empty())
                        report = "{\"error\":\"start the viewer with --package <built map directory> to run checks\"}";
                    else
                    {
                        const std::string command = "python -I \"" + path_join(ToolsDirectory(), "mapcheck.py") +
                                                    "\" \"" + options.package + "\" --json";
                        FILE *pipe = _popen(command.c_str(), "r");
                        char chunk[4096];
                        while (pipe && std::fgets(chunk, sizeof(chunk), pipe))
                            report += chunk;
                        if (pipe)
                            _pclose(pipe);
                        if (report.empty())
                            report = "{\"error\":\"mapcheck produced no output\"}";
                    }
                    Respond(client, "200 OK", "application/json", report.data(), report.size());
                }
                else
                    Respond(client, "404 Not Found", "text/plain", "not found", 9);
            }
            catch (const std::exception &error)
            {
                const std::string body = std::string("{\"error\":\"") + error.what() + "\"}";
                Respond(client, "500 Internal Server Error", "application/json", body.data(), body.size());
            }
            }
            closesocket(client);
        }).detach();
    }
}
} // namespace

std::string ToolsDirectory()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    // xmake-out/x64/Release/iw8-zonetool.exe -> <repo>/tools
    std::filesystem::path exe(path);
    for (auto directory = exe.parent_path(); !directory.empty(); directory = directory.parent_path())
    {
        if (std::filesystem::is_directory(directory / "tools") && std::filesystem::exists(directory / "xmake.lua"))
            return (directory / "tools").string();
        if (directory == directory.parent_path())
            break;
    }
    return exe.parent_path().string();
}

// Depth-tie (flicker) sweep: every view is drawn in normal and reversed order;
// a pixel whose surface changes is a tie the game breaks by draw order.
int Flicker(Simulator &simulator, const Options &options)
{
    std::vector<Camera> points;
    if (options.cameraSet)
        points.push_back(options.camera);
    else
        points = PanoramaPoints(simulator, options.points);
    RenderOptions render = options.render;
    render.width = 960;
    render.height = 540;
    render.view = View::surfaceId;
    struct Pair
    {
        uint64_t pixels{};
        unsigned views{};
        Camera example;
        unsigned px{}, py{};
        uint64_t best{};
        std::string modelA, modelB;
    };
    std::map<std::pair<uint32_t, uint32_t>, Pair> pairs;
    uint64_t totalPixels = 0, tiedPixels = 0;
    unsigned viewCount = 0;
    for (const Camera &point : points)
        for (const float pitch : {options.cameraSet ? point.pitch : 0.0f, options.cameraSet ? point.pitch : 25.0f})
            for (int heading = 0; heading < (options.cameraSet ? 1 : 8); ++heading)
            {
                Camera camera = point;
                camera.pitch = pitch;
                if (!options.cameraSet)
                    camera.yaw = heading * 45.0f;
                render.reverseOrder = false;
                const Frame forward = simulator.Render(camera, render);
                render.reverseOrder = true;
                const Frame reverse = simulator.Render(camera, render);
                ++viewCount;
                std::map<std::pair<uint32_t, uint32_t>, uint64_t> local;
                std::map<std::pair<uint32_t, uint32_t>, std::pair<unsigned, unsigned>> where;
                std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> instances;
                for (unsigned y = 0; y < render.height; ++y)
                    for (unsigned x = 0; x < render.width; ++x)
                    {
                        const size_t i = (size_t(y) * render.width + x) * 2;
                        ++totalPixels;
                        if (forward.ids[i] == reverse.ids[i] && forward.ids[i + 1] == reverse.ids[i + 1])
                            continue;
                        if (!forward.ids[i] || !reverse.ids[i])
                            continue;
                        ++tiedPixels;
                        const auto key = std::minmax(forward.ids[i], reverse.ids[i]);
                        ++local[key];
                        where.try_emplace(key, x, y);
                        instances.try_emplace(key, forward.ids[i] <= reverse.ids[i] ? forward.ids[i + 1] : reverse.ids[i + 1],
                                              forward.ids[i] <= reverse.ids[i] ? reverse.ids[i + 1] : forward.ids[i + 1]);
                    }
                for (const auto &[key, count] : local)
                {
                    Pair &pair = pairs[key];
                    pair.pixels += count;
                    ++pair.views;
                    if (count > pair.best)
                    {
                        pair.best = count;
                        pair.example = camera;
                        pair.px = where[key].first;
                        pair.py = where[key].second;
                        pair.modelA = simulator.InstanceModel(instances[key].first);
                        pair.modelB = simulator.InstanceModel(instances[key].second);
                    }
                }
            }
    std::vector<std::pair<std::pair<uint32_t, uint32_t>, Pair>> sorted(pairs.begin(), pairs.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second.pixels > b.second.pixels; });
    Json report = {{"map", simulator.MapName()}, {"views", viewCount}, {"pixels", totalPixels},
                   {"tiedPixels", tiedPixels}, {"pairs", Json::array()}};
    for (const auto &[key, pair] : sorted)
    {
        if (pair.pixels < 16)
            continue;
        const MaterialInfo a = simulator.Material(key.first), b = simulator.Material(key.second);
        const char *kind = key.first == key.second ? "same material"
                           : a.opaque && b.opaque  ? "opaque/opaque"
                                                   : "involves translucent/decal";
        report["pairs"].push_back(
            {{"kind", kind},
             {"pixels", pair.pixels},
             {"views", pair.views},
             {"a", {{"material", a.name}, {"techset", a.techset}, {"opaque", a.opaque}, {"model", pair.modelA}}},
             {"b", {{"material", b.name}, {"techset", b.techset}, {"opaque", b.opaque}, {"model", pair.modelB}}},
             {"example", {{"camera", {pair.example.origin[0], pair.example.origin[1], pair.example.origin[2],
                                      pair.example.yaw, pair.example.pitch}},
                          {"pixel", {pair.px, pair.py}}, {"size", {render.width, render.height}}}}});
    }
    const std::string text = report.dump(2);
    if (!options.output.empty())
        write_file_str(options.output, text);
    zt::info("sim flicker: %u views, %.3f%% of pixels change owner with draw order, %zu material pairs",
             viewCount, totalPixels ? 100.0 * double(tiedPixels) / double(totalPixels) : 0.0,
             report["pairs"].size());
    for (size_t i = 0; i < std::min<size_t>(report["pairs"].size(), 15); ++i)
    {
        const Json &pair = report["pairs"][i];
        zt::info("  %8llu px %3u views  %-26s %s [%s] <-> %s [%s]", pair["pixels"].get<unsigned long long>(),
                 pair["views"].get<unsigned>(), pair["kind"].get<std::string>().c_str(),
                 pair["a"]["techset"].get<std::string>().c_str(), pair["a"]["model"].get<std::string>().c_str(),
                 pair["b"]["techset"].get<std::string>().c_str(), pair["b"]["model"].get<std::string>().c_str());
    }
    return 0;
}

// Rotation invariance: with the camera position fixed, a world point is seen
// along the same ray whatever the view rotation, so its shading must not change
// (the simulator's engine inputs are view-independent). View B is rotated from
// view A; each B pixel is mapped back to A through the pure rotation (no depth
// needed) and coherent patches whose colour changes are reported by material.
struct Basis
{
    float forward[3], right[3], up[3], sx, sy;
};
Basis CameraBasis(const Camera &camera, const unsigned width, const unsigned height)
{
    constexpr float pi = 3.14159265358979f;
    const float yaw = camera.yaw * pi / 180, pitch = camera.pitch * pi / 180;
    Basis b{};
    b.forward[0] = std::cos(pitch) * std::cos(yaw);
    b.forward[1] = std::cos(pitch) * std::sin(yaw);
    b.forward[2] = -std::sin(pitch);
    // right = normalize(forward x Z), up = right x forward (as BuildView).
    float rx = b.forward[1], ry = -b.forward[0];
    const float rl = std::sqrt(rx * rx + ry * ry);
    b.right[0] = rx / rl;
    b.right[1] = ry / rl;
    b.right[2] = 0;
    b.up[0] = b.right[1] * b.forward[2] - b.right[2] * b.forward[1];
    b.up[1] = b.right[2] * b.forward[0] - b.right[0] * b.forward[2];
    b.up[2] = b.right[0] * b.forward[1] - b.right[1] * b.forward[0];
    const float tanHalf = std::tan(camera.fov * pi / 360);
    b.sx = 1 / tanHalf;
    b.sy = (float(width) / float(height)) / tanHalf;
    return b;
}

int Rotation(Simulator &simulator, const Options &options)
{
    std::vector<Camera> points;
    if (options.cameraSet)
        points.push_back(options.camera);
    else
        points = PanoramaPoints(simulator, options.points);
    RenderOptions render = options.render;
    render.width = 640;
    render.height = 360;
    const unsigned w = render.width, h = render.height;
    const float threshold = 0.08f;
    struct Hit
    {
        uint64_t pixels{};
        unsigned views{};
        uint64_t best{};
        Camera a, b;
    };
    std::map<uint32_t, Hit> hits;
    uint64_t compared = 0, flaggedTotal = 0;
    unsigned tests = 0;
    const std::array<std::array<float, 2>, 3> rotations = {{{20, 0}, {0, 20}, {0, -20}}};
    for (const Camera &point : points)
        for (int heading = 0; heading < (options.cameraSet ? 1 : 8); ++heading)
            for (const auto &rotation : rotations)
            {
                Camera a = point;
                if (!options.cameraSet)
                {
                    a.yaw = heading * 45.0f;
                    a.pitch = 0;
                }
                Camera b = a;
                b.yaw += rotation[0];
                b.pitch += rotation[1];
                render.view = View::exact;
                const Frame fa = simulator.Render(a, render);
                const Frame fb = simulator.Render(b, render);
                render.view = View::materialId;
                const Frame ib = simulator.Render(b, render);
                ++tests;
                // Compare what is seen: tonemapped 8-bit colour, as on screen.
                const std::vector<uint8_t> ta = Simulator::Tonemap(fa, render.exposure),
                                           tb = Simulator::Tonemap(fb, render.exposure);
                const Basis ba = CameraBasis(a, w, h), bb = CameraBasis(b, w, h);
                std::vector<uint8_t> flagged(size_t(w) * h, 0);
                for (unsigned y = 1; y + 1 < h; ++y)
                    for (unsigned x = 1; x + 1 < w; ++x)
                    {
                        const float nx = (x + 0.5f) / w * 2 - 1, ny = 1 - (y + 0.5f) / h * 2;
                        float d[3];
                        for (int k = 0; k < 3; ++k)
                            d[k] = bb.forward[k] + bb.right[k] * (nx / bb.sx) + bb.up[k] * (ny / bb.sy);
                        const float fz = d[0] * ba.forward[0] + d[1] * ba.forward[1] + d[2] * ba.forward[2];
                        if (fz <= 0)
                            continue;
                        const float ax = (d[0] * ba.right[0] + d[1] * ba.right[1] + d[2] * ba.right[2]) * ba.sx / fz;
                        const float ay = (d[0] * ba.up[0] + d[1] * ba.up[1] + d[2] * ba.up[2]) * ba.sy / fz;
                        const int px = int(std::floor((ax + 1) * 0.5f * w)), py = int(std::floor((1 - ay) * 0.5f * h));
                        if (px < 1 || py < 1 || px + 1 >= int(w) || py + 1 >= int(h))
                            continue;
                        const uint8_t *pb = tb.data() + (size_t(y) * w + x) * 4;
                        ++compared;
                        // Smallest relative difference over A's 3x3 neighbourhood
                        // (absorbs resampling at edges and texture filtering).
                        float best = 1e9f;
                        for (int oy = -1; oy <= 1; ++oy)
                            for (int ox = -1; ox <= 1; ++ox)
                            {
                                const uint8_t *pa = ta.data() + (size_t(py + oy) * w + (px + ox)) * 4;
                                float diff = 0;
                                for (int c = 0; c < 3; ++c)
                                    diff = std::max(diff, std::abs(float(pa[c]) - float(pb[c])) /
                                                              std::max({float(pa[c]), float(pb[c]), 5.0f}));
                                best = std::min(best, diff);
                            }
                        if (best > threshold)
                            flagged[size_t(y) * w + x] = 1;
                    }
                std::map<uint32_t, uint64_t> local;
                for (unsigned y = 1; y + 1 < h; ++y)
                    for (unsigned x = 1; x + 1 < w; ++x)
                    {
                        if (!flagged[size_t(y) * w + x])
                            continue;
                        int neighbours = 0;
                        for (int oy = -1; oy <= 1; ++oy)
                            for (int ox = -1; ox <= 1; ++ox)
                                neighbours += flagged[size_t(y + oy) * w + (x + ox)];
                        if (neighbours < 7) // coherent patches only
                            continue;
                        ++flaggedTotal;
                        ++local[ib.ids[(size_t(y) * w + x) * 2]];
                    }
                for (const auto &[material, count] : local)
                {
                    Hit &hit = hits[material];
                    hit.pixels += count;
                    ++hit.views;
                    if (count > hit.best)
                    {
                        hit.best = count;
                        hit.a = a;
                        hit.b = b;
                    }
                }
            }
    std::vector<std::pair<uint32_t, Hit>> sorted(hits.begin(), hits.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &l, const auto &r) { return l.second.pixels > r.second.pixels; });
    Json report = {{"map", simulator.MapName()}, {"tests", tests}, {"compared", compared},
                   {"flagged", flaggedTotal}, {"threshold", threshold}, {"materials", Json::array()}};
    const auto cameraJson = [](const Camera &c) {
        return Json::array({c.origin[0], c.origin[1], c.origin[2], c.yaw, c.pitch});
    };
    for (const auto &[material, hit] : sorted)
    {
        if (hit.pixels < 64)
            continue;
        const MaterialInfo info = simulator.Material(material);
        report["materials"].push_back({{"material", info.name}, {"techset", info.techset},
                                       {"translucent", info.translucent}, {"pixels", hit.pixels},
                                       {"views", hit.views}, {"worstA", cameraJson(hit.a)},
                                       {"worstB", cameraJson(hit.b)}, {"worstPixels", hit.best}});
    }
    if (!options.output.empty())
        write_file_str(options.output, report.dump(2));
    zt::info("sim rotation: %u rotation tests, %.3f%% of compared pixels change shading with view rotation",
             tests, compared ? 100.0 * double(flaggedTotal) / double(compared) : 0.0);
    for (size_t i = 0; i < std::min<size_t>(report["materials"].size(), 15); ++i)
    {
        const Json &m = report["materials"][i];
        zt::info("  %9llu px %4u tests  %s", m["pixels"].get<unsigned long long>(), m["views"].get<unsigned>(),
                 m["techset"].get<std::string>().c_str());
    }
    return 0;
}

int Main(const int argc, char **argv)
{
    Options options;
    try
    {
        if (!Parse(argc, argv, options))
        {
            std::fprintf(stderr,
                         "usage: iw8-zonetool sim <map.sim.json> [render|info|panorama|serve|check] [options]\n"
                         "  render   -o <file.png|jpg> [--camera x,y,z,yaw,pitch[,fov]] [--spawn N] [--size WxH]\n"
                         "           [--view exact|material|surface|cull|normals|depth] [--exposure E] [--lod N]\n"
                         "  panorama -o <dir> [--points spawns|x,y,z;...] [--panorama-size 1024] [--quality 88]\n"
                         "  serve    [--port 8765] [--package <built map dir>] [--auth-file <user:password file>]\n"
                         "  check    --package <built map dir>\n"
                         "  flicker  [-o report.json] [--points spawns|x,y,z;...] [--camera ...]  (draw-order depth ties)\n"
                         "  rotation [-o report.json] [--points ...] [--camera ...]  (shading that changes with view rotation)\n"
                         "  audit    [-o report.json]  (undrawn surfaces, images and inputs the map data lacks)\n"
                         "  info\n"
                         "  common: --no-world --no-models --no-dynamic --no-glass --flip-winding\n");
            return 2;
        }
        if (options.command == "check")
        {
            if (options.package.empty())
                throw std::runtime_error("sim check: pass --package <built map directory>");
            const std::string command = "python -I \"" + path_join(ToolsDirectory(), "mapcheck.py") + "\" \"" +
                                        options.package + "\"";
            return std::system(command.c_str());
        }
        Simulator simulator;
        simulator.Load(options.simJson);
        if (options.command == "info")
        {
            std::printf("%s\n", InfoJson(simulator).dump(2).c_str());
            return 0;
        }
        if (options.command == "render")
            return RenderOnce(simulator, options);
        if (options.command == "pick")
        {
            // pick --camera ... --size WxH --at px,py
            const Camera camera = options.cameraSet ? options.camera : SpawnCamera(simulator, std::max(options.spawn, 0));
            const std::string description = simulator.Describe(camera, options.render, options.pickX, options.pickY);
            std::puts(description.c_str());
            return 0;
        }
        if (options.command == "panorama")
            return Panorama(simulator, options);
        if (options.command == "flicker")
            return Flicker(simulator, options);
        if (options.command == "rotation")
            return Rotation(simulator, options);
        if (options.command == "audit")
        {
            const std::string report = simulator.Audit();
            if (!options.output.empty())
                write_file_str(options.output, report);
            std::puts(report.c_str());
            return 0;
        }
        if (options.command == "serve")
            return Serve(simulator, options);
        throw std::runtime_error("sim: unknown command " + options.command);
    }
    catch (const std::exception &error)
    {
        zt::err("sim: %s", error.what());
        return 1;
    }
}
} // namespace sim
