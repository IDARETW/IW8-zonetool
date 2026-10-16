#include "iw3_fastfile.h"
#include "iw3_lightgrid.h"
#include "iw3_render_assets.h"
#include "iw3_scripts.h"
#include "iw3_sound.h"
#include "iw3_vision.h"

#include "../convert/maps_convert.h"
#include "../convert/xsurface_convert.h"
#include "../dumpsrc/xse_dump.h"

#include "common/fs_util.h"
#include "common/json.hpp"
#include "common/log.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace iw3
{
namespace
{
using Json = nlohmann::json;
using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;

struct Vertex
{
    Vec3 position{};
    Vec3 normal{};
    Vec3 tangent{};
    Vec2 uv{};
    Vec2 lightmapUv{};
    float binormalSign{1.0f};
    bool authoredTangent{};
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
};

struct Surface
{
    std::string material;
    int lightmap{-1};
    std::uint16_t reflectionProbe{};
    std::uint32_t visibilityGroup{UINT32_MAX};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    // Source bone per vertex for skinned entity models (empty: one root bone).
    std::vector<std::uint8_t> vertexBones;
};

struct BrushModel
{
    Vec3 minimum{};
    Vec3 maximum{};
    std::vector<Surface> surfaces;
};

struct SourceStaticModelLod
{
    float distance{};
    std::vector<Surface> surfaces;
};

// A bind-pose bone from an IW3 XMODEL_EXPORT: model-space origin and axes.
struct SourceBone
{
    std::string name;
    int parent{-1};
    Vec3 origin{};
    std::array<Vec3, 3> axis{};
};

struct SourceStaticModel
{
    std::string name;
    std::size_t collisionLod{SIZE_MAX};
    std::vector<SourceStaticModelLod> lods;
    // IW3 skeleton of a scripted model that hides, shows, or plays effects on
    // parts by bone (destructible vehicles); empty keeps one tag_origin bone.
    std::vector<SourceBone> bones;
    // Simulated clutter body from the IW3 physics preset (dynamic models).
    std::optional<iw8::havok::ClutterBody> clutter;
};

struct SourceStaticModelInstance
{
    std::size_t model{};
    Vec3 origin{};
    std::array<Vec3, 3> axis{};
    float scale{1.0f};
    std::uint16_t reflectionProbe{};
};

struct SourceStaticModels
{
    std::vector<SourceStaticModel> models;
    std::vector<SourceStaticModelInstance> instances;
};

struct SourceDynamicEntities
{
    struct Brush
    {
        Vec4 quaternion{};
        Vec3 origin{};
        std::uint16_t model{};
        std::uint16_t physicsModel{};
    };

    SourceStaticModels models;
    std::vector<std::size_t> definitionModels;
    std::vector<Vec4> quaternions;
    std::vector<Vec3> origins;
    std::vector<Brush> brushes;
};

struct CollisionHull
{
    std::vector<Vec3> points;
    struct Slab
    {
        Vec3 direction{};
        float midpoint{};
        float halfSize{};
    };
    std::vector<Slab> slabs;
    std::uint32_t contents{1};
    std::uint32_t model{};
    std::uint32_t surfaceFlags{};
    std::uint16_t glassId{};
    std::uint32_t materialOverride{};
    std::vector<Vec4> ladderPlanes;
    // Per face: world-space tangent coordinate of the model center, then width.
    std::vector<std::array<float, 2>> ladderModelTangents;
    float ladderRungOffset{};
};

struct CollisionModel
{
    Vec3 minimum{};
    Vec3 maximum{};
};

struct CollisionMesh
{
    struct Triangle
    {
        std::array<std::uint32_t, 3> indices{};
        std::uint32_t contents{1};
        std::uint32_t surfaceFlags{};
        std::uint32_t material{5};
    };

    std::vector<Vec3> vertices;
    std::vector<Triangle> triangles;
    std::uint32_t model{};
};

struct CollisionData
{
    std::vector<CollisionHull> hulls;
    std::vector<CollisionModel> models;
    std::vector<CollisionMesh> meshes;
};

struct VisibilityGroups
{
    static constexpr std::uint32_t Global = UINT32_MAX;
    static constexpr std::uint32_t Unassigned = UINT32_MAX - 1;

    std::vector<std::uint32_t> surfaces;
    std::vector<std::uint32_t> models;
    std::vector<Json> treeBounds;
    std::vector<std::vector<std::uint32_t>> treeModels;
    std::vector<std::vector<std::uint32_t>> groupTrees;
    std::vector<std::uint16_t> treeChildCounts;
    std::vector<std::uint32_t> treeFirstChildren;
    std::vector<std::vector<std::uint32_t>> cellTrees;
    bool nativeTopology{};
    std::size_t sourceTreeCount{};
};

float Dot(const Vec3 &a, const Vec3 &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 Add(const Vec3 &a, const Vec3 &b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec3 Subtract(const Vec3 &a, const Vec3 &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char letter) {
        return static_cast<char>(std::tolower(letter));
    });
    return value;
}

Vec3 Multiply(const Vec3 &a, const float scale)
{
    return {a[0] * scale, a[1] * scale, a[2] * scale};
}

Vec3 Cross(const Vec3 &a, const Vec3 &b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec3 Unit(const Vec3 &value)
{
    const float length = std::sqrt(Dot(value, value));
    if (!std::isfinite(length) || length < 1.0e-10f)
    {
        throw std::runtime_error("degenerate vector in IW3 map data");
    }
    return Multiply(value, 1.0f / length);
}

template <std::size_t Size> std::array<float, Size> ReadVector(const Json &value)
{
    if (!value.is_array() || value.size() != Size)
    {
        throw std::runtime_error("invalid vector in IW3 map export");
    }
    std::array<float, Size> result{};
    for (std::size_t index = 0; index < Size; ++index)
    {
        result[index] = value.at(index).get<float>();
        if (!std::isfinite(result[index]))
        {
            throw std::runtime_error("non-finite value in IW3 map export");
        }
    }
    return result;
}

std::wstring Quote(const std::wstring &value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos)
    {
        return value;
    }

    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        if (character == L'\"')
        {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(character);
            slashes = 0;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0;
        output.push_back(character);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

std::filesystem::path FindUnlinker(const std::filesystem::path &requested)
{
    std::vector<std::filesystem::path> candidates;
    if (!requested.empty())
    {
        candidates.push_back(requested);
    }

    wchar_t environment[32768]{};
    DWORD environmentLength =
        GetEnvironmentVariableW(L"IW8_ZONETOOL_UNLINKER", environment, std::size(environment));
    if (!environmentLength)
    {
        environmentLength =
            GetEnvironmentVariableW(L"MW120R_UNLINKER", environment, std::size(environment));
    }
    if (environmentLength && environmentLength < std::size(environment))
    {
        candidates.emplace_back(environment);
    }

    wchar_t module[MAX_PATH]{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, module, std::size(module));
    if (moduleLength && moduleLength < std::size(module))
    {
        auto directory = std::filesystem::path(module).parent_path();
        for (unsigned depth = 0; depth < 6 && !directory.empty(); ++depth)
        {
            candidates.push_back(directory / "Unlinker.exe");
            candidates.push_back(directory / "tools" / "Unlinker.exe");
            candidates.push_back(directory / "mw120rproxy" / "tools" / "_vendor" /
                                 "OpenAssetTools" / "build" / "bin" / "Release_x86" /
                                 "Unlinker.exe");
            directory = directory.parent_path();
        }
    }
    auto directory = std::filesystem::current_path();
    for (unsigned depth = 0; depth < 4 && !directory.empty(); ++depth)
    {
        candidates.push_back(directory / "Unlinker.exe");
        candidates.push_back(directory / "tools" / "Unlinker.exe");
        candidates.push_back(directory / "mw120rproxy" / "tools" / "_vendor" / "OpenAssetTools" /
                             "build" / "bin" / "Release_x86" / "Unlinker.exe");
        directory = directory.parent_path();
    }

    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"Unlinker.exe", nullptr, std::size(found), found, nullptr))
    {
        candidates.emplace_back(found);
    }

    std::error_code error;
    for (const auto &candidate : candidates)
    {
        if (std::filesystem::is_regular_file(candidate, error))
        {
            return std::filesystem::absolute(candidate);
        }
        error.clear();
    }
    throw std::runtime_error(
        "OpenAssetTools Unlinker was not found; pass --unlinker or set IW8_ZONETOOL_UNLINKER");
}

std::filesystem::path MakeScratchDirectory(const std::filesystem::path &scratchRoot)
{
    std::error_code error;
    std::filesystem::create_directories(scratchRoot, error);
    if (error)
    {
        throw std::runtime_error("cannot create the IW3 conversion scratch parent: " +
                                 scratchRoot.string());
    }
    const auto root = std::filesystem::absolute(scratchRoot).wstring() + L"\\";

    wchar_t temporaryFile[MAX_PATH]{};
    if (!GetTempFileNameW(root.c_str(), L"iw3", 0, temporaryFile) || !DeleteFileW(temporaryFile) ||
        !CreateDirectoryW(temporaryFile, nullptr))
    {
        throw std::runtime_error("cannot create a private IW3 conversion directory");
    }
    return std::filesystem::path(temporaryFile);
}

void RunUnlinker(const std::filesystem::path &unlinker, const ImportOptions &options,
                 const std::filesystem::path &output,
                 const std::filesystem::path &sourceFastfile = {},
                 const bool supplementalAssets = false, const bool skeletons = false)
{
    std::vector<std::wstring> arguments{unlinker.wstring(),
                                        L"--no-color",
                                        L"--image-format",
                                        L"DDS",
                                        L"--model-format",
                                        skeletons ? L"XMODEL_EXPORT" : L"OBJ",
                                        L"-o",
                                        output.wstring()};
    if (skeletons)
    {
        // XMODEL_EXPORT keeps the bind skeleton and each vertex's bone, which
        // the OBJ extraction drops.
        arguments.push_back(L"--include-assets");
        arguments.push_back(L"xmodel");
    }
    else if (supplementalAssets)
    {
        arguments.push_back(L"--include-assets");
        arguments.push_back(L"xmodel,material,image,fx,impactfx,physpreset,soundcurve");
    }

    std::vector<std::filesystem::path> searchPaths;
    const auto appendSearchPath = [&searchPaths](const std::filesystem::path &path) {
        const auto directory = path.filename().empty() ? path.parent_path() : path;
        searchPaths.push_back(directory);
        auto main = directory / "main";
        if (directory.filename() == "zone")
            main = directory.parent_path() / "main";
        else if (directory.filename() == "english" &&
                 directory.parent_path().filename() == "zone")
            main = directory.parent_path().parent_path() / "main";
        std::error_code error;
        if (std::filesystem::is_regular_file(main / "iw_00.iwd", error))
            searchPaths.push_back(std::move(main));
    };
    for (const auto &path : options.searchPaths)
        appendSearchPath(path);
    appendSearchPath(options.fastfile.parent_path());
    if (!searchPaths.empty())
    {
        std::wstring joined;
        for (const auto &path : searchPaths)
        {
            if (!joined.empty())
            {
                joined.push_back(L';');
            }
            joined += std::filesystem::absolute(path).wstring();
        }
        arguments.push_back(L"--search-path");
        arguments.push_back(std::move(joined));
    }

    const auto inputFastfile = sourceFastfile.empty() ? options.fastfile : sourceFastfile;
    arguments.push_back(std::filesystem::absolute(inputFastfile).wstring());
    const auto loadFastfile =
        inputFastfile.parent_path() / (inputFastfile.stem().wstring() + L"_load.ff");
    std::error_code error;
    if (!supplementalAssets && !skeletons &&
        std::filesystem::is_regular_file(loadFastfile, error))
    {
        arguments.push_back(std::filesystem::absolute(loadFastfile).wstring());
    }

    std::wstring commandLine;
    for (const auto &argument : arguments)
    {
        if (!commandLine.empty())
        {
            commandLine.push_back(L' ');
        }
        commandLine += Quote(argument);
    }

    const auto logPath = output / (skeletons            ? "unlinker-skeleton.log"
                                   : supplementalAssets ? "unlinker-supplemental.log"
                                                        : "unlinker.log");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
    {
        throw std::runtime_error("cannot create the temporary Unlinker log");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL started =
        CreateProcessW(unlinker.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(log);
    if (!started)
    {
        throw std::runtime_error("cannot start OpenAssetTools Unlinker");
    }

    const DWORD timeoutMinutes = supplementalAssets || skeletons ? 30u : 10u;
    const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMinutes * 60u * 1000u);
    DWORD exitCode = 1;
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    else if (wait == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (wait != WAIT_OBJECT_0 || exitCode != 0)
    {
        std::ifstream logFile(logPath, std::ios::binary);
        std::string logTail;
        if (logFile)
        {
            logFile.seekg(0, std::ios::end);
            const auto size = logFile.tellg();
            if (size > 0)
            {
                const auto length = static_cast<std::streamoff>(std::min<std::streamoff>(size, 4096));
                logFile.seekg(size - length);
                logTail.resize(static_cast<std::size_t>(length));
                logFile.read(logTail.data(), length);
            }
        }
        throw std::runtime_error("OpenAssetTools failed while reading the IW3 fastfile" +
                                 (logTail.empty() ? std::string{} : ":\n" + logTail));
    }
}

std::filesystem::path FindIw3CommonFastfile(const ImportOptions &options)
{
    std::vector<std::filesystem::path> roots = options.searchPaths;
    roots.push_back(options.fastfile.parent_path());
    std::vector<std::filesystem::path> candidates;
    for (const auto &root : roots)
    {
        candidates.push_back(root);
        candidates.push_back(root / "common_mp.ff");
        candidates.push_back(root / "zone" / "english" / "common_mp.ff");
        candidates.push_back(root / "zone" / "common_mp.ff");
        candidates.push_back(root.parent_path() / "zone" / "english" / "common_mp.ff");
        candidates.push_back(root.parent_path() / "zone" / "common_mp.ff");
    }

    std::error_code error;
    for (const auto &candidate : candidates)
    {
        if (candidate.filename() == "common_mp.ff" &&
            std::filesystem::is_regular_file(candidate, error))
        {
            return std::filesystem::absolute(candidate);
        }
        error.clear();
    }
    return {};
}

std::vector<std::string> MissingEntityModels(const std::filesystem::path &root,
                                              const std::vector<std::string> &models)
{
    std::vector<std::string> missing;
    for (const auto &model : models)
    {
        std::error_code error;
        const auto path = root / "xmodel" / (model + ".json");
        if (!std::filesystem::is_regular_file(path, error))
            missing.push_back(model);
    }
    return missing;
}

void CopyExtractedEntityModels(const std::filesystem::path &destination,
                               const std::vector<std::string> &models,
                               const std::vector<std::filesystem::path> &searchPaths)
{
    for (const std::string &model : models)
    {
        for (const auto &searchPath : searchPaths)
        {
            const auto sourceModel = searchPath / "xmodel" / (model + ".json");
            std::error_code error;
            if (!std::filesystem::is_regular_file(sourceModel, error))
                continue;

            const auto targetModel = destination / "xmodel" / sourceModel.filename();
            std::filesystem::create_directories(targetModel.parent_path(), error);
            if (error)
                throw std::runtime_error("cannot create IW3 entity-model staging directory");
            std::filesystem::copy_file(sourceModel, targetModel,
                                       std::filesystem::copy_options::overwrite_existing, error);
            if (error)
                throw std::runtime_error("cannot stage IW3 entity model " + model);

            const auto sourceAttributes = searchPath / "xmodel" / (model + ".replay.json");
            if (!std::filesystem::is_regular_file(sourceAttributes, error))
                continue;
            std::filesystem::copy_file(sourceAttributes, destination / "xmodel" /
                                                            sourceAttributes.filename(),
                                       std::filesystem::copy_options::overwrite_existing, error);
            if (error)
                throw std::runtime_error("cannot stage IW3 entity-model attributes " + model);

            const auto sourceModels = searchPath / "model_export";
            if (!std::filesystem::is_directory(sourceModels, error))
                continue;
            const auto targetModels = destination / "model_export";
            std::filesystem::create_directories(targetModels, error);
            if (error)
                throw std::runtime_error("cannot create IW3 model-export staging directory");
            for (std::filesystem::directory_iterator entry(sourceModels, error), end;
                 !error && entry != end; entry.increment(error))
            {
                if (!entry->is_regular_file(error) ||
                    !entry->path().filename().string().starts_with(model))
                    continue;
                std::filesystem::copy_file(entry->path(), targetModels / entry->path().filename(),
                                           std::filesystem::copy_options::overwrite_existing, error);
                if (error)
                    throw std::runtime_error("cannot stage IW3 entity-model mesh " + model);
            }
            if (error)
                throw std::runtime_error("cannot enumerate IW3 entity-model mesh files");
            zt::info("iw3: staged entity model %s from extracted search assets", model.c_str());
            break;
        }
    }
}

void CopyExtractedFx(const std::filesystem::path &destination,
                     const std::vector<std::filesystem::path> &searchPaths)
{
    std::size_t copied = 0;
    for (const auto &searchPath : searchPaths)
    {
        const auto sourceRoot = searchPath / "fx";
        std::error_code error;
        if (!std::filesystem::is_directory(sourceRoot, error))
            continue;
        for (std::filesystem::recursive_directory_iterator entry(sourceRoot, error), end;
             !error && entry != end; entry.increment(error))
        {
            if (!entry->is_regular_file(error) || entry->path().extension() != ".json")
                continue;
            const auto target = destination / "fx" /
                                std::filesystem::relative(entry->path(), sourceRoot, error);
            if (error)
                throw std::runtime_error("cannot resolve extracted IW3 FX asset path");
            std::filesystem::create_directories(target.parent_path(), error);
            if (error)
                throw std::runtime_error("cannot create IW3 FX staging directory");
            std::filesystem::copy_file(entry->path(), target,
                                       std::filesystem::copy_options::skip_existing, error);
            if (error)
                throw std::runtime_error("cannot stage extracted IW3 FX source asset");
            ++copied;
        }
        if (error)
            throw std::runtime_error("cannot enumerate extracted IW3 FX source assets");
    }
    if (copied)
        zt::info("iw3: staged %zu extracted source FX graph(s)", copied);
}

void CopyExtractedFxMaterials(const std::filesystem::path &destination,
                              const std::vector<std::filesystem::path> &searchPaths)
{
    std::size_t copied = 0;
    for (const auto &searchPath : searchPaths)
    {
        const auto sourceRoot = searchPath / "materials";
        std::error_code error;
        if (!std::filesystem::is_directory(sourceRoot, error))
            continue;
        for (std::filesystem::recursive_directory_iterator entry(sourceRoot, error), end;
             !error && entry != end; entry.increment(error))
        {
            if (!entry->is_regular_file(error) || entry->path().extension() != ".json")
                continue;
            const auto target = destination / "materials" /
                                std::filesystem::relative(entry->path(), sourceRoot, error);
            if (error)
                throw std::runtime_error("cannot resolve extracted IW3 FX material path");
            std::filesystem::create_directories(target.parent_path(), error);
            if (error)
                throw std::runtime_error("cannot create IW3 FX material staging directory");
            std::filesystem::copy_file(entry->path(), target,
                                       std::filesystem::copy_options::skip_existing, error);
            if (error)
                throw std::runtime_error("cannot stage extracted IW3 FX material source");
            ++copied;
        }
        if (error)
            throw std::runtime_error("cannot enumerate extracted IW3 FX material sources");
    }
    if (copied)
        zt::info("iw3: staged %zu extracted source FX material(s)", copied);
}

std::string JoinNames(const std::vector<std::string> &names)
{
    std::string result;
    for (const auto &name : names)
    {
        if (!result.empty())
            result += ", ";
        result += name;
    }
    return result;
}

std::filesystem::path FindSingleExport(const std::filesystem::path &root,
                                       const std::string &extension)
{
    std::error_code error;
    std::filesystem::path result;
    for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error))
        {
            continue;
        }
        const std::string name = iterator->path().filename().string();
        if (!name.ends_with(extension))
        {
            continue;
        }
        if (!result.empty())
        {
            throw std::runtime_error("IW3 input contains more than one map world");
        }
        result = iterator->path();
    }
    if (error || result.empty())
    {
        throw std::runtime_error(
            "Unlinker did not produce Replay map data; build it with ReplayMapDumpers.h");
    }
    return result;
}

Json ReadJson(const std::filesystem::path &path);
std::string TrimSourceAssetField(std::string_view value);

bool IsValidFxAssetName(const std::string &name)
{
    if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos)
        return false;
    const std::filesystem::path path(name);
    if (path.has_root_path())
        return false;
    for (const auto &part : path)
        if (part == "." || part == "..")
            return false;
    return true;
}

template <typename T>
T FxInteger(const Json &value, const char *field)
{
    if (!value.is_number_integer() && !value.is_number_unsigned())
        throw std::runtime_error(std::string("IW3 FX field is not an integer: ") + field);
    const auto number = value.get<std::int64_t>();
    if (number < static_cast<std::int64_t>(std::numeric_limits<T>::lowest()) ||
        number > static_cast<std::int64_t>(std::numeric_limits<T>::max()))
        throw std::runtime_error(std::string("IW3 FX integer is out of range: ") + field);
    return static_cast<T>(number);
}

float FxFloat(const Json &value, const char *field)
{
    if (!value.is_number())
        throw std::runtime_error(std::string("IW3 FX field is not numeric: ") + field);
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < -std::numeric_limits<float>::max() ||
        number > std::numeric_limits<float>::max())
        throw std::runtime_error(std::string("IW3 FX float is invalid: ") + field);
    return static_cast<float>(number);
}

template <std::size_t Count>
std::array<float, Count> FxFloatArray(const Json &value, const char *field)
{
    if (!value.is_array() || value.size() != Count)
        throw std::runtime_error(std::string("IW3 FX array has the wrong size: ") + field);
    std::array<float, Count> result{};
    for (std::size_t index = 0; index < Count; ++index)
        result[index] = FxFloat(value.at(index), field);
    return result;
}

PreparedFxFloatRange FxFloatRange(const Json &value, const char *field)
{
    if (!value.is_object())
        throw std::runtime_error(std::string("IW3 FX range is not an object: ") + field);
    return {FxFloat(value.at("base"), field), FxFloat(value.at("amplitude"), field)};
}

PreparedFxIntRange FxIntRange(const Json &value, const char *field)
{
    if (!value.is_object())
        throw std::runtime_error(std::string("IW3 FX range is not an object: ") + field);
    return {FxInteger<std::int32_t>(value.at("base"), field),
            FxInteger<std::int32_t>(value.at("amplitude"), field)};
}

PreparedFxVec3Range FxVec3Range(const Json &value, const char *field)
{
    if (!value.is_object())
        throw std::runtime_error(std::string("IW3 FX vector range is not an object: ") + field);
    return {FxFloatArray<3>(value.at("base"), field),
            FxFloatArray<3>(value.at("amplitude"), field)};
}

template <std::size_t Count>
std::array<PreparedFxFloatRange, Count> FxFloatRangeArray(const Json &value,
                                                         const char *field)
{
    if (!value.is_array() || value.size() != Count)
        throw std::runtime_error(std::string("IW3 FX range array has the wrong size: ") + field);
    std::array<PreparedFxFloatRange, Count> result{};
    for (std::size_t index = 0; index < Count; ++index)
        result[index] = FxFloatRange(value.at(index), field);
    return result;
}

PreparedFxVisualState FxVisualState(const Json &value)
{
    if (!value.is_object())
        throw std::runtime_error("IW3 FX visual state is not an object");
    const auto &colors = value.at("color");
    if (!colors.is_array() || colors.size() != 4)
        throw std::runtime_error("IW3 FX visual-state color has the wrong size");
    PreparedFxVisualState result;
    for (std::size_t index = 0; index < result.color.size(); ++index)
        result.color[index] = FxInteger<std::uint8_t>(colors.at(index), "visual color");
    result.rotationDelta = FxFloat(value.at("rotation_delta"), "visual rotation delta");
    result.rotationTotal = FxFloat(value.at("rotation_total"), "visual rotation total");
    result.size = FxFloatArray<2>(value.at("size"), "visual size");
    result.scale = FxFloat(value.at("scale"), "visual scale");
    return result;
}

PreparedFxVelocityFrame FxVelocityFrame(const Json &value)
{
    if (!value.is_object())
        throw std::runtime_error("IW3 FX velocity frame is not an object");
    return {FxVec3Range(value.at("velocity"), "velocity"),
            FxVec3Range(value.at("total_delta"), "velocity total delta")};
}

std::string FxEffectReference(const Json &value, const char *field,
                              std::set<std::pair<std::string, std::string>> &references)
{
    if (value.is_null())
        return {};
    if (!value.is_string())
        throw std::runtime_error(std::string("IW3 FX reference is not a string: ") + field);
    const auto name = value.get<std::string>();
    if (!IsValidFxAssetName(name))
        throw std::runtime_error(std::string("IW3 FX reference has an invalid name: ") + field);
    references.emplace("fx", name);
    return name;
}

PreparedFxVisual FxVisual(const Json &value, const std::uint8_t elementType,
                          std::set<std::pair<std::string, std::string>> &references)
{
    PreparedFxVisual result;
    if (elementType == 9)
    {
        if (!value.is_array() || value.size() != 2)
            throw std::runtime_error("IW3 FX decal visual must contain two material slots");
        result.kind = PreparedFxVisualKind::decal;
        for (const auto &slot : value)
        {
            if (slot.is_null())
            {
                result.names.emplace_back();
                continue;
            }
            if (!slot.is_string() || !IsValidFxAssetName(slot.get<std::string>()))
                throw std::runtime_error("IW3 FX decal has an invalid material name");
            result.names.push_back(slot.get<std::string>());
            references.emplace("material", result.names.back());
        }
        return result;
    }

    if (!value.is_object())
        throw std::runtime_error("IW3 FX visual is not an object");
    const auto type = value.at("type").get<std::string>();
    std::string expected;
    std::string dependencyType;
    if (elementType <= 4)
    {
        result.kind = PreparedFxVisualKind::material;
        expected = dependencyType = "material";
    }
    else if (elementType == 5)
    {
        result.kind = PreparedFxVisualKind::xmodel;
        expected = dependencyType = "xmodel";
    }
    else if (elementType == 8)
    {
        result.kind = PreparedFxVisualKind::sound;
        expected = dependencyType = "sound";
    }
    else if (elementType == 10)
    {
        result.kind = PreparedFxVisualKind::effect;
        expected = dependencyType = "fx";
    }
    else
    {
        result.kind = PreparedFxVisualKind::none;
        expected = "none";
    }
    if (type != expected)
        throw std::runtime_error("IW3 FX visual type does not match its element type");
    if (result.kind == PreparedFxVisualKind::none)
        return result;

    const auto &nameValue = value.at("name");
    if (nameValue.is_null() && result.kind == PreparedFxVisualKind::sound)
    {
        result.names.emplace_back();
        return result;
    }
    if (!nameValue.is_string() || !IsValidFxAssetName(nameValue.get<std::string>()))
        throw std::runtime_error("IW3 FX visual has an invalid asset name");
    result.names.push_back(nameValue.get<std::string>());
    references.emplace(dependencyType, result.names.front());
    return result;
}

std::optional<PreparedFxTrail> FxTrail(const Json &value)
{
    if (value.is_null())
        return std::nullopt;
    if (!value.is_object())
        throw std::runtime_error("IW3 FX trail is not an object");
    PreparedFxTrail result;
    result.scrollTimeMsec = FxInteger<std::int32_t>(value.at("scroll_time_msec"), "trail scroll");
    result.repeatDist = FxInteger<std::int32_t>(value.at("repeat_dist"), "trail repeat distance");
    result.splitDist = FxInteger<std::int32_t>(value.at("split_dist"), "trail split distance");
    const auto &vertices = value.at("vertices");
    const auto &indices = value.at("indices");
    if (!vertices.is_array() || vertices.size() > 16384 || !indices.is_array() ||
        indices.size() > 16384)
        throw std::runtime_error("IW3 FX trail arrays exceed the source limits");
    result.vertices.reserve(vertices.size());
    for (const auto &vertex : vertices)
    {
        if (!vertex.is_object())
            throw std::runtime_error("IW3 FX trail vertex is not an object");
        result.vertices.push_back({FxFloatArray<2>(vertex.at("position"), "trail position"),
                                   FxFloatArray<2>(vertex.at("normal"), "trail normal"),
                                   FxFloat(vertex.at("tex_coord"), "trail texcoord")});
    }
    result.indices.reserve(indices.size());
    for (const auto &index : indices)
    {
        const auto parsed = FxInteger<std::uint16_t>(index, "trail index");
        if (parsed >= result.vertices.size())
            throw std::runtime_error("IW3 FX trail index is outside the vertex array");
        result.indices.push_back(parsed);
    }
    return result;
}

PreparedFxElement FxElement(const Json &value,
                            std::set<std::pair<std::string, std::string>> &references)
{
    if (!value.is_object())
        throw std::runtime_error("IW3 FX element is not an object");
    PreparedFxElement result;
    result.flags = FxInteger<std::int32_t>(value.at("flags"), "element flags");
    const auto &spawn = value.at("spawn");
    if (!spawn.is_object())
        throw std::runtime_error("IW3 FX spawn definition is not an object");
    const auto &raw = spawn.at("raw");
    if (!raw.is_array() || raw.size() != 2)
        throw std::runtime_error("IW3 FX raw spawn union has the wrong size");
    result.spawn.raw = {FxInteger<std::int32_t>(raw.at(0), "spawn raw interval"),
                        FxInteger<std::int32_t>(raw.at(1), "spawn raw count")};
    const auto &looping = spawn.at("looping");
    const auto &oneShot = spawn.at("one_shot");
    if (!looping.is_object() || !oneShot.is_object())
        throw std::runtime_error("IW3 FX spawn views are not objects");
    result.spawn.loopingIntervalMsec =
        FxInteger<std::int32_t>(looping.at("interval_msec"), "spawn interval");
    result.spawn.loopingCount = FxInteger<std::int32_t>(looping.at("count"), "spawn count");
    result.spawn.oneShotCount = FxIntRange(oneShot.at("count"), "one-shot count");
    if (result.spawn.raw[0] != result.spawn.loopingIntervalMsec ||
        result.spawn.raw[1] != result.spawn.loopingCount ||
        result.spawn.raw[0] != result.spawn.oneShotCount.base ||
        result.spawn.raw[1] != result.spawn.oneShotCount.amplitude)
        throw std::runtime_error("IW3 FX spawn union views disagree");

    result.spawnRange = FxFloatRange(value.at("spawn_range"), "spawn range");
    result.fadeInRange = FxFloatRange(value.at("fade_in_range"), "fade-in range");
    result.fadeOutRange = FxFloatRange(value.at("fade_out_range"), "fade-out range");
    result.spawnFrustumCullRadius =
        FxFloat(value.at("spawn_frustum_cull_radius"), "spawn frustum radius");
    result.spawnDelayMsec = FxIntRange(value.at("spawn_delay_msec"), "spawn delay");
    result.lifeSpanMsec = FxIntRange(value.at("life_span_msec"), "life span");
    result.spawnOrigin = FxFloatRangeArray<3>(value.at("spawn_origin"), "spawn origin");
    result.spawnOffsetRadius =
        FxFloatRange(value.at("spawn_offset_radius"), "spawn offset radius");
    result.spawnOffsetHeight =
        FxFloatRange(value.at("spawn_offset_height"), "spawn offset height");
    result.spawnAngles = FxFloatRangeArray<3>(value.at("spawn_angles"), "spawn angles");
    result.angularVelocity =
        FxFloatRangeArray<3>(value.at("angular_velocity"), "angular velocity");
    result.initialRotation = FxFloatRange(value.at("initial_rotation"), "initial rotation");
    result.gravity = FxFloatRange(value.at("gravity"), "gravity");
    result.reflectionFactor = FxFloatRange(value.at("reflection_factor"), "reflection factor");

    const auto &atlas = value.at("atlas");
    if (!atlas.is_object())
        throw std::runtime_error("IW3 FX atlas is not an object");
    result.atlas = {FxInteger<std::uint8_t>(atlas.at("behavior"), "atlas behavior"),
                    FxInteger<std::uint8_t>(atlas.at("index"), "atlas index"),
                    FxInteger<std::uint8_t>(atlas.at("fps"), "atlas fps"),
                    FxInteger<std::uint8_t>(atlas.at("loop_count"), "atlas loop count"),
                    FxInteger<std::uint8_t>(atlas.at("col_index_bits"), "atlas column bits"),
                    FxInteger<std::uint8_t>(atlas.at("row_index_bits"), "atlas row bits"),
                    FxInteger<std::int16_t>(atlas.at("entry_count"), "atlas entry count")};

    result.type = FxInteger<std::uint8_t>(value.at("elem_type"), "element type");
    static constexpr std::array<const char *, 11> typeNames{
        "sprite_billboard", "sprite_oriented", "tail",       "trail", "cloud", "model",
        "omni_light",       "spot_light",      "sound",      "decal", "runner"};
    if (result.type >= typeNames.size() || value.at("elem_type_name").get<std::string>() !=
                                               typeNames[result.type])
        throw std::runtime_error("IW3 FX element type and name disagree");
    result.visualCount = FxInteger<std::uint8_t>(value.at("visual_count"), "visual count");
    result.velocityIntervalCount =
        FxInteger<std::uint8_t>(value.at("vel_interval_count"), "velocity interval count");
    result.visualStateIntervalCount =
        FxInteger<std::uint8_t>(value.at("vis_state_interval_count"),
                                "visual-state interval count");

    const auto &velocitySamples = value.at("velocity_samples");
    if (!velocitySamples.is_array() || velocitySamples.size() > 256 ||
        (!velocitySamples.empty() &&
         velocitySamples.size() != static_cast<std::size_t>(result.velocityIntervalCount) + 1) ||
        (velocitySamples.empty() && result.velocityIntervalCount != 0))
        throw std::runtime_error("IW3 FX velocity sample count is inconsistent");
    result.velocitySamples.reserve(velocitySamples.size());
    for (const auto &sample : velocitySamples)
    {
        if (!sample.is_object())
            throw std::runtime_error("IW3 FX velocity sample is not an object");
        result.velocitySamples.push_back(
            {FxVelocityFrame(sample.at("local")), FxVelocityFrame(sample.at("world"))});
    }

    const auto &visualSamples = value.at("visual_samples");
    if (!visualSamples.is_array() || visualSamples.size() > 256 ||
        (!visualSamples.empty() &&
         visualSamples.size() != static_cast<std::size_t>(result.visualStateIntervalCount) + 1) ||
        (visualSamples.empty() && result.visualStateIntervalCount != 0))
        throw std::runtime_error("IW3 FX visual sample count is inconsistent");
    result.visualSamples.reserve(visualSamples.size());
    for (const auto &sample : visualSamples)
    {
        if (!sample.is_object())
            throw std::runtime_error("IW3 FX visual sample is not an object");
        result.visualSamples.push_back(
            {FxVisualState(sample.at("base")), FxVisualState(sample.at("amplitude"))});
    }

    if (!value.at("visuals_present").is_boolean())
        throw std::runtime_error("IW3 FX visual presence flag is not boolean");
    result.visualsPresent = value.at("visuals_present").get<bool>();
    const auto &visuals = value.at("visuals");
    if (!visuals.is_array() || visuals.size() != result.visualCount ||
        result.visualsPresent != !visuals.empty())
        throw std::runtime_error("IW3 FX visual array does not match its source count");
    result.visuals.reserve(visuals.size());
    for (const auto &visual : visuals)
        result.visuals.push_back(FxVisual(visual, result.type, references));

    result.collisionMins = FxFloatArray<3>(value.at("collision_mins"), "collision minimum");
    result.collisionMaxs = FxFloatArray<3>(value.at("collision_maxs"), "collision maximum");
    for (std::size_t axis = 0; axis < 3; ++axis)
        if (result.collisionMins[axis] > result.collisionMaxs[axis])
            throw std::runtime_error("IW3 FX collision bounds are reversed");
    result.effectOnImpact =
        FxEffectReference(value.at("effect_on_impact"), "effect on impact", references);
    result.effectOnDeath =
        FxEffectReference(value.at("effect_on_death"), "effect on death", references);
    result.effectEmitted =
        FxEffectReference(value.at("effect_emitted"), "emitted effect", references);
    result.emitDist = FxFloatRange(value.at("emit_dist"), "emit distance");
    result.emitDistVariance =
        FxFloatRange(value.at("emit_dist_variance"), "emit distance variance");
    result.trail = FxTrail(value.at("trail"));
    result.sortOrder = FxInteger<std::uint8_t>(value.at("sort_order"), "sort order");
    result.lightingFrac = FxInteger<std::uint8_t>(value.at("lighting_frac"), "lighting fraction");
    result.useItemClip = FxInteger<std::uint8_t>(value.at("use_item_clip"), "item clip flag");
    return result;
}

std::vector<PreparedFx> ReadPreparedFx(const std::filesystem::path &root)
{
    const auto fxRoot = root / "fx";
    std::vector<PreparedFx> effects;
    std::error_code error;
    if (!std::filesystem::is_directory(fxRoot, error))
    {
        return effects;
    }

    for (std::filesystem::recursive_directory_iterator iterator(fxRoot, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error) || !iterator->path().filename().string().ends_with(".iw3.json"))
            continue;

        try
        {
            Json graph = ReadJson(iterator->path());
            // Names of assets referenced from another zone carry IW3's ','
            // prefix (mp_agroprom: ",fx_me_brick_01"); the asset is the same.
            const auto stripReferencePrefix = [](auto &self, Json &value) -> void {
                if (value.is_string())
                {
                    auto &text = value.get_ref<std::string &>();
                    if (text.size() > 1 && text.front() == ',')
                        text.erase(0, 1);
                }
                else if (value.is_structured())
                    for (auto &child : value)
                        self(self, child);
            };
            stripReferencePrefix(stripReferencePrefix, graph);
            if (graph.value("schema", 0) != 1 ||
                graph.value("asset_type", std::string{}) != "iw3_fx")
                throw std::runtime_error("schema or asset type is unsupported");
            const auto &layout = graph.at("layout");
            if (!layout.is_object() || layout.value("FxEffectDef", 0) != 32 ||
                layout.value("FxElemDef", 0) != 252 || layout.value("FxTrailDef", 0) != 28)
                throw std::runtime_error("source ABI layout does not match IW3 multiplayer");

            PreparedFx prepared;
            prepared.name = graph.at("name").get<std::string>();
            if (!IsValidFxAssetName(prepared.name))
                throw std::runtime_error("effect name is invalid");
            prepared.sourcePath = iterator->path();
            prepared.flags = FxInteger<std::int32_t>(graph.at("flags"), "effect flags");
            prepared.totalSize = FxInteger<std::int32_t>(graph.at("total_size"), "effect size");
            prepared.msecLoopingLife =
                FxInteger<std::int32_t>(graph.at("msec_looping_life"), "effect looping life");

            const auto &counts = graph.at("element_counts");
            if (!counts.is_object())
                throw std::runtime_error("element counts are not an object");
            prepared.elementCounts = {
                FxInteger<std::uint32_t>(counts.at("looping"), "looping element count"),
                FxInteger<std::uint32_t>(counts.at("one_shot"), "one-shot element count"),
                FxInteger<std::uint32_t>(counts.at("emission"), "emission element count")};
            const auto total = FxInteger<std::uint32_t>(counts.at("total"), "total element count");
            const std::uint64_t computedTotal = static_cast<std::uint64_t>(prepared.elementCounts[0]) +
                                                prepared.elementCounts[1] +
                                                prepared.elementCounts[2];
            const auto &elements = graph.at("elements");
            if (computedTotal != total || total > 4096 || !elements.is_array() ||
                elements.size() != total)
                throw std::runtime_error("element counts do not match the element array");

            std::set<std::pair<std::string, std::string>> declaredDependencies;
            const auto &dependencies = graph.at("dependencies");
            if (!dependencies.is_array() || dependencies.size() > 4096)
                throw std::runtime_error("dependency list is invalid");
            for (const auto &dependency : dependencies)
            {
                if (!dependency.is_object())
                    throw std::runtime_error("dependency is not an object");
                const auto dependencyName = dependency.at("name").get<std::string>();
                const auto dependencyType = dependency.at("type").get<std::string>();
                if (!IsValidFxAssetName(dependencyName) ||
                    (dependencyType != "fx" && dependencyType != "material" &&
                     dependencyType != "image" && dependencyType != "xmodel" &&
                     dependencyType != "sound"))
                    throw std::runtime_error("dependency type or name is unsupported");
                if (!declaredDependencies.emplace(dependencyType, dependencyName).second)
                    throw std::runtime_error("dependency is duplicated");
                prepared.dependencies.push_back({dependencyName, dependencyType});
            }

            std::set<std::pair<std::string, std::string>> elementReferences;
            prepared.elements.reserve(elements.size());
            for (const auto &element : elements)
                prepared.elements.push_back(FxElement(element, elementReferences));
            for (const auto &reference : elementReferences)
                if (!declaredDependencies.contains(reference))
                    throw std::runtime_error("element reference is absent from the dependency list: " +
                                             reference.first + " " + reference.second);
            effects.push_back(std::move(prepared));
        }
        catch (const std::exception &error)
        {
            throw std::runtime_error("invalid IW3 FX source export '" + iterator->path().string() +
                                     "': " + error.what());
        }
    }
    if (error)
        throw std::runtime_error("cannot enumerate IW3 FX source exports");
    std::sort(effects.begin(), effects.end(), [](const PreparedFx &left, const PreparedFx &right) {
        return left.name < right.name;
    });
    for (std::size_t index = 1; index < effects.size(); ++index)
    {
        if (effects[index - 1].name == effects[index].name)
            throw std::runtime_error("duplicate IW3 FX source export: " + effects[index].name);
    }
    return effects;
}

// Placeholder for an IW3 effect name in translated GSC; replaced with the
// converted Replay VFX (or a stock stand-in) once effects are converted.
constexpr std::string_view kScriptEffectMarker = "@mw120r_iw3fx@";

// IW3 maps\mp\_destructible_types vehicle sounds: the fire state's flare-up and
// burning loop, and the explosion (its secondaries chain the close, boom and
// debris layers). IW3 MP ships no audio for veh_glass_break_*; car glass uses
// the window break alias (window_entry_glass_med) instead.
constexpr const char *kIw3VehicleDestructibleSounds[] = {
    "fire_vehicle_flareup_med", "fire_vehicle_med", "car_explode", "window_entry_glass_med"};

// IW3 maps\mp\_destructible_types vehicle effects (sedan, wagon, hatchback, ...).
constexpr const char *kIw3VehicleDestructibleEffects[] = {
    "smoke/car_damage_whitesmoke",    "smoke/car_damage_blacksmoke",
    "smoke/car_damage_blacksmoke_fire", "explosions/small_vehicle_explosion",
    "props/car_glass_large",          "props/car_glass_med",
    "props/car_glass_headlight",      "props/car_glass_brakelight",
};

// IW3 effects the compatibility layer plays for TVs and destructable pieces in
// place of stock stand-ins (converted with the map's impact effects).
constexpr const char *kIw3CompatPropEffects[] = {"impacts/small_glass",
                                                 "impacts/small_metalhit_1"};

// IW3 maps\mp\_global_fx.gsc: a one-shot effect (and loop sound) on every
// script_struct with these targetnames (prefab fires, lamp haze, tower lights).
struct Iw3GlobalFx
{
    const char *targetname;
    const char *effect;
    const char *sound;
};
constexpr Iw3GlobalFx kIw3GlobalFx[] = {
    {"barrel_fireFX_origin", "fire/firelp_barrel_pm", "fire_barrel_small"},
    {"ch_streetlight_02_FX_origin", "misc/lighthaze", nullptr},
    {"me_streetlight_01_FX_origin", "misc/lighthaze_bog_a", nullptr},
    {"ch_street_light_01_on", "misc/light_glow_white", nullptr},
    {"highway_lamp_post", "misc/lighthaze_villassault", nullptr},
    {"cs_cargoship_spotlight_on_FX_origin", "misc/lighthaze", nullptr},
    {"me_dumpster_fire_FX_origin", "fire/firelp_med_pm_nodistort", "fire_dumpster_medium"},
    {"com_tires_burning01_FX_origin", "fire/tire_fire_med", nullptr},
    {"icbm_powerlinetower_FX_origin", "misc/power_tower_light_red_blink", nullptr},
};


// loadfx( "name" ) literals in the map's own IW3 scripts.
std::set<std::string> ReadScriptEffectNames(const std::filesystem::path &scriptDirectory)
{
    std::set<std::string> names;
    std::error_code error;
    if (!std::filesystem::is_directory(scriptDirectory, error))
        return names;
    static const std::regex call(R"re(\bloadfx\s*\(\s*"([^"]+)"\s*\))re", std::regex::icase);
    for (const auto &entry : std::filesystem::directory_iterator(scriptDirectory, error))
    {
        if (entry.path().extension() != ".gsc")
            continue;
        std::string text;
        if (!zt::read_file_str(entry.path().string(), text))
            continue;
        for (auto match = std::sregex_iterator(text.begin(), text.end(), call);
             match != std::sregex_iterator(); ++match)
        {
            std::string name = (*match)[1].str();
            std::replace(name.begin(), name.end(), '\\', '/');
            std::transform(name.begin(), name.end(), name.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            names.insert(name);
        }
    }
    return names;
}

// Effect-like string literals ("mire/bilbord_loop") in the map scripts: maps
// pass effect names through their own helpers to loadfx( variable ).
std::set<std::string> ReadScriptEffectLiterals(const std::filesystem::path &scriptDirectory)
{
    std::set<std::string> names;
    std::error_code error;
    if (!std::filesystem::is_directory(scriptDirectory, error))
        return names;
    static const std::regex literal(R"re("([A-Za-z0-9_\-]+(?:[/\\][A-Za-z0-9_\-\.]+)+)")re");
    for (const auto &entry : std::filesystem::directory_iterator(scriptDirectory, error))
    {
        if (entry.path().extension() != ".gsc")
            continue;
        std::string text;
        if (!zt::read_file_str(entry.path().string(), text))
            continue;
        for (auto match = std::sregex_iterator(text.begin(), text.end(), literal);
             match != std::sregex_iterator(); ++match)
        {
            std::string name = (*match)[1].str();
            std::replace(name.begin(), name.end(), '\\', '/');
            names.insert(Lowercase(name));
        }
    }
    return names;
}

std::set<std::string> ReadDeclaredFx(const std::filesystem::path &zoneFile)
{
    std::ifstream input(zoneFile);
    if (!input)
        throw std::runtime_error("cannot read IW3 FX zone declarations: " + zoneFile.string());
    std::set<std::string> names;
    std::string line;
    while (std::getline(input, line))
    {
        const auto comma = line.find(',');
        if (comma == std::string::npos ||
            TrimSourceAssetField(std::string_view(line).substr(0, comma)) != "fx")
            continue;
        const auto name = TrimSourceAssetField(
            std::string_view(line).substr(line.rfind(',') + 1));
        if (name.empty())
            continue;
        const std::filesystem::path asset(name);
        if (asset.has_root_path() ||
            std::find(asset.begin(), asset.end(), std::filesystem::path("..")) != asset.end())
            throw std::runtime_error("invalid IW3 FX declaration name: " + name);
        names.insert(name);
    }
    if (input.bad())
        throw std::runtime_error("cannot read IW3 FX zone declarations");
    return names;
}

// Effects the IW3 impact table plays. Maps whose zone does not redeclare them
// (Gulag) use common_mp's, which the supplemental extraction provides.
std::set<std::string> ReadImpactEffectNames(const std::filesystem::path &root)
{
    std::set<std::string> names;
    for (const auto &path : {root / "impactfx" / "_default.iw3.json",
                             root / "raw" / "impactfx" / "_default.iw3.json"})
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
            continue;
        const Json source = ReadJson(path);
        const auto entries = source.find("entries");
        if (entries == source.end() || !entries->is_array())
            break;
        for (const auto &entry : *entries)
            for (const char *surfaces : {"nonflesh", "flesh"})
                if (const auto slots = entry.find(surfaces); slots != entry.end() && slots->is_array())
                    for (const auto &slot : *slots)
                        if (slot.is_string() && IsValidFxAssetName(slot.get<std::string>()))
                            names.insert(slot.get<std::string>());
        break;
    }
    return names;
}

std::vector<PreparedRawFile> ReadPreparedRawFiles(const std::filesystem::path &root,
                                                  const std::string &sourceMap)
{
    const auto zoneRoot = root / "zone_source";
    std::error_code error;
    std::set<std::string> names;
    for (std::filesystem::directory_iterator iterator(zoneRoot, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".zone")
            continue;
        const std::string stem = iterator->path().stem().string();
        if (stem != sourceMap && !stem.starts_with(sourceMap + "_"))
            continue;

        std::ifstream input(iterator->path(), std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot read IW3 rawfile zone declarations");
        std::string line;
        while (std::getline(input, line))
        {
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
            const auto comma = line.find(',');
            if (comma == std::string::npos ||
                TrimSourceAssetField(std::string_view(line).substr(0, comma)) != "rawfile")
                continue;
            const auto name = TrimSourceAssetField(
                std::string_view(line).substr(line.rfind(',') + 1));
            const std::filesystem::path relative(name);
            if (name.empty() || relative.has_root_path() ||
                std::find(relative.begin(), relative.end(), std::filesystem::path("..")) !=
                    relative.end())
                throw std::runtime_error("invalid IW3 rawfile declaration name: " + name);
            names.insert(name);
        }
        if (input.bad())
            throw std::runtime_error("cannot read IW3 rawfile zone declarations");
    }
    if (error)
        throw std::runtime_error("cannot enumerate IW3 rawfile zone declarations");

    std::vector<PreparedRawFile> rawFiles;
    rawFiles.reserve(names.size());
    std::size_t sourceScripts = 0;
    for (const auto &name : names)
    {
        // IW3 GSC text is not a Replay ScriptFile. Native map startup is built
        // separately; importing these source scripts as RawFiles leaves a stale
        // IW3 implementation in the target package without converting it.
        if (std::filesystem::path(name).extension() == ".gsc")
        {
            ++sourceScripts;
            continue;
        }
        // IW3 vision text (r_film*, r_glow*) is not Replay's 218-field vision
        // format; PrepareFastfile emits a converted vision file instead.
        if (std::filesystem::path(name).extension() == ".vision")
            continue;
        PreparedRawFile rawFile;
        rawFile.name = name;
        if (!zt::read_file((root / std::filesystem::path(name)).string(), rawFile.data))
            throw std::runtime_error("map-local IW3 rawfile payload is missing: " + name);
        if (rawFile.data.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("map-local IW3 rawfile payload is too large: " + name);
        rawFiles.push_back(std::move(rawFile));
    }
    zt::info("iw3: omitted %zu IW3 GSC source script(s)", sourceScripts);
    zt::info("iw3: prepared %zu map-local rawfile asset(s)", rawFiles.size());
    return rawFiles;
}

// Physics presets named by the map's dynamic models that are not exported yet
// (shared presets such as ",bucket_metal" live in common_mp).
std::vector<std::string> MissingPhysicsPresets(const std::filesystem::path &root)
{
    std::set<std::string> missing;
    std::error_code error;
    const auto directory = root / "maps" / "mp";
    if (!std::filesystem::is_directory(directory, error))
        return {};
    for (const auto &entry : std::filesystem::directory_iterator(directory, error))
    {
        if (!entry.path().string().ends_with(".replay-collision.json"))
            continue;
        const Json collision = ReadJson(entry.path());
        for (const auto &definition : collision.value("dynamic_models", Json::array()))
        {
            if (!definition.contains("physics_preset") || !definition.at("physics_preset").is_string())
                continue;
            std::string name = definition.at("physics_preset").get<std::string>();
            while (!name.empty() && name.front() == ',')
                name.erase(name.begin());
            if (!name.empty() && name.find_first_of("/\\") == std::string::npos &&
                !std::filesystem::is_regular_file(root / "physic" / name, error))
                missing.insert(name);
        }
    }
    return {missing.begin(), missing.end()};
}

std::vector<std::string> MissingDeclaredFx(const std::filesystem::path &root)
{
    std::set<std::string> missing;
    std::error_code error;
    const auto zoneRoot = root / "zone_source";
    if (!std::filesystem::is_directory(zoneRoot, error))
        return {};
    for (std::filesystem::directory_iterator iterator(zoneRoot, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".zone")
            continue;
        for (const auto &name : ReadDeclaredFx(iterator->path()))
            if (!std::filesystem::is_regular_file(root / "fx" / (name + ".iw3.json"), error))
                missing.insert(name);
    }
    if (error)
        throw std::runtime_error("cannot enumerate IW3 FX zone declarations");
    return {missing.begin(), missing.end()};
}

bool IsKnownIw3SourceAssetType(const std::string_view type)
{
    // These names are the canonical IW3 names emitted by OpenAssetTools'
    // ZoneDefWriterIW3. Keep this list closed: an unrecognised declaration
    // must not disappear silently when a new Unlinker build adds an asset.
    constexpr std::array<std::string_view, 33> knownTypes{
        "xmodelpieces", "physpreset",       "xanim",      "xmodel",   "material",
        "techniqueset", "image",            "sound",      "soundcurve", "loadedsound",
        "clipmap_unused", "clipmap",         "comworld",   "gameworldsp", "gameworldmp",
        "mapents",      "gfxworld",         "lightdef",   "uimap",      "font",
        "menulist",     "menu",             "localize",   "weapon",     "snddriverglobals",
        "fx",           "impactfx",         "aitype",     "mptype",     "character",
        "xmodelalias",  "rawfile",          "stringtable"};
    return std::find(knownTypes.begin(), knownTypes.end(), type) != knownTypes.end();
}

bool IsConsumedIw3SourceAssetType(const std::string_view type)
{
    // ReplayMapDumpers exports the roots and entity text, while build-iw3
    // converts the reachable model/material/image dependency graph. This is a
    // source-consumption classification, not a claim that every declaration of
    // a dependency type becomes a top-level Replay asset.
    constexpr std::array<std::string_view, 9> consumedTypes{
        "clipmap", "comworld", "gameworldmp", "mapents",
        "gfxworld", "image", "impactfx", "material", "xmodel"};
    return std::find(consumedTypes.begin(), consumedTypes.end(), type) != consumedTypes.end();
}

std::string_view SourceAssetAuditReason(const std::string_view type)
{
    if (type == "techniqueset")
        return "source techniquesets are not portable; reachable materials use matched Replay "
               "material and techniqueset contracts";
    if (type == "fx")
        return "effects the map never uses (no script, createfx, impact-table, destructible or "
               "child reference) or that an \"FX ... not converted\" line names";
    if (type == "sound" || type == "soundcurve" || type == "loadedsound")
        return "only aliases the map uses (scripts, createfx, ambient, destructibles), their "
               "audio and falloff curves go into the map bank";
    if (type == "physpreset")
        return "presets convert as simulated clutter on the dynamic models that use them; "
               "unused presets are not converted";
    if (type == "lightdef")
        return "only map-local light_point_linear has a matched Replay LightDef conversion";
    if (type == "gameworldsp")
        return "build-iw3 accepts multiplayer map roots only";
    if (type == "rawfile")
        return "map-local non-script raw files are imported; IW3 GSC scripts are intentionally "
               "omitted and shared-zone declarations remain stock dependencies";
    return "the declaration has no native build-iw3 consumer";
}

std::string TrimSourceAssetField(const std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string SourceAssetExamples(const std::set<std::string> &names)
{
    std::string result;
    std::size_t count = 0;
    for (const auto &name : names)
    {
        if (!result.empty())
            result += ", ";
        result += name;
        if (++count == 3)
            break;
    }
    if (names.size() > count)
        result += ", ...";
    return result;
}

void AuditSourceAssetDeclarations(const std::filesystem::path &root,
                                  const std::set<std::string> &emittedFx,
                                  const std::set<std::string> &emittedRawFiles,
                                  const bool emittedLinearLightDef)
{
    const auto zoneRoot = root / "zone_source";
    std::error_code error;
    if (!std::filesystem::is_directory(zoneRoot, error))
    {
        throw std::runtime_error(
            "OpenAssetTools did not produce IW3 zone declarations; source assets cannot be "
            "accounted for");
    }

    std::vector<std::filesystem::path> zoneFiles;
    for (std::filesystem::directory_iterator iterator(zoneRoot, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".zone")
            continue;
        zoneFiles.push_back(iterator->path());
    }
    if (error)
        throw std::runtime_error("cannot enumerate OpenAssetTools IW3 zone declarations");
    if (zoneFiles.empty())
    {
        throw std::runtime_error(
            "OpenAssetTools produced an empty IW3 zone_source directory; source assets cannot "
            "be accounted for");
    }
    std::sort(zoneFiles.begin(), zoneFiles.end());

    std::map<std::string, std::set<std::string>> declarations;
    for (const auto &zoneFile : zoneFiles)
    {
        std::ifstream input(zoneFile, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot read OpenAssetTools IW3 zone declaration");

        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(input, line))
        {
            ++lineNumber;
            if (lineNumber == 1 && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
            {
                line.erase(0, 3);
            }

            const auto firstComma = line.find(',');
            if (firstComma == std::string::npos)
                continue;
            std::string type = TrimSourceAssetField(std::string_view(line).substr(0, firstComma));
            if (type.empty() || type.front() == '#' || type.front() == '/' || type.front() == '>')
                continue;
            std::transform(type.begin(), type.end(), type.begin(), [](const char character) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            });
            if (!IsKnownIw3SourceAssetType(type))
            {
                throw std::runtime_error("OpenAssetTools emitted unknown IW3 source asset type '" +
                                         type + "' in zone declaration; update the closed "
                                               "build-iw3 asset classification");
            }

            const auto lastComma = line.rfind(',');
            std::string name = TrimSourceAssetField(
                std::string_view(line).substr(lastComma == std::string::npos ? firstComma + 1
                                                                              : lastComma + 1));
            if (name.empty())
                name = "<unnamed>";
            declarations[std::move(type)].insert(std::move(name));
        }
        if (input.bad())
            throw std::runtime_error("cannot read OpenAssetTools IW3 zone declaration");
    }

    std::size_t declarationCount = 0;
    std::string summary;
    for (const auto &[type, names] : declarations)
    {
        declarationCount += names.size();
        if (!summary.empty())
            summary += ", ";
        summary += type + "=" + std::to_string(names.size());
    }
    zt::info("iw3: accounted for %zu unique source asset declarations across %zu zone file(s): %s",
             declarationCount, zoneFiles.size(), summary.c_str());

    for (const auto &[type, names] : declarations)
    {
        if (IsConsumedIw3SourceAssetType(type))
            continue;
        std::set<std::string> unconsumed(names);
        if (type == "fx")
            for (const auto &name : emittedFx)
                unconsumed.erase(name);
        if (type == "rawfile")
        {
            for (const auto &name : emittedRawFiles)
                unconsumed.erase(name);
            for (const auto &name : names)
                if (std::filesystem::path(name).extension() == ".gsc")
                    unconsumed.erase(name);
        }
        if (type == "lightdef" && emittedLinearLightDef)
            unconsumed.erase("light_point_linear");
        if (unconsumed.empty())
            continue;
        const auto examples = SourceAssetExamples(unconsumed);
        zt::warn("iw3: source type '%s' has %zu declaration(s) outside the native map package "
                 "(%s); examples: %s",
                 type.c_str(), unconsumed.size(), SourceAssetAuditReason(type).data(),
                 examples.c_str());
    }
}

Json ReadJson(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("cannot read " + path.string());
    }
    return Json::parse(input);
}

void WriteJson(const std::filesystem::path &path, const Json &value)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("cannot write " + path.string());
    }
    output << value;
    if (!output)
    {
        throw std::runtime_error("cannot finish " + path.string());
    }
}

Surface ReadWorldSurface(const Json &source)
{
    Surface output;
    output.material = source.at("material").get<std::string>();
    output.lightmap = source.value("lightmap", -1);
    // IW3 reserves the five-bit value 31 for an unlightmapped world surface.
    // Keep real indices range-checked later, but do not treat this sentinel as
    // a reference to a missing atlas entry.
    if (output.lightmap == 31)
        output.lightmap = -1;
    const auto reflectionProbe = source.at("reflection_probe").get<unsigned>();
    if (reflectionProbe > UINT16_MAX)
        throw std::runtime_error("IW3 surface reflection-probe index exceeds native width");
    output.reflectionProbe = static_cast<std::uint16_t>(reflectionProbe);
    const auto &vertices = source.at("vertices");
    const auto &indices = source.at("indices");
    if (!vertices.is_array() || vertices.empty() || !indices.is_array() || indices.size() % 3 != 0)
    {
        throw std::runtime_error("invalid IW3 world surface");
    }

    output.vertices.reserve(vertices.size());
    for (const auto &item : vertices)
    {
        Vertex vertex;
        vertex.position = ReadVector<3>(item.at("position"));
        vertex.normal = Unit(ReadVector<3>(item.at("normal")));
        vertex.tangent = ReadVector<3>(item.at("tangent"));
        if (Dot(vertex.tangent, vertex.tangent) < 1.0e-12f)
            vertex.tangent = Cross(
                std::abs(vertex.normal[2]) < 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0}, vertex.normal);
        vertex.tangent = Unit(vertex.tangent);
        vertex.binormalSign = item.at("binormal_sign").get<float>();
        if (!std::isfinite(vertex.binormalSign) || std::abs(vertex.binormalSign) < 0.5f)
            throw std::runtime_error("invalid IW3 vertex tangent handedness");
        vertex.binormalSign = vertex.binormalSign < 0 ? -1.0f : 1.0f;
        vertex.authoredTangent = true;
        vertex.uv = ReadVector<2>(item.at("uv"));
        vertex.lightmapUv = ReadVector<2>(item.at("lightmap_uv"));
        if (item.contains("color"))
        {
            const auto &color = item.at("color");
            if (!color.is_array() || color.size() != 4)
            {
                throw std::runtime_error("invalid IW3 vertex color");
            }
            for (std::size_t channel = 0; channel < 4; ++channel)
            {
                const int value = color.at(channel).get<int>();
                if (value < 0 || value > 255)
                {
                    throw std::runtime_error("invalid IW3 vertex color");
                }
                vertex.color[channel] = static_cast<std::uint8_t>(value);
            }
        }
        output.vertices.push_back(vertex);
    }
    output.indices.reserve(indices.size());
    for (const auto &item : indices)
    {
        const auto index = item.get<std::uint32_t>();
        if (index >= output.vertices.size())
        {
            throw std::runtime_error("IW3 surface index is outside its vertex array");
        }
        output.indices.push_back(index);
    }
    return output;
}

// IW3 culls by index winding alone; vertex normals never change which side is
// drawn. Orient a surface as a whole by majority vote. Flipping single
// triangles whose smoothed normals disagree with their geometry (the narrow
// edges of thin brush panels) made those faces back-facing in Replay.
void AlignWinding(Surface &surface)
{
    std::ptrdiff_t vote = 0;
    for (std::size_t index = 0; index < surface.indices.size(); index += 3)
    {
        const Vertex &a = surface.vertices[surface.indices[index]];
        const Vertex &b = surface.vertices[surface.indices[index + 1]];
        const Vertex &c = surface.vertices[surface.indices[index + 2]];
        const Vec3 face = Cross(Subtract(b.position, a.position), Subtract(c.position, a.position));
        vote += Dot(face, Add(Add(a.normal, b.normal), c.normal)) < 0.0f ? -1 : 1;
    }
    if (vote < 0)
        for (std::size_t index = 0; index < surface.indices.size(); index += 3)
            std::swap(surface.indices[index + 1], surface.indices[index + 2]);
}

std::filesystem::path SafeChild(const std::filesystem::path &root,
                                const std::filesystem::path &relative)
{
    if (relative.is_absolute())
    {
        throw std::runtime_error("absolute path in IW3 map export");
    }
    const auto canonicalRoot = std::filesystem::weakly_canonical(root);
    const auto candidate = std::filesystem::weakly_canonical(root / relative);
    const auto mismatch = std::mismatch(canonicalRoot.begin(), canonicalRoot.end(),
                                        candidate.begin(), candidate.end());
    if (mismatch.first != canonicalRoot.end())
    {
        throw std::runtime_error("path escapes the IW3 extraction directory");
    }
    return candidate;
}

VisibilityGroups ReadVisibilityGroups(const Json &world)
{
    const auto &surfaces = world.at("surfaces");
    const auto &models = world.value("models", Json::array());
    const auto &dpvs = world.at("dpvs");
    const auto &cells = dpvs.at("cells");
    const auto &planes = dpvs.at("planes");
    const auto &nodes = dpvs.at("nodes");
    // Large IW3 maps (mp_arnhem) exceed 65535 BSP planes and nodes; Replay
    // receives one merged cell, so only the cell count is bounded here.
    if (!cells.is_array() || cells.empty() || cells.size() > 65535 || !planes.is_array() ||
        !nodes.is_array() || nodes.empty())
        throw std::runtime_error("invalid IW3 DPVS topology");

    VisibilityGroups result;
    result.cellTrees.resize(cells.size());
    result.surfaces.assign(surfaces.size(), VisibilityGroups::Unassigned);
    result.models.assign(models.size(), VisibilityGroups::Unassigned);
    result.nativeTopology = !cells.at(0).at("trees").empty() &&
                            cells.at(0).at("trees").at(0).contains("childCount");
    std::vector<std::vector<std::uint32_t>> surfaceTrees;
    if (result.nativeTopology)
        surfaceTrees.resize(surfaces.size());
    const auto assign = [](std::vector<std::uint32_t> &groups, const std::size_t index,
                           const std::uint32_t tree) {
        if (index >= groups.size())
            throw std::runtime_error("IW3 DPVS ownership index is outside its array");
        if (groups[index] == VisibilityGroups::Unassigned)
            groups[index] = tree;
        else if (groups[index] != tree)
            groups[index] = VisibilityGroups::Global;
    };
    for (std::size_t cellIndex = 0; cellIndex < cells.size(); ++cellIndex)
    {
        const auto &cell = cells.at(cellIndex);
        const auto &trees = cell.at("trees");
        if (!trees.is_array())
            throw std::runtime_error("invalid IW3 DPVS AABB trees");
        for (std::size_t treeIndex = 0; treeIndex < trees.size(); ++treeIndex)
        {
            const auto &tree = trees.at(treeIndex);
            ++result.sourceTreeCount;
            const auto &treeSurfaces = tree.at("surfaces");
            const auto &treeModels = tree.at("models");
            if (!treeSurfaces.is_array() || !treeModels.is_array())
                throw std::runtime_error("invalid IW3 DPVS AABB ownership");
            if (!result.nativeTopology && treeSurfaces.empty() && treeModels.empty())
                continue;
            if (result.treeBounds.size() >= VisibilityGroups::Unassigned)
                throw std::runtime_error("IW3 DPVS has too many AABB trees");
            Json bounds = tree.at("bounds");
            if (!bounds.is_array() || bounds.size() != 2)
                throw std::runtime_error("invalid IW3 DPVS AABB bounds");
            const Vec3 minimum = ReadVector<3>(bounds.at(0));
            const Vec3 maximum = ReadVector<3>(bounds.at(1));
            for (std::size_t axis = 0; axis < minimum.size(); ++axis)
                if (minimum[axis] > maximum[axis])
                {
                    if (!result.nativeTopology || !treeSurfaces.empty() ||
                        !treeModels.empty() || tree.at("childCount").get<unsigned>())
                        throw std::runtime_error("reversed IW3 DPVS AABB bounds");
                    bounds = cell.at("bounds");
                    break;
                }

            const auto group = static_cast<std::uint32_t>(result.treeBounds.size());
            result.treeBounds.push_back(bounds);
            if (result.nativeTopology)
            {
                const auto childCount = tree.at("childCount").get<unsigned>();
                const auto firstChild = tree.at("firstChild").get<unsigned>();
                if (childCount > UINT16_MAX ||
                    (childCount && (firstChild <= treeIndex ||
                                    firstChild + childCount > trees.size())) ||
                    (!childCount && firstChild))
                    throw std::runtime_error("invalid IW3 DPVS AABB child range");
                result.treeChildCounts.push_back(static_cast<std::uint16_t>(childCount));
                result.treeFirstChildren.push_back(static_cast<std::uint32_t>(firstChild));
            }
            std::vector<std::uint32_t> ownedModels;
            ownedModels.reserve(treeModels.size());
            for (const auto &model : treeModels)
            {
                const auto index = model.get<std::size_t>();
                assign(result.models, index, group);
                ownedModels.push_back(static_cast<std::uint32_t>(index));
            }
            result.treeModels.push_back(std::move(ownedModels));
            result.cellTrees[cellIndex].push_back(group);
            for (const auto &surface : treeSurfaces)
            {
                const auto index = surface.get<std::size_t>();
                if (result.nativeTopology)
                {
                    if (index >= surfaceTrees.size())
                        throw std::runtime_error("IW3 DPVS surface index is outside its array");
                    surfaceTrees[index].push_back(group);
                }
                else
                    assign(result.surfaces, index, group);
            }
        }
        if (result.nativeTopology && !trees.empty())
        {
            std::vector<bool> visited(trees.size());
            std::vector<std::size_t> pending{0};
            while (!pending.empty())
            {
                const auto index = pending.back();
                pending.pop_back();
                if (visited.at(index))
                    throw std::runtime_error("IW3 DPVS AABB tree has repeated descendants");
                visited[index] = true;
                const auto &tree = trees.at(index);
                const auto first = tree.at("firstChild").get<std::size_t>();
                const auto count = tree.at("childCount").get<std::size_t>();
                for (std::size_t child = first; child < first + count; ++child)
                    pending.push_back(child);
            }
            if (std::ranges::find(visited, false) != visited.end())
                throw std::runtime_error("IW3 DPVS AABB tree has unreachable nodes");
        }
    }
    if (result.nativeTopology)
    {
        std::map<std::vector<std::uint32_t>, std::uint32_t> membershipGroups;
        for (std::size_t index = 0; index < surfaceTrees.size(); ++index)
        {
            auto &members = surfaceTrees[index];
            if (members.empty())
                continue;
            std::ranges::sort(members);
            members.erase(std::unique(members.begin(), members.end()), members.end());
            const auto [it, inserted] = membershipGroups.try_emplace(
                members, static_cast<std::uint32_t>(result.groupTrees.size()));
            if (inserted)
                result.groupTrees.push_back(members);
            result.surfaces[index] = it->second;
        }
    }
    for (auto &group : result.surfaces)
        if (group == VisibilityGroups::Unassigned)
            group = VisibilityGroups::Global;
    for (auto &group : result.models)
        if (group == VisibilityGroups::Unassigned)
            group = VisibilityGroups::Global;
    return result;
}

// IW3 tuned LOD and cull distances for its own LOD metric; Replay scales view
// distance more aggressively, so IW3 models dropped detail and culled early.
// Owner-tuned: keep IW3's ratios and stretch every distance by this factor.
constexpr float kIw3ToReplayLodScale = 2.0f;
constexpr float kReplayNeverCulled = 1000000.0f;
// Owner-tuned floor for a model's final draw distance. IW3 culled thin props
// such as mil_barbedwire3 at 900 units, which pops visibly on small maps.
constexpr float kReplayMinimumDrawDistance = 4000.0f;

float ReplayLodDistance(const float distance)
{
    return distance >= kReplayNeverCulled ? kReplayNeverCulled
                                          : std::min(distance * kIw3ToReplayLodScale,
                                                     kReplayNeverCulled);
}

// One LOD of an OpenAssetTools XMODEL_EXPORT (version 6): the model's bind
// skeleton and, per object (one per source XSurface, in order), each
// triangle's corners as model-space position and dominant bone.
struct SourceSkeletonLod
{
    struct Corner
    {
        Vec3 position{};
        std::uint8_t bone{};
    };
    std::vector<SourceBone> bones;
    std::vector<std::vector<std::array<Corner, 3>>> objects;
};

SourceSkeletonLod ReadXModelExport(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    if (!stream)
        throw std::runtime_error("cannot read IW3 skeleton export " + path.string());
    std::vector<std::string> lines;
    for (std::string line; std::getline(stream, line);)
    {
        std::ranges::replace(line, ',', ' ');
        if (line.find_first_not_of(" \t\r") != std::string::npos && !line.starts_with("//"))
            lines.push_back(std::move(line));
    }
    std::size_t next = 0;
    const auto fail = [&path](const std::string &what) {
        return std::runtime_error("malformed IW3 skeleton export " + path.filename().string() +
                                  ": " + what);
    };
    // The fields after the next line's keyword, which must be `keyword`.
    const auto read = [&](const std::string &keyword) {
        if (next >= lines.size())
            throw fail("missing " + keyword);
        std::istringstream fields(lines[next++]);
        std::string word;
        fields >> word;
        if (word != keyword)
            throw fail("expected " + keyword + ", found " + word);
        return fields;
    };
    const auto seek = [&](const std::string &keyword) {
        while (next < lines.size() && !lines[next].starts_with(keyword + ' '))
            ++next;
        return read(keyword);
    };
    const auto readCount = [&](std::istringstream fields, const std::size_t limit) {
        std::size_t count = 0;
        fields >> count;
        if (!fields || count > limit)
            throw fail("invalid count");
        return count;
    };
    const auto readVector = [&](const std::string &keyword) {
        auto fields = read(keyword);
        Vec3 value{};
        fields >> value[0] >> value[1] >> value[2];
        if (!fields || !std::isfinite(value[0]) || !std::isfinite(value[1]) ||
            !std::isfinite(value[2]))
            throw fail("invalid " + keyword);
        return value;
    };

    SourceSkeletonLod result;
    const std::size_t boneCount = readCount(seek("NUMBONES"), 128);
    if (!boneCount)
        throw fail("no bones");
    result.bones.resize(boneCount);
    for (std::size_t index = 0; index < boneCount; ++index)
    {
        auto fields = read("BONE");
        std::size_t number = 0;
        int parent = 0;
        std::string name;
        fields >> number >> parent >> std::quoted(name);
        // Single-bone exports name their bone "".
        if (!fields || number != index || parent < -1 || parent >= static_cast<int>(index))
            throw fail("invalid bone " + std::to_string(index));
        result.bones[index].name = std::move(name);
        result.bones[index].parent = parent;
    }
    for (std::size_t index = 0; index < boneCount; ++index)
    {
        if (readCount(read("BONE"), boneCount) != index)
            throw fail("bone poses out of order");
        SourceBone &bone = result.bones[index];
        bone.origin = readVector("OFFSET");
        const Vec3 scale = readVector("SCALE");
        if (std::abs(scale[0] - 1.0f) > 1.0e-3f || std::abs(scale[1] - 1.0f) > 1.0e-3f ||
            std::abs(scale[2] - 1.0f) > 1.0e-3f)
            throw fail("scaled bone " + bone.name);
        bone.axis[0] = readVector("X");
        bone.axis[1] = readVector("Y");
        bone.axis[2] = readVector("Z");
    }

    const std::size_t vertexCount = readCount(seek("NUMVERTS"), 1u << 24);
    std::vector<SourceSkeletonLod::Corner> vertices(vertexCount);
    for (std::size_t index = 0; index < vertexCount; ++index)
    {
        if (readCount(read("VERT"), vertexCount) != index)
            throw fail("vertices out of order");
        vertices[index].position = readVector("OFFSET");
        const std::size_t weights = readCount(read("BONES"), 16);
        if (!weights)
            throw fail("unweighted vertex");
        float strongest = -1.0f;
        for (std::size_t weight = 0; weight < weights; ++weight)
        {
            auto fields = read("BONE");
            std::size_t bone = 0;
            float value = 0.0f;
            fields >> bone >> value;
            if (!fields || bone >= boneCount || !std::isfinite(value))
                throw fail("invalid vertex weight");
            if (value > strongest)
            {
                strongest = value;
                vertices[index].bone = static_cast<std::uint8_t>(bone);
            }
        }
    }

    const std::size_t faceCount = readCount(seek("NUMFACES"), 1u << 24);
    for (std::size_t face = 0; face < faceCount; ++face)
    {
        auto fields = read("TRI");
        std::size_t object = 0;
        fields >> object;
        if (!fields || object > UINT16_MAX)
            throw fail("invalid face object");
        if (result.objects.size() <= object)
            result.objects.resize(object + 1);
        std::array<SourceSkeletonLod::Corner, 3> triangle{};
        for (auto &corner : triangle)
        {
            const std::size_t vertex = readCount(read("VERT"), vertexCount);
            if (vertex >= vertexCount)
                throw fail("face references a missing vertex");
            corner = vertices[vertex];
            read("NORMAL");
            read("COLOR");
            read("UV");
        }
        result.objects[object].push_back(triangle);
    }
    return result;
}

// Every LOD of `modelName` from the skeleton export, when the model has more
// than one bone. Without an export the model keeps a single root bone.
std::vector<SourceSkeletonLod> ReadSourceSkeleton(const std::filesystem::path &skeletonRoot,
                                                  const std::string &modelName,
                                                  const std::size_t lodCount)
{
    if (skeletonRoot.empty())
        return {};
    std::error_code error;
    const auto modelPath =
        SafeChild(skeletonRoot, std::filesystem::path("xmodel") / (modelName + ".json"));
    if (!std::filesystem::is_regular_file(modelPath, error))
    {
        zt::warn("iw3: no skeleton export for entity model %s; it keeps one bone",
                 modelName.c_str());
        return {};
    }
    const Json model = ReadJson(modelPath);
    if (!model.contains("lods") || !model.at("lods").is_array() ||
        model.at("lods").size() != lodCount)
        throw std::runtime_error("IW3 skeleton export does not match the model LODs: " +
                                 modelName);
    std::vector<SourceSkeletonLod> lods;
    for (const auto &lod : model.at("lods"))
        lods.push_back(ReadXModelExport(
            SafeChild(skeletonRoot, std::filesystem::path(lod.at("file").get<std::string>()))));
    if (lods.front().bones.size() < 2)
        return {};
    for (const auto &bone : lods.front().bones)
        if (bone.name.empty())
            throw std::runtime_error("IW3 skeleton export has an unnamed bone: " + modelName);
    for (const auto &lod : lods)
    {
        const bool same = std::ranges::equal(
            lod.bones, lods.front().bones, [](const SourceBone &a, const SourceBone &b) {
                return a.name == b.name && a.parent == b.parent;
            });
        if (!same)
            throw std::runtime_error("IW3 model LODs have different skeletons: " + modelName);
    }
    return lods;
}

// Gives each vertex of source surface `object` its IW3 bone. OpenAssetTools
// writes one object per XSurface with that surface's triangles in order, so
// triangle t is face t of the object; corners match by position because the
// export merges vertices that share a position and weights.
bool AssignSourceBones(const SourceSkeletonLod &skeleton, const std::size_t object,
                       Surface &surface)
{
    if (object >= skeleton.objects.size())
        return false;
    const auto &triangles = skeleton.objects[object];
    if (triangles.empty() || triangles.size() * 3 != surface.indices.size())
        return false;
    constexpr std::uint8_t unassigned = UINT8_MAX;
    surface.vertexBones.assign(surface.vertices.size(), unassigned);
    for (std::size_t triangle = 0; triangle < triangles.size(); ++triangle)
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
            const auto index = surface.indices[triangle * 3 + corner];
            if (index >= surface.vertices.size())
                return false;
            const Vec3 &position = surface.vertices[index].position;
            const auto match =
                std::ranges::find_if(triangles[triangle], [&position](const auto &source) {
                    return std::abs(source.position[0] - position[0]) < 1.0e-3f &&
                           std::abs(source.position[1] - position[1]) < 1.0e-3f &&
                           std::abs(source.position[2] - position[2]) < 1.0e-3f;
                });
            if (match == triangles[triangle].end())
                return false;
            if (surface.vertexBones[index] == unassigned)
                surface.vertexBones[index] = match->bone;
        }
    // Vertices no triangle uses follow the surface's first bone.
    const std::uint8_t fallback = surface.vertexBones[surface.indices.front()];
    for (auto &bone : surface.vertexBones)
        if (bone == unassigned)
            bone = fallback;
    return true;
}

SourceStaticModel ReadSourceModel(const std::filesystem::path &root, const std::string &modelName,
                                  const bool placedStaticBindPose = false,
                                  const std::filesystem::path &skeletonRoot = {})
{
    const auto modelPath = SafeChild(root, std::filesystem::path("xmodel") / (modelName + ".json"));
    const Json model = ReadJson(modelPath);
    const auto modelType = model.value("type", std::string{});
    // Entity models (with a skeleton root) also take an animated model's bind
    // pose: IW3 xanims are not converted (mp_greenband's swaying lanterns).
    const bool bindPose = placedStaticBindPose || !skeletonRoot.empty();
    if ((modelType != "rigid" && !(bindPose && modelType == "animated")) ||
        !model.contains("lods") ||
        model.at("lods").empty() || model.at("lods").size() > 6)
    {
        throw std::runtime_error("IW3 model has unsupported render LODs: " + modelName);
    }

    SourceStaticModel converted;
    converted.name = modelName;
    const int collisionLod = model.value("collLod", -1);
    if (collisionLod < -1 || collisionLod >= static_cast<int>(model.at("lods").size()))
        throw std::runtime_error("IW3 model has an invalid collision LOD: " + modelName);
    if (collisionLod >= 0)
        converted.collisionLod = static_cast<std::size_t>(collisionLod);
    const auto geometryPath =
        SafeChild(root, std::filesystem::path("xmodel") / (modelName + ".replay.json"));
    if (!std::filesystem::is_regular_file(geometryPath))
        throw std::runtime_error("IW3 model attributes are missing for '" + modelName +
                                 "'; configure and rebuild the bundled OpenAssetTools Unlinker");
    const Json geometry = ReadJson(geometryPath);
    if (geometry.value("schema", 0) != 1 || geometry.at("name") != modelName ||
        !geometry.at("lods").is_array() || geometry.at("lods").size() != model.at("lods").size())
        throw std::runtime_error("IW3 model geometry does not match its LOD table: " + modelName);
    const auto skeleton = ReadSourceSkeleton(skeletonRoot, modelName, model.at("lods").size());
    if (!skeleton.empty())
        converted.bones = skeleton.front().bones;
    float previousDistance = 0.0f;
    for (const auto &sourceLod : model.at("lods"))
    {
        SourceStaticModelLod lod;
        lod.distance = sourceLod.at("distance").get<float>();
        if (!std::isfinite(lod.distance) || lod.distance < previousDistance)
            throw std::runtime_error("IW3 model has invalid LOD distances: " + modelName);
        previousDistance = lod.distance;
        const auto &geometryLod = geometry.at("lods").at(converted.lods.size());
        if (geometryLod.at("distance").get<float>() != lod.distance ||
            !geometryLod.at("surfaces").is_array() || geometryLod.at("surfaces").empty())
            throw std::runtime_error("IW3 model has an empty render LOD: " + modelName);
        std::size_t sourceObject = 0;
        for (const auto &sourceSurface : geometryLod.at("surfaces"))
        {
            const std::size_t object = sourceObject++;
            // OAT's IW3 model exporter writes verts0.xyz directly. Its GLTF bind
            // pose uses each baseMat and its inverse, so a placed static model's
            // deformed vertices have these exact model-space positions at rest.
            if (sourceSurface.at("deformed").get<bool>() && !bindPose)
                throw std::runtime_error("IW3 placed model requires skinning: " + modelName);
            Surface surface = ReadWorldSurface(sourceSurface);
            // IW3 destructible base models hold every damage state and hide the
            // damaged ones per bone. With the source skeleton every state stays
            // (scripts hide and show the parts); a one-bone conversion keeps only
            // the intact state (shattered glass, burnt panels removed).
            if (!converted.bones.empty())
            {
                if (!AssignSourceBones(skeleton[converted.lods.size()], object, surface))
                    throw std::runtime_error(
                        "IW3 model skeleton export does not match its geometry: " + modelName);
            }
            else if (modelName.find("_destructible") != std::string::npos)
            {
                const std::string material = Lowercase(surface.material);
                if (material.find("shattered") != std::string::npos ||
                    material.find("destroy") != std::string::npos ||
                    material.ends_with("_dam"))
                    continue;
            }
            // IW3 and Replay XSurfaces use the same clockwise winding. World-surface
            // normal alignment would turn these native model faces inside out.
            lod.surfaces.push_back(std::move(surface));
        }
        if (lod.surfaces.empty())
            throw std::runtime_error("IW3 model has no intact surfaces: " + modelName);
        converted.lods.push_back(std::move(lod));
    }
    for (auto &lod : converted.lods)
        lod.distance = ReplayLodDistance(lod.distance);
    converted.lods.back().distance =
        std::max(converted.lods.back().distance, kReplayMinimumDrawDistance);
    // r_forceLod N draws nothing for a model with N or fewer LODs. Pad to
    // Replay's six LODs with copies of the lowest LOD that each own a sliver
    // just below the final distance, so normal LOD selection is unchanged.
    constexpr std::size_t replayLodCount = 6;
    const std::size_t sourceLods = converted.lods.size();
    std::size_t totalSurfaces = 0;
    for (const auto &lod : converted.lods)
        totalSurfaces += lod.surfaces.size();
    // Replay static models index at most 255 surfaces across all LODs.
    const std::size_t lowestSurfaces = converted.lods.back().surfaces.size();
    const std::size_t affordable =
        totalSurfaces >= 255 ? 0 : (255 - totalSurfaces) / std::max<std::size_t>(lowestSurfaces, 1);
    const std::size_t padding =
        sourceLods < replayLodCount ? std::min(replayLodCount - sourceLods, affordable) : 0;
    if (padding)
    {
        const float last = converted.lods.back().distance;
        const float previous = sourceLods > 1 ? converted.lods[sourceLods - 2].distance : 0.0f;
        const float step = std::min(1.0f, (last - previous) / 8.0f);
        converted.lods.back().distance = last - static_cast<float>(padding) * step;
        for (std::size_t index = 0; index < padding; ++index)
        {
            SourceStaticModelLod copy = converted.lods[sourceLods - 1];
            copy.distance = last - static_cast<float>(padding - 1 - index) * step;
            converted.lods.push_back(std::move(copy));
        }
    }
    return converted;
}

// Replay's ladder IK advances its hand target by 12 units.  The IW3
// com_ladder_wood render mesh has authored rung bands every 24 units.  Keep
// the source model intact for its collision LOD, and add only the five
// missing render rung bands to each known authored LOD.
void AddLadderMidpointRungs(Surface &surface, const std::size_t lodIndex)
{
    constexpr std::array<float, 5> sourceRungCenters{24.0f, 48.0f, 72.0f, 96.0f, 120.0f};
    constexpr std::array<std::size_t, 2> expectedVertexCounts{232, 144};
    constexpr std::array<std::size_t, 2> expectedTriangleCounts{128, 64};
    constexpr float rungHalfExtent = 4.0f;

    if (lodIndex >= expectedVertexCounts.size() || surface.vertices.size() != expectedVertexCounts[lodIndex] ||
        surface.indices.size() != expectedTriangleCounts[lodIndex] * 3)
    {
        throw std::runtime_error("IW3 com_ladder_wood geometry does not match the midpoint-rung contract");
    }

    std::array<std::vector<std::array<std::uint32_t, 3>>, sourceRungCenters.size()> rungTriangles;
    for (std::size_t index = 0; index < surface.indices.size(); index += 3)
    {
        const std::array<std::uint32_t, 3> triangle{surface.indices[index], surface.indices[index + 1],
                                                    surface.indices[index + 2]};
        const auto &a = surface.vertices.at(triangle[0]);
        const auto &b = surface.vertices.at(triangle[1]);
        const auto &c = surface.vertices.at(triangle[2]);
        const float minimum = (std::min)({a.position[2], b.position[2], c.position[2]});
        const float maximum = (std::max)({a.position[2], b.position[2], c.position[2]});
        for (std::size_t rung = 0; rung < sourceRungCenters.size(); ++rung)
        {
            if (minimum >= sourceRungCenters[rung] - rungHalfExtent &&
                maximum <= sourceRungCenters[rung] + rungHalfExtent)
            {
                rungTriangles[rung].push_back(triangle);
                break;
            }
        }
    }

    const std::size_t expectedPerRung = lodIndex == 0 ? 12 : 8;
    for (const auto &triangles : rungTriangles)
        if (triangles.size() != expectedPerRung)
            throw std::runtime_error("IW3 com_ladder_wood rung bands are incomplete");

    for (std::size_t rung = 0; rung < rungTriangles.size(); ++rung)
        for (const auto &triangle : rungTriangles[rung])
        {
            const auto first = static_cast<std::uint32_t>(surface.vertices.size());
            for (const auto vertexIndex : triangle)
            {
                Vertex vertex = surface.vertices.at(vertexIndex);
                vertex.position[2] += 12.0f;
                surface.vertices.push_back(std::move(vertex));
            }
            surface.indices.insert(surface.indices.end(), {first, first + 1, first + 2});
        }
}

std::array<Vec3, 3> BasisFromQuaternion(const Vec4 &source)
{
    float lengthSquared = 0.0f;
    for (const float value : source)
    {
        if (!std::isfinite(value))
            throw std::runtime_error("IW3 dynamic entity has an invalid quaternion");
        lengthSquared += value * value;
    }
    if (lengthSquared < 0.99f || lengthSquared > 1.01f)
        throw std::runtime_error("IW3 dynamic entity has a non-unit quaternion");
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    const float x = source[0] * inverseLength;
    const float y = source[1] * inverseLength;
    const float z = source[2] * inverseLength;
    const float w = source[3] * inverseLength;
    return {{{1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w), 2.0f * (x * z - y * w)},
             {2.0f * (x * y - z * w), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w)},
             {2.0f * (x * z + y * w), 2.0f * (y * z - x * w), 1.0f - 2.0f * (x * x + y * y)}}};
}

// IW3 physics preset (physic/<name>: "PHYSIC\mass\25\bounce\0.1\...") as a
// Replay clutter body. IW3 and Replay masses are both kilograms. The Replay
// material comes from the preset's sound prefix or name; props up to 2 kg are
// debris (Replay's shipped buckets, cans and bottles weigh 0.5-1.5).
std::optional<iw8::havok::ClutterBody> ReadPhysicsPreset(const std::filesystem::path &root,
                                                         std::string name)
{
    while (!name.empty() && name.front() == ',')
        name.erase(name.begin());
    std::string text;
    if (name.empty() || name.find_first_of("/\\") != std::string::npos ||
        !zt::read_file_str((root / "physic" / name).string(), text))
        return std::nullopt;
    std::vector<std::string> fields;
    std::string field;
    for (const char character : text)
    {
        if (character == '\\')
        {
            fields.push_back(field);
            field.clear();
        }
        else if (character != '\r' && character != '\n')
            field.push_back(character);
    }
    fields.push_back(field);
    if (fields.empty() || fields.front() != "PHYSIC")
        return std::nullopt;
    float mass = 0.0f;
    std::string sound;
    for (std::size_t index = 1; index + 1 < fields.size(); index += 2)
    {
        if (fields[index] == "mass")
            mass = std::strtof(fields[index + 1].c_str(), nullptr);
        else if (fields[index] == "sndAliasPrefix")
            sound = Lowercase(fields[index + 1]);
    }
    if (!std::isfinite(mass) || mass <= 0.0f)
        return std::nullopt;
    const std::string key = sound + " " + Lowercase(name);
    const auto has = [&](const char *word) { return key.find(word) != std::string::npos; };
    // physicsmaterial.hkx name CRCs.
    std::uint32_t material = 0x4B02BC9Du; // PM_Plastic
    if (has("plastic"))
        material = 0x4B02BC9Du;
    else if (has("glass") || has("bottle"))
        material = 0xF728E572u; // PM_Glass
    else if (has("pipe") || has("barrel") || has("extinguisher"))
        material = 0xCBF7A6C4u; // PM_Metal_Solid
    else if (has("metal") || has("tin") || has("can") || has("car_") || has("bucket") ||
             has("helmet") || has("trash"))
        material = 0xE8F3FA9Au; // PM_Metal_Sheet
    else if (has("wood") || has("crate") || has("pallet") || has("picture") || has("frame"))
        material = 0x0AD71E4Eu; // PM_Wood
    else if (has("cardboard") || has("paper") || has("box") || has("book") || has("carton"))
        material = 0xA1F93A3Bu; // PM_Paper
    else if (has("tire") || has("rubber"))
        material = 0xFFD772CDu; // PM_Rubber
    else if (has("brick") || has("cinder"))
        material = 0xD8B39111u; // PM_Brick
    else if (has("concrete") || has("rock") || has("stone"))
        material = 0x1AB7BC33u; // PM_Concrete
    else if (has("cloth") || has("bag") || has("luggage") || has("teddy") || has("pillow"))
        material = 0xCD123193u; // PM_Cloth
    else if (has("ceramic") || has("plate") || has("pot"))
        material = 0xF49C81BFu; // PM_Ceramic
    mass = std::clamp(mass, 0.1f, 200.0f);
    return iw8::havok::ClutterBody{mass, material, mass <= 2.0f};
}

SourceDynamicEntities ReadDynamicEntities(const Json &collision, const std::filesystem::path &root)
{
    SourceDynamicEntities result;
    const auto modelCount = collision.value("dynamic_model_count", std::size_t{});
    const auto brushCount = collision.value("dynamic_brush_count", std::size_t{});
    if (!collision.contains("dynamic_models"))
    {
        if (modelCount)
            throw std::runtime_error(
                "IW3 dynamic definitions are missing; rebuild OpenAssetTools Unlinker");
        return result;
    }
    const auto &definitions = collision.at("dynamic_models");
    if (!definitions.is_array() || definitions.size() != modelCount)
        throw std::runtime_error("invalid IW3 dynamic-model definition table");

    std::unordered_map<std::string, std::size_t> modelCache;
    result.definitionModels.reserve(definitions.size());
    result.quaternions.reserve(definitions.size());
    result.origins.reserve(definitions.size());
    result.models.instances.reserve(definitions.size());
    for (const auto &definition : definitions)
    {
        if (definition.at("type").get<unsigned>() != 1)
            throw std::runtime_error("IW3 destructible dynamic models require Scriptable data");
        std::string modelName = definition.at("model").get<std::string>();
        // Models from another zone carry IW3's ',' reference prefix (mp_agroprom).
        while (!modelName.empty() && modelName.front() == ',')
            modelName.erase(modelName.begin());
        if (modelName.empty())
            throw std::runtime_error("IW3 dynamic model has no XModel");
        auto found = modelCache.find(modelName);
        if (found == modelCache.end())
        {
            const std::size_t modelIndex = result.models.models.size();
            result.models.models.push_back(ReadSourceModel(root, modelName));
            if (definition.contains("physics_preset") && definition.at("physics_preset").is_string())
                result.models.models.back().clutter =
                    ReadPhysicsPreset(root, definition.at("physics_preset").get<std::string>());
            found = modelCache.emplace(modelName, modelIndex).first;
        }

        const Vec4 quaternion = ReadVector<4>(definition.at("quaternion"));
        const Vec3 origin = ReadVector<3>(definition.at("origin"));
        result.definitionModels.push_back(found->second);
        result.quaternions.push_back(quaternion);
        result.origins.push_back(origin);
        result.models.instances.push_back(
            {found->second, origin, BasisFromQuaternion(quaternion), 1.0f});
    }

    if (!collision.contains("dynamic_brushes"))
    {
        if (brushCount)
            throw std::runtime_error(
                "IW3 dynamic brush definitions are missing; rebuild OpenAssetTools Unlinker");
        return result;
    }
    const auto &brushes = collision.at("dynamic_brushes");
    if (!brushes.is_array() || brushes.size() != brushCount)
        throw std::runtime_error("invalid IW3 dynamic-brush definition table");
    const auto submodelCount = collision.at("submodel_count").get<std::size_t>();
    if (submodelCount > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error("IW3 brush-model table exceeds Replay limits");
    result.brushes.reserve(brushes.size());
    for (const auto &definition : brushes)
    {
        if (definition.at("type").get<unsigned>() != 1)
            throw std::runtime_error("IW3 destructible dynamic brushes require Scriptable data");
        const auto brushModel = definition.at("brush_model").get<std::size_t>();
        const auto physicsModel = definition.at("physics_brush_model").get<std::size_t>();
        if (brushModel >= submodelCount || physicsModel >= submodelCount)
            throw std::runtime_error("IW3 dynamic brush references a missing brush model");
        if (brushModel != physicsModel)
        {
            throw std::runtime_error(
                "IW3 dynamic brush uses separate render and physics brush models");
        }
        SourceDynamicEntities::Brush converted;
        converted.quaternion = ReadVector<4>(definition.at("quaternion"));
        converted.origin = ReadVector<3>(definition.at("origin"));
        BasisFromQuaternion(converted.quaternion);
        converted.model = static_cast<std::uint16_t>(brushModel);
        converted.physicsModel = static_cast<std::uint16_t>(physicsModel);
        result.brushes.push_back(converted);
    }
    return result;
}

std::vector<BrushModel> ReadBrushModels(const Json &world, const std::filesystem::path &root,
                                        const VisibilityGroups &visibility,
                                        SourceStaticModels &staticModels)
{
    const auto &sourceSurfaces = world.at("surfaces");
    if (!sourceSurfaces.is_array() || sourceSurfaces.empty())
    {
        throw std::runtime_error("IW3 world contains no render surfaces");
    }

    std::vector<BrushModel> output;
    if (world.contains("brush_models"))
    {
        const auto &sourceModels = world.at("brush_models");
        if (!sourceModels.is_array() || sourceModels.empty() || sourceModels.size() > 65535)
        {
            throw std::runtime_error("invalid IW3 world brush-model table");
        }
        output.reserve(sourceModels.size());
        std::vector<bool> claimed(sourceSurfaces.size());
        for (const auto &sourceModel : sourceModels)
        {
            BrushModel model;
            const auto &bounds = sourceModel.at("bounds");
            if (!bounds.is_array() || bounds.size() != 2)
                throw std::runtime_error("invalid IW3 brush-model bounds");
            model.minimum = ReadVector<3>(bounds.at(0));
            model.maximum = ReadVector<3>(bounds.at(1));
            for (std::size_t axis = 0; axis < 3; ++axis)
                if (model.minimum[axis] > model.maximum[axis])
                    throw std::runtime_error("reversed IW3 brush-model bounds");

            const std::size_t count = sourceModel.at("count").get<std::size_t>();
            if (count)
            {
                const std::size_t first = sourceModel.at("start").get<std::size_t>();
                if (first > sourceSurfaces.size() || count > sourceSurfaces.size() - first)
                    throw std::runtime_error("invalid IW3 brush-model surface range");
                model.surfaces.reserve(count);
                for (std::size_t index = first; index < first + count; ++index)
                {
                    if (claimed[index])
                        throw std::runtime_error("overlapping IW3 brush-model surface ranges");
                    claimed[index] = true;
                    Surface surface = ReadWorldSurface(sourceSurfaces.at(index));
                    surface.visibilityGroup = visibility.surfaces.at(index);
                    AlignWinding(surface);
                    model.surfaces.push_back(std::move(surface));
                }
            }
            output.push_back(std::move(model));
        }
        if (std::find(claimed.begin(), claimed.end(), false) != claimed.end())
            throw std::runtime_error("IW3 brush-model table does not own every render surface");
    }
    else
    {
        const auto &bounds = world.at("bounds");
        if (!bounds.is_array() || bounds.size() != 2)
            throw std::runtime_error("invalid IW3 world bounds");
        BrushModel model;
        model.minimum = ReadVector<3>(bounds.at(0));
        model.maximum = ReadVector<3>(bounds.at(1));
        model.surfaces.reserve(sourceSurfaces.size());
        std::size_t sourceIndex = 0;
        for (const auto &sourceSurface : sourceSurfaces)
        {
            Surface surface = ReadWorldSurface(sourceSurface);
            surface.visibilityGroup = visibility.surfaces.at(sourceIndex++);
            AlignWinding(surface);
            model.surfaces.push_back(std::move(surface));
        }
        output.push_back(std::move(model));
    }

    std::unordered_map<std::string, std::size_t> modelCache;
    std::size_t instanceIndex = 0;
    for (const auto &instance : world.value("models", Json::array()))
    {
        std::string modelName = instance.at("model").get<std::string>();
        if (!modelName.empty() && modelName.front() == ',')
        {
            modelName.erase(0, 1);
            std::error_code error;
            const auto modelPath = SafeChild(root, std::filesystem::path("xmodel") /
                                                      (modelName + ".json"));
            if (modelName.empty() || !std::filesystem::is_regular_file(modelPath, error))
                throw std::runtime_error(
                    "IW3 static-model instance " + std::to_string(instanceIndex) +
                    " needs external xmodel '" + modelName +
                    "'; add the matching IW3 zone with --search-path");
        }

        auto found = modelCache.find(modelName);
        if (found == modelCache.end())
        {
            const std::size_t modelIndex = staticModels.models.size();
            staticModels.models.push_back(ReadSourceModel(root, modelName, true));
            found = modelCache.emplace(modelName, modelIndex).first;
        }

        SourceStaticModelInstance converted;
        converted.model = found->second;
        const auto reflectionProbe = instance.at("reflection_probe").get<unsigned>();
        const auto sourceProbeCount = world.at("reflection_probes").size();
        if (reflectionProbe >= sourceProbeCount || reflectionProbe > UINT16_MAX)
            throw std::runtime_error("IW3 static-model instance references an invalid reflection probe");
        converted.reflectionProbe = static_cast<std::uint16_t>(reflectionProbe);
        converted.origin = ReadVector<3>(instance.at("origin"));
        converted.scale = instance.at("scale").get<float>();
        if (!std::isfinite(converted.scale) || converted.scale <= 0.0f)
        {
            throw std::runtime_error("invalid IW3 static-model scale");
        }
        for (std::size_t row = 0; row < 3; ++row)
        {
            converted.axis[row] = ReadVector<3>(instance.at("axis").at(row));
        }
        // IW3 culls each placement at its own cullDist and keeps drawing the
        // lowest LOD past that LOD's distance. Replay culls at the model's last
        // LOD distance, so extend it to the farthest placement's cullDist.
        if (const auto cull = instance.find("cull_dist"); cull != instance.end())
        {
            const float cullDistance = ReplayLodDistance(cull->get<float>());
            if (!std::isfinite(cullDistance) || cullDistance <= 0.0f)
                throw std::runtime_error("invalid IW3 static-model cull distance");
            auto &lowest = staticModels.models[converted.model].lods.back().distance;
            lowest = std::max(lowest, cullDistance);
        }
        staticModels.instances.push_back(std::move(converted));
        ++instanceIndex;
    }

    return output;
}

std::uint32_t PackedNormal(const Vec3 &source, const Vec3 &sourceTangent, const float binormalSign,
                           const bool authoredTangent)
{
    const Vec3 normal = Unit(source);
    Vec3 tangent = authoredTangent
                       ? sourceTangent
                       : Cross(std::abs(normal[2]) < 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0}, normal);
    tangent = Subtract(tangent, Multiply(normal, Dot(tangent, normal)));
    if (Dot(tangent, tangent) < 1.0e-10f)
        tangent = Cross(std::abs(normal[2]) < 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0}, normal);
    tangent = Unit(tangent);
    // The quaternion stores a proper rotation. A mirrored tangent frame has
    // determinant -1 and cannot be encoded as a quaternion; Replay carries
    // its handedness separately in packed bit 29.
    const Vec3 bitangent = Cross(normal, tangent);
    const float matrix[3][3] = {{tangent[0], bitangent[0], normal[0]},
                                {tangent[1], bitangent[1], normal[1]},
                                {tangent[2], bitangent[2], normal[2]}};
    std::array<float, 4> quaternion{};
    const float trace = matrix[0][0] + matrix[1][1] + matrix[2][2];
    if (trace > 0.0f)
    {
        const float scale = std::sqrt(trace + 1.0f) * 2.0f;
        quaternion = {(matrix[2][1] - matrix[1][2]) / scale, (matrix[0][2] - matrix[2][0]) / scale,
                      (matrix[1][0] - matrix[0][1]) / scale, scale / 4.0f};
    }
    else
    {
        std::size_t largest = 0;
        if (matrix[1][1] > matrix[largest][largest])
            largest = 1;
        if (matrix[2][2] > matrix[largest][largest])
            largest = 2;
        const std::size_t next = (largest + 1) % 3;
        const std::size_t last = (largest + 2) % 3;
        const float scale =
            std::sqrt(1.0f + matrix[largest][largest] - matrix[next][next] - matrix[last][last]) *
            2.0f;
        quaternion[largest] = scale / 4.0f;
        quaternion[next] = (matrix[next][largest] + matrix[largest][next]) / scale;
        quaternion[last] = (matrix[last][largest] + matrix[largest][last]) / scale;
        quaternion[3] = (matrix[last][next] - matrix[next][last]) / scale;
    }

    std::size_t largest = 0;
    for (std::size_t index = 1; index < quaternion.size(); ++index)
    {
        if (std::abs(quaternion[index]) >= std::abs(quaternion[largest]))
        {
            largest = index;
        }
    }
    const float scale = std::sqrt(2.0f) * (quaternion[largest] >= 0.0f ? 1.0f : -1.0f);
    std::array<std::uint32_t, 3> encoded{};
    std::size_t outputIndex = 0;
    for (std::size_t index = 0; index < quaternion.size(); ++index)
    {
        if (index == largest)
            continue;
        const unsigned bits = outputIndex < 2 ? 10u : 9u;
        const float maximum = static_cast<float>((1u << bits) - 1u);
        const int value = static_cast<int>((quaternion[index] * scale + 1.0f) * maximum / 2.0f);
        encoded[outputIndex++] =
            static_cast<std::uint32_t>(std::clamp(value, 0, static_cast<int>(maximum)));
    }
    std::uint32_t packed = encoded[0] | (encoded[1] << 10) | (encoded[2] << 20) |
                           (static_cast<std::uint32_t>(largest) << 30);
    if (binormalSign < 0)
        packed |= 1u << 29;
    return packed;
}

unsigned MaterialIndex(const RenderPlan &plan, const MaterialPlan &material,
                       const bool nativeWorld)
{
    if (nativeWorld)
    {
        if (!material.worldMaterialIndex ||
            material.worldMaterialIndex > plan.additionalMaterials.size())
            throw std::runtime_error("IW3 render plan is missing a native world material");
        return material.worldMaterialIndex;
    }
    if (material.kind == SurfaceKind::opaque && !material.depthBias)
        return 0;
    const std::string suffix = material.kind == SurfaceKind::opaque ? "_opaque_bias"
                               : material.kind == SurfaceKind::cutout
                                   ? "_" + CutoutVariantKind(material)
                               : material.kind == SurfaceKind::glass ? "_" + GlassVariantKind(material)
                                                                     : "_sky";
    for (std::size_t index = 0; index < plan.additionalMaterials.size(); ++index)
        if (plan.additionalMaterials[index].at("material").get<std::string>().ends_with(suffix))
            return static_cast<unsigned>(index + 1);
    throw std::runtime_error("IW3 render plan is missing a material variant");
}

// A source reflection probe as the shader's tag: converted slot + 1, 0 for
// none. IW3 probe 0 is R_CreateDefaultProbe's image, which the converter drops
// when authored captures exist (slot = index - 1); a lone default is slot 0.
unsigned ReflectionProbeTag(const std::size_t sourceProbe, const std::size_t sourceProbeCount)
{
    if (sourceProbe >= sourceProbeCount || sourceProbeCount > 256)
        return 0;
    return sourceProbeCount > 1 ? static_cast<unsigned>(sourceProbe) : 1u;
}

Vec2 EncodedLightmap(const Vertex &vertex, const Surface &surface, const MaterialPlan &material,
                     const RenderPlan &plan, const bool nativeWorld)
{
    float x = 0, y = 0;
    if (surface.lightmap >= 0 && static_cast<std::size_t>(surface.lightmap) >= plan.lightmaps.size())
        throw std::runtime_error("IW3 surface references an invalid lightmap");
    const bool baked = surface.lightmap >= 0;
    if (baked)
    {
        const auto &rectangle = plan.lightmaps[surface.lightmap];
        // A single native lightmap retains its source dimensions and has no
        // offset in the generated material atlas. Replay applies its own atlas
        // placement to these local UVs in the native world pixel shader.
        if (nativeWorld && plan.lightmaps.size() == 1)
            return {std::clamp(vertex.lightmapUv[0], 0.5f / rectangle.width,
                               1.0f - 0.5f / rectangle.width),
                    std::clamp(vertex.lightmapUv[1], 0.5f / rectangle.height,
                               1.0f - 0.5f / rectangle.height)};
        x = (rectangle.x +
             std::clamp(vertex.lightmapUv[0] * rectangle.width, 0.5f, rectangle.width - 0.5f)) /
            static_cast<float>(plan.atlasSize);
        y = (rectangle.y +
             std::clamp(vertex.lightmapUv[1] * rectangle.height, 0.5f, rectangle.height - 0.5f)) /
            static_cast<float>(plan.atlasSize);
    }
    if (nativeWorld)
    {
        if (!baked)
            throw std::runtime_error("native Replay world material requires a baked lightmap");
        return {x, y};
    }
    const unsigned kind = material.kind == SurfaceKind::cutout  ? 3u
                          : material.kind == SurfaceKind::glass ? 2u
                                                                : 0u;
    return {static_cast<float>(material.tile) + 0.25f + x * 0.25f,
            static_cast<float>(kind + material.flags + (baked ? 4u : 0u)) + 0.25f + y * 0.25f};
}

using EntityFields = std::unordered_map<std::string, std::string>;

std::vector<EntityFields> ParseEntityFields(
    const std::string &text, std::vector<std::pair<std::size_t, std::size_t>> *spans = nullptr)
{
    std::vector<EntityFields> entities;
    EntityFields entity;
    std::vector<std::string> values;
    std::string value;
    bool insideEntity = false;
    bool insideString = false;
    bool escaped = false;
    std::size_t entityStart = 0;
    for (std::size_t position = 0; position < text.size(); ++position)
    {
        const char character = text[position];
        if (insideString)
        {
            if (escaped)
            {
                value.push_back(character);
                escaped = false;
            }
            else if (character == '\\')
            {
                escaped = true;
            }
            else if (character == '"')
            {
                values.push_back(std::move(value));
                value.clear();
                insideString = false;
            }
            else
            {
                value.push_back(character);
            }
            continue;
        }
        if (character == '{')
        {
            if (insideEntity)
                throw std::runtime_error("nested IW3 entity block");
            insideEntity = true;
            entityStart = position;
            entity.clear();
            values.clear();
        }
        else if (character == '"' && insideEntity)
        {
            insideString = true;
        }
        else if (character == '}' && insideEntity)
        {
            if (values.size() % 2)
                throw std::runtime_error("IW3 entity has an unpaired key or value");
            for (std::size_t index = 0; index < values.size(); index += 2)
                entity[values[index]] = values[index + 1];
            entities.push_back(entity);
            if (spans)
                spans->emplace_back(entityStart, position + 1);
            insideEntity = false;
        }
    }
    if (insideEntity || insideString)
        throw std::runtime_error("unterminated IW3 entity block");
    return entities;
}

// The _global_fx entries whose script_structs the map places.
std::vector<const Iw3GlobalFx *> UsedGlobalFx(const std::string &entities)
{
    std::set<std::string> targetnames;
    for (const auto &entity : ParseEntityFields(entities))
    {
        const auto classname = entity.find("classname");
        const auto targetname = entity.find("targetname");
        if (classname != entity.end() && classname->second == "script_struct" &&
            targetname != entity.end())
            targetnames.insert(targetname->second);
    }
    std::vector<const Iw3GlobalFx *> used;
    for (const auto &entry : kIw3GlobalFx)
        if (targetnames.contains(entry.targetname))
            used.push_back(&entry);
    return used;
}

std::string WithoutLegacyGlassEntities(const std::string &text,
                                       const std::set<std::uint32_t> &glassModels,
                                       const std::set<std::string> &glassTargets)
{
    if (glassModels.empty())
        return text;
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const auto entities = ParseEntityFields(text, &spans);
    const auto isGlassBrush = [&](const EntityFields &entity) {
        const auto classname = entity.find("classname");
        const auto model = entity.find("model");
        if (classname == entity.end() || classname->second != "script_brushmodel" ||
            model == entity.end() || model->second.size() < 2 || model->second.front() != '*')
            return false;
        std::uint32_t modelIndex{};
        const char *begin = model->second.data() + 1;
        const char *end = model->second.data() + model->second.size();
        const auto parsed = std::from_chars(begin, end, modelIndex);
        return parsed.ec == std::errc{} && parsed.ptr == end && glassModels.contains(modelIndex);
    };
    std::set<std::string> glassBrushNames;
    for (const auto &entity : entities)
        if (const auto name = entity.find("targetname");
            name != entity.end() && isGlassBrush(entity))
            glassBrushNames.insert(name->second);
    std::string result;
    std::size_t copied = 0;
    std::size_t removed = 0;
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const auto &entity = entities[index];
        const auto classname = entity.find("classname");
        if (classname == entity.end())
            continue;
        const auto target = entity.find("target");
        bool omit = false;
        if (classname->second == "script_brushmodel")
            omit = isGlassBrush(entity);
        else if (classname->second == "trigger_damage")
            omit = target != entity.end() && glassTargets.contains(target->second);
        else if (classname->second == "script_model")
        {
            // A script_model targeting a replaced glass brush is the prefab's
            // shattered pane; one named as a native glass target is its intact
            // pane. Native FxGlass now draws and breaks the window instead.
            const auto name = entity.find("targetname");
            omit = (target != entity.end() && glassBrushNames.contains(target->second)) ||
                   (name != entity.end() && glassTargets.contains(name->second));
        }
        if (!omit)
            continue;
        result.append(text, copied, spans[index].first - copied);
        copied = spans[index].second;
        ++removed;
    }
    result.append(text, copied, text.size() - copied);
    zt::info("iw3: replaced %zu IW3 scripted glass entities with native glass", removed);
    return result;
}

// IW3 window prefabs chain intact model -> shattered model -> clip brush ->
// shard brush; their GSC hides the shattered states until the pane is shot.
// Replay runs no IW3 GSC, so keep only the intact model and the clip brush.
std::string WithoutUnscriptedWindowStates(const std::string &text)
{
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const auto entities = ParseEntityFields(text, &spans);
    std::map<std::string, std::size_t> byName;
    for (std::size_t index = 0; index < entities.size(); ++index)
        if (const auto name = entities[index].find("targetname");
            name != entities[index].end())
            byName.emplace(name->second, index);
    const auto field = [&](const std::size_t index, const char *key) -> const std::string * {
        const auto found = entities[index].find(key);
        return found == entities[index].end() ? nullptr : &found->second;
    };
    const auto next = [&](const std::size_t index, const char *classname) -> std::optional<std::size_t> {
        const auto *target = field(index, "target");
        if (!target)
            return std::nullopt;
        const auto found = byName.find(*target);
        if (found == byName.end())
            return std::nullopt;
        const auto *type = field(found->second, "classname");
        if (!type || *type != classname)
            return std::nullopt;
        return found->second;
    };
    std::set<std::size_t> omitted;
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const auto *type = field(index, "classname");
        if (!type || *type != "script_model")
            continue;
        const auto shattered = next(index, "script_model");
        const auto clip = shattered ? next(*shattered, "script_brushmodel") : std::nullopt;
        const auto shards = clip ? next(*clip, "script_brushmodel") : std::nullopt;
        if (!shards)
            continue;
        omitted.insert(*shattered);
        omitted.insert(*shards);
    }
    if (omitted.empty())
        return text;
    std::string result;
    std::size_t copied = 0;
    for (const auto index : omitted)
    {
        result.append(text, copied, spans[index].first - copied);
        copied = spans[index].second;
    }
    result.append(text, copied, text.size() - copied);
    zt::info("iw3: kept the intact state of %zu unscripted IW3 window prefab(s)",
             omitted.size() / 2);
    return result;
}

Vec3 ParseVector(const std::string &value)
{
    std::istringstream input(value);
    Vec3 result{};
    if (!(input >> result[0] >> result[1] >> result[2]) ||
        std::any_of(result.begin(), result.end(),
                    [](const float item) { return !std::isfinite(item); }))
    {
        throw std::runtime_error("invalid IW3 entity vector");
    }
    return result;
}

std::array<Vec3, 3> EntityAxis(const EntityFields &entity)
{
    Vec3 angles{};
    if (const auto anglesValue = entity.find("angles"); anglesValue != entity.end())
        angles = ParseVector(anglesValue->second);
    else if (const auto angleValue = entity.find("angle"); angleValue != entity.end())
        angles[1] = std::stof(angleValue->second);

    return iw8::staticmodelcollision::axisFromAngles(angles);
}

struct PromotedScriptModel
{
    std::string model;
    Vec3 origin{};
    std::array<Vec3, 3> axis{};
    float scale{1.0f};
};

// IW3 decorative script_models (destructible cars, ceiling fans, the sign
// digits, TVs) are entities only so their GSC can animate or destroy them.
// Replay runs no IW3 GSC, so place them like misc_model through the static
// model path (GPU static-model draw, sun shadows and static collision).
// Gametype objects, exploders, old-school pickups and linked prefab members
// remain entities for Replay's own scripts and the glass conversion.
std::vector<PromotedScriptModel> TakeUnscriptedScriptModels(std::string &text,
                                                            const std::set<std::string> &scripted)
{
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const auto entities = ParseEntityFields(text, &spans);
    std::set<std::string> targeted;
    for (const auto &entity : entities)
        if (const auto target = entity.find("target"); target != entity.end())
            targeted.insert(target->second);
    std::vector<PromotedScriptModel> promoted;
    std::vector<std::size_t> removed;
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const auto &entity = entities[index];
        const auto field = [&](const char *key) -> const std::string * {
            const auto found = entity.find(key);
            return found == entity.end() ? nullptr : &found->second;
        };
        const auto *classname = field("classname");
        const auto *model = field("model");
        if (!classname || *classname != "script_model" || !model || model->empty() ||
            model->front() == '*' || field("script_gameobjectname") || field("script_exploder"))
            continue;
        // Entities the converted map script drives (by targetname) or the
        // IW3 compatibility layer handles stay entities.
        if (const auto *name = field("targetname");
            name && (*name == "exploder" || *name == "oldschool_pickup" ||
                     *name == "destructible" || *name == "interactive_tv" ||
                     targeted.contains(*name) || scripted.contains(*name)))
            continue;
        PromotedScriptModel placement;
        placement.model = *model;
        if (const auto *origin = field("origin"))
            placement.origin = ParseVector(*origin);
        placement.axis = EntityAxis(entity);
        if (const auto *scale = field("modelscale"))
            placement.scale = std::stof(*scale);
        if (!std::isfinite(placement.scale) || placement.scale <= 0.0f)
            throw std::runtime_error("invalid IW3 script_model scale: " + *model);
        promoted.push_back(std::move(placement));
        removed.push_back(index);
    }
    if (removed.empty())
        return promoted;
    std::string result;
    std::size_t copied = 0;
    for (const auto index : removed)
    {
        result.append(text, copied, spans[index].first - copied);
        copied = spans[index].second;
    }
    result.append(text, copied, text.size() - copied);
    text = std::move(result);
    return promoted;
}

void AddPromotedStaticModels(const Json &world, const std::filesystem::path &root,
                             const std::vector<PromotedScriptModel> &placements,
                             SourceStaticModels &staticModels)
{
    if (placements.empty())
        return;
    const std::size_t compiledInstances = staticModels.instances.size();
    const auto &probes = world.at("reflection_probes");
    std::unordered_map<std::string, std::size_t> models;
    for (std::size_t index = 0; index < staticModels.models.size(); ++index)
        models.emplace(staticModels.models[index].name, index);
    for (const auto &placement : placements)
    {
        auto found = models.find(placement.model);
        if (found == models.end())
        {
            found = models.emplace(placement.model, staticModels.models.size()).first;
            staticModels.models.push_back(ReadSourceModel(root, placement.model, true));
        }
        SourceStaticModelInstance instance;
        instance.model = found->second;
        instance.origin = placement.origin;
        instance.axis = placement.axis;
        instance.scale = placement.scale;
        // The IW3 compiler assigns probes per placement; the nearest compiled
        // static model's probe matches it for 94% of Nuketown's placements.
        float best = std::numeric_limits<float>::infinity();
        for (std::size_t other = 0; other < compiledInstances; ++other)
        {
            const Vec3 delta = Subtract(staticModels.instances[other].origin, placement.origin);
            if (const float distance = Dot(delta, delta); distance < best)
            {
                best = distance;
                instance.reflectionProbe = staticModels.instances[other].reflectionProbe;
            }
        }
        if (!compiledInstances)
            for (std::size_t probe = 1; probe < probes.size() && probe <= UINT16_MAX; ++probe)
            {
                const Vec3 delta = Subtract(ReadVector<3>(probes.at(probe).at("origin")),
                                            placement.origin);
                if (const float distance = Dot(delta, delta); distance < best)
                {
                    best = distance;
                    instance.reflectionProbe = static_cast<std::uint16_t>(probe);
                }
            }
        staticModels.instances.push_back(instance);
    }
    zt::info("iw3: placed %zu unscripted script_model(s) through the static-model path",
             placements.size());
}

Vec3 Transform(const std::array<Vec3, 3> &axis, const Vec3 &value)
{
    Vec3 result{};
    for (std::size_t component = 0; component < 3; ++component)
        for (std::size_t direction = 0; direction < 3; ++direction)
            result[component] += axis[direction][component] * value[direction];
    return result;
}

std::array<Vec3, 3> StaticModelAxis(const SourceStaticModelInstance &instance)
{
    std::array<Vec3, 3> axis{};
    for (std::size_t index = 0; index < axis.size(); ++index)
        axis[index] = Unit(instance.axis[index]);
    if (std::abs(Dot(axis[0], axis[1])) > 0.01f || std::abs(Dot(axis[0], axis[2])) > 0.01f ||
        std::abs(Dot(axis[1], axis[2])) > 0.01f ||
        Dot(Cross(axis[0], axis[1]), axis[2]) < 0.99f)
        throw std::runtime_error("IW3 static-model instance has an invalid rotation basis");
    return axis;
}

Vec4 QuaternionFromBasis(const Vec3 &x, const Vec3 &y, const Vec3 &z)
{
    const float matrix[3][3]{{x[0], y[0], z[0]}, {x[1], y[1], z[1]}, {x[2], y[2], z[2]}};
    Vec4 quaternion{};
    const float trace = matrix[0][0] + matrix[1][1] + matrix[2][2];
    if (trace > 0.0f)
    {
        const float scale = std::sqrt(trace + 1.0f) * 2.0f;
        quaternion = {(matrix[2][1] - matrix[1][2]) / scale, (matrix[0][2] - matrix[2][0]) / scale,
                      (matrix[1][0] - matrix[0][1]) / scale, scale * 0.25f};
    }
    else
    {
        const std::size_t largest = matrix[0][0] > matrix[1][1]
                                        ? (matrix[0][0] > matrix[2][2] ? 0 : 2)
                                        : (matrix[1][1] > matrix[2][2] ? 1 : 2);
        const std::size_t next = (largest + 1) % 3;
        const std::size_t last = (largest + 2) % 3;
        const float scale =
            std::sqrt(1.0f + matrix[largest][largest] - matrix[next][next] - matrix[last][last]) *
            2.0f;
        quaternion[largest] = scale * 0.25f;
        quaternion[3] = (matrix[last][next] - matrix[next][last]) / scale;
        quaternion[next] = (matrix[next][largest] + matrix[largest][next]) / scale;
        quaternion[last] = (matrix[last][largest] + matrix[largest][last]) / scale;
    }
    return quaternion;
}

Vec4 QuaternionMultiply(const Vec4 &a, const Vec4 &b)
{
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Vec4 QuaternionConjugate(const Vec4 &quaternion)
{
    return {-quaternion[0], -quaternion[1], -quaternion[2], quaternion[3]};
}

Vec3 QuaternionRotate(const Vec4 &quaternion, const Vec3 &value)
{
    const Vec3 axis{quaternion[0], quaternion[1], quaternion[2]};
    const Vec3 twice = Multiply(Cross(axis, value), 2.0f);
    return Add(Add(value, Multiply(twice, quaternion[3])), Cross(axis, twice));
}

// IW8 keeps each bone's world bind pose in baseMat and every child bone
// relative to its parent: world = parent * local, with the local translation in
// the parent's frame (checked against shipped multi-bone XModels).
void SetSourceSkeleton(const std::vector<SourceBone> &bones,
                       convert::xmodel::Iw8XModelRecord &model)
{
    std::size_t roots = 0;
    while (roots < bones.size() && bones[roots].parent < 0)
        ++roots;
    if (!roots || bones.size() > 128)
        throw std::runtime_error("IW3 skeleton has an unsupported bone layout");
    for (std::size_t index = roots; index < bones.size(); ++index)
        if (bones[index].parent < 0 || bones[index].parent >= static_cast<int>(index))
            throw std::runtime_error("IW3 skeleton bones are not ordered after their parents");
    model.numBones = static_cast<std::uint8_t>(bones.size());
    model.numRootBones = static_cast<std::uint8_t>(roots);
    model.boneNames.clear();
    model.skeleton = {};
    std::vector<Vec4> world(bones.size());
    for (std::size_t index = 0; index < bones.size(); ++index)
    {
        const SourceBone &bone = bones[index];
        Vec4 rotation =
            QuaternionFromBasis(Unit(bone.axis[0]), Unit(bone.axis[1]), Unit(bone.axis[2]));
        const float length = std::sqrt(std::inner_product(rotation.begin(), rotation.end(),
                                                          rotation.begin(), 0.0f));
        if (!std::isfinite(length) || length < 0.5f)
            throw std::runtime_error("IW3 skeleton has an invalid bone basis: " + bone.name);
        const float sign = rotation[3] < 0.0f ? -1.0f : 1.0f;
        for (float &component : rotation)
            component *= sign / length;
        world[index] = rotation;
        model.boneNames.push_back(bone.name);
        model.skeleton.partClassification.push_back(0);
        // transWeight is 2 / |quat|^2 for the unit bind rotation.
        model.skeleton.baseMat.push_back({rotation, bone.origin, 2.0f});
        if (index < roots)
            continue;
        const auto parent = static_cast<std::size_t>(bone.parent);
        const Vec4 inverseParent = QuaternionConjugate(world[parent]);
        Vec4 local = QuaternionMultiply(inverseParent, rotation);
        if (local[3] < 0.0f)
            for (float &component : local)
                component = -component;
        std::array<std::int16_t, 4> packed{};
        for (std::size_t component = 0; component < 4; ++component)
            packed[component] = static_cast<std::int16_t>(
                std::clamp(std::lround(local[component] * 32767.0f), -32767l, 32767l));
        model.skeleton.parentList.push_back(static_cast<std::uint8_t>(index - parent));
        model.skeleton.quats.push_back(packed);
        model.skeleton.trans.push_back(
            QuaternionRotate(inverseParent, Subtract(bone.origin, bones[parent].origin)));
    }
    model.skeleton.boneInfo.assign(bones.size(), {});
}

std::uint32_t PackIw3UnitVector(const Vec3 &source)
{
    const Vec3 value = Unit(source);
    std::uint32_t packed = 0;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        const auto component = static_cast<std::uint32_t>(
            std::clamp(std::lround(value[axis] * 127.0f + 127.0f), 0l, 254l));
        packed |= component << (axis * 8);
    }
    return packed | 0x7F000000u;
}

unsigned NativeSurfaceMaterial(std::uint32_t sourceBits);

void BuildPreparedStaticModels(const SourceStaticModels &source, const RenderPlan &plan,
                               const std::string &map, const std::string &assetPrefix,
                               std::vector<PreparedXModel> &xmodels,
                               replayrender::StaticModels &staticModels, const bool nativePhysics,
                               const bool entityDraw, const bool preserveSourceNames = false,
                               std::vector<iw8::staticmodelcollision::Model> *collisionModels =
                                   nullptr,
                               const bool scriptModelPhysics = false)
{
    constexpr std::uint32_t rootBonePartBit = 0x80000000u;

    xmodels.reserve(source.models.size());
    staticModels.models.reserve(source.models.size());
    std::vector<std::optional<std::size_t>> collisionModelIndices(source.models.size());
    std::size_t visualCollisionModels = 0;
    for (std::size_t modelIndex = 0; modelIndex < source.models.size(); ++modelIndex)
    {
        const SourceStaticModel &sourceModel = source.models[modelIndex];
        const std::string assetName = preserveSourceNames ? sourceModel.name
                                                          : "mw120r/" + map + "/" + assetPrefix +
                                                                "_" + std::to_string(modelIndex);
        PreparedXModel prepared;
        prepared.model.name = assetName;
        const bool skinned = sourceModel.bones.size() > 1;
        if (skinned)
            SetSourceSkeleton(sourceModel.bones, prepared.model);
        else
        {
            prepared.model.numBones = 1;
            prepared.model.numRootBones = 1;
            prepared.model.boneNames = {"tag_origin"};
            prepared.model.skeleton.partClassification = {0};
            // DObjAnimMat.transWeight is 2 / |quat|^2; shipped identity bind poses store 2.
            prepared.model.skeleton.baseMat = {{{0, 0, 0, 1}, {0, 0, 0}, 2}};
        }
        // Bind-space bounds of each bone's LOD0 vertices (skinned models).
        std::vector<replaybounds::Accumulator> boneBounds(skinned ? sourceModel.bones.size() : 0);
        std::vector<bool> boneUsed(boneBounds.size());
        prepared.model.collLod = sourceModel.collisionLod == SIZE_MAX
                                     ? UINT8_MAX
                                     : static_cast<std::uint8_t>(sourceModel.collisionLod);
        prepared.model.shadowCutoffLod = 6;
        prepared.model.flags = 0;
        prepared.model.contents = 1;
        prepared.model.scale = 1.0f;

        replayrender::StaticModel staticModel;
        staticModel.name = assetName;
        replaybounds::Accumulator modelBounds;
        std::uint32_t surfaceOffset = 0;
        for (std::size_t lodIndex = 0; lodIndex < sourceModel.lods.size(); ++lodIndex)
        {
            const SourceStaticModelLod &sourceLod = sourceModel.lods[lodIndex];
            dumpsrc::XseFile xse;
            xse.loaded = true;
            xse.name = assetName + "_lod" + std::to_string(lodIndex);
            if (!skinned)
                xse.modelSurfPartBits[0] = rootBonePartBit;
            std::vector<const MaterialPlan *> surfaceMaterials;
            for (const Surface &surface : sourceLod.surfaces)
            {
                const auto found = plan.materials.find(surface.material);
                if (found == plan.materials.end())
                    throw std::runtime_error("IW3 render plan omitted model material " +
                                             surface.material);
                const MaterialPlan &material = found->second;
                if (material.kind == SurfaceKind::skipped)
                    continue;
                if (material.kind == SurfaceKind::sky)
                    throw std::runtime_error("IW3 static model uses a sky material: " +
                                             sourceModel.name);
                Surface ladderRenderSurface;
                const Surface *renderSurface = &surface;
                if (sourceModel.name == "com_ladder_wood")
                {
                    ladderRenderSurface = surface;
                    AddLadderMidpointRungs(ladderRenderSurface, lodIndex);
                    renderSurface = &ladderRenderSurface;
                }
                if (renderSurface->vertices.empty() || renderSurface->vertices.size() > UINT16_MAX ||
                    renderSurface->indices.empty() || renderSurface->indices.size() % 3 ||
                    renderSurface->indices.size() / 3 > UINT16_MAX)
                {
                    throw std::runtime_error("IW3 static model has invalid surface counts: " +
                                             sourceModel.name);
                }

                for (const std::uint32_t index : renderSurface->indices)
                    if (index >= renderSurface->vertices.size())
                        throw std::runtime_error(
                            "IW3 static-model triangle references a missing vertex");
                if (skinned && renderSurface->vertexBones.size() != renderSurface->vertices.size())
                    throw std::runtime_error("IW3 skinned model surface lacks vertex bones: " +
                                             sourceModel.name);
                const auto boneOf = [&](const std::uint32_t vertex) -> std::uint8_t {
                    return skinned ? renderSurface->vertexBones[vertex] : std::uint8_t{0};
                };
                // Rigid runs need each bone's vertices contiguous, so order the
                // vertices and triangles by bone (a triangle follows its first vertex).
                std::vector<std::uint32_t> vertexOrder(renderSurface->vertices.size());
                std::iota(vertexOrder.begin(), vertexOrder.end(), 0u);
                std::vector<std::size_t> triangleOrder(renderSurface->indices.size() / 3);
                std::iota(triangleOrder.begin(), triangleOrder.end(), std::size_t{0});
                if (skinned)
                {
                    std::ranges::stable_sort(vertexOrder, {}, boneOf);
                    std::ranges::stable_sort(triangleOrder, {}, [&](const std::size_t triangle) {
                        return boneOf(renderSurface->indices[triangle * 3]);
                    });
                }
                std::vector<std::uint32_t> remap(vertexOrder.size());
                for (std::size_t position = 0; position < vertexOrder.size(); ++position)
                    remap[vertexOrder[position]] = static_cast<std::uint32_t>(position);

                dumpsrc::XseSurface converted;
                if (!skinned)
                    converted.partBits[0] = rootBonePartBit;
                converted.vertCount = static_cast<std::uint32_t>(renderSurface->vertices.size());
                converted.triCount = static_cast<std::uint32_t>(renderSurface->indices.size() / 3);
                converted.verticies.reserve(renderSurface->vertices.size());
                for (const std::uint32_t sourceVertex : vertexOrder)
                {
                    const Vertex &vertex = renderSurface->vertices[sourceVertex];
                    const Vec3 normal = Unit(vertex.normal);
                    Vec3 tangent =
                        Subtract(vertex.tangent, Multiply(normal, Dot(vertex.tangent, normal)));
                    if (Dot(tangent, tangent) < 1.0e-10f)
                    {
                        tangent = Cross(std::abs(normal[2]) < 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0},
                                        normal);
                    }
                    tangent = Unit(tangent);

                    dumpsrc::XseVertex output;
                    std::copy_n(vertex.position.data(), 3, output.xyz);
                    output.binormalSign = vertex.binormalSign;
                    output.color = static_cast<std::uint32_t>(vertex.color[2]) |
                                   (static_cast<std::uint32_t>(vertex.color[1]) << 8) |
                                   (static_cast<std::uint32_t>(vertex.color[0]) << 16) |
                                   (static_cast<std::uint32_t>(vertex.color[3]) << 24);
                    output.texCoord =
                        (static_cast<std::uint32_t>(xsurf_conv::floatToHalf(vertex.uv[0])) << 16) |
                        xsurf_conv::floatToHalf(vertex.uv[1]);
                    output.normal = PackIw3UnitVector(normal);
                    output.tangent = PackIw3UnitVector(tangent);
                    converted.verticies.push_back(output);
                    if (lodIndex == 0)
                    {
                        modelBounds.Add(vertex.position);
                        if (skinned)
                        {
                            const auto &pose =
                                prepared.model.skeleton.baseMat[boneOf(sourceVertex)];
                            boneBounds[boneOf(sourceVertex)].Add(
                                QuaternionRotate(QuaternionConjugate(pose.quat),
                                                 Subtract(vertex.position, pose.trans)));
                            boneUsed[boneOf(sourceVertex)] = true;
                        }
                    }
                }
                converted.triIndices.reserve(renderSurface->indices.size());
                for (const std::size_t triangle : triangleOrder)
                    for (std::size_t corner = 0; corner < 3; ++corner)
                        converted.triIndices.push_back(static_cast<std::uint16_t>(
                            remap[renderSurface->indices[triangle * 3 + corner]]));
                // One rigid run per bone: the matrix offset is bone * 64.
                for (std::size_t vertexStart = 0, triangleStart = 0;
                     vertexStart < vertexOrder.size();)
                {
                    const std::uint8_t bone = boneOf(vertexOrder[vertexStart]);
                    std::size_t vertexEnd = vertexStart;
                    while (vertexEnd < vertexOrder.size() && boneOf(vertexOrder[vertexEnd]) == bone)
                        ++vertexEnd;
                    std::size_t triangleEnd = triangleStart;
                    while (triangleEnd < triangleOrder.size() &&
                           boneOf(renderSurface->indices[triangleOrder[triangleEnd] * 3]) == bone)
                        ++triangleEnd;
                    converted.rigidVertLists.push_back(
                        {static_cast<std::uint16_t>(bone * 64),
                         static_cast<std::uint16_t>(vertexEnd - vertexStart),
                         static_cast<std::uint16_t>(triangleStart),
                         static_cast<std::uint16_t>(triangleEnd - triangleStart)});
                    if (skinned)
                    {
                        const auto bit = static_cast<std::int32_t>(0x80000000u >> (bone % 32));
                        converted.partBits[bone / 32] |= bit;
                        xse.modelSurfPartBits[bone / 32] |= bit;
                    }
                    vertexStart = vertexEnd;
                    triangleStart = triangleEnd;
                }
                converted.vertListCount = static_cast<std::uint32_t>(converted.rigidVertLists.size());
                xse.surfaces.push_back(std::move(converted));
                surfaceMaterials.push_back(&material);
            }
            if (xse.surfaces.empty())
                throw std::runtime_error("IW3 static model LOD has no visible surfaces: " +
                                         sourceModel.name);

            conv_xsurf::Iw8Surfs converted = conv_xsurf::convert(xse);
            if (!converted.ok || converted.surfaces.size() != surfaceMaterials.size())
                throw std::runtime_error("could not convert IW3 static-model surfaces: " +
                                         sourceModel.name);

            replayrender::StaticModelLod staticLod;
            staticLod.distance = sourceLod.distance;
            for (std::size_t surfaceIndex = 0; surfaceIndex < converted.surfaces.size();
                 ++surfaceIndex)
            {
                const MaterialPlan &material = *surfaceMaterials[surfaceIndex];
                const unsigned kind = material.kind == SurfaceKind::cutout  ? 3u
                                      : material.kind == SurfaceKind::glass ? 2u
                                                                            : 0u;
                // Models bind their own textures, so the low 16 bits carry no atlas
                // tile: bits 0-3 hold the blend and falloff flags (256-2048), bits
                // 4-9 the layer rank (flags bits 12-17), bits 16-23 the low flags.
                const unsigned lowFlags = (kind + material.flags) & 0xFFu;
                const unsigned blendFlags = (material.flags >> 8) & 0xFu;
                const unsigned layer = (material.flags >> 12) & 0x3Fu;
                if (material.flags > 0x3FFFFu)
                    throw std::runtime_error("IW3 model material metadata exceeds Replay fields");
                const std::uint32_t metadata =
                    blendFlags | (layer << 4) | (lowFlags << 16) | 0xFF000000u;
                auto &destinationSurface = converted.surfaces[surfaceIndex];
                constexpr std::size_t packedVertexSize = 20;
                constexpr std::size_t selfVisibilityOffset = 8;
                const std::size_t vertexBytes =
                    static_cast<std::size_t>(destinationSurface.vertCount) * packedVertexSize;
                if (destinationSurface.sharedVertDataOffset > converted.sharedBlob.size() ||
                    vertexBytes >
                        converted.sharedBlob.size() - destinationSurface.sharedVertDataOffset)
                {
                    throw std::runtime_error("IW3 model vertex buffer is outside shared geometry");
                }
                for (std::size_t vertex = 0; vertex < destinationSurface.vertCount; ++vertex)
                {
                    std::memcpy(converted.sharedBlob.data() +
                                    destinationSurface.sharedVertDataOffset +
                                    vertex * packedVertexSize + selfVisibilityOffset,
                                &metadata, sizeof(metadata));
                }

                const std::string &materialName =
                    entityDraw ? material.entityMaterial : material.modelMaterial;
                if (materialName.empty())
                    throw std::runtime_error("IW3 render plan omitted a source model material");
                prepared.model.materials.push_back(materialName);
                replayrender::StaticModelSurface staticSurface;
                std::copy_n(destinationSurface.boundsMid, 3, staticSurface.bounds.midpoint.begin());
                std::copy_n(destinationSurface.boundsHalf, 3,
                            staticSurface.bounds.halfSize.begin());
                staticSurface.material = materialName;
                staticSurface.backfaceCull = material.cullMode == 1;
                staticLod.surfaces.push_back(std::move(staticSurface));
            }

            if (surfaceOffset + converted.surfaces.size() > UINT16_MAX)
                throw std::runtime_error("IW3 static model exceeds Replay's surface count");
            convert::xmodel::Iw8LodInfo lod;
            lod.dist = sourceLod.distance;
            lod.numsurfs = static_cast<std::uint16_t>(converted.surfaces.size());
            lod.surfIndex = static_cast<std::uint16_t>(surfaceOffset);
            for (std::size_t word = 0; word < 6; ++word)
                lod.partBits[word] = static_cast<std::uint32_t>(xse.modelSurfPartBits[word]);
            lod.surfsName = converted.name;
            prepared.model.lods.push_back(std::move(lod));
            surfaceOffset += static_cast<std::uint32_t>(converted.surfaces.size());
            prepared.lods.push_back(std::move(converted));
            staticModel.lods.push_back(std::move(staticLod));
        }

        const replaybounds::Bounds bounds = modelBounds.Finish();
        prepared.model.numsurfs = static_cast<std::uint16_t>(surfaceOffset);
        prepared.model.himipRadiusInvSq.reserve(prepared.model.numsurfs);
        for (const auto &lod : staticModel.lods)
            for (const auto &surface : lod.surfaces)
            {
                const float radiusSquared = std::inner_product(
                    surface.bounds.halfSize.begin(), surface.bounds.halfSize.end(),
                    surface.bounds.halfSize.begin(), 0.0f);
                prepared.model.himipRadiusInvSq.push_back(1.0f / std::max(radiusSquared, 1.0f));
            }
        prepared.model.numLods = static_cast<std::uint8_t>(prepared.model.lods.size());
        prepared.model.radius = bounds.Radius();
        std::copy_n(bounds.midpoint.data(), 3, prepared.model.boundsMid);
        std::copy_n(bounds.halfSize.data(), 3, prepared.model.boundsHalf);
        if (skinned)
        {
            for (std::size_t bone = 0; bone < boneBounds.size(); ++bone)
            {
                if (!boneUsed[bone])
                    continue;
                const replaybounds::Bounds local = boneBounds[bone].Finish();
                auto &info = prepared.model.skeleton.boneInfo[bone];
                info.midPoint = local.midpoint;
                info.halfSize = local.halfSize;
                // Squared distance from the bone origin to the farthest corner.
                info.radiusSquared = 0.0f;
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    const float extent = std::abs(local.midpoint[axis]) + local.halfSize[axis];
                    info.radiusSquared += extent * extent;
                }
            }
            zt::info("iw3: entity model %s keeps its %zu-bone IW3 skeleton",
                     sourceModel.name.c_str(), sourceModel.bones.size());
        }
        else
            prepared.model.skeleton.boneInfo = {
                {bounds.midpoint, bounds.halfSize, prepared.model.radius * prepared.model.radius}};
        // Simulated clutter (an IW3 physics preset on a dynamic model) needs
        // a hull even when the model has no collision LOD: use its first LOD.
        const bool clutter =
            nativePhysics && sourceModel.clutter && !collisionModels && !scriptModelPhysics;
        std::size_t physicsLod =
            sourceModel.collisionLod != SIZE_MAX ? sourceModel.collisionLod : clutter ? 0 : SIZE_MAX;
        // A static placement without an IW3 collision LOD stopped no bullet in
        // IW3; maps relied on clip brushes, and prefab-built maps such as
        // Greenband have none around their props and walls (owner: bullets and
        // grenades through rocks and buildings). Its opaque visual surfaces
        // collide instead, from the coarsest LOD that keeps a quarter of LOD0's
        // triangles (at least 300). Contents 0xB180 stay outside the player
        // mask: players still pass props, as in IW3.
        const auto solidSurface = [&](const Surface &surface) {
            const auto material = plan.materials.find(surface.material);
            return material != plan.materials.end() &&
                   material->second.kind == SurfaceKind::opaque && !material->second.depthBias &&
                   (material->second.flags & 0x700u) == 0;
        };
        const bool visualCollision = nativePhysics && collisionModels &&
                                     physicsLod == SIZE_MAX && !sourceModel.lods.empty();
        if (visualCollision)
        {
            std::vector<std::size_t> triangles;
            for (const auto &lod : sourceModel.lods)
            {
                std::size_t count = 0;
                for (const Surface &surface : lod.surfaces)
                    if (solidSurface(surface))
                        count += surface.indices.size() / 3;
                triangles.push_back(count);
            }
            const std::size_t wanted =
                std::min(triangles.front(), std::max<std::size_t>(triangles.front() / 4, 300));
            for (std::size_t lod = 0; lod < triangles.size(); ++lod)
                if (triangles[lod] && triangles[lod] >= wanted)
                    physicsLod = lod;
            if (physicsLod != SIZE_MAX)
                ++visualCollisionModels;
        }
        if (nativePhysics && physicsLod != SIZE_MAX)
        {
            iw8::havok::PhysicsMesh physics;
            // Fixed placements use Replay's static-model collision list. The
            // generic 0xB180 contents match common shipped static assets;
            // per-material contents need their own source-to-target mapping.
            // Static placements and script models use the contents of the
            // shipped Static / KeyframedEntity assets.
            if (collisionModels || scriptModelPhysics || clutter)
                physics.contents = 0xB180u;
            std::map<Vec3, std::uint32_t> vertexIndices;
            const auto appendVertex = [&](const Vec3 &vertex) {
                const auto found = vertexIndices.find(vertex);
                if (found != vertexIndices.end())
                    return found->second;
                if (physics.vertices.size() >= std::numeric_limits<std::uint32_t>::max())
                    throw std::runtime_error("IW3 dynamic-model physics exceeds Replay limits");
                const auto index = static_cast<std::uint32_t>(physics.vertices.size());
                physics.vertices.push_back(vertex);
                vertexIndices.emplace(vertex, index);
                return index;
            };
            // Replay carries one surface type per model mesh (shipped assets do
            // the same); take the IW3 type covering the most collision area.
            std::map<unsigned, float> surfaceAreas;
            for (const Surface &surface : sourceModel.lods.at(physicsLod).surfaces)
            {
                if (visualCollision && !solidSurface(surface))
                    continue;
                const auto material = plan.materials.find(surface.material);
                // Script-model glass (car windows, lights) breaks by hiding its
                // part, which cannot remove a mesh shape: keep glass out of the
                // keyframed body so bullets pass windows as they do once broken.
                if (scriptModelPhysics &&
                    ((material != plan.materials.end() &&
                      material->second.kind == SurfaceKind::glass) ||
                     Lowercase(surface.material).find("glass") != std::string::npos))
                    continue;
                const unsigned surfaceType =
                    material == plan.materials.end()
                        ? 0
                        : NativeSurfaceMaterial(material->second.sourceSurfaceTypeBits);
                for (std::size_t first = 0; first < surface.indices.size(); first += 3)
                {
                    std::array<std::uint32_t, 3> triangle{};
                    std::array<Vec3, 3> corners{};
                    for (std::size_t corner = 0; corner < triangle.size(); ++corner)
                    {
                        const auto sourceIndex = surface.indices.at(first + corner);
                        if (sourceIndex >= surface.vertices.size())
                            throw std::runtime_error(
                                "IW3 dynamic-model physics references a missing vertex");
                        corners[corner] = surface.vertices[sourceIndex].position;
                        triangle[corner] = appendVertex(corners[corner]);
                    }
                    // IW3 model triangles are clockwise seen from outside;
                    // Havok fronts are counter-clockwise (see the world mesh).
                    std::swap(triangle[1], triangle[2]);
                    physics.triangles.push_back(triangle);
                    if (surfaceType)
                    {
                        const Vec3 normal = Cross(Subtract(corners[1], corners[0]),
                                                  Subtract(corners[2], corners[0]));
                        surfaceAreas[surfaceType] += std::sqrt(Dot(normal, normal));
                    }
                }
            }
            const auto dominant = std::ranges::max_element(
                surfaceAreas, {}, [](const auto &entry) { return entry.second; });
            physics.surfaceType = dominant == surfaceAreas.end() ? 0 : dominant->first;
            prepared.physicsAsset =
                collisionModels ? iw8::havok::BakePhysicsAsset(assetName, physics, 0, 0)
                : clutter       ? iw8::havok::BakePhysicsAsset(assetName, physics, 3,
                                                               sourceModel.clutter->debris ? 5 : 4,
                                                               false, &*sourceModel.clutter)
                          : iw8::havok::BakePhysicsAsset(assetName, physics, 3, 1, scriptModelPhysics);
            prepared.model.physicsAssetName = assetName;
        }
        if (collisionModels && !prepared.model.physicsAssetName.empty())
        {
            iw8::staticmodelcollision::Model collisionModel;
            collisionModel.name = assetName;
            collisionModel.physicsAssetName = prepared.model.physicsAssetName;
            collisionModelIndices[modelIndex] = collisionModels->size();
            collisionModels->push_back(std::move(collisionModel));
        }
        staticModel.bounds = bounds;
        xmodels.push_back(std::move(prepared));
        staticModels.models.push_back(std::move(staticModel));
    }

    staticModels.instances.reserve(source.instances.size());
    std::size_t unrotatedInstances = 0;
    for (const SourceStaticModelInstance &sourceInstance : source.instances)
    {
        auto axis = StaticModelAxis(sourceInstance);
        const Vec3 angles = iw8::staticmodelcollision::anglesFromAxis(axis);
        const auto rotation = iw8::staticmodelcollision::axisFromAngles(angles);
        if (!iw8::staticmodelcollision::axesNearlyEqual(axis, rotation, 2.0e-4f))
        {
            // A mirrored or skewed Radiant placement has no quaternion; Replay
            // places the model with the nearest rotation instead.
            axis = rotation;
            ++unrotatedInstances;
        }
        staticModels.instances.push_back(
            {static_cast<unsigned>(sourceInstance.model), sourceInstance.origin,
             QuaternionFromBasis(axis[0], axis[1], axis[2]), sourceInstance.scale});
        if (collisionModels && collisionModelIndices.at(sourceInstance.model))
        {
            auto &collisionModel = collisionModels->at(*collisionModelIndices.at(sourceInstance.model));
            collisionModel.instances.push_back({sourceInstance.origin, angles, sourceInstance.scale});
        }
    }
    if (unrotatedInstances)
        zt::warn("iw3: %zu static model(s) have a mirrored or skewed IW3 placement; placed with "
                 "the nearest rotation",
                 unrotatedInstances);
    if (visualCollisionModels)
        zt::info("iw3: %zu static model(s) without an IW3 collision LOD collide by their visual "
                 "mesh (bullets and grenades)",
                 visualCollisionModels);
}

void BuildShadowScene(const std::vector<BrushModel> &brushModels,
                      const SourceStaticModels &staticModels, const RenderPlan &plan,
                      replaysunshadow::Scene &scene)
{
    const auto append = [&](const Surface &source, const unsigned material,
                            const SourceStaticModelInstance *instance,
                            const std::array<Vec3, 3> *axis) {
        if (source.indices.size() % 3 || source.vertices.empty())
            throw std::runtime_error("invalid IW3 shadow-caster surface");
        if (source.indices.empty())
            return;
        replaysunshadow::Surface caster;
        caster.material = material;
        caster.vertices.reserve(source.indices.size());
        // IW3 BSP indices already have the winding expected by Replay's sun
        // rasterizer. Reversing them culls sun-facing single-sided geometry.
        for (std::size_t first = 0; first < source.indices.size(); first += 3)
            for (const std::size_t corner : {0u, 1u, 2u})
            {
                const std::uint32_t index = source.indices[first + corner];
                if (index >= source.vertices.size())
                    throw std::runtime_error("IW3 shadow triangle references a missing vertex");
                const Vertex &vertex = source.vertices[index];
                replaysunshadow::Vertex output;
                output.position = instance
                                      ? Add(instance->origin,
                                            Multiply(Transform(*axis, vertex.position),
                                                     instance->scale))
                                      : vertex.position;
                output.uv = vertex.uv;
                output.alpha = vertex.color[3] / 255.0f;
                for (const float coordinate : output.position)
                    if (!std::isfinite(coordinate) || std::abs(coordinate) > 1.0e7f)
                        throw std::runtime_error("IW3 shadow vertex is outside world range");
                for (const float coordinate : output.uv)
                    if (!std::isfinite(coordinate) || std::abs(coordinate) > 1.0e6f)
                        throw std::runtime_error("IW3 shadow UV is outside supported range");
                caster.vertices.push_back(output);
            }
        scene.surfaces.push_back(std::move(caster));
    };

    if (brushModels.empty())
        throw std::runtime_error("IW3 shadow scene has no fixed world brush model");
    for (const Surface &surface : brushModels.front().surfaces)
    {
        const auto found = plan.worldShadowMaterials.find(surface.material);
        if (found != plan.worldShadowMaterials.end())
            append(surface, found->second, nullptr, nullptr);
    }
    const std::size_t worldSurfaceCount = scene.surfaces.size();
    for (const SourceStaticModelInstance &instance : staticModels.instances)
    {
        if (instance.model >= staticModels.models.size())
            throw std::runtime_error("IW3 shadow instance references a missing model");
        const SourceStaticModel &model = staticModels.models[instance.model];
        if (model.lods.empty())
            throw std::runtime_error("IW3 shadow instance model has no render LOD");
        const auto axis = StaticModelAxis(instance);
        for (const Surface &surface : model.lods.front().surfaces)
        {
            const auto found = plan.modelShadowMaterials.find(surface.material);
            if (found == plan.modelShadowMaterials.end())
                continue;
            if (model.name == "com_ladder_wood")
            {
                Surface corrected = surface;
                AddLadderMidpointRungs(corrected, 0);
                append(corrected, found->second, &instance, &axis);
            }
            else
            {
                append(surface, found->second, &instance, &axis);
            }
        }
    }
    const std::size_t triangles = std::accumulate(
        scene.surfaces.begin(), scene.surfaces.end(), std::size_t{},
        [](const std::size_t count, const replaysunshadow::Surface &surface) {
            return count + surface.vertices.size() / 3;
        });
    zt::info("iw3: prepared sun-shadow scene: %zu material(s), %zu fixed-world and %zu "
             "static-model surface(s), %zu triangle(s)",
             scene.materials.size(), worldSurfaceCount,
             scene.surfaces.size() - worldSurfaceCount, triangles);
}

struct GlassFaceUv
{
    std::string material;
    std::array<float, 4> gradient{};
    Vec2 atSample{};
};

std::optional<GlassFaceUv> BestGlassFaceUv(const BrushModel &model, const RenderPlan &plan,
                                         const std::array<std::size_t, 2> plane,
                                         const Vec3 &sample, const bool intactOnly,
                                         const std::string &wantedMaterial = {})
{
    std::optional<GlassFaceUv> best;
    float largestFace = 0;
    for (const Surface &surface : model.surfaces)
    {
        const auto material = plan.materials.find(surface.material);
        if (material == plan.materials.end() || material->second.glassMaterial.empty() ||
            (intactOnly && surface.material.find("shattered") != std::string::npos) ||
            (!wantedMaterial.empty() && material->second.glassMaterial != wantedMaterial))
            continue;
        for (std::size_t index = 0; index + 2 < surface.indices.size(); index += 3)
        {
            const Vertex &a = surface.vertices.at(surface.indices[index]);
            const Vertex &b = surface.vertices.at(surface.indices[index + 1]);
            const Vertex &c = surface.vertices.at(surface.indices[index + 2]);
            const float bx = b.position[plane[0]] - a.position[plane[0]];
            const float by = b.position[plane[1]] - a.position[plane[1]];
            const float cx = c.position[plane[0]] - a.position[plane[0]];
            const float cy = c.position[plane[1]] - a.position[plane[1]];
            const float determinant = bx * cy - by * cx;
            if (std::abs(determinant) <= std::max(0.0001f, largestFace))
                continue;
            largestFace = std::abs(determinant);
            GlassFaceUv candidate;
            candidate.material = material->second.glassMaterial;
            for (std::size_t channel = 0; channel < 2; ++channel)
            {
                const float buv = b.uv[channel] - a.uv[channel];
                const float cuv = c.uv[channel] - a.uv[channel];
                const float dx = (buv * cy - cuv * by) / determinant;
                const float dy = (cuv * bx - buv * cx) / determinant;
                candidate.gradient[channel * 2] = dx;
                candidate.gradient[channel * 2 + 1] = dy;
                candidate.atSample[channel] = a.uv[channel] +
                    dx * (sample[plane[0]] - a.position[plane[0]]) +
                    dy * (sample[plane[1]] - a.position[plane[1]]);
            }
            best = std::move(candidate);
        }
    }
    return best;
}

Json BuildGlassPanes(const std::vector<BrushModel> &models, const RenderPlan &plan,
                     const std::string &entityText, CollisionData &collision,
                     std::set<std::uint32_t> &glassModels,
                     std::set<std::string> &glassTargets,
                     const SourceStaticModels &entityModels)
{
    Json panes = Json::array();
    const std::vector<EntityFields> entities = ParseEntityFields(entityText);
    const auto linkedBrush = [&](const EntityFields &from) -> const EntityFields * {
        const auto target = from.find("target");
        if (target == from.end())
            return nullptr;
        const EntityFields *match = nullptr;
        for (const EntityFields &candidate : entities)
        {
            const auto name = candidate.find("targetname");
            if (name == candidate.end() || name->second != target->second)
                continue;
            if (match)
                return nullptr;
            match = &candidate;
        }
        return match && match->contains("classname") &&
                       match->at("classname") == "script_brushmodel"
                   ? match
                   : nullptr;
    };
    for (const EntityFields &entity : entities)
    {
        const auto classname = entity.find("classname");
        const auto modelValue = entity.find("model");
        if (classname == entity.end() || classname->second != "script_brushmodel" ||
            modelValue == entity.end() || modelValue->second.size() < 2 ||
            modelValue->second.front() != '*')
            continue;

        std::uint32_t modelIndex{};
        const char *first = modelValue->second.data() + 1;
        const char *last = modelValue->second.data() + modelValue->second.size();
        const auto parsed = std::from_chars(first, last, modelIndex);
        if (parsed.ec != std::errc{} || parsed.ptr != last || modelIndex >= models.size())
            throw std::runtime_error("IW3 glass entity references an invalid brush model");

        const BrushModel &model = models[modelIndex];
        const bool intactGlass =
            std::any_of(model.surfaces.begin(), model.surfaces.end(), [&](const Surface &surface) {
                const auto material = plan.materials.find(surface.material);
                // Additive and multiply layers (blend != 0) are not panes.
                return material != plan.materials.end() &&
                       material->second.kind == SurfaceKind::glass &&
                       material->second.blend == 0 &&
                       surface.material.find("shattered") == std::string::npos;
            });
        if (!intactGlass)
            continue;

        Vec3 size{}, center{};
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            size[axis] = model.maximum[axis] - model.minimum[axis];
            center[axis] = (model.minimum[axis] + model.maximum[axis]) * 0.5f;
        }
        const std::size_t thin =
            static_cast<std::size_t>(std::min_element(size.begin(), size.end()) - size.begin());
        std::array<std::size_t, 2> plane{};
        std::size_t planeIndex = 0;
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (axis != thin)
                plane[planeIndex++] = axis;
        const float halfWidth = size[plane[0]] * 0.5f;
        const float halfHeight = size[plane[1]] * 0.5f;
        if (halfWidth < 0.125f || halfHeight < 0.125f || halfWidth * 32.0f > 32767.0f ||
            halfHeight * 32.0f > 32767.0f)
            throw std::runtime_error("IW3 glass pane dimensions exceed Replay limits");

        const auto intactFace = BestGlassFaceUv(model, plan, plane, center, true);
        if (!intactFace)
            throw std::runtime_error("IW3 glass pane has no nondegenerate textured face");
        const std::string paneMaterial = intactFace->material;
        std::array<float, 4> texVecs{};
        for (std::size_t index = 0; index < texVecs.size(); ++index)
            texVecs[index] = intactFace->gradient[index] / 32.0f;
        const Vec2 texOrigin = intactFace->atSample;

        // IW3 scripted glass commonly links its intact brush to the shattered
        // replacement by target/targetname. Follow that authored link rather
        // than assuming adjacent brush-model indices or a material name suffix.
        const EntityFields *shatteredEntity = nullptr;
        std::size_t shatteredModel = 0;
        if (const EntityFields *replacement = linkedBrush(entity))
        {
            const auto linkedModel = replacement->find("model");
            if (linkedModel != replacement->end() && linkedModel->second.size() > 1 &&
                linkedModel->second.front() == '*')
            {
                std::uint32_t linkedIndex{};
                const char *begin = linkedModel->second.data() + 1;
                const char *end = linkedModel->second.data() + linkedModel->second.size();
                const auto number = std::from_chars(begin, end, linkedIndex);
                if (number.ec == std::errc{} && number.ptr == end &&
                    linkedIndex < models.size())
                {
                    // The linked brush is the pane's broken state; its own
                    // material is not used (Replay fractures the intact pane),
                    // but it must leave the world. IW3 shattered textures such
                    // as com_glass_shattered_inside_fb are not glass materials.
                    const bool brokenGlass = std::ranges::any_of(
                        models[linkedIndex].surfaces, [&](const Surface &surface) {
                            const auto material = plan.materials.find(surface.material);
                            return Lowercase(surface.material).find("shatter") !=
                                       std::string::npos ||
                                   (material != plan.materials.end() &&
                                    material->second.kind == SurfaceKind::glass);
                        });
                    if (brokenGlass)
                    {
                        shatteredEntity = replacement;
                        shatteredModel = linkedIndex;
                    }
                }
            }
        }

        const auto axis = EntityAxis(entity);
        const Vec3 localU{plane[0] == 0 ? 1.0f : 0.0f, plane[0] == 1 ? 1.0f : 0.0f,
                          plane[0] == 2 ? 1.0f : 0.0f};
        const Vec3 localV{plane[1] == 0 ? 1.0f : 0.0f, plane[1] == 1 ? 1.0f : 0.0f,
                          plane[1] == 2 ? 1.0f : 0.0f};
        const Vec3 worldU = Transform(axis, localU);
        const Vec3 worldV = Transform(axis, localV);
        const Vec3 worldNormal = Unit(Cross(worldU, worldV));
        Vec3 origin = Transform(axis, center);
        Vec3 entityOrigin{};
        if (const auto found = entity.find("origin"); found != entity.end())
            entityOrigin = ParseVector(found->second);
        origin = Add(origin, entityOrigin);
        if (panes.size() >= std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("IW3 glass has too many native collision pieces");
        const auto glassId = static_cast<std::uint16_t>(panes.size() + 1);
        const std::size_t sourceHullCount = collision.hulls.size();
        std::size_t matchedHulls = 0;
        std::size_t duplicateHulls = 0;
        const auto samePaneHull = [](const CollisionHull &left, const CollisionHull &right) {
            if (left.model != right.model || left.contents != right.contents ||
                left.surfaceFlags != right.surfaceFlags || left.glassId != right.glassId ||
                left.materialOverride != right.materialOverride ||
                left.ladderPlanes != right.ladderPlanes ||
                left.ladderModelTangents != right.ladderModelTangents ||
                left.ladderRungOffset != right.ladderRungOffset || !left.slabs.empty() ||
                !right.slabs.empty() || left.points.size() != right.points.size())
                return false;
            auto leftPoints = left.points;
            auto rightPoints = right.points;
            std::ranges::sort(leftPoints);
            std::ranges::sort(rightPoints);
            return leftPoints == rightPoints;
        };
        for (std::size_t hullIndex = 0; hullIndex < sourceHullCount; ++hullIndex)
        {
            auto &sourceHull = collision.hulls[hullIndex];
            if (sourceHull.model != modelIndex)
                continue;
            CollisionHull paneHull = sourceHull;
            paneHull.model = 0;
            paneHull.contents = iw8::havok::kBreakableGlassContents;
            paneHull.glassId = glassId;
            paneHull.slabs.clear();
            for (auto &point : paneHull.points)
                point = Add(Transform(axis, point), entityOrigin);
            sourceHull.contents = iw8::havok::kBreakableGlassContents;
            sourceHull.glassId = glassId;
            const bool duplicate = std::any_of(
                collision.hulls.begin() + sourceHullCount, collision.hulls.end(),
                [&](const CollisionHull &previous) { return samePaneHull(previous, paneHull); });
            if (duplicate)
                ++duplicateHulls;
            else
                collision.hulls.push_back(std::move(paneHull));
            ++matchedHulls;
        }
        if (!matchedHulls)
        {
            // A nonsolid (render-only) pane: native glass still needs a world
            // hull to break, so give it its bounds, at least half a unit thick.
            CollisionHull paneHull;
            paneHull.contents = iw8::havok::kBreakableGlassContents;
            paneHull.materialOverride = 9; // native surftype glass_pane
            paneHull.glassId = glassId;
            Vec3 low = model.minimum, high = model.maximum;
            if (high[thin] - low[thin] < 0.5f)
            {
                low[thin] = center[thin] - 0.25f;
                high[thin] = center[thin] + 0.25f;
            }
            for (unsigned corner = 0; corner < 8; ++corner)
                paneHull.points.push_back(
                    Add(Transform(axis, Vec3{corner & 1 ? high[0] : low[0],
                                             corner & 2 ? high[1] : low[1],
                                             corner & 4 ? high[2] : low[2]}),
                        entityOrigin));
            collision.hulls.push_back(std::move(paneHull));
            zt::info("iw3: glass pane %u has no collision brush; gave it a box hull", glassId);
        }
        if (duplicateHulls)
            zt::info("iw3: removed %zu exact duplicate native hull(s) from glass pane %u",
                     duplicateHulls, glassId);
        // FxGlass and the numbered world hull replace the IW3 script-controlled
        // intact, shattered and broken brush states. Without the IW3 script, all
        // three would spawn together and leave a solid brush in the opening.
        glassModels.insert(modelIndex);
        if (const auto name = entity.find("targetname"); name != entity.end())
            glassTargets.insert(name->second);
        if (shatteredEntity)
        {
            glassModels.insert(static_cast<std::uint32_t>(shatteredModel));
            // The prefab continues past the shattered state: clip brush, then
            // the final broken remnants (jagged shards left in the frame). All
            // of them are superseded by the native pane, so leave the world.
            const EntityFields *stage = shatteredEntity;
            for (std::size_t depth = 0; depth < 8; ++depth)
            {
                const EntityFields *next = linkedBrush(*stage);
                if (!next)
                    break;
                const auto nextModel = next->find("model");
                if (nextModel == next->end() || nextModel->second.size() < 2 ||
                    nextModel->second.front() != '*')
                    break;
                std::uint32_t nextIndex{};
                const char *begin = nextModel->second.data() + 1;
                const char *end = nextModel->second.data() + nextModel->second.size();
                const auto nextNumber = std::from_chars(begin, end, nextIndex);
                if (nextNumber.ec != std::errc{} || nextNumber.ptr != end || nextIndex >= models.size() ||
                    !glassModels.insert(nextIndex).second)
                    break;
                stage = next;
            }
        }
        // FxGlassDef stores fracture extrusion in eighths of an IW world unit.
        // Keep the full brush depth for collision, but convert its half-depth for
        // the native shard geometry (an 8-unit IW3 pane becomes 0.5 here).
        const float fractureHalfThickness = std::max(0.125f, size[thin] / 16.0f);
        panes.push_back({{"material", paneMaterial},
                         {"materialShattered", paneMaterial},
                         {"texVecs", texVecs},
                         {"texCoordOrigin", texOrigin},
                         {"origin", origin},
                         {"quaternion", QuaternionFromBasis(worldU, worldV, worldNormal)},
                         {"halfWidth", halfWidth},
                         {"halfHeight", halfHeight},
                         {"halfThickness", fractureHalfThickness}});
    }

    // IW3 window prefabs keep the visible pane in models instead of brushes:
    // intact script_model -> shattered script_model -> clip script_brushmodel
    // -> shard script_brushmodel, swapped by map GSC. Build the same native
    // pane from the intact model's glass box and the clip brush's collision.
    const auto byName = [&](const std::string &name) -> const EntityFields * {
        const EntityFields *match = nullptr;
        for (const EntityFields &candidate : entities)
            if (const auto found = candidate.find("targetname");
                found != candidate.end() && found->second == name)
            {
                if (match)
                    return nullptr;
                match = &candidate;
            }
        return match;
    };
    const auto targetOf = [&](const EntityFields &from,
                              const char *classname) -> const EntityFields * {
        const auto target = from.find("target");
        if (target == from.end())
            return nullptr;
        const EntityFields *found = byName(target->second);
        const auto type = found ? found->find("classname") : from.end();
        return found && type != found->end() && type->second == classname ? found : nullptr;
    };
    const auto brushIndex = [&](const EntityFields &entity) -> std::optional<std::uint32_t> {
        const auto value = entity.find("model");
        if (value == entity.end() || value->second.size() < 2 || value->second.front() != '*')
            return std::nullopt;
        std::uint32_t index{};
        const char *begin = value->second.data() + 1;
        const char *end = value->second.data() + value->second.size();
        const auto parsed = std::from_chars(begin, end, index);
        if (parsed.ec != std::errc{} || parsed.ptr != end || index >= models.size())
            return std::nullopt;
        return index;
    };
    const auto modelBox = [&](const EntityFields &entity) -> std::optional<BrushModel> {
        const auto name = entity.find("model");
        if (name == entity.end())
            return std::nullopt;
        for (const SourceStaticModel &model : entityModels.models)
        {
            if (model.name != name->second || model.lods.empty())
                continue;
            BrushModel box;
            box.surfaces = model.lods.front().surfaces;
            box.minimum = {std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity()};
            box.maximum = {-std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity()};
            for (const Surface &surface : box.surfaces)
                for (const Vertex &vertex : surface.vertices)
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        box.minimum[axis] = std::min(box.minimum[axis], vertex.position[axis]);
                        box.maximum[axis] = std::max(box.maximum[axis], vertex.position[axis]);
                    }
            if (box.surfaces.empty() || !(box.minimum[0] <= box.maximum[0]))
                return std::nullopt;
            return box;
        }
        return std::nullopt;
    };
    std::size_t prefabPanes = 0;
    for (const EntityFields &intact : entities)
    {
        const auto classname = intact.find("classname");
        if (classname == intact.end() || classname->second != "script_model")
            continue;
        const EntityFields *shattered = targetOf(intact, "script_model");
        const EntityFields *clip = shattered ? targetOf(*shattered, "script_brushmodel") : nullptr;
        const EntityFields *shards = clip ? targetOf(*clip, "script_brushmodel") : nullptr;
        const auto clipIndex = clip ? brushIndex(*clip) : std::nullopt;
        const auto shardIndex = shards ? brushIndex(*shards) : std::nullopt;
        if (!clipIndex || !shardIndex || glassModels.contains(*clipIndex))
            continue;
        const auto intactBox = modelBox(intact);
        const auto shatteredBox = modelBox(*shattered);
        if (!intactBox || !shatteredBox)
            continue;

        Vec3 size{}, center{};
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            size[axis] = intactBox->maximum[axis] - intactBox->minimum[axis];
            center[axis] = (intactBox->minimum[axis] + intactBox->maximum[axis]) * 0.5f;
        }
        const std::size_t thin =
            static_cast<std::size_t>(std::min_element(size.begin(), size.end()) - size.begin());
        std::array<std::size_t, 2> plane{};
        std::size_t planeIndex = 0;
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (axis != thin)
                plane[planeIndex++] = axis;
        const float halfWidth = size[plane[0]] * 0.5f;
        const float halfHeight = size[plane[1]] * 0.5f;
        if (halfWidth < 0.125f || halfHeight < 0.125f || halfWidth * 32.0f > 32767.0f ||
            halfHeight * 32.0f > 32767.0f)
            continue;
        const auto intactFace = BestGlassFaceUv(*intactBox, plan, plane, center, true);
        const auto shatteredFace = BestGlassFaceUv(*shatteredBox, plan, plane, center, false);
        if (!intactFace || !shatteredFace)
            continue;
        std::array<float, 4> texVecs{};
        for (std::size_t index = 0; index < texVecs.size(); ++index)
            texVecs[index] = intactFace->gradient[index] / 32.0f;
        const auto axis = EntityAxis(intact);
        const Vec3 localU{plane[0] == 0 ? 1.0f : 0.0f, plane[0] == 1 ? 1.0f : 0.0f,
                          plane[0] == 2 ? 1.0f : 0.0f};
        const Vec3 localV{plane[1] == 0 ? 1.0f : 0.0f, plane[1] == 1 ? 1.0f : 0.0f,
                          plane[1] == 2 ? 1.0f : 0.0f};
        const Vec3 worldU = Transform(axis, localU);
        const Vec3 worldV = Transform(axis, localV);
        const Vec3 worldNormal = Unit(Cross(worldU, worldV));
        Vec3 entityOrigin{};
        if (const auto found = intact.find("origin"); found != intact.end())
            entityOrigin = ParseVector(found->second);
        const Vec3 origin = Add(Transform(axis, center), entityOrigin);

        if (panes.size() >= std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("IW3 glass has too many native collision pieces");
        const auto glassId = static_cast<std::uint16_t>(panes.size() + 1);
        const auto clipAxis = EntityAxis(*clip);
        Vec3 clipOrigin{};
        if (const auto found = clip->find("origin"); found != clip->end())
            clipOrigin = ParseVector(found->second);
        const std::size_t sourceHullCount = collision.hulls.size();
        std::size_t matchedHulls = 0;
        for (std::size_t hullIndex = 0; hullIndex < sourceHullCount; ++hullIndex)
        {
            auto &sourceHull = collision.hulls[hullIndex];
            if (sourceHull.model != *clipIndex)
                continue;
            CollisionHull paneHull = sourceHull;
            paneHull.model = 0;
            paneHull.contents = iw8::havok::kBreakableGlassContents;
            paneHull.glassId = glassId;
            paneHull.slabs.clear();
            for (auto &point : paneHull.points)
                point = Add(Transform(clipAxis, point), clipOrigin);
            collision.hulls.push_back(std::move(paneHull));
            ++matchedHulls;
        }
        if (!matchedHulls)
            continue;
        glassModels.insert(*clipIndex);
        glassModels.insert(*shardIndex);
        if (const auto name = intact.find("targetname"); name != intact.end())
            glassTargets.insert(name->second);
        const float fractureHalfThickness = std::max(0.125f, size[thin] / 16.0f);
        panes.push_back({{"material", intactFace->material},
                         {"materialShattered", intactFace->material},
                         {"texVecs", texVecs},
                         {"texCoordOrigin", intactFace->atSample},
                         {"origin", origin},
                         {"quaternion", QuaternionFromBasis(worldU, worldV, worldNormal)},
                         {"halfWidth", halfWidth},
                         {"halfHeight", halfHeight},
                         {"halfThickness", fractureHalfThickness}});
        ++prefabPanes;
    }
    if (prefabPanes)
        zt::info("iw3: converted %zu IW3 window prefab(s) to native breakable glass", prefabPanes);
    return panes;
}

Json BuildRender(const Json &world, const VisibilityGroups &visibility,
                 const std::vector<BrushModel> &models, RenderPlan &plan,
                 const std::string &entities, const SourceStaticModels &sourceStaticModels,
                 const replayrender::StaticModels &staticModels, CollisionData &collision,
                 std::size_t &triangleCount,
                 std::set<std::uint32_t> &glassModels,
                 std::set<std::string> &glassTargets,
                 const SourceStaticModels &entityModels)
{
    const unsigned sunCount = world.at("sun_primary_light_index").get<unsigned>();
    if (sunCount > 5)
        throw std::runtime_error("IW3 sun-light count exceeds Replay surface-shadow mask");
    const unsigned sunShadowMask = ((1u << sunCount) - 1u) << 1;
    Json output = {{"schema", 1},
                   {"material", plan.material},
                   {"materialDefinition", plan.materialDefinition},
                   {"additionalMaterials", plan.additionalMaterials},
                   {"assetMaterials", plan.assetMaterials},
                   {"reflectionProbeArrayImage", plan.reflectionProbeArrayImage},
                   {"nativeLightmaps", Json::array()},
                   {"atlasVertexLayout", 3},
                   {"brushModels", Json::array()},
                   {"glassPanes", Json::array()},
                   {"reflectionProbes", Json::array()},
                   {"surfaces", Json::array()}};
    for (const auto &lightmap : plan.nativeLightmaps)
    {
        Json images = Json::array();
        for (std::size_t channel = 0; channel < lightmap.files.size(); ++channel)
            images.push_back({{"format", lightmap.formats[channel]},
                              {"pixels", lightmap.files[channel]}});
        output["nativeLightmaps"].push_back(
            {{"width", lightmap.width}, {"height", lightmap.height}, {"images", images}});
    }
    std::vector<std::set<unsigned>> treeSurfaces(visibility.treeBounds.size());
    std::set<unsigned> globalSurfaces;
    struct ProbeBounds
    {
        Vec3 minimum{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity()};
        Vec3 maximum{-std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity()};
        bool valid{};

        void Add(const Vec3 &point)
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                minimum[axis] = std::min(minimum[axis], point[axis]);
                maximum[axis] = std::max(maximum[axis], point[axis]);
            }
            valid = true;
        }

        void Add(const Json &bounds)
        {
            if (!bounds.is_array() || bounds.size() != 2)
                throw std::runtime_error("invalid IW3 reflection-probe cell bounds");
            Add(ReadVector<3>(bounds.at(0)));
            Add(ReadVector<3>(bounds.at(1)));
        }
    };
    if (plan.reflectionProbes.empty() || plan.reflectionProbes.size() > 256)
        throw std::runtime_error("invalid IW3 reflection-probe render plan");
    // Geometry and cell lists still use source indices, including the reserved
    // default probe. The render plan contains only selectable captures.
    const auto sourceProbeCount = world.at("reflection_probes").size();
    std::vector<ProbeBounds> geometryProbeBounds(sourceProbeCount);
    std::vector<ProbeBounds> cellProbeBounds(sourceProbeCount);
    // Dynamic models (the viewmodel) take the instance volume containing them.
    // IW3 assigns probes per surface, and one large merged surface can sit far
    // from its probe; its whole extent would make that probe's volume span the
    // map. Keep IW3's wall-aware assignment, but grow a probe's volume only by
    // points for which it is (nearly) the closest selectable capture.
    std::vector<std::optional<Vec3>> probeOrigins(sourceProbeCount);
    for (const auto &probe : plan.reflectionProbes)
        if (probe.sourceIndex < sourceProbeCount && probe.sourceIndex)
            probeOrigins[probe.sourceIndex] = probe.origin;
    const auto addProbePoint = [&](const std::size_t probe, const Vec3 &point) {
        if (!probeOrigins[probe])
        {
            geometryProbeBounds[probe].Add(point);
            return;
        }
        const auto distance = [&](const Vec3 &origin) {
            const Vec3 delta = Subtract(point, origin);
            return Dot(delta, delta);
        };
        float nearest = std::numeric_limits<float>::infinity();
        for (const auto &origin : probeOrigins)
            if (origin)
                nearest = std::min(nearest, distance(*origin));
        constexpr float slackSquared = 1.25f * 1.25f;
        if (distance(*probeOrigins[probe]) <= nearest * slackSquared)
            geometryProbeBounds[probe].Add(point);
    };

    if (sourceStaticModels.instances.size() != staticModels.instances.size())
        throw std::runtime_error("IW3 static-model reflection assignments do not match Replay instances");
    for (std::size_t index = 0; index < sourceStaticModels.instances.size(); ++index)
    {
        const SourceStaticModelInstance &sourceInstance = sourceStaticModels.instances[index];
        const replayrender::StaticModelInstance &targetInstance = staticModels.instances[index];
        if (sourceInstance.reflectionProbe >= sourceProbeCount ||
            sourceInstance.model >= staticModels.models.size() ||
            targetInstance.model != sourceInstance.model)
            throw std::runtime_error("IW3 static-model reflection assignment has invalid indices");

        const replaybounds::Bounds &localBounds = staticModels.models[sourceInstance.model].bounds;
        const std::array<Vec3, 3> axis = StaticModelAxis(sourceInstance);
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            Vec3 local{};
            for (std::size_t component = 0; component < 3; ++component)
                local[component] = localBounds.midpoint[component] +
                                   ((corner & (1u << component)) ? 1.0f : -1.0f) *
                                       localBounds.halfSize[component];
            const Vec3 worldPosition = Add(
                sourceInstance.origin,
                Multiply(Transform(axis, local), sourceInstance.scale));
            addProbePoint(sourceInstance.reflectionProbe, worldPosition);
        }
    }

    const auto appendSky = [&] {
        MaterialPlan skyPlan;
        skyPlan.kind = SurfaceKind::sky;
        const unsigned skyMaterial = MaterialIndex(plan, skyPlan, false);
        using SkyMapping = Vec3 (*)(float, float);
        const std::array<SkyMapping, 6> mappings{[](float u, float v) { return Vec3{1, -v, -u}; },
                                                 [](float u, float v) { return Vec3{-1, -v, u}; },
                                                 [](float u, float v) { return Vec3{u, 1, v}; },
                                                 [](float u, float v) { return Vec3{u, -1, -v}; },
                                                 [](float u, float v) { return Vec3{u, -v, 1}; },
                                                 [](float u, float v) { return Vec3{-u, -v, -1}; }};
        for (std::size_t face = 0; face < mappings.size(); ++face)
        {
            Json sky = {{"vertices", Json::array()},
                        {"indices", {0, 1, 2, 2, 3, 0}},
                        {"materialIndex", skyMaterial},
                        {"renderClass", "sky"}};
            std::array<Vec3, 4> positions{};
            const std::array<Vec2, 4> uvs{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
            for (std::size_t index = 0; index < uvs.size(); ++index)
            {
                positions[index] = Multiply(
                    mappings[face](uvs[index][0] * 2 - 1, uvs[index][1] * 2 - 1), 32768.0f);
                sky["vertices"].push_back(
                    {{"position", positions[index]},
                     {"uv", uvs[index]},
                     {"normal", 3489135616u},
                     {"lightmapUV", {static_cast<float>(plan.skyTiles[face]) + 0.25f, 1.25f}}});
            }
            if (Dot(Cross(Subtract(positions[1], positions[0]),
                          Subtract(positions[2], positions[0])),
                    positions[0]) < 0)
                sky["indices"] = {0, 2, 1, 2, 0, 3};
            output["surfaces"].push_back(std::move(sky));
            const unsigned destinationSurface =
                static_cast<unsigned>(output["surfaces"].size() - 1);
            globalSurfaces.insert(destinationSurface);
            triangleCount += 2;
        }
    };

    for (std::size_t modelIndex = 0; modelIndex < models.size(); ++modelIndex)
    {
        const BrushModel &model = models[modelIndex];
        std::vector<const Surface *> ordered;
        ordered.reserve(model.surfaces.size());
        for (const Surface &surface : model.surfaces)
        {
            const auto found = plan.materials.find(surface.material);
            if (found == plan.materials.end())
                throw std::runtime_error("IW3 render plan omitted material " + surface.material);
            if (found->second.kind != SurfaceKind::skipped)
                ordered.push_back(&surface);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [&](const Surface *a, const Surface *b) {
            const auto &ma = plan.materials.at(a->material);
            const auto &mb = plan.materials.at(b->material);
            const bool ag = ma.kind == SurfaceKind::glass;
            const bool bg = mb.kind == SurfaceKind::glass;
            if (ag != bg)
                return !ag;
            if (ma.kind != mb.kind)
                return static_cast<unsigned>(ma.kind) < static_cast<unsigned>(mb.kind);
            if (ma.environment != mb.environment)
                return ma.environment < mb.environment;
            if (a->visibilityGroup != b->visibilityGroup)
                return a->visibilityGroup < b->visibilityGroup;
            if (a->reflectionProbe != b->reflectionProbe)
                return a->reflectionProbe < b->reflectionProbe;
            return a->material < b->material;
        });

        const std::size_t firstSurface = output["surfaces"].size();
        Json *target = nullptr;
        std::size_t targetVertices = 0;
        std::size_t targetIndices = 0;
        std::string activeKey;
        bool skyAdded = modelIndex != 0;
        for (const Surface *source : ordered)
        {
            const Surface &surface = *source;
            const MaterialPlan &material = plan.materials.at(surface.material);
            if (material.kind == SurfaceKind::glass && !skyAdded)
            {
                appendSky();
                skyAdded = true;
                target = nullptr;
                activeKey.clear();
            }
            // Opaque BSP surfaces draw with the generated atlas material. In
            // Replay 1.20 the native world materials were drawn through the
            // atlas world pipeline anyway (tile 0 of a layer-1 record), which
            // showed one 32-texel corner of each texture per repeat (Nuketown
            // fixes-29..31 grass, siding, garage doors). MW120R_NATIVE_WORLD=1
            // keeps the retained Replay world shader path for investigation.
            static const bool nativeWorldPath = std::getenv("MW120R_NATIVE_WORLD") != nullptr;
            const bool nativeWorld =
                material.kind == SurfaceKind::opaque && surface.lightmap >= 0 && nativeWorldPath;
            const unsigned materialIndex = MaterialIndex(plan, material, nativeWorld);
            // Replay's world shader carries texture coordinates at reduced
            // precision; stock maps keep them near zero. IW3 world UVs are
            // absolute (Nuketown reaches 164 repeats), which snapped them into
            // blocks of texels. Repeating textures tolerate whole-tile shifts,
            // so each triangle moves to the tile of its UV centroid; vertices
            // shared by triangles in different tiles are duplicated.
            std::vector<std::array<int, 2>> triangleTiles(surface.indices.size() / 3);
            std::size_t emittedVertices = surface.vertices.size();
            if (nativeWorld)
            {
                std::set<std::tuple<std::uint32_t, int, int>> unique;
                for (std::size_t triangle = 0; triangle < triangleTiles.size(); ++triangle)
                {
                    Vec2 centroid{};
                    float largest = 0.0f;
                    for (std::size_t corner = 0; corner < 3; ++corner)
                    {
                        const Vec2 &uv = surface.vertices[surface.indices[triangle * 3 + corner]].uv;
                        for (std::size_t axis = 0; axis < 2; ++axis)
                        {
                            centroid[axis] += uv[axis] / 3.0f;
                            largest = std::max(largest, std::abs(uv[axis]));
                        }
                    }
                    // Clamped decals and trims stay within a tile or two.
                    if (largest > 2.0f)
                        triangleTiles[triangle] = {static_cast<int>(std::floor(centroid[0])),
                                                   static_cast<int>(std::floor(centroid[1]))};
                    for (std::size_t corner = 0; corner < 3; ++corner)
                        unique.insert({surface.indices[triangle * 3 + corner],
                                       triangleTiles[triangle][0], triangleTiles[triangle][1]});
                }
                emittedVertices = unique.size();
                if (emittedVertices > 60000)
                    throw std::runtime_error("IW3 world surface exceeds Replay's vertex batch after "
                                             "UV recentring: " + surface.material);
            }
            std::ostringstream key;
            key << materialIndex;
            for (const float value : material.environment)
                key << ':' << value;
            key << ':' << surface.visibilityGroup;
            key << ':' << surface.reflectionProbe;
            // Several IW3 materials share one Replay fallback material. Keep
            // their caster eligibility separate when merging fixed and entity brushes.
            key << ':' << material.castsShadow;
            if (!target || activeKey != key.str() ||
                targetVertices + emittedVertices > 60000 ||
                targetIndices + surface.indices.size() > 65535u * 3u)
            {
                output["surfaces"].push_back({{"vertices", Json::array()},
                                              {"indices", Json::array()},
                                              {"materialIndex", materialIndex},
                                              {"atlasVertexLayout", nativeWorld ? 1u : 3u},
                                              {"lightmapIndex", 0u},
                                              {"opaque", material.kind == SurfaceKind::opaque},
                                              {"sunShadowMask", material.castsShadow ? sunShadowMask : 0u},
                                              {"reflectionProbe", surface.reflectionProbe},
                                              {"materialParameters", material.environment}});
                target = &output["surfaces"].back();
                targetVertices = targetIndices = 0;
                activeKey = key.str();
            }
            const unsigned destinationSurface =
                static_cast<unsigned>(output["surfaces"].size() - 1);
            if (modelIndex == 0)
            {
                if (surface.visibilityGroup == VisibilityGroups::Global)
                {
                    globalSurfaces.insert(destinationSurface);
                }
                else
                {
                    if (visibility.nativeTopology)
                    {
                        for (const auto tree : visibility.groupTrees.at(surface.visibilityGroup))
                            treeSurfaces.at(tree).insert(destinationSurface);
                    }
                    else
                        treeSurfaces.at(surface.visibilityGroup).insert(destinationSurface);
                }
            }
            std::map<std::tuple<std::uint32_t, int, int>, std::uint32_t> remap;
            for (std::size_t offset = 0; offset < surface.indices.size(); offset += 3)
            {
                const auto &tile = triangleTiles[offset / 3];
                const Vertex &a = surface.vertices[surface.indices[offset]];
                const Vertex &b = surface.vertices[surface.indices[offset + 1]];
                const Vertex &c = surface.vertices[surface.indices[offset + 2]];
                const Vec3 cross =
                    Cross(Subtract(b.position, a.position), Subtract(c.position, a.position));
                if (Dot(cross, cross) < 1.0e-12f)
                    throw std::runtime_error("degenerate render triangle in IW3 map");
                for (const std::uint32_t sourceIndex :
                     {surface.indices[offset], surface.indices[offset + 2],
                      surface.indices[offset + 1]})
                {
                    const std::tuple<std::uint32_t, int, int> remapKey{sourceIndex, tile[0],
                                                                       tile[1]};
                    auto found = remap.find(remapKey);
                    if (found == remap.end())
                    {
                        const Vertex &vertex = surface.vertices[sourceIndex];
                        const Vec2 uv{vertex.uv[0] - static_cast<float>(tile[0]),
                                      vertex.uv[1] - static_cast<float>(tile[1])};
                        if (surface.reflectionProbe >= geometryProbeBounds.size())
                            throw std::runtime_error(
                                "IW3 surface references an invalid reflection probe");
                        addProbePoint(surface.reflectionProbe, vertex.position);
                        const std::uint32_t index = static_cast<std::uint32_t>(targetVertices++);
                        const Vec2 lightmap =
                            EncodedLightmap(vertex, surface, material, plan, nativeWorld);
                        // The tile word carries the surface's IW3 reflection probe
                        // as converted slot + 1 above bit 16 (the shader reflects
                        // that probe alone); added in double so the lightmap
                        // fraction keeps its precision.
                        Json encoded = Json::array({lightmap[0], lightmap[1]});
                        if (!nativeWorld)
                        {
                            if (material.tile >= 0x10000)
                                throw std::runtime_error("IW3 atlas tile exceeds the 16-bit tile word");
                            encoded[0] = static_cast<double>(lightmap[0]) +
                                         65536.0 * ReflectionProbeTag(surface.reflectionProbe,
                                                                      geometryProbeBounds.size());
                        }
                        (*target)["vertices"].push_back(
                            {{"position", vertex.position},
                             {"uv", uv},
                             {"normal", PackedNormal(vertex.normal, vertex.tangent,
                                                     vertex.binormalSign, vertex.authoredTangent)},
                             {"lightmapUV", encoded},
                             {"color", vertex.color}});
                        found = remap.emplace(remapKey, index).first;
                    }
                    (*target)["indices"].push_back(found->second);
                    ++targetIndices;
                }
                ++triangleCount;
            }
        }

        if (!skyAdded)
            appendSky();
        const std::size_t surfaceCount = output["surfaces"].size() - firstSurface;
        if (surfaceCount > 65535)
            throw std::runtime_error("IW3 brush model exceeds Replay surface limits");
        output["brushModels"].push_back({{"firstSurface", firstSurface},
                                         {"surfaceCount", surfaceCount},
                                         {"bounds", {model.minimum, model.maximum}}});
    }
    if (output["surfaces"].empty() || output["surfaces"].size() > 4096)
    {
        throw std::runtime_error("IW3 render geometry exceeds Replay surface limits");
    }
    output["dpvs"] = {{"planes", world.at("dpvs").at("planes")},
                      {"nodes", world.at("dpvs").at("nodes")},
                      {"cells", Json::array()}};
    const auto &sourceCells = world.at("dpvs").at("cells");
    for (std::size_t cellIndex = 0; cellIndex < sourceCells.size(); ++cellIndex)
    {
        const auto &source = sourceCells.at(cellIndex);
        for (const auto &probeValue : source.at("reflection_probes"))
        {
            const auto probe = probeValue.get<unsigned>();
            if (probe >= cellProbeBounds.size())
                throw std::runtime_error("IW3 cell references an invalid reflection probe");
            cellProbeBounds[probe].Add(source.at("bounds"));
        }
        Json trees = Json::array();
        for (std::size_t treeIndex = 0; treeIndex < visibility.cellTrees.at(cellIndex).size();
             ++treeIndex)
        {
            const auto group = visibility.cellTrees.at(cellIndex).at(treeIndex);
            const auto &owned = treeSurfaces.at(group);
            const auto &ownedModels = visibility.treeModels.at(group);
            if (visibility.nativeTopology || !owned.empty() || !ownedModels.empty())
            {
                Json tree = {{"bounds", visibility.treeBounds.at(group)},
                             {"surfaces", std::vector<unsigned>(owned.begin(), owned.end())},
                             {"models", std::vector<unsigned>(ownedModels.begin(),
                                                                ownedModels.end())}};
                if (visibility.nativeTopology)
                {
                    tree["childCount"] = visibility.treeChildCounts.at(group);
                    tree["firstChild"] = visibility.treeFirstChildren.at(group);
                    if (!treeIndex && !globalSurfaces.empty())
                    {
                        auto all = tree.at("surfaces").get<std::vector<unsigned>>();
                        all.insert(all.end(), globalSurfaces.begin(), globalSurfaces.end());
                        std::ranges::sort(all);
                        all.erase(std::unique(all.begin(), all.end()), all.end());
                        tree["surfaces"] = std::move(all);
                        tree["bounds"] = world.at("bounds");
                    }
                }
                trees.push_back(std::move(tree));
            }
        }
        if (!visibility.nativeTopology && !globalSurfaces.empty())
            trees.push_back({{"bounds", source.at("bounds")},
                             {"surfaces", std::vector<unsigned>(globalSurfaces.begin(),
                                                                globalSurfaces.end())}});
        output["dpvs"]["cells"].push_back({{"bounds", source.at("bounds")},
                                           {"portals", source.at("portals")},
                                           {"trees", std::move(trees)}});
    }
    {
        // Shipped Replay maps are one DPVS cell (Shipment: no portals, nodes {1})
        // and leave occlusion to Umbra and GPU culling. Replay does not flood
        // IW3's portals, so props owned by another source cell vanished as the
        // camera crossed into a neighbouring cell. Merge every tree into one leaf.
        std::set<unsigned> surfaces, cellModels;
        Vec3 minimum{}, maximum{};
        bool first = true;
        for (const auto &cell : output["dpvs"]["cells"])
        {
            const auto add = [&](const Json &bounds) {
                const Vec3 low = ReadVector<3>(bounds.at(0)), high = ReadVector<3>(bounds.at(1));
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    minimum[axis] = first ? low[axis] : std::min(minimum[axis], low[axis]);
                    maximum[axis] = first ? high[axis] : std::max(maximum[axis], high[axis]);
                }
                first = false;
            };
            add(cell.at("bounds"));
            for (const auto &tree : cell.at("trees"))
            {
                add(tree.at("bounds"));
                for (const auto &surface : tree.at("surfaces"))
                    surfaces.insert(surface.get<unsigned>());
                for (const auto &model : tree.value("models", Json::array()))
                    cellModels.insert(model.get<unsigned>());
            }
        }
        Json tree = {{"bounds", {minimum, maximum}},
                     {"surfaces", std::vector<unsigned>(surfaces.begin(), surfaces.end())},
                     {"models", std::vector<unsigned>(cellModels.begin(), cellModels.end())}};
        if (visibility.nativeTopology)
        {
            tree["childCount"] = 0;
            tree["firstChild"] = 0;
        }
        // The merged cell's nodes {1} reference no plane; drop IW3's planes
        // when they exceed Replay's 16-bit table (mp_arnhem).
        if (output["dpvs"]["planes"].size() > 65535)
            output["dpvs"]["planes"] = Json::array();
        output["dpvs"]["nodes"] = Json::array({1});
        output["dpvs"]["cells"] = Json::array({{{"bounds", {minimum, maximum}},
                                                {"portals", Json::array()},
                                                {"trees", Json::array({std::move(tree)})}}});
    }
    const auto &worldBounds = world.at("bounds");
    if (!worldBounds.is_array() || worldBounds.size() != 2)
        throw std::runtime_error("invalid IW3 reflection-probe world bounds");
    for (std::size_t index = 0; index < plan.reflectionProbes.size(); ++index)
    {
        const auto &probe = plan.reflectionProbes[index];
        const auto sourceIndex = probe.sourceIndex;
        if (sourceIndex >= sourceProbeCount)
            throw std::runtime_error("IW3 render plan references an invalid reflection probe");
        Vec3 minimum{}, maximum{};
        if (sourceIndex == 0)
        {
            minimum = ReadVector<3>(worldBounds.at(0));
            maximum = ReadVector<3>(worldBounds.at(1));
        }
        else if (geometryProbeBounds[sourceIndex].valid)
        {
            minimum = geometryProbeBounds[sourceIndex].minimum;
            maximum = geometryProbeBounds[sourceIndex].maximum;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                minimum[axis] -= 32.0f;
                maximum[axis] += 32.0f;
            }
        }
        else if (cellProbeBounds[sourceIndex].valid)
        {
            minimum = cellProbeBounds[sourceIndex].minimum;
            maximum = cellProbeBounds[sourceIndex].maximum;
        }
        else
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                minimum[axis] = probe.origin[axis] - 128.0f;
                maximum[axis] = probe.origin[axis] + 128.0f;
            }
        }
        output["reflectionProbes"].push_back({{"origin", probe.origin},
                                              {"volume", {minimum, maximum}},
                                              {"image", probe.image},
                                              {"sh", probe.sh}});
    }
    output["glassPanes"] = BuildGlassPanes(models, plan, entities, collision, glassModels,
                                          glassTargets, entityModels);
    output["assetMaterials"] = plan.assetMaterials;
    output["stockMaterials"] = plan.stockMaterials;
    return output;
}

std::vector<Vec3> HullFromPlanes(const std::vector<Vec4> &source)
{
    if (source.size() < 4 || source.size() > 64)
    {
        throw std::runtime_error("IW3 collision brush has an invalid plane count");
    }
    std::vector<Vec4> planes;
    for (const Vec4 &plane : source)
    {
        const float length =
            std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        if (length < 1.0e-8f)
        {
            throw std::runtime_error("IW3 collision contains a zero plane");
        }
        const Vec4 normalized{plane[0] / length, plane[1] / length, plane[2] / length,
                              plane[3] / length};
        const bool duplicate = std::any_of(planes.begin(), planes.end(), [&](const Vec4 &other) {
            return std::abs(normalized[0] - other[0]) < 1.0e-8f &&
                   std::abs(normalized[1] - other[1]) < 1.0e-8f &&
                   std::abs(normalized[2] - other[2]) < 1.0e-8f &&
                   std::abs(normalized[3] - other[3]) < 1.0e-6f;
        });
        if (!duplicate)
        {
            planes.push_back(normalized);
        }
    }

    std::array<std::array<float, 2>, 3> bounds{};
    for (auto &axis : bounds)
    {
        axis = {-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    }
    bool axial = true;
    for (const Vec4 &plane : planes)
    {
        int component = -1;
        for (int index = 0; index < 3; ++index)
        {
            if (std::abs(plane[index]) > 1.0e-8f)
            {
                if (component >= 0)
                {
                    axial = false;
                    break;
                }
                component = index;
            }
        }
        if (!axial || component < 0)
        {
            axial = false;
            break;
        }
        if (plane[component] > 0.0f)
            bounds[component][1] = std::min(bounds[component][1], plane[3] / plane[component]);
        else
            bounds[component][0] = std::max(bounds[component][0], plane[3] / plane[component]);
    }
    if (axial)
    {
        if (std::any_of(bounds.begin(), bounds.end(), [](const auto &axis) {
                return !std::isfinite(axis[0]) || !std::isfinite(axis[1]) || axis[0] >= axis[1];
            }))
        {
            throw std::runtime_error("empty or unbounded axial IW3 collision brush");
        }
        std::vector<Vec3> points;
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z)
                    points.push_back({bounds[0][x], bounds[1][y], bounds[2][z]});
        return points;
    }

    std::vector<Vec3> points;
    for (std::size_t a = 0; a < planes.size(); ++a)
        for (std::size_t b = a + 1; b < planes.size(); ++b)
            for (std::size_t c = b + 1; c < planes.size(); ++c)
            {
                const Vec3 na{planes[a][0], planes[a][1], planes[a][2]};
                const Vec3 nb{planes[b][0], planes[b][1], planes[b][2]};
                const Vec3 nc{planes[c][0], planes[c][1], planes[c][2]};
                const Vec3 bc = Cross(nb, nc);
                const float determinant = Dot(na, bc);
                if (std::abs(determinant) < 1.0e-9f)
                    continue;
                Vec3 point =
                    Add(Add(Multiply(bc, planes[a][3]), Multiply(Cross(nc, na), planes[b][3])),
                        Multiply(Cross(na, nb), planes[c][3]));
                point = Multiply(point, 1.0f / determinant);
                const bool inside = std::all_of(planes.begin(), planes.end(), [&](const Vec4 &p) {
                    return p[0] * point[0] + p[1] * point[1] + p[2] * point[2] <= p[3] + 0.002f;
                });
                const bool duplicate =
                    std::any_of(points.begin(), points.end(), [&](const Vec3 &p) {
                        const Vec3 delta = Subtract(point, p);
                        return Dot(delta, delta) < 1.0e-6f;
                    });
                if (inside && !duplicate)
                    points.push_back(point);
            }
    // A brush whose planes leave no volume (mp_arnhem has one: two planes that
    // cut its whole box away) collides with nothing; callers skip it.
    if (points.size() < 4)
        return {};
    if (points.size() > 252)
        throw std::runtime_error("IW3 collision brush exceeds 252 vertices");
    return points;
}

void CollectLeafBrushes(const Json &nodes, const std::size_t index,
                        std::vector<std::uint32_t> &brushes, std::vector<std::uint8_t> &visited)
{
    if (index >= nodes.size())
        throw std::runtime_error("IW3 collision leaf-brush child is outside its array");
    if (visited[index])
        throw std::runtime_error("IW3 collision leaf-brush tree contains a cycle");
    visited[index] = 1;

    const auto &node = nodes.at(index);
    const int count = node.at("count").get<int>();
    if (count > 0)
    {
        const auto &indices = node.at("brushes");
        if (!indices.is_array() || indices.size() != static_cast<std::size_t>(count))
            throw std::runtime_error("IW3 collision leaf-brush count is inconsistent");
        for (const auto &brush : indices)
            brushes.push_back(brush.get<std::uint32_t>());
        visited[index] = 0;
        return;
    }

    const auto &children = node.at("children");
    if (!children.is_array() || children.size() != 2)
        throw std::runtime_error("IW3 collision leaf-brush node has invalid children");

    // IW3 stores child indices relative to the current node. A negative count also
    // owns the immediately following node; zero-count nodes only use the two offsets.
    if (count < 0)
        CollectLeafBrushes(nodes, index + 1, brushes, visited);
    for (const auto &child : children)
    {
        const auto offset = child.get<std::uint32_t>();
        if (offset)
            CollectLeafBrushes(nodes, index + offset, brushes, visited);
    }
    visited[index] = 0;
}

std::uint32_t ConvertContents(const std::uint32_t source)
{
    // 0x2 is foliage in both games (Replay physicscontents_foliage). No player,
    // bullet, melee or physics-body mask includes it; like IW3 it drives the
    // foliage movement and bullet sounds (cg_foliagesnd_alias / EV_FOLIAGE_SOUND,
    // cg_foliagebulletsnd_alias) and AI sight (ai_foliageSeeThroughDist).
    constexpr std::uint32_t replayContents = 0x33683u;
    std::uint32_t result = source & replayContents;
    // IW3 glass brushes participate in native solid traces. 0x40 is IW3's
    // "can shoot through" clip flag (clip_nosight in window frames); treating
    // it as solid stopped bullets before they reached native glass.
    if (source & 0x10u)
        result |= 1u;
    // Non-solid IW3 player clips (clip_nosight stair ramps and window clips
    // carry AI-no-sight 0x1000 and item-clip 0x400) must use the shipped
    // player-clip contents (0x30200 / 0x30000 / 0x10000); with the IW3 extras
    // the player walked through stair ramps and stepped on raw stairs.
    if (!(result & 1u) && (result & 0x10000u))
        result &= 0x30202u;
    return result;
}

unsigned NativeSurfaceMaterial(const std::uint32_t sourceBits)
{
    // IW3 uses bit (surface type - 1); Replay has additional surface families
    // and a different ordering (infoParms table 4618420: 3 carpet_solid,
    // 4 cloth). Return zero for types without a known mapping.
    switch (sourceBits)
    {
    case 1u: return 1;         // bark
    case 2u: return 2;         // brick
    case 4u: return 3;         // carpet
    case 8u: return 4;         // cloth
    case 16u: return 5;        // concrete
    case 32u: return 6;        // dirt
    case 64u: return 7;        // flesh
    case 128u: return 8;       // foliage
    case 256u: return 9;       // glass
    case 512u: return 10;      // grass
    case 1024u: return 11;     // gravel
    case 2048u: return 12;     // ice
    case 4096u: return 13;     // metal
    case 8192u: return 15;     // mud
    case 16384u: return 16;    // paper
    case 32768u: return 17;    // plaster
    case 65536u: return 18;    // rock
    case 131072u: return 19;   // sand
    case 262144u: return 20;   // snow
    case 524288u: return 21;   // water
    case 1048576u: return 22;  // wood
    case 2097152u: return 23;  // asphalt
    case 4194304u: return 24;  // ceramic
    case 8388608u: return 25;  // plastic
    case 16777216u: return 26; // rubber
    case 33554432u: return 35; // cushion
    case 67108864u: return 27; // fruit
    case 134217728u: return 28; // painted metal
    default: return 0;
    }
}

std::vector<CollisionHull::Slab> BuildTriggerSlabs(const std::vector<Vec3> &points,
                                                   const std::vector<Vec4> &planes)
{
    std::vector<CollisionHull::Slab> slabs;
    for (const Vec4 &plane : planes)
    {
        Vec3 direction = Unit({plane[0], plane[1], plane[2]});
        if (std::ranges::any_of(direction, [](const float component) {
                return std::abs(std::abs(component) - 1.0f) < 0.0001f;
            }))
            continue;

        const auto first = std::find_if(direction.begin(), direction.end(), [](const float value) {
            return std::abs(value) > 0.0001f;
        });
        if (first != direction.end() && *first < 0.0f)
            direction = Multiply(direction, -1.0f);

        float minimum = Dot(points.front(), direction);
        float maximum = minimum;
        for (const Vec3 &point : points)
        {
            const float projection = Dot(point, direction);
            minimum = (std::min)(minimum, projection);
            maximum = (std::max)(maximum, projection);
        }
        CollisionHull::Slab slab{direction, (minimum + maximum) * 0.5f, (maximum - minimum) * 0.5f};
        const bool duplicate = std::ranges::any_of(slabs, [&](const CollisionHull::Slab &item) {
            return std::abs(Dot(item.direction, slab.direction) - 1.0f) < 0.0001f &&
                   std::abs(item.midpoint - slab.midpoint) < 0.01f &&
                   std::abs(item.halfSize - slab.halfSize) < 0.01f;
        });
        if (!duplicate)
            slabs.push_back(slab);
    }
    return slabs;
}

std::optional<std::vector<CollisionHull>> SplitSlantedBrushFaces(
    const CollisionHull &hull, const std::vector<Vec4> &planes,
    const std::vector<unsigned> &materials)
{
    if (planes.size() != materials.size())
        throw std::runtime_error("IW3 brush face materials do not match its planes");
    Vec3 center{};
    for (const Vec3 &point : hull.points)
        center = Add(center, point);
    center = Multiply(center, 1.0f / hull.points.size());

    std::vector<CollisionHull> pieces;
    std::vector<std::vector<std::size_t>> faces;
    std::vector<unsigned> faceIncidence(hull.points.size());
    for (std::size_t planeIndex = 0; planeIndex < planes.size(); ++planeIndex)
    {
        const Vec4 &plane = planes[planeIndex];
        const Vec3 normal{plane[0], plane[1], plane[2]};
        const float length = std::sqrt(Dot(normal, normal));
        if (length < 1.0e-6f || plane[3] - Dot(normal, center) < 0.02f * length)
            return std::nullopt;

        std::vector<std::size_t> face;
        // Nearby thin-brush faces must not share reconstructed vertices.
        for (std::size_t index = 0; index < hull.points.size(); ++index)
            if (std::abs(Dot(normal, hull.points[index]) - plane[3]) <= 0.002f * length)
                face.push_back(index);
        if (face.size() < 3)
            continue;
        const Vec3 edge = Subtract(hull.points[face[1]], hull.points[face[0]]);
        const bool planarArea = std::ranges::any_of(face.begin() + 2, face.end(),
                                                    [&](const std::size_t index) {
            return std::abs(Dot(Cross(edge, Subtract(hull.points[index], hull.points[face[0]])),
                                normal)) > 0.01f * length;
        });
        if (!planarArea)
            continue;
        if (std::ranges::find(faces, face) != faces.end())
            return std::nullopt;
        faces.push_back(face);
        for (const std::size_t index : face)
            ++faceIncidence[index];

        CollisionHull piece;
        piece.contents = hull.contents;
        piece.model = hull.model;
        piece.materialOverride = materials[planeIndex];
        piece.points.push_back(center);
        for (const std::size_t index : face)
            piece.points.push_back(hull.points[index]);
        if (piece.points.size() > 252)
            return std::nullopt;
        pieces.push_back(std::move(piece));
    }
    // A closed convex polyhedron has every vertex on at least three faces and
    // V - E + F = 2, with 2E equal to the sum of face-vertex incidences.
    // Reject incomplete face lists instead of leaving a gap in the collision
    // volume when a source plane cannot be matched to the reconstructed hull.
    const std::size_t incidences = std::accumulate(faceIncidence.begin(), faceIncidence.end(),
                                                   std::size_t{});
    if (pieces.size() < 4 ||
        !std::ranges::all_of(faceIncidence, [](const unsigned count) { return count >= 3; }) ||
        incidences != 2 * (hull.points.size() + faces.size() - 2))
        return std::nullopt;
    return pieces;
}

CollisionData ReadCollision(const Json &collision)
{
    const auto &sourceBrushes = collision.at("brushes");
    const auto &sourceModels = collision.at("submodels");
    const auto &nodes = collision.at("leaf_brush_nodes");
    if (!sourceBrushes.is_array() || !sourceModels.is_array() || sourceModels.empty() ||
        !nodes.is_array())
        throw std::runtime_error("IW3 collision is missing brush-model topology");

    std::vector<unsigned> collisionMaterials;
    if (const auto sourceMaterials = collision.find("materials");
        sourceMaterials != collision.end())
    {
        if (!sourceMaterials->is_array())
            throw std::runtime_error("IW3 collision materials are not an array");
        collisionMaterials.reserve(sourceMaterials->size());
        for (const auto &material : *sourceMaterials)
        {
            const auto flags =
                static_cast<std::uint32_t>(material.at("surfaceFlags").get<std::int32_t>());
            const unsigned sourceType = (flags >> 20) & 31u;
            const unsigned nativeType =
                sourceType ? NativeSurfaceMaterial(1u << (sourceType - 1)) : 0;
            collisionMaterials.push_back(sourceType && !nativeType
                                             ? std::numeric_limits<unsigned>::max()
                                             : nativeType);
        }
    }

    CollisionData result;
    result.models.reserve(sourceModels.size());
    std::vector<std::uint32_t> owner(sourceBrushes.size(), 0);
    for (std::size_t modelIndex = 0; modelIndex < sourceModels.size(); ++modelIndex)
    {
        const auto &model = sourceModels.at(modelIndex);
        result.models.push_back({ReadVector<3>(model.at("mins")), ReadVector<3>(model.at("maxs"))});
        if (modelIndex == 0)
            continue;

        std::vector<std::uint32_t> brushes;
        std::vector<std::uint8_t> visited(nodes.size());
        CollectLeafBrushes(nodes, model.at("leaf").get<std::size_t>(), brushes, visited);
        std::ranges::sort(brushes);
        brushes.erase(std::unique(brushes.begin(), brushes.end()), brushes.end());
        for (const auto brush : brushes)
        {
            if (brush >= owner.size())
                throw std::runtime_error("IW3 collision submodel references an invalid brush");
            if (owner[brush] && owner[brush] != modelIndex)
                throw std::runtime_error("IW3 collision brush belongs to multiple submodels");
            owner[brush] = static_cast<std::uint32_t>(modelIndex);
        }
    }

    result.hulls.reserve(sourceBrushes.size() + collision.at("triangles").size());
    std::size_t sourceTaggedBrushes = 0;
    std::size_t mixedMaterialBrushes = 0;
    std::size_t splitMixedBoxes = 0;
    std::size_t emptyBrushes = 0;
    std::size_t splitMixedSlanted = 0;
    for (std::size_t brushIndex = 0; brushIndex < sourceBrushes.size(); ++brushIndex)
    {
        const auto &brush = sourceBrushes.at(brushIndex);
        const std::uint32_t contents = ConvertContents(brush.at("contents").get<std::uint32_t>());
        if (!contents)
            continue;
        std::vector<Vec4> planes;
        for (const auto &plane : brush.at("planes"))
            planes.push_back(ReadVector<4>(plane));
        const Vec3 minimum = ReadVector<3>(brush.at("mins"));
        const Vec3 maximum = ReadVector<3>(brush.at("maxs"));
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            Vec4 low{};
            low[axis] = -1.0f;
            low[3] = -minimum[axis];
            planes.push_back(low);
            Vec4 high{};
            high[axis] = 1.0f;
            high[3] = maximum[axis];
            planes.push_back(high);
        }
        CollisionHull hull;
        hull.points = HullFromPlanes(planes);
        if (hull.points.empty())
        {
            ++emptyBrushes;
            continue;
        }
        hull.slabs = BuildTriggerSlabs(hull.points, planes);
        hull.contents = contents;
        hull.model = owner.at(brushIndex);
        bool mixedMaterials = false;
        std::array<std::array<unsigned, 3>, 2> axialMaterials{};
        std::vector<unsigned> sideMaterials;
        if (!collisionMaterials.empty())
        {
            const auto &axial = brush.at("axial_materials");
            const auto &sides = brush.at("side_materials");
            if (!axial.is_array() || axial.size() != 2 || !sides.is_array() ||
                sides.size() != brush.at("planes").size())
                throw std::runtime_error("IW3 brush material references are invalid");

            unsigned sourceMaterial = 0;
            const auto consider = [&](const Json &reference) -> unsigned {
                const auto index = reference.get<std::int64_t>();
                if (index < 0)
                    return 0;
                if (static_cast<std::uint64_t>(index) >= collisionMaterials.size())
                    throw std::runtime_error("IW3 brush material index is invalid");
                const unsigned material = collisionMaterials[static_cast<std::size_t>(index)];
                if (material == std::numeric_limits<unsigned>::max())
                    mixedMaterials = true;
                else if (material)
                {
                    if (sourceMaterial && sourceMaterial != material)
                        mixedMaterials = true;
                    sourceMaterial = material;
                }
                return material == std::numeric_limits<unsigned>::max() ? 0 : material;
            };
            for (std::size_t side = 0; side < 2; ++side)
            {
                const auto &row = axial.at(side);
                if (!row.is_array() || row.size() != 3)
                    throw std::runtime_error("IW3 axial brush materials are invalid");
                for (std::size_t axis = 0; axis < 3; ++axis)
                    axialMaterials[side][axis] = consider(row.at(axis));
            }
            sideMaterials.reserve(sides.size());
            for (const auto &reference : sides)
                sideMaterials.push_back(consider(reference));
            if (mixedMaterials)
                ++mixedMaterialBrushes;
            else if (sourceMaterial)
            {
                hull.materialOverride = sourceMaterial;
                ++sourceTaggedBrushes;
            }
        }
        // IW3 records the climbable face separately from the brush planes.
        // Replay carries SURFACE_FLAG_LADDER through the low bits of the
        // Havok shape tag's userData field, so retain it in the native
        // collision intermediate instead of requiring a movement hook.
        if (const auto ladderPlanes = brush.find("ladder_planes");
            ladderPlanes != brush.end() && ladderPlanes->is_array() && !ladderPlanes->empty())
        {
            const std::size_t thinAxis = maximum[0] - minimum[0] < maximum[1] - minimum[1] ? 0 : 1;
            for (const auto &plane : *ladderPlanes)
            {
                const Vec4 value = ReadVector<4>(plane);
                if (std::abs(value[thinAxis]) >= 0.999f && std::abs(value[2]) <= 0.001f)
                    hull.ladderPlanes.push_back(value);
            }
            if (!hull.ladderPlanes.empty())
                hull.surfaceFlags |= 0x8u;
        }
        // Each pyramid has one original box face as its base and the box center
        // as its apex. The six convex pieces tile the same solid volume, while
        // Replay can give their exterior faces distinct native shape tags.
        if (mixedMaterials && hull.model == 0 && brush.at("planes").empty() &&
            !(brush.at("contents").get<std::uint32_t>() & (0x10u | 0x40u)) &&
            hull.ladderPlanes.empty())
        {
            const Vec3 center = Multiply(Add(minimum, maximum), 0.5f);
            for (std::size_t side = 0; side < 2; ++side)
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    CollisionHull face;
                    face.contents = hull.contents;
                    face.model = hull.model;
                    face.materialOverride = axialMaterials[side][axis];
                    face.points.push_back(center);
                    for (unsigned corner = 0; corner < 4; ++corner)
                    {
                        Vec3 point = minimum;
                        point[axis] = side ? maximum[axis] : minimum[axis];
                        point[(axis + 1) % 3] = corner & 1u ? maximum[(axis + 1) % 3]
                                                           : minimum[(axis + 1) % 3];
                        point[(axis + 2) % 3] = corner & 2u ? maximum[(axis + 2) % 3]
                                                           : minimum[(axis + 2) % 3];
                        face.points.push_back(point);
                    }
                    result.hulls.push_back(std::move(face));
                }
            ++splitMixedBoxes;
        }
        else if (mixedMaterials && hull.model == 0 && !brush.at("planes").empty() &&
                 !(brush.at("contents").get<std::uint32_t>() & (0x10u | 0x40u)) &&
                 hull.ladderPlanes.empty())
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                sideMaterials.push_back(axialMaterials[0][axis]);
                sideMaterials.push_back(axialMaterials[1][axis]);
            }
            if (auto pieces = SplitSlantedBrushFaces(hull, planes, sideMaterials))
            {
                for (auto &piece : *pieces)
                    result.hulls.push_back(std::move(piece));
                ++splitMixedSlanted;
            }
            else
                result.hulls.push_back(std::move(hull));
        }
        else
            result.hulls.push_back(std::move(hull));
    }
    if (emptyBrushes)
        zt::warn("iw3: left out %zu IW3 collision brush(es) whose planes enclose no volume",
                 emptyBrushes);
    if (!collisionMaterials.empty())
        zt::info("iw3: tagged %zu brush hulls from source materials; split %zu mixed axial and %zu slanted brushes into face-tagged convex pieces; %zu other mixed brushes retain floor-derived tags",
                 sourceTaggedBrushes, splitMixedBoxes, splitMixedSlanted,
                 mixedMaterialBrushes - splitMixedBoxes - splitMixedSlanted);

    const auto &vertices = collision.at("vertices");
    const auto &sourceTriangles = collision.at("triangles");
    const auto sourceTriangleContents = collision.find("triangle_contents");
    if (sourceTriangleContents != collision.end() &&
        (!sourceTriangleContents->is_array() ||
         sourceTriangleContents->size() != sourceTriangles.size() ||
         collision.value("triangle_contents_schema", std::string{}) !=
             "material-partitions-v1"))
        throw std::runtime_error("invalid IW3 collision triangle contents");
    std::size_t unsupportedTriangleContents = 0;
    std::size_t unreferencedTriangles = 0;
    // Terrain and patch collision stays a triangle mesh, as in shipped maps. Thin
    // convex slabs per triangle kept player traces working, but simulated bodies
    // (ragdolls, dropped weapons) tunnelled through the quarter-unit walls.
    CollisionMesh terrain;
    std::unordered_map<std::size_t, std::uint32_t> terrainVertices;
    for (std::size_t triangleIndex = 0; triangleIndex < sourceTriangles.size(); ++triangleIndex)
    {
        std::uint32_t contents = 1;
        if (sourceTriangleContents != collision.end())
        {
            const auto &sourceContents = sourceTriangleContents->at(triangleIndex);
            if (sourceContents.is_null())
            {
                ++unreferencedTriangles;
                continue;
            }
            contents = ConvertContents(sourceContents.get<std::uint32_t>());
            if (!contents)
            {
                ++unsupportedTriangleContents;
                continue;
            }
        }
        const auto &triangle = sourceTriangles.at(triangleIndex);
        if (!triangle.is_array() || triangle.size() != 3)
        {
            throw std::runtime_error("invalid IW3 collision triangle");
        }
        std::array<Vec3, 3> points{};
        std::array<std::size_t, 3> sourceIndices{};
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
            const auto index = triangle.at(corner).get<std::size_t>();
            if (index >= vertices.size())
            {
                throw std::runtime_error(
                    "IW3 collision triangle index is outside its vertex array");
            }
            sourceIndices[corner] = index;
            points[corner] = ReadVector<3>(vertices.at(index));
        }
        const Vec3 cross = Cross(Subtract(points[1], points[0]), Subtract(points[2], points[0]));
        if (Dot(cross, cross) < 1.0e-12f)
            continue;
        CollisionMesh::Triangle meshTriangle;
        meshTriangle.contents = contents;
        // Zero asks the baker for the rendered floor material, like terrain hulls.
        meshTriangle.material = 0;
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
            const auto [found, inserted] = terrainVertices.try_emplace(
                sourceIndices[corner], static_cast<std::uint32_t>(terrain.vertices.size()));
            if (inserted)
                terrain.vertices.push_back(points[corner]);
            meshTriangle.indices[corner] = found->second;
        }
        // IW3 winds collision triangles clockwise seen from the open side (6006
        // of Nuketown's 7652 face down by the right-hand rule). Havok takes the
        // counter-clockwise face as the front: contact welding pushed ragdolls
        // and dropped weapons through every terrain floor, while two-sided ray
        // traces (players, bullets) still hit. Reverse to Havok's convention.
        std::swap(meshTriangle.indices[1], meshTriangle.indices[2]);
        terrain.triangles.push_back(meshTriangle);
    }
    if (!terrain.triangles.empty())
    {
        zt::info("iw3: kept %zu source collision triangles as a native world mesh",
                 terrain.triangles.size());
        result.meshes.push_back(std::move(terrain));
    }
    if (unsupportedTriangleContents)
        zt::info("iw3: omitted %zu source collision triangles whose contents have no Replay mapping",
                 unsupportedTriangleContents);
    if (unreferencedTriangles)
        zt::info("iw3: omitted %zu source collision triangles not referenced by a clip-map partition",
                 unreferencedTriangles);
    constexpr std::size_t maximumHulls = 262144;
    if (result.hulls.empty() || result.hulls.size() > maximumHulls)
    {
        throw std::runtime_error("IW3 collision has " + std::to_string(result.hulls.size()) +
                                 " hulls; maximum is " + std::to_string(maximumHulls));
    }
    return result;
}

unsigned FootstepMaterial(std::string material, const RenderPlan &renderPlan);

// Replay smooths the view over every step-up (PMF_SMOOTH_STEP), so raw IW3
// stair treads leave the camera lagging a riser behind the player. Cover each
// detected flight with a player-clip ramp through its step nosings; bullets,
// grenades and rendering keep the real steps.
std::size_t AddStairRamps(CollisionData &collision)
{
    using Point2 = std::array<float, 2>;
    struct Tread
    {
        std::vector<Point2> outline; // convex, counter-clockwise
        Point2 center{};
        Point2 low{}, high{};
        float top{};
        float area{};
    };
    const auto convexHull = [](std::vector<Point2> points) {
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        if (points.size() < 3)
            return std::vector<Point2>{};
        const auto cross = [](const Point2 &o, const Point2 &a, const Point2 &b) {
            return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
        };
        std::vector<Point2> hull(points.size() * 2);
        std::size_t count = 0;
        for (const auto &point : points)
        {
            while (count >= 2 && cross(hull[count - 2], hull[count - 1], point) <= 0)
                --count;
            hull[count++] = point;
        }
        for (std::size_t index = points.size() - 1, lower = count + 1; index-- > 0;)
        {
            while (count >= lower && cross(hull[count - 2], hull[count - 1], points[index]) <= 0)
                --count;
            hull[count++] = points[index];
        }
        hull.resize(count - 1);
        return hull;
    };
    const auto contains = [](const auto &tread, const Point2 &point) {
        const auto &polygon = tread.outline;
        for (std::size_t index = 0; index < polygon.size(); ++index)
        {
            const Point2 &a = polygon[index];
            const Point2 &b = polygon[(index + 1) % polygon.size()];
            if ((b[0] - a[0]) * (point[1] - a[1]) - (b[1] - a[1]) * (point[0] - a[0]) < -1.0e-3f)
                return false;
        }
        return true;
    };
    const auto extent = [](const auto &tread, const Point2 &axis) {
        std::array<float, 2> range{INFINITY, -INFINITY};
        for (const auto &point : tread.outline)
        {
            const float value = point[0] * axis[0] + point[1] * axis[1];
            range[0] = std::min(range[0], value);
            range[1] = std::max(range[1], value);
        }
        return range;
    };

    std::vector<Tread> all;
    for (const auto &hull : collision.hulls)
    {
        if (hull.model != 0 || !(hull.contents & 1u) || hull.glassId || hull.points.size() < 4)
            continue;
        float top = -std::numeric_limits<float>::infinity();
        for (const auto &point : hull.points)
            top = std::max(top, point[2]);
        std::vector<Point2> face;
        for (const auto &point : hull.points)
            if (point[2] > top - 0.05f)
                face.push_back({point[0], point[1]});
        Tread tread;
        tread.top = top;
        tread.outline = convexHull(std::move(face));
        if (tread.outline.size() < 3)
            continue;
        tread.low = tread.outline.front();
        tread.high = tread.outline.front();
        for (std::size_t index = 0; index < tread.outline.size(); ++index)
        {
            const Point2 &a = tread.outline[index];
            const Point2 &b = tread.outline[(index + 1) % tread.outline.size()];
            tread.area += a[0] * b[1] - b[0] * a[1];
            for (std::size_t axis = 0; axis < 2; ++axis)
            {
                tread.center[axis] += a[axis] / static_cast<float>(tread.outline.size());
                tread.low[axis] = std::min(tread.low[axis], a[axis]);
                tread.high[axis] = std::max(tread.high[axis], a[axis]);
            }
        }
        tread.area = std::abs(tread.area) * 0.5f;
        if (tread.area >= 64.0f)
            all.push_back(std::move(tread));
    }
    // A step is often a riser block under a thin cap: keep only exposed tops.
    std::vector<Tread> treads;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        bool covered = false;
        for (std::size_t j = 0; j < all.size() && !covered; ++j)
        {
            const float rise = all[j].top - all[i].top;
            covered = i != j && rise > 0.05f && rise < 4.0f && all[j].area >= 0.5f * all[i].area &&
                      contains(all[j], all[i].center);
        }
        if (!covered)
            treads.push_back(all[i]);
    }
    std::vector<std::size_t> order(treads.size());
    std::iota(order.begin(), order.end(), std::size_t{});
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return treads[a].top < treads[b].top; });

    std::vector<bool> claimed(treads.size());
    std::size_t ramps = 0;
    for (const std::size_t first : order)
    {
        if (claimed[first])
            continue;
        std::vector<std::size_t> chain{first};
        std::optional<Point2> heading;
        float riser = 0, run = 0;
        for (;;)
        {
            const Tread &current = treads[chain.back()];
            std::optional<std::size_t> next;
            float bestScore = INFINITY;
            Point2 nextHeading{};
            float nextRise = 0, nextRun = 0;
            for (const std::size_t candidate : order)
            {
                if (claimed[candidate] ||
                    std::find(chain.begin(), chain.end(), candidate) != chain.end())
                    continue;
                const Tread &tread = treads[candidate];
                const float rise = tread.top - current.top;
                const float dx = tread.center[0] - current.center[0];
                const float dy = tread.center[1] - current.center[1];
                const float distance = std::sqrt(dx * dx + dy * dy);
                if (rise < 4.0f || rise > 18.0f || distance < 4.0f || distance > 40.0f)
                    continue;
                const Point2 direction = heading ? *heading : Point2{dx / distance, dy / distance};
                const Point2 side{-direction[1], direction[0]};
                const auto from = extent(current, direction);
                const auto to = extent(tread, direction);
                const auto fromSide = extent(current, side);
                const auto toSide = extent(tread, side);
                const float advance = to[0] - from[0];
                const float overlap =
                    std::min(fromSide[1], toSide[1]) - std::max(fromSide[0], toSide[0]);
                const float narrower =
                    std::min(fromSide[1] - fromSide[0], toSide[1] - toSide[0]);
                if (advance < 6.0f || advance > 26.0f || overlap < 16.0f ||
                    overlap < 0.5f * narrower)
                    continue;
                if (heading &&
                    (std::abs(rise - riser) > 1.5f || std::abs(advance - run) > 0.4f * run))
                    continue;
                // Take the very next step: lowest riser first, then the best-aligned piece.
                const float score =
                    rise * 100.0f - overlap + std::abs(advance - (heading ? run : advance));
                if (score < bestScore)
                {
                    bestScore = score;
                    next = candidate;
                    nextHeading = direction;
                    nextRise = rise;
                    nextRun = advance;
                }
            }
            if (!next)
                break;
            if (!heading)
            {
                heading = nextHeading;
                riser = nextRise;
                run = nextRun;
            }
            chain.push_back(*next);
        }
        if (chain.size() < 4)
            continue;
        // Both pieces of a capped step belong to this flight.
        for (const std::size_t index : chain)
            for (std::size_t other = 0; other < treads.size(); ++other)
                if (std::abs(treads[other].top - treads[index].top) <= 0.25f &&
                    treads[other].low[0] < treads[index].high[0] &&
                    treads[other].high[0] > treads[index].low[0] &&
                    treads[other].low[1] < treads[index].high[1] &&
                    treads[other].high[1] > treads[index].low[1])
                    claimed[other] = true;

        const Tread &low = treads[chain.front()];
        const Tread &high = treads[chain.back()];
        Point2 direction{high.center[0] - low.center[0], high.center[1] - low.center[1]};
        const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1]);
        if (length < 1.0f)
            continue;
        direction = {direction[0] / length, direction[1] / length};
        const Point2 side{-direction[1], direction[0]};
        float sideLow = -INFINITY, sideHigh = INFINITY;
        for (const std::size_t index : chain)
        {
            const auto range = extent(treads[index], side);
            sideLow = std::max(sideLow, range[0]);
            sideHigh = std::min(sideHigh, range[1]);
        }
        const float firstNose = extent(low, direction)[0];
        float endNose = extent(high, direction)[0];
        const float stepRise = (high.top - low.top) / static_cast<float>(chain.size() - 1);
        const float stepRun = (endNose - firstNose) / static_cast<float>(chain.size() - 1);
        if (!(stepRun > 4.0f) || !(sideHigh - sideLow >= 16.0f) || stepRise > stepRun)
            continue;
        const auto at = [&](const float along, const float across, const float z) {
            return Vec3{direction[0] * along + side[0] * across,
                        direction[1] * along + side[1] * across, z};
        };
        // Continue onto a landing one riser above the last tread.
        float endTop = high.top;
        const Vec3 probe = at(endNose + stepRun * 1.5f, (sideLow + sideHigh) * 0.5f, 0);
        if (std::ranges::any_of(treads, [&](const Tread &tread) {
                return std::abs(tread.top - (high.top + stepRise)) <= 1.5f &&
                       contains(tread, Point2{probe[0], probe[1]});
            }))
        {
            endNose += stepRun;
            endTop += stepRise;
        }
        const float floor = low.top - stepRise;
        const float start = firstNose - stepRun;
        // A slab one riser (+2) deep fills every tread pocket under the ramp
        // surface without closing the space beneath the flight.
        const float depth = stepRise + 2.0f;
        CollisionHull ramp;
        ramp.contents = 0x30200u;
        for (const float across : {sideLow, sideHigh})
        {
            ramp.points.push_back(at(start, across, floor));
            ramp.points.push_back(at(start, across, floor - depth));
            ramp.points.push_back(at(endNose, across, endTop));
            ramp.points.push_back(at(endNose, across, endTop - depth));
        }
        collision.hulls.push_back(std::move(ramp));
        ++ramps;
        const Vec3 from = at(start, sideLow, floor);
        const Vec3 to = at(endNose, sideLow, endTop);
        zt::info("iw3: stair ramp %zu: %zu treads, riser %.2f, run %.2f, (%.1f %.1f %.1f) to "
                 "(%.1f %.1f %.1f), width %.1f",
                 ramps, chain.size(), stepRise, stepRun, from[0], from[1], from[2], to[0], to[1],
                 to[2], sideHigh - sideLow);
    }
    return ramps;
}

// Returns the rung spacing of the map's ladder models (0 when none were
// measured). Replay climbs in rungs of 12 by default; the DW client applies a
// map's own spacing from map.json (ladderRungDistance) so hands meet the
// original IW3 rungs (Discovery's p_rus_ladder_metal: 16, owner 2026-10-15).
float AlignLadderEdgesToModels(CollisionData &collision, const SourceStaticModels &models,
                               const RenderPlan &renderPlan)
{
    struct LadderModel
    {
        std::string name;
        std::uint32_t material{};
        Vec3 minimum{INFINITY, INFINITY, INFINITY};
        Vec3 maximum{-INFINITY, -INFINITY, -INFINITY};
        std::vector<Vec3> vertices;
        float rungPhase{};
        float rungSpacing{12.0f};
        bool hasRungPhase{};
    };
    std::vector<LadderModel> ladders;
    for (const SourceStaticModelInstance &instance : models.instances)
    {
        const SourceStaticModel &model = models.models.at(instance.model);
        std::string name = model.name;
        std::ranges::transform(name, name.begin(), [](const unsigned char letter) {
            return static_cast<char>(std::tolower(letter));
        });
        if (name.find("ladder") == std::string::npos || model.lods.empty())
            continue;

        LadderModel candidate;
        candidate.name = model.name;
        if (!model.lods.front().surfaces.empty())
        {
            candidate.material =
                FootstepMaterial(model.lods.front().surfaces.front().material, renderPlan);
            for (const Surface &surface : model.lods.front().surfaces)
                if (FootstepMaterial(surface.material, renderPlan) != candidate.material)
                {
                    candidate.material = 0;
                    break;
                }
        }
        std::vector<float> rungHeights;
        for (const Surface &surface : model.lods.front().surfaces)
        {
            std::vector<Vec3> worldVertices;
            worldVertices.reserve(surface.vertices.size());
            for (const Vertex &vertex : surface.vertices)
            {
                const Vec3 position = Add(
                    instance.origin,
                    Multiply(Transform(instance.axis, vertex.position), instance.scale));
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    candidate.minimum[axis] = (std::min)(candidate.minimum[axis], position[axis]);
                    candidate.maximum[axis] = (std::max)(candidate.maximum[axis], position[axis]);
                }
                candidate.vertices.push_back(position);
                worldVertices.push_back(position);
            }
            for (std::size_t first = 0; first + 2 < surface.indices.size(); first += 3)
            {
                const Vec3 &a = worldVertices.at(surface.indices[first]);
                const Vec3 &b = worldVertices.at(surface.indices[first + 1]);
                const Vec3 &c = worldVertices.at(surface.indices[first + 2]);
                const float low = (std::min)({a[2], b[2], c[2]});
                const float high = (std::max)({a[2], b[2], c[2]});
                const auto broad = [](const Vec3 &p, const Vec3 &q) {
                    const float dx = p[0] - q[0], dy = p[1] - q[1];
                    return dx * dx + dy * dy;
                };
                if (high - low > 4.0f ||
                    (std::max)({broad(a, b), broad(b, c), broad(c, a)}) < 144.0f)
                    continue;
                rungHeights.push_back((a[2] + b[2] + c[2]) / 3.0f);
            }
        }
        // Rung spacing: the most common gap between consecutive rung heights.
        // com_ladder_wood keeps 12: its 24-unit bands get midpoint rungs
        // (AddLadderMidpointRungs).
        {
            std::vector<float> levels = rungHeights;
            std::ranges::sort(levels);
            std::vector<float> rungs;
            for (const float level : levels)
                if (rungs.empty() || level - rungs.back() > 2.0f)
                    rungs.push_back(level);
            std::map<int, unsigned> gaps;
            for (std::size_t index = 1; index < rungs.size(); ++index)
                ++gaps[static_cast<int>(std::lround((rungs[index] - rungs[index - 1]) * 2.0f))];
            const auto common = std::ranges::max_element(
                gaps, {}, [](const auto &entry) { return entry.second; });
            if (common != gaps.end() && common->second >= 2 && common->first >= 16 &&
                common->first <= 64)
                candidate.rungSpacing = common->first * 0.5f;
            std::string lower = model.name;
            std::ranges::transform(lower, lower.begin(), [](const unsigned char letter) {
                return static_cast<char>(std::tolower(letter));
            });
            if (lower == "com_ladder_wood")
                candidate.rungSpacing = 12.0f;
        }
        double phaseCos = 0.0, phaseSin = 0.0;
        for (const float height : rungHeights)
        {
            const double angle = height * (2.0 * 3.141592653589793 / candidate.rungSpacing);
            phaseCos += std::cos(angle);
            phaseSin += std::sin(angle);
        }
        const std::size_t rungTriangles = rungHeights.size();
        if (rungTriangles >= 16 &&
            std::hypot(phaseCos, phaseSin) / rungTriangles >= 0.75)
        {
            candidate.rungPhase = static_cast<float>(
                std::atan2(phaseSin, phaseCos) *
                (candidate.rungSpacing / (2.0 * 3.141592653589793)));
            const float halfUnit = std::round(candidate.rungPhase * 2.0f) * 0.5f;
            if (std::abs(candidate.rungPhase - halfUnit) < 0.15f)
                candidate.rungPhase = halfUnit;
            candidate.hasRungPhase = true;
        }
        if (!candidate.vertices.empty())
            ladders.push_back(std::move(candidate));
    }

    std::size_t aligned = 0;
    std::map<float, unsigned> spacings;
    for (CollisionHull &hull : collision.hulls)
    {
        if (hull.ladderPlanes.empty() || hull.points.empty())
            continue;
        Vec3 minimum{INFINITY, INFINITY, INFINITY};
        Vec3 maximum{-INFINITY, -INFINITY, -INFINITY};
        for (const Vec3 &point : hull.points)
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                minimum[axis] = (std::min)(minimum[axis], point[axis]);
                maximum[axis] = (std::max)(maximum[axis], point[axis]);
            }
        const Vec3 center{(minimum[0] + maximum[0]) * 0.5f,
                          (minimum[1] + maximum[1]) * 0.5f,
                          (minimum[2] + maximum[2]) * 0.5f};

        const LadderModel *best = nullptr;
        float bestScore = INFINITY;
        for (const LadderModel &model : ladders)
        {
            const float overlapZ = (std::min)(maximum[2], model.maximum[2]) -
                                   (std::max)(minimum[2], model.minimum[2]);
            if (overlapZ < 12.0f)
                continue;
            const float modelX = (model.minimum[0] + model.maximum[0]) * 0.5f;
            const float modelY = (model.minimum[1] + model.maximum[1]) * 0.5f;
            const float modelZ = (model.minimum[2] + model.maximum[2]) * 0.5f;
            const float dx = modelX - center[0], dy = modelY - center[1];
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > 24.0f * 24.0f)
                continue;
            const float dz = modelZ - center[2];
            const float score = distanceSquared + dz * dz * 0.02f;
            if (score < bestScore)
            {
                bestScore = score;
                best = &model;
            }
        }
        if (!best)
            continue;

        // The IW3 brush marks the climbable volume, which can be wider and
        // deeper than its rendered ladder. Replay IK uses the edge centerline
        // as its hand anchor. Keep the collision hull and normal unchanged;
        // move only the serialized edge plane to the visible model surface.
        std::vector<float> adjusted;
        std::vector<std::array<float, 2>> modelTangents;
        adjusted.reserve(hull.ladderPlanes.size());
        modelTangents.reserve(hull.ladderPlanes.size());
        bool valid = true;
        for (const Vec4 &plane : hull.ladderPlanes)
        {
            float modelFace = -INFINITY;
            float modelTangentMin = INFINITY;
            float modelTangentMax = -INFINITY;
            float hullTangentMin = INFINITY;
            float hullTangentMax = -INFINITY;
            const float tangentX = -plane[1], tangentY = plane[0];
            for (const Vec3 &position : best->vertices)
            {
                modelFace = (std::max)(modelFace, plane[0] * position[0] +
                                                     plane[1] * position[1] +
                                                     plane[2] * position[2]);
                const float tangent = tangentX * position[0] + tangentY * position[1];
                modelTangentMin = (std::min)(modelTangentMin, tangent);
                modelTangentMax = (std::max)(modelTangentMax, tangent);
            }
            for (const Vec3 &position : hull.points)
            {
                const float tangent = tangentX * position[0] + tangentY * position[1];
                hullTangentMin = (std::min)(hullTangentMin, tangent);
                hullTangentMax = (std::max)(hullTangentMax, tangent);
            }
            const float inset = plane[3] - modelFace;
            const float width = modelTangentMax - modelTangentMin;
            if (!std::isfinite(modelFace) || inset < -2.0f || inset > 16.0f ||
                width < 12.0f || modelTangentMin < hullTangentMin - 2.0f ||
                modelTangentMax > hullTangentMax + 2.0f)
            {
                valid = false;
                break;
            }
            adjusted.push_back(modelFace);
            modelTangents.push_back({(modelTangentMin + modelTangentMax) * 0.5f, width});
        }
        if (!valid)
            continue;
        // The player stands against this hull, and Replay's climb IK reaches
        // a fixed SuitDef distance (+0x1AC) toward the edge on the model face.
        // An IW3 ladder brush that stands proud of its model kept the player
        // too far back: the hands hung in front of the rungs (Discovery, owner
        // test 2026-10-16). Pull the climbable side back onto the model face,
        // as stock ladders' own collision is.
        float largestInset = 0.0f;
        for (std::size_t index = 0; index < adjusted.size(); ++index)
        {
            const Vec4 &plane = hull.ladderPlanes[index];
            float back = INFINITY;
            for (const Vec3 &point : hull.points)
                back = (std::min)(back, plane[0] * point[0] + plane[1] * point[1] +
                                            plane[2] * point[2]);
            // Keep at least 2 units of hull behind the face.
            const float face = (std::max)(adjusted[index], back + 2.0f);
            for (Vec3 &point : hull.points)
            {
                const float beyond =
                    plane[0] * point[0] + plane[1] * point[1] + plane[2] * point[2] - face;
                if (beyond <= 0.0f)
                    continue;
                for (std::size_t axis = 0; axis < 3; ++axis)
                    point[axis] -= plane[axis] * beyond;
            }
            largestInset = (std::max)(largestInset, plane[3] - face);
        }
        for (std::size_t index = 0; index < adjusted.size(); ++index)
            hull.ladderPlanes[index][3] = adjusted[index];
        hull.ladderModelTangents = std::move(modelTangents);
        hull.materialOverride = best->material;
        if (best->hasRungPhase)
        {
            hull.ladderRungOffset =
                std::remainder(best->rungPhase + 2.0f - minimum[2], best->rungSpacing);
            ++spacings[best->rungSpacing];
        }
        ++aligned;
        zt::info("iw3: aligned ladder edge to %s at (%.1f %.1f %.1f), rung phase offset %.1f, "
                 "hull pulled back %.1f to the model face",
                 best->name.c_str(), center[0], center[1], center[2], hull.ladderRungOffset,
                 largestInset);
    }
    if (aligned)
        zt::info("iw3: aligned %zu ladder edge pairs to rendered models", aligned);
    if (spacings.empty())
        return 0.0f;
    const auto common = std::ranges::max_element(
        spacings, {}, [](const auto &entry) { return entry.second; });
    zt::info("iw3: ladder rung spacing %.1f units (%zu spacing value(s) among aligned ladders)",
             common->first, spacings.size());
    return common->first;
}

void WriteCollision(const std::filesystem::path &path, const CollisionData &collision)
{
    std::vector<std::uint8_t> output{'M', 'W', 'C', 'O', 'L', 'L', '1', '2'};
    const auto append = [&](const auto &value) {
        const std::size_t offset = output.size();
        output.resize(offset + sizeof(value));
        std::memcpy(output.data() + offset, &value, sizeof(value));
    };
    append(static_cast<std::uint32_t>(collision.hulls.size()));
    append(static_cast<std::uint32_t>(collision.models.size()));
    append(static_cast<std::uint32_t>(collision.meshes.size()));
    for (const auto &model : collision.models)
    {
        for (const auto value : model.minimum)
            append(value);
        for (const auto value : model.maximum)
            append(value);
    }
    for (const auto &hull : collision.hulls)
    {
        append(static_cast<std::uint32_t>(hull.points.size()));
        append(hull.contents);
        append(hull.model);
        append(static_cast<std::uint32_t>(hull.slabs.size()));
        append(hull.surfaceFlags);
        append(static_cast<std::uint32_t>(hull.ladderPlanes.size()));
        append(hull.glassId);
        append(hull.materialOverride);
        append(hull.ladderRungOffset);
        for (const Vec3 &point : hull.points)
        {
            for (const float value : point)
            {
                if (!std::isfinite(value) || std::abs(value) > 1000000.0f)
                {
                    throw std::runtime_error("IW3 collision vertex exceeds Replay bounds");
                }
                append(value);
            }
        }
        for (const auto &slab : hull.slabs)
        {
            for (const float value : slab.direction)
                append(value);
            append(slab.midpoint);
            append(slab.halfSize);
        }
        for (const auto &plane : hull.ladderPlanes)
            for (const float value : plane)
                append(value);
        for (std::size_t index = 0; index < hull.ladderPlanes.size(); ++index)
        {
            const std::array<float, 2> tangent = hull.ladderModelTangents.empty()
                                                     ? std::array<float, 2>{}
                                                     : hull.ladderModelTangents.at(index);
            append(tangent[0]);
            append(tangent[1]);
        }
    }
    for (const auto &mesh : collision.meshes)
    {
        append(static_cast<std::uint32_t>(mesh.vertices.size()));
        append(static_cast<std::uint32_t>(mesh.triangles.size()));
        append(mesh.model);
        for (const Vec3 &point : mesh.vertices)
            for (const float value : point)
                append(value);
        for (const auto &triangle : mesh.triangles)
        {
            for (const auto index : triangle.indices)
                append(index);
            append(triangle.contents);
            append(triangle.surfaceFlags);
            append(triangle.material);
        }
    }
    if (!zt::write_file(path.string(), output))
    {
        throw std::runtime_error("cannot write temporary Replay collision");
    }
}

std::string EntityValue(const std::string &entities, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyStart = entities.find(needle);
    if (keyStart == std::string::npos)
        return {};
    const std::size_t valueStart = entities.find('"', keyStart + needle.size());
    if (valueStart == std::string::npos)
        return {};
    const std::size_t valueEnd = entities.find('"', valueStart + 1);
    if (valueEnd == std::string::npos)
        return {};
    return entities.substr(valueStart + 1, valueEnd - valueStart - 1);
}

Vec3 ParseEntityVector(const std::string &value, const Vec3 &fallback)
{
    if (value.empty())
        return fallback;
    std::istringstream input(value);
    Vec3 result{};
    if (!(input >> result[0] >> result[1] >> result[2]) ||
        std::any_of(result.begin(), result.end(), [](float item) { return !std::isfinite(item); }))
    {
        throw std::runtime_error("invalid IW3 worldspawn vector");
    }
    return result;
}

float ParseEntityFloat(const std::string &value, const float fallback)
{
    if (value.empty())
        return fallback;
    std::size_t consumed = 0;
    const float result = std::stof(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result) || result < 0.0f)
    {
        throw std::runtime_error("invalid IW3 worldspawn sunlight");
    }
    return result;
}

// The worldspawn sundirection (else the exported sun angles) as a unit vector
// toward the sun.
Vec3 SunDirection(const Json &world, const std::string &entities)
{
    const Vec3 exportedAngles = ReadVector<3>(world.at("sun").at("angles"));
    const Vec3 angles = ParseEntityVector(EntityValue(entities, "sundirection"), exportedAngles);
    constexpr float degrees = 3.14159265358979323846f / 180.0f;
    const float pitch = angles[0] * degrees;
    const float yaw = angles[1] * degrees;
    return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
}

Json BuildLighting(const Json &world, const Json &commonWorld, const std::string &entities)
{
    const Json &sun = world.at("sun");
    const Vec3 direction = SunDirection(world, entities);

    Vec3 fallbackColor = ReadVector<3>(sun.at("color"));
    if (*std::max_element(fallbackColor.begin(), fallbackColor.end()) <= 0.0f)
    {
        fallbackColor = ReadVector<3>(sun.at("ambient"));
        const float peak = *std::max_element(fallbackColor.begin(), fallbackColor.end());
        if (peak > 0.0f)
            fallbackColor = Multiply(fallbackColor, 1.0f / peak);
    }
    const Vec3 color = ParseEntityVector(EntityValue(entities, "suncolor"), fallbackColor);
    if (std::any_of(color.begin(), color.end(), [](float value) { return value < 0.0f; }))
    {
        throw std::runtime_error("invalid IW3 worldspawn sun color");
    }
    const float intensity =
        ParseEntityFloat(EntityValue(entities, "sunlight"), sun.at("intensity").get<float>());
    return {{"schema", 1},
            {"intensity", intensity},
            {"color", color},
            {"direction", Unit(direction)},
            {"up", {0.0f, 0.0f, 0.0f}},
            {"sun_primary_light_index", world.at("sun_primary_light_index")},
            {"primary_lights", commonWorld.at("primary_lights")}};
}

void CopyCompass(const std::filesystem::path &extracted, const std::filesystem::path &prepared,
                 const std::string &sourceMap, const std::string &targetMap)
{
    const std::string sourceStem = "compass_map_" + sourceMap;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(extracted, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        std::string extension = iterator->path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (!iterator->is_regular_file(error) || (extension != ".dds" && extension != ".iwi") ||
            _stricmp(iterator->path().stem().string().c_str(), sourceStem.c_str()) != 0)
            continue;
        const auto destination = prepared / "images" / ("compass_map_" + targetMap + extension);
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(iterator->path(), destination,
                                   std::filesystem::copy_options::overwrite_existing);
        zt::info("iw3: included HUD minimap %s", destination.filename().string().c_str());
        return;
    }
}

unsigned FootstepMaterial(std::string material, const RenderPlan &renderPlan)
{
    const auto source = renderPlan.materials.find(material);
    if (source != renderPlan.materials.end())
    {
        if (const unsigned nativeType = NativeSurfaceMaterial(source->second.sourceSurfaceTypeBits))
            return nativeType;
    }
    std::ranges::transform(material, material.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    const auto contains = [&material](const std::string_view token) {
        return material.find(token) != std::string::npos;
    };
    // Replay's movement sound picker uses these native surface-type values.
    // Keep PM_Concrete (5) as the safe fallback for unclassified geometry.
    if (contains("carpet") || contains("rug") || contains("fabric"))
        return 3;
    if (contains("wood") || contains("plank") || contains("timber"))
        return 22;
    if (contains("metal") || contains("steel") || contains("iron") || contains("diamond"))
        return 13;
    if (contains("grass"))
        return 10;
    if (contains("foliage"))
        return 8;
    if (contains("dirt"))
        return 6;
    if (contains("mud"))
        return 15;
    if (contains("sand"))
        return 19;
    if (contains("gravel"))
        return 11;
    if (contains("rock") || contains("stone"))
        return 18;
    if (contains("snow"))
        return 20;
    if (contains("ice"))
        return 12;
    if (contains("glass"))
        return 9;
    if (contains("brick"))
        return 2;
    if (contains("plaster"))
        return 17;
    if (contains("paper"))
        return 16;
    if (contains("tile"))
        return 24;
    if (contains("plastic"))
        return 25;
    if (contains("rubber"))
        return 26;
    if (contains("asphalt"))
        return 23;
    return 5;
}

void WriteFootsteps(const std::filesystem::path &path, const std::vector<BrushModel> &models,
                    const RenderPlan &renderPlan)
{
    // MWRSTEP1 stores world-space triangles. Only upward-facing faces are
    // walkable; walls, ceilings, and degenerate faces are excluded.
    std::vector<std::array<float, 10>> triangles;
    if (!models.empty())
    {
        for (const auto &surface : models.front().surfaces)
        {
            const unsigned material = FootstepMaterial(surface.material, renderPlan);
            for (std::size_t offset = 0; offset < surface.indices.size(); offset += 3)
            {
                const auto &a = surface.vertices.at(surface.indices[offset]);
                const auto &b = surface.vertices.at(surface.indices[offset + 1]);
                const auto &c = surface.vertices.at(surface.indices[offset + 2]);
                const Vec3 normal =
                    Cross(Subtract(b.position, a.position), Subtract(c.position, a.position));
                const float length = std::sqrt(Dot(normal, normal));
                if (!std::isfinite(length) || length < 1.0e-5f || normal[2] / length < 0.55f)
                    continue;
                std::array<float, 10> triangle{a.position[0], a.position[1],
                                               a.position[2], b.position[0],
                                               b.position[1], b.position[2],
                                               c.position[0], c.position[1],
                                               c.position[2], static_cast<float>(material)};
                if (std::ranges::any_of(triangle, [](const float value) {
                        return !std::isfinite(value) || std::abs(value) > 262144.0f;
                    }))
                    throw std::runtime_error("IW3 footstep triangle contains invalid coordinates");
                triangles.push_back(triangle);
                if (triangles.size() > 600000)
                    throw std::runtime_error("IW3 walkable surface count exceeds Replay limits");
            }
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot write temporary IW3 footsteps");
    output.write("MWRSTEP1", 8);
    const auto count = static_cast<std::uint32_t>(triangles.size());
    output.write(reinterpret_cast<const char *>(&count), sizeof(count));
    for (const auto &triangle : triangles)
    {
        output.write(reinterpret_cast<const char *>(triangle.data()), 9 * sizeof(float));
        const auto material = static_cast<std::uint32_t>(triangle[9]);
        output.write(reinterpret_cast<const char *>(&material), sizeof(material));
    }
    if (!output)
        throw std::runtime_error("cannot finish temporary IW3 footsteps");
    zt::info("iw3: generated %u walkable footstep triangles", count);
}

template <typename Payload>
iw8::vfx::Module VfxValueModule(const iw8_focus::ParticleModuleType type,
                                const Payload &payload)
{
    static_assert(sizeof(Payload) <= 0xE0);
    iw8::vfx::Module module;
    module.native.moduleType = static_cast<std::uint16_t>(type);
    std::memcpy(module.native.moduleData, &payload, sizeof(payload));
    return module;
}

iw8_focus::ParticleFloatRange VfxRange(const PreparedFxFloatRange &source,
                                       const float scale = 1.0f)
{
    const float first = source.base * scale;
    const float second = (source.base + source.amplitude) * scale;
    return {std::min(first, second), std::max(first, second)};
}

iw8_focus::ParticleIntRange VfxRange(const PreparedFxIntRange &source)
{
    const std::int32_t second = source.base + source.amplitude;
    return {std::min(source.base, second), std::max(source.base, second)};
}

iw8::vfx::Module::Curve VfxCurve(const std::vector<float> &values, const float scale)
{
    iw8::vfx::Module::Curve curve;
    curve.points.reserve(values.size());
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const float time = values.size() <= 1
                               ? 0.0f
                               : static_cast<float>(index) /
                                     static_cast<float>(values.size() - 1);
        const float previous = index == 0
                                   ? 0.0f
                                   : static_cast<float>(index - 1) /
                                         static_cast<float>(values.size() - 1);
        curve.points.push_back(
            {time, scale == 0.0f ? values[index] : values[index] / scale,
             index == 0 ? 0.0f : 1.0f / (time - previous), 0});
    }
    return curve;
}

float VfxCurveScale(const std::vector<float> &first, const std::vector<float> &second,
                    const float fallback = 1.0f)
{
    float scale = 0.0f;
    for (const float value : first)
        scale = std::max(scale, std::abs(value));
    for (const float value : second)
        scale = std::max(scale, std::abs(value));
    return scale == 0.0f ? fallback : scale;
}

std::string VfxMaterial(const PreparedMap &map, const std::string &name)
{
    const auto material = map.fxMaterialAliases.find(name);
    if (material == map.fxMaterialAliases.end())
        throw std::runtime_error("IW3 VFX material has no converted Replay asset: " + name);
    return material->second;
}

void VfxSetVec(iw8_focus::vec4_t &target, const float x, const float y, const float z,
               const float w = 0.0f)
{
    target.v[0] = x;
    target.v[1] = y;
    target.v[2] = z;
    target.v[3] = w;
}

iw8::vfx::Module VfxSpawnModule()
{
    iw8_focus::ParticleModuleInitSpawn payload{};
    payload.base.type = static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initSpawn);
    payload.curve.scale = 1.0f;
    auto module = VfxValueModule(iw8_focus::ParticleModuleType::initSpawn, payload);
    module.spawnCurve = {{0.0f, 1.0f, 0.0f, 0}, {1.0f, 1.0f, 1.0f, 0}};
    return module;
}

iw8::vfx::Module VfxAttributesModule()
{
    iw8_focus::ParticleModuleInitAttributes payload{};
    payload.base.type =
        static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initAttributes);
    VfxSetVec(payload.sizeMin, 1.0f, 1.0f, 1.0f);
    VfxSetVec(payload.sizeMax, 1.0f, 1.0f, 1.0f);
    VfxSetVec(payload.colorMin, 1.0f, 1.0f, 1.0f, 1.0f);
    VfxSetVec(payload.colorMax, 1.0f, 1.0f, 1.0f, 1.0f);
    return VfxValueModule(iw8_focus::ParticleModuleType::initAttributes, payload);
}

void AddVfxSpawnShape(const PreparedFxElement &source, const bool impact, iw8::vfx::State &state)
{
    const auto radius = VfxRange(source.spawnOffsetRadius);
    const auto height = VfxRange(source.spawnOffsetHeight);
    const bool hasOffset = radius.min != 0.0f || radius.max != 0.0f ||
                           height.min != 0.0f || height.max != 0.0f ||
                           std::ranges::any_of(source.spawnOrigin, [](const auto &range) {
                               return range.base != 0.0f || range.amplitude != 0.0f;
                           });
    if (!hasOffset && source.type != 6)
        return;

    if (source.type == 6)
    {
        iw8_focus::ParticleModuleInitSpawnShapeBox payload{};
        payload.base.base.type = static_cast<std::uint16_t>(
            iw8_focus::ParticleModuleType::initSpawnShapeBox);
        payload.base.axisFlags = 0x3F;
        payload.base.normalAxis = 2;
        payload.base.spawnType = 0;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const float minimum = source.spawnOrigin[axis].base;
            const float maximum = minimum + source.spawnOrigin[axis].amplitude;
            payload.dimensionsMin.v[axis] = std::min(minimum, maximum);
            payload.dimensionsMax.v[axis] = std::max(minimum, maximum);
            payload.base.offset.v[axis] = (minimum + maximum) * 0.5f;
        }
        auto module = VfxValueModule(iw8_focus::ParticleModuleType::initSpawnShapeBox,
                                     payload);
        module.curves.resize(6);
        for (auto &curve : module.curves)
            curve = VfxCurve({1.0f, 1.0f}, 0.0f);
        state.groups[0].push_back(std::move(module));
        return;
    }

    iw8_focus::ParticleModuleInitSpawnShapeCylinder payload{};
    payload.base.base.type = static_cast<std::uint16_t>(
        iw8_focus::ParticleModuleType::initSpawnShapeCylinder);
    payload.base.axisFlags = 0x3F;
    // IW3's spawn cylinder runs along the effect's X; impact effects are rotated
    // so that X is Replay's Z (ToImpactFrame).
    payload.base.normalAxis = impact ? 2 : 0;
    payload.base.spawnType = 1;
    for (std::size_t axis = 0; axis < 3; ++axis)
        payload.base.offset.v[axis] =
            source.spawnOrigin[axis].base + source.spawnOrigin[axis].amplitude * 0.5f;
    payload.halfHeight = std::max(std::abs(height.min), std::abs(height.max));
    payload.radius = radius;
    const float volume = 6.28318530718f * radius.max * radius.max * payload.halfHeight;
    payload.base.volumeCubeRoot = volume > 0.0f ? std::cbrt(volume) : 0.0f;
    auto module =
        VfxValueModule(iw8_focus::ParticleModuleType::initSpawnShapeCylinder, payload);
    module.curves.resize(5);
    for (auto &curve : module.curves)
        curve = VfxCurve({1.0f, 1.0f}, 0.0f);
    state.groups[0].push_back(std::move(module));
}

void AddVfxVelocity(const PreparedFxElement &source, iw8::vfx::State &state)
{
    if (source.velocitySamples.empty())
        return;
    // IW3 sums two velocity graphs: "local" along the effect's axes at spawn (X is the
    // surface normal of an impact) and "world" along world axes. Replay states may carry
    // both; graph flag 0x80 marks the world one (stock offhand_wm_deployable_cover_panel_1).
    const auto curves = [&](const bool world) {
        std::array<std::vector<float>, 6> values;
        for (const auto &sample : source.velocitySamples)
        {
            const auto &velocity = world ? sample.world.velocity : sample.local.velocity;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                values[axis].push_back(velocity.base[axis] * 1000.0f);
                values[axis + 3].push_back(
                    (velocity.base[axis] + velocity.amplitude[axis]) * 1000.0f);
            }
        }
        return values;
    };
    const auto zero = [](const std::array<std::vector<float>, 6> &values) {
        return std::ranges::all_of(values, [](const auto &curve) {
            return std::ranges::all_of(curve, [](const float value) { return value == 0.0f; });
        });
    };
    auto local = curves(false), world = curves(true);
    if (zero(local) && zero(world))
        return;

    // Replay velocity types (stock enum): 0 local, 1 world, 2 relative to the effect
    // origin (radial), 3-5 the same with bolt info. zt-28 used 2, zt-29 0/3 and
    // zt-30 no module; IW3 impacts sprayed sideways with all three (owner tests
    // 2026-10-14), so the frame mode is not the cause. zt-30's missing module
    // crashed Particle_HandleSuccessfulTest (bad per-particle stream,
    // impacts/large_brick -> brickimpact_em2); zt-29's types stay: 3 (bolt) when
    // IW3 runs the element relative to the effect (run modes 0x80/0xC0; 0x100 is
    // the collision bit), else 0.
    constexpr std::int32_t runMask = 0xC0;
    iw8_focus::ParticleModuleInitRelativeVelocity relative{};
    relative.base.type = static_cast<std::uint16_t>(
        iw8_focus::ParticleModuleType::initRelativeVelocity);
    relative.velocityType = zero(local) ? 1u : (source.flags & runMask) >= 0x80 ? 3u : 0u;
    relative.useBoltInfo = relative.velocityType == 3u;
    state.groups[0].push_back(
        VfxValueModule(iw8_focus::ParticleModuleType::initRelativeVelocity, relative));

    const auto addGraph = [&](const std::array<std::vector<float>, 6> &values, const bool inWorld) {
        iw8_focus::ParticleModuleVelocityGraph payload{};
        payload.base.type =
            static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::velocityGraph);
        if (inWorld)
            payload.base.flags |= 0x80;
        auto module = VfxValueModule(iw8_focus::ParticleModuleType::velocityGraph, payload);
        module.curves.resize(6);
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const float scale = VfxCurveScale(values[axis], values[axis + 3]);
            module.curves[axis] = VfxCurve(values[axis], scale);
            module.curves[axis + 3] = VfxCurve(values[axis + 3], scale);
            iw8_focus::ParticleModuleVelocityGraph stored{};
            std::memcpy(&stored, module.native.moduleData, sizeof(payload));
            stored.curves[axis].scale = scale;
            stored.curves[axis + 3].scale = scale;
            if (values[axis] != values[axis + 3])
                stored.base.flags |= 0x10;
            std::memcpy(module.native.moduleData, &stored, sizeof(stored));
        }
        state.groups[1].push_back(std::move(module));
    };
    if (!zero(local))
        addGraph(local, false);
    if (!zero(world))
        addGraph(world, true);
}

void AddVfxRotation(const PreparedFxElement &source, const bool model,
                    iw8::vfx::State &state)
{
    if (model)
    {
        iw8_focus::ParticleModuleInitRotation3D payload{};
        payload.base.type =
            static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initRotation3D);
        const auto angle = VfxRange(source.initialRotation);
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            payload.rotationAngleMin.v[axis] = angle.min;
            payload.rotationAngleMax.v[axis] = angle.max;
            const auto rate = VfxRange(source.angularVelocity[axis], 1000.0f);
            payload.rotationRateMin.v[axis] = rate.min;
            payload.rotationRateMax.v[axis] = rate.max;
        }
        state.groups[0].push_back(
            VfxValueModule(iw8_focus::ParticleModuleType::initRotation3D, payload));
        return;
    }
    if (source.initialRotation.base == 0.0f && source.initialRotation.amplitude == 0.0f)
        return;
    iw8_focus::ParticleModuleInitRotation payload{};
    payload.base.type =
        static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initRotation);
    payload.base.flags = source.initialRotation.amplitude == 0.0f ? 0u : 1u;
    payload.rotationAngle = VfxRange(source.initialRotation);
    state.groups[0].push_back(
        VfxValueModule(iw8_focus::ParticleModuleType::initRotation, payload));
}

void AddVfxVisualCurves(const PreparedFxElement &source, iw8::vfx::State &state)
{
    if (source.visualSamples.empty())
        return;
    std::array<std::vector<float>, 8> colors;
    std::array<std::vector<float>, 6> sizes;
    for (const auto &sample : source.visualSamples)
    {
        constexpr std::array<std::size_t, 4> sourceColorChannels{2, 1, 0, 3};
        for (std::size_t channel = 0; channel < 4; ++channel)
        {
            const auto sourceChannel = sourceColorChannels[channel];
            colors[channel].push_back(
                static_cast<float>(sample.base.color[sourceChannel]) / 255.0f);
            colors[channel + 4].push_back(
                static_cast<float>(sample.amplitude.color[sourceChannel]) / 255.0f);
        }
        const float baseScale = sample.base.scale;
        const float maxScale = baseScale + sample.amplitude.scale;
        std::array<float, 3> base{sample.base.size[0], sample.base.size[1], baseScale};
        std::array<float, 3> maximum{
            sample.base.size[0] + sample.amplitude.size[0],
            sample.base.size[1] + sample.amplitude.size[1], maxScale};
        if (source.type == 5)
        {
            base = {baseScale, baseScale, baseScale};
            maximum = {maxScale, maxScale, maxScale};
        }
        else if (source.type == 6)
        {
            // IW3 omni lights animate radius in size[0]. Its unused second
            // channel maps to Replay's neutral brightness multiplier.
            if (base[1] == 0.0f && maximum[1] == 0.0f)
                base[1] = maximum[1] = 1.0f;
            base[2] = maximum[2] = 1.0f;
        }
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            sizes[axis].push_back(base[axis]);
            sizes[axis + 3].push_back(maximum[axis]);
        }
    }

    iw8_focus::ParticleModuleColorGraph colorPayload{};
    colorPayload.base.type =
        static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::colorGraph);
    colorPayload.firstCurve = 1;
    auto color = VfxValueModule(iw8_focus::ParticleModuleType::colorGraph, colorPayload);
    color.curves.resize(8);
    for (std::size_t index = 0; index < colors.size(); ++index)
        color.curves[index] = VfxCurve(colors[index], 1.0f);
    colorPayload.base.flags =
        std::equal(colors.begin(), colors.begin() + 4, colors.begin() + 4) ? 0u : 0x10u;
    std::memcpy(color.native.moduleData, &colorPayload, sizeof(colorPayload));
    state.groups[1].push_back(std::move(color));

    iw8_focus::ParticleModuleSizeGraph sizePayload{};
    sizePayload.base.type =
        static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::sizeGraph);
    sizePayload.firstCurve = 1;
    auto size = VfxValueModule(iw8_focus::ParticleModuleType::sizeGraph, sizePayload);
    size.curves.resize(6);
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        const float scale = VfxCurveScale(sizes[axis], sizes[axis + 3]);
        size.curves[axis] = VfxCurve(sizes[axis], scale);
        size.curves[axis + 3] = VfxCurve(sizes[axis + 3], scale);
        sizePayload.curves[axis].scale = scale;
        sizePayload.curves[axis + 3].scale = scale;
        if (sizes[axis] != sizes[axis + 3])
            sizePayload.base.flags |= 0x10;
    }
    std::memcpy(size.native.moduleData, &sizePayload, sizeof(sizePayload));
    state.groups[1].push_back(std::move(size));
}

void AddVfxGravity(const PreparedFxElement &source, iw8::vfx::State &state)
{
    if (source.gravity.base == 0.0f && source.gravity.amplitude == 0.0f)
        return;
    iw8_focus::ParticleModuleGravity payload{};
    payload.base.type = static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::gravity);
    payload.percentage = VfxRange(source.gravity);
    state.groups[1].push_back(
        VfxValueModule(iw8_focus::ParticleModuleType::gravity, payload));
}

void AddVfxImpact(
    const PreparedFxElement &source,
    const std::unordered_map<std::string, std::string> &effectAliases,
    iw8::vfx::State &state)
{
    if (source.effectOnImpact.empty())
        return;
    if (source.type != 5 && source.type != 0)
        throw std::runtime_error("IW3 FX impact conversion is pinned only for model and sprite particles");

    iw8_focus::ParticleModulePhysicsRayCast rayCast{};
    rayCast.base.type =
        static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::physicsRayCast);
    rayCast.bounce = VfxRange(source.reflectionFactor);
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        rayCast.bounds.midPoint.v[axis] =
            (source.collisionMins[axis] + source.collisionMaxs[axis]) * 0.5f;
        rayCast.bounds.halfSize.v[axis] =
            (source.collisionMaxs[axis] - source.collisionMins[axis]) * 0.5f;
    }
    rayCast.useItemClip = source.useItemClip;
    state.groups[1].push_back(
        VfxValueModule(iw8_focus::ParticleModuleType::physicsRayCast, rayCast));

    const auto child = effectAliases.find(source.effectOnImpact);
    if (child == effectAliases.end())
        throw std::runtime_error("IW3 FX impact child was not converted: " +
                                 source.effectOnImpact);
    iw8_focus::ParticleModuleTestImpact impact{};
    impact.test.base.type =
        static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::testImpact);
    impact.test.moduleIndex = static_cast<std::uint16_t>(state.groups[2].size());
    impact.test.orientationOptions = 6;
    impact.test.eventHandlerData.kill = 1;
    auto module = VfxValueModule(iw8_focus::ParticleModuleType::testImpact, impact);
    module.childEffects.push_back(child->second);
    state.groups[2].push_back(std::move(module));
}

void AddVfxBirth(
    const PreparedFxElement &source,
    const std::unordered_map<std::string, std::string> &effectAliases,
    iw8::vfx::State &state)
{
    if (source.effectEmitted.empty())
        return;
    const auto child = effectAliases.find(source.effectEmitted);
    if (child == effectAliases.end())
        throw std::runtime_error("IW3 FX birth child was not converted: " +
                                 source.effectEmitted);

    iw8_focus::ParticleModuleTest birth{};
    birth.base.type = static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::testBirth);
    birth.moduleIndex = static_cast<std::uint16_t>(state.groups[2].size());
    birth.orientationOptions = 6;
    auto module = VfxValueModule(iw8_focus::ParticleModuleType::testBirth, birth);
    module.childEffects.push_back(child->second);
    state.groups[2].push_back(std::move(module));
    state.native.flags |= 0x100000000ull;
}

// IW3 effectOnDeath: Replay's TEST_DEATH in module group 2 (shipped
// common_mp flash_bang_cld_sparks), set up like TEST_BIRTH.
void AddVfxDeath(
    const PreparedFxElement &source,
    const std::unordered_map<std::string, std::string> &effectAliases,
    iw8::vfx::State &state)
{
    if (source.effectOnDeath.empty())
        return;
    const auto child = effectAliases.find(source.effectOnDeath);
    if (child == effectAliases.end())
        throw std::runtime_error("IW3 FX death child was not converted: " + source.effectOnDeath);

    iw8_focus::ParticleModuleTest death{};
    death.base.type = static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::testDeath);
    death.moduleIndex = static_cast<std::uint16_t>(state.groups[2].size());
    death.orientationOptions = 6;
    auto module = VfxValueModule(iw8_focus::ParticleModuleType::testDeath, death);
    module.childEffects.push_back(child->second);
    state.groups[2].push_back(std::move(module));
    state.native.flags |= 0x100000000ull;
}

void AddVfxAtlas(const PreparedFxElement &source, iw8::vfx::State &state)
{
    if (source.atlas.entryCount <= 1)
        return;
    iw8_focus::ParticleModuleInitAtlas payload{};
    payload.base.type = static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initAtlas);
    payload.startFrame = source.atlas.index;
    payload.loopCount = source.atlas.loopCount;
    payload.randomIndex = source.atlas.behavior == 0 ? 1 : 0;
    payload.playOverLife = source.atlas.behavior == 0 ? 0 : 1;
    payload.curves[0].scale = 1.0f;
    payload.curves[1].scale = 1.0f;
    auto module = VfxValueModule(iw8_focus::ParticleModuleType::initAtlas, payload);
    module.curves = {VfxCurve({0.0f, 1.0f}, 1.0f), VfxCurve({1.0f, 1.0f}, 1.0f)};
    state.groups[0].push_back(std::move(module));
}

// Replay plays bullet-impact effects with Z along the surface normal: the impact
// player (0x1733E10) copies the normal into axis row 2 and derives rows 0 and 1
// from it (sub_1413DBBE0) before spawning the effect. IW3 plays them with X along
// the normal, so every IW3 element thrown out of the surface went sideways across
// it (owner tests 2026-10-14, zt-28..zt-30). Rotate the element's local frame
// X -> Z, Z -> -X (Y stays): velocities, spawn offsets, model spin and collision
// bounds. World-space velocity is untouched. IW3's one-sided local +Z "rise" of
// impact dust (small_concrete_1: 75..114 u/s along a tangent IW3 picks) first
// joins the outward velocity, so the dust rises instead of drifting along the
// arbitrary tangent Replay picks.
PreparedFxElement ToImpactFrame(const PreparedFxElement &source)
{
    PreparedFxElement result = source;
    const auto negate = [](PreparedFxFloatRange range) {
        range.base = -range.base;
        range.amplitude = -range.amplitude;
        return range;
    };
    const auto rotateRanges = [&](std::array<PreparedFxFloatRange, 3> &ranges) {
        const auto old = ranges;
        ranges = {negate(old[2]), old[1], old[0]};
    };
    bool oneSidedRise = true;
    for (const auto &sample : source.velocitySamples)
    {
        const auto &velocity = sample.local.velocity;
        oneSidedRise = oneSidedRise && velocity.base[2] >= 0.0f &&
                       velocity.base[2] + velocity.amplitude[2] >= 0.0f;
    }
    for (auto &sample : result.velocitySamples)
    {
        auto &velocity = sample.local.velocity;
        if (oneSidedRise)
        {
            velocity.base[0] += velocity.base[2];
            velocity.amplitude[0] += velocity.amplitude[2];
            velocity.base[2] = 0.0f;
            velocity.amplitude[2] = 0.0f;
        }
        for (auto *vector : {&velocity.base, &velocity.amplitude, &sample.local.totalDelta.base,
                             &sample.local.totalDelta.amplitude})
        {
            const auto old = *vector;
            *vector = {-old[2], old[1], old[0]};
        }
    }
    rotateRanges(result.spawnOrigin);
    rotateRanges(result.spawnAngles);
    rotateRanges(result.angularVelocity);
    result.collisionMins = {-source.collisionMaxs[2], source.collisionMins[1], source.collisionMins[0]};
    result.collisionMaxs = {-source.collisionMins[2], source.collisionMaxs[1], source.collisionMaxs[0]};
    return result;
}

iw8::vfx::Emitter ConvertFxElement(
    const PreparedMap &map, const PreparedFxElement &sourceElement,
    const std::unordered_map<std::string, std::string> &effectAliases,
    const std::string &lightDef, const bool impact)
{
    const std::optional<PreparedFxElement> rotated =
        impact ? std::optional(ToImpactFrame(sourceElement)) : std::nullopt;
    const PreparedFxElement &source = rotated ? *rotated : sourceElement;
    iw8::vfx::Emitter emitter;
    const auto burst = VfxRange(source.spawn.oneShotCount);
    const std::uint32_t maxCount = static_cast<std::uint32_t>(std::max(1, burst.max));
    emitter.native.particleSpawnRate = {static_cast<float>(maxCount),
                                        static_cast<float>(maxCount)};
    emitter.native.particleLife = VfxRange(
        {static_cast<float>(source.lifeSpanMsec.base),
         static_cast<float>(source.lifeSpanMsec.amplitude)},
        0.001f);
    emitter.native.particleDelay = VfxRange(
        {static_cast<float>(source.spawnDelayMsec.base),
         static_cast<float>(source.spawnDelayMsec.amplitude)},
        0.001f);
    emitter.native.particleCountMax = maxCount;
    emitter.native.particleBurstCount = {std::max(1, burst.min), std::max(1, burst.max)};
    const auto distance = VfxRange(source.spawnRange);
    emitter.native.spawnRangeSq = {distance.min * distance.min, distance.max * distance.max};
    emitter.native.spawnFrustumCullRadius = source.spawnFrustumCullRadius;
    emitter.native.emitByDistanceDensity = {0.1f, 0.1f};

    iw8::vfx::State state;
    const bool model = source.type == 5;
    switch (source.type)
    {
    case 0:
        state.native.elementType = 0;
        state.native.flags = 550830604356ull;
        emitter.native.flags = 4194306;
        emitter.native.dataFlags = 30603519;
        if (source.distortion)
        {
            // Shipped distortion sprites (common_mp grenadeexp_half,
            // vfx_muz_undbrl_w_a, cruise-missile thrust) set state bit 40
            // instead of 39 and emitter flags 2.
            state.native.flags = (state.native.flags & ~(1ull << 39)) | (1ull << 40);
            emitter.native.flags = 2;
        }
        break;
    case 1:
        state.native.elementType = 8;
        state.native.flags = 550830635076ull;
        emitter.native.flags = 2;
        emitter.native.dataFlags = 26409215;
        break;
    case 2:
        state.native.elementType = 10;
        state.native.flags = 1074790400ull;
        emitter.native.flags = 4194306;
        emitter.native.dataFlags = 26409215;
        break;
    case 4:
        // Replay keeps the legacy initCloud selector in its ABI, but none of
        // the shipped common-zone VFX uses it. Native cloud/smoke emitters use
        // an element-0 material state with this measured flag family.
        state.native.elementType = 0;
        state.native.flags = 550830630912ull;
        emitter.native.flags = 4194306;
        emitter.native.dataFlags = 30603519;
        break;
    case 5:
        state.native.elementType = 7;
        state.native.flags = 549755815044ull;
        emitter.native.flags = 2;
        emitter.native.dataFlags = 131529215;
        break;
    case 6:
        state.native.elementType = 5;
        state.native.flags = 549756862468ull;
        emitter.native.flags = 130;
        emitter.native.dataFlags = 26409215;
        break;
    case 9:
        // Stock bullet-impact decals (vfx_40mm_imp_asphalt, riotshield hits):
        // state 0x100040, emitter 0x400002, spawn/attributes/decal/rotation
        // only, the size in INIT_ATTRIBUTES (below). The former sprite-style
        // family (state 0x8000007840, size graph) never showed (owner tests
        // 2026-10-14, zt-33/zt-34).
        state.native.elementType = 2;
        state.native.flags = 0x100040ull;
        emitter.native.flags = 0x400002;
        emitter.native.dataFlags = 26409215;
        break;
    case 10:
        state.native.elementType = 9;
        state.native.flags = 4296015872ull;
        emitter.native.flags = 1048578;
        emitter.native.dataFlags = 26410239;
        break;
    default:
        throw std::runtime_error("IW3 FX graph contains an unsupported element type");
    }
    if (!source.effectOnImpact.empty())
    {
        if (model)
        {
            // Shipped Replay debris with PHYSICS_RAY_CAST + TEST_IMPACT uses this
            // exact model-state family. The collision bit is 0x10000 in m_dataFlags.
            state.native.flags = 4297130116ull;
            emitter.native.flags = 4194306;
            emitter.native.dataFlags = 131594751;
        }
        else if (source.type == 0 && !source.distortion)
        {
            // Shipped sprites with PHYSICS_RAY_CAST + TEST_IMPACT (common_mp
            // dismemberment blood, 13 states) use this state family; their
            // data flags are the sprite's plus the 0x10000 collision stream.
            state.native.flags = 5371920388ull;
            emitter.native.flags = 2;
            emitter.native.dataFlags = 30669055;
        }
        else
            throw std::runtime_error("IW3 FX impact conversion is pinned only for model and sprite particles");
    }

    // Every stock state with a test module (impact, birth, death) allocates
    // the 0x10000 per-particle stream that Particle_HandleSuccessfulTest
    // writes its result flags through; without it IW3 debris with a birth
    // effect (impacts/large_brick -> brickimpact_em2) crashed the game there
    // (owner tests 2026-10-14, zt-30 and zt-34).
    if (!source.effectOnImpact.empty() || !source.effectOnDeath.empty() ||
        !source.effectEmitted.empty())
        emitter.native.dataFlags |= 0x10000;
    state.groups[0].push_back(VfxSpawnModule());
    if (source.type == 9)
    {
        // IW3 decals give a radius in size[0] (impacts/small_concrete_1: 4);
        // stock decals carry it in INIT_ATTRIBUTES (vfx_semtex_bolt_decal 50, 50).
        auto attributes = VfxAttributesModule();
        iw8_focus::ParticleModuleInitAttributes payload{};
        std::memcpy(&payload, attributes.native.moduleData, sizeof(payload));
        float low = 1.0f, high = 1.0f;
        if (!source.visualSamples.empty())
        {
            const auto &first = source.visualSamples.front();
            low = first.base.size[0];
            high = first.base.size[0] + first.amplitude.size[0];
            if (low > high)
                std::swap(low, high);
        }
        VfxSetVec(payload.sizeMin, low, low, 0.0f);
        VfxSetVec(payload.sizeMax, high, high, 0.0f);
        std::memcpy(attributes.native.moduleData, &payload, sizeof(payload));
        state.groups[0].push_back(std::move(attributes));
    }
    else
        state.groups[0].push_back(VfxAttributesModule());
    if (source.type == 10)
    {
        iw8_focus::ParticleModuleInitRunner payload{};
        payload.base.type =
            static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initRunner);
        VfxSetVec(payload.scaleMin, 1.0f, 1.0f, 1.0f);
        VfxSetVec(payload.scaleMax, 1.0f, 1.0f, 1.0f);
        VfxSetVec(payload.velocityMin, 1.0f, 1.0f, 1.0f);
        VfxSetVec(payload.velocityMax, 1.0f, 1.0f, 1.0f);
        payload.orientationOptions = 2;
        payload.attachToParent = 1;
        payload.stopChildOnDeath = 1;
        payload.killChildOnDeath = 1;
        payload.legacyOrientationRotation = 1;
        auto module = VfxValueModule(iw8_focus::ParticleModuleType::initRunner, payload);
        for (const auto &visual : source.visuals)
        {
            if (visual.kind != PreparedFxVisualKind::effect || visual.names.size() != 1)
                throw std::runtime_error("IW3 runner has an invalid child effect");
            const auto alias = effectAliases.find(visual.names.front());
            if (alias == effectAliases.end())
                throw std::runtime_error("IW3 runner child effect was not converted: " +
                                         visual.names.front());
            module.childEffects.push_back(alias->second);
        }
        state.groups[0].push_back(std::move(module));
        emitter.states.push_back(std::move(state));
        return emitter;
    }
    if (source.type == 6)
    {
        if (lightDef.empty())
            throw std::runtime_error("IW3 omni light has no Replay LightDef");
        if (!std::ranges::all_of(source.visuals, [](const PreparedFxVisual &visual) {
                return visual.kind == PreparedFxVisualKind::none && visual.names.empty();
            }))
            throw std::runtime_error("IW3 omni light has an invalid visual definition");
        iw8_focus::ParticleModuleInitLightOmni payload{};
        payload.base.type =
            static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initLightOmni);
        payload.base.flags = 2048;
        payload.fovOuter = 0.7853981852531433f;
        payload.bulbRadius = 2.0f;
        payload.bulbLength = 1.0f / 255.0f;
        payload.distanceFalloff = 1.0f;
        payload.brightness = 1.0f;
        payload.shadowSoftness = 0.55f;
        payload.shadowBias = 0.4f;
        payload.shadowArea = 0.018f;
        payload.toneMappingScaleFactor = 1.0f;
        payload.disableVolumetric = 1;
        payload.disableDynamicShadows = 1;
        payload.scriptScale = 1;
        auto module = VfxValueModule(iw8_focus::ParticleModuleType::initLightOmni, payload);
        module.lightDefs.push_back(lightDef);
        state.groups[0].push_back(std::move(module));
    }
    else if (source.type == 9)
    {
        iw8_focus::ParticleModuleInitDecal payload{};
        payload.base.type =
            static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initDecal);
        // As every stock decal (vfx_explo_symtex_gren_a, vfx_semtex_bolt_decal,
        // footstep ripples): no stop fade (0xFFFF) and a white lerp colour. The
        // projection runs into the surface: FX_MARK_PROJECTION_AXIS_NEG_Z (5)
        // for an impact effect, whose frame has Z along the normal
        // (ToImpactFrame), else IW3's own -X (FX_MARK_PROJECTION_AXIS_NEG_X, 1).
        // The zeroed fields projected IW3 bullet holes along +X, a tangent, with
        // a black lerp colour: none showed (owner test 2026-10-14, zt-33).
        payload.stoppableFadeOutTime = 0xFFFF;
        VfxSetVec(payload.lerpColor, 1.0f, 1.0f, 1.0f, 1.0f);
        payload.projectionAxis = impact ? 5 : 1;
        auto module = VfxValueModule(iw8_focus::ParticleModuleType::initDecal, payload);
        for (const auto &visual : source.visuals)
        {
            if (visual.kind != PreparedFxVisualKind::decal || visual.names.size() != 2)
                throw std::runtime_error("IW3 decal has an invalid material pair");
            // Replay consumes one selected platform/profile material in all three
            // ParticleMarkVisuals slots. The second IW3 decal entry is the WC
            // world-context material used by this Windows Replay target.
            const auto material = VfxMaterial(map, visual.names[1]);
            module.decalMaterials.push_back({material, material, material});
        }
        state.groups[0].push_back(std::move(module));
    }
    else
    {
        iw8::vfx::Module module;
        if (model)
        {
            iw8_focus::ParticleModuleInitModel payload{};
            payload.base =
                static_cast<std::uint64_t>(iw8_focus::ParticleModuleType::initModel);
            module = VfxValueModule(iw8_focus::ParticleModuleType::initModel, payload);
            for (const auto &visual : source.visuals)
            {
                if (visual.kind != PreparedFxVisualKind::xmodel || visual.names.size() != 1)
                    throw std::runtime_error("IW3 model particle has an invalid XModel visual");
                module.models.push_back(visual.names.front());
            }
        }
        else
        {
            if (source.type == 1)
            {
                iw8_focus::ParticleModuleInitOrientedSprite oriented{};
                oriented.base.type = static_cast<std::uint16_t>(
                    iw8_focus::ParticleModuleType::initOrientedSprite);
                VfxSetVec(oriented.orientationQuat, 0.0f, 0.0f, 0.0f, 1.0f);
                state.groups[0].push_back(VfxValueModule(
                    iw8_focus::ParticleModuleType::initOrientedSprite, oriented));
            }
            if (source.type == 2)
            {
                iw8_focus::ParticleModuleInitTail tail{};
                tail.base.type =
                    static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initTail);
                state.groups[0].push_back(
                    VfxValueModule(iw8_focus::ParticleModuleType::initTail, tail));
            }
            iw8_focus::ParticleModuleInitMaterial payload{};
            payload.base.type =
                static_cast<std::uint16_t>(iw8_focus::ParticleModuleType::initMaterial);
            module = VfxValueModule(iw8_focus::ParticleModuleType::initMaterial, payload);
            for (const auto &visual : source.visuals)
            {
                if (visual.kind != PreparedFxVisualKind::material || visual.names.size() != 1)
                    throw std::runtime_error("IW3 particle has an invalid material visual");
                module.materials.push_back(VfxMaterial(map, visual.names.front()));
            }
        }
        state.groups[0].push_back(std::move(module));
    }

    AddVfxAtlas(source, state);
    AddVfxSpawnShape(source, impact, state);
    AddVfxVelocity(source, state);
    AddVfxRotation(source, model, state);
    AddVfxGravity(source, state);
    AddVfxImpact(source, effectAliases, state);
    AddVfxBirth(source, effectAliases, state);
    AddVfxDeath(source, effectAliases, state);
    if (source.type != 9)
        AddVfxVisualCurves(source, state);
    emitter.states.push_back(std::move(state));
    return emitter;
}

iw8::vfx::Effect ConvertFx(
    const PreparedMap &map, const PreparedFx &source,
    const std::unordered_map<std::string, std::string> &effectAliases,
    const std::string &lightDef)
{
    if (source.elements.empty())
        throw std::runtime_error("IW3 FX graph has no elements: " + source.name);
    iw8::vfx::Effect effect;
    effect.name = effectAliases.at(source.name);
    effect.native.flags = 262145;
    if (std::ranges::any_of(source.elements,
                            [](const PreparedFxElement &element) { return element.type == 6; }))
        effect.native.flags |= 0x4;
    effect.native.occlusionOverrideEmitterIndex = -1;
    effect.native.drawFrustumCullRadius = 350.0f;
    effect.native.updateFrustumCullRadius = 500.0f;
    effect.native.sunDistance = 100000.0f;
    effect.emitters.reserve(source.elements.size());
    for (const auto &element : source.elements)
        effect.emitters.push_back(ConvertFxElement(map, element, effectAliases, lightDef,
                                                   source.name.starts_with("impacts/")));
    return effect;
}

// First reason an IW3 FX graph has no direct Replay mapping (empty when it has one).
std::string FxMappingBlocker(const PreparedMap &map, const PreparedFx &effect)
{
    if (effect.elements.empty())
        return "no elements";
    for (const auto &element : effect.elements)
    {
        const std::string type = std::to_string(element.type);
        if (!(element.type == 0 || element.type == 1 || element.type == 2 || element.type == 4 ||
              element.type == 5 || element.type == 6 || element.type == 9 || element.type == 10))
            return "element type " + type;
        if (!element.effectOnDeath.empty() && element.type == 10)
            return "runner with death effect";
        if (!element.effectOnImpact.empty() && element.type != 5 && element.type != 0)
            return "effect on impact on element type " + type;
        if (!element.effectEmitted.empty() && element.type == 10)
            return "runner with emitted effect";
        for (const auto &visual : element.visuals)
        {
            if ((element.type == 0 || element.type == 1 || element.type == 2 ||
                 element.type == 4) &&
                (visual.kind != PreparedFxVisualKind::material || visual.names.size() != 1 ||
                 !map.fxMaterialAliases.contains(visual.names.front())))
                return "material " + (visual.names.empty() ? std::string("?") : visual.names.front()) +
                       " (type " + type + ")";
            if (element.type == 9 &&
                (visual.kind != PreparedFxVisualKind::decal || visual.names.size() != 2 ||
                 !map.fxMaterialAliases.contains(visual.names[1])))
                return "decal material " +
                       (visual.names.size() == 2 ? visual.names[1] : std::string("?"));
            if (element.type == 6 &&
                (visual.kind != PreparedFxVisualKind::none || !visual.names.empty()))
                return "omni light visual";
        }
    }
    return {};
}

bool HasDirectReplayFxMapping(const PreparedMap &map, const PreparedFx &effect)
{
    return !effect.elements.empty() &&
           std::ranges::all_of(effect.elements, [&](const PreparedFxElement &element) {
               const bool supportedType = element.type == 0 || element.type == 1 ||
                                          element.type == 2 ||
                                          element.type == 4 || element.type == 5 ||
                                          element.type == 6 ||
                                          element.type == 9 || element.type == 10;
               if (!supportedType || (!element.effectOnDeath.empty() && element.type == 10) ||
                   (!element.effectOnImpact.empty() && element.type != 5 && element.type != 0) ||
                   (!element.effectEmitted.empty() && element.type == 10))
                   return false;
               if (element.type == 0 || element.type == 1 || element.type == 2 ||
                   element.type == 4)
                   return std::ranges::all_of(element.visuals, [&](const PreparedFxVisual &visual) {
                       return visual.kind == PreparedFxVisualKind::material &&
                              visual.names.size() == 1 &&
                              map.fxMaterialAliases.contains(visual.names.front());
                   });
               if (element.type == 6)
                   return std::ranges::all_of(
                       element.visuals, [](const PreparedFxVisual &visual) {
                           return visual.kind == PreparedFxVisualKind::none &&
                                  visual.names.empty();
                       });
               if (element.type == 9)
                   return std::ranges::all_of(
                       element.visuals, [&](const PreparedFxVisual &visual) {
                           // The Windows Replay decal module selects IW3's
                           // world-context (second) material. Do not admit a
                           // graph unless that exact material has a converted
                           // Replay carrier; ConvertFxElement would otherwise
                           // discover the missing asset only after aliasing the
                           // parent graph.
                           return visual.kind == PreparedFxVisualKind::decal &&
                                  visual.names.size() == 2 &&
                                  map.fxMaterialAliases.contains(visual.names[1]);
                       });
               return true;
           });
}

std::unordered_map<std::string, std::string>
BuildFxAliases(const PreparedMap &map, const std::string &targetMap)
{
    const auto &effects = map.fxEffects;
    std::set<std::string> convertible;
    for (const auto &effect : effects)
    {
        if (HasDirectReplayFxMapping(map, effect))
            convertible.insert(effect.name);
        else
            zt::info("iw3: FX %s not converted: %s", effect.name.c_str(),
                     FxMappingBlocker(map, effect).c_str());
    }

    bool changed = true;
    while (changed)
    {
        changed = false;
        for (auto iterator = convertible.begin(); iterator != convertible.end();)
        {
            const auto effect = std::ranges::find(effects, *iterator, &PreparedFx::name);
            const bool missingChild = std::ranges::any_of(
                effect->elements, [&](const PreparedFxElement &element) {
                    const bool missingRunner =
                        element.type == 10 &&
                        std::ranges::any_of(element.visuals, [&](const PreparedFxVisual &visual) {
                            return visual.kind != PreparedFxVisualKind::effect ||
                                   visual.names.size() != 1 ||
                                   !convertible.contains(visual.names.front());
                        });
                    const bool missingImpact = !element.effectOnImpact.empty() &&
                                               !convertible.contains(element.effectOnImpact);
                    const bool missingBirth = !element.effectEmitted.empty() &&
                                              !convertible.contains(element.effectEmitted);
                    const bool missingDeath = !element.effectOnDeath.empty() &&
                                              !convertible.contains(element.effectOnDeath);
                    return missingRunner || missingImpact || missingBirth || missingDeath;
                });
            if (missingChild)
            {
                zt::info("iw3: FX %s not converted: a child effect is not convertible",
                         iterator->c_str());
                iterator = convertible.erase(iterator);
                changed = true;
            }
            else
            {
                ++iterator;
            }
        }
    }

    std::unordered_map<std::string, std::string> aliases;
    aliases.reserve(convertible.size());
    for (const auto &name : convertible)
        aliases.emplace(name, "vfx/mw120r/" + targetMap + "/" + name);
    return aliases;
}

iw8::impact::EffectOverrides ReadImpactOverrides(
    const std::filesystem::path &root,
    const std::vector<std::filesystem::path> &sourcePaths,
    const std::unordered_map<std::string, std::string> &effectAliases)
{
    // IW3 rows: bullet small, large, shotgun and armor piercing (each followed
    // by its exit row), grenade bounce, grenade explosion, rocket explosion and
    // projectile dud. Replay weapons select the VFX impact type themselves
    // (weapon +452, q_CG_Weapons_GetVFXImpactType), so every Replay bullet type
    // takes the IW3 row of its calibre: sml 1/37/38, med 2 and the med blueprint
    // variants 33-36/39/41, lrg 3/4/5/7, 12g 6/40, xxl 15. Exits exist only
    // where impact_data::kExitMask has the type.
    struct Target
    {
        std::size_t sourceRow;
        std::size_t impactType;
        std::size_t direction;
    };
    constexpr std::array<Target, 36> targets{{
        {0, 1, 0},   {0, 37, 0},  {0, 38, 0},  {1, 1, 1},   {1, 37, 1},  {1, 38, 1},
        {2, 2, 0},   {2, 33, 0},  {2, 34, 0},  {2, 35, 0},  {2, 36, 0},  {2, 39, 0},
        {2, 41, 0},  {2, 3, 0},   {2, 4, 0},   {2, 5, 0},   {2, 7, 0},   {3, 2, 1},
        {3, 33, 1},  {3, 34, 1},  {3, 35, 1},  {3, 36, 1},  {3, 39, 1},  {3, 41, 1},
        {3, 3, 1},   {3, 4, 1},   {4, 6, 0},   {4, 40, 0},  {5, 6, 1},   {5, 40, 1},
        {6, 15, 0},  {7, 15, 1},  {8, 9, 0},   {9, 10, 0},  {10, 12, 0}, {11, 14, 0},
    }};
    constexpr std::size_t sourceRowCount = 12;
    constexpr std::size_t nonFleshCount = 29;
    constexpr std::size_t fleshCount = 4;

    std::filesystem::path path;
    const auto relative = std::filesystem::path("impactfx") / "_default.iw3.json";
    std::vector<std::filesystem::path> roots{root};
    roots.insert(roots.end(), sourcePaths.begin(), sourcePaths.end());
    std::error_code pathError;
    for (const auto &source : roots)
    {
        for (const auto &candidate : {source / relative, source / "raw" / relative})
        {
            if (std::filesystem::is_regular_file(candidate, pathError))
            {
                path = candidate;
                break;
            }
            pathError.clear();
        }
        if (!path.empty())
            break;
    }
    if (path.empty())
        throw std::runtime_error(
            "IW3 impact FX export is missing; add common_mp.ff or an extracted IW3 raw "
            "directory with --search-path");
    const Json source = ReadJson(path);
    const auto &layout = source.at("layout");
    const auto &entries = source.at("entries");
    if (source.value("schema", 0) != 1 ||
        source.value("asset_type", std::string{}) != "iw3_impact_fx" ||
        !layout.is_object() || layout.value("impact_count", 0) != sourceRowCount ||
        layout.value("nonflesh_count", 0) != nonFleshCount ||
        layout.value("flesh_count", 0) != fleshCount || !entries.is_array() ||
        entries.size() != sourceRowCount)
        throw std::runtime_error("IW3 impact FX export has an unsupported schema or layout");

    iw8::impact::EffectOverrides overrides;
    overrides.reserve(targets.size() * (nonFleshCount + fleshCount) * 2);
    std::set<std::string> unconvertedImpacts;
    for (std::size_t sourceRow = 0; sourceRow < sourceRowCount; ++sourceRow)
    {
        const auto &entry = entries.at(sourceRow);
        if (entry.at("index").get<std::size_t>() != sourceRow)
            throw std::runtime_error("IW3 impact FX row index is inconsistent");
        const auto &nonFlesh = entry.at("nonflesh");
        const auto &flesh = entry.at("flesh");
        if (!nonFlesh.is_array() || nonFlesh.size() != nonFleshCount ||
            !flesh.is_array() || flesh.size() != fleshCount)
            throw std::runtime_error("IW3 impact FX row has invalid surface arrays");

        const auto append = [&](const Json &value, const bool isFlesh,
                                const std::size_t index) {
            if (value.is_null())
                return;
            if (!value.is_string() || !IsValidFxAssetName(value.get<std::string>()))
                throw std::runtime_error("IW3 impact FX slot has an invalid effect name");
            const auto alias = effectAliases.find(value.get<std::string>());
            if (alias == effectAliases.end())
            {
                unconvertedImpacts.insert(value.get<std::string>());
                return;
            }
            // Nonflesh slots are IW3 surface types; Replay indexes the table by
            // its own surface type (trace surfaceFlags >> 19), the mapping the
            // collision tags use. IW3 flesh slots (body/head x nonfatal/fatal)
            // match Replay's first four flesh slots.
            const std::size_t target = isFlesh ? index
                                       : index ? NativeSurfaceMaterial(1u << (index - 1))
                                               : 0;
            if (!isFlesh && !target)
                return;
            for (const Target &destination : targets)
            {
                if (destination.sourceRow != sourceRow)
                    continue;
                // IW3 plays one effect per slot; clear Replay's second pack too.
                overrides.push_back({destination.impactType, destination.direction, isFlesh,
                                     target, 0, "," + alias->second});
                overrides.push_back({destination.impactType, destination.direction, isFlesh,
                                     target, 1, {}});
            }
        };
        for (std::size_t index = 0; index < nonFleshCount; ++index)
            append(nonFlesh.at(index), false, index);
        for (std::size_t index = 0; index < fleshCount; ++index)
            append(flesh.at(index), true, index);
    }
    if (!unconvertedImpacts.empty())
    {
        std::string names;
        for (const auto &name : unconvertedImpacts)
            names += (names.empty() ? "" : ", ") + name;
        zt::warn("iw3: %zu IW3 impact effect(s) did not convert; their slots keep Replay's "
                 "stock impact: %s",
                 unconvertedImpacts.size(), names.c_str());
    }
    return overrides;
}
} // namespace

PreparedMap::PreparedMap(PreparedMap &&other) noexcept
    : root(std::move(other.root))
    , collision(std::move(other.collision))
    , footsteps(std::move(other.footsteps))
    , scratch(std::move(other.scratch))
    , xmodels(std::move(other.xmodels))
    , rawFiles(std::move(other.rawFiles))
    , fxEffects(std::move(other.fxEffects))
    , fxMaterialAliases(std::move(other.fxMaterialAliases))
    , vfxEffects(std::move(other.vfxEffects))
    , vfxLightDef(std::move(other.vfxLightDef))
    , smallGlassEffect(std::move(other.smallGlassEffect))
    , impactOverrides(std::move(other.impactOverrides))
    , staticModels(std::move(other.staticModels))
    , staticModelCollisionModels(std::move(other.staticModelCollisionModels))
    , shadowScene(std::move(other.shadowScene))
    , dynamicEntities(std::move(other.dynamicEntities))
    , navigation(std::move(other.navigation))
    , mapScript(std::move(other.mapScript))
    , gradingClutName(std::move(other.gradingClutName))
    , gradingClutPixels(std::move(other.gradingClutPixels))
    , soundBank(std::move(other.soundBank))
{
    other.scratch.clear();
}

PreparedMap &PreparedMap::operator=(PreparedMap &&other) noexcept
{
    if (this != &other)
    {
        if (!scratch.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(scratch, error);
        }
        root = std::move(other.root);
        collision = std::move(other.collision);
        footsteps = std::move(other.footsteps);
        scratch = std::move(other.scratch);
        xmodels = std::move(other.xmodels);
        rawFiles = std::move(other.rawFiles);
        fxEffects = std::move(other.fxEffects);
        fxMaterialAliases = std::move(other.fxMaterialAliases);
        vfxEffects = std::move(other.vfxEffects);
        vfxLightDef = std::move(other.vfxLightDef);
        smallGlassEffect = std::move(other.smallGlassEffect);
        impactOverrides = std::move(other.impactOverrides);
        staticModels = std::move(other.staticModels);
        staticModelCollisionModels = std::move(other.staticModelCollisionModels);
        shadowScene = std::move(other.shadowScene);
        dynamicEntities = std::move(other.dynamicEntities);
        navigation = std::move(other.navigation);
        mapScript = std::move(other.mapScript);
        gradingClutName = std::move(other.gradingClutName);
        gradingClutPixels = std::move(other.gradingClutPixels);
        soundBank = std::move(other.soundBank);
        other.scratch.clear();
    }
    return *this;
}

PreparedMap::~PreparedMap()
{
    // Diagnostic only: keep the extracted/prepared intermediates for inspection.
    if (!scratch.empty() && GetEnvironmentVariableW(L"IW8_ZONETOOL_KEEP_SCRATCH", nullptr, 0))
    {
        zt::info("iw3: kept conversion intermediates at %s", scratch.string().c_str());
        return;
    }
    if (!scratch.empty())
    {
        std::error_code error;
        std::filesystem::remove_all(scratch, error);
    }
}

// Unlinker extraction depends only on the Unlinker build, the source zones and
// the search paths, yet costs over a minute per build. Key a reusable copy of
// the finished extraction on those inputs (path, size, modification time).
std::filesystem::path ExtractionCacheDirectory(const std::filesystem::path &unlinker,
                                               const ImportOptions &options)
{
    // v3: physics presets and sound curves are resolved from common_mp.
    std::string key = "iw3-extract-v3";
    const auto stamp = [&key](const std::filesystem::path &path) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        const auto size = std::filesystem::file_size(path, error);
        const auto written = error ? std::filesystem::file_time_type{}
                                   : std::filesystem::last_write_time(path, error);
        key += '\x1f' + absolute.string() + '\x1f' + std::to_string(error ? 0 : size) + '\x1f' +
               std::to_string(error ? 0 : written.time_since_epoch().count());
    };
    stamp(unlinker);
    stamp(options.fastfile);
    stamp(options.fastfile.parent_path() /
          (options.fastfile.stem().string() + "_load.ff"));
    if (const auto common = FindIw3CommonFastfile(options); !common.empty())
        stamp(common);
    for (const auto &path : options.searchPaths)
        key += '\x1f' + std::filesystem::absolute(path).string();
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : key)
    {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return options.scratchRoot / ".iw3-extract-cache" / text;
}

// IW3 gametype visuals (--iw3-mode-assets). With them, IW3's domination flag
// props stand at each flag; Replay's domination only draws objective markers.
// The "dom" gameobject name lets the gametype delete them outside domination,
// as IW3 did. Without them, the IW3 headquarters radios keep their entities
// (Replay's hq.gsc needs hqloc) but lose their IW3 laptop model.
std::string WithIw3ModeAssets(const std::string &text, const bool enabled)
{
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const auto entities = ParseEntityFields(text, &spans);
    const auto value = [](const EntityFields &entity, const char *key) {
        const auto found = entity.find(key);
        return found == entity.end() ? std::string{} : found->second;
    };
    std::string result;
    std::size_t copied = 0;
    std::size_t flags = 0;
    std::size_t radios = 0;
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const auto &entity = entities[index];
        const bool flag = value(entity, "classname") == "trigger_radius" &&
                          value(entity, "targetname") == "flag_primary";
        const bool radio = value(entity, "classname") == "script_model" &&
                           value(entity, "targetname") == "hq_hardpoint";
        if (enabled && flag)
        {
            result.append(text, copied, spans[index].second - copied);
            copied = spans[index].second;
            result += "\n{\n\"classname\" \"script_model\"\n\"targetname\" \"mw120r_dom_flag\"\n"
                      "\"script_gameobjectname\" \"dom\"\n\"model\" \"prop_flag_neutral\"\n"
                      "\"origin\" \"" +
                      value(entity, "origin") + "\"\n}";
            ++flags;
        }
        else if (!enabled && radio)
        {
            result.append(text, copied, spans[index].first - copied);
            copied = spans[index].second;
            result += "{\n";
            for (const auto &[key, field] : entity)
                if (key != "model")
                    result += "\"" + key + "\" \"" + field + "\"\n";
            result += "}";
            ++radios;
        }
    }
    result.append(text, copied, text.size() - copied);
    if (enabled)
        zt::info("iw3: mode assets on: placed %zu IW3 domination flag prop(s)", flags);
    else
        zt::info("iw3: mode assets off: removed the IW3 model from %zu headquarters radio(s)",
                 radios);
    return result;
}

// Capture the flag. IW3 had no stock CTF; custom maps place the flags from
// their map script for mod CTF (`addobj("allied_flag", (x, y, z), ...)`, which
// spawns a trigger_radius) or as allied_flag / axis_flag entities. Replay's
// ctf.gsc reads ctf_zone_<team> (only its origin; it spawns its own trigger)
// and ctf_flag_<team> (it sets the common_base_mp flag model on it), spawns
// at mp_ctf_spawn for both teams, and the mp_ctf_spawn_<team>_start sets.
// Without flags, no CTF entities are made and the gametype uses its fallbacks.
std::string WithIw3CtfObjectives(const std::string &text, const std::string &mapScript)
{
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    const auto entities = ParseEntityFields(text, &spans);
    const auto value = [](const EntityFields &entity, const char *key) {
        const auto found = entity.find(key);
        return found == entity.end() ? std::string{} : found->second;
    };
    const auto parseOrigin = [](const std::string &origin) -> std::optional<Vec3> {
        Vec3 result{};
        if (std::sscanf(origin.c_str(), "%f %f %f", &result[0], &result[1], &result[2]) != 3)
            return std::nullopt;
        return result;
    };
    std::array<std::optional<Vec3>, 2> flags; // allies, axis
    constexpr std::array<const char *, 2> teams{{"allies", "axis"}};
    constexpr std::array<const char *, 2> flagNames{{"allied_flag", "axis_flag"}};
    for (const auto &entity : entities)
        for (std::size_t team = 0; team < 2; ++team)
            if (value(entity, "targetname") == flagNames[team] && !flags[team])
                flags[team] = parseOrigin(value(entity, "origin"));
    for (std::size_t team = 0; team < 2; ++team)
    {
        // addobj("allied_flag", (x, y, z), ...) in the IW3 map script.
        const std::string quoted = std::string("\"") + flagNames[team] + "\"";
        for (std::size_t at = mapScript.find(quoted); !flags[team] && at != std::string::npos;
             at = mapScript.find(quoted, at + 1))
        {
            const auto open = mapScript.find('(', at);
            const auto close = mapScript.find(')', open);
            if (open == std::string::npos || close == std::string::npos ||
                mapScript.find_first_not_of(" \t,", at + quoted.size()) != open)
                continue;
            Vec3 origin{};
            const auto inside = mapScript.substr(open + 1, close - open - 1);
            if (std::sscanf(inside.c_str(), " %f , %f , %f", &origin[0], &origin[1],
                            &origin[2]) == 3)
                flags[team] = origin;
        }
    }
    // Without placed flags, CoD4 CTF mods use each team's sabotage bomb site
    // (sab_bomb_<team>, in that team's base) as its flag base.
    const char *flagSource = "placed flags";
    if (!flags[0] || !flags[1])
    {
        std::array<std::optional<Vec3>, 2> sites;
        for (const auto &entity : entities)
            for (std::size_t team = 0; team < 2; ++team)
                if (value(entity, "targetname") == std::string("sab_bomb_") + teams[team] &&
                    !sites[team])
                    sites[team] = parseOrigin(value(entity, "origin"));
        if (!sites[0] || !sites[1])
            return text;
        flags = sites;
        flagSource = "sabotage bomb sites";
    }

    const auto distance =[](const Vec3 &a, const Vec3 &b) {
        return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
    };
    const auto originText = [](const Vec3 &origin) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", origin[0], origin[1], origin[2]);
        return std::string(buffer);
    };
    const auto write = [](const EntityFields &entity, const std::string &classname) {
        std::string block = "\n{\n\"classname\" \"" + classname + "\"\n";
        for (const auto &[key, field] : entity)
            if (key != "classname")
                block += "\"" + key + "\" \"" + field + "\"\n";
        return block + "}";
    };

    bool haveSpawns = false;
    std::array<bool, 2> haveStarts{};
    std::vector<const EntityFields *> tdmSpawns;
    std::array<std::vector<const EntityFields *>, 2> tdmStarts;
    std::string result;
    std::size_t copied = 0;
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        const auto &entity = entities[index];
        const auto classname = value(entity, "classname");
        const auto targetname = value(entity, "targetname");
        const bool flag = targetname == flagNames[0] || targetname == flagNames[1];
        const bool teamSpawn = classname == "mp_ctf_spawn_allies" || classname == "mp_ctf_spawn_axis";
        haveSpawns = haveSpawns || teamSpawn || classname == "mp_ctf_spawn";
        for (std::size_t team = 0; team < 2; ++team)
        {
            if (classname == std::string("mp_ctf_spawn_") + teams[team] + "_start")
                haveStarts[team] = true;
            if (classname == std::string("mp_tdm_spawn_") + teams[team] + "_start")
                tdmStarts[team].push_back(&entity);
        }
        if (classname == "mp_tdm_spawn")
            tdmSpawns.push_back(&entity);
        if (!flag && !teamSpawn)
            continue;
        // Replay registers one mp_ctf_spawn class for both teams; the mod flag
        // entities are replaced by the Replay zone and flag records below.
        result.append(text, copied, spans[index].first - copied);
        copied = spans[index].second;
        if (teamSpawn)
            result += write(entity, "mp_ctf_spawn");
    }
    result.append(text, copied, text.size() - copied);

    for (std::size_t team = 0; team < 2; ++team)
    {
        const auto origin = originText(*flags[team]);
        result += std::string("\n{\n\"classname\" \"trigger_radius\"\n\"targetname\" \"ctf_zone_") +
                  teams[team] + "\"\n\"origin\" \"" + origin +
                  "\"\n\"radius\" \"48\"\n\"height\" \"148\"\n}";
        result += std::string("\n{\n\"classname\" \"script_model\"\n\"targetname\" \"ctf_flag_") +
                  teams[team] + "\"\n\"origin\" \"" + origin + "\"\n}";
    }
    std::size_t spawns = 0, starts = 0;
    if (!haveSpawns)
        for (const auto *spawn : tdmSpawns)
        {
            result += write(*spawn, "mp_ctf_spawn");
            ++spawns;
        }
    if (!haveStarts[0] && !haveStarts[1] && !tdmStarts[0].empty() && !tdmStarts[1].empty())
    {
        // Each team starts at the TDM start set nearer its own flag.
        const auto centroid = [&](const std::vector<const EntityFields *> &set) {
            Vec3 sum{};
            for (const auto *spawn : set)
                if (const auto origin = parseOrigin(value(*spawn, "origin")))
                    for (std::size_t axis = 0; axis < 3; ++axis)
                        sum[axis] += (*origin)[axis] / static_cast<float>(set.size());
            return sum;
        };
        const Vec3 first = centroid(tdmStarts[0]);
        const Vec3 second = centroid(tdmStarts[1]);
        const bool swap = distance(first, *flags[1]) + distance(second, *flags[0]) <
                          distance(first, *flags[0]) + distance(second, *flags[1]);
        for (std::size_t team = 0; team < 2; ++team)
            for (const auto *spawn : tdmStarts[swap ? 1 - team : team])
            {
                result += write(*spawn, std::string("mp_ctf_spawn_") + teams[team] + "_start");
                ++starts;
            }
    }
    zt::info("iw3: capture the flag from %s: allies flag (%s), axis flag (%s), %zu spawn(s) and "
             "%zu start spawn(s) made from TDM",
             flagSource, originText(*flags[0]).c_str(), originText(*flags[1]).c_str(), spawns,
             starts);
    return result;
}

// IW3 sun sprite, flare, blind and glare (sun/<map>.sun r_sun* dvars) as
// Replay's GfxWorld sunflare_t. IW3 R_LoadSunThroughDvars stores the angles as
// cosines and the fade times in milliseconds, as Replay does. Without a .sun
// file, Replay's stock values (mp_rust) apply: no sprite or flare, the stock
// blind and glare.
Json ReadSunFlare(const std::filesystem::path &exportRoot, const std::string &sourceMap)
{
    std::map<std::string, float> values{
        {"r_sunsprite_size", 0.0f},        {"r_sunflare_min_size", 0.0f},
        {"r_sunflare_min_angle", 45.0f},   {"r_sunflare_max_size", 0.0f},
        {"r_sunflare_max_angle", 2.0f},    {"r_sunflare_max_alpha", 0.0f},
        {"r_sunflare_fadein", 0.7f},       {"r_sunflare_fadeout", 1.0f},
        {"r_sunblind_min_angle", 30.0f},   {"r_sunblind_max_angle", 5.0f},
        {"r_sunblind_max_darken", 0.5f},   {"r_sunblind_fadein", 2.5f},
        {"r_sunblind_fadeout", 6.0f},      {"r_sunglare_min_angle", 30.0f},
        {"r_sunglare_max_angle", 5.0f},    {"r_sunglare_max_lighten", 0.3f},
        {"r_sunglare_fadein", 2.5f},       {"r_sunglare_fadeout", 6.0f}};
    std::string text;
    const bool authored = zt::read_file_str(
        (exportRoot / "sun" / (sourceMap + ".sun")).string(), text);
    if (authored)
    {
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);)
        {
            std::istringstream fields(line);
            std::string key, value;
            if (!(fields >> key) || !values.contains(Lowercase(key)))
                continue;
            std::getline(fields, value);
            const auto quote = value.find('"');
            const auto end = value.find('"', quote + 1);
            if (quote == std::string::npos || end == std::string::npos)
                continue;
            const float number = std::strtof(value.substr(quote + 1, end - quote - 1).c_str(), nullptr);
            if (std::isfinite(number))
                values[Lowercase(key)] = number;
        }
    }
    const auto dot = [&](const char *key) {
        return std::cos(values.at(key) * 3.14159265358979323846f / 180.0f);
    };
    const auto ms = [&](const char *key) {
        return static_cast<int>(std::lround(std::max(0.0f, values.at(key)) * 1000.0f));
    };
    zt::info("iw3: sun %s: sprite %g, flare %g..%g, blind darken %g, glare lighten %g",
             authored ? "file converted" : "file absent; stock Replay blind and glare",
             values.at("r_sunsprite_size"), values.at("r_sunflare_min_size"),
             values.at("r_sunflare_max_size"), values.at("r_sunblind_max_darken"),
             values.at("r_sunglare_max_lighten"));
    return {{"spriteSize", values.at("r_sunsprite_size")},
            {"flareMinSize", values.at("r_sunflare_min_size")},
            {"flareMinDot", dot("r_sunflare_min_angle")},
            {"flareMaxSize", values.at("r_sunflare_max_size")},
            {"flareMaxDot", dot("r_sunflare_max_angle")},
            {"flareMaxAlpha", values.at("r_sunflare_max_alpha")},
            {"flareFadeInTime", ms("r_sunflare_fadein")},
            {"flareFadeOutTime", ms("r_sunflare_fadeout")},
            {"blindMinDot", dot("r_sunblind_min_angle")},
            {"blindMaxDot", dot("r_sunblind_max_angle")},
            {"blindMaxDarken", values.at("r_sunblind_max_darken")},
            {"blindFadeInTime", ms("r_sunblind_fadein")},
            {"blindFadeOutTime", ms("r_sunblind_fadeout")},
            {"glareMinDot", dot("r_sunglare_min_angle")},
            {"glareMaxDot", dot("r_sunglare_max_angle")},
            {"glareMaxLighten", values.at("r_sunglare_max_lighten")},
            {"glareFadeInTime", ms("r_sunglare_fadein")},
            {"glareFadeOutTime", ms("r_sunglare_fadeout")}};
}

// Replay's killstreak scripts read stock template entities at load (getent;
// an undefined one errors when the streak runs): care packages clone
// care_package_col into each crate, deployable cover clones tactical_cover_col
// and white phosphorus moves white_phosphorus_damage to each strike. IW3 maps
// have none. Shipped maps park them outside play; these sit below the world,
// inside the broadphase. Sizes: the Replay crate and offhand_wm_deployable_cover
// bounds, and mp_shipment's white phosphorus volume (1504 x 512 x 500).
void AddReplayTemplateEntities(CollisionData &collision, Json &render, std::string &entities,
                               const Vec3 &minimum, const Vec3 &maximum, const float floor)
{
    auto &brushModels = render.at("brushModels");
    if (brushModels.size() != collision.models.size())
        throw std::runtime_error("IW3 render and collision brush-model counts differ");
    struct Box
    {
        const char *targetname;
        Vec3 low;
        Vec3 high;
    };
    const std::array<Box, 2> boxes{{
        {"care_package_col", {-26.0f, -26.0f, 0.0f}, {26.0f, 26.0f, 55.0f}},
        {"tactical_cover_col", {-5.0f, -30.0f, 0.0f}, {5.0f, 30.0f, 49.0f}},
    }};
    const auto surfaceCount = render.at("surfaces").size();
    const float y = (minimum[1] + maximum[1]) * 0.5f;
    float x = minimum[0] + 256.0f;
    const auto originText = [](const Vec3 &origin) {
        char text[96];
        std::snprintf(text, sizeof(text), "%.1f %.1f %.1f", origin[0], origin[1], origin[2]);
        return std::string(text);
    };
    for (const Box &box : boxes)
    {
        const Vec3 origin{x, y, floor};
        x += 256.0f;
        Vec3 low{}, high{};
        std::vector<Vec4> planes;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            low[axis] = origin[axis] + box.low[axis];
            high[axis] = origin[axis] + box.high[axis];
            Vec4 lower{};
            lower[axis] = -1.0f;
            lower[3] = -low[axis];
            planes.push_back(lower);
            Vec4 upper{};
            upper[axis] = 1.0f;
            upper[3] = high[axis];
            planes.push_back(upper);
        }
        const auto modelIndex = static_cast<std::uint32_t>(collision.models.size());
        collision.models.push_back({box.low, box.high});
        CollisionHull hull;
        hull.points = HullFromPlanes(planes);
        if (hull.points.empty())
            throw std::runtime_error("Replay template collision box is empty");
        hull.contents = 1;
        hull.model = modelIndex;
        collision.hulls.push_back(std::move(hull));
        brushModels.push_back(
            {{"firstSurface", surfaceCount}, {"surfaceCount", 0}, {"bounds", {low, high}}});
        entities += "{\n\"classname\" \"script_brushmodel\"\n\"targetname\" \"" +
                    std::string(box.targetname) + "\"\n\"origin\" \"" + originText(origin) +
                    "\"\n\"model\" \"*" + std::to_string(modelIndex) + "\"\n}\n";
    }
    // A radius trigger needs no brush model and moves like the stock volume.
    entities += "{\n\"classname\" \"trigger_radius\"\n\"targetname\" \"white_phosphorus_damage\"\n"
                "\"origin\" \"" +
                originText({x + 512.0f, y, floor}) + "\"\n\"radius\" \"500\"\n\"height\" \"500\"\n}\n";
    zt::info("iw3: added Replay template entities care_package_col, tactical_cover_col and "
             "white_phosphorus_damage below the world");
}

PreparedMap PrepareFastfile(const ImportOptions &options)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(options.fastfile, error) ||
        options.fastfile.extension() != ".ff")
    {
        throw std::runtime_error("IW3 input must be a readable .ff file");
    }

    PreparedMap result;
    result.scratch = MakeScratchDirectory(options.scratchRoot);
    const auto extracted = result.scratch / "extracted";
    result.root = result.scratch / "prepared";
    std::filesystem::create_directories(extracted);
    std::filesystem::create_directories(result.root);

    const auto unlinker = FindUnlinker(options.unlinker);
    const auto cacheDirectory = ExtractionCacheDirectory(unlinker, options);
    const bool cached = !std::getenv("IW8_ZONETOOL_NO_EXTRACT_CACHE") &&
                        std::filesystem::is_regular_file(cacheDirectory / "complete", error);
    if (cached)
    {
        zt::info("iw3: reusing cached extraction %s", cacheDirectory.string().c_str());
        std::filesystem::copy(cacheDirectory / "extracted", extracted,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing);
    }
    else
    {
        zt::info("iw3: reading %s with %s", options.fastfile.string().c_str(),
                 unlinker.string().c_str());
        RunUnlinker(unlinker, options, extracted);
    }

    const auto worldPath = FindSingleExport(extracted, ".replay-world.json");
    const auto collisionPath = FindSingleExport(extracted, ".replay-collision.json");
    const auto commonWorldPath = FindSingleExport(extracted, ".replay-comworld.json");
    const auto exportRoot = worldPath.parent_path().parent_path().parent_path();
    const std::string sourceAsset = worldPath.filename().string().substr(
        0, worldPath.filename().string().size() - std::string(".replay-world.json").size());
    constexpr std::string_view bspSuffix = ".d3dbsp";
    const std::string sourceMap = sourceAsset.ends_with(bspSuffix)
                                      ? sourceAsset.substr(0, sourceAsset.size() - bspSuffix.size())
                                      : sourceAsset;
    const Json world = ReadJson(worldPath);
    const Json collision = ReadJson(collisionPath);
    const Json commonWorld = ReadJson(commonWorldPath);
    if (world.at("schema") != 1 || collision.at("schema") != 1 || commonWorld.at("schema") != 1)
    {
        throw std::runtime_error("unsupported ReplayMapDumpers export schema");
    }
    if (world.at("name").get<std::string>() != collision.at("name").get<std::string>() ||
        world.at("name").get<std::string>() != commonWorld.at("name").get<std::string>())
    {
        throw std::runtime_error(
            "IW3 world, collision, and common-world exports name different maps");
    }
    const auto &sourceLights = commonWorld.at("primary_lights");
    const auto primaryLightCount = world.at("primary_light_count").get<std::size_t>();
    const auto sunPrimaryLightIndex = world.at("sun_primary_light_index").get<std::size_t>();
    if (!sourceLights.is_array() || sourceLights.size() != primaryLightCount ||
        primaryLightCount < 2 || primaryLightCount > 65535 ||
        sunPrimaryLightIndex >= primaryLightCount)
    {
        throw std::runtime_error("invalid IW3 primary-light table");
    }

    std::string entities;
    const auto sourceEntities = worldPath.parent_path() / (sourceAsset + ".ents");
    if (!zt::read_file_str(sourceEntities.string(), entities) || entities.empty())
    {
        throw std::runtime_error("IW3 map entity export is missing");
    }

    entities = WithIw3ModeAssets(entities, options.iw3ModeAssets);
    {
        std::string mapScript;
        const auto scriptPath =
            worldPath.parent_path() /
            (std::filesystem::path(sourceAsset).stem().string() + ".gsc");
        zt::read_file_str(scriptPath.string(), mapScript);
        entities = WithIw3CtfObjectives(entities, mapScript);
    }
    const auto entityModelNames = convert::iw3ReplayEntityModels(entities);
    auto missingEntityModels = MissingEntityModels(exportRoot, entityModelNames);
    if (!cached && !missingEntityModels.empty())
    {
        // A direct conversion may be given an already-extracted IW3 asset pack
        // instead of a full installation containing common_mp.ff.  Stage only
        // the entity models the map names, then retain the normal common-zone
        // fallback for ordinary IW3 installations.
        CopyExtractedEntityModels(exportRoot, missingEntityModels, options.searchPaths);
        missingEntityModels = MissingEntityModels(exportRoot, entityModelNames);
    }
    if (!missingEntityModels.empty())
    {
        const auto commonFastfile = FindIw3CommonFastfile(options);
        if (commonFastfile.empty())
        {
            throw std::runtime_error(
                "IW3 entity model assets are absent from the map fastfile (" +
                JoinNames(missingEntityModels) +
                "); add the IW3 installation root or zone/english directory with --search-path");
        }
        zt::info("iw3: resolving %zu entity model asset(s) from %s", missingEntityModels.size(),
                 commonFastfile.string().c_str());
        RunUnlinker(unlinker, options, exportRoot, commonFastfile, true);
        missingEntityModels = MissingEntityModels(exportRoot, entityModelNames);
        if (!missingEntityModels.empty())
        {
            throw std::runtime_error("IW3 entity model assets were not found in common_mp.ff: " +
                                     JoinNames(missingEntityModels));
        }
    }

    // The ordinary world extraction can omit map-local FX payloads. Read those
    // from the map fastfile first, then resolve only still-missing declarations
    // from the shared multiplayer zone.
    if (!cached)
    {
        RunUnlinker(unlinker, options, exportRoot, options.fastfile, true);
        CopyExtractedFx(exportRoot, options.searchPaths);
        CopyExtractedFxMaterials(exportRoot, options.searchPaths);
    }
    const auto missingFx = MissingDeclaredFx(exportRoot);
    if (!cached && !missingFx.empty())
    {
        const auto commonFastfile = FindIw3CommonFastfile(options);
        if (!commonFastfile.empty())
        {
            zt::info("iw3: resolving %zu shared source FX graph(s) from %s",
                     missingFx.size(), commonFastfile.string().c_str());
            RunUnlinker(unlinker, options, exportRoot, commonFastfile, true);
        }
    }
    if (const auto presets = MissingPhysicsPresets(exportRoot); !cached && !presets.empty())
    {
        const auto commonFastfile = FindIw3CommonFastfile(options);
        if (!commonFastfile.empty())
        {
            zt::info("iw3: resolving %zu shared physics preset(s) from %s", presets.size(),
                     commonFastfile.string().c_str());
            RunUnlinker(unlinker, options, exportRoot, commonFastfile, true);
        }
    }
    if (const auto presets = MissingPhysicsPresets(exportRoot); !presets.empty())
        zt::warn("iw3: %zu physics preset(s) have no exported payload; their models stay "
                 "static: %s",
                 presets.size(), JoinNames(presets).c_str());
    const auto unresolvedFx = MissingDeclaredFx(exportRoot);
    if (!unresolvedFx.empty())
        zt::warn("iw3: %zu declared source FX graph(s) have no exported payload; first: %s",
                 unresolvedFx.size(), unresolvedFx.front().c_str());
    if (!cached)
    {
        std::error_code cacheError;
        std::filesystem::remove_all(cacheDirectory, cacheError);
        std::filesystem::create_directories(cacheDirectory, cacheError);
        if (!cacheError)
            std::filesystem::copy(extracted, cacheDirectory / "extracted",
                                  std::filesystem::copy_options::recursive, cacheError);
        if (!cacheError && zt::write_file_str((cacheDirectory / "complete").string(), "1"))
            zt::info("iw3: cached extraction at %s", cacheDirectory.string().c_str());
        else
            zt::warn("iw3: could not cache the extraction: %s", cacheError.message().c_str());
    }

    // Entity model skeletons: a second, model-only export in XMODEL_EXPORT
    // form, kept beside the cached extraction.
    const auto skeletonRoot = std::getenv("IW8_ZONETOOL_NO_EXTRACT_CACHE")
                                  ? result.scratch / "skeleton"
                                  : cacheDirectory / "skeleton";
    if (!std::filesystem::is_regular_file(skeletonRoot / "complete", error))
    {
        std::error_code skeletonError;
        std::filesystem::remove_all(skeletonRoot, skeletonError);
        std::filesystem::create_directories(skeletonRoot);
        zt::info("iw3: exporting model skeletons from %s", options.fastfile.string().c_str());
        RunUnlinker(unlinker, options, skeletonRoot, options.fastfile, false, true);
        std::vector<std::string> unexported;
        for (const auto &model : entityModelNames)
            if (!std::filesystem::is_regular_file(
                    SafeChild(skeletonRoot, std::filesystem::path("xmodel") / (model + ".json")),
                    skeletonError))
                unexported.push_back(model);
        const auto commonFastfile = FindIw3CommonFastfile(options);
        if (!unexported.empty() && !commonFastfile.empty())
        {
            zt::info("iw3: exporting %zu entity model skeleton(s) from %s", unexported.size(),
                     commonFastfile.string().c_str());
            RunUnlinker(unlinker, options, skeletonRoot, commonFastfile, false, true);
        }
        zt::write_file_str((skeletonRoot / "complete").string(), "1");
    }

    // A map-local IWD beside the fastfile (mp_discovery_night.iwd) holds the
    // map's loose images, vision file and sounds; the zone only names them.
    if (const auto count = sound::ExtractIwdMembers(options.fastfile.parent_path(),
                                                    {"images/", "vision/", "sound/"}, exportRoot))
        zt::info("iw3: extracted %zu image, vision and sound file(s) from the map IWD(s)", count);
    if (const auto preview = BuildLoadscreenPreview(exportRoot, options.searchPaths, sourceMap);
        !preview.empty())
    {
        std::filesystem::create_directories(result.root);
        if (!zt::write_file((result.root / "preview.rgba").string(), preview))
            throw std::runtime_error("cannot write the loading-screen preview");
    }
    result.rawFiles = ReadPreparedRawFiles(exportRoot, sourceMap);
    // setExpFog and the vision film grade become Replay's vision fog, a grading
    // CLUT and (below) the converted surface shader's fog.
    const auto expFog = ReadMapExpFog(exportRoot, sourceMap);
    // Both styles grade with the map's IW3 film (Nuketown's warm brown tint):
    // the owner wants --style iw8 to carry it too (2026-10-14).
    if (const auto film = ReadMapFilm(exportRoot, sourceMap))
    {
        result.gradingClutName = "mw120r_" + options.map + "_clut";
        result.gradingClutPixels = BuildGradingClutPixels(*film);
    }
    {
        const std::string vision = BuildReplayVision(expFog, result.gradingClutName, options.style,
                                                     SunDirection(world, entities));
        result.rawFiles.push_back(
            {"vision/" + options.map + ".vision",
             std::vector<std::uint8_t>(vision.begin(), vision.end())});
        zt::info("iw3: converted vision (%s lighting, fog %s, grading CLUT %s)",
                 options.style == MapStyle::iw8 ? "iw8" : "iw3", expFog ? "yes" : "no",
                 result.gradingClutName.empty() ? "none" : result.gradingClutName.c_str());
    }
    result.fxEffects = ReadPreparedFx(exportRoot);
    auto declaredFx = ReadDeclaredFx(exportRoot / "zone_source" / (sourceMap + ".zone"));
    // Effect names the map scripts hold as plain literals (loaded at runtime
    // through iw3compat_loadfx).
    std::set<std::string> literalFx;
    for (const char *folder : {"mp", "createfx", "createart"})
        for (const auto &name : ReadScriptEffectLiterals(exportRoot / "maps" / folder))
            if (std::ranges::any_of(result.fxEffects,
                                    [&](const PreparedFx &effect) { return effect.name == name; }))
            {
                literalFx.insert(name);
                declaredFx.insert(name);
            }
    {
        // Script-driven IW3 effects: literals the map scripts load, and the
        // common-zone effects maps\mp\_destructible_types plays on vehicles.
        std::set<std::string> scriptFx = ReadScriptEffectNames(exportRoot / "maps" / "mp");
        const bool hasDestructibleVehicles = std::ranges::any_of(
            ParseEntityFields(entities), [](const EntityFields &entity) {
                const auto type = entity.find("destructible_type");
                return type != entity.end() && type->second.find("vehicle") != std::string::npos;
            });
        if (hasDestructibleVehicles)
            for (const char *name : kIw3VehicleDestructibleEffects)
                scriptFx.insert(name);
        for (const auto *entry : UsedGlobalFx(entities))
            scriptFx.insert(entry->effect);
        std::size_t commonImpactFx = 0;
        for (const auto &name : ReadImpactEffectNames(exportRoot))
            if (!declaredFx.contains(name) &&
                std::ranges::any_of(result.fxEffects,
                                    [&](const PreparedFx &effect) { return effect.name == name; }))
            {
                declaredFx.insert(name);
                ++commonImpactFx;
            }
        if (commonImpactFx)
            zt::info("iw3: %zu impact-table effect(s) not declared by the map zone convert from common_mp",
                     commonImpactFx);
        for (const auto &name : scriptFx)
        {
            if (declaredFx.contains(name))
                continue;
            if (std::ranges::none_of(result.fxEffects,
                                     [&](const PreparedFx &effect) { return effect.name == name; }))
            {
                zt::warn("iw3: script effect %s has no extracted IW3 FX graph", name.c_str());
                continue;
            }
            declaredFx.insert(name);
        }
    }
    std::set<std::string> reachableFx(declaredFx.begin(), declaredFx.end());
    std::vector<std::string> pendingFx(declaredFx.begin(), declaredFx.end());
    for (std::size_t index = 0; index < pendingFx.size(); ++index)
    {
        const auto &name = pendingFx[index];
        const auto graph = std::find_if(result.fxEffects.begin(), result.fxEffects.end(),
                                        [&name](const PreparedFx &effect) {
                                            return effect.name == name;
                                        });
        if (graph == result.fxEffects.end())
            throw std::runtime_error("map-reachable IW3 FX graph is missing: " + name +
                                     "; add the matching IW3 zone with --search-path");
        for (const auto &dependency : graph->dependencies)
            if (dependency.type == "fx" && reachableFx.insert(dependency.name).second)
                pendingFx.push_back(dependency.name);
    }
    std::erase_if(result.fxEffects, [&reachableFx](const PreparedFx &effect) {
        return !reachableFx.contains(effect.name);
    });
    std::array<std::size_t, 11> fxElementTypes{};
    std::size_t fxElementCount = 0;
    for (const auto &effect : result.fxEffects)
        for (const auto &element : effect.elements)
        {
            ++fxElementTypes.at(element.type);
            ++fxElementCount;
        }
    zt::info("iw3: validated %zu element(s) in %zu map-reachable source FX graph(s) from "
             "%zu declaration(s)",
             fxElementCount, result.fxEffects.size(), declaredFx.size());
    zt::info("iw3: FX elements sprite=%zu oriented=%zu tail=%zu trail=%zu cloud=%zu model=%zu "
             "omni=%zu spot=%zu sound=%zu decal=%zu runner=%zu",
             fxElementTypes[0], fxElementTypes[1], fxElementTypes[2], fxElementTypes[3],
             fxElementTypes[4], fxElementTypes[5], fxElementTypes[6], fxElementTypes[7],
             fxElementTypes[8], fxElementTypes[9], fxElementTypes[10]);
    std::vector<std::string> fxDependencyModels;
    for (const auto &effect : result.fxEffects)
        for (const auto &dependency : effect.dependencies)
            if (dependency.type == "xmodel")
                fxDependencyModels.push_back(dependency.name);
    std::ranges::sort(fxDependencyModels);
    fxDependencyModels.erase(
        std::unique(fxDependencyModels.begin(), fxDependencyModels.end()), fxDependencyModels.end());
    CopyExtractedEntityModels(exportRoot, MissingEntityModels(exportRoot, fxDependencyModels),
                              options.searchPaths);
    std::set<std::string> fxMaterialNames;
    for (const auto &effect : result.fxEffects)
        for (const auto &dependency : effect.dependencies)
            if (dependency.type == "material")
                fxMaterialNames.insert(dependency.name);
    std::vector<std::string> missingFxMaterials;
    for (const auto &name : fxMaterialNames)
    {
        std::error_code materialError;
        if (!std::filesystem::is_regular_file(exportRoot / "materials" / (name + ".json"),
                                              materialError))
            missingFxMaterials.push_back(name);
    }
    if (!missingFxMaterials.empty())
        throw std::runtime_error("map-reachable IW3 FX material source is missing: " +
                                 JoinNames(missingFxMaterials) +
                                 "; add the matching IW3 main and zone directories with --search-path");
    zt::info("iw3: verified %zu distinct FX material source export(s)",
             fxMaterialNames.size());
    bool localLinearLightDef = false;
    for (std::size_t index = 0; index < sourceLights.size(); ++index)
        if (index != sunPrimaryLightIndex &&
            sourceLights.at(index).value("definition", std::string{}) == "light_point_linear")
            localLinearLightDef = true;
    if (localLinearLightDef)
    {
        std::string sourceLightDef;
        const auto path = exportRoot / "lights" / "light_point_linear";
        std::string expected(1, static_cast<char>(0x62));
        expected.append("falloff_linear", 14);
        expected.push_back('\0');
        if (!zt::read_file_str(path.string(), sourceLightDef) || sourceLightDef != expected)
            throw std::runtime_error(
                "map-local IW3 light_point_linear needs its exact attenuation export; "
                "add the matching IW3 zone with --search-path");
        zt::info("iw3: matched map-local light_point_linear to Replay's native type-34 definition");
    }
    const auto glassFx = std::find_if(result.fxEffects.begin(), result.fxEffects.end(),
                                      [](const PreparedFx &effect) {
                                          return effect.name == "impacts/small_glass";
                                      });
    if (glassFx != result.fxEffects.end())
    {
        std::vector<std::string> models;
        for (const auto &dependency : glassFx->dependencies)
            if (dependency.type == "xmodel")
                models.push_back(dependency.name);
        const auto missing = MissingEntityModels(exportRoot, models);
        zt::info("iw3: small_glass has %zu source dependency(s), %zu model shard(s), %zu missing model export(s)",
                 glassFx->dependencies.size(), models.size(), missing.size());
        if (!missing.empty())
            zt::warn("iw3: small_glass shard model export missing: %s", JoinNames(missing).c_str());
    }

    zt::LapTimer timing("iw3");
    const VisibilityGroups visibility = ReadVisibilityGroups(world);
    SourceStaticModels sourceStaticModels;
    std::vector<BrushModel> brushModels =
        ReadBrushModels(world, exportRoot, visibility, sourceStaticModels);
    SourceDynamicEntities sourceDynamicEntities = ReadDynamicEntities(collision, exportRoot);
    // IW3 map scripts: translate them first so the entities they drive stay
    // entities, and so destroyed-state models can be converted and precached.
    std::set<std::string> scriptStrings;
    std::vector<std::string> scriptModels;
    {
        std::vector<scripts::Source> scriptSources;
        // The map scripts, and the createfx (effect placement) and createart
        // scripts the map's main calls.
        for (const char *folder : {"mp", "createfx", "createart"})
        {
            const auto scriptDirectory = exportRoot / "maps" / folder;
            std::error_code scriptError;
            if (!std::filesystem::is_directory(scriptDirectory, scriptError))
                continue;
            for (const auto &entry : std::filesystem::directory_iterator(scriptDirectory))
            {
                if (!entry.is_regular_file() || Lowercase(entry.path().extension().string()) != ".gsc")
                    continue;
                std::string text;
                if (zt::read_file_str(entry.path().string(), text))
                    scriptSources.push_back({std::string("maps/") + folder + "/" +
                                                 Lowercase(entry.path().filename().string()),
                                             std::move(text)});
            }
        }
        // Destroyed states of IW3 destructibles (<type>_destroyed, <model>_d).
        // Custom types (Broadcast2's cust_vehicle_80s_sedan1_green) name the
        // destroyed model after the vehicle model instead:
        // vehicle_80s_sedan1_green_destructible_mp -> vehicle_80s_sedan1_green_destroyed.
        std::set<std::string> extraModels;
        std::map<std::string, std::string> destroyedModels; // destructible_type -> model
        const auto exportedModel = [&](const std::string &model) {
            std::error_code missing;
            return std::filesystem::is_regular_file(exportRoot / "xmodel" / (model + ".json"), missing);
        };
        for (const EntityFields &entity : ParseEntityFields(entities))
        {
            const auto type = entity.find("destructible_type");
            const auto name = entity.find("targetname");
            const auto model = entity.find("model");
            if (type != entity.end())
            {
                std::vector<std::string> candidates{type->second + "_destroyed"};
                if (model != entity.end())
                    for (const std::string suffix : {"_destructible_mp", "_destructible"})
                        if (model->second.ends_with(suffix))
                        {
                            candidates.push_back(
                                model->second.substr(0, model->second.size() - suffix.size()) +
                                "_destroyed");
                            break;
                        }
                for (const auto &candidate : candidates)
                    if (exportedModel(candidate))
                    {
                        extraModels.insert(candidate);
                        destroyedModels.emplace(type->second, candidate);
                        break;
                    }
            }
            if (name != entity.end() && name->second == "interactive_tv" && model != entity.end())
                extraModels.insert(model->second + "_d");
        }
        std::string generated = "iw3compat_generated()\n{\n    level.mw120r_models = [];\n"
                                "    level.mw120r_destroyed = [];\n";
        for (const auto &[type, model] : destroyedModels)
            generated += "    level.mw120r_destroyed[\"" + type + "\"] = \"" + model + "\";\n";
        for (const auto &model : extraModels)
        {
            std::error_code missing;
            if (!std::filesystem::is_regular_file(exportRoot / "xmodel" / (model + ".json"), missing))
                continue;
            scriptModels.push_back(model);
            generated += "    precachemodel( \"" + model + "\" );\n    level.mw120r_models[\"" +
                         model + "\"] = 1;\n";
        }
        {
            // The play space for iw3compat_out_of_bounds: the minimap region
            // (or the world without its sky) plus a margin, from below the
            // lowest geometry to well above the highest. Past it Replay's
            // visibility pass indexed a light bitset with a negative index
            // (crash at 0x141959080+0x71A when falling or flying out; IW8 maps
            // kill out-of-bounds players before that).
            Vec3 low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
            for (const auto &surface : world.at("surfaces"))
            {
                if (surface.value("material", std::string{}).find("sky") != std::string::npos)
                    continue;
                for (const auto &vertex : surface.at("vertices"))
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        const float value = vertex.at("position").at(axis).get<float>();
                        low[axis] = std::min(low[axis], value);
                        high[axis] = std::max(high[axis], value);
                    }
            }
            convert::CompassBounds compass;
            if (convert::iw3CompassBounds(entities, compass))
            {
                low[0] = std::min(compass.northwestX, compass.southeastX);
                high[0] = std::max(compass.northwestX, compass.southeastX);
                low[1] = std::min(compass.northwestY, compass.southeastY);
                high[1] = std::max(compass.northwestY, compass.southeastY);
            }
            if (low[0] <= high[0] && low[2] <= high[2])
            {
                char bounds[256];
                std::snprintf(bounds, sizeof(bounds),
                              "    level.mw120r_playspace_min = ( %.0f, %.0f, %.0f );\n"
                              "    level.mw120r_playspace_max = ( %.0f, %.0f, %.0f );\n",
                              low[0] - 1024, low[1] - 1024, low[2] - 512, high[0] + 1024,
                              high[1] + 1024, high[2] + 2048);
                generated += bounds;
            }
        }
        generated += "}\n";
        try
        {
            // The IW3 source name, which can differ from the Replay map id
            // (mp_discovery_night built as mp_discovery).
            auto translation = scripts::Translate(
                sourceMap, scriptSources,
                [](const std::string &effect) { return std::string(kScriptEffectMarker) + effect; },
                generated);
            for (const auto &note : translation.notes)
                zt::info("iw3: script translation: %s", note.c_str());
            zt::info("iw3: translated %zu IW3 map script file(s) to IW8 GSC (%s main)",
                     scriptSources.size(), translation.translatedMain ? "source" : "generated");
            scriptStrings = std::move(translation.strings);
            result.mapScript = std::move(translation.gsc);
        }
        catch (const std::exception &error)
        {
            zt::warn("iw3: map scripts were not translated: %s", error.what());
        }
    }
    AddPromotedStaticModels(world, exportRoot, TakeUnscriptedScriptModels(entities, scriptStrings),
                            sourceStaticModels);
    timing.lap("read world");
    SourceStaticModels sourceEntityModels;
    auto keptEntityModelNames = convert::iw3ReplayEntityModels(entities);
    for (const auto &model : scriptModels)
        if (std::find(keptEntityModelNames.begin(), keptEntityModelNames.end(), model) ==
            keptEntityModelNames.end())
            keptEntityModelNames.push_back(model);
    sourceEntityModels.models.reserve(keptEntityModelNames.size());
    for (const auto &modelName : keptEntityModelNames)
        sourceEntityModels.models.push_back(
            ReadSourceModel(exportRoot, modelName, false, skeletonRoot));
    SourceStaticModels sourceFxModels;
    std::set<std::string> fxModelNames;
    for (const auto &effect : result.fxEffects)
        for (const auto &dependency : effect.dependencies)
            if (dependency.type == "xmodel")
                fxModelNames.insert(dependency.name);
    for (const auto &model : sourceEntityModels.models)
        fxModelNames.erase(model.name);
    sourceFxModels.models.reserve(fxModelNames.size());
    for (const auto &name : fxModelNames)
        sourceFxModels.models.push_back(ReadSourceModel(exportRoot, name));
    std::vector<std::string> materialNames;
    std::vector<std::string> worldMaterialNames;
    std::vector<std::string> modelMaterialNames;
    std::vector<std::string> entityMaterialNames;
    std::vector<std::string> shadowWorldMaterialNames;
    std::vector<std::string> shadowModelMaterialNames;
    for (const Surface &surface : brushModels.front().surfaces)
        shadowWorldMaterialNames.push_back(surface.material);
    for (const SourceStaticModel &model : sourceStaticModels.models)
        if (!model.lods.empty())
            for (const Surface &surface : model.lods.front().surfaces)
                shadowModelMaterialNames.push_back(surface.material);
    for (const BrushModel &model : brushModels)
        for (const Surface &surface : model.surfaces)
        {
            materialNames.push_back(surface.material);
            worldMaterialNames.push_back(surface.material);
        }
    for (const SourceStaticModel &model : sourceStaticModels.models)
        for (const SourceStaticModelLod &lod : model.lods)
            for (const Surface &surface : lod.surfaces)
            {
                materialNames.push_back(surface.material);
                modelMaterialNames.push_back(surface.material);
            }
    for (const SourceStaticModels *models :
         {&sourceDynamicEntities.models, &sourceEntityModels, &sourceFxModels})
        for (const SourceStaticModel &model : models->models)
            for (const SourceStaticModelLod &lod : model.lods)
                for (const Surface &surface : lod.surfaces)
                {
                    materialNames.push_back(surface.material);
                    entityMaterialNames.push_back(surface.material);
                }
    timing.lap("read models");
    const auto mapDirectory = result.root / "maps" / "mp";
    std::filesystem::create_directories(mapDirectory);
    const std::vector<std::string> fxMaterials(fxMaterialNames.begin(), fxMaterialNames.end());
    // Entities (script models, dynamic entities, FX models) read the IW3 light
    // grid from a world-space volume, as IW3 lit them from the grid.
    std::optional<EntityLightVolume> entityLightVolume;
    if (const auto volume = BuildLightGridVolume(exportRoot, sourceAsset))
    {
        entityLightVolume.emplace();
        entityLightVolume->image = "mw120r/" + options.map + "_light_volume";
        entityLightVolume->file = options.map + "_light_volume.rgba";
        entityLightVolume->width = volume->width;
        entityLightVolume->height = volume->height;
        if (!zt::write_file((mapDirectory / entityLightVolume->file).string(), volume->pixels))
            throw std::runtime_error("cannot write the entity light-grid volume");
        char defines[512];
        std::snprintf(defines, sizeof(defines),
                      "#define MAP_LIGHT_VOLUME 1\n"
                      "#define MAP_LIGHT_VOLUME_ORIGIN float3(%.6g, %.6g, %.6g)\n"
                      "#define MAP_LIGHT_VOLUME_CELL float3(%.6g, %.6g, %.6g)\n"
                      "#define MAP_LIGHT_VOLUME_DIMS float3(%u, %u, %u)\n"
                      "#define MAP_LIGHT_VOLUME_TILES %u\n"
                      "#define MAP_LIGHT_VOLUME_SIZE float2(%u, %u)\n",
                      volume->origin[0], volume->origin[1], volume->origin[2], volume->cell[0],
                      volume->cell[1], volume->cell[2], volume->dims[0], volume->dims[1],
                      volume->dims[2], volume->tilesPerRow, volume->width, volume->height);
        entityLightVolume->defines = defines;
    }
    RenderPlan renderPlan =
        PrepareRenderAssets(exportRoot, world, materialNames, worldMaterialNames,
                            modelMaterialNames, entityMaterialNames, fxMaterials, shadowWorldMaterialNames,
                            shadowModelMaterialNames, options.searchPaths, mapDirectory,
                            options.map, options.scratchRoot / ".iw3-render-cache",
                            FogShaderDefines(expFog, options.style, ReadMapSun(exportRoot, sourceMap)),
                            static_cast<unsigned>(sourceStaticModels.instances.size()),
                            entityLightVolume ? &*entityLightVolume : nullptr);
    timing.lap("render assets");
    result.fxMaterialAliases = renderPlan.fxMaterialAliases;
    for (auto &effect : result.fxEffects)
    {
        // IW3 heat-haze sprites draw with a stock Replay distortion material
        // and the stock distortion particle state. Every shipped Replay
        // distortion emitter is a sprite (element type 0); distortion on other
        // IW3 element types has no Replay counterpart and is left out.
        std::size_t distortionSprites = 0;
        const std::size_t removed = std::erase_if(effect.elements, [&](PreparedFxElement &element) {
            const bool distortion =
                std::ranges::any_of(element.visuals, [&](const PreparedFxVisual &visual) {
                    return visual.kind == PreparedFxVisualKind::material &&
                           visual.names.size() == 1 &&
                           renderPlan.fxDistortionMaterials.contains(visual.names.front());
                });
            element.distortion = distortion && element.type == 0;
            distortionSprites += element.distortion ? 1 : 0;
            return distortion && element.type != 0;
        });
        if (distortionSprites)
            zt::info("iw3: FX %s: %zu distortion sprite(s) use a stock Replay distortion material",
                     effect.name.c_str(), distortionSprites);
        if (removed)
            zt::info("iw3: FX %s: left out %zu non-sprite distortion element(s)", effect.name.c_str(),
                     removed);
        for (auto &element : effect.elements)
        {
            // Shipped Replay impact states exist for model particles (debris)
            // and sprites (shell-eject smoke, blood); distortion sprites keep
            // their distortion state.
            if (!element.effectOnImpact.empty() && element.type != 5 &&
                !(element.type == 0 && !element.distortion))
            {
                zt::info("iw3: FX %s: element type %u impact effect %s not spawned",
                         effect.name.c_str(), static_cast<unsigned>(element.type),
                         element.effectOnImpact.c_str());
                element.effectOnImpact.clear();
            }
        }
    }
    BuildPreparedStaticModels(sourceStaticModels, renderPlan, options.map, "smodel", result.xmodels,
                              result.staticModels, true, false, false,
                              &result.staticModelCollisionModels);
    if (renderPlan.instanceLightingCount)
    {
        // IW3 lit each static model from its light grid at the model's
        // lighting origin (the bounds centre); one ambient cube per placement,
        // in placement order (the GfxWorld instance index the shader reads).
        if (renderPlan.instanceLightingCount != result.staticModels.instances.size())
            throw std::runtime_error("static-model lighting count does not match the placements");
        std::vector<std::array<float, 3>> origins;
        origins.reserve(result.staticModels.instances.size());
        for (const auto &instance : result.staticModels.instances)
        {
            const auto &mid = result.staticModels.models.at(instance.model).bounds.midpoint;
            const auto &q = instance.quaternion;
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            const float rotation[3][3] = {
                {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
            std::array<float, 3> origin = instance.origin;
            for (unsigned row = 0; row < 3; ++row)
                for (unsigned column = 0; column < 3; ++column)
                    origin[row] += rotation[row][column] * mid[column] * instance.scale;
            origins.push_back(origin);
        }
        // Alpha of face 0: IW3's baked sun visibility for the instance.
        std::vector<float> sunVisibility;
        auto cubes = SampleLightGridAmbientCubes(
            exportRoot, sourceAsset, origins,
            world.at("sun_primary_light_index").get<unsigned>(), &sunVisibility);
        constexpr unsigned columns = 256;
        const unsigned rows = (renderPlan.instanceLightingCount + columns - 1) / columns;
        std::vector<std::uint8_t> pixels(std::size_t(columns) * 6 * rows * 4, 0);
        for (std::size_t index = 0; index < origins.size(); ++index)
            for (unsigned face = 0; face < 6; ++face)
            {
                // Values / 2 in RGBA8 UNORM (IW3 grid shading values peak near 1).
                const std::size_t texel = (index / columns) * columns * 6 + (index % columns) * 6 + face;
                for (unsigned channel = 0; channel < 3; ++channel)
                {
                    const float value = cubes.empty() ? 0.31f : cubes[index][face][channel];
                    pixels[texel * 4 + channel] = static_cast<std::uint8_t>(
                        std::lround(std::clamp(value * 0.5f, 0.0f, 1.0f) * 255.0f));
                }
                // Alpha of face 1: the instance's IW3 reflection probe tag
                // (converted slot + 1, 0 for none).
                pixels[texel * 4 + 3] =
                    face == 0 && index < sunVisibility.size()
                        ? static_cast<std::uint8_t>(
                              std::lround(std::clamp(sunVisibility[index], 0.0f, 1.0f) * 255.0f))
                    : face == 1
                        ? static_cast<std::uint8_t>(ReflectionProbeTag(
                              sourceStaticModels.instances.at(index).reflectionProbe,
                              world.at("reflection_probes").size()))
                        : 255;
            }
        if (cubes.empty())
            zt::warn("iw3: no IW3 light grid; static models use a flat 0.31 ambient");
        if (!zt::write_file((mapDirectory / renderPlan.instanceLightingFile).string(), pixels))
            throw std::runtime_error("cannot write the static-model lighting image");
    }
    result.shadowScene = std::move(renderPlan.shadowScene);
    BuildShadowScene(brushModels, sourceStaticModels, renderPlan, result.shadowScene);
    {
        // Baked sun visibility for every converted receiver (world, static
        // models, entities) over the play space: the minimap region plus a
        // margin, at every height a caster there reaches. Native shadow
        // cascades drop casters outside their range and view; this does not.
        float minX = -16384, maxX = 16384, minY = -16384, maxY = 16384;
        convert::CompassBounds compass;
        if (convert::iw3CompassBounds(entities, compass))
        {
            constexpr float margin = 512;
            minX = std::min(compass.northwestX, compass.southeastX) - margin;
            maxX = std::max(compass.northwestX, compass.southeastX) + margin;
            minY = std::min(compass.northwestY, compass.southeastY) - margin;
            maxY = std::max(compass.northwestY, compass.southeastY) + margin;
        }
        float minZ = INFINITY, maxZ = -INFINITY;
        for (const auto &surface : result.shadowScene.surfaces)
            for (const auto &vertex : surface.vertices)
            {
                const auto &p = vertex.position;
                if (p[0] >= minX && p[0] <= maxX && p[1] >= minY && p[1] <= maxY &&
                    std::abs(p[2]) <= 100000.0f)
                {
                    minZ = std::min(minZ, p[2]);
                    maxZ = std::max(maxZ, p[2]);
                }
            }
        replaysunshadow::DepthImage depth;
        if (minZ <= maxZ)
        {
            replaybounds::Bounds region;
            region.midpoint = {(minX + maxX) * .5f, (minY + maxY) * .5f, (minZ + maxZ) * .5f};
            region.halfSize = {(maxX - minX) * .5f, (maxY - minY) * .5f, (maxZ - minZ) * .5f + 64};
            const Vec3 sun = Unit(SunDirection(world, entities));
            depth = replaysunshadow::BakeDepthImage(result.shadowScene, {sun[0], sun[1], sun[2]},
                                                    region);
            zt::info("iw3: baked sun depth %ux%u at %.3g units per texel over %.0f..%.0f, "
                     "%.0f..%.0f, %.0f..%.0f",
                     depth.width, depth.height, depth.texelSize, minX, maxX, minY, maxY, minZ, maxZ);
        }
        if (depth.rgba.empty())
        {
            // No caster in the play space: an image whose header marks it unbaked.
            depth.rgba.assign(std::size_t(replaysunshadow::DepthImageWidth) *
                                  replaysunshadow::DepthImageHeight * 4,
                              0);
            zt::warn("iw3: no sun shadow casters in the play space; baked sun depth is empty");
        }
        if (!zt::write_file((mapDirectory / renderPlan.sunDepthFile).string(), depth.rgba))
            throw std::runtime_error("cannot write the baked sun depth image");
        timing.lap("baked sun depth");
    }
    replayrender::StaticModels unusedDynamicTables;
    BuildPreparedStaticModels(sourceDynamicEntities.models, renderPlan, options.map, "dynmodel",
                              result.xmodels, unusedDynamicTables, true, true);
    BuildPreparedStaticModels(sourceEntityModels, renderPlan, options.map, "entitymodel",
                              result.xmodels, unusedDynamicTables, true, true, true, nullptr, true);
    replayrender::StaticModels unusedFxTables;
    BuildPreparedStaticModels(sourceFxModels, renderPlan, options.map, "fxmodel",
                              result.xmodels, unusedFxTables, true, true, true);
    zt::info("iw3: converted %zu FX-referenced XModels through native model/material closure",
             sourceFxModels.models.size());
    timing.lap("prepare models");
    const auto effectAliases = BuildFxAliases(result, options.map);
    const bool hasOmniLights = std::ranges::any_of(
        result.fxEffects, [&](const PreparedFx &effect) {
            return effectAliases.contains(effect.name) &&
                   std::ranges::any_of(effect.elements, [](const PreparedFxElement &element) {
                       return element.type == 6;
                   });
        });
    if (hasOmniLights)
        result.vfxLightDef = "mw120r/" + options.map + "/light_fx_default";
    result.vfxEffects.reserve(effectAliases.size());
    std::unordered_map<std::string, const PreparedFx *> convertibleEffects;
    convertibleEffects.reserve(effectAliases.size());
    for (const auto &effect : result.fxEffects)
    {
        if (effectAliases.contains(effect.name))
            convertibleEffects.emplace(effect.name, &effect);
    }
    std::unordered_map<std::string, std::uint8_t> effectStates;
    effectStates.reserve(effectAliases.size());
    const auto appendEffect = [&](auto &&self, const PreparedFx &effect) -> void {
        auto &state = effectStates[effect.name];
        if (state == 2)
            return;
        if (state == 1)
            throw std::runtime_error("IW3 FX graph contains a child dependency cycle: " +
                                     effect.name);
        state = 1;
        for (const auto &element : effect.elements)
        {
            if (element.type == 10)
            {
                for (const auto &visual : element.visuals)
                {
                    const auto child = convertibleEffects.find(visual.names.front());
                    if (child == convertibleEffects.end())
                        throw std::runtime_error("IW3 runner child effect was not converted: " +
                                                 visual.names.front());
                    self(self, *child->second);
                }
            }
            if (!element.effectOnImpact.empty())
            {
                const auto child = convertibleEffects.find(element.effectOnImpact);
                if (child == convertibleEffects.end())
                    throw std::runtime_error("IW3 impact child effect was not converted: " +
                                             element.effectOnImpact);
                self(self, *child->second);
            }
            if (!element.effectEmitted.empty())
            {
                const auto child = convertibleEffects.find(element.effectEmitted);
                if (child == convertibleEffects.end())
                    throw std::runtime_error("IW3 birth child effect was not converted: " +
                                             element.effectEmitted);
                self(self, *child->second);
            }
            if (!element.effectOnDeath.empty())
            {
                const auto child = convertibleEffects.find(element.effectOnDeath);
                if (child == convertibleEffects.end())
                    throw std::runtime_error("IW3 death child effect was not converted: " +
                                             element.effectOnDeath);
                self(self, *child->second);
            }
        }
        result.vfxEffects.push_back(
            ConvertFx(result, effect, effectAliases, result.vfxLightDef));
        if (effect.name == "impacts/small_glass")
            result.smallGlassEffect = effectAliases.at(effect.name);
        state = 2;
    };
    for (const auto &effect : result.fxEffects)
        if (effectAliases.contains(effect.name))
            appendEffect(appendEffect, effect);
    zt::info("iw3: converted %zu of %zu map-reachable source FX graph(s) to native Replay VFX",
             result.vfxEffects.size(), result.fxEffects.size());
    if (!result.smallGlassEffect.empty())
        zt::info("iw3: converted impacts/small_glass to native Replay VFX '%s'",
                 result.smallGlassEffect.c_str());
    // --style iw8 keeps Replay's own surface impact effects.
    if (options.style == MapStyle::iw3)
        result.impactOverrides = ReadImpactOverrides(exportRoot, options.searchPaths, effectAliases);
    zt::info("iw3: mapped %zu native IW3 impact slots into Replay's impact table",
             static_cast<std::size_t>(std::ranges::count(result.impactOverrides, std::size_t{0},
                                                         &iw8::impact::EffectOverride::pack)));
    // Sound aliases the map's scripts play, plus the IW3 vehicle destructible
    // sounds the compatibility layer plays itself.
    std::map<std::string, bool> soundAliases;
    {
        std::set<std::string> requestedSounds = sound::ReadScriptSoundNames(exportRoot / "maps" / "mp");
        for (const char *folder : {"createfx", "createart"})
            for (const auto &name : sound::ReadScriptSoundNames(exportRoot / "maps" / folder))
                requestedSounds.insert(name);
        const bool hasDestructibleVehicles = std::ranges::any_of(
            ParseEntityFields(entities), [](const EntityFields &entity) {
                const auto type = entity.find("destructible_type");
                return type != entity.end() && type->second.find("vehicle") != std::string::npos;
            });
        if (hasDestructibleVehicles)
            for (const char *name : kIw3VehicleDestructibleSounds)
                requestedSounds.insert(name);
        for (const auto *entry : UsedGlobalFx(entities))
            if (entry->sound)
                requestedSounds.insert(entry->sound);
        std::set<std::string> literals;
        for (const char *folder : {"mp", "createfx", "createart"})
            literals.merge(sound::ReadScriptStringLiterals(exportRoot / "maps" / folder));
        if (auto sounds = sound::Convert(exportRoot, options.searchPaths, options.map, requestedSounds,
                                         literals))
        {
            soundAliases = std::move(sounds->aliases);
            result.soundBank = std::move(sounds->bank);
        }
        zt::info("iw3: converted %zu of %zu requested IW3 sound alias(es)", soundAliases.size(),
                 requestedSounds.size());
    }
    if (!result.mapScript.empty())
    {
        // Translated loadfx( "<marker><iw3 name>" ) calls load the converted
        // effect, or a stock Replay stand-in when the graph did not convert.
        // --style iw8 plays the stock Replay effect wherever one has the IW3
        // effect's role (the converted graph stays for converted parents).
        const auto scriptEffect = [&](const std::string &name) -> std::optional<std::string> {
            if (options.style == MapStyle::iw8)
                return scripts::Iw8StyleEffectFor(name);
            return std::nullopt;
        };
        std::string &gsc = result.mapScript;
        for (std::size_t at = gsc.find(kScriptEffectMarker); at != std::string::npos;
             at = gsc.find(kScriptEffectMarker, at))
        {
            const std::size_t end = gsc.find('"', at);
            if (end == std::string::npos)
                throw std::runtime_error("translated script has an unterminated effect name");
            const std::string name =
                gsc.substr(at + kScriptEffectMarker.size(), end - at - kScriptEffectMarker.size());
            const auto alias = effectAliases.find(name);
            const std::string replacement = scriptEffect(name).value_or(
                alias != effectAliases.end() ? alias->second : scripts::StockEffectFor(name));
            zt::info("iw3: script effect %s -> %s", name.c_str(), replacement.c_str());
            gsc.replace(at, end - at, replacement);
            at += replacement.size();
        }
        // iw3compat_iw3fx(): IW3 effects the compatibility layer plays itself
        // (destructible vehicles), keyed by IW3 name.
        gsc += "\niw3compat_iw3fx()\n{\n    level.mw120r_iw3fx = [];\n"
               "    level.mw120r_iw3fx_stock = [];\n";
        // --style iw8: the compatibility layer plays stock effects upright.
        if (options.style == MapStyle::iw8)
            gsc += "    level.mw120r_iw8_fx = 1;\n";
        std::vector<const char *> compatEffects(std::begin(kIw3VehicleDestructibleEffects),
                                                std::end(kIw3VehicleDestructibleEffects));
        compatEffects.insert(compatEffects.end(), std::begin(kIw3CompatPropEffects),
                             std::end(kIw3CompatPropEffects));
        const auto globalFx = UsedGlobalFx(entities);
        for (const auto *entry : globalFx)
            compatEffects.push_back(entry->effect);
        std::string globalFxGsc;
        // iw3compat_global_fx(): IW3 _global_fx placements as literals (Replay
        // scripts here have no struct lookup); played like createfx one-shots
        // with IW3's random -20..-15 s start offset and loop sound.
        {
            std::string placements;
            std::size_t placed = 0;
            for (const auto &entity : ParseEntityFields(entities))
            {
                const auto classname = entity.find("classname");
                const auto targetname = entity.find("targetname");
                const auto origin = entity.find("origin");
                if (classname == entity.end() || classname->second != "script_struct" ||
                    targetname == entity.end() || origin == entity.end())
                    continue;
                const auto entry = std::ranges::find_if(globalFx, [&](const Iw3GlobalFx *candidate) {
                    return targetname->second == candidate->targetname;
                });
                if (entry == globalFx.end() || !effectAliases.contains((*entry)->effect))
                    continue;
                const auto angles = entity.find("angles");
                const auto vector = [](const std::string &text) {
                    Vec3 value{};
                    std::sscanf(text.c_str(), "%f %f %f", &value[0], &value[1], &value[2]);
                    char buffer[96];
                    std::snprintf(buffer, sizeof(buffer), "( %g, %g, %g )", value[0], value[1],
                                  value[2]);
                    return std::string(buffer);
                };
                placements += "    iw3compat_global_fx_one( \"" + std::string((*entry)->effect) +
                              "\", " + vector(origin->second) + ", " +
                              (angles == entity.end() ? std::string("( 0, 0, 0 )")
                                                      : vector(angles->second)) +
                              ", \"" + ((*entry)->sound ? (*entry)->sound : "") + "\" );\n";
                ++placed;
            }
            globalFxGsc = "\niw3compat_global_fx()\n{\n" + placements + "}\n";
            if (placed)
                zt::info("iw3: placed %zu IW3 _global_fx effect(s)", placed);
        }
        const auto compatEffect = [&](const std::string &name) -> std::optional<std::string> {
            if (const auto stock = scriptEffect(name))
                return stock;
            if (const auto alias = effectAliases.find(name); alias != effectAliases.end())
                return alias->second;
            return std::nullopt;
        };
        // level.mw120r_iw3fx_stock marks slots holding a stock Replay effect
        // (--style iw8); the vehicle code plays those in the vehicle's frame.
        for (const char *name : compatEffects)
            if (const auto effect = compatEffect(name))
            {
                gsc += "    level.mw120r_iw3fx[\"" + std::string(name) + "\"] = loadfx( \"" +
                       *effect + "\" );\n";
                if (scriptEffect(name))
                    gsc += "    level.mw120r_iw3fx_stock[\"" + std::string(name) + "\"] = 1;\n";
            }
        for (const auto &name : literalFx)
        {
            const auto effect = compatEffect(name);
            if (!effect ||
                std::ranges::any_of(compatEffects, [&](const char *known) { return name == known; }))
                continue;
            gsc += "    level.mw120r_iw3fx[\"" + name + "\"] = loadfx( \"" + *effect + "\" );\n";
        }
        gsc += "}\n" + globalFxGsc;
        // iw3compat_structs(): the map's keyed script_structs for IW3
        // getstructarray() (Replay scripts cannot read IW3 structs from MapEnts).
        {
            std::string structs;
            std::size_t count = 0;
            const auto text = [](std::string value) {
                std::erase_if(value, [](const char c) { return c == '"' || c == '\\'; });
                return "\"" + value + "\"";
            };
            const auto vector = [](const std::string &value) {
                Vec3 parsed{};
                std::sscanf(value.c_str(), "%f %f %f", &parsed[0], &parsed[1], &parsed[2]);
                char buffer[96];
                std::snprintf(buffer, sizeof(buffer), "( %g, %g, %g )", parsed[0], parsed[1],
                              parsed[2]);
                return std::string(buffer);
            };
            for (const auto &entity : ParseEntityFields(entities))
            {
                const auto field = [&](const char *key) {
                    const auto found = entity.find(key);
                    return found == entity.end() ? std::string{} : found->second;
                };
                if (field("classname") != "script_struct" ||
                    (field("targetname").empty() && field("target").empty() &&
                     field("script_noteworthy").empty() && field("script_linkname").empty()))
                    continue;
                structs += "    iw3compat_struct_add( " + vector(field("origin")) + ", " +
                           vector(field("angles")) + ", " + text(field("targetname")) + ", " +
                           text(field("target")) + ", " + text(field("script_noteworthy")) + ", " +
                           text(field("script_linkname")) + " );\n";
                ++count;
            }
            gsc += "\niw3compat_structs()\n{\n" + structs + "}\n";
            if (count)
                zt::info("iw3: %zu IW3 script_struct(s) available to getstructarray", count);
        }
        // iw3compat_sounds(): converted aliases, 1 = playsound, 2 = playloopsound.
        gsc += "\niw3compat_sounds()\n{\n    level.mw120r_sounds = [];\n";
        for (const auto &[name, looping] : soundAliases)
            gsc += "    level.mw120r_sounds[\"" + name + "\"] = " + (looping ? "2" : "1") + ";\n";
        gsc += "}\n";
        // iw3compat_model_parts(): each skinned entity model's bones with
        // geometry and their bind-pose boxes in model space (LOD0), which the
        // compatibility layer uses to route hits to IW3 destructible parts.
        gsc += "\niw3compat_model_parts()\n{\n    level.mw120r_part_min = [];\n"
               "    level.mw120r_part_max = [];\n";
        const auto vector = [](const Vec3 &value) {
            char text[96]{};
            std::snprintf(text, sizeof(text), "( %.3f, %.3f, %.3f )", value[0], value[1], value[2]);
            return std::string(text);
        };
        for (const SourceStaticModel &model : sourceEntityModels.models)
        {
            if (model.bones.size() < 2 || model.lods.empty())
                continue;
            std::vector<replaybounds::Accumulator> boxes(model.bones.size());
            std::vector<bool> used(model.bones.size());
            for (const Surface &surface : model.lods.front().surfaces)
                for (std::size_t vertex = 0;
                     vertex < surface.vertices.size() && vertex < surface.vertexBones.size(); ++vertex)
                {
                    boxes[surface.vertexBones[vertex]].Add(surface.vertices[vertex].position);
                    used[surface.vertexBones[vertex]] = true;
                }
            const std::string key = "[\"" + model.name + "\"]";
            gsc += "    level.mw120r_part_min" + key + " = [];\n    level.mw120r_part_max" + key +
                   " = [];\n";
            for (std::size_t bone = 0; bone < model.bones.size(); ++bone)
            {
                if (!used[bone])
                    continue;
                const replaybounds::Bounds box = boxes[bone].Finish();
                const std::string tag = "[\"" + model.bones[bone].name + "\"]";
                gsc += "    level.mw120r_part_min" + key + tag + " = " +
                       vector(Subtract(box.midpoint, box.halfSize)) + ";\n";
                gsc += "    level.mw120r_part_max" + key + tag + " = " +
                       vector(Add(box.midpoint, box.halfSize)) + ";\n";
            }
        }
        gsc += "}\n";
    }
    std::set<std::string> emittedFx;
    for (const auto &entry : effectAliases)
        emittedFx.insert(entry.first);
    std::set<std::string> emittedRawFiles;
    for (const auto &rawFile : result.rawFiles)
        emittedRawFiles.insert(rawFile.name);
    AuditSourceAssetDeclarations(exportRoot, emittedFx, emittedRawFiles, localLinearLightDef);
    result.dynamicEntities.reserve(sourceDynamicEntities.definitionModels.size() +
                                   sourceDynamicEntities.brushes.size());
    for (std::size_t index = 0; index < sourceDynamicEntities.definitionModels.size(); ++index)
    {
        const std::size_t modelIndex = sourceDynamicEntities.definitionModels[index];
        const auto &model = sourceDynamicEntities.models.models.at(modelIndex);
        const bool clutter = model.clutter.has_value();
        const bool noPhysics = model.collisionLod == SIZE_MAX && !clutter;
        iw8::DynamicEntity entity{"mw120r/" + options.map + "/dynmodel_" + std::to_string(modelIndex),
                                  sourceDynamicEntities.quaternions[index],
                                  sourceDynamicEntities.origins[index], noPhysics};
        if (clutter)
        {
            entity.rigidBodies = 1;
            entity.collisionFlags = 2;
        }
        result.dynamicEntities.push_back(std::move(entity));
    }
    for (const auto &source : sourceDynamicEntities.brushes)
    {
        iw8::DynamicEntity entity;
        entity.quaternion = source.quaternion;
        entity.origin = source.origin;
        entity.noPhysics = false;
        entity.basis = iw8::DynamicEntityBasis::brush;
        entity.brushModel = source.model;
        result.dynamicEntities.push_back(std::move(entity));
    }
    timing.lap("fx and audits");
    std::size_t triangles = 0;
    auto nativeCollision = ReadCollision(collision);
    result.ladderRungDistance =
        AlignLadderEdgesToModels(nativeCollision, sourceStaticModels, renderPlan);
    AddStairRamps(nativeCollision);
    std::set<std::uint32_t> nativeGlassModels;
    std::set<std::string> nativeGlassTargets;
    Json render = BuildRender(world, visibility, brushModels, renderPlan, entities,
                                    sourceStaticModels,
                                    result.staticModels, nativeCollision, triangles,
                                    nativeGlassModels, nativeGlassTargets, sourceEntityModels);
    timing.lap("collision read and render json");
    for (const auto &brush : sourceDynamicEntities.brushes)
        if (nativeGlassModels.contains(brush.model))
            throw std::runtime_error("IW3 scripted glass model is also a dynamic brush");
    {
        // IW3 window frames carry player clips (clip_nosight) across the
        // opening that its GSC never removed. Native glass blocks players
        // while intact and frees the opening when shattered, so drop
        // non-solid world clips overlapping a pane to allow mantling through.
        std::vector<std::pair<Vec3, Vec3>> paneBoxes;
        for (const auto &hull : nativeCollision.hulls)
        {
            if (!hull.glassId || hull.points.empty())
                continue;
            Vec3 low = hull.points.front(), high = hull.points.front();
            for (const auto &point : hull.points)
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    low[axis] = std::min(low[axis], point[axis]);
                    high[axis] = std::max(high[axis], point[axis]);
                }
            paneBoxes.emplace_back(low, high);
        }
        const std::size_t windowClips = std::erase_if(nativeCollision.hulls, [&](const CollisionHull &hull) {
            if (hull.glassId || hull.model != 0 || (hull.contents & 1u) || hull.points.empty() ||
                !(hull.contents & 0x10000u))
                return false;
            Vec3 low = hull.points.front(), high = hull.points.front();
            for (const auto &point : hull.points)
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    low[axis] = std::min(low[axis], point[axis]);
                    high[axis] = std::max(high[axis], point[axis]);
                }
            return std::ranges::any_of(paneBoxes, [&](const auto &pane) {
                for (std::size_t axis = 0; axis < 3; ++axis)
                    if (high[axis] < pane.first[axis] - 1.0f || low[axis] > pane.second[axis] + 1.0f)
                        return false;
                return true;
            });
        });
        if (windowClips)
            zt::info("iw3: removed %zu window player clip hull(s) covering native glass",
                     windowClips);
    }
    const std::size_t glassHullCount =
        std::erase_if(nativeCollision.hulls, [&](const CollisionHull &hull) {
            return hull.model != 0 && nativeGlassModels.contains(hull.model);
        });
    const std::size_t glassMeshCount =
        std::erase_if(nativeCollision.meshes, [&](const CollisionMesh &mesh) {
            return mesh.model != 0 && nativeGlassModels.contains(mesh.model);
        });
    if (!nativeGlassModels.empty())
    {
        // Render-only (nonsolid) glass brushes have no source collision; their
        // panes got box hulls when they were built (mp_agroprom).
        zt::info("iw3: removed %zu obsolete scripted glass hull(s) and %zu mesh(es); "
                 "numbered world panes own collision",
                 glassHullCount, glassMeshCount);
    }
    {
        // Bot navigation source: world collision as players meet it (Replay
        // player trace mask 0x2014011), solid world and static models for
        // sight, plus the IW3 pathnode and spawn markers.
        auto &navigation = result.navigation;
        constexpr std::uint32_t playerMask = 0x2014011u;
        const auto navigationFlags = [](const std::uint32_t contents) {
            std::uint32_t flags = 0;
            if (contents & playerMask)
                flags |= iw8::navigation::kBlocksMovement;
            if (contents & 1u)
                flags |= iw8::navigation::kBlocksSight;
            return flags;
        };
        for (const auto &hull : nativeCollision.hulls)
            if (hull.model == 0 && hull.points.size() >= 4)
                if (const auto flags = navigationFlags(hull.contents))
                    navigation.hulls.push_back({hull.points, flags});
        for (const auto &mesh : nativeCollision.meshes)
        {
            if (mesh.model != 0)
                continue;
            for (const auto &triangle : mesh.triangles)
                if (const auto flags = navigationFlags(triangle.contents))
                    navigation.triangles.push_back({{mesh.vertices.at(triangle.indices[0]),
                                                     mesh.vertices.at(triangle.indices[1]),
                                                     mesh.vertices.at(triangle.indices[2])},
                                                    flags});
        }
        // Static models with a collision LOD collide through Replay's
        // static-model collision list (contents 0xB180, not in the player mask).
        for (const auto &instance : sourceStaticModels.instances)
        {
            const SourceStaticModel &model = sourceStaticModels.models.at(instance.model);
            if (model.collisionLod == SIZE_MAX)
                continue;
            const auto axis = StaticModelAxis(instance);
            for (const Surface &surface : model.lods.at(model.collisionLod).surfaces)
                for (std::size_t first = 0; first + 2 < surface.indices.size(); first += 3)
                {
                    iw8::navigation::Triangle triangle;
                    for (std::size_t corner = 0; corner < 3; ++corner)
                        triangle.corners[corner] = Add(
                            instance.origin,
                            Multiply(Transform(axis,
                                               surface.vertices.at(surface.indices[first + corner])
                                                   .position),
                                     instance.scale));
                    triangle.flags =
                        iw8::navigation::kBlocksMovement | iw8::navigation::kBlocksSight;
                    navigation.triangles.push_back(triangle);
                }
        }
        for (const EntityFields &entity : ParseEntityFields(entities))
        {
            const auto classname = entity.find("classname");
            const auto origin = entity.find("origin");
            if (classname == entity.end() || origin == entity.end())
                continue;
            std::string name = classname->second;
            std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (name == "node_pathnode")
                navigation.pathNodes.push_back(ParseVector(origin->second));
            else if (name.starts_with("mp_") && name.find("_spawn") != std::string::npos)
                navigation.spawns.push_back(ParseVector(origin->second));
        }
        zt::info("iw3: navigation source has %zu hulls, %zu triangles, %zu pathnode markers and "
                 "%zu spawns",
                 navigation.hulls.size(), navigation.triangles.size(),
                 navigation.pathNodes.size(), navigation.spawns.size());
    }
    result.footsteps = result.scratch / "footsteps.native";
    WriteFootsteps(result.footsteps, brushModels, renderPlan);

    const std::string targetAsset = options.map + ".d3dbsp";
    WriteJson(mapDirectory / (targetAsset + ".lighting.json"),
              BuildLighting(world, commonWorld, entities));
    timing.lap("render json write");
    PrepareNativeLightGrid(exportRoot, sourceAsset,
                           mapDirectory / (targetAsset + ".gpulightgrid.native"));
    timing.lap("light grid");

    const auto worldBounds = world.at("bounds");
    if (!worldBounds.is_array() || worldBounds.size() != 2)
    {
        throw std::runtime_error("invalid IW3 world bounds");
    }
    Vec3 minimum = ReadVector<3>(worldBounds.at(0));
    Vec3 maximum = ReadVector<3>(worldBounds.at(1));
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        minimum[axis] -= 64.0f;
        maximum[axis] += 64.0f;
    }
    const float templateFloor = minimum[2] - 1200.0f;
    AddReplayTemplateEntities(nativeCollision, render, entities, minimum, maximum, templateFloor);
    minimum[2] = templateFloor - 64.0f;
    render["sunFlare"] = ReadSunFlare(exportRoot, sourceMap);
    WriteJson(mapDirectory / (targetAsset + ".render.json"), render);
    WriteJson(mapDirectory / (targetAsset + ".bounds.json"),
              {{"schema", 1}, {"min", minimum}, {"max", maximum}});

    const std::string convertedEntities = WithoutUnscriptedWindowStates(
        WithoutLegacyGlassEntities(entities, nativeGlassModels, nativeGlassTargets));
    if (!zt::write_file_str((mapDirectory / (targetAsset + ".ents")).string(), convertedEntities))
    {
        throw std::runtime_error("cannot write temporary IW3 entities");
    }
    // Build-time probe: IW8_ZONETOOL_COLLISION_PROBE="x,y" logs every world
    // hull and mesh triangle above that point, as the baker receives them.
    if (const char *probe = std::getenv("IW8_ZONETOOL_COLLISION_PROBE"))
    {
        float px = 0.0f, py = 0.0f;
        if (std::sscanf(probe, "%f,%f", &px, &py) == 2)
        {
            for (const CollisionHull &hull : nativeCollision.hulls)
            {
                Vec3 low{FLT_MAX, FLT_MAX, FLT_MAX}, high{-FLT_MAX, -FLT_MAX, -FLT_MAX};
                for (const Vec3 &point : hull.points)
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        low[axis] = std::min(low[axis], point[axis]);
                        high[axis] = std::max(high[axis], point[axis]);
                    }
                if (px >= low[0] && px <= high[0] && py >= low[1] && py <= high[1])
                    zt::info("collision probe: hull model %u contents 0x%X glass %u z %.2f..%.2f "
                             "points %zu slabs %zu",
                             hull.model, hull.contents, hull.glassId, low[2], high[2],
                             hull.points.size(), hull.slabs.size());
            }
            for (const CollisionMesh &mesh : nativeCollision.meshes)
                for (const auto &triangle : mesh.triangles)
                {
                    const Vec3 &a = mesh.vertices[triangle.indices[0]];
                    const Vec3 &b = mesh.vertices[triangle.indices[1]];
                    const Vec3 &c = mesh.vertices[triangle.indices[2]];
                    const auto side = [&](const Vec3 &p, const Vec3 &q) {
                        return (q[0] - p[0]) * (py - p[1]) - (q[1] - p[1]) * (px - p[0]);
                    };
                    const float d0 = side(a, b), d1 = side(b, c), d2 = side(c, a);
                    if ((d0 < 0 || d1 < 0 || d2 < 0) && (d0 > 0 || d1 > 0 || d2 > 0))
                        continue;
                    const Vec3 normal = Cross(Subtract(b, a), Subtract(c, a));
                    const float z = std::abs(normal[2]) > 1.0e-6f
                                        ? a[2] - (normal[0] * (px - a[0]) + normal[1] * (py - a[1])) /
                                                     normal[2]
                                        : a[2];
                    zt::info("collision probe: mesh model %u triangle contents 0x%X material %u "
                             "z %.2f",
                             mesh.model, triangle.contents, triangle.material, z);
                }
        }
    }
    result.collision = result.scratch / "collision.native";
    WriteCollision(result.collision, nativeCollision);
    CopyCompass(extracted, result.root, sourceMap, options.map);
    timing.lap("entities and collision write");

    zt::info("iw3: normalized %zu triangles, %zu collision hulls, %zu brush models, %zu static "
             "models and %zu dynamic models",
             triangles, nativeCollision.hulls.size(), nativeCollision.models.size(),
             sourceStaticModels.instances.size(), result.dynamicEntities.size());
    zt::info("iw3: preserved %zu source materials and %zu authored lightmaps",
             renderPlan.materials.size(), renderPlan.lightmaps.size());
    std::size_t portalCount = 0;
    for (const auto &cell : world.at("dpvs").at("cells"))
        portalCount += cell.at("portals").size();
    zt::info(
        "iw3: merged %zu DPVS cells, %zu AABB trees (%zu populated), %zu planes and %zu portals "
        "into one Replay cell",
        visibility.cellTrees.size(), visibility.sourceTreeCount, visibility.treeBounds.size(),
        world.at("dpvs").at("planes").size(), portalCount);
    return result;
}
} // namespace iw3
