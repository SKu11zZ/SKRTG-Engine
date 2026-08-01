#include "skrtg/viewer/retarget_bridge.h"

#include "skrtg/viewer/retarget_asset_catalog.h"
#include "skrtg/viewer/skrv/package.h"
#include "skrtg/viewer/skrv/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

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
constexpr const char* ExternalStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v1";
constexpr const char* UEIKJsonStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v2";
constexpr const char* UEIKJsonExactStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v3";
constexpr const char* UEIKJsonCatalogStatusSchema =
    "skrtg.native_viewer.retarget_bridge_status.v4";

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
            " does not match the selected asset catalog SHA-256");
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
            " path does not match the selected asset catalog");
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
    const char* Label)
{
    std::string Error;
    if (!skrv::Sha256File(Path, OutHash, Error))
    {
        Errors.push_back(std::string("failed to hash ") + Label + ": " +
                         Error);
        return false;
    }
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
            Json["schema"] = UEIKJsonCatalogRequestSchema;
            Json["assetSelection"] = {
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
    return {
        {"schema", Request.AssetBinding.Required
            ? UEIKJsonCatalogStatusSchema
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
             Request.AssetBinding.SourceAnimationSkeletonId}
        }},
        {"success", Result.Success},
        {"reviewPackage", PathToUtf8(Result.ReviewPackage)},
        {"sourceAnimationFbx", PathToUtf8(Request.SourceAnimationFbx)},
        {"assetCatalogSha256", Preflight.AssetCatalogSha256},
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

RetargetBridgePreflight PreflightRetargetBridge(
    const RetargetBridgeRequest& Request)
{
    RetargetBridgePreflight Result;
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
            !IsContractId(Binding.SourceSkeletonId) ||
            !IsContractId(Binding.TargetSkeletonId) ||
            !IsContractId(Binding.SourceAnimationId) ||
            !IsContractId(Binding.SourceAnimationSkeletonId))
        {
            Result.Errors.push_back(
                "asset catalog IDs must contain only lowercase ASCII "
                "letters, digits, and underscore");
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
    if (!Result.Errors.empty()) return Result;

    if (Request.AssetBinding.Required)
    {
        ComputeHash(
            Request.AssetBinding.CatalogFile,
            Result.AssetCatalogSha256,
            Result.Errors, "retarget asset catalog JSON");
    }
    ComputeHash(
        Request.SourceAnimationFbx,
        Result.SourceAnimationSha256, Result.Errors, "source animation");
    ComputeHash(
        Request.SourceRestFbx,
        Result.SourceRestSha256, Result.Errors, "source rest");
    ComputeHash(
        Request.TargetSkeletonFbx,
        Result.TargetSkeletonSha256, Result.Errors, "target skeleton");
    if (UEIKJson)
    {
        ComputeHash(
            Request.SourceRigJson,
            Result.SourceRigJsonSha256, Result.Errors,
            "source IK Rig JSON");
        ComputeHash(
            Request.TargetRigJson,
            Result.TargetRigJsonSha256, Result.Errors,
            "target IK Rig JSON");
        ComputeHash(
            Request.SourceAlignmentRetargeterJson,
            Result.SourceAlignmentRetargeterJsonSha256, Result.Errors,
            "source alignment IK Retargeter JSON");
        ComputeHash(
            Request.TargetAlignmentRetargeterJson,
            Result.TargetAlignmentRetargeterJsonSha256, Result.Errors,
            "target alignment IK Retargeter JSON");
        if (ExactSourceImport)
        {
            ComputeHash(
                Request.SourceAnimationGoldenJson,
                Result.SourceAnimationGoldenJsonSha256,
                Result.Errors,
                "source animation golden JSON");
        }
    }
    else
    {
        ComputeHash(
            Request.Tools.CanonicalJson,
            Result.CanonicalSha256, Result.Errors, "frozen canonical");
    }
    if (!Result.Errors.empty()) return Result;

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

        const RetargetAssetCatalogLoadResult CatalogResult =
            LoadRetargetAssetCatalog(
                Binding.CatalogFile, false);
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
            const RetargetAssetSelectionValidation Selection =
                ValidateRetargetAssetSelection(
                    Catalog,
                    Binding.SourceSkeletonId,
                    Binding.SourceAnimationId,
                    Binding.TargetSkeletonId);
            for (const std::string& Error : Selection.Errors)
            {
                Result.Errors.push_back(
                    "asset catalog selection mismatch: " + Error);
            }
            if (Selection.Success)
            {
                const RetargetSkeletonAsset& Source =
                    *FindRetargetSkeletonAsset(
                        Catalog, Binding.SourceSkeletonId);
                const RetargetSkeletonAsset& Target =
                    *FindRetargetSkeletonAsset(
                        Catalog, Binding.TargetSkeletonId);
                const RetargetAnimationAsset& Animation =
                    *FindRetargetAnimationAsset(
                        Catalog, Binding.SourceAnimationId);
                AddFileBindingError(
                    Request.SourceAnimationFbx,
                    Animation.Fbx,
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
                    Animation.GoldenJson,
                    "source animation golden JSON",
                    Result.Errors);

                AddHashBindingError(
                    Binding.SourceAnimationSha256,
                    Animation.FbxSha256,
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
                    Animation.GoldenJsonSha256,
                    "source animation golden request binding",
                    Result.Errors);
                if (Request.AnimationStack !=
                    Animation.AnimationStack)
                {
                    Result.Errors.push_back(
                        "animation Stack does not match the selected "
                        "asset catalog animation");
                }
                if (Request.SourceFbxImportMode !=
                        Animation.SourceFbxImportMode ||
                    Request.RestFbxImportMode !=
                        Animation.RestFbxImportMode)
                {
                    Result.Errors.push_back(
                        "UE FBX import modes do not match the selected "
                        "asset catalog animation");
                }
            }
        }
        if (!Result.Errors.empty()) return Result;
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
    return Result;
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
        const bool IsUEIKJson =
            IsUEIKJsonV2 || IsUEIKJsonV3 || IsUEIKJsonV4;
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
        if (IsUEIKJsonV3 || IsUEIKJsonV4)
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
        if (IsUEIKJsonV4)
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

RetargetBridgeRunResult RunRetargetBridge(
    const RetargetBridgeRequest& Request)
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
        return RunRetargetBridge(Resolved);

    RetargetBridgeRunResult Result;
    Result.StatusJson = Request.OutputDirectory / "bridge_status.json";
    Result.ReviewPackage = Request.OutputDirectory / "review.skrv";
    Result.RetargeterLog = Request.OutputDirectory / "retargeter.log";
    Result.AdapterLog = Request.OutputDirectory / "adapter.log";
    Result.PackLog = Request.OutputDirectory / "pack.log";
    const RetargetBridgePreflight Preflight =
        PreflightRetargetBridge(Request);
    Result.SourceAnimationSha256 = Preflight.SourceAnimationSha256;
    Result.Errors = Preflight.Errors;
    std::error_code DirectoryError;
    if (!Preflight.Success)
    {
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
        return Result;
    }
    const std::filesystem::path RetargeterOutput =
        Request.OutputDirectory / "retargeter";
    const std::filesystem::path Payload =
        Request.OutputDirectory / "payload";
    const bool UEIKJson =
        Request.RouteKind == RetargetBridgeRouteKind::UEIKJsonV1;

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
        Process = RunProcessBlocking({
            Request.Tools.NodeExecutable,
            {PathToUtf8(Request.Tools.AdapterScript),
             PathToUtf8(Review / "SKRTG_UEIK_Retarget_Review_Viewer.html"),
             PathToUtf8(Review /
                 "SKRTG_UEIK_Mesh_And_FBX_Export_Verification.json"),
             PathToUtf8(Payload)},
            Request.OutputDirectory,
            Result.AdapterLog});
        Result.AdapterExitCode = Process.ExitCode;
        if (!Process.Started || Process.ExitCode != 0)
        {
            Result.Errors.push_back(Process.Error.empty()
                ? "frozen HTML-to-SKRV adapter failed; see adapter.log"
                : Process.Error);
        }
        else
        {
            Process = RunProcessBlocking({
                Request.Tools.SkrvPackExecutable,
                {PathToUtf8(Payload), PathToUtf8(Result.ReviewPackage)},
                Request.OutputDirectory,
                Result.PackLog});
            Result.PackExitCode = Process.ExitCode;
            if (!Process.Started || Process.ExitCode != 0)
            {
                Result.Errors.push_back(Process.Error.empty()
                    ? "SKRV pack failed; see pack.log"
                    : Process.Error);
            }
            else
            {
                const skrv::PackageInspectResult Inspection =
                    skrv::InspectDirectoryPackage(Result.ReviewPackage);
                if (!Inspection.Success)
                {
                    Result.Errors.insert(
                        Result.Errors.end(),
                        Inspection.Errors.begin(), Inspection.Errors.end());
                }
                else
                {
                    Result.Success = true;
                }
            }
        }
    }
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

} // namespace skrtg::viewer
