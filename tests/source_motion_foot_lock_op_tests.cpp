#include "skrtg/retarget/ops/source_motion_foot_lock_op.h"

#include "skrtg/core/math/transform.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>

namespace
{
using skrtg::core::animation::PoseBuffer;
using skrtg::core::animation::PoseSpace;
using skrtg::core::math::Add;
using skrtg::core::math::Compose;
using skrtg::core::math::FromAxisAngleDegrees;
using skrtg::core::math::IdentityTransform;
using skrtg::core::math::Length;
using skrtg::core::math::Multiply;
using skrtg::core::math::NearlyEqual;
using skrtg::core::math::Subtract;
using skrtg::core::math::TransformRT;
using skrtg::core::math::Vec3;
using skrtg::core::skeleton::NormalizedRuntimeSkeleton;
using skrtg::core::skeleton::RuntimeBone;
using skrtg::core::skeleton::SkeletonIdentity;
using skrtg::retarget::EquivalentRetargetOpClips;
using skrtg::retarget::RetargetOpClip;
using skrtg::retarget::RetargetOpFrame;
using skrtg::retarget::RetargetOpStack;
using skrtg::retarget::ops::RunSourceMotionFootLock;
using skrtg::retarget::ops::SourceMotionFootLockBinding;
using skrtg::retarget::ops::SourceMotionFootLockFailureReason;
using skrtg::retarget::ops::SourceMotionFootLockOp;
using skrtg::retarget::ops::SourceMotionFootLockOptions;

int Failures = 0;

void Check(bool Condition, const std::string& Message)
{
    if (Condition) return;
    ++Failures;
    std::cerr << "FAIL: " << Message << '\n';
}

RuntimeBone Bone(const std::string& Name,
                 int Parent,
                 int Index,
                 TransformRT Rest)
{
    RuntimeBone Result;
    Result.Name = Name;
    Result.RawPath = "synthetic/" + Name;
    Result.ParentIndex = Parent;
    Result.RawIndex = Index;
    Result.LocalRest = Rest;
    return Result;
}

NormalizedRuntimeSkeleton MakeSkeleton(const std::string& Label,
                                       double FirstLength,
                                       double SecondLength,
                                       double ScaleResidue = 0.0)
{
    NormalizedRuntimeSkeleton Skeleton;
    SkeletonIdentity Identity;
    Identity.HierarchyHash = "source-motion-foot-lock-" + Label +
        "-hierarchy";
    Identity.RestPoseHash = "source-motion-foot-lock-" + Label + "-rest";
    Identity.SourceAssetId = "synthetic";
    Skeleton.SetIdentity(Identity);

    TransformRT Hips = IdentityTransform();
    Hips.TranslationCm = {0.0, 90.0, 0.0};
    Hips.Scale = {1.0 + ScaleResidue,
                  1.0 - ScaleResidue,
                  1.0 + ScaleResidue * 0.5};
    Skeleton.AddBone(Bone("Hips", -1, 0, Hips));

    TransformRT LeftUp = IdentityTransform();
    LeftUp.TranslationCm = {-5.0, 0.0, 0.0};
    Skeleton.AddBone(Bone("LeftUpLeg", 0, 1, LeftUp));
    TransformRT LeftLeg = IdentityTransform();
    LeftLeg.TranslationCm = {0.0, -FirstLength, 0.0};
    LeftLeg.Rotation = FromAxisAngleDegrees({0.0, 0.0, 1.0}, 25.0);
    Skeleton.AddBone(Bone("LeftLeg", 1, 2, LeftLeg));
    TransformRT LeftFoot = IdentityTransform();
    LeftFoot.TranslationCm = {0.0, -SecondLength, 0.0};
    LeftFoot.Rotation = FromAxisAngleDegrees({1.0, 0.0, 0.0}, 8.0);
    Skeleton.AddBone(Bone("LeftFoot", 2, 3, LeftFoot));

    TransformRT RightUp = IdentityTransform();
    RightUp.TranslationCm = {5.0, 0.0, 0.0};
    Skeleton.AddBone(Bone("RightUpLeg", 0, 4, RightUp));
    TransformRT RightLeg = IdentityTransform();
    RightLeg.TranslationCm = {0.0, -FirstLength, 0.0};
    RightLeg.Rotation = FromAxisAngleDegrees({0.0, 0.0, 1.0}, -25.0);
    Skeleton.AddBone(Bone("RightLeg", 4, 5, RightLeg));
    TransformRT RightFoot = IdentityTransform();
    RightFoot.TranslationCm = {0.0, -SecondLength, 0.0};
    RightFoot.Rotation = FromAxisAngleDegrees({1.0, 0.0, 0.0}, -8.0);
    Skeleton.AddBone(Bone("RightFoot", 5, 6, RightFoot));

    TransformRT Sentinel = IdentityTransform();
    Sentinel.TranslationCm = {0.0, 12.0, 2.0};
    Sentinel.Rotation = FromAxisAngleDegrees({0.0, 1.0, 0.0}, 11.0);
    Skeleton.AddBone(Bone("SpineSentinel", 0, 7, Sentinel));
    return Skeleton;
}

PoseBuffer RestLocal(const NormalizedRuntimeSkeleton& Skeleton)
{
    PoseBuffer Result(PoseSpace::Local,
                      Skeleton.GetIdentity().HierarchyHash);
    Result.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
        Result[Index] = Skeleton.BoneAt(Index).LocalRest;
    return Result;
}

PoseBuffer Model(const NormalizedRuntimeSkeleton& Skeleton,
                 const PoseBuffer& Local)
{
    PoseBuffer Result(PoseSpace::Model,
                      Skeleton.GetIdentity().HierarchyHash);
    Result.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        const int Parent = Skeleton.BoneAt(Index).ParentIndex;
        Result[Index] = Parent < 0
            ? Local[Index]
            : Compose(Result[static_cast<std::size_t>(Parent)],
                      Local[Index]);
    }
    return Result;
}

SourceMotionFootLockOptions Options()
{
    SourceMotionFootLockOptions Result;
    Result.Feet[0].Label = "left_foot";
    Result.Feet[0].SourceIndices = {{1, 2, 3}};
    Result.Feet[0].TargetIndices = {{1, 2, 3}};
    Result.Feet[1].Label = "right_foot";
    Result.Feet[1].SourceIndices = {{4, 5, 6}};
    Result.Feet[1].TargetIndices = {{4, 5, 6}};
    return Result;
}

RetargetOpClip MakeClip(const NormalizedRuntimeSkeleton& Source,
                        const NormalizedRuntimeSkeleton& Target)
{
    RetargetOpClip Clip;
    const Vec3 LeftMotion{0.5, 0.0, 0.0};
    const Vec3 RightMotion{0.0, 0.0, 0.4};
    for (int FrameIndex = 0; FrameIndex < 4; ++FrameIndex)
    {
        PoseBuffer SourceLocal = RestLocal(Source);
        PoseBuffer SourceModel = Model(Source, SourceLocal);
        if (FrameIndex >= 2)
        {
            SourceModel[3].TranslationCm = Add(
                SourceModel[3].TranslationCm, LeftMotion);
            SourceModel[3].Rotation = Multiply(
                FromAxisAngleDegrees({0.0, 1.0, 0.0}, 5.0),
                SourceModel[3].Rotation);
            SourceModel[6].TranslationCm = Add(
                SourceModel[6].TranslationCm, RightMotion);
            SourceModel[6].Rotation = Multiply(
                FromAxisAngleDegrees({1.0, 0.0, 0.0}, -4.0),
                SourceModel[6].Rotation);
        }

        PoseBuffer TargetLocal = RestLocal(Target);
        TargetLocal[0].TranslationCm.X += FrameIndex * 0.25;
        RetargetOpFrame Frame;
        Frame.FrameIndex = FrameIndex;
        Frame.TimeSeconds = FrameIndex / 30.0;
        Frame.SourceModelPose = std::move(SourceModel);
        Frame.TargetLocalPose = TargetLocal;
        Frame.TargetModelPose = Model(Target, TargetLocal);
        Clip.Frames.push_back(std::move(Frame));
    }
    return Clip;
}

bool SameTransform(TransformRT A, TransformRT B)
{
    return A.TranslationCm.X == B.TranslationCm.X &&
        A.TranslationCm.Y == B.TranslationCm.Y &&
        A.TranslationCm.Z == B.TranslationCm.Z &&
        A.Rotation.X == B.Rotation.X &&
        A.Rotation.Y == B.Rotation.Y &&
        A.Rotation.Z == B.Rotation.Z &&
        A.Rotation.W == B.Rotation.W &&
        A.Scale.X == B.Scale.X && A.Scale.Y == B.Scale.Y &&
        A.Scale.Z == B.Scale.Z;
}

void CheckSourceMotionContract(
    const NormalizedRuntimeSkeleton& Source,
    const NormalizedRuntimeSkeleton& Target,
    const RetargetOpClip& Foundation,
    const RetargetOpClip& Output)
{
    (void)Source;
    (void)Target;
    const std::array<int, 2> Ends{{3, 6}};
    for (int End : Ends)
    {
        const Vec3 P0 = Output.Frames[0].TargetModelPose[
            static_cast<std::size_t>(End)].TranslationCm;
        const Vec3 P1 = Output.Frames[1].TargetModelPose[
            static_cast<std::size_t>(End)].TranslationCm;
        const Vec3 P2 = Output.Frames[2].TargetModelPose[
            static_cast<std::size_t>(End)].TranslationCm;
        const Vec3 P3 = Output.Frames[3].TargetModelPose[
            static_cast<std::size_t>(End)].TranslationCm;
        const Vec3 FoundationDelta = Subtract(
            Foundation.Frames[2].TargetModelPose[
                static_cast<std::size_t>(End)].TranslationCm,
            Foundation.Frames[0].TargetModelPose[
                static_cast<std::size_t>(End)].TranslationCm);
        Check(Length(Subtract(P1, P0)) <= 1.0e-3,
              "source-stationary frame moved a target foot");
        Check(Length(Subtract(Subtract(P2, P1), FoundationDelta)) <= 1.0e-3,
              "source-moving interval did not release the accumulated Foundation foot delta");
        Check(Length(Subtract(P3, P2)) <= 1.0e-3,
              "target foot drifted after source foot stopped");
        Check(NearlyEqual(
                  Output.Frames[2].TargetModelPose[
                      static_cast<std::size_t>(End)].Rotation,
                  Output.Frames[3].TargetModelPose[
                      static_cast<std::size_t>(End)].Rotation,
                  1.0e-8),
              "source-stationary orientation interval changed target foot orientation");
    }

    const std::set<int> Declared{1, 2, 3, 4, 5, 6};
    for (std::size_t Frame = 0; Frame < Output.Frames.size(); ++Frame)
    {
        for (std::size_t BoneIndex = 0;
             BoneIndex < Output.Frames[Frame].TargetLocalPose.Size();
             ++BoneIndex)
        {
            const TransformRT& Before =
                Foundation.Frames[Frame].TargetLocalPose[BoneIndex];
            const TransformRT& After =
                Output.Frames[Frame].TargetLocalPose[BoneIndex];
            Check(Before.TranslationCm.X == After.TranslationCm.X &&
                      Before.TranslationCm.Y == After.TranslationCm.Y &&
                      Before.TranslationCm.Z == After.TranslationCm.Z,
                  "FootLock changed a target local translation");
            Check(Before.Scale.X == After.Scale.X &&
                      Before.Scale.Y == After.Scale.Y &&
                      Before.Scale.Z == After.Scale.Z,
                  "FootLock changed a target local scale");
            if (Declared.count(static_cast<int>(BoneIndex)) == 0)
                Check(SameTransform(Before, After),
                      "FootLock changed an unrelated target local transform");
        }
    }
}

void TestPureSourceMotionRoute()
{
    const auto Source = MakeSkeleton("source", 10.0, 8.0);
    const auto Target = MakeSkeleton("target", 12.0, 9.0);
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    const auto Run = RunSourceMotionFootLock(
        Source, Target, Foundation, Options());
    Check(Run.Success && Run.InputImmutable &&
              Run.NoGroundOrContactSemanticsUsed &&
              Run.AllTransactionsBounded &&
              Run.FoundationSeedFramesExact,
          "pure source-motion FootLock route failed its top-level contracts");
    Check(Run.Coverage.InputFrames == 4 &&
              Run.Coverage.SeedPassthroughFrames == 1 &&
              Run.Coverage.CommittedFrames == 4 &&
              Run.Coverage.RolledBackFrames == 0 &&
              Run.Coverage.PositionNoMotionDeltas == 6 &&
              Run.Coverage.PositionMotionDeltas == 2,
          "source-motion coverage counts do not match the synthetic clip");
    Check(Run.Coverage.MaximumNoMotionTargetDriftCm <= 1.0e-3 &&
              Run.Coverage.MaximumTargetDeltaErrorCm <= 1.0e-3,
          "source-motion positional contract exceeded tolerance");
    Check(!Run.Frames[2].Feet[0].SolverExecuted &&
              !Run.Frames[2].Feet[1].SolverExecuted,
          "Foundation-equivalent moving-foot goals ran unnecessary IK");
    Check(EquivalentRetargetOpClips(
              Foundation, MakeClip(Source, Target)),
          "standalone FootLock mutated its foundation input");
    CheckSourceMotionContract(Source, Target, Foundation, Run.Output);
}

void TestToggleAndDeterminism()
{
    const auto Source = MakeSkeleton("source-stack", 10.0, 8.0);
    const auto Target = MakeSkeleton("target-stack", 12.0, 9.0);
    const RetargetOpClip Foundation = MakeClip(Source, Target);

    RetargetOpStack Disabled;
    Check(Disabled.Add("foot_lock",
                       std::make_unique<SourceMotionFootLockOp>(Options()),
                       false),
          "disabled FootLock op could not be registered");
    const auto DisabledRun = Disabled.Run(Source, Target, Foundation);
    Check(DisabledRun.Success &&
              EquivalentRetargetOpClips(Foundation,
                                        DisabledRun.FinalOutput),
          "disabled FootLock was not an exact Foundation passthrough");

    RetargetOpStack Enabled;
    Check(Enabled.Add("foot_lock",
                      std::make_unique<SourceMotionFootLockOp>(Options()),
                      true),
          "enabled FootLock op could not be registered");
    const auto EnabledRun = Enabled.Run(Source, Target, Foundation);
    Check(EnabledRun.Success && EnabledRun.FoundationInputImmutable &&
              EnabledRun.AllExecutedEntriesBounded &&
              EnabledRun.Stages.size() == 1 &&
              EnabledRun.Stages[0].Success,
          "enabled FootLock did not pass OpStack bounded execution");
    Check(!EquivalentRetargetOpClips(Foundation,
                                     EnabledRun.FinalOutput),
          "enabled FootLock produced no correction on a drifting foundation");
    CheckSourceMotionContract(Source, Target, Foundation,
                              EnabledRun.FinalOutput);
}

void TestScaleEligibilityFailsClosed()
{
    const auto Source = MakeSkeleton("source-scale", 10.0, 8.0);
    const auto Target = MakeSkeleton("target-scale", 12.0, 9.0,
                                     2.0e-5);
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    const auto Run = RunSourceMotionFootLock(
        Source, Target, Foundation, Options());
    Check(!Run.Success && Run.Coverage.RolledBackFrames == 3 &&
              EquivalentRetargetOpClips(Foundation, Run.Output),
          "out-of-eligibility target scale did not fail closed");
    for (std::size_t Frame = 1; Frame < Run.Frames.size(); ++Frame)
        Check(Run.Frames[Frame].FailureReason ==
                  SourceMotionFootLockFailureReason::
                      ScaleOutsideShadowEligibility &&
                  Run.Frames[Frame].FailureLeftInputUnchanged,
              "scale rejection did not preserve its frame transaction");
}

void TestEligibleScaleResidueSucceeds()
{
    const auto Source = MakeSkeleton("source-scale-eligible", 10.0, 8.0);
    const auto Target = MakeSkeleton("target-scale-eligible", 12.0, 9.0,
                                     5.0e-6);
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    const auto Run = RunSourceMotionFootLock(
        Source, Target, Foundation, Options());
    Check(Run.Success && Run.AllTransactionsBounded &&
              Run.Coverage.RolledBackFrames == 0 &&
              Run.Coverage.MaximumEligibleScaleDeviation <= 1.0e-5,
          "eligible target scale residue was rejected by segment validation");
}

void TestSubthresholdMotionAccumulates()
{
    const auto Source = MakeSkeleton("source-slow-motion", 10.0, 8.0);
    const auto Target = MakeSkeleton("target-slow-motion", 12.0, 9.0);
    RetargetOpClip Foundation = MakeClip(Source, Target);
    const PoseBuffer SourceRest = Model(Source, RestLocal(Source));
    for (std::size_t Frame = 0; Frame < Foundation.Frames.size(); ++Frame)
    {
        Foundation.Frames[Frame].SourceModelPose = SourceRest;
        Foundation.Frames[Frame].SourceModelPose[3].TranslationCm.X +=
            static_cast<double>(Frame) * 4.0e-4;
        Foundation.Frames[Frame].TargetLocalPose = RestLocal(Target);
        Foundation.Frames[Frame].TargetLocalPose[0].TranslationCm.X +=
            static_cast<double>(Frame) * 4.0e-4;
        Foundation.Frames[Frame].TargetModelPose = Model(
            Target, Foundation.Frames[Frame].TargetLocalPose);
    }
    const auto Run = RunSourceMotionFootLock(
        Source, Target, Foundation, Options());
    Check(Run.Success && Run.Coverage.PositionMotionDeltas == 1 &&
              Run.Coverage.PositionNoMotionDeltas == 7,
          "subthreshold source motion was discarded instead of accumulated");
    const Vec3 P0 = Run.Output.Frames[0].TargetModelPose[3].TranslationCm;
    const Vec3 P1 = Run.Output.Frames[1].TargetModelPose[3].TranslationCm;
    const Vec3 P2 = Run.Output.Frames[2].TargetModelPose[3].TranslationCm;
    const Vec3 P3 = Run.Output.Frames[3].TargetModelPose[3].TranslationCm;
    Check(Length(Subtract(P1, P0)) <= 1.0e-3 &&
              Length(Subtract(P2, P1)) <= 1.0e-3 &&
              std::abs(Length(Subtract(P3, P2)) - 1.2e-3) <= 1.0e-3,
          "accumulated slow source motion did not release at the tolerance boundary");
}

void TestSubthresholdRotationAccumulatesAndReleasesFoundationRotation()
{
    const auto Source = MakeSkeleton("source-slow-rotation", 10.0, 8.0);
    const auto Target = MakeSkeleton("target-slow-rotation", 12.0, 9.0);
    RetargetOpClip Foundation = MakeClip(Source, Target);
    const PoseBuffer SourceRest = Model(Source, RestLocal(Source));
    for (std::size_t Frame = 0; Frame < Foundation.Frames.size(); ++Frame)
    {
        Foundation.Frames[Frame].SourceModelPose = SourceRest;
        Foundation.Frames[Frame].SourceModelPose[3].Rotation = Multiply(
            FromAxisAngleDegrees(
                {0.0, 1.0, 0.0},
                static_cast<double>(Frame) * 4.0e-4),
            SourceRest[3].Rotation);

        Foundation.Frames[Frame].TargetLocalPose = RestLocal(Target);
        Foundation.Frames[Frame].TargetLocalPose[3].Rotation = Multiply(
            FromAxisAngleDegrees(
                {0.0, 1.0, 0.0},
                static_cast<double>(Frame) * 4.0e-4),
            Foundation.Frames[Frame].TargetLocalPose[3].Rotation);
        Foundation.Frames[Frame].TargetModelPose = Model(
            Target, Foundation.Frames[Frame].TargetLocalPose);
    }

    const auto Run = RunSourceMotionFootLock(
        Source, Target, Foundation, Options());
    Check(Run.Success && Run.Coverage.RotationMotionDeltas == 1 &&
              Run.Coverage.RotationNoMotionDeltas == 7 &&
              Run.Coverage.RotationGateReleases == 1 &&
              Run.Coverage.MaximumReleasedFoundationRotationDegrees >
                  1.0e-3 &&
              Run.Coverage.MaximumReleasedFoundationRotationDegrees <
                  1.3e-3 &&
              Run.Coverage.MaximumRealEndOrientationErrorDegrees <=
                  1.0e-5,
          "subthreshold source rotation was not accumulated and released with the Foundation rotation");

    const auto& R0 = Run.Output.Frames[0].TargetModelPose[3].Rotation;
    const auto& R1 = Run.Output.Frames[1].TargetModelPose[3].Rotation;
    const auto& R2 = Run.Output.Frames[2].TargetModelPose[3].Rotation;
    const auto& R3 = Run.Output.Frames[3].TargetModelPose[3].Rotation;
    const auto& FoundationR3 =
        Foundation.Frames[3].TargetModelPose[3].Rotation;
    Check(NearlyEqual(R0, R1, 1.0e-8) &&
              NearlyEqual(R0, R2, 1.0e-8) &&
              NearlyEqual(R3, FoundationR3, 1.0e-8) &&
              !NearlyEqual(R0, R3, 1.0e-8),
          "rotation gate did not hold two subthreshold frames and release the non-zero accumulated Foundation rotation");
}

void TestNonFiniteSourcePoseRejected()
{
    const auto Source = MakeSkeleton("source-nonfinite", 10.0, 8.0);
    const auto Target = MakeSkeleton("target-nonfinite", 12.0, 9.0);
    RetargetOpClip Foundation = MakeClip(Source, Target);
    Foundation.Frames[2].SourceModelPose[7].TranslationCm.X =
        std::numeric_limits<double>::quiet_NaN();
    const auto Run = RunSourceMotionFootLock(
        Source, Target, Foundation, Options());
    Check(!Run.Success && EquivalentRetargetOpClips(
              Foundation, Run.Output),
          "non-finite source pose escaped input validation");
}

void TestUnreachableGoalFailsClosed()
{
    const auto Source = MakeSkeleton("source-unreachable", 10.0, 8.0);
    const auto Target = MakeSkeleton("target-unreachable", 12.0, 9.0);
    RetargetOpClip Foundation = MakeClip(Source, Target);
    const PoseBuffer SourceRest = Model(Source, RestLocal(Source));
    for (std::size_t Frame = 1; Frame < Foundation.Frames.size(); ++Frame)
    {
        Foundation.Frames[Frame].SourceModelPose = SourceRest;
        Foundation.Frames[Frame].TargetLocalPose[0].TranslationCm.X += 100.0;
        Foundation.Frames[Frame].TargetModelPose = Model(
            Target, Foundation.Frames[Frame].TargetLocalPose);
    }
    const auto Run = RunSourceMotionFootLock(
        Source, Target, Foundation, Options());
    Check(!Run.Success && Run.NoGroundOrContactSemanticsUsed &&
              Run.Coverage.RolledBackFrames == 3 &&
              Run.Coverage.ClampedSolves > 0 &&
              EquivalentRetargetOpClips(Foundation, Run.Output),
          "unreachable source-stationary goals did not fail closed");
    for (std::size_t Frame = 1; Frame < Run.Frames.size(); ++Frame)
        Check(Run.Frames[Frame].FailureReason ==
                  SourceMotionFootLockFailureReason::ShadowSolverFailed &&
                  Run.Frames[Frame].FailureLeftInputUnchanged,
              "clamped unreachable solve escaped its frame transaction");
}

void TestClipAtomicRollbackAfterEarlierCommit()
{
    const auto Source = MakeSkeleton("source-atomic", 10.0, 8.0);
    const auto Target = MakeSkeleton("target-atomic", 12.0, 9.0);
    RetargetOpClip Foundation = MakeClip(Source, Target);
    const PoseBuffer SourceRest = Model(Source, RestLocal(Source));
    for (RetargetOpFrame& Frame : Foundation.Frames)
        Frame.SourceModelPose = SourceRest;
    for (std::size_t Frame = 2; Frame < Foundation.Frames.size(); ++Frame)
    {
        Foundation.Frames[Frame].TargetLocalPose[0].TranslationCm.X += 100.0;
        Foundation.Frames[Frame].TargetModelPose = Model(
            Target, Foundation.Frames[Frame].TargetLocalPose);
    }
    const auto Run = RunSourceMotionFootLock(
        Source, Target, Foundation, Options());
    Check(!Run.Success && Run.Coverage.CommittedFrames >= 2 &&
              Run.Coverage.RolledBackFrames >= 1 &&
              EquivalentRetargetOpClips(Foundation, Run.Output),
          "later FootLock failure leaked earlier committed frames from the clip transaction");
}
} // namespace

int main()
{
    TestPureSourceMotionRoute();
    TestToggleAndDeterminism();
    TestScaleEligibilityFailsClosed();
    TestEligibleScaleResidueSucceeds();
    TestSubthresholdMotionAccumulates();
    TestSubthresholdRotationAccumulatesAndReleasesFoundationRotation();
    TestNonFiniteSourcePoseRejected();
    TestUnreachableGoalFailsClosed();
    TestClipAtomicRollbackAfterEarlierCommit();
    if (Failures != 0)
    {
        std::cerr << "source_motion_foot_lock_op_tests failed: "
                  << Failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "source_motion_foot_lock_op_tests passed: 9 contract groups\n";
    return EXIT_SUCCESS;
}
