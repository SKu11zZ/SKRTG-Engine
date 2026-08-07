#pragma once

#include "skrtg/fbx/retarget_review_package.h"
#include "skrtg/retarget/ue_ik_json_route.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::fbx
{
enum class UEIKSourceFbxImportMode
{
    FbxBodyBasisV7,
    UE58ExactGoldenV1
};

enum class UEIKRestFbxImportMode
{
    ReconciledRestV1,
    UE58ExportedYReflectionV1
};

struct UEIKJsonRetargetOptions
{
    retarget::UEIKJsonCanonicalBridgeLoadOptions Route;
    std::string SourceRigJsonExpectedSha256;
    std::string TargetRigJsonExpectedSha256;
    std::string SourceAlignmentRetargeterJsonExpectedSha256;
    std::string TargetAlignmentRetargeterJsonExpectedSha256;

    std::filesystem::path SourceRestFbxPath;
    std::string SourceRestFbxExpectedSha256;
    std::filesystem::path SourceAnimationFbxPath;
    std::string SourceAnimationFbxExpectedSha256;
    UEIKSourceFbxImportMode SourceFbxImportMode =
        UEIKSourceFbxImportMode::UE58ExactGoldenV1;
    UEIKRestFbxImportMode RestFbxImportMode =
        UEIKRestFbxImportMode::UE58ExportedYReflectionV1;
    std::filesystem::path SourceAnimationGoldenJsonPath;
    std::string SourceAnimationGoldenJsonExpectedSha256;
    std::filesystem::path TargetRestFbxPath;
    std::string TargetRestFbxExpectedSha256;
    RetargetReviewMeshSelection SourceMeshSelection;
    RetargetReviewMeshSelection TargetMeshSelection;

    // Optional, hash-bound candidate Operation System v2 program. Absence
    // means exact Foundation passthrough. Loading never selects or adopts an
    // algorithm route.
    std::filesystem::path OperationStackJsonPath;
    std::string OperationStackJsonExpectedSha256;

    std::filesystem::path OutputDirectory;
    std::string AnimationStackName;
    std::string ClipId = "ue_ik_json_clip";
    std::string ClipLabel = "UE IK JSON retarget clip";
    std::string FoundationExportFbxFileName =
        "UEIK_Foundation_With_Target_Mesh.fbx";
    std::string ExportFbxFileName =
        "UEIK_Final_With_Target_Mesh.fbx";
    double SampleRate = 30.0;
    double RestTranslationToleranceCm = 0.25;
    double RestRotationToleranceDegrees = 0.25;
    double RestScaleTolerance = 1.0e-3;
};

struct UEIKJsonRetargetResult
{
    bool Success = false;
    std::filesystem::path ViewerPath;
    std::filesystem::path ExportedFoundationFbxPath;
    std::string ExportedFoundationFbxSha256;
    std::filesystem::path ExportedFbxPath;
    std::string ExportedFbxSha256;
    std::string ConsoleSummary;
    std::vector<std::string> Warnings;
    std::vector<std::string> Errors;
    std::vector<RetargetReviewPackageArtifact> Artifacts;
};

UEIKJsonRetargetResult GenerateUEIKJsonRetargetReview(
    const UEIKJsonRetargetOptions& Options);
} // namespace skrtg::fbx
