#include "skrtg/viewer/retarget_bridge.h"

#include "skrtg/viewer/profile/character_profile.h"
#include "skrtg/viewer/retarget_asset_catalog.h"
#include "skrtg/viewer/skrv/package.h"
#include "skrtg/viewer/skrv/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace skrtg::viewer
{
namespace
{
constexpr const char* ExternalRequestSchema =
    "skrtg.native_viewer.retarget_bridge_request.v1";
constexpr const char* UEIKJsonRequestSchema =
    "skrtg.native_viewer.retarget_bridge_request.v2";
constexpr const char* UEIKJsonExactRequestSchema =
    "skrtg.native_viewer.retarget_bridge_request.v3";
constexpr const char* UEIKJsonCatalogRequestSchema =
    "skrtg.native_viewer.retarget_bridge_request.v4";
constexpr const char* UEIKJsonProfileRequestSchema =
    "skrtg.native_viewer.retarget_bridge_request.v5";
constexpr const char* ExternalStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v1";
constexpr const char* UEIKJsonStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v2";
constexpr const char* UEIKJsonExactStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v3";
constexpr const char* UEIKJsonCatalogStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v4";
constexpr const char* UEIKJsonProfileStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v6";

struct CachedHash
{
    std::uintmax_t ByteCount = 0;
    std::filesystem::file_time_type LastWriteTime{};
    std::string Sha256;
};

struct PreflightCache
{
    std::map<std::string, CachedHash> Hashes;
    std::map<std::string, profile::ProfileInspectResult> Profiles;
    std::map<std::string, RetargetAssetCatalogLoadResult> Catalogs;
    std::size_t Hits = 0;
};

bool IsRegularFile(const std::filesystem::path& Path)
{
    std::error_code Error;
    return !Path.empty() &&
        std::filesystem::is_regular_file(Path, Error) && !Error;
}

std::filesystem::path FirstRegular(
    const std::vector<std::filesystem::path>& Candidates)
{
    for (const std::filesystem::path& Candidate : Candidates)
    {
        if (IsRegularFile(Candidate))
            return Candidate.lexically_normal();
    }
    return {};
}

std::filesystem::path FindNodeExecutable(
    const std::filesystem::path& ViewerDirectory)
{
#if defined(_WIN32)
    constexpr const char* NodeName = "node.exe";
    constexpr char PathSeparator = ';';
#else
    constexpr const char* NodeName = "node";
    constexpr char PathSeparator = ':';
#endif
    std::vector<std::filesystem::path> Candidates = {
        ViewerDirectory / NodeName,
        ViewerDirectory / "tools" / NodeName,
        ViewerDirectory / "runtime" / "node" / NodeName,
        ViewerDirectory / "runtime" / "node" / "bin" / NodeName,
        ViewerDirectory / ".." / ".." / "Runtime" / "Node" /
            NodeName};
    std::string PathText;
#if defined(_WIN32)
    char* RawPath = nullptr;
    std::size_t RawPathSize = 0;
    if (_dupenv_s(&RawPath, &RawPathSize, "PATH") == 0 &&
        RawPath != nullptr)
    {
        PathText = RawPath;
        std::free(RawPath);
    }
#else
    if (const char* RawPath = std::getenv("PATH"); RawPath != nullptr)
        PathText = RawPath;
#endif
    if (!PathText.empty())
    {
        std::size_t Begin = 0;
        while (Begin <= PathText.size())
        {
            const std::size_t End = PathText.find(PathSeparator, Begin);
            std::string Entry = PathText.substr(
                Begin, End == std::string::npos
                    ? std::string::npos
                    : End - Begin);
            if (Entry.size() >= 2 && Entry.front() == '"' &&
                Entry.back() == '"')
            {
                Entry = Entry.substr(1, Entry.size() - 2);
            }
            if (!Entry.empty())
            {
                const std::filesystem::path Directory =
                    PathFromUtf8(Entry);
                Candidates.push_back(Directory / NodeName);
                // Codex's local runtime exposes dependency bin folders on
                // PATH while keeping Node in dependencies/node/bin.
                Candidates.push_back(
                    Directory.parent_path().parent_path() /
                    "node" / "bin" / NodeName);
            }
            if (End == std::string::npos) break;
            Begin = End + 1;
        }
    }
    return FirstRegular(Candidates);
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

bool HasProfileExtension(const std::filesystem::path& Path)
{
    return LowerAscii(Path.extension().string()) ==
        ".skrtgprofile";
}

bool IsContractId(const std::string& Value)
{
    return !Value.empty() &&
        std::all_of(
            Value.begin(), Value.end(),
            [](const unsigned char Character)
            {
                return (Character >= 'a' && Character <= 'z') ||
                    (Character >= '0' && Character <= '9') ||
                    Character == '_';
            });
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

std::string CachePathKey(const std::filesystem::path& Path)
{
    std::error_code Error;
    std::filesystem::path Resolved =
        std::filesystem::weakly_canonical(Path, Error);
    if (Error)
    {
        Error.clear();
        Resolved = std::filesystem::absolute(Path, Error);
    }
    std::string Key = PathToUtf8(
        (Error ? Path : Resolved).lexically_normal());
#if defined(_WIN32)
    Key = LowerAscii(std::move(Key));
#endif
    return Key;
}

void AddHashBindingError(
    const std::string& Actual,
    const std::string& Expected,
    const char* Label,
    std::vector<std::string>& Errors)
{
    if (LowerAscii(Actual) != LowerAscii(Expected))
    {
        Errors.push_back(
            std::string(Label) +
            " does not match the selected asset binding SHA-256");
    }
}

void AddFileBindingError(
    const std::filesystem::path& Actual,
    const std::filesystem::path& Expected,
    const char* Label,
    std::vector<std::string>& Errors)
{
    std::error_code Error;
    const bool Equivalent =
        std::filesystem::equivalent(Actual, Expected, Error);
    if (Error || !Equivalent)
    {
        Errors.push_back(
            std::string(Label) +
            " path does not match the selected asset binding");
    }
}

void AddFileError(
    const std::filesystem::path& Path,
    const char* Label,
    std::vector<std::string>& Errors)
{
    if (!IsRegularFile(Path))
        Errors.push_back(std::string(Label) + " is not a readable file: " +
                         PathToUtf8(Path));
}

bool ComputeHash(
    const std::filesystem::path& Path,
    std::string& OutHash,
    std::vector<std::string>& Errors,
    const char* Label,
    PreflightCache* Cache = nullptr)
{
    std::error_code MetadataError;
    const std::uintmax_t ByteCount =
        std::filesystem::file_size(Path, MetadataError);
    const std::filesystem::file_time_type LastWriteTime =
        MetadataError
        ? std::filesystem::file_time_type{}
        : std::filesystem::last_write_time(Path, MetadataError);
    const std::string Key = CachePathKey(Path);
    if (Cache != nullptr && !MetadataError)
    {
        const auto Found = Cache->Hashes.find(Key);
        if (Found != Cache->Hashes.end() &&
            Found->second.ByteCount == ByteCount &&
            Found->second.LastWriteTime == LastWriteTime)
        {
            OutHash = Found->second.Sha256;
            ++Cache->Hits;
            return true;
        }
    }
    std::string Error;
    if (!skrv::Sha256File(Path, OutHash, Error))
    {
        Errors.push_back(std::string("failed to hash ") + Label + ": " +
                         Error);
        return false;
    }
    if (Cache != nullptr && !MetadataError)
    {
        Cache->Hashes[Key] = {
            ByteCount, LastWriteTime, OutHash};
    }
    return true;
}

bool ResolveBoundSkeleton(
    const RetargetAssetCatalog& Catalog,
    const std::string& SkeletonId,
    const std::filesystem::path& ProfilePackage,
    const std::string& ProfilePackageSha256,
    const std::string& ProfileVersion,
    const bool SourceRole,
    RetargetSkeletonAsset& Out,
    std::vector<std::string>& Errors,
    PreflightCache* Cache)
{
    const char* Role = SourceRole ? "source" : "target";
    const std::size_t InitialErrorCount = Errors.size();
    if (ProfilePackage.empty())
    {
        const RetargetSkeletonAsset* CatalogSkeleton =
            FindRetargetSkeletonAsset(Catalog, SkeletonId);
        if (CatalogSkeleton == nullptr)
        {
            Errors.push_back(
                std::string(Role) +
                " skeleton is absent from both the catalog and a "
                "character profile binding");
            return false;
        }
        if ((SourceRole && !CatalogSkeleton->SourceEnabled) ||
            (!SourceRole && !CatalogSkeleton->TargetEnabled))
        {
            Errors.push_back(
                std::string(Role) +
                " skeleton is disabled for the selected role");
            return false;
        }
        Out = *CatalogSkeleton;
        return true;
    }

    const std::string ProfileCacheKey =
        CachePathKey(ProfilePackage) + "#" +
        LowerAscii(ProfilePackageSha256);
    profile::ProfileInspectResult Inspection;
    if (Cache != nullptr)
    {
        const auto Found = Cache->Profiles.find(ProfileCacheKey);
        if (Found != Cache->Profiles.end())
        {
            Inspection = Found->second;
            ++Cache->Hits;
        }
        else
        {
            Inspection = profile::InspectCharacterProfilePackage(
                ProfilePackage);
            Cache->Profiles.emplace(ProfileCacheKey, Inspection);
        }
    }
    else
    {
        Inspection = profile::InspectCharacterProfilePackage(
            ProfilePackage);
    }
    if (!Inspection.Success)
    {
        for (const std::string& Error : Inspection.Errors)
        {
            Errors.push_back(
                std::string(Role) +
                " character profile is invalid: " + Error);
        }
        return false;
    }
    const profile::CharacterProfileDescriptor& Descriptor =
        Inspection.Profile;
    if (LowerAscii(Inspection.PackageSha256) !=
        LowerAscii(ProfilePackageSha256))
    {
        Errors.push_back(
            std::string(Role) +
            " character profile package SHA-256 changed");
    }
    if (Descriptor.ProfileId != SkeletonId ||
        Descriptor.ProfileVersion != ProfileVersion)
    {
        Errors.push_back(
            std::string(Role) +
            " character profile identity does not match the request");
    }
    if ((SourceRole && !Descriptor.SourceEnabled) ||
        (!SourceRole && !Descriptor.TargetEnabled))
    {
        Errors.push_back(
            std::string(Role) +
            " character profile is disabled for the selected role");
    }
    if (Errors.size() != InitialErrorCount) return false;

    const std::filesystem::path ContentRoot =
        ProfilePackage.parent_path() / "content";
    Out.Id = Descriptor.ProfileId;
    Out.Label = Descriptor.DisplayName;
    Out.SkeletonSignatureSha256 =
        Descriptor.SkeletonSignatureSha256;
    Out.RestFbx = (
        ContentRoot / Descriptor.RestFbx.RelativePath)
        .lexically_normal();
    Out.RestFbxSha256 = Descriptor.RestFbx.Sha256;
    Out.IkRigJson = (
        ContentRoot / Descriptor.IkRigJson.RelativePath)
        .lexically_normal();
    Out.IkRigJsonSha256 = Descriptor.IkRigJson.Sha256;
    Out.AlignmentRetargeterJson = (
        ContentRoot /
        Descriptor.AlignmentRetargeterJson.RelativePath)
        .lexically_normal();
    Out.AlignmentRetargeterJsonSha256 =
        Descriptor.AlignmentRetargeterJson.Sha256;
    Out.SourceEnabled = Descriptor.SourceEnabled;
    Out.TargetEnabled = Descriptor.TargetEnabled;
    return true;
}

bool WriteTextAtomic(
    const std::filesystem::path& Path,
    const std::string& Text,
    std::string& OutError)
{
    std::error_code Error;
    if (Path.has_parent_path())
        std::filesystem::create_directories(Path.parent_path(), Error);
    if (Error)
    {
        OutError = "failed to create JSON output directory";
        return false;
    }
    const std::filesystem::path Temporary = Path.string() + ".tmp";
    {
        std::ofstream Stream(Temporary, std::ios::binary | std::ios::trunc);
        if (!Stream)
        {
            OutError = "failed to open temporary JSON output";
            return false;
        }
        Stream.write(Text.data(), static_cast<std::streamsize>(Text.size()));
        if (!Stream)
        {
            OutError = "failed to write temporary JSON output";
            return false;
        }
    }
    std::filesystem::remove(Path, Error);
    Error.clear();
    std::filesystem::rename(Temporary, Path, Error);
    if (Error)
    {
        std::filesystem::remove(Temporary, Error);
        OutError = "failed to commit JSON output";
        return false;
    }
    return true;
}

nlohmann::json RequestJson(const RetargetBridgeRequest& Request)
{
    nlohmann::json Json = {
        {"schema", ExternalRequestSchema},
        {"sourceAnimationFbx", PathToUtf8(Request.SourceAnimationFbx)},
        {"targetSkeletonFbx", PathToUtf8(Request.TargetSkeletonFbx)},
        {"sourceRestFbx", PathToUtf8(Request.SourceRestFbx)},
        {"outputDirectory", PathToUtf8(Request.OutputDirectory)},
        {"clipId", Request.ClipId},
        {"clipLabel", Request.ClipLabel},
        {"enableSpinePelvisFollow", Request.EnableSpinePelvisFollow},
        {"enableSourceMotionFootLock", Request.EnableSourceMotionFootLock},
        {"tools", {
            {"bridgeExecutable", PathToUtf8(Request.Tools.BridgeExecutable)},
            {"retargeterExecutable", PathToUtf8(Request.Tools.RetargeterExecutable)},
            {"nodeExecutable", PathToUtf8(Request.Tools.NodeExecutable)},
            {"adapterScript", PathToUtf8(Request.Tools.AdapterScript)},
            {"skrvPackExecutable", PathToUtf8(Request.Tools.SkrvPackExecutable)},
            {"canonicalJson", PathToUtf8(Request.Tools.CanonicalJson)},
            {"defaultSourceRestFbx", PathToUtf8(Request.Tools.DefaultSourceRestFbx)}
        }}
    };
    if (Request.RouteKind == RetargetBridgeRouteKind::UEIKJsonV1)
    {
        const bool ExactImport =
            Request.SourceFbxImportMode ==
                RetargetBridgeSourceFbxImportMode::
                    UE58ExactGoldenV1 ||
            Request.RestFbxImportMode ==
                RetargetBridgeRestFbxImportMode::
                    UE58ExportedYReflectionV1 ||
            !Request.SourceAnimationGoldenJson.empty();
        Json["schema"] = ExactImport
            ? UEIKJsonExactRequestSchema
            : UEIKJsonRequestSchema;
        Json["routeKind"] = RetargetBridgeRouteKindName(Request.RouteKind);
        Json["animationStack"] = Request.AnimationStack;
        Json["tools"]["ueIkRetargeterExecutable"] =
            PathToUtf8(Request.Tools.UEIKRetargeterExecutable);
        Json["ueIkJson"] = {
            {"sourceRigJson", PathToUtf8(Request.SourceRigJson)},
            {"targetRigJson", PathToUtf8(Request.TargetRigJson)},
            {"sourceAlignmentRetargeterJson",
             PathToUtf8(Request.SourceAlignmentRetargeterJson)},
            {"targetAlignmentRetargeterJson",
             PathToUtf8(Request.TargetAlignmentRetargeterJson)}
        };
        if (ExactImport)
        {
            Json["ueFbxImport"] = {
                {"sourceAnimationMode",
                 RetargetBridgeSourceFbxImportModeName(
                     Request.SourceFbxImportMode)},
                {"restMode",
                 RetargetBridgeRestFbxImportModeName(
                     Request.RestFbxImportMode)},
                {"sourceAnimationGoldenJson",
                 PathToUtf8(
                     Request.SourceAnimationGoldenJson)}
            };
        }
        if (Request.AssetBinding.Required)
        {
            const RetargetBridgeAssetBinding& Binding =
                Request.AssetBinding;
            const bool UsesProfiles =
                !Binding.SourceProfilePackage.empty() ||
                !Binding.TargetProfilePackage.empty();
            Json["schema"] = UsesProfiles
                ? UEIKJsonProfileRequestSchema
                : UEIKJsonCatalogRequestSchema;
            nlohmann::json Selection = {
                {"catalogFile", PathToUtf8(Binding.CatalogFile)},
                {"catalogSha256", Binding.CatalogSha256},
                {"catalogId", Binding.CatalogId},
                {"sourceSkeletonId", Binding.SourceSkeletonId},
                {"targetSkeletonId", Binding.TargetSkeletonId},
                {"sourceAnimationId", Binding.SourceAnimationId},
                {"sourceAnimationSkeletonId",
                 Binding.SourceAnimationSkeletonId},
                {"expectedSha256", {
                    {"sourceAnimation", Binding.SourceAnimationSha256},
                    {"sourceRest", Binding.SourceRestSha256},
                    {"targetRest", Binding.TargetRestSha256},
                    {"sourceRigJson", Binding.SourceRigJsonSha256},
                    {"targetRigJson", Binding.TargetRigJsonSha256},
                    {"sourceAlignmentRetargeterJson",
                     Binding.SourceAlignmentRetargeterJsonSha256},
                    {"targetAlignmentRetargeterJson",
                     Binding.TargetAlignmentRetargeterJsonSha256},
                    {"sourceAnimationGoldenJson",
                     Binding.SourceAnimationGoldenJsonSha256}
                 }}
            };
            if (UsesProfiles)
            {
                Selection["characterProfiles"] = {
                    {"source", {
                        {"packageFile",
                         PathToUtf8(Binding.SourceProfilePackage)},
                        {"packageSha256",
                         Binding.SourceProfilePackageSha256},
                        {"profileVersion",
                         Binding.SourceProfileVersion}
                    }},
                    {"target", {
                        {"packageFile",
                         PathToUtf8(Binding.TargetProfilePackage)},
                        {"packageSha256",
                         Binding.TargetProfilePackageSha256},
                        {"profileVersion",
                         Binding.TargetProfileVersion}
                    }}
                };
            }
            Json["assetSelection"] = std::move(Selection);
        }
    }
    return Json;
}

nlohmann::json RunStatusJson(
    const RetargetBridgeRequest& Request,
    const RetargetBridgePreflight& Preflight,
    const RetargetBridgeRunResult& Result)
{
    const bool UEIKJson =
        Request.RouteKind == RetargetBridgeRouteKind::UEIKJsonV1;
    const bool ExactImport =
        UEIKJson &&
        Request.SourceFbxImportMode ==
            RetargetBridgeSourceFbxImportMode::
                UE58ExactGoldenV1;
    const bool UsesProfiles =
        !Request.AssetBinding.SourceProfilePackage.empty() ||
        !Request.AssetBinding.TargetProfilePackage.empty();
    nlohmann::json Status = {
        {"schema", Request.AssetBinding.Required
            ? ((!Request.AssetBinding.SourceProfilePackage.empty() ||
                !Request.AssetBinding.TargetProfilePackage.empty())
                ? UEIKJsonProfileStatusSchema
                : UEIKJsonCatalogStatusSchema)
            : (ExactImport
            ? UEIKJsonExactStatusSchema
            : (UEIKJson
                ? UEIKJsonStatusSchema
                : ExternalStatusSchema))},
        {"routeKind", RetargetBridgeRouteKindName(Request.RouteKind)},
        {"assetSelection", {
            {"required", Request.AssetBinding.Required},
            {"catalogFile",
             PathToUtf8(Request.AssetBinding.CatalogFile)},
            {"catalogSha256",
             Request.AssetBinding.CatalogSha256},
            {"catalogId", Request.AssetBinding.CatalogId},
            {"sourceSkeletonId",
             Request.AssetBinding.SourceSkeletonId},
            {"targetSkeletonId",
             Request.AssetBinding.TargetSkeletonId},
            {"sourceAnimationId",
             Request.AssetBinding.SourceAnimationId},
            {"sourceAnimationSkeletonId",
             Request.AssetBinding.SourceAnimationSkeletonId},
            {"sourceProfilePackage",
             PathToUtf8(
                 Request.AssetBinding.SourceProfilePackage)},
            {"sourceProfilePackageSha256",
             Request.AssetBinding.SourceProfilePackageSha256},
            {"sourceProfileVersion",
             Request.AssetBinding.SourceProfileVersion},
            {"targetProfilePackage",
             PathToUtf8(
                 Request.AssetBinding.TargetProfilePackage)},
            {"targetProfilePackageSha256",
             Request.AssetBinding.TargetProfilePackageSha256},
            {"targetProfileVersion",
             Request.AssetBinding.TargetProfileVersion}
        }},
        {"success", Result.Success},
        {"reviewPackage", PathToUtf8(Result.ReviewPackage)},
        {"sourceAnimationFbx", PathToUtf8(Request.SourceAnimationFbx)},
        {"assetCatalogSha256", Preflight.AssetCatalogSha256},
        {"sourceProfilePackageSha256",
         Preflight.SourceProfilePackageSha256},
        {"targetProfilePackageSha256",
         Preflight.TargetProfilePackageSha256},
        {"sourceAnimationSha256", Preflight.SourceAnimationSha256},
        {"sourceRestFbx", PathToUtf8(Request.SourceRestFbx)},
        {"sourceRestSha256", Preflight.SourceRestSha256},
        {"targetSkeletonFbx", PathToUtf8(Request.TargetSkeletonFbx)},
        {"targetSkeletonSha256", Preflight.TargetSkeletonSha256},
        {"canonicalSha256", Preflight.CanonicalSha256},
        {"sourceRigJsonSha256", Preflight.SourceRigJsonSha256},
        {"targetRigJsonSha256", Preflight.TargetRigJsonSha256},
        {"sourceAlignmentRetargeterJsonSha256",
         Preflight.SourceAlignmentRetargeterJsonSha256},
        {"targetAlignmentRetargeterJsonSha256",
         Preflight.TargetAlignmentRetargeterJsonSha256},
        {"sourceAnimationGoldenJsonSha256",
         Preflight.SourceAnimationGoldenJsonSha256},
        {"sourceFbxImportMode",
         RetargetBridgeSourceFbxImportModeName(
             Request.SourceFbxImportMode)},
        {"restFbxImportMode",
         RetargetBridgeRestFbxImportModeName(
             Request.RestFbxImportMode)},
        {"foundationRoute", UEIKJson
            ? "ue_ik_json_fk_pelvis_limb_ik_candidate_v1"
            : "skrtg_fkik_foundation_v1"},
        {"foundationFrozen", !UEIKJson},
        {"routeSelected", false},
        {"routeAdopted", false},
        {"skrvV1Modified", false},
        {"n3Started", false},
        {"n4Started", true},
        {"n4Scope", "viewer-ui-playback-operation-stack-verified-export"},
        {"retargeterExitCode", Result.RetargeterExitCode},
        {"adapterExitCode", Result.AdapterExitCode},
        {"packExitCode", Result.PackExitCode},
        {"logs", {
            {"retargeter", PathToUtf8(Result.RetargeterLog)},
            {"adapter", PathToUtf8(Result.AdapterLog)},
            {"pack", PathToUtf8(Result.PackLog)}
        }},
        {"errors", Result.Errors}
    };
    if (!UsesProfiles)
    {
        Status["assetSelection"].erase(
            "sourceProfilePackage");
        Status["assetSelection"].erase(
            "sourceProfilePackageSha256");
        Status["assetSelection"].erase(
            "sourceProfileVersion");
        Status["assetSelection"].erase(
            "targetProfilePackage");
        Status["assetSelection"].erase(
            "targetProfilePackageSha256");
        Status["assetSelection"].erase(
            "targetProfileVersion");
        Status.erase("sourceProfilePackageSha256");
        Status.erase("targetProfilePackageSha256");
    }
    else
    {
        Status["verifiedFinalFbx"] =
            PathToUtf8(Result.VerifiedFinalFbx);
        Status["verifiedFinalFbxSha256"] =
            Result.VerifiedFinalFbxSha256;
        Status["timings"] = {
            {"preflightReused", Result.Timings.PreflightReused},
            {"preflightSeconds", Result.Timings.PreflightSeconds},
            {"retargetWorkerSeconds",
             Result.Timings.RetargetWorkerSeconds},
            {"adapterSeconds", Result.Timings.AdapterSeconds},
            {"packSeconds", Result.Timings.PackSeconds},
            {"packageInspectSeconds",
             Result.Timings.PackageInspectSeconds},
            {"totalSeconds", Result.Timings.TotalSeconds}};
    }
    return Status;
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

std::string ExportFileName(const RetargetBridgeRequest& Request)
{
    return "SKRTG_Final__" + Request.ClipId +
        (Request.RouteKind == RetargetBridgeRouteKind::UEIKJsonV1
            ? "__UEIK_Target.fbx"
            : "__External_Target.fbx");
}

std::string FoundationExportFileName(
    const RetargetBridgeRequest& Request)
{
    return "SKRTG_Foundation__" + Request.ClipId +
        (Request.RouteKind == RetargetBridgeRouteKind::UEIKJsonV1
            ? "__UEIK_Target.fbx"
            : "__External_Target.fbx");
}

bool FindSealedVerifiedExport(
    const std::filesystem::path& PackageDirectory,
    const std::vector<skrv::IntegrityEntry>& Entries,
    const std::string& ClipId,
    const std::string& Lane,
    std::filesystem::path& OutPath,
    std::string& OutSha256,
    std::vector<std::string>& Errors)
{
    try
    {
        std::ifstream Stream(
            PackageDirectory / "manifest.json", std::ios::binary);
        if (!Stream)
        {
            Errors.push_back("sealed SKRV manifest is not readable");
            return false;
        }
        const nlohmann::json Manifest = nlohmann::json::parse(Stream);
        for (const nlohmann::json& Export :
             Manifest.at("verifiedExports"))
        {
            if (Export.at("clip_id").get<std::string>() != ClipId ||
                Export.at("lane").get<std::string>() != Lane)
            {
                continue;
            }
            const std::filesystem::path Relative = PathFromUtf8(
                Export.at("path").get<std::string>());
            const std::string Expected = Export.at("sha256")
                .get<std::string>();
            const std::string RelativeKey =
                LowerAscii(PathToUtf8(Relative.lexically_normal()));
            const auto Entry = std::find_if(
                Entries.begin(), Entries.end(),
                [&](const skrv::IntegrityEntry& Candidate)
                {
                    return Candidate.Role == skrv::EntryRole::Export &&
                        LowerAscii(PathToUtf8(
                            Candidate.RelativePath.lexically_normal())) ==
                            RelativeKey &&
                        LowerAscii(Candidate.Sha256) ==
                            LowerAscii(Expected);
                });
            if (Entry == Entries.end())
            {
                Errors.push_back(
                    "sealed verified export does not match the integrity index");
                return false;
            }
            OutPath = PackageDirectory / Entry->RelativePath;
            OutSha256 = Entry->Sha256;
            std::error_code FileError;
            if (!std::filesystem::is_regular_file(OutPath, FileError) ||
                FileError)
            {
                Errors.push_back(
                    "sealed verified export is no longer readable");
                return false;
            }
            return true;
        }
        Errors.push_back(
            "sealed SKRV has no verified export for clip/lane");
    }
    catch (const std::exception& Error)
    {
        Errors.push_back(
            "sealed verified export metadata is invalid: " +
            std::string(Error.what()));
    }
    return false;
}
} // namespace

std::string PathToUtf8(const std::filesystem::path& Path)
{
#if defined(_WIN32)
    const std::u8string Value = Path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(Value.data()), Value.size());
#else
    return Path.generic_string();
#endif
}

std::filesystem::path PathFromUtf8(const std::string& Text)
{
#if defined(_WIN32)
    const std::u8string Value(
        reinterpret_cast<const char8_t*>(Text.data()), Text.size());
    return std::filesystem::path(Value);
#else
    return std::filesystem::path(Text);
#endif
}

const char* RetargetBridgeRouteKindName(
    const RetargetBridgeRouteKind Kind)
{
    switch (Kind)
    {
        case RetargetBridgeRouteKind::ExternalFoundationV1:
            return "external_foundation_v1";
        case RetargetBridgeRouteKind::UEIKJsonV1:
            return "ue_ik_json_v1";
    }
    return "unknown";
}

bool ParseRetargetBridgeRouteKind(
    const std::string& Text,
    RetargetBridgeRouteKind& OutKind)
{
    if (Text == "external_foundation_v1")
    {
        OutKind = RetargetBridgeRouteKind::ExternalFoundationV1;
        return true;
    }
    if (Text == "ue_ik_json_v1")
    {
        OutKind = RetargetBridgeRouteKind::UEIKJsonV1;
        return true;
    }
    return false;
}

const char* RetargetBridgeSourceFbxImportModeName(
    const RetargetBridgeSourceFbxImportMode Mode)
{
    switch (Mode)
    {
        case RetargetBridgeSourceFbxImportMode::
            FbxBodyBasisV7:
            return "fbx_body_basis_v7";
        case RetargetBridgeSourceFbxImportMode::
            UE58ExactGoldenV1:
            return "ue5.8_exact_golden_v1";
    }
    return "unknown";
}

bool ParseRetargetBridgeSourceFbxImportMode(
    const std::string& Text,
    RetargetBridgeSourceFbxImportMode& OutMode)
{
    if (Text == "fbx_body_basis_v7")
    {
        OutMode =
            RetargetBridgeSourceFbxImportMode::
                FbxBodyBasisV7;
        return true;
    }
    if (Text == "ue5.8_exact_golden_v1")
    {
        OutMode =
            RetargetBridgeSourceFbxImportMode::
                UE58ExactGoldenV1;
        return true;
    }
    return false;
}

const char* RetargetBridgeRestFbxImportModeName(
    const RetargetBridgeRestFbxImportMode Mode)
{
    switch (Mode)
    {
        case RetargetBridgeRestFbxImportMode::
            ReconciledRestV1:
            return "reconciled_rest_v1";
        case RetargetBridgeRestFbxImportMode::
            UE58ExportedYReflectionV1:
            return "ue5.8_exported_y_reflection_v1";
    }
    return "unknown";
}

bool ParseRetargetBridgeRestFbxImportMode(
    const std::string& Text,
    RetargetBridgeRestFbxImportMode& OutMode)
{
    if (Text == "reconciled_rest_v1")
    {
        OutMode =
            RetargetBridgeRestFbxImportMode::
                ReconciledRestV1;
        return true;
    }
    if (Text == "ue5.8_exported_y_reflection_v1")
    {
        OutMode =
            RetargetBridgeRestFbxImportMode::
                UE58ExportedYReflectionV1;
        return true;
    }
    return false;
}

RetargetBridgeTools DiscoverRetargetBridgeTools(
    const std::filesystem::path& ViewerExecutable)
{
    const std::filesystem::path Directory =
        std::filesystem::absolute(ViewerExecutable).parent_path();
    const std::filesystem::path Repository =
        (Directory / ".." / "..").lexically_normal();
    RetargetBridgeTools Tools;
#if defined(_WIN32)
    constexpr const char* BridgeName = "skrtg_retarget_bridge.exe";
    constexpr const char* RetargeterName =
        "skrtg_external_foundation_worker.exe";
    constexpr const char* UEIKRetargeterName =
        "skrtg_ueik_retarget_worker.exe";
    constexpr const char* PackName = "skrv_pack.exe";
#else
    constexpr const char* BridgeName = "skrtg_retarget_bridge";
    constexpr const char* RetargeterName =
        "skrtg_external_foundation_worker";
    constexpr const char* UEIKRetargeterName =
        "skrtg_ueik_retarget_worker";
    constexpr const char* PackName = "skrv_pack";
#endif
    Tools.BridgeExecutable = FirstRegular({
        Directory / BridgeName,
        Directory / "tools" / BridgeName});
    Tools.NodeExecutable = FindNodeExecutable(Directory);
    Tools.RetargeterExecutable = FirstRegular({
        Directory / RetargeterName,
        Directory / "tools" / RetargeterName,
        Repository / "build" / "phase0" / "Release" / RetargeterName,
        Repository / "build" / "phase0" / "Debug" / RetargeterName});
    Tools.UEIKRetargeterExecutable = FirstRegular({
        Directory / UEIKRetargeterName,
        Directory / "tools" / UEIKRetargeterName,
        Repository / "build" / "retargeter" / "Release" /
            UEIKRetargeterName,
        Repository / "build" / "retargeter" / "Debug" /
            UEIKRetargeterName,
        Directory / ".." / ".." / "retargeter" / "Release" /
            UEIKRetargeterName,
        Directory / ".." / ".." / "retargeter" / "Debug" /
            UEIKRetargeterName});
    Tools.SkrvPackExecutable = FirstRegular({
        Directory / PackName,
        Directory / "tools" / PackName});
    Tools.AdapterScript = FirstRegular({
        Directory / "scripts" / "review_html_to_skrv_payload.js",
        Directory / ".." / "scripts" /
            "review_html_to_skrv_payload.js",
        Repository / "native_viewer" / "scripts" /
            "review_html_to_skrv_payload.js"});
    Tools.CanonicalJson.clear();
    Tools.DefaultSourceRestFbx.clear();
    return Tools;
}

std::string MakeBridgeClipId(const std::filesystem::path& AnimationPath)
{
    const std::string Stem = LowerAscii(
        PathToUtf8(AnimationPath.stem()));
    std::string Result;
    bool LastUnderscore = false;
    for (const unsigned char Character : Stem)
    {
        const bool Alphanumeric =
            (Character >= 'a' && Character <= 'z') ||
            (Character >= '0' && Character <= '9');
        if (Alphanumeric)
        {
            Result.push_back(static_cast<char>(Character));
            LastUnderscore = false;
        }
        else if (!Result.empty() && !LastUnderscore)
        {
            Result.push_back('_');
            LastUnderscore = true;
        }
        if (Result.size() >= 80) break;
    }
    while (!Result.empty() && Result.back() == '_') Result.pop_back();
    return Result.empty() ? "selected_animation" : Result;
}

std::string RequestIdentity(const RetargetBridgeRequest& Request)
{
    const auto PathIdentity =
        [](const std::filesystem::path& Path)
        {
            if (Path.empty()) return std::string();
            std::error_code Error;
            std::filesystem::path Resolved = Path.is_absolute()
                ? Path
                : std::filesystem::absolute(Path, Error);
            std::string Result = PathToUtf8(
                (Error ? Path : Resolved).lexically_normal());
#if defined(_WIN32)
            Result = LowerAscii(std::move(Result));
#endif
            return Result;
        };
    const RetargetBridgeAssetBinding& Binding = Request.AssetBinding;
    const nlohmann::json Identity = {
        {"routeKind", RetargetBridgeRouteKindName(Request.RouteKind)},
        {"sourceAnimationFbx", PathIdentity(Request.SourceAnimationFbx)},
        {"targetSkeletonFbx", PathIdentity(Request.TargetSkeletonFbx)},
        {"sourceRestFbx", PathIdentity(Request.SourceRestFbx)},
        {"sourceRigJson", PathIdentity(Request.SourceRigJson)},
        {"targetRigJson", PathIdentity(Request.TargetRigJson)},
        {"sourceAlignmentRetargeterJson",
         PathIdentity(Request.SourceAlignmentRetargeterJson)},
        {"targetAlignmentRetargeterJson",
         PathIdentity(Request.TargetAlignmentRetargeterJson)},
        {"sourceAnimationGoldenJson",
         PathIdentity(Request.SourceAnimationGoldenJson)},
        {"outputDirectory", PathIdentity(Request.OutputDirectory)},
        {"clipId", Request.ClipId},
        {"clipLabel", Request.ClipLabel},
        {"animationStack", Request.AnimationStack},
        {"sourceFbxImportMode",
         RetargetBridgeSourceFbxImportModeName(
             Request.SourceFbxImportMode)},
        {"restFbxImportMode",
         RetargetBridgeRestFbxImportModeName(Request.RestFbxImportMode)},
        {"enableSpinePelvisFollow", Request.EnableSpinePelvisFollow},
        {"enableSourceMotionFootLock", Request.EnableSourceMotionFootLock},
        {"tools", {
            {"bridgeExecutable", PathIdentity(Request.Tools.BridgeExecutable)},
            {"retargeterExecutable",
             PathIdentity(Request.Tools.RetargeterExecutable)},
            {"ueIkRetargeterExecutable",
             PathIdentity(Request.Tools.UEIKRetargeterExecutable)},
            {"nodeExecutable", PathIdentity(Request.Tools.NodeExecutable)},
            {"adapterScript", PathIdentity(Request.Tools.AdapterScript)},
            {"skrvPackExecutable",
             PathIdentity(Request.Tools.SkrvPackExecutable)},
            {"canonicalJson", PathIdentity(Request.Tools.CanonicalJson)},
            {"defaultSourceRestFbx",
             PathIdentity(Request.Tools.DefaultSourceRestFbx)}
        }},
        {"assetBinding", {
            {"required", Binding.Required},
            {"catalogFile", PathIdentity(Binding.CatalogFile)},
            {"catalogSha256", LowerAscii(Binding.CatalogSha256)},
            {"catalogId", Binding.CatalogId},
            {"sourceSkeletonId", Binding.SourceSkeletonId},
            {"targetSkeletonId", Binding.TargetSkeletonId},
            {"sourceAnimationId", Binding.SourceAnimationId},
            {"sourceAnimationSkeletonId",
             Binding.SourceAnimationSkeletonId},
            {"sourceProfilePackage",
             PathIdentity(Binding.SourceProfilePackage)},
            {"sourceProfilePackageSha256",
             LowerAscii(Binding.SourceProfilePackageSha256)},
            {"sourceProfileVersion", Binding.SourceProfileVersion},
            {"targetProfilePackage",
             PathIdentity(Binding.TargetProfilePackage)},
            {"targetProfilePackageSha256",
             LowerAscii(Binding.TargetProfilePackageSha256)},
            {"targetProfileVersion", Binding.TargetProfileVersion},
            {"sourceAnimationSha256",
             LowerAscii(Binding.SourceAnimationSha256)},
            {"sourceRestSha256", LowerAscii(Binding.SourceRestSha256)},
            {"targetRestSha256", LowerAscii(Binding.TargetRestSha256)},
            {"sourceRigJsonSha256",
             LowerAscii(Binding.SourceRigJsonSha256)},
            {"targetRigJsonSha256",
             LowerAscii(Binding.TargetRigJsonSha256)},
            {"sourceAlignmentRetargeterJsonSha256",
             LowerAscii(Binding.SourceAlignmentRetargeterJsonSha256)},
            {"targetAlignmentRetargeterJsonSha256",
             LowerAscii(Binding.TargetAlignmentRetargeterJsonSha256)},
            {"sourceAnimationGoldenJsonSha256",
             LowerAscii(Binding.SourceAnimationGoldenJsonSha256)}
        }}
    };
    return Identity.dump();
}

static RetargetBridgePreflight PreflightRetargetBridgeImpl(
    const RetargetBridgeRequest& Request,
    PreflightCache* Cache)
{
    const auto Started = std::chrono::steady_clock::now();
    const std::size_t InitialCacheHits =
        Cache == nullptr ? 0 : Cache->Hits;
    RetargetBridgePreflight Result;
    const auto Finish = [&]() -> RetargetBridgePreflight
    {
        Result.DurationSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - Started).count();
        Result.CacheHits = Cache == nullptr
            ? 0 : Cache->Hits - InitialCacheHits;
        return std::move(Result);
    };
    Result.RequestIdentity = RequestIdentity(Request);
    const bool UEIKJson =
        Request.RouteKind == RetargetBridgeRouteKind::UEIKJsonV1;
    const bool ExactSourceImport =
        UEIKJson &&
        Request.SourceFbxImportMode ==
            RetargetBridgeSourceFbxImportMode::
                UE58ExactGoldenV1;
    const bool UE58ExportedRestImport =
        UEIKJson &&
        Request.RestFbxImportMode ==
            RetargetBridgeRestFbxImportMode::
                UE58ExportedYReflectionV1;
    if (Request.AssetBinding.Required)
    {
        const RetargetBridgeAssetBinding& Binding =
            Request.AssetBinding;
        if (!UEIKJson)
        {
            Result.Errors.push_back(
                "asset catalog binding is supported only by the "
                "UE IK JSON route");
        }
        if (!ExactSourceImport || !UE58ExportedRestImport)
        {
            Result.Errors.push_back(
                "asset catalog binding requires the exact UE 5.8 "
                "animation and exported-rest import modes");
        }
        if (!IsContractId(Binding.CatalogId) ||
            !profile::IsCharacterProfileId(
                Binding.SourceSkeletonId) ||
            !profile::IsCharacterProfileId(
                Binding.TargetSkeletonId) ||
            !IsContractId(Binding.SourceAnimationId) ||
            !profile::IsCharacterProfileId(
                Binding.SourceAnimationSkeletonId))
        {
            Result.Errors.push_back(
                "asset selection IDs are invalid or outside the "
                "portable lowercase profile-id contract");
        }
        if (!IsSha256(Binding.CatalogSha256))
        {
            Result.Errors.push_back(
                "asset catalog SHA-256 must contain 64 hexadecimal "
                "characters");
        }
        if (Binding.SourceSkeletonId !=
            Binding.SourceAnimationSkeletonId)
        {
            Result.Errors.push_back(
                "selected animation does not belong to the selected "
                "source skeleton");
        }
        const std::vector<std::pair<const char*, const std::string*>>
            HashBindings = {
                {"source animation", &Binding.SourceAnimationSha256},
                {"source rest", &Binding.SourceRestSha256},
                {"target rest", &Binding.TargetRestSha256},
                {"source IK Rig JSON",
                 &Binding.SourceRigJsonSha256},
                {"target IK Rig JSON",
                 &Binding.TargetRigJsonSha256},
                {"source alignment Retargeter JSON",
                 &Binding.SourceAlignmentRetargeterJsonSha256},
                {"target alignment Retargeter JSON",
                 &Binding.TargetAlignmentRetargeterJsonSha256},
                {"source animation golden JSON",
                 &Binding.SourceAnimationGoldenJsonSha256}};
        for (const auto& [Label, Hash] : HashBindings)
        {
            if (!IsSha256(*Hash))
            {
                Result.Errors.push_back(
                    std::string("asset catalog expected SHA-256 is "
                                "invalid for ") + Label);
            }
        }
        const auto ValidateProfileBinding =
            [&](const std::filesystem::path& Package,
                const std::string& Hash,
                const std::string& Version,
                const char* Label)
            {
                if (Package.empty())
                {
                    if (!Hash.empty() || !Version.empty())
                    {
                        Result.Errors.push_back(
                            std::string(Label) +
                            " profile binding is incomplete");
                    }
                    return;
                }
                if (!HasProfileExtension(Package))
                {
                    Result.Errors.push_back(
                        std::string(Label) +
                        " profile package must use .skrtgprofile");
                }
                if (!IsSha256(Hash))
                {
                    Result.Errors.push_back(
                        std::string(Label) +
                        " profile package SHA-256 is invalid");
                }
                if (!profile::IsCharacterProfileVersion(Version))
                {
                    Result.Errors.push_back(
                        std::string(Label) +
                        " profile version is invalid");
                }
            };
        ValidateProfileBinding(
            Binding.SourceProfilePackage,
            Binding.SourceProfilePackageSha256,
            Binding.SourceProfileVersion, "source");
        ValidateProfileBinding(
            Binding.TargetProfilePackage,
            Binding.TargetProfilePackageSha256,
            Binding.TargetProfileVersion, "target");
    }
    if (Request.AssetBinding.Required)
    {
        AddFileError(
            Request.AssetBinding.CatalogFile,
            "retarget asset catalog JSON", Result.Errors);
        if (!HasJsonExtension(Request.AssetBinding.CatalogFile))
        {
            Result.Errors.push_back(
                "retarget asset catalog must use an exported .json "
                "file");
        }
        if (!Request.AssetBinding.SourceProfilePackage.empty())
        {
            AddFileError(
                Request.AssetBinding.SourceProfilePackage,
                "source character profile package",
                Result.Errors);
        }
        if (!Request.AssetBinding.TargetProfilePackage.empty())
        {
            AddFileError(
                Request.AssetBinding.TargetProfilePackage,
                "target character profile package",
                Result.Errors);
        }
    }
    AddFileError(
        Request.SourceAnimationFbx, "source animation FBX", Result.Errors);
    AddFileError(
        Request.TargetSkeletonFbx, "target skeleton FBX", Result.Errors);
    AddFileError(
        Request.SourceRestFbx, "source rest FBX", Result.Errors);
    AddFileError(
        Request.Tools.BridgeExecutable,
        "retarget bridge executable", Result.Errors);
    AddFileError(
        UEIKJson
            ? Request.Tools.UEIKRetargeterExecutable
            : Request.Tools.RetargeterExecutable,
        UEIKJson
            ? "UE IK JSON Retargeter executable"
            : "external Foundation provider executable",
        Result.Errors);
    AddFileError(
        Request.Tools.AdapterScript,
        "frozen HTML-to-SKRV adapter", Result.Errors);
    AddFileError(
        Request.Tools.SkrvPackExecutable,
        "SKRV pack executable", Result.Errors);
    if (UEIKJson)
    {
        AddFileError(
            Request.SourceRigJson, "source IK Rig JSON", Result.Errors);
        AddFileError(
            Request.TargetRigJson, "target IK Rig JSON", Result.Errors);
        AddFileError(
            Request.SourceAlignmentRetargeterJson,
            "source alignment IK Retargeter JSON", Result.Errors);
        AddFileError(
            Request.TargetAlignmentRetargeterJson,
            "target alignment IK Retargeter JSON", Result.Errors);
        if (ExactSourceImport)
        {
            AddFileError(
                Request.SourceAnimationGoldenJson,
                "source animation golden JSON", Result.Errors);
        }
    }
    else
    {
        Result.Errors.push_back(
            "external_foundation_v1 requires a separately distributed "
            "provider and data contract; it is not bundled in this source "
            "release");
    }
    AddFileError(
        Request.Tools.NodeExecutable,
        "adapter Node executable", Result.Errors);
    if (!HasFbxExtension(Request.SourceAnimationFbx))
        Result.Errors.push_back("source animation must have .fbx extension");
    if (!HasFbxExtension(Request.TargetSkeletonFbx))
        Result.Errors.push_back("target skeleton must have .fbx extension");
    if (!HasFbxExtension(Request.SourceRestFbx))
        Result.Errors.push_back("source rest must have .fbx extension");
    if (UEIKJson &&
        (!HasJsonExtension(Request.SourceRigJson) ||
         !HasJsonExtension(Request.TargetRigJson) ||
         !HasJsonExtension(Request.SourceAlignmentRetargeterJson) ||
         !HasJsonExtension(Request.TargetAlignmentRetargeterJson)))
    {
        Result.Errors.push_back(
            "UE IK Rig and IK Retargeter definitions must use exported "
            ".json files");
    }
    if (ExactSourceImport &&
        !HasJsonExtension(Request.SourceAnimationGoldenJson))
    {
        Result.Errors.push_back(
            "exact UE 5.8 source animation import requires an "
            "exported .animgolden.json file");
    }
    if (UE58ExportedRestImport && !ExactSourceImport)
    {
        Result.Errors.push_back(
            "UE 5.8 exported-rest Y-reflection mode requires the "
            "exact UE 5.8 source animation golden mode");
    }
    if (Request.OutputDirectory.empty())
        Result.Errors.push_back("output directory is empty");
    else if (!DirectoryEmptyOrAbsent(Request.OutputDirectory))
        Result.Errors.push_back(
            "output directory must be absent or empty (fail-closed overwrite policy)");
#if defined(_WIN32)
    if (!Request.OutputDirectory.empty())
    {
        std::error_code AbsoluteError;
        const std::filesystem::path AbsoluteOutput =
            std::filesystem::absolute(
                Request.OutputDirectory, AbsoluteError);
        if (!AbsoluteError)
        {
            const std::filesystem::path ExpectedFinalFbx =
                AbsoluteOutput / "retargeter" / "Review" /
                "FinalFBX" / ExportFileName(Request);
            const std::filesystem::path ExpectedFoundationFbx =
                AbsoluteOutput / "retargeter" / "Review" /
                "FinalFBX" / FoundationExportFileName(Request);
            if (ExpectedFinalFbx.native().size() >= 248 ||
                ExpectedFoundationFbx.native().size() >= 248)
            {
                Result.Errors.push_back(
                    "output path is too long for the FBX SDK export "
                    "chain; choose a shorter output root (expected "
                    "Final and Foundation FBX paths must both stay "
                    "below 248 characters)");
            }
        }
    }
#endif
    if (Request.ClipId.empty() || Request.ClipLabel.empty())
        Result.Errors.push_back("clip ID and label are required");
    const std::set<char> Allowed = {
        'a','b','c','d','e','f','g','h','i','j','k','l','m',
        'n','o','p','q','r','s','t','u','v','w','x','y','z',
        '0','1','2','3','4','5','6','7','8','9','_'};
    if (std::any_of(
            Request.ClipId.begin(), Request.ClipId.end(),
            [&](const char Character) { return !Allowed.contains(Character); }))
    {
        Result.Errors.push_back(
            "clip ID may contain only lowercase ASCII letters, digits, and underscore");
    }
    if (UEIKJson &&
        (Request.EnableSpinePelvisFollow ||
         Request.EnableSourceMotionFootLock))
    {
        Result.Errors.push_back(
            "UE IK JSON candidate route does not enable the frozen "
            "Spine/Pelvis or FootLock modules");
    }
    else if (Request.EnableSourceMotionFootLock &&
             !Request.EnableSpinePelvisFollow)
    {
        Result.Errors.push_back(
            "frozen FootLock bridge requires the frozen Spine/Pelvis prerequisite");
    }
    if (!Result.Errors.empty()) return Finish();

    if (Request.AssetBinding.Required)
    {
        ComputeHash(
            Request.AssetBinding.CatalogFile,
            Result.AssetCatalogSha256,
            Result.Errors, "retarget asset catalog JSON", Cache);
        if (!Request.AssetBinding.SourceProfilePackage.empty())
        {
            ComputeHash(
                Request.AssetBinding.SourceProfilePackage,
                Result.SourceProfilePackageSha256,
                Result.Errors,
                "source character profile package", Cache);
        }
        if (!Request.AssetBinding.TargetProfilePackage.empty())
        {
            ComputeHash(
                Request.AssetBinding.TargetProfilePackage,
                Result.TargetProfilePackageSha256,
                Result.Errors,
                "target character profile package", Cache);
        }
    }
    ComputeHash(
        Request.SourceAnimationFbx,
        Result.SourceAnimationSha256, Result.Errors, "source animation",
        Cache);
    ComputeHash(
        Request.SourceRestFbx,
        Result.SourceRestSha256, Result.Errors, "source rest", Cache);
    ComputeHash(
        Request.TargetSkeletonFbx,
        Result.TargetSkeletonSha256, Result.Errors, "target skeleton",
        Cache);
    if (UEIKJson)
    {
        ComputeHash(
            Request.SourceRigJson,
            Result.SourceRigJsonSha256, Result.Errors,
            "source IK Rig JSON", Cache);
        ComputeHash(
            Request.TargetRigJson,
            Result.TargetRigJsonSha256, Result.Errors,
            "target IK Rig JSON", Cache);
        ComputeHash(
            Request.SourceAlignmentRetargeterJson,
            Result.SourceAlignmentRetargeterJsonSha256, Result.Errors,
            "source alignment IK Retargeter JSON", Cache);
        ComputeHash(
            Request.TargetAlignmentRetargeterJson,
            Result.TargetAlignmentRetargeterJsonSha256, Result.Errors,
            "target alignment IK Retargeter JSON", Cache);
        if (ExactSourceImport)
        {
            ComputeHash(
                Request.SourceAnimationGoldenJson,
                Result.SourceAnimationGoldenJsonSha256,
                Result.Errors,
                "source animation golden JSON", Cache);
        }
    }
    else
    {
        ComputeHash(
            Request.Tools.CanonicalJson,
            Result.CanonicalSha256, Result.Errors, "frozen canonical",
            Cache);
    }
    if (!Result.Errors.empty()) return Finish();

    if (Request.AssetBinding.Required)
    {
        const RetargetBridgeAssetBinding& Binding =
            Request.AssetBinding;
        AddHashBindingError(
            Result.AssetCatalogSha256,
            Binding.CatalogSha256,
            "retarget asset catalog JSON", Result.Errors);
        AddHashBindingError(
            Result.SourceAnimationSha256,
            Binding.SourceAnimationSha256,
            "source animation", Result.Errors);
        AddHashBindingError(
            Result.SourceRestSha256,
            Binding.SourceRestSha256,
            "source rest", Result.Errors);
        AddHashBindingError(
            Result.TargetSkeletonSha256,
            Binding.TargetRestSha256,
            "target rest", Result.Errors);
        AddHashBindingError(
            Result.SourceRigJsonSha256,
            Binding.SourceRigJsonSha256,
            "source IK Rig JSON", Result.Errors);
        AddHashBindingError(
            Result.TargetRigJsonSha256,
            Binding.TargetRigJsonSha256,
            "target IK Rig JSON", Result.Errors);
        AddHashBindingError(
            Result.SourceAlignmentRetargeterJsonSha256,
            Binding.SourceAlignmentRetargeterJsonSha256,
            "source alignment Retargeter JSON", Result.Errors);
        AddHashBindingError(
            Result.TargetAlignmentRetargeterJsonSha256,
            Binding.TargetAlignmentRetargeterJsonSha256,
            "target alignment Retargeter JSON", Result.Errors);
        AddHashBindingError(
            Result.SourceAnimationGoldenJsonSha256,
            Binding.SourceAnimationGoldenJsonSha256,
            "source animation golden JSON", Result.Errors);
        if (!Binding.SourceProfilePackage.empty())
        {
            AddHashBindingError(
                Result.SourceProfilePackageSha256,
                Binding.SourceProfilePackageSha256,
                "source character profile package",
                Result.Errors);
        }
        if (!Binding.TargetProfilePackage.empty())
        {
            AddHashBindingError(
                Result.TargetProfilePackageSha256,
                Binding.TargetProfilePackageSha256,
                "target character profile package",
                Result.Errors);
        }

        std::vector<std::string> ExternalSkeletonIds;
        if (!Binding.SourceProfilePackage.empty())
            ExternalSkeletonIds.push_back(
                Binding.SourceSkeletonId);
        if (!Binding.TargetProfilePackage.empty() &&
            Binding.TargetSkeletonId != Binding.SourceSkeletonId)
        {
            ExternalSkeletonIds.push_back(
                Binding.TargetSkeletonId);
        }
        const std::string CatalogCacheKey =
            CachePathKey(Binding.CatalogFile) + "#" +
            LowerAscii(Result.AssetCatalogSha256) + "#" +
            [&]()
            {
                std::string Key;
                for (const std::string& Id : ExternalSkeletonIds)
                    Key += Id + ";";
                return Key;
            }();
        RetargetAssetCatalogLoadResult CatalogResult;
        if (Cache != nullptr)
        {
            const auto Found = Cache->Catalogs.find(CatalogCacheKey);
            if (Found != Cache->Catalogs.end())
            {
                CatalogResult = Found->second;
                ++Cache->Hits;
            }
            else
            {
                CatalogResult = LoadRetargetAssetCatalog(
                    Binding.CatalogFile, false, ExternalSkeletonIds);
                Cache->Catalogs.emplace(
                    CatalogCacheKey, CatalogResult);
            }
        }
        else
        {
            CatalogResult = LoadRetargetAssetCatalog(
                Binding.CatalogFile, false, ExternalSkeletonIds);
        }
        if (!CatalogResult.Success)
        {
            for (const std::string& Error : CatalogResult.Errors)
            {
                Result.Errors.push_back(
                    "selected asset catalog is invalid: " + Error);
            }
        }
        else
        {
            const RetargetAssetCatalog& Catalog =
                CatalogResult.Catalog;
            if (Catalog.CatalogSha256 !=
                Result.AssetCatalogSha256)
            {
                Result.Errors.push_back(
                    "selected asset catalog changed while preflight "
                    "was running");
            }
            if (Catalog.CatalogId != Binding.CatalogId)
            {
                Result.Errors.push_back(
                    "request catalogId does not match the selected "
                    "asset catalog");
            }
            const RetargetAnimationAsset* Animation =
                FindRetargetAnimationAsset(
                    Catalog, Binding.SourceAnimationId);
            bool AnimationValid = true;
            if (Animation == nullptr)
            {
                Result.Errors.push_back(
                    "asset catalog selection mismatch: source "
                    "animation is not in the catalog");
                AnimationValid = false;
            }
            else if (!Animation->Enabled)
            {
                Result.Errors.push_back(
                    "asset catalog selection mismatch: source "
                    "animation is disabled");
                AnimationValid = false;
            }
            else if (Animation->SourceSkeletonId !=
                     Binding.SourceSkeletonId)
            {
                Result.Errors.push_back(
                    "asset catalog selection mismatch: source "
                    "animation belongs to " +
                    Animation->SourceSkeletonId + ", not " +
                    Binding.SourceSkeletonId);
                AnimationValid = false;
            }

            RetargetSkeletonAsset Source;
            RetargetSkeletonAsset Target;
            const bool SourceValid = ResolveBoundSkeleton(
                Catalog, Binding.SourceSkeletonId,
                Binding.SourceProfilePackage,
                Binding.SourceProfilePackageSha256,
                Binding.SourceProfileVersion, true,
                Source, Result.Errors, Cache);
            const bool TargetValid = ResolveBoundSkeleton(
                Catalog, Binding.TargetSkeletonId,
                Binding.TargetProfilePackage,
                Binding.TargetProfilePackageSha256,
                Binding.TargetProfileVersion, false,
                Target, Result.Errors, Cache);
            if (AnimationValid && SourceValid && TargetValid)
            {
                if (Animation->SourceSkeletonSignatureSha256 !=
                    Source.SkeletonSignatureSha256)
                {
                    Result.Errors.push_back(
                        "asset catalog selection mismatch: source "
                        "animation skeleton signature does not match "
                        "the selected source profile");
                    AnimationValid = false;
                }
            }
            if (AnimationValid && SourceValid && TargetValid)
            {
                AddFileBindingError(
                    Request.SourceAnimationFbx,
                    Animation->Fbx,
                    "source animation", Result.Errors);
                AddFileBindingError(
                    Request.SourceRestFbx,
                    Source.RestFbx,
                    "source rest", Result.Errors);
                AddFileBindingError(
                    Request.TargetSkeletonFbx,
                    Target.RestFbx,
                    "target rest", Result.Errors);
                AddFileBindingError(
                    Request.SourceRigJson,
                    Source.IkRigJson,
                    "source IK Rig JSON", Result.Errors);
                AddFileBindingError(
                    Request.TargetRigJson,
                    Target.IkRigJson,
                    "target IK Rig JSON", Result.Errors);
                AddFileBindingError(
                    Request.SourceAlignmentRetargeterJson,
                    Source.AlignmentRetargeterJson,
                    "source alignment Retargeter JSON",
                    Result.Errors);
                AddFileBindingError(
                    Request.TargetAlignmentRetargeterJson,
                    Target.AlignmentRetargeterJson,
                    "target alignment Retargeter JSON",
                    Result.Errors);
                AddFileBindingError(
                    Request.SourceAnimationGoldenJson,
                    Animation->GoldenJson,
                    "source animation golden JSON",
                    Result.Errors);

                AddHashBindingError(
                    Binding.SourceAnimationSha256,
                    Animation->FbxSha256,
                    "source animation request binding",
                    Result.Errors);
                AddHashBindingError(
                    Binding.SourceRestSha256,
                    Source.RestFbxSha256,
                    "source rest request binding",
                    Result.Errors);
                AddHashBindingError(
                    Binding.TargetRestSha256,
                    Target.RestFbxSha256,
                    "target rest request binding",
                    Result.Errors);
                AddHashBindingError(
                    Binding.SourceRigJsonSha256,
                    Source.IkRigJsonSha256,
                    "source IK Rig request binding",
                    Result.Errors);
                AddHashBindingError(
                    Binding.TargetRigJsonSha256,
                    Target.IkRigJsonSha256,
                    "target IK Rig request binding",
                    Result.Errors);
                AddHashBindingError(
                    Binding.SourceAlignmentRetargeterJsonSha256,
                    Source.AlignmentRetargeterJsonSha256,
                    "source alignment Retargeter request binding",
                    Result.Errors);
                AddHashBindingError(
                    Binding.TargetAlignmentRetargeterJsonSha256,
                    Target.AlignmentRetargeterJsonSha256,
                    "target alignment Retargeter request binding",
                    Result.Errors);
                AddHashBindingError(
                    Binding.SourceAnimationGoldenJsonSha256,
                    Animation->GoldenJsonSha256,
                    "source animation golden request binding",
                    Result.Errors);
                if (Request.AnimationStack !=
                    Animation->AnimationStack)
                {
                    Result.Errors.push_back(
                        "animation Stack does not match the selected "
                        "asset catalog animation");
                }
                if (Request.SourceFbxImportMode !=
                        Animation->SourceFbxImportMode ||
                    Request.RestFbxImportMode !=
                        Animation->RestFbxImportMode)
                {
                    Result.Errors.push_back(
                        "UE FBX import modes do not match the selected "
                        "asset catalog animation");
                }
            }
        }
        if (!Result.Errors.empty()) return Finish();
    }

    if (UEIKJson)
    {
        Result.Warnings.push_back(
            "UE IK JSON route is an unselected, unadopted candidate. "
            "All four exported configuration files and all three FBX "
            "inputs are SHA-256 bound; no uasset is read and no mapping "
            "is inferred.");
        if (ExactSourceImport)
        {
            Result.Warnings.push_back(
                "UE 5.8 exact animation import and exported-rest "
                "coordinate validation are hash-bound candidate "
                "inputs; they do not select or adopt the route.");
        }
    }
    if (!UEIKJson)
    {
        Result.Warnings.push_back(
            "The external Foundation provider is outside this repository. "
            "Its compatibility contract must be supplied and verified "
            "independently before any SKRV is committed.");
    }
    Result.Success = Result.Errors.empty();
    return Finish();
}

RetargetBridgePreflight PreflightRetargetBridge(
    const RetargetBridgeRequest& Request)
{
    return PreflightRetargetBridgeImpl(Request, nullptr);
}

std::vector<RetargetBridgePreflight> PreflightRetargetBridges(
    const std::vector<RetargetBridgeRequest>& Requests)
{
    std::vector<RetargetBridgePreflight> Results;
    Results.reserve(Requests.size());
    PreflightCache Cache;
    for (const RetargetBridgeRequest& Request : Requests)
        Results.push_back(
            PreflightRetargetBridgeImpl(Request, &Cache));
    return Results;
}

bool WriteRetargetBridgeRequest(
    const RetargetBridgeRequest& Request,
    const std::filesystem::path& OutputJson,
    std::string& OutError)
{
    return WriteTextAtomic(
        OutputJson, RequestJson(Request).dump(2) + "\n", OutError);
}

bool ReadRetargetBridgeRequest(
    const std::filesystem::path& InputJson,
    RetargetBridgeRequest& OutRequest,
    std::string& OutError)
{
    OutError.clear();
    OutRequest = {};
    try
    {
        std::ifstream Stream(InputJson, std::ios::binary);
        if (!Stream)
        {
            OutError = "failed to open retarget bridge request";
            return false;
        }
        const nlohmann::json Json = nlohmann::json::parse(Stream);
        const std::string Schema =
            Json.at("schema").get<std::string>();
        const bool IsFrozenV1 = Schema == ExternalRequestSchema;
        const bool IsUEIKJsonV2 = Schema == UEIKJsonRequestSchema;
        const bool IsUEIKJsonV3 =
            Schema == UEIKJsonExactRequestSchema;
        const bool IsUEIKJsonV4 =
            Schema == UEIKJsonCatalogRequestSchema;
        const bool IsUEIKJsonV5 =
            Schema == UEIKJsonProfileRequestSchema;
        const bool IsUEIKJson =
            IsUEIKJsonV2 || IsUEIKJsonV3 ||
            IsUEIKJsonV4 || IsUEIKJsonV5;
        if (!IsFrozenV1 && !IsUEIKJson)
        {
            OutError = "unsupported retarget bridge request schema";
            return false;
        }
        if (IsUEIKJson &&
            !ParseRetargetBridgeRouteKind(
                Json.at("routeKind").get<std::string>(),
                OutRequest.RouteKind))
        {
            OutError = "unsupported retarget bridge route kind";
            return false;
        }
        if (IsUEIKJson &&
            OutRequest.RouteKind != RetargetBridgeRouteKind::UEIKJsonV1)
        {
            OutError =
                "UE IK JSON request requires the explicit "
                "ue_ik_json_v1 route";
            return false;
        }
        OutRequest.SourceAnimationFbx =
            PathFromUtf8(Json.at("sourceAnimationFbx").get<std::string>());
        OutRequest.TargetSkeletonFbx =
            PathFromUtf8(Json.at("targetSkeletonFbx").get<std::string>());
        OutRequest.SourceRestFbx =
            PathFromUtf8(Json.at("sourceRestFbx").get<std::string>());
        OutRequest.OutputDirectory =
            PathFromUtf8(Json.at("outputDirectory").get<std::string>());
        OutRequest.ClipId = Json.at("clipId").get<std::string>();
        OutRequest.ClipLabel = Json.at("clipLabel").get<std::string>();
        OutRequest.EnableSpinePelvisFollow =
            Json.at("enableSpinePelvisFollow").get<bool>();
        OutRequest.EnableSourceMotionFootLock =
            Json.at("enableSourceMotionFootLock").get<bool>();
        if (IsUEIKJson)
        {
            OutRequest.AnimationStack =
                Json.at("animationStack").get<std::string>();
            const nlohmann::json& UEIKJson = Json.at("ueIkJson");
            OutRequest.SourceRigJson = PathFromUtf8(
                UEIKJson.at("sourceRigJson").get<std::string>());
            OutRequest.TargetRigJson = PathFromUtf8(
                UEIKJson.at("targetRigJson").get<std::string>());
            OutRequest.SourceAlignmentRetargeterJson = PathFromUtf8(
                UEIKJson.at("sourceAlignmentRetargeterJson")
                    .get<std::string>());
            OutRequest.TargetAlignmentRetargeterJson = PathFromUtf8(
                UEIKJson.at("targetAlignmentRetargeterJson")
                    .get<std::string>());
        }
        if (IsUEIKJsonV3 || IsUEIKJsonV4 || IsUEIKJsonV5)
        {
            const nlohmann::json& Import =
                Json.at("ueFbxImport");
            if (!ParseRetargetBridgeSourceFbxImportMode(
                    Import.at("sourceAnimationMode")
                        .get<std::string>(),
                    OutRequest.SourceFbxImportMode) ||
                !ParseRetargetBridgeRestFbxImportMode(
                    Import.at("restMode").get<std::string>(),
                    OutRequest.RestFbxImportMode))
            {
                OutError =
                    "unsupported UE FBX import mode in request v3";
                return false;
            }
            OutRequest.SourceAnimationGoldenJson =
                PathFromUtf8(
                    Import.at("sourceAnimationGoldenJson")
                        .get<std::string>());
        }
        if (IsUEIKJsonV4 || IsUEIKJsonV5)
        {
            const nlohmann::json& Selection =
                Json.at("assetSelection");
            const nlohmann::json& Expected =
                Selection.at("expectedSha256");
            RetargetBridgeAssetBinding& Binding =
                OutRequest.AssetBinding;
            Binding.Required = true;
            Binding.CatalogFile = PathFromUtf8(
                Selection.at("catalogFile").get<std::string>());
            Binding.CatalogSha256 =
                Selection.at("catalogSha256").get<std::string>();
            Binding.CatalogId =
                Selection.at("catalogId").get<std::string>();
            Binding.SourceSkeletonId =
                Selection.at("sourceSkeletonId").get<std::string>();
            Binding.TargetSkeletonId =
                Selection.at("targetSkeletonId").get<std::string>();
            Binding.SourceAnimationId =
                Selection.at("sourceAnimationId").get<std::string>();
            Binding.SourceAnimationSkeletonId =
                Selection.at("sourceAnimationSkeletonId")
                    .get<std::string>();
            if (IsUEIKJsonV5)
            {
                const nlohmann::json& Profiles =
                    Selection.at("characterProfiles");
                const nlohmann::json& Source =
                    Profiles.at("source");
                const nlohmann::json& Target =
                    Profiles.at("target");
                Binding.SourceProfilePackage = PathFromUtf8(
                    Source.at("packageFile").get<std::string>());
                Binding.SourceProfilePackageSha256 =
                    Source.at("packageSha256").get<std::string>();
                Binding.SourceProfileVersion =
                    Source.at("profileVersion").get<std::string>();
                Binding.TargetProfilePackage = PathFromUtf8(
                    Target.at("packageFile").get<std::string>());
                Binding.TargetProfilePackageSha256 =
                    Target.at("packageSha256").get<std::string>();
                Binding.TargetProfileVersion =
                    Target.at("profileVersion").get<std::string>();
            }
            Binding.SourceAnimationSha256 =
                Expected.at("sourceAnimation").get<std::string>();
            Binding.SourceRestSha256 =
                Expected.at("sourceRest").get<std::string>();
            Binding.TargetRestSha256 =
                Expected.at("targetRest").get<std::string>();
            Binding.SourceRigJsonSha256 =
                Expected.at("sourceRigJson").get<std::string>();
            Binding.TargetRigJsonSha256 =
                Expected.at("targetRigJson").get<std::string>();
            Binding.SourceAlignmentRetargeterJsonSha256 =
                Expected.at("sourceAlignmentRetargeterJson")
                    .get<std::string>();
            Binding.TargetAlignmentRetargeterJsonSha256 =
                Expected.at("targetAlignmentRetargeterJson")
                    .get<std::string>();
            Binding.SourceAnimationGoldenJsonSha256 =
                Expected.at("sourceAnimationGoldenJson")
                    .get<std::string>();
        }
        const nlohmann::json& Tools = Json.at("tools");
        OutRequest.Tools.BridgeExecutable = PathFromUtf8(
            Tools.at("bridgeExecutable").get<std::string>());
        OutRequest.Tools.RetargeterExecutable = PathFromUtf8(
            Tools.at("retargeterExecutable").get<std::string>());
        OutRequest.Tools.NodeExecutable = PathFromUtf8(
            Tools.at("nodeExecutable").get<std::string>());
        OutRequest.Tools.AdapterScript = PathFromUtf8(
            Tools.at("adapterScript").get<std::string>());
        OutRequest.Tools.SkrvPackExecutable = PathFromUtf8(
            Tools.at("skrvPackExecutable").get<std::string>());
        OutRequest.Tools.CanonicalJson = PathFromUtf8(
            Tools.at("canonicalJson").get<std::string>());
        OutRequest.Tools.DefaultSourceRestFbx = PathFromUtf8(
            Tools.at("defaultSourceRestFbx").get<std::string>());
        if (IsUEIKJson)
        {
            OutRequest.Tools.UEIKRetargeterExecutable = PathFromUtf8(
                Tools.at("ueIkRetargeterExecutable")
                    .get<std::string>());
        }
        return true;
    }
    catch (const std::exception& Error)
    {
        OutError = std::string("invalid retarget bridge request: ") +
            Error.what();
        return false;
    }
}

std::vector<std::string> BuildFrozenRetargeterArguments(
    const RetargetBridgeRequest& Request,
    const RetargetBridgePreflight& Preflight,
    const std::filesystem::path& RetargeterOutputDirectory)
{
    std::vector<std::string> Arguments = {
        "--single-clip-bridge",
        "--source-tpose-fbx", PathToUtf8(Request.SourceRestFbx),
        "--target-tpose-fbx", PathToUtf8(Request.TargetSkeletonFbx),
        "--d1-14c-canonical", PathToUtf8(Request.Tools.CanonicalJson),
        "--out-dir", PathToUtf8(RetargeterOutputDirectory)};
    if (Request.EnableSpinePelvisFollow)
        Arguments.emplace_back("--spine-pelvis-follow");
    if (Request.EnableSourceMotionFootLock)
        Arguments.emplace_back("--source-motion-foot-lock");
    Arguments.insert(
        Arguments.end(), {
            "--clip", Request.ClipId, Request.ClipLabel,
            PathToUtf8(Request.SourceAnimationFbx),
            Preflight.SourceAnimationSha256,
            ExportFileName(Request)});
    return Arguments;
}

std::vector<std::string> BuildUEIKJsonRetargeterArguments(
    const RetargetBridgeRequest& Request,
    const RetargetBridgePreflight& Preflight,
    const std::filesystem::path& RetargeterOutputDirectory)
{
    std::vector<std::string> Arguments = {
        "--source-rig-json", PathToUtf8(Request.SourceRigJson),
        "--source-rig-sha256", Preflight.SourceRigJsonSha256,
        "--target-rig-json", PathToUtf8(Request.TargetRigJson),
        "--target-rig-sha256", Preflight.TargetRigJsonSha256,
        "--source-alignment-rtg-json",
        PathToUtf8(Request.SourceAlignmentRetargeterJson),
        "--source-alignment-rtg-sha256",
        Preflight.SourceAlignmentRetargeterJsonSha256,
        "--target-alignment-rtg-json",
        PathToUtf8(Request.TargetAlignmentRetargeterJson),
        "--target-alignment-rtg-sha256",
        Preflight.TargetAlignmentRetargeterJsonSha256,
        "--source-rest-fbx", PathToUtf8(Request.SourceRestFbx),
        "--source-rest-sha256", Preflight.SourceRestSha256,
        "--source-animation-fbx",
        PathToUtf8(Request.SourceAnimationFbx),
        "--source-animation-sha256",
        Preflight.SourceAnimationSha256,
        "--source-fbx-import-mode",
        RetargetBridgeSourceFbxImportModeName(
            Request.SourceFbxImportMode),
        "--rest-fbx-import-mode",
        RetargetBridgeRestFbxImportModeName(
            Request.RestFbxImportMode),
        "--target-rest-fbx", PathToUtf8(Request.TargetSkeletonFbx),
        "--target-rest-sha256", Preflight.TargetSkeletonSha256,
        "--out-dir", PathToUtf8(RetargeterOutputDirectory),
        "--clip-id", Request.ClipId,
        "--clip-label", Request.ClipLabel,
        "--foundation-export-fbx",
        FoundationExportFileName(Request),
        "--export-fbx", ExportFileName(Request)};
    if (Request.SourceFbxImportMode ==
        RetargetBridgeSourceFbxImportMode::UE58ExactGoldenV1)
    {
        Arguments.insert(
            Arguments.end(), {
                "--source-animation-golden-json",
                PathToUtf8(
                    Request.SourceAnimationGoldenJson),
                "--source-animation-golden-sha256",
                Preflight.SourceAnimationGoldenJsonSha256});
    }
    if (!Request.AnimationStack.empty())
    {
        Arguments.emplace_back("--animation-stack");
        Arguments.emplace_back(Request.AnimationStack);
    }
    return Arguments;
}

static RetargetBridgeRunResult RunRetargetBridgeImpl(
    const RetargetBridgeRequest& Request,
    const RetargetBridgePreflight* SuppliedPreflight)
{
    RetargetBridgeRequest Resolved = Request;
    bool ResolvedAnyPath = false;
    auto ResolvePath =
        [&](std::filesystem::path& Path)
        {
            if (Path.empty() || Path.is_absolute()) return;
            std::error_code Error;
            const std::filesystem::path Absolute =
                std::filesystem::absolute(Path, Error);
            if (!Error)
            {
                Path = Absolute.lexically_normal();
                ResolvedAnyPath = true;
            }
        };
    ResolvePath(Resolved.SourceAnimationFbx);
    ResolvePath(Resolved.TargetSkeletonFbx);
    ResolvePath(Resolved.SourceRestFbx);
    ResolvePath(Resolved.SourceRigJson);
    ResolvePath(Resolved.TargetRigJson);
    ResolvePath(Resolved.SourceAlignmentRetargeterJson);
    ResolvePath(Resolved.TargetAlignmentRetargeterJson);
    ResolvePath(Resolved.SourceAnimationGoldenJson);
    ResolvePath(Resolved.AssetBinding.CatalogFile);
    ResolvePath(Resolved.AssetBinding.SourceProfilePackage);
    ResolvePath(Resolved.AssetBinding.TargetProfilePackage);
    ResolvePath(Resolved.OutputDirectory);
    ResolvePath(Resolved.Tools.BridgeExecutable);
    ResolvePath(Resolved.Tools.RetargeterExecutable);
    ResolvePath(Resolved.Tools.UEIKRetargeterExecutable);
    ResolvePath(Resolved.Tools.NodeExecutable);
    ResolvePath(Resolved.Tools.AdapterScript);
    ResolvePath(Resolved.Tools.SkrvPackExecutable);
    ResolvePath(Resolved.Tools.CanonicalJson);
    ResolvePath(Resolved.Tools.DefaultSourceRestFbx);
    if (ResolvedAnyPath)
        return RunRetargetBridgeImpl(Resolved, SuppliedPreflight);

    RetargetBridgeRunResult Result;
    const auto TotalStarted = std::chrono::steady_clock::now();
    Result.StatusJson = Request.OutputDirectory / "bridge_status.json";
    Result.ReviewPackage = Request.OutputDirectory / "review.skrv";
    Result.RetargeterLog = Request.OutputDirectory / "retargeter.log";
    Result.AdapterLog = Request.OutputDirectory / "adapter.log";
    Result.PackLog = Request.OutputDirectory / "pack.log";
    const auto PreflightStarted = std::chrono::steady_clock::now();
    RetargetBridgePreflight Preflight = SuppliedPreflight == nullptr
        ? PreflightRetargetBridge(Request)
        : *SuppliedPreflight;
    if (SuppliedPreflight != nullptr &&
        Preflight.RequestIdentity != RequestIdentity(Request))
    {
        Preflight.Success = false;
        Preflight.Errors.push_back(
            "supplied complete-selection preflight does not match this "
            "retarget request");
    }
    Result.Timings.PreflightReused = SuppliedPreflight != nullptr;
    Result.Timings.PreflightSeconds =
        SuppliedPreflight == nullptr
        ? std::chrono::duration<double>(
              std::chrono::steady_clock::now() - PreflightStarted).count()
        : 0.0;
    Result.SourceAnimationSha256 = Preflight.SourceAnimationSha256;
    Result.Errors = Preflight.Errors;
    std::error_code DirectoryError;
    if (!Preflight.Success)
    {
        Result.Timings.TotalSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - TotalStarted).count();
        if (!Request.OutputDirectory.empty() &&
            DirectoryEmptyOrAbsent(Request.OutputDirectory))
        {
            std::filesystem::create_directories(
                Request.OutputDirectory, DirectoryError);
            std::string Ignored;
            WriteTextAtomic(
                Result.StatusJson,
                RunStatusJson(Request, Preflight, Result).dump(2) + "\n",
                Ignored);
        }
        return Result;
    }

    std::filesystem::create_directories(
        Request.OutputDirectory, DirectoryError);
    if (DirectoryError)
    {
        Result.Errors.push_back("failed to create bridge output directory");
        Result.Timings.TotalSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - TotalStarted).count();
        return Result;
    }
    const std::filesystem::path RetargeterOutput =
        Request.OutputDirectory / "retargeter";
    const std::filesystem::path Payload =
        Request.OutputDirectory / "payload";
    const bool UEIKJson =
        Request.RouteKind == RetargetBridgeRouteKind::UEIKJsonV1;

    auto PhaseStarted = std::chrono::steady_clock::now();
    ProcessRunResult Process = RunProcessBlocking({
        UEIKJson
            ? Request.Tools.UEIKRetargeterExecutable
            : Request.Tools.RetargeterExecutable,
        UEIKJson
            ? BuildUEIKJsonRetargeterArguments(
                Request, Preflight, RetargeterOutput)
            : BuildFrozenRetargeterArguments(
                Request, Preflight, RetargeterOutput),
        Request.OutputDirectory,
        Result.RetargeterLog});
    Result.Timings.RetargetWorkerSeconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - PhaseStarted).count();
    Result.RetargeterExitCode = Process.ExitCode;
    if (!Process.Started || Process.ExitCode != 0)
    {
        Result.Errors.push_back(Process.Error.empty()
            ? (UEIKJson
                ? "UE IK JSON Retargeter worker failed; see "
                  "retargeter.log"
                : "frozen Retargeter worker failed; see "
                  "retargeter.log")
            : Process.Error);
    }
    else
    {
        const std::filesystem::path Review = RetargeterOutput / "Review";
        PhaseStarted = std::chrono::steady_clock::now();
        Process = RunProcessBlocking({
            Request.Tools.NodeExecutable,
            {PathToUtf8(Request.Tools.AdapterScript),
             PathToUtf8(Review / "SKRTG_UEIK_Retarget_Review_Viewer.html"),
             PathToUtf8(Review /
                 "SKRTG_UEIK_Mesh_And_FBX_Export_Verification.json"),
             PathToUtf8(Payload)},
            Request.OutputDirectory,
            Result.AdapterLog});
        Result.Timings.AdapterSeconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - PhaseStarted).count();
        Result.AdapterExitCode = Process.ExitCode;
        if (!Process.Started || Process.ExitCode != 0)
        {
            Result.Errors.push_back(Process.Error.empty()
                ? "frozen HTML-to-SKRV adapter failed; see adapter.log"
                : Process.Error);
        }
        else
        {
            PhaseStarted = std::chrono::steady_clock::now();
            const skrv::PackageWriteResult Package =
                skrv::SealDirectoryPackage(
                    Payload, Result.ReviewPackage);
            const double PackTotalSeconds =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - PhaseStarted)
                    .count();
            Result.Timings.PackageInspectSeconds =
                Package.InspectionSeconds;
            Result.Timings.PackSeconds = std::max(
                0.0, PackTotalSeconds - Package.InspectionSeconds);
            Result.PackExitCode = Package.Success ? 0 : 1;
            std::uintmax_t IndexedBytes = 0;
            for (const skrv::IntegrityEntry& Entry : Package.Entries)
                IndexedBytes += Entry.ByteCount;
            std::ostringstream PackSummary;
            if (Package.Success)
            {
                PackSummary
                    << "SKRV package sealed in process\n"
                    << "path=" << PathToUtf8(Result.ReviewPackage)
                    << '\n'
                    << "indexed_files=" << Package.Entries.size()
                    << '\n'
                    << "indexed_bytes=" << IndexedBytes << '\n'
                    << "preparation_seconds="
                    << Package.PreparationSeconds << '\n'
                    << "inspection_seconds="
                    << Package.InspectionSeconds << '\n';
            }
            else
            {
                PackSummary << "SKRV package sealing failed\n";
                for (const std::string& Error : Package.Errors)
                    PackSummary << "error=" << Error << '\n';
            }
            std::string PackLogError;
            if (!WriteTextAtomic(
                    Result.PackLog, PackSummary.str(), PackLogError))
            {
                Result.Errors.push_back(PackLogError);
            }
            if (!Package.Success)
            {
                if (Package.Errors.empty())
                    Result.Errors.push_back(
                        "SKRV package sealing failed; see pack.log");
                else
                {
                    Result.Errors.insert(
                        Result.Errors.end(),
                        Package.Errors.begin(), Package.Errors.end());
                }
            }
            else if (FindSealedVerifiedExport(
                         Result.ReviewPackage,
                         Package.Entries,
                         Request.ClipId,
                         "final",
                         Result.VerifiedFinalFbx,
                         Result.VerifiedFinalFbxSha256,
                         Result.Errors))
            {
                Result.Success = true;
            }
        }
    }
    Result.Timings.TotalSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - TotalStarted).count();
    std::string StatusError;
    if (!WriteTextAtomic(
            Result.StatusJson,
            RunStatusJson(Request, Preflight, Result).dump(2) + "\n",
            StatusError))
    {
        Result.Success = false;
        Result.Errors.push_back(StatusError);
    }
    return Result;
}

RetargetBridgeRunResult RunRetargetBridge(
    const RetargetBridgeRequest& Request)
{
    return RunRetargetBridgeImpl(Request, nullptr);
}

RetargetBridgeRunResult RunRetargetBridgePreflighted(
    const RetargetBridgeRequest& Request,
    const RetargetBridgePreflight& Preflight)
{
    return RunRetargetBridgeImpl(Request, &Preflight);
}

} // namespace skrtg::viewer
