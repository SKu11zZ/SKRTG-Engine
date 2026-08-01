#include "skrtg/viewer/mesh_skinning.h"
#include "skrtg/viewer/review_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int Failures = 0;

void Check(const bool Condition, const std::string& Label)
{
    if (Condition)
    {
        std::cout << "PASS " << Label << '\n';
        return;
    }
    std::cerr << "FAIL " << Label << '\n';
    ++Failures;
}

bool FinitePose(
    const skrtg::viewer::PoseLane& Pose,
    const std::size_t BoneCount)
{
    if (Pose.ModelPositions.size() != BoneCount ||
        Pose.ModelRotations.size() != BoneCount ||
        Pose.ModelScales.size() != BoneCount)
    {
        return false;
    }
    for (std::size_t Index = 0; Index < BoneCount; ++Index)
    {
        const auto& P = Pose.ModelPositions[Index];
        const auto& Q = Pose.ModelRotations[Index];
        const auto& S = Pose.ModelScales[Index];
        const float Norm = std::sqrt(
            Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W);
        if (!std::isfinite(P.X) || !std::isfinite(P.Y) ||
            !std::isfinite(P.Z) || !std::isfinite(Q.X) ||
            !std::isfinite(Q.Y) || !std::isfinite(Q.Z) ||
            !std::isfinite(Q.W) || !std::isfinite(S.X) ||
            !std::isfinite(S.Y) || !std::isfinite(S.Z) ||
            std::fabs(Norm - 1.0F) > 1.0e-3F ||
            std::fabs(S.X - 1.0F) > 1.0e-3F ||
            std::fabs(S.Y - 1.0F) > 1.0e-3F ||
            std::fabs(S.Z - 1.0F) > 1.0e-3F)
        {
            return false;
        }
    }
    return true;
}

float PositionDistance(
    const skrtg::viewer::Vec3& Left,
    const skrtg::viewer::Vec3& Right)
{
    const float X = Left.X - Right.X;
    const float Y = Left.Y - Right.Y;
    const float Z = Left.Z - Right.Z;
    return std::sqrt(X * X + Y * Y + Z * Z);
}

bool FoundationEqualsFinal(const skrtg::viewer::ReviewScene& Scene)
{
    if (Scene.Foundation.ModelPositions.size() !=
            Scene.Final.ModelPositions.size() ||
        Scene.Foundation.ModelRotations.size() !=
            Scene.Final.ModelRotations.size() ||
        Scene.Foundation.ModelScales.size() !=
            Scene.Final.ModelScales.size())
    {
        return false;
    }
    for (std::size_t Index = 0;
         Index < Scene.Foundation.ModelPositions.size(); ++Index)
    {
        const auto& FP = Scene.Foundation.ModelPositions[Index];
        const auto& RP = Scene.Final.ModelPositions[Index];
        const auto& FQ = Scene.Foundation.ModelRotations[Index];
        const auto& RQ = Scene.Final.ModelRotations[Index];
        const auto& FS = Scene.Foundation.ModelScales[Index];
        const auto& RS = Scene.Final.ModelScales[Index];
        if (PositionDistance(FP, RP) > 1.0e-6F ||
            std::fabs(std::fabs(
                FQ.X * RQ.X + FQ.Y * RQ.Y +
                FQ.Z * RQ.Z + FQ.W * RQ.W) - 1.0F) > 1.0e-5F ||
            PositionDistance(FS, RS) > 1.0e-6F)
        {
            return false;
        }
    }
    return true;
}

void TestPackage(const std::filesystem::path& Package)
{
    const std::string Prefix =
        Package.parent_path().parent_path().filename().string();
    const skrtg::viewer::ReviewSceneLoadResult Loaded =
        skrtg::viewer::LoadReviewScene(Package, 0, 0);
    Check(Loaded.Success, Prefix + "_load");
    if (!Loaded.Success)
    {
        for (const std::string& Error : Loaded.Errors)
            std::cerr << Error << '\n';
        return;
    }
    skrtg::viewer::ReviewScene Scene = Loaded.Scene;
    Check(
        Scene.RouteId == "ue_ik_json_canonical_bridge_v1" &&
        Scene.FoundationRouteId ==
            "ue_ik_json_fk_pelvis_limb_ik_candidate_v1" &&
        !Scene.RouteSelected && !Scene.RouteAdopted &&
        !Scene.FoundationFrozen &&
        skrtg::viewer::IsUEIKJsonCandidateRoute(Scene),
        Prefix + "_candidate_route_state");
    Check(
        Scene.UEIKJsonDisplayBasisConversion,
        Prefix + "_display_basis_conversion");
    Check(
        Scene.SourceBones.size() == 65 &&
        !Scene.TargetBones.empty() &&
        Scene.RetargetChains.size() == 21,
        Prefix + "_skeleton_and_chain_inventory");
    const std::size_t IkChainCount = static_cast<std::size_t>(
        std::count_if(
            Scene.RetargetChains.begin(),
            Scene.RetargetChains.end(),
            [](const skrtg::viewer::RetargetChain& Chain)
            {
                return Chain.IkMode != "fk_only";
            }));
    Check(IkChainCount == 4, Prefix + "_four_limb_ik_chains");
    Check(
        !skrtg::viewer::FootLockComparisonAvailable(Scene) &&
        skrtg::viewer::ResolveFootLockDisplayLane(Scene, false) ==
            skrtg::viewer::ReviewLane::Final &&
        skrtg::viewer::ResolveFootLockDisplayLane(Scene, true) ==
            skrtg::viewer::ReviewLane::Final,
        Prefix + "_foot_lock_fail_closed");
    Check(
        Scene.GhostAnchor.Label == "pelvis" &&
        std::fabs(Scene.SourceGhostOpacity - 0.1F) <= 1.0e-6F,
        Prefix + "_ghost_contract");

    float MaximumTargetFrameStepCm = 0.0F;
    bool AllFramesFinite = true;
    bool AllFramesFoundationEqualFinal = true;
    bool PoseChanged = false;
    std::vector<skrtg::viewer::Vec3> PreviousTarget;
    std::vector<std::string> Errors;
    for (std::uint64_t Frame = 0;
         Frame < Scene.ClipFrameCount; ++Frame)
    {
        if (Frame > 0 &&
            !skrtg::viewer::LoadVerifiedReviewSceneFrame(
                Scene, Frame, Errors))
        {
            AllFramesFinite = false;
            break;
        }
        AllFramesFinite =
            AllFramesFinite &&
            FinitePose(Scene.Source, Scene.SourceBones.size()) &&
            FinitePose(Scene.Fk, Scene.TargetBones.size()) &&
            FinitePose(Scene.Foundation, Scene.TargetBones.size()) &&
            FinitePose(Scene.Final, Scene.TargetBones.size());
        AllFramesFoundationEqualFinal =
            AllFramesFoundationEqualFinal &&
            FoundationEqualsFinal(Scene);
        if (!PreviousTarget.empty())
        {
            for (std::size_t Index = 0;
                 Index < PreviousTarget.size(); ++Index)
            {
                const float Step = PositionDistance(
                    PreviousTarget[Index],
                    Scene.Final.ModelPositions[Index]);
                MaximumTargetFrameStepCm =
                    std::max(MaximumTargetFrameStepCm, Step);
                PoseChanged = PoseChanged || Step > 1.0e-4F;
            }
        }
        PreviousTarget = Scene.Final.ModelPositions;

        if (Frame == 0 ||
            Frame == Scene.ClipFrameCount / 2 ||
            Frame + 1 == Scene.ClipFrameCount)
        {
            const auto SourceSkin =
                skrtg::viewer::SkinMeshPackage(
                    Scene.SourceMesh, Scene.Source);
            const auto TargetSkin =
                skrtg::viewer::SkinMeshPackage(
                    Scene.TargetMesh, Scene.Final);
            AllFramesFinite =
                AllFramesFinite &&
                SourceSkin.Success && TargetSkin.Success;
        }
    }
    Check(Errors.empty(), Prefix + "_all_frames_read");
    Check(AllFramesFinite, Prefix + "_all_frames_finite_and_skin_valid");
    Check(
        AllFramesFoundationEqualFinal,
        Prefix + "_candidate_has_no_hidden_postprocess");
    Check(PoseChanged, Prefix + "_dynamic_pose_changes");

    const auto History =
        skrtg::viewer::LoadReviewGoalHistory(Scene, 50);
    Check(
        History.Success &&
        History.History.SampleCount ==
            std::min<std::size_t>(
                50, static_cast<std::size_t>(Scene.ClipFrameCount)) &&
        History.History.Trails.size() == 4,
        Prefix + "_goal_history");
    const auto FoundationExport =
        skrtg::viewer::FindVerifiedReviewExport(Scene, "foundation");
    const auto FinalExport =
        skrtg::viewer::FindVerifiedReviewExport(Scene, "final");
    Check(
        FoundationExport.Success && FinalExport.Success &&
        FoundationExport.ExpectedSha256.size() == 64 &&
        FinalExport.ExpectedSha256.size() == 64,
        Prefix + "_verified_exports");
    std::cout << "METRIC " << Prefix
              << " frame_count=" << Scene.ClipFrameCount
              << " max_target_frame_step_cm="
              << MaximumTargetFrameStepCm << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr
            << "usage: ueik_candidate_review_scene_tests <candidate.skrv> [more.skrv]\n";
        return 2;
    }
    for (int Index = 1; Index < argc; ++Index)
        TestPackage(std::filesystem::path(argv[Index]));
    std::cout << "ueik_candidate_review_scene_tests failures="
              << Failures << '\n';
    return Failures == 0 ? 0 : 1;
}
