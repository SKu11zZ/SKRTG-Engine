#pragma once

#include "skrtg/core/animation/pose.h"
#include "skrtg/core/math/transform.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::fbx
{
struct RetargetReviewBone
{
    int ParentIndex = -1;
    std::string Name;
    std::string Path;
    bool ParticipatesInIk = false;
    core::math::Vec3 RestModelPositionCm;
};

struct RetargetReviewChain
{
    std::string Label;
    std::vector<int> SourceBoneIndices;
    std::vector<int> TargetBoneIndices;
    std::string IkMode = "fk_only";
    std::string SourceGoalName;
    std::string TargetGoalName;
    int SourceGoalBoneIndex = -1;
    int TargetGoalBoneIndex = -1;
    int SourcePoleBoneIndex = -1;
    int TargetPoleBoneIndex = -1;
};

struct RetargetReviewRootPelvisContract
{
    int SourceRootBoneIndex = -1;
    int SourcePelvisBoneIndex = -1;
    int TargetHipsBoneIndex = -1;
    std::string RootOwnership;
    std::string PelvisOwnership;
    std::string ScaleOwnership;
};

struct RetargetReviewAnchor
{
    std::string Label;
    int SourceBoneIndex = -1;
    int TargetBoneIndex = -1;
    std::string SourcePath;
    std::string TargetPath;
    core::math::Quat SourceToTargetRestBasis;
};

struct RetargetReviewFrameView
{
    int FrameIndex = 0;
    double TimeSeconds = 0.0;
    const std::vector<core::math::TransformRT>* SourceModelPose = nullptr;
    const core::animation::PoseBuffer* TargetFkModelPose = nullptr;
    const core::animation::PoseBuffer* TargetFoundationLocalPose = nullptr;
    const core::animation::PoseBuffer* TargetFoundationModelPose = nullptr;
    const core::animation::PoseBuffer* TargetFinalLocalPose = nullptr;
    const core::animation::PoseBuffer* TargetFinalModelPose = nullptr;
};

struct RetargetReviewClipView
{
    std::string Id;
    std::string Label;
    double FramesPerSecond = 0.0;
    std::filesystem::path SourceAnimationFbxPath;
    std::string SourceAnimationSha256;
    int SourceDirectBoneCount = 0;
    int SourceRestPassthroughBoneCount = 0;
    bool LimbIkUnitScaleShadowProjectionApplied = false;
    int LimbIkFamilyTransactions = 0;
    int LimbIkCommittedFamilyTransactions = 0;
    int LimbIkRolledBackFamilyTransactions = 0;
    int LimbIkAppliedChainRecords = 0;
    int LimbIkFailClosedChainRecords = 0;
    double LimbIkMaximumEndpointErrorCm = 0.0;
    double LimbIkMaximumShadowToRealPositionDeltaCm = 0.0;
    bool OperationStackEnabled = false;
    bool SourceMotionFootLockEnabled = false;
    bool SourceMotionFootLockSuccess = false;
    bool SourceMotionFootLockDeterministic = false;
    bool SourceMotionFootLockNoGroundOrContactSemanticsUsed = false;
    int SourceMotionFootLockCommittedFrames = 0;
    int SourceMotionFootLockRolledBackFrames = 0;
    int SourceMotionFootLockPositionNoMotionDeltas = 0;
    int SourceMotionFootLockPositionMotionDeltas = 0;
    int SourceMotionFootLockRotationNoMotionDeltas = 0;
    int SourceMotionFootLockRotationMotionDeltas = 0;
    int SourceMotionFootLockRotationGateReleases = 0;
    double SourceMotionFootLockMaximumReleasedFoundationRotationDegrees = 0.0;
    double SourceMotionFootLockMaximumRealEndOrientationErrorDegrees = 0.0;
    double SourceMotionFootLockMaximumNoMotionTargetDriftCm = 0.0;
    double SourceMotionFootLockMaximumTargetDeltaErrorCm = 0.0;
    std::string FoundationExportFbxFileName;
    std::string ExportFbxFileName;
    std::vector<RetargetReviewFrameView> Frames;
};

struct RetargetReviewMeshSelection
{
    int ActiveLod = -1;
    std::vector<std::string> MeshNodePaths;
};

struct RetargetReviewPackageOptions
{
    std::string ContractKind = "foundation_v1";
    std::filesystem::path SourceAnimationFbxPath;
    std::string SourceAnimationExpectedSha256;
    // Optional display-only source Mesh provider for a first/only animation
    // FBX that contains skeleton animation but no Mesh. Its skeleton identity
    // is still checked by ExtractMeshPackage against SourceBones.
    std::filesystem::path SourceMeshFallbackFbxPath;
    std::string SourceMeshFallbackExpectedSha256;
    std::filesystem::path TargetTposeFbxPath;
    std::string TargetTposeExpectedSha256;
    std::filesystem::path OutputDirectory;
    std::string RouteId;
    std::string FoundationRouteId = "skrtg_fkik_foundation_v1";
    bool FoundationFrozen = false;
    std::string SourceMotionFootLockRouteId =
        "source_motion_foot_lock_no_ground_semantics_v1";
    bool SourceMotionFootLockCandidateEnabled = false;
    bool SourceMotionFootLockCandidateSelected = false;
    bool SourceMotionFootLockCandidateAdopted = false;
    bool OperationStackCandidateEnabled = false;
    bool OperationStackCandidateSelected = false;
    bool OperationStackCandidateAdopted = false;
    std::vector<RetargetReviewBone> SourceBones;
    std::vector<RetargetReviewBone> TargetBones;
    std::vector<RetargetReviewChain> RetargetChains;
    RetargetReviewRootPelvisContract RootPelvisContract;
    std::vector<RetargetReviewAnchor> Anchors;
    std::vector<RetargetReviewClipView> Clips;
    double ExportLocalTranslationToleranceCm = 0.001;
    double ExportModelTranslationToleranceCm = 0.01;
    double ExportRotationToleranceDegrees = 0.001;
    double ExportScaleTolerance = 1.0e-5;
    bool ScalePolicySelectedByUser = false;
    bool AllowSharedSourceMeshFallbackForMeshlessClips = false;
    bool UpstreamLimbIkRouteSelected = false;
    bool UpstreamLimbIkRouteAdopted = false;
    bool SpinePelvisFollowCandidateEnabled = false;
    bool SpinePelvisFollowCandidateSelected = false;
    bool SpinePelvisFollowCandidateAdopted = false;
    // Opt-in explicit UE JSON <-> native FBX basis adapter for versioned UE
    // JSON routes. Manual FBX routes retain their declared input basis.
    bool NormalizeFbxToUEJsonSpace = false;
    // Exact, profile-authored FBX scene paths. Review payloads include only
    // these Mesh nodes; source and exported FBX files retain their complete
    // LOD inventory.
    RetargetReviewMeshSelection SourceMeshSelection;
    RetargetReviewMeshSelection TargetMeshSelection;
    bool RequireExplicitMeshSelectionForMultipleMeshes = false;
};

struct RetargetReviewPackageArtifact
{
    std::filesystem::path Path;
    std::string Text;
};

struct RetargetReviewPackageResult
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

RetargetReviewPackageResult GenerateRetargetReviewPackage(
    const RetargetReviewPackageOptions& Options);
} // namespace skrtg::fbx
