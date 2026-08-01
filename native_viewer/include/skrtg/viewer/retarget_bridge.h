#pragma once

#include "skrtg/viewer/process.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::viewer
{
enum class RetargetBridgeRouteKind
{
    ExternalFoundationV1,
    UEIKJsonV1
};

enum class RetargetBridgeSourceFbxImportMode
{
    FbxBodyBasisV7,
    UE58ExactGoldenV1
};

enum class RetargetBridgeRestFbxImportMode
{
    ReconciledRestV1,
    UE58ExportedYReflectionV1
};

struct RetargetBridgeTools
{
    std::filesystem::path BridgeExecutable;
    std::filesystem::path RetargeterExecutable;
    std::filesystem::path UEIKRetargeterExecutable;
    std::filesystem::path NodeExecutable = "node";
    std::filesystem::path AdapterScript;
    std::filesystem::path SkrvPackExecutable;
    std::filesystem::path CanonicalJson;
    std::filesystem::path DefaultSourceRestFbx;
};

struct RetargetBridgeAssetBinding
{
    bool Required = false;
    std::filesystem::path CatalogFile;
    std::string CatalogSha256;
    std::string CatalogId;
    std::string SourceSkeletonId;
    std::string TargetSkeletonId;
    std::string SourceAnimationId;
    std::string SourceAnimationSkeletonId;
    std::filesystem::path SourceProfilePackage;
    std::string SourceProfilePackageSha256;
    std::string SourceProfileVersion;
    std::filesystem::path TargetProfilePackage;
    std::string TargetProfilePackageSha256;
    std::string TargetProfileVersion;
    std::string SourceAnimationSha256;
    std::string SourceRestSha256;
    std::string TargetRestSha256;
    std::string SourceRigJsonSha256;
    std::string TargetRigJsonSha256;
    std::string SourceAlignmentRetargeterJsonSha256;
    std::string TargetAlignmentRetargeterJsonSha256;
    std::string SourceAnimationGoldenJsonSha256;
};

struct RetargetBridgeRequest
{
    RetargetBridgeRouteKind RouteKind =
        RetargetBridgeRouteKind::UEIKJsonV1;
    std::filesystem::path SourceAnimationFbx;
    std::filesystem::path TargetSkeletonFbx;
    std::filesystem::path SourceRestFbx;
    std::filesystem::path SourceRigJson;
    std::filesystem::path TargetRigJson;
    std::filesystem::path SourceAlignmentRetargeterJson;
    std::filesystem::path TargetAlignmentRetargeterJson;
    std::filesystem::path SourceAnimationGoldenJson;
    std::filesystem::path OutputDirectory;
    RetargetBridgeTools Tools;
    std::string ClipId;
    std::string ClipLabel;
    std::string AnimationStack;
    RetargetBridgeSourceFbxImportMode SourceFbxImportMode =
        RetargetBridgeSourceFbxImportMode::UE58ExactGoldenV1;
    RetargetBridgeRestFbxImportMode RestFbxImportMode =
        RetargetBridgeRestFbxImportMode::UE58ExportedYReflectionV1;
    RetargetBridgeAssetBinding AssetBinding;
    bool EnableSpinePelvisFollow = true;
    bool EnableSourceMotionFootLock = true;
};

struct RetargetBridgePreflight
{
    bool Success = false;
    std::string AssetCatalogSha256;
    std::string SourceProfilePackageSha256;
    std::string TargetProfilePackageSha256;
    std::string SourceAnimationSha256;
    std::string SourceRestSha256;
    std::string TargetSkeletonSha256;
    std::string CanonicalSha256;
    std::string SourceRigJsonSha256;
    std::string TargetRigJsonSha256;
    std::string SourceAlignmentRetargeterJsonSha256;
    std::string TargetAlignmentRetargeterJsonSha256;
    std::string SourceAnimationGoldenJsonSha256;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

struct RetargetBridgeRunResult
{
    bool Success = false;
    std::filesystem::path ReviewPackage;
    std::filesystem::path StatusJson;
    std::filesystem::path RetargeterLog;
    std::filesystem::path AdapterLog;
    std::filesystem::path PackLog;
    std::string SourceAnimationSha256;
    int RetargeterExitCode = -1;
    int AdapterExitCode = -1;
    int PackExitCode = -1;
    std::vector<std::string> Errors;
};

RetargetBridgeTools DiscoverRetargetBridgeTools(
    const std::filesystem::path& ViewerExecutable);

const char* RetargetBridgeRouteKindName(RetargetBridgeRouteKind Kind);
bool ParseRetargetBridgeRouteKind(
    const std::string& Text,
    RetargetBridgeRouteKind& OutKind);
const char* RetargetBridgeSourceFbxImportModeName(
    RetargetBridgeSourceFbxImportMode Mode);
bool ParseRetargetBridgeSourceFbxImportMode(
    const std::string& Text,
    RetargetBridgeSourceFbxImportMode& OutMode);
const char* RetargetBridgeRestFbxImportModeName(
    RetargetBridgeRestFbxImportMode Mode);
bool ParseRetargetBridgeRestFbxImportMode(
    const std::string& Text,
    RetargetBridgeRestFbxImportMode& OutMode);

std::string MakeBridgeClipId(const std::filesystem::path& AnimationPath);
std::string PathToUtf8(const std::filesystem::path& Path);
std::filesystem::path PathFromUtf8(const std::string& Text);

RetargetBridgePreflight PreflightRetargetBridge(
    const RetargetBridgeRequest& Request);

bool WriteRetargetBridgeRequest(
    const RetargetBridgeRequest& Request,
    const std::filesystem::path& OutputJson,
    std::string& OutError);

bool ReadRetargetBridgeRequest(
    const std::filesystem::path& InputJson,
    RetargetBridgeRequest& OutRequest,
    std::string& OutError);

std::vector<std::string> BuildFrozenRetargeterArguments(
    const RetargetBridgeRequest& Request,
    const RetargetBridgePreflight& Preflight,
    const std::filesystem::path& RetargeterOutputDirectory);

std::vector<std::string> BuildUEIKJsonRetargeterArguments(
    const RetargetBridgeRequest& Request,
    const RetargetBridgePreflight& Preflight,
    const std::filesystem::path& RetargeterOutputDirectory);

RetargetBridgeRunResult RunRetargetBridge(
    const RetargetBridgeRequest& Request);

} // namespace skrtg::viewer
