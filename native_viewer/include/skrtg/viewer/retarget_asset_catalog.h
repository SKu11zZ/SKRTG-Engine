#pragma once

#include "skrtg/viewer/retarget_bridge.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::viewer
{
struct RetargetSkeletonAsset
{
    std::string Id;
    std::string Label;
    std::string SkeletonSignatureSha256;
    std::filesystem::path RestFbx;
    std::string RestFbxSha256;
    std::filesystem::path IkRigJson;
    std::string IkRigJsonSha256;
    std::filesystem::path AlignmentRetargeterJson;
    std::string AlignmentRetargeterJsonSha256;
    bool SourceEnabled = true;
    bool TargetEnabled = true;
};

struct RetargetAnimationAsset
{
    std::string Id;
    std::string Label;
    std::string SourceSkeletonId;
    std::string SourceSkeletonSignatureSha256;
    std::filesystem::path Fbx;
    std::string FbxSha256;
    std::filesystem::path GoldenJson;
    std::string GoldenJsonSha256;
    std::string AnimationStack;
    RetargetBridgeSourceFbxImportMode SourceFbxImportMode =
        RetargetBridgeSourceFbxImportMode::UE58ExactGoldenV1;
    RetargetBridgeRestFbxImportMode RestFbxImportMode =
        RetargetBridgeRestFbxImportMode::UE58ExportedYReflectionV1;
    bool Enabled = true;
};

struct RetargetAssetCatalog
{
    std::string CatalogId;
    std::filesystem::path CatalogFile;
    std::string CatalogSha256;
    std::filesystem::path AssetRoot;
    std::vector<std::string> ExternalSkeletonIds;
    std::vector<RetargetSkeletonAsset> Skeletons;
    std::vector<RetargetAnimationAsset> Animations;
};

struct RetargetAssetCatalogLoadResult
{
    bool Success = false;
    RetargetAssetCatalog Catalog;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

struct RetargetAssetSelectionValidation
{
    bool Success = false;
    std::vector<std::string> Errors;
};

std::filesystem::path DiscoverRetargetAssetCatalog(
    const std::filesystem::path& ViewerExecutable);

RetargetAssetCatalogLoadResult LoadRetargetAssetCatalog(
    const std::filesystem::path& CatalogFile,
    bool VerifyFilesAndHashes = true,
    const std::vector<std::string>& ExternalSkeletonIds = {});

const RetargetSkeletonAsset* FindRetargetSkeletonAsset(
    const RetargetAssetCatalog& Catalog,
    const std::string& SkeletonId);

const RetargetAnimationAsset* FindRetargetAnimationAsset(
    const RetargetAssetCatalog& Catalog,
    const std::string& AnimationId);

std::vector<std::size_t> CompatibleRetargetAnimationIndices(
    const RetargetAssetCatalog& Catalog,
    const std::string& SourceSkeletonId);

RetargetAssetSelectionValidation ValidateRetargetAssetSelection(
    const RetargetAssetCatalog& Catalog,
    const std::string& SourceSkeletonId,
    const std::string& AnimationId,
    const std::string& TargetSkeletonId);

bool ApplyRetargetAssetSelection(
    const RetargetAssetCatalog& Catalog,
    const std::string& SourceSkeletonId,
    const std::string& AnimationId,
    const std::string& TargetSkeletonId,
    RetargetBridgeRequest& InOutRequest,
    std::vector<std::string>& OutErrors);

} // namespace skrtg::viewer
