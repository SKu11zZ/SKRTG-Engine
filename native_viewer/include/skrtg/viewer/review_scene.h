#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::viewer
{
struct Vec3
{
    float X = 0.0F;
    float Y = 0.0F;
    float Z = 0.0F;
};

struct Quaternion
{
    float X = 0.0F;
    float Y = 0.0F;
    float Z = 0.0F;
    float W = 1.0F;
};

struct Bone
{
    int ParentIndex = -1;
    std::string Name;
    std::string Path;
    bool ParticipatesInIk = false;
    Vec3 RestPosition;
};

struct PoseLane
{
    std::vector<Vec3> ModelPositions;
    std::vector<Quaternion> ModelRotations;
    std::vector<Vec3> ModelScales;
};

struct RetargetChain
{
    std::string Label;
    std::string IkMode;
    std::string SourceGoalName;
    std::string TargetGoalName;
    int SourceGoalBone = -1;
    int TargetGoalBone = -1;
    int SourcePoleBone = -1;
    int TargetPoleBone = -1;
};

struct Mesh
{
    std::string Name;
    std::string Path;
    std::string SkinMode;
    std::vector<Vec3> BindPositions;
    std::vector<std::uint32_t> TriangleIndices;
    std::vector<std::uint32_t> InfluenceOffsets;
    std::vector<std::uint32_t> InfluenceClusterIndices;
    std::vector<float> InfluenceWeights;
    std::vector<std::uint32_t> ClusterBoneIndices;
    // One affine 3x4 transform per cluster. Each transform is stored as
    // translation xyz followed by the three basis columns, matching SKRV v1.
    std::vector<std::array<float, 12>> ClusterBindOffsets;
    std::array<float, 12> FallbackAffine = {
        0.0F, 0.0F, 0.0F,
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
};

struct MeshPackage
{
    std::string Label;
    std::uint64_t ControlPointCount = 0;
    std::uint64_t TriangleCount = 0;
    std::vector<Mesh> Meshes;
};

enum class ReviewLane
{
    Original,
    Fk,
    Foundation,
    Final
};

struct CameraFollowTarget
{
    std::string Label;
    int SourceBone = -1;
    int TargetBone = -1;
};

struct GoalTrail
{
    std::size_t ChainIndex = 0;
    std::vector<Vec3> SourceOriginal;
    std::vector<Vec3> SourceAlignedFk;
    std::vector<Vec3> TargetFk;
    std::vector<Vec3> SourceAlignedFoundation;
    std::vector<Vec3> TargetFoundation;
    std::vector<Vec3> TargetFinal;
};

struct GoalHistory
{
    std::uint64_t FirstFrame = 0;
    std::uint64_t LastFrame = 0;
    std::size_t SampleCount = 0;
    std::vector<GoalTrail> Trails;
};

struct GoalHistoryLoadResult
{
    bool Success = false;
    GoalHistory History;
    std::vector<std::string> Errors;
};

struct DisplayAnchor
{
    std::string Label;
    std::string SourcePath;
    std::string TargetPath;
    std::size_t SourceBone = 0;
    std::size_t TargetBone = 0;
    // SKRV v1 stores basis as qx, qy, qz, qw.
    std::array<float, 4> Basis = {0.0F, 0.0F, 0.0F, 1.0F};
};

struct ReviewClipInfo
{
    std::string Id;
    std::string Label;
    std::uint64_t FrameCount = 0;
    double FramesPerSecond = 0.0;
    bool SourceMotionFootLockEnabled = false;
    bool SourceMotionFootLockSuccess = false;
    bool FootLockHasStoredPoseDelta = false;
};

struct FootLockDeltaSummary
{
    bool PoseValid = false;
    bool GoalHistoryValid = false;
    float MaximumBonePositionCm = 0.0F;
    float MaximumBoneRotationDegrees = 0.0F;
    float LeftFootGoalPositionCm = 0.0F;
    float RightFootGoalPositionCm = 0.0F;
    float MaximumRecentFootGoalPositionCm = 0.0F;
    std::string MaximumBoneName;
};

struct ReviewScene
{
    std::filesystem::path PackageDirectory;
    std::string RouteId;
    bool RouteSelected = false;
    bool RouteAdopted = false;
    std::string FoundationRouteId;
    bool FoundationFrozen = true;
    // UE IK JSON snapshots are stored in UE's +X forward, +Y right,
    // +Z up left-handed coordinates. The Native Viewer is Y-up. This flag
    // enables an explicit display-only basis conversion after SKRV
    // verification; it never changes the stored package or exported FBX.
    bool UEIKJsonDisplayBasisConversion = false;
    std::size_t ClipIndex = 0;
    std::vector<ReviewClipInfo> Clips;
    std::string ClipId;
    std::string ClipLabel;
    std::uint64_t ClipFrameCount = 0;
    std::uint64_t FrameIndex = 0;
    double FramesPerSecond = 0.0;
    std::vector<Bone> SourceBones;
    std::vector<Bone> TargetBones;
    std::vector<RetargetChain> RetargetChains;
    int SourceRootBone = -1;
    int SourcePelvisBone = -1;
    int TargetHipsBone = -1;
    MeshPackage SourceMesh;
    MeshPackage TargetMesh;
    PoseLane Source;
    PoseLane Fk;
    PoseLane Foundation;
    PoseLane Final;
    DisplayAnchor GhostAnchor;
    float SourceGhostOpacity = 0.1F;
    // These paths come from a package that passed strict SKRV inspection.
    // They are retained so playback can read another frame without rerunning
    // the complete package hash inventory on every display tick.
    std::filesystem::path SourcePoseRelativePath;
    std::filesystem::path FkPoseRelativePath;
    std::filesystem::path FoundationPoseRelativePath;
    std::filesystem::path FinalPoseRelativePath;
};

struct ReviewSceneLoadResult
{
    bool Success = false;
    ReviewScene Scene;
    std::vector<std::string> Errors;
};

ReviewSceneLoadResult LoadReviewScene(
    const std::filesystem::path& PackageDirectory,
    std::size_t ClipIndex = 0,
    std::uint64_t FrameIndex = 0);

bool LoadVerifiedReviewSceneFrame(
    ReviewScene& Scene,
    std::uint64_t FrameIndex,
    std::vector<std::string>& OutErrors);

struct ReviewExportResult
{
    bool Success = false;
    std::filesystem::path SourceFbx;
    std::string SuggestedFileName;
    std::string ExpectedSha256;
    std::vector<std::string> Errors;
};

ReviewExportResult FindVerifiedReviewExport(
    const ReviewScene& Scene,
    const std::string& Lane);

ReviewExportResult FindVerifiedReviewExport(
    const std::filesystem::path& PackageDirectory,
    const std::string& ClipId,
    const std::string& Lane);

const ReviewClipInfo* CurrentReviewClipInfo(const ReviewScene& Scene);

bool IsUEIKJsonCandidateRoute(const ReviewScene& Scene);

bool FootLockComparisonAvailable(const ReviewScene& Scene);

ReviewLane ResolveFootLockDisplayLane(
    const ReviewScene& Scene,
    bool FootLockEnabled);

FootLockDeltaSummary MeasureFootLockDelta(
    const ReviewScene& Scene,
    const GoalHistory& History);

std::vector<Vec3> BuildAnchorAlignedSourceGhost(
    const ReviewScene& Scene,
    const PoseLane& TargetLane);

std::vector<Vec3> BuildAnchorAlignedSourcePoints(
    const ReviewScene& Scene,
    const PoseLane& TargetLane,
    const std::vector<Vec3>& SourcePoints);

std::vector<CameraFollowTarget> BuildCameraFollowTargets(
    const ReviewScene& Scene);

bool ResolveCameraFollowPoint(
    const ReviewScene& Scene,
    ReviewLane Lane,
    const CameraFollowTarget& Target,
    Vec3& OutPoint);

GoalHistoryLoadResult LoadReviewGoalHistory(
    const ReviewScene& Scene,
    std::size_t MaximumSamples = 50);

} // namespace skrtg::viewer
