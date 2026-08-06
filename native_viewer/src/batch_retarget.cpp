#include "skrtg/viewer/batch_retarget.h"

#include "skrtg/viewer/review_scene.h"
#include "skrtg/viewer/verified_export.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace skrtg::viewer
{
namespace
{
constexpr const char* ExternalRequestSchema =
    "skrtg.native_viewer.batch_retarget_request.v1";
constexpr const char* UEIKJsonRequestSchema =
    "skrtg.native_viewer.batch_retarget_request.v2";
constexpr const char* ProfileCatalogRequestSchema =
    "skrtg.native_viewer.batch_retarget_request.v3";
constexpr const char* ProfileCatalogOperationRequestSchema =
    "skrtg.native_viewer.batch_retarget_request.v4";
constexpr const char* LegacyStatusSchema =
    "skrtg.native_viewer.batch_retarget_status.v1";
constexpr const char* ProfileCatalogStatusSchema =
    "skrtg.native_viewer.batch_retarget_status.v3";
constexpr const char* ProfileCatalogOperationStatusSchema =
    "skrtg.native_viewer.batch_retarget_status.v4";
constexpr const char* ProfileCatalogStatusSchemaV2 =
    "skrtg.native_viewer.batch_retarget_status.v2";
constexpr const char* ExternalDefinitionKind = "external_foundation_v1";
constexpr const char* UEIKJsonDefinitionKind = "ue_ik_json_v1";
constexpr std::size_t MaximumPlannedFiles = 100000;

bool IsRegularFile(const std::filesystem::path& Path)
{
    std::error_code Error;
    return !Path.empty() &&
        std::filesystem::is_regular_file(Path, Error) && !Error;
}

bool IsDirectory(const std::filesystem::path& Path)
{
    std::error_code Error;
    return !Path.empty() &&
        std::filesystem::is_directory(Path, Error) && !Error;
}

std::string LowerAscii(std::string Value)
{
    std::transform(
        Value.begin(), Value.end(), Value.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::tolower(Character));
        });
    return Value;
}

bool HasFbxExtension(const std::filesystem::path& Path)
{
    return LowerAscii(Path.extension().string()) == ".fbx";
}

bool HasJsonExtension(const std::filesystem::path& Path)
{
    return LowerAscii(Path.extension().string()) == ".json";
}

bool IsSha256(const std::string& Value)
{
    return Value.size() == 64 &&
        std::all_of(
            Value.begin(), Value.end(),
            [](const unsigned char Character)
            {
                return std::isxdigit(Character) != 0;
            });
}

std::filesystem::path NormalizedAbsolute(
    const std::filesystem::path& Path)
{
    std::error_code Error;
    std::filesystem::path Result =
        std::filesystem::weakly_canonical(Path, Error);
    if (Error)
    {
        Error.clear();
        Result = std::filesystem::absolute(Path, Error);
    }
    return (Error ? Path : Result).lexically_normal();
}

std::string ComparablePath(const std::filesystem::path& Path)
{
    std::string Result = PathToUtf8(NormalizedAbsolute(Path));
#if defined(_WIN32)
    Result = LowerAscii(std::move(Result));
#endif
    while (Result.size() > 1 && Result.back() == '/') Result.pop_back();
    return Result;
}

bool SamePath(
    const std::filesystem::path& Left,
    const std::filesystem::path& Right)
{
    return !Left.empty() && !Right.empty() &&
        ComparablePath(Left) == ComparablePath(Right);
}

bool ResolveForContainment(
    const std::filesystem::path& Path,
    std::filesystem::path& OutResolved)
{
    std::error_code Error;
    std::filesystem::path Cursor =
        std::filesystem::absolute(Path, Error).lexically_normal();
    if (Error || Cursor.empty()) return false;
    std::vector<std::filesystem::path> MissingSuffix;
    for (;;)
    {
        const bool Exists = std::filesystem::exists(Cursor, Error);
        if (Error) return false;
        if (Exists) break;
        const std::filesystem::path Parent = Cursor.parent_path();
        if (Parent.empty() || Parent == Cursor) return false;
        MissingSuffix.push_back(Cursor.filename());
        Cursor = Parent;
    }
    OutResolved = std::filesystem::canonical(Cursor, Error);
    if (Error || OutResolved.empty()) return false;
    for (auto Part = MissingSuffix.rbegin();
         Part != MissingSuffix.rend(); ++Part)
    {
        OutResolved /= *Part;
    }
    OutResolved = OutResolved.lexically_normal();
    return true;
}

bool IsPathInsideOrEqual(
    const std::filesystem::path& Candidate,
    const std::filesystem::path& Root,
    bool& OutResolved)
{
    std::filesystem::path CandidatePath;
    std::filesystem::path RootPath;
    OutResolved = ResolveForContainment(Candidate, CandidatePath) &&
        ResolveForContainment(Root, RootPath);
    if (!OutResolved) return false;
    auto CandidatePart = CandidatePath.begin();
    for (auto RootPart = RootPath.begin(); RootPart != RootPath.end();
         ++RootPart, ++CandidatePart)
    {
        if (CandidatePart == CandidatePath.end()) return false;
        std::string Left = PathToUtf8(*CandidatePart);
        std::string Right = PathToUtf8(*RootPart);
#if defined(_WIN32)
        Left = LowerAscii(std::move(Left));
        Right = LowerAscii(std::move(Right));
#endif
        if (Left != Right) return false;
    }
    return true;
}

bool DirectoryEmptyOrAbsent(const std::filesystem::path& Path)
{
    std::error_code Error;
    if (!std::filesystem::exists(Path, Error)) return !Error;
    if (Error || !std::filesystem::is_directory(Path, Error) || Error)
        return false;
    return std::filesystem::directory_iterator(Path, Error) ==
            std::filesystem::directory_iterator() && !Error;
}

bool CreateUniqueTemporaryText(
    const std::filesystem::path& Destination,
    const std::string& Text,
    std::filesystem::path& OutTemporary,
    std::string& OutError)
{
    static std::atomic<std::uint64_t> Counter{0};
    const std::uint64_t Tick = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#if defined(_WIN32)
    const std::uint64_t ProcessId = GetCurrentProcessId();
#else
    const std::uint64_t ProcessId = static_cast<std::uint64_t>(getpid());
#endif
    for (std::uint64_t Attempt = 0; Attempt < 32; ++Attempt)
    {
        OutTemporary = Destination;
        OutTemporary += ".tmp." + std::to_string(ProcessId) + "." +
            std::to_string(Tick) + "." +
            std::to_string(Counter.fetch_add(1)) + "." +
            std::to_string(Attempt);
#if defined(_WIN32)
        HANDLE File = CreateFileW(
            OutTemporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (File == INVALID_HANDLE_VALUE)
        {
            const DWORD FileError = GetLastError();
            if (FileError == ERROR_FILE_EXISTS ||
                FileError == ERROR_ALREADY_EXISTS)
            {
                continue;
            }
            OutError = "failed to create exclusive temporary JSON: win32=" +
                std::to_string(FileError);
            return false;
        }
        bool Success = true;
        std::size_t Offset = 0;
        while (Offset < Text.size())
        {
            const DWORD Chunk = static_cast<DWORD>(std::min<std::size_t>(
                Text.size() - Offset, 1U << 30U));
            DWORD Written = 0;
            if (!WriteFile(
                    File, Text.data() + Offset, Chunk, &Written, nullptr) ||
                Written != Chunk)
            {
                Success = false;
                break;
            }
            Offset += Written;
        }
        if (Success) Success = FlushFileBuffers(File) != FALSE;
        const DWORD WriteError = Success ? ERROR_SUCCESS : GetLastError();
        CloseHandle(File);
        if (!Success)
        {
            std::error_code CleanupError;
            std::filesystem::remove(OutTemporary, CleanupError);
            OutError = "failed to write temporary JSON: win32=" +
                std::to_string(WriteError);
            return false;
        }
#else
        const int File = open(
            OutTemporary.c_str(), O_CREAT | O_EXCL | O_WRONLY
#if defined(O_CLOEXEC)
                | O_CLOEXEC
#endif
#if defined(O_NOFOLLOW)
                | O_NOFOLLOW
#endif
            , 0600);
        if (File < 0)
        {
            if (errno == EEXIST) continue;
            OutError = std::string(
                "failed to create exclusive temporary JSON: ") +
                std::strerror(errno);
            return false;
        }
        bool Success = true;
        std::size_t Offset = 0;
        while (Offset < Text.size())
        {
            const ssize_t Written = write(
                File, Text.data() + Offset, Text.size() - Offset);
            if (Written <= 0)
            {
                Success = false;
                break;
            }
            Offset += static_cast<std::size_t>(Written);
        }
        const int WriteError = Success ? 0 : errno;
        close(File);
        if (!Success)
        {
            std::error_code CleanupError;
            std::filesystem::remove(OutTemporary, CleanupError);
            OutError = std::string("failed to write temporary JSON: ") +
                std::strerror(WriteError);
            return false;
        }
#endif
        return true;
    }
    OutError = "failed to allocate a unique temporary JSON path";
    return false;
}

bool WriteTextAtomic(
    const std::filesystem::path& Path,
    const std::string& Text,
    std::string& OutError)
{
    OutError.clear();
    std::error_code Error;
    if (Path.has_parent_path())
        std::filesystem::create_directories(Path.parent_path(), Error);
    if (Error)
    {
        OutError = "failed to create JSON output directory";
        return false;
    }
    std::filesystem::path Temporary;
    if (!CreateUniqueTemporaryText(
            Path, Text, Temporary, OutError))
        return false;
#if defined(_WIN32)
    if (!MoveFileExW(
            Temporary.c_str(), Path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD MoveError = GetLastError();
        std::error_code CleanupError;
        std::filesystem::remove(Temporary, CleanupError);
        OutError = "failed to atomically commit JSON output: win32=" +
            std::to_string(MoveError);
        return false;
    }
#else
    std::filesystem::rename(Temporary, Path, Error);
    if (Error)
    {
        std::error_code CleanupError;
        std::filesystem::remove(Temporary, CleanupError);
        OutError = "failed to atomically commit JSON output: " +
            Error.message();
        return false;
    }
#endif
    return true;
}

nlohmann::json CharacterJson(const BatchCharacterInput& Character)
{
    nlohmann::json Json = {
        {"restFbx", PathToUtf8(Character.RestFbx)},
        {"definitionKind", Character.DefinitionKind},
        {"definitionFile", PathToUtf8(Character.DefinitionFile)}};
    if (!Character.AlignmentRetargeterFile.empty())
    {
        Json["alignmentRetargeterFile"] =
            PathToUtf8(Character.AlignmentRetargeterFile);
    }
    return Json;
}

BatchCharacterInput ReadCharacterJson(const nlohmann::json& Json)
{
    BatchCharacterInput Result;
    Result.RestFbx = PathFromUtf8(Json.at("restFbx").get<std::string>());
    Result.DefinitionKind =
        Json.at("definitionKind").get<std::string>();
    Result.DefinitionFile =
        PathFromUtf8(Json.at("definitionFile").get<std::string>());
    Result.AlignmentRetargeterFile = PathFromUtf8(
        Json.value("alignmentRetargeterFile", std::string()));
    return Result;
}

nlohmann::json ProfileCatalogBindingJson(
    const RetargetBridgeAssetBinding& Binding)
{
    return {
        {"catalogFile", PathToUtf8(Binding.CatalogFile)},
        {"catalogSha256", Binding.CatalogSha256},
        {"catalogId", Binding.CatalogId},
        {"sourceSkeletonId", Binding.SourceSkeletonId},
        {"targetSkeletonId", Binding.TargetSkeletonId},
        {"characterProfiles", {
            {"source", {
                {"packageFile",
                 PathToUtf8(Binding.SourceProfilePackage)},
                {"packageSha256",
                 Binding.SourceProfilePackageSha256},
                {"profileVersion",
                 Binding.SourceProfileVersion}}},
            {"target", {
                {"packageFile",
                 PathToUtf8(Binding.TargetProfilePackage)},
                {"packageSha256",
                 Binding.TargetProfilePackageSha256},
                {"profileVersion",
                 Binding.TargetProfileVersion}}}}},
        {"expectedSha256", {
            {"sourceRest", Binding.SourceRestSha256},
            {"targetRest", Binding.TargetRestSha256},
            {"sourceRigJson", Binding.SourceRigJsonSha256},
            {"targetRigJson", Binding.TargetRigJsonSha256},
            {"sourceAlignmentRetargeterJson",
             Binding.SourceAlignmentRetargeterJsonSha256},
            {"targetAlignmentRetargeterJson",
             Binding.TargetAlignmentRetargeterJsonSha256}}}};
}

RetargetBridgeAssetBinding ReadProfileCatalogBindingJson(
    const nlohmann::json& Json)
{
    RetargetBridgeAssetBinding Result;
    Result.Required = true;
    Result.CatalogFile = PathFromUtf8(
        Json.at("catalogFile").get<std::string>());
    Result.CatalogSha256 =
        Json.at("catalogSha256").get<std::string>();
    Result.CatalogId = Json.at("catalogId").get<std::string>();
    Result.SourceSkeletonId =
        Json.at("sourceSkeletonId").get<std::string>();
    Result.TargetSkeletonId =
        Json.at("targetSkeletonId").get<std::string>();
    const nlohmann::json& Profiles =
        Json.at("characterProfiles");
    const nlohmann::json& Source = Profiles.at("source");
    const nlohmann::json& Target = Profiles.at("target");
    Result.SourceProfilePackage = PathFromUtf8(
        Source.at("packageFile").get<std::string>());
    Result.SourceProfilePackageSha256 =
        Source.at("packageSha256").get<std::string>();
    Result.SourceProfileVersion =
        Source.at("profileVersion").get<std::string>();
    Result.TargetProfilePackage = PathFromUtf8(
        Target.at("packageFile").get<std::string>());
    Result.TargetProfilePackageSha256 =
        Target.at("packageSha256").get<std::string>();
    Result.TargetProfileVersion =
        Target.at("profileVersion").get<std::string>();
    const nlohmann::json& Expected = Json.at("expectedSha256");
    Result.SourceRestSha256 =
        Expected.at("sourceRest").get<std::string>();
    Result.TargetRestSha256 =
        Expected.at("targetRest").get<std::string>();
    Result.SourceRigJsonSha256 =
        Expected.at("sourceRigJson").get<std::string>();
    Result.TargetRigJsonSha256 =
        Expected.at("targetRigJson").get<std::string>();
    Result.SourceAlignmentRetargeterJsonSha256 =
        Expected.at("sourceAlignmentRetargeterJson")
            .get<std::string>();
    Result.TargetAlignmentRetargeterJsonSha256 =
        Expected.at("targetAlignmentRetargeterJson")
            .get<std::string>();
    return Result;
}

nlohmann::json CatalogAnimationJson(
    const BatchCatalogAnimationInput& Animation)
{
    return {
        {"animationId", Animation.AnimationId},
        {"label", Animation.Label},
        {"sourceSkeletonId", Animation.SourceSkeletonId},
        {"sourceAnimationFbx",
         PathToUtf8(Animation.SourceAnimationFbx)},
        {"sourceAnimationSha256",
         Animation.SourceAnimationSha256},
        {"sourceAnimationGoldenJson",
         PathToUtf8(Animation.SourceAnimationGoldenJson)},
        {"sourceAnimationGoldenJsonSha256",
         Animation.SourceAnimationGoldenJsonSha256},
        {"animationStack", Animation.AnimationStack},
        {"sourceFbxImportMode",
         RetargetBridgeSourceFbxImportModeName(
             Animation.SourceFbxImportMode)},
        {"restFbxImportMode",
         RetargetBridgeRestFbxImportModeName(
             Animation.RestFbxImportMode)}};
}

BatchCatalogAnimationInput ReadCatalogAnimationJson(
    const nlohmann::json& Json)
{
    BatchCatalogAnimationInput Result;
    Result.AnimationId =
        Json.at("animationId").get<std::string>();
    Result.Label = Json.at("label").get<std::string>();
    Result.SourceSkeletonId =
        Json.at("sourceSkeletonId").get<std::string>();
    Result.SourceAnimationFbx = PathFromUtf8(
        Json.at("sourceAnimationFbx").get<std::string>());
    Result.SourceAnimationSha256 =
        Json.at("sourceAnimationSha256").get<std::string>();
    Result.SourceAnimationGoldenJson = PathFromUtf8(
        Json.at("sourceAnimationGoldenJson").get<std::string>());
    Result.SourceAnimationGoldenJsonSha256 =
        Json.at("sourceAnimationGoldenJsonSha256")
            .get<std::string>();
    Result.AnimationStack =
        Json.value("animationStack", std::string());
    if (!ParseRetargetBridgeSourceFbxImportMode(
            Json.at("sourceFbxImportMode").get<std::string>(),
            Result.SourceFbxImportMode) ||
        !ParseRetargetBridgeRestFbxImportMode(
            Json.at("restFbxImportMode").get<std::string>(),
            Result.RestFbxImportMode))
    {
        throw std::runtime_error(
            "unsupported profile batch FBX import mode");
    }
    return Result;
}

nlohmann::json ToolsJson(const RetargetBridgeTools& Tools)
{
    nlohmann::json Json = {
        {"bridgeExecutable", PathToUtf8(Tools.BridgeExecutable)},
        {"retargeterExecutable", PathToUtf8(Tools.RetargeterExecutable)},
        {"nodeExecutable", PathToUtf8(Tools.NodeExecutable)},
        {"adapterScript", PathToUtf8(Tools.AdapterScript)},
        {"skrvPackExecutable", PathToUtf8(Tools.SkrvPackExecutable)},
        {"canonicalJson", PathToUtf8(Tools.CanonicalJson)},
        {"defaultSourceRestFbx", PathToUtf8(Tools.DefaultSourceRestFbx)}};
    if (!Tools.UEIKRetargeterExecutable.empty())
    {
        Json["ueIkRetargeterExecutable"] =
            PathToUtf8(Tools.UEIKRetargeterExecutable);
    }
    return Json;
}

RetargetBridgeTools ReadToolsJson(const nlohmann::json& Json)
{
    RetargetBridgeTools Result;
    Result.BridgeExecutable = PathFromUtf8(
        Json.at("bridgeExecutable").get<std::string>());
    Result.RetargeterExecutable = PathFromUtf8(
        Json.at("retargeterExecutable").get<std::string>());
    Result.UEIKRetargeterExecutable = PathFromUtf8(
        Json.value("ueIkRetargeterExecutable", std::string()));
    Result.NodeExecutable = PathFromUtf8(
        Json.at("nodeExecutable").get<std::string>());
    Result.AdapterScript = PathFromUtf8(
        Json.at("adapterScript").get<std::string>());
    Result.SkrvPackExecutable = PathFromUtf8(
        Json.at("skrvPackExecutable").get<std::string>());
    Result.CanonicalJson = PathFromUtf8(
        Json.at("canonicalJson").get<std::string>());
    Result.DefaultSourceRestFbx = PathFromUtf8(
        Json.at("defaultSourceRestFbx").get<std::string>());
    return Result;
}

nlohmann::json JobJson(
    const BatchRetargetJob& Job,
    const bool IncludeProfileCatalogFields)
{
    nlohmann::json Json = {
        {"index", Job.Index},
        {"sourceAnimationFbx", PathToUtf8(Job.SourceAnimationFbx)},
        {"relativeAnimationPath", PathToUtf8(Job.RelativeAnimationPath)},
        {"jobDirectory", PathToUtf8(Job.JobDirectory)},
        {"reviewPackage", PathToUtf8(Job.ReviewPackage)},
        {"finalFbx", PathToUtf8(Job.FinalFbx)},
        {"clipId", Job.ClipId},
        {"clipLabel", Job.ClipLabel},
        {"sourceAnimationSha256", Job.SourceAnimationSha256},
        {"finalFbxSha256", Job.FinalFbxSha256},
        {"state", BatchRetargetJobStateName(Job.State)},
        {"durationSeconds", Job.DurationSeconds},
        {"errors", Job.Errors}};
    if (IncludeProfileCatalogFields)
    {
        Json["sourceAnimationId"] = Job.SourceAnimationId;
        Json["sourceAnimationSkeletonId"] =
            Job.SourceAnimationSkeletonId;
        Json["sourceAnimationGoldenJson"] =
            PathToUtf8(Job.SourceAnimationGoldenJson);
        Json["sourceAnimationGoldenJsonSha256"] =
            Job.SourceAnimationGoldenJsonSha256;
        Json["animationStack"] = Job.AnimationStack;
        Json["sourceFbxImportMode"] =
            RetargetBridgeSourceFbxImportModeName(
                Job.SourceFbxImportMode);
        Json["restFbxImportMode"] =
            RetargetBridgeRestFbxImportModeName(
                Job.RestFbxImportMode);
        Json["timings"] = {
            {"planningPreflightSeconds",
             Job.PlanningPreflightSeconds},
            {"bridgePreflightReused",
             Job.BridgeTimings.PreflightReused},
            {"bridgePreflightSeconds",
             Job.BridgeTimings.PreflightSeconds},
            {"retargetWorkerSeconds",
             Job.BridgeTimings.RetargetWorkerSeconds},
            {"adapterSeconds", Job.BridgeTimings.AdapterSeconds},
            {"packSeconds", Job.BridgeTimings.PackSeconds},
            {"packageInspectSeconds",
             Job.BridgeTimings.PackageInspectSeconds},
            {"verifiedExportCopySeconds",
             Job.VerifiedExportCopySeconds},
            {"bridgeTotalSeconds",
             Job.BridgeTimings.TotalSeconds}};
    }
    return Json;
}

BatchRetargetJobState ParseJobState(const std::string& Text)
{
    if (Text == "running") return BatchRetargetJobState::Running;
    if (Text == "succeeded") return BatchRetargetJobState::Succeeded;
    if (Text == "failed") return BatchRetargetJobState::Failed;
    return BatchRetargetJobState::Pending;
}

BatchRetargetJob ReadJobJson(
    const nlohmann::json& Json,
    const bool RequireProfileCatalogFields)
{
    BatchRetargetJob Result;
    Result.Index = Json.at("index").get<std::size_t>();
    Result.SourceAnimationFbx = PathFromUtf8(
        Json.at("sourceAnimationFbx").get<std::string>());
    Result.RelativeAnimationPath = PathFromUtf8(
        Json.at("relativeAnimationPath").get<std::string>());
    Result.JobDirectory = PathFromUtf8(
        Json.at("jobDirectory").get<std::string>());
    Result.ReviewPackage = PathFromUtf8(
        Json.at("reviewPackage").get<std::string>());
    Result.FinalFbx = PathFromUtf8(
        Json.at("finalFbx").get<std::string>());
    Result.ClipId = Json.at("clipId").get<std::string>();
    Result.ClipLabel = Json.at("clipLabel").get<std::string>();
    Result.SourceAnimationId = RequireProfileCatalogFields
        ? Json.at("sourceAnimationId").get<std::string>()
        : Json.value("sourceAnimationId", std::string());
    Result.SourceAnimationSkeletonId = RequireProfileCatalogFields
        ? Json.at("sourceAnimationSkeletonId").get<std::string>()
        : Json.value("sourceAnimationSkeletonId", std::string());
    Result.SourceAnimationSha256 =
        Json.value("sourceAnimationSha256", std::string());
    Result.SourceAnimationGoldenJson = PathFromUtf8(
        RequireProfileCatalogFields
            ? Json.at("sourceAnimationGoldenJson")
                  .get<std::string>()
            : Json.value(
                  "sourceAnimationGoldenJson", std::string()));
    Result.SourceAnimationGoldenJsonSha256 =
        RequireProfileCatalogFields
            ? Json.at("sourceAnimationGoldenJsonSha256")
                  .get<std::string>()
            : Json.value(
                  "sourceAnimationGoldenJsonSha256",
                  std::string());
    Result.AnimationStack = RequireProfileCatalogFields
        ? Json.at("animationStack").get<std::string>()
        : Json.value("animationStack", std::string());
    if ((RequireProfileCatalogFields ||
         Json.contains("sourceFbxImportMode")) &&
        (!ParseRetargetBridgeSourceFbxImportMode(
             Json.at("sourceFbxImportMode").get<std::string>(),
             Result.SourceFbxImportMode) ||
         !ParseRetargetBridgeRestFbxImportMode(
             Json.at("restFbxImportMode").get<std::string>(),
             Result.RestFbxImportMode)))
    {
        throw std::runtime_error(
            "unsupported batch status FBX import mode");
    }
    if (RequireProfileCatalogFields &&
        (Result.SourceAnimationId.empty() ||
         Result.SourceAnimationSkeletonId.empty() ||
         !IsSha256(Result.SourceAnimationSha256) ||
         Result.SourceAnimationGoldenJson.empty() ||
         !IsSha256(Result.SourceAnimationGoldenJsonSha256)))
    {
        throw std::runtime_error(
            "profile-backed batch status has incomplete per-animation "
            "provenance");
    }
    Result.FinalFbxSha256 =
        Json.value("finalFbxSha256", std::string());
    Result.State = ParseJobState(Json.at("state").get<std::string>());
    Result.DurationSeconds = Json.at("durationSeconds").get<double>();
    if (RequireProfileCatalogFields && Json.contains("timings"))
    {
        const nlohmann::json& Timings = Json.at("timings");
        Result.PlanningPreflightSeconds = Timings.value(
            "planningPreflightSeconds", 0.0);
        Result.BridgeTimings.PreflightReused = Timings.value(
            "bridgePreflightReused", false);
        Result.BridgeTimings.PreflightSeconds = Timings.value(
            "bridgePreflightSeconds", 0.0);
        Result.BridgeTimings.RetargetWorkerSeconds = Timings.value(
            "retargetWorkerSeconds", 0.0);
        Result.BridgeTimings.AdapterSeconds = Timings.value(
            "adapterSeconds", 0.0);
        Result.BridgeTimings.PackSeconds = Timings.value(
            "packSeconds", 0.0);
        Result.BridgeTimings.PackageInspectSeconds = Timings.value(
            "packageInspectSeconds", 0.0);
        Result.VerifiedExportCopySeconds = Timings.value(
            "verifiedExportCopySeconds", 0.0);
        Result.BridgeTimings.TotalSeconds = Timings.value(
            "bridgeTotalSeconds", 0.0);
    }
    Result.Errors = Json.at("errors").get<std::vector<std::string>>();
    return Result;
}

std::string IndexedJobDirectoryName(
    const std::size_t Index,
    const std::string& ClipId)
{
    std::ostringstream Stream;
    Stream << std::setfill('0') << std::setw(6) << (Index + 1) << '_'
           << ClipId;
    return Stream.str();
}

RetargetBridgeRequest BuildBridgeRequestForJob(
    const BatchRetargetRequest& Request,
    const BatchRetargetJob& Job)
{
    RetargetBridgeRequest Result;
    const bool UEIKJson =
        Request.SourceCharacter.DefinitionKind ==
            UEIKJsonDefinitionKind;
    Result.RouteKind = UEIKJson
        ? RetargetBridgeRouteKind::UEIKJsonV1
        : RetargetBridgeRouteKind::ExternalFoundationV1;
    Result.SourceAnimationFbx = Job.SourceAnimationFbx;
    Result.TargetSkeletonFbx =
        Request.TargetCharacter.RestFbx;
    Result.SourceRestFbx = Request.SourceCharacter.RestFbx;
    Result.OutputDirectory = Job.JobDirectory;
    Result.Tools = Request.Tools;
    Result.ClipId = Job.ClipId;
    Result.ClipLabel = Job.ClipLabel;
    Result.AnimationStack = Request.AssetBinding.Required
        ? Job.AnimationStack
        : Request.AnimationStack;
    Result.SourceRigJson =
        Request.SourceCharacter.DefinitionFile;
    Result.TargetRigJson =
        Request.TargetCharacter.DefinitionFile;
    Result.SourceAlignmentRetargeterJson =
        Request.SourceCharacter.AlignmentRetargeterFile;
    Result.TargetAlignmentRetargeterJson =
        Request.TargetCharacter.AlignmentRetargeterFile;
    Result.EnableSpinePelvisFollow =
        Request.EnableSpinePelvisFollow;
    Result.EnableSourceMotionFootLock =
        Request.EnableSourceMotionFootLock;
    Result.OperationStackJson = Request.OperationStackJson;
    Result.OperationStackJsonExpectedSha256 =
        Request.OperationStackJsonExpectedSha256;
    if (Request.AssetBinding.Required)
    {
        Result.SourceAnimationGoldenJson =
            Job.SourceAnimationGoldenJson;
        Result.SourceFbxImportMode = Job.SourceFbxImportMode;
        Result.RestFbxImportMode = Job.RestFbxImportMode;
        Result.AssetBinding = Request.AssetBinding;
        Result.AssetBinding.SourceAnimationId =
            Job.SourceAnimationId;
        Result.AssetBinding.SourceAnimationSkeletonId =
            Job.SourceAnimationSkeletonId;
        Result.AssetBinding.SourceAnimationSha256 =
            Job.SourceAnimationSha256;
        Result.AssetBinding.SourceAnimationGoldenJsonSha256 =
            Job.SourceAnimationGoldenJsonSha256;
    }
    return Result;
}

void AddScanEntry(
    const std::filesystem::directory_entry& Entry,
    const BatchRetargetRequest& Request,
    std::vector<std::filesystem::path>& Files,
    std::vector<std::string>& Warnings)
{
    std::error_code Error;
    if (Entry.is_symlink(Error) && !Error)
    {
        if (Warnings.size() < 20)
            Warnings.push_back("symbolic link skipped: " +
                               PathToUtf8(Entry.path()));
        return;
    }
    Error.clear();
    if (!Entry.is_regular_file(Error) || Error ||
        !HasFbxExtension(Entry.path()))
    {
        return;
    }
    if (SamePath(Entry.path(), Request.SourceCharacter.RestFbx) ||
        SamePath(Entry.path(), Request.TargetCharacter.RestFbx))
    {
        if (Warnings.size() < 20)
            Warnings.push_back("selected T-pose excluded from animation scan: " +
                               PathToUtf8(Entry.path()));
        return;
    }
    Files.push_back(Entry.path().lexically_normal());
}

std::vector<std::filesystem::path> ScanAnimationFiles(
    const BatchRetargetRequest& Request,
    std::vector<std::string>& Errors,
    std::vector<std::string>& Warnings)
{
    std::vector<std::filesystem::path> Files;
    std::error_code Error;
    if (Request.Recursive)
    {
        std::filesystem::recursive_directory_iterator Iterator(
            Request.AnimationDirectory,
            std::filesystem::directory_options::skip_permission_denied,
            Error);
        const std::filesystem::recursive_directory_iterator End;
        while (!Error && Iterator != End)
        {
            AddScanEntry(*Iterator, Request, Files, Warnings);
            if (Files.size() > MaximumPlannedFiles)
            {
                Errors.push_back(
                    "animation folder exceeds the 100000-file safety limit");
                return {};
            }
            Iterator.increment(Error);
        }
    }
    else
    {
        std::filesystem::directory_iterator Iterator(
            Request.AnimationDirectory,
            std::filesystem::directory_options::skip_permission_denied,
            Error);
        const std::filesystem::directory_iterator End;
        while (!Error && Iterator != End)
        {
            AddScanEntry(*Iterator, Request, Files, Warnings);
            if (Files.size() > MaximumPlannedFiles)
            {
                Errors.push_back(
                    "animation folder exceeds the 100000-file safety limit");
                return {};
            }
            Iterator.increment(Error);
        }
    }
    if (Error)
        Errors.push_back("failed while scanning animation folder: " +
                         Error.message());
    std::sort(
        Files.begin(), Files.end(),
        [](const std::filesystem::path& Left,
           const std::filesystem::path& Right)
        {
            return LowerAscii(PathToUtf8(Left)) <
                LowerAscii(PathToUtf8(Right));
        });
    return Files;
}

void Recount(BatchRetargetStatus& Status)
{
    Status.TotalJobs = Status.Jobs.size();
    Status.CompletedJobs = 0;
    Status.SucceededJobs = 0;
    Status.FailedJobs = 0;
    for (const BatchRetargetJob& Job : Status.Jobs)
    {
        if (Job.State == BatchRetargetJobState::Succeeded)
        {
            ++Status.CompletedJobs;
            ++Status.SucceededJobs;
        }
        else if (Job.State == BatchRetargetJobState::Failed)
        {
            ++Status.CompletedJobs;
            ++Status.FailedJobs;
        }
    }
}
} // namespace

const char* BatchRetargetJobStateName(const BatchRetargetJobState State)
{
    switch (State)
    {
    case BatchRetargetJobState::Pending: return "pending";
    case BatchRetargetJobState::Running: return "running";
    case BatchRetargetJobState::Succeeded: return "succeeded";
    case BatchRetargetJobState::Failed: return "failed";
    }
    return "pending";
}

std::filesystem::path DiscoverBatchRetargetExecutable(
    const std::filesystem::path& ViewerExecutable)
{
    const std::filesystem::path Directory =
        std::filesystem::absolute(ViewerExecutable).parent_path();
#if defined(_WIN32)
    constexpr const char* Name = "skrtg_batch_retarget.exe";
#else
    constexpr const char* Name = "skrtg_batch_retarget";
#endif
    for (const std::filesystem::path& Candidate : {
             Directory / Name, Directory / "tools" / Name})
    {
        if (IsRegularFile(Candidate)) return Candidate.lexically_normal();
    }
    return {};
}

BatchRetargetPlan BuildBatchRetargetPlan(
    const BatchRetargetRequest& Request)
{
    BatchRetargetPlan Result;
    const bool ProfileCatalogBatch =
        Request.AssetBinding.Required;
    if (!IsRegularFile(Request.SourceCharacter.RestFbx) ||
        !HasFbxExtension(Request.SourceCharacter.RestFbx))
    {
        Result.Errors.push_back("source T-pose must be a readable .fbx file");
    }
    if (!IsRegularFile(Request.TargetCharacter.RestFbx) ||
        !HasFbxExtension(Request.TargetCharacter.RestFbx))
    {
        Result.Errors.push_back("target T-pose must be a readable .fbx file");
    }
    const bool ExternalRoute =
        Request.SourceCharacter.DefinitionKind == ExternalDefinitionKind &&
        Request.TargetCharacter.DefinitionKind == ExternalDefinitionKind;
    const bool UEIKJsonRoute =
        Request.SourceCharacter.DefinitionKind == UEIKJsonDefinitionKind &&
        Request.TargetCharacter.DefinitionKind == UEIKJsonDefinitionKind;
    if (!ExternalRoute && !UEIKJsonRoute)
    {
        Result.Errors.push_back(
            "source and target character definitions must select the "
            "same supported route: external_foundation_v1 or "
            "ue_ik_json_v1");
    }
    else if (ProfileCatalogBatch && !UEIKJsonRoute)
    {
        Result.Errors.push_back(
            "profile-backed batch v3 requires the UE IK JSON route");
    }
    else if (ExternalRoute &&
             (!Request.SourceCharacter.DefinitionFile.empty() ||
              !Request.TargetCharacter.DefinitionFile.empty() ||
              !Request.SourceCharacter.AlignmentRetargeterFile.empty() ||
              !Request.TargetCharacter.AlignmentRetargeterFile.empty()))
    {
        Result.Errors.push_back(
            "external_foundation_v1 does not accept character definition "
            "files");
    }
    else if (UEIKJsonRoute)
    {
        for (const auto& [Path, Label] :
             std::vector<std::pair<std::filesystem::path, const char*>>{
                 {Request.SourceCharacter.DefinitionFile,
                  "source IK Rig JSON"},
                 {Request.TargetCharacter.DefinitionFile,
                  "target IK Rig JSON"},
                 {Request.SourceCharacter.AlignmentRetargeterFile,
                  "source alignment IK Retargeter JSON"},
                 {Request.TargetCharacter.AlignmentRetargeterFile,
                  "target alignment IK Retargeter JSON"}})
        {
            if (!IsRegularFile(Path) || !HasJsonExtension(Path))
            {
                Result.Errors.push_back(
                    std::string(Label) +
                    " must be a readable exported .json file");
            }
        }
    }
    if (!Request.OperationStackJson.empty() &&
        (!UEIKJsonRoute || !ProfileCatalogBatch))
    {
        Result.Errors.push_back(
            "Operation System v2 batch execution requires the exact "
            "profile/catalog-backed UE IK JSON route");
    }
    if (!Request.OperationStackJsonExpectedSha256.empty() &&
        Request.OperationStackJson.empty())
    {
        Result.Errors.push_back(
            "Operation System v2 expected SHA-256 requires a config JSON");
    }
    if (ProfileCatalogBatch)
    {
        if (Request.AssetBinding.SourceProfilePackage.empty() ||
            Request.AssetBinding.TargetProfilePackage.empty())
        {
            Result.Errors.push_back(
                "profile-backed batch requires source and target "
                ".skrtgprofile package bindings");
        }
        if (Request.CatalogAnimations.empty())
        {
            Result.Errors.push_back(
                "profile-backed batch has no selected catalog animations");
        }
        else if (Request.CatalogAnimations.size() >
                 MaximumPlannedFiles)
        {
            Result.Errors.push_back(
                "profile-backed batch exceeds the 100000-animation "
                "safety limit");
        }
        if (!Request.AnimationDirectory.empty())
        {
            Result.Errors.push_back(
                "profile-backed batch does not accept an arbitrary "
                "animation input folder");
        }
        if (Request.Recursive)
        {
            Result.Errors.push_back(
                "profile-backed batch does not accept recursive folder "
                "semantics");
        }
    }
    else if (!IsDirectory(Request.AnimationDirectory))
    {
        Result.Errors.push_back(
            "animation input folder is not readable");
    }
    if (Request.OutputDirectory.empty())
        Result.Errors.push_back("batch output directory is empty");
    else if (!DirectoryEmptyOrAbsent(Request.OutputDirectory))
        Result.Errors.push_back(
            "batch output directory must be absent or empty (fail-closed overwrite policy)");
    if (!ProfileCatalogBatch)
    {
        bool OutputInInputResolved = true;
        bool InputInOutputResolved = true;
        const bool OutputInInput =
            !Request.AnimationDirectory.empty() &&
            !Request.OutputDirectory.empty() &&
            IsPathInsideOrEqual(
                Request.OutputDirectory,
                Request.AnimationDirectory,
                OutputInInputResolved);
        const bool InputInOutput =
            !Request.AnimationDirectory.empty() &&
            !Request.OutputDirectory.empty() &&
            IsPathInsideOrEqual(
                Request.AnimationDirectory,
                Request.OutputDirectory,
                InputInOutputResolved);
        if (!OutputInInputResolved || !InputInOutputResolved)
        {
            Result.Errors.push_back(
                "animation input and batch output paths could not be "
                "securely resolved");
        }
        else if (OutputInInput || InputInOutput)
        {
            Result.Errors.push_back(
                "animation input and batch output directories may not "
                "overlap");
        }
    }
    if (UEIKJsonRoute &&
        (Request.EnableSpinePelvisFollow ||
         Request.EnableSourceMotionFootLock))
    {
        Result.Errors.push_back(
            "UE IK JSON candidate batch route does not enable the "
            "frozen Spine/Pelvis or FootLock modules");
    }
    else if (Request.EnableSourceMotionFootLock &&
             !Request.EnableSpinePelvisFollow)
    {
        Result.Errors.push_back(
            "frozen FootLock batch route requires the frozen Spine/Pelvis prerequisite");
    }
    if (!Result.Errors.empty()) return Result;

    std::set<std::string> PlannedFinalPaths;
    if (ProfileCatalogBatch)
    {
        std::set<std::string> AnimationIds;
        Result.Jobs.reserve(Request.CatalogAnimations.size());
        for (std::size_t Index = 0;
             Index < Request.CatalogAnimations.size(); ++Index)
        {
            const BatchCatalogAnimationInput& Animation =
                Request.CatalogAnimations[Index];
            BatchRetargetJob Job;
            Job.Index = Index;
            Job.SourceAnimationFbx =
                Animation.SourceAnimationFbx;
            Job.RelativeAnimationPath =
                PathFromUtf8(Animation.AnimationId + ".fbx");
            Job.ClipId = Animation.AnimationId;
            Job.ClipLabel = Animation.Label;
            Job.SourceAnimationId = Animation.AnimationId;
            Job.SourceAnimationSkeletonId =
                Animation.SourceSkeletonId;
            Job.SourceAnimationSha256 =
                Animation.SourceAnimationSha256;
            Job.SourceAnimationGoldenJson =
                Animation.SourceAnimationGoldenJson;
            Job.SourceAnimationGoldenJsonSha256 =
                Animation.SourceAnimationGoldenJsonSha256;
            Job.AnimationStack = Animation.AnimationStack;
            Job.SourceFbxImportMode =
                Animation.SourceFbxImportMode;
            Job.RestFbxImportMode =
                Animation.RestFbxImportMode;
            Job.JobDirectory = Request.OutputDirectory / "Jobs" /
                IndexedJobDirectoryName(Index, Job.ClipId);
            Job.ReviewPackage =
                Job.JobDirectory / "review.skrv";
            Job.FinalFbx =
                Request.OutputDirectory / "FinalFBX" /
                (Animation.AnimationId + "__SKRTG_Final.fbx");
            if (!AnimationIds.insert(Animation.AnimationId).second)
            {
                Result.Errors.push_back(
                    "profile-backed batch contains duplicate animation "
                    "ID: " + Animation.AnimationId);
            }
            if (!PlannedFinalPaths.insert(
                    ComparablePath(Job.FinalFbx)).second)
            {
                Result.Errors.push_back(
                    "multiple catalog animations map to the same Final "
                    "FBX path: " + PathToUtf8(Job.FinalFbx));
            }
            Result.Jobs.push_back(std::move(Job));
        }
    }
    else
    {
        const std::vector<std::filesystem::path> Files =
            ScanAnimationFiles(
                Request, Result.Errors, Result.Warnings);
        if (!Result.Errors.empty()) return Result;
        if (Files.empty())
        {
            Result.Errors.push_back(
                "animation input folder contains no .fbx files");
            return Result;
        }
        Result.Jobs.reserve(Files.size());
        for (std::size_t Index = 0; Index < Files.size(); ++Index)
        {
            BatchRetargetJob Job;
            Job.Index = Index;
            Job.SourceAnimationFbx = Files[Index];
            std::error_code RelativeError;
            Job.RelativeAnimationPath = std::filesystem::relative(
                Files[Index], Request.AnimationDirectory,
                RelativeError);
            if (RelativeError ||
                Job.RelativeAnimationPath.empty())
            {
                Result.Errors.push_back(
                    "failed to make animation path relative to the "
                    "selected folder: " +
                    PathToUtf8(Files[Index]));
                continue;
            }
            Job.ClipId = MakeBridgeClipId(Files[Index]);
            Job.ClipLabel = PathToUtf8(Files[Index].stem());
            Job.JobDirectory =
                Request.OutputDirectory / "Jobs" /
                IndexedJobDirectoryName(Index, Job.ClipId);
            Job.ReviewPackage =
                Job.JobDirectory / "review.skrv";
            Job.FinalFbx =
                Request.OutputDirectory / "FinalFBX" /
                Job.RelativeAnimationPath.parent_path() /
                (PathToUtf8(Files[Index].stem()) +
                 "__SKRTG_Final.fbx");
            if (!PlannedFinalPaths.insert(
                    ComparablePath(Job.FinalFbx)).second)
            {
                Result.Errors.push_back(
                    "multiple source animations map to the same Final "
                    "FBX path: " + PathToUtf8(Job.FinalFbx));
            }
            Result.Jobs.push_back(std::move(Job));
        }
    }
    if (!Result.Errors.empty()) return Result;

    std::vector<RetargetBridgeRequest> BridgeRequests;
    BridgeRequests.reserve(Result.Jobs.size());
    for (const BatchRetargetJob& Job : Result.Jobs)
        BridgeRequests.push_back(
            BuildBridgeRequestForJob(Request, Job));
    // Evidence is shared only while planning this immutable selection; each
    // returned preflight remains bound to its exact job and output path.
    Result.Preflights =
        PreflightRetargetBridges(BridgeRequests);
    if (Result.Preflights.size() != Result.Jobs.size())
    {
        Result.Errors.push_back(
            "batch preflight did not return one result per job");
        return Result;
    }
    for (std::size_t Index = 0;
         Index < Result.Preflights.size(); ++Index)
    {
        const RetargetBridgePreflight& Preflight =
            Result.Preflights[Index];
        Result.Jobs[Index].PlanningPreflightSeconds =
            Preflight.DurationSeconds;
        for (const std::string& Error : Preflight.Errors)
        {
            Result.Errors.push_back(
                ProfileCatalogBatch
                    ? (Result.Jobs[Index].SourceAnimationId +
                       ": " + Error)
                    : Error);
        }
        for (const std::string& Warning : Preflight.Warnings)
        {
            Result.Warnings.push_back(
                ProfileCatalogBatch
                    ? (Result.Jobs[Index].SourceAnimationId +
                       ": " + Warning)
                    : Warning);
        }
    }
    Result.Warnings.push_back(
        "low-memory streaming remains fixed at one active animation; "
        "immutable hashes, profiles, catalog records, and the optional "
        "Operation System v2 config are cached during whole-batch preflight, "
        "while each Retargeter worker still re-hashes solver inputs");
    Result.Success = Result.Errors.empty();
    return Result;
}

bool WriteBatchRetargetRequest(
    const BatchRetargetRequest& Request,
    const std::filesystem::path& OutputJson,
    std::string& OutError)
{
    const bool UEIKJson =
        Request.SourceCharacter.DefinitionKind == UEIKJsonDefinitionKind;
    const bool ProfileCatalogBatch =
        Request.AssetBinding.Required;
    if (!Request.OperationStackJson.empty() && !ProfileCatalogBatch)
    {
        OutError =
            "Operation System v2 batch requests require an exact "
            "profile/catalog asset selection";
        return false;
    }
    nlohmann::json Json = {
        {"schema", !Request.OperationStackJson.empty()
            ? ProfileCatalogOperationRequestSchema
            : (ProfileCatalogBatch
            ? ProfileCatalogRequestSchema
            : (UEIKJson
                ? UEIKJsonRequestSchema
                : ExternalRequestSchema))},
        {"sourceCharacter", CharacterJson(Request.SourceCharacter)},
        {"targetCharacter", CharacterJson(Request.TargetCharacter)},
        {"animationDirectory", PathToUtf8(Request.AnimationDirectory)},
        {"outputDirectory", PathToUtf8(Request.OutputDirectory)},
        {"recursive", ProfileCatalogBatch
            ? false
            : Request.Recursive},
        {"animationStack", Request.AnimationStack},
        {"enableSpinePelvisFollow", Request.EnableSpinePelvisFollow},
        {"enableSourceMotionFootLock", Request.EnableSourceMotionFootLock},
        {"executionPolicy", {
            {"maximumConcurrentJobs", 1},
            {"mode", "streaming_one_animation_per_worker"},
            {"continueAfterJobFailure", true}}},
        {"tools", ToolsJson(Request.Tools)}};
    if (!Request.OperationStackJson.empty())
    {
        Json["operationStack"] = {
            {"configJson", PathToUtf8(Request.OperationStackJson)},
            {"expectedSha256",
             Request.OperationStackJsonExpectedSha256},
            {"candidate", true},
            {"selected", false},
            {"adopted", false}};
    }
    if (ProfileCatalogBatch)
    {
        Json["assetSelection"] =
            ProfileCatalogBindingJson(Request.AssetBinding);
        nlohmann::json Animations = nlohmann::json::array();
        for (const BatchCatalogAnimationInput& Animation :
             Request.CatalogAnimations)
        {
            Animations.push_back(
                CatalogAnimationJson(Animation));
        }
        Json["animations"] = std::move(Animations);
    }
    return WriteTextAtomic(OutputJson, Json.dump(2) + "\n", OutError);
}

bool ReadBatchRetargetRequest(
    const std::filesystem::path& InputJson,
    BatchRetargetRequest& OutRequest,
    std::string& OutError)
{
    OutError.clear();
    try
    {
        std::ifstream Stream(InputJson, std::ios::binary);
        if (!Stream)
        {
            OutError = "failed to open batch retarget request";
            return false;
        }
        const nlohmann::json Json = nlohmann::json::parse(Stream);
        const std::string Schema =
            Json.at("schema").get<std::string>();
        if (Schema != ExternalRequestSchema &&
            Schema != UEIKJsonRequestSchema &&
            Schema != ProfileCatalogRequestSchema &&
            Schema != ProfileCatalogOperationRequestSchema)
        {
            OutError = "unsupported batch retarget request schema";
            return false;
        }
        const nlohmann::json& Execution = Json.at("executionPolicy");
        if (Execution.at("maximumConcurrentJobs").get<std::size_t>() != 1 ||
            Execution.at("mode").get<std::string>() !=
                "streaming_one_animation_per_worker" ||
            !Execution.at("continueAfterJobFailure").get<bool>())
        {
            OutError =
                "batch request violates the fixed serial execution policy";
            return false;
        }
        OutRequest = {};
        OutRequest.SourceCharacter =
            ReadCharacterJson(Json.at("sourceCharacter"));
        OutRequest.TargetCharacter =
            ReadCharacterJson(Json.at("targetCharacter"));
        OutRequest.AnimationDirectory = PathFromUtf8(
            Json.at("animationDirectory").get<std::string>());
        OutRequest.OutputDirectory = PathFromUtf8(
            Json.at("outputDirectory").get<std::string>());
        OutRequest.Recursive = Json.at("recursive").get<bool>();
        OutRequest.AnimationStack =
            Json.value("animationStack", std::string());
        OutRequest.EnableSpinePelvisFollow =
            Json.at("enableSpinePelvisFollow").get<bool>();
        OutRequest.EnableSourceMotionFootLock =
            Json.at("enableSourceMotionFootLock").get<bool>();
        OutRequest.Tools = ReadToolsJson(Json.at("tools"));
        const bool ProfileCatalogRequest =
            Schema == ProfileCatalogRequestSchema ||
            Schema == ProfileCatalogOperationRequestSchema;
        if (ProfileCatalogRequest)
        {
            OutRequest.AssetBinding =
                ReadProfileCatalogBindingJson(
                    Json.at("assetSelection"));
            const nlohmann::json& Animations =
                Json.at("animations");
            if (!Animations.is_array() ||
                Animations.size() > MaximumPlannedFiles)
            {
                OutError =
                    "profile-backed batch animation inventory exceeds "
                    "the fixed safety contract";
                return false;
            }
            for (const nlohmann::json& Animation : Animations)
            {
                OutRequest.CatalogAnimations.push_back(
                    ReadCatalogAnimationJson(Animation));
            }
            if (!OutRequest.AnimationDirectory.empty() ||
                OutRequest.Recursive)
            {
                OutError =
                    "profile-backed batch must not scan an "
                    "animation directory";
                return false;
            }
        }
        if (Schema == ProfileCatalogOperationRequestSchema)
        {
            const nlohmann::json& OperationStack =
                Json.at("operationStack");
            if (!OperationStack.at("candidate").get<bool>() ||
                OperationStack.at("selected").get<bool>() ||
                OperationStack.at("adopted").get<bool>())
            {
                OutError =
                    "Operation System v2 batch config must remain "
                    "candidate=true, selected=false, adopted=false";
                return false;
            }
            OutRequest.OperationStackJson = PathFromUtf8(
                OperationStack.at("configJson").get<std::string>());
            OutRequest.OperationStackJsonExpectedSha256 =
                OperationStack.at("expectedSha256")
                    .get<std::string>();
        }
        return true;
    }
    catch (const std::exception& Error)
    {
        OutError = std::string("invalid batch retarget request: ") +
            Error.what();
        return false;
    }
}

bool WriteBatchRetargetStatus(
    const BatchRetargetStatus& Status,
    const std::filesystem::path& OutputJson,
    std::string& OutError)
{
    nlohmann::json Jobs = nlohmann::json::array();
    for (const BatchRetargetJob& Job : Status.Jobs)
    {
        Jobs.push_back(
            JobJson(Job, Status.AssetBinding.Required));
    }
    nlohmann::json Json = {
        {"schema", !Status.OperationStackJson.empty()
            ? ProfileCatalogOperationStatusSchema
            : (Status.AssetBinding.Required
            ? ProfileCatalogStatusSchema
            : LegacyStatusSchema)},
        {"running", Status.Running},
        {"complete", Status.Complete},
        {"success", Status.Success},
        {"cancelled", Status.Cancelled},
        {"executionPolicy", {
            {"maximumConcurrentJobs", Status.MaximumConcurrentJobs},
            {"mode", "streaming_one_animation_per_worker"},
            {"continueAfterJobFailure", true}}},
        {"counts", {
            {"total", Status.TotalJobs},
            {"completed", Status.CompletedJobs},
            {"succeeded", Status.SucceededJobs},
            {"failed", Status.FailedJobs}}},
        {"activeJobIndex", Status.HasActiveJob
            ? nlohmann::json(Status.ActiveJobIndex)
            : nlohmann::json(nullptr)},
        {"durationSeconds", Status.DurationSeconds},
        {"sourceCharacter", CharacterJson(Status.SourceCharacter)},
        {"targetCharacter", CharacterJson(Status.TargetCharacter)},
        {"animationDirectory", PathToUtf8(Status.AnimationDirectory)},
        {"outputDirectory", PathToUtf8(Status.OutputDirectory)},
        {"recursive", Status.Recursive},
        {"animationStack", Status.AnimationStack},
        {"enableSpinePelvisFollow", Status.EnableSpinePelvisFollow},
        {"enableSourceMotionFootLock", Status.EnableSourceMotionFootLock},
        {"foundationFrozen",
         Status.SourceCharacter.DefinitionKind == ExternalDefinitionKind},
        {"skrvV1Modified", false},
        {"xmlDefinitionParsingEnabled", false},
        {"jobs", std::move(Jobs)},
        {"errors", Status.Errors}};
    if (!Status.OperationStackJson.empty())
    {
        Json["operationStack"] = {
            {"configJson", PathToUtf8(Status.OperationStackJson)},
            {"configSha256", Status.OperationStackJsonSha256},
            {"candidate", true},
            {"selected", false},
            {"adopted", false}};
    }
    if (Status.AssetBinding.Required)
    {
        Json["assetSelection"] =
            ProfileCatalogBindingJson(Status.AssetBinding);
        Json["candidateRouteSelected"] = false;
        Json["candidateRouteAdopted"] = false;
        Json["timings"] = {
            {"planningSeconds", Status.PlanningDurationSeconds},
            {"executionSeconds", Status.DurationSeconds},
            {"wallSeconds", Status.WallDurationSeconds}};
    }
    return WriteTextAtomic(OutputJson, Json.dump(2) + "\n", OutError);
}

bool ReadBatchRetargetStatus(
    const std::filesystem::path& InputJson,
    BatchRetargetStatus& OutStatus,
    std::string& OutError)
{
    OutError.clear();
    try
    {
        std::ifstream Stream(InputJson, std::ios::binary);
        if (!Stream)
        {
            OutError = "failed to open batch retarget status";
            return false;
        }
        const nlohmann::json Json = nlohmann::json::parse(Stream);
        const std::string Schema =
            Json.at("schema").get<std::string>();
        if (Schema != LegacyStatusSchema &&
            Schema != ProfileCatalogStatusSchemaV2 &&
            Schema != ProfileCatalogStatusSchema &&
            Schema != ProfileCatalogOperationStatusSchema)
        {
            OutError = "unsupported batch retarget status schema";
            return false;
        }
        const nlohmann::json& Execution =
            Json.at("executionPolicy");
        const bool ProfileStatus =
            Schema == ProfileCatalogStatusSchemaV2 ||
            Schema == ProfileCatalogStatusSchema ||
            Schema == ProfileCatalogOperationStatusSchema;
        if (ProfileStatus &&
            (Execution.at("maximumConcurrentJobs")
                    .get<std::size_t>() != 1 ||
             Execution.at("mode").get<std::string>() !=
                 "streaming_one_animation_per_worker" ||
             !Execution.at("continueAfterJobFailure").get<bool>()))
        {
            OutError =
                "profile-backed batch status violates the fixed "
                "serial execution policy";
            return false;
        }
        OutStatus = {};
        OutStatus.Running = Json.at("running").get<bool>();
        OutStatus.Complete = Json.at("complete").get<bool>();
        OutStatus.Success = Json.at("success").get<bool>();
        OutStatus.Cancelled = Json.value("cancelled", false);
        OutStatus.MaximumConcurrentJobs = Json.at("executionPolicy")
            .at("maximumConcurrentJobs").get<std::size_t>();
        OutStatus.TotalJobs = Json.at("counts").at("total").get<std::size_t>();
        OutStatus.CompletedJobs = Json.at("counts")
            .at("completed").get<std::size_t>();
        OutStatus.SucceededJobs = Json.at("counts")
            .at("succeeded").get<std::size_t>();
        OutStatus.FailedJobs = Json.at("counts")
            .at("failed").get<std::size_t>();
        if (!Json.at("activeJobIndex").is_null())
        {
            OutStatus.HasActiveJob = true;
            OutStatus.ActiveJobIndex =
                Json.at("activeJobIndex").get<std::size_t>();
        }
        OutStatus.DurationSeconds =
            Json.at("durationSeconds").get<double>();
        OutStatus.SourceCharacter =
            ReadCharacterJson(Json.at("sourceCharacter"));
        OutStatus.TargetCharacter =
            ReadCharacterJson(Json.at("targetCharacter"));
        OutStatus.AnimationDirectory = PathFromUtf8(
            Json.at("animationDirectory").get<std::string>());
        OutStatus.OutputDirectory = PathFromUtf8(
            Json.at("outputDirectory").get<std::string>());
        OutStatus.Recursive = Json.at("recursive").get<bool>();
        OutStatus.AnimationStack =
            Json.value("animationStack", std::string());
        OutStatus.EnableSpinePelvisFollow =
            Json.at("enableSpinePelvisFollow").get<bool>();
        OutStatus.EnableSourceMotionFootLock =
            Json.at("enableSourceMotionFootLock").get<bool>();
        if (ProfileStatus)
        {
            if (!OutStatus.AnimationDirectory.empty() ||
                OutStatus.Recursive)
            {
                OutError =
                    "profile-backed batch status may not claim folder "
                    "scan semantics";
                return false;
            }
            if (Json.at("candidateRouteSelected").get<bool>() ||
                Json.at("candidateRouteAdopted").get<bool>())
            {
                OutError =
                    "profile-backed batch status may not select or "
                    "adopt the candidate route";
                return false;
            }
            OutStatus.AssetBinding =
                ReadProfileCatalogBindingJson(
                    Json.at("assetSelection"));
            if (Json.contains("timings"))
            {
                const nlohmann::json& Timings = Json.at("timings");
                OutStatus.PlanningDurationSeconds = Timings.value(
                    "planningSeconds", 0.0);
                OutStatus.WallDurationSeconds = Timings.value(
                    "wallSeconds", OutStatus.DurationSeconds);
            }
        }
        if (Schema == ProfileCatalogOperationStatusSchema)
        {
            const nlohmann::json& OperationStack =
                Json.at("operationStack");
            if (!OperationStack.at("candidate").get<bool>() ||
                OperationStack.at("selected").get<bool>() ||
                OperationStack.at("adopted").get<bool>())
            {
                OutError =
                    "Operation System v2 batch status may not select "
                    "or adopt the candidate stack";
                return false;
            }
            OutStatus.OperationStackJson = PathFromUtf8(
                OperationStack.at("configJson").get<std::string>());
            OutStatus.OperationStackJsonSha256 =
                OperationStack.at("configSha256").get<std::string>();
        }
        std::set<std::string> ProfileAnimationIds;
        for (const nlohmann::json& Job : Json.at("jobs"))
        {
            BatchRetargetJob Parsed = ReadJobJson(
                Job, ProfileStatus);
            if (ProfileStatus)
            {
                if (Parsed.SourceAnimationId != Parsed.ClipId ||
                    Parsed.SourceAnimationSkeletonId !=
                        OutStatus.AssetBinding.SourceSkeletonId)
                {
                    throw std::runtime_error(
                        "profile-backed batch status job provenance "
                        "does not match its clip or source profile");
                }
                if (!ProfileAnimationIds.insert(
                        Parsed.SourceAnimationId).second)
                {
                    throw std::runtime_error(
                        "profile-backed batch status contains duplicate "
                        "animation IDs");
                }
            }
            OutStatus.Jobs.push_back(std::move(Parsed));
        }
        if (ProfileStatus &&
            OutStatus.Jobs.size() != OutStatus.TotalJobs)
        {
            throw std::runtime_error(
                "profile-backed batch status job inventory does not "
                "match its declared total");
        }
        OutStatus.Errors = Json.at("errors").get<std::vector<std::string>>();
        return true;
    }
    catch (const std::exception& Error)
    {
        OutError = std::string("invalid batch retarget status: ") +
            Error.what();
        return false;
    }
}

std::vector<BatchReviewAnimation> BuildBatchReviewAnimationList(
    const BatchRetargetStatus& Status)
{
    std::vector<BatchReviewAnimation> Result;
    Result.reserve(std::min(
        Status.SucceededJobs, Status.Jobs.size()));
    for (const BatchRetargetJob& Job : Status.Jobs)
    {
        if (Job.State != BatchRetargetJobState::Succeeded ||
            Job.ReviewPackage.empty())
        {
            continue;
        }

        BatchReviewAnimation Animation;
        Animation.JobIndex = Job.Index;
        Animation.Id = !Job.SourceAnimationId.empty()
            ? Job.SourceAnimationId : Job.ClipId;
        Animation.Label = !Job.ClipLabel.empty()
            ? Job.ClipLabel
            : PathToUtf8(Job.RelativeAnimationPath.stem());
        Animation.ReviewPackage = Job.ReviewPackage;
        if (Animation.Id.empty() || Animation.Label.empty())
            continue;
        Result.push_back(std::move(Animation));
    }
    return Result;
}

BatchRetargetRunResult RunBatchRetarget(
    const BatchRetargetRequest& Request)
{
    const auto WallStart = std::chrono::steady_clock::now();
    BatchRetargetRunResult Result;
    Result.StatusJson = Request.OutputDirectory / "batch_status.json";
    const BatchRetargetPlan Plan = BuildBatchRetargetPlan(Request);
    const double PlanningSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - WallStart).count();
    if (!Plan.Success)
    {
        Result.Errors = Plan.Errors;
        return Result;
    }

    std::error_code DirectoryError;
    std::filesystem::create_directories(
        Request.OutputDirectory / "Jobs", DirectoryError);
    if (!DirectoryError)
        std::filesystem::create_directories(
            Request.OutputDirectory / "FinalFBX", DirectoryError);
    if (DirectoryError)
    {
        Result.Errors.push_back("failed to create batch output directories");
        return Result;
    }

    BatchRetargetStatus Status;
    Status.Running = true;
    Status.MaximumConcurrentJobs = 1;
    Status.SourceCharacter = Request.SourceCharacter;
    Status.TargetCharacter = Request.TargetCharacter;
    Status.AnimationDirectory = Request.AnimationDirectory;
    Status.OutputDirectory = Request.OutputDirectory;
    Status.OperationStackJson = Request.OperationStackJson;
    if (!Plan.Preflights.empty())
    {
        Status.OperationStackJsonSha256 =
            Plan.Preflights.front().OperationStackJsonSha256;
    }
    Status.Recursive = Request.Recursive;
    Status.AnimationStack = Request.AnimationStack;
    Status.EnableSpinePelvisFollow = Request.EnableSpinePelvisFollow;
    Status.EnableSourceMotionFootLock = Request.EnableSourceMotionFootLock;
    Status.AssetBinding = Request.AssetBinding;
    Status.Jobs = Plan.Jobs;
    Status.PlanningDurationSeconds = PlanningSeconds;
    Status.WallDurationSeconds = PlanningSeconds;
    Recount(Status);
    const auto BatchStart = std::chrono::steady_clock::now();
    std::string StatusError;
    if (!WriteBatchRetargetStatus(Status, Result.StatusJson, StatusError))
    {
        Result.Errors.push_back(StatusError);
        return Result;
    }

    bool StatusWriteFailed = false;
    for (std::size_t Index = 0; Index < Status.Jobs.size(); ++Index)
    {
        BatchRetargetJob& Job = Status.Jobs[Index];
        Job.State = BatchRetargetJobState::Running;
        Status.HasActiveJob = true;
        Status.ActiveJobIndex = Index;
        Status.DurationSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - BatchStart).count();
        if (!WriteBatchRetargetStatus(
                Status, Result.StatusJson, StatusError))
        {
            Result.Errors.push_back(StatusError);
            StatusWriteFailed = true;
            break;
        }

        const auto JobStart = std::chrono::steady_clock::now();
        const RetargetBridgeRequest Single =
            BuildBridgeRequestForJob(Request, Job);

        const RetargetBridgeRunResult Bridge =
            RunRetargetBridgePreflighted(
                Single, Plan.Preflights[Index]);
        Job.BridgeTimings = Bridge.Timings;
        Job.SourceAnimationSha256 = Bridge.SourceAnimationSha256;
        if (!Bridge.Success)
        {
            Job.Errors = Bridge.Errors;
        }
        else
        {
            std::filesystem::path VerifiedFinalFbx =
                Bridge.VerifiedFinalFbx;
            std::string VerifiedFinalSha256 =
                Bridge.VerifiedFinalFbxSha256;
            if (VerifiedFinalFbx.empty() ||
                VerifiedFinalSha256.empty())
            {
                const ReviewExportResult Export =
                    FindVerifiedReviewExport(
                        Bridge.ReviewPackage, Job.ClipId, "final");
                if (!Export.Success)
                {
                    Job.Errors = Export.Errors;
                }
                else
                {
                    VerifiedFinalFbx = Export.SourceFbx;
                    VerifiedFinalSha256 = Export.ExpectedSha256;
                }
            }
            if (Job.Errors.empty())
            {
                const auto CopyStarted =
                    std::chrono::steady_clock::now();
                const VerifiedExportCopyResult Copy = CopyVerifiedExport({
                    VerifiedFinalFbx,
                    Job.FinalFbx,
                    Bridge.ReviewPackage,
                    VerifiedFinalSha256,
                    false});
                Job.VerifiedExportCopySeconds =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - CopyStarted)
                        .count();
                if (!Copy.Success) Job.Errors = Copy.Errors;
                else Job.FinalFbxSha256 = VerifiedFinalSha256;
            }
        }
        Job.DurationSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - JobStart).count();
        Job.State = Job.Errors.empty()
            ? BatchRetargetJobState::Succeeded
            : BatchRetargetJobState::Failed;
        Status.HasActiveJob = false;
        Recount(Status);
        Status.DurationSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - BatchStart).count();
        Status.WallDurationSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - WallStart).count();
        if (!WriteBatchRetargetStatus(
                Status, Result.StatusJson, StatusError))
        {
            Result.Errors.push_back(StatusError);
            StatusWriteFailed = true;
            break;
        }
    }

    Status.HasActiveJob = false;
    Status.Running = false;
    Status.Complete = !StatusWriteFailed &&
        Status.CompletedJobs == Status.TotalJobs;
    Status.Success = Status.Complete && Status.FailedJobs == 0;
    Status.DurationSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - BatchStart).count();
    Status.WallDurationSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - WallStart).count();
    Status.Errors.insert(
        Status.Errors.end(), Result.Errors.begin(), Result.Errors.end());
    if (!WriteBatchRetargetStatus(Status, Result.StatusJson, StatusError))
    {
        Result.Errors.push_back(StatusError);
        Status.Success = false;
    }
    Result.Status = std::move(Status);
    Result.Success = Result.Status.Success;
    if (!Result.Success && Result.Errors.empty() &&
        Result.Status.FailedJobs > 0)
    {
        Result.Errors.push_back(
            "one or more animations failed; inspect batch_status.json and per-job logs");
    }
    return Result;
}

} // namespace skrtg::viewer
