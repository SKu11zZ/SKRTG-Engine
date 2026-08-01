#include "skrtg/retarget/ops/source_motion_foot_lock_op.h"

#include "skrtg/core/ik/two_bone_pose_buffer_consumer.h"
#include "skrtg/core/math/transform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace skrtg::retarget::ops
{
namespace
{
using core::animation::PoseBuffer;
using core::animation::PoseSpace;
using core::ik::ApplyAnalyticTwoBoneToPoseBuffer;
using core::ik::EndOrientationPolicy;
using core::ik::PoleFallbackPolicy;
using core::ik::QuaternionContinuityReference;
using core::ik::TwoBonePoseBufferRequest;
using core::math::Add;
using core::math::Compose;
using core::math::Conjugate;
using core::math::Dot;
using core::math::Length;
using core::math::Multiply;
using core::math::NearlyEqual;
using core::math::Normalize;
using core::math::Quat;
using core::math::RotateVector;
using core::math::Scale;
using core::math::Subtract;
using core::math::TransformRT;
using core::math::Vec3;
using core::skeleton::NormalizedRuntimeSkeleton;

constexpr double Pi = 3.141592653589793238462643383279502884;

bool Finite(double Value) { return std::isfinite(Value); }
bool Finite(Vec3 Value)
{
    return Finite(Value.X) && Finite(Value.Y) && Finite(Value.Z);
}
bool Finite(Quat Value)
{
    return Finite(Value.X) && Finite(Value.Y) &&
        Finite(Value.Z) && Finite(Value.W);
}
bool Finite(TransformRT Value)
{
    return Finite(Value.TranslationCm) && Finite(Value.Rotation) &&
        Finite(Value.Scale);
}

bool Equal(double Left, double Right)
{
    return std::memcmp(&Left, &Right, sizeof(double)) == 0;
}

bool Equal(Vec3 Left, Vec3 Right)
{
    return Equal(Left.X, Right.X) && Equal(Left.Y, Right.Y) &&
        Equal(Left.Z, Right.Z);
}
bool Equal(Quat Left, Quat Right)
{
    return Equal(Left.X, Right.X) && Equal(Left.Y, Right.Y) &&
        Equal(Left.Z, Right.Z) && Equal(Left.W, Right.W);
}
bool Equal(TransformRT Left, TransformRT Right)
{
    return Equal(Left.TranslationCm, Right.TranslationCm) &&
        Equal(Left.Rotation, Right.Rotation) &&
        Equal(Left.Scale, Right.Scale);
}
bool EqualPose(const PoseBuffer& Left, const PoseBuffer& Right)
{
    if (Left.Space() != Right.Space() ||
        Left.SkeletonHash() != Right.SkeletonHash() ||
        Left.Size() != Right.Size()) return false;
    for (std::size_t Index = 0; Index < Left.Size(); ++Index)
        if (!Equal(Left[Index], Right[Index])) return false;
    return true;
}
bool EqualClip(const RetargetOpClip& Left, const RetargetOpClip& Right)
{
    return EquivalentRetargetOpClips(Left, Right);
}

double RotationDeltaDegrees(Quat Left, Quat Right)
{
    Left = Normalize(Left);
    Right = Normalize(Right);
    const double D = std::clamp(
        std::abs(Left.X * Right.X + Left.Y * Right.Y +
                 Left.Z * Right.Z + Left.W * Right.W),
        0.0, 1.0);
    return 2.0 * std::acos(D) * 180.0 / Pi;
}

double ScaleDeviation(Vec3 Value)
{
    return std::max({std::abs(Value.X - 1.0),
                     std::abs(Value.Y - 1.0),
                     std::abs(Value.Z - 1.0)});
}

bool BuildModelPose(const NormalizedRuntimeSkeleton& Skeleton,
                    const PoseBuffer& Local,
                    PoseBuffer& Out)
{
    if (Local.Space() != PoseSpace::Local ||
        !Local.IsSizedFor(Skeleton) ||
        !Skeleton.ValidateParentIndexInvariant()) return false;
    PoseBuffer Model(PoseSpace::Model,
                     Skeleton.GetIdentity().HierarchyHash);
    Model.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        if (!Finite(Local[Index])) return false;
        const int Parent = Skeleton.BoneAt(Index).ParentIndex;
        Model[Index] = Parent < 0
            ? Local[Index]
            : Compose(Model[static_cast<std::size_t>(Parent)],
                      Local[Index]);
        if (!Finite(Model[Index])) return false;
    }
    Out = std::move(Model);
    return true;
}

bool ModelMatches(const PoseBuffer& Left, const PoseBuffer& Right,
                  double PositionToleranceCm = 1.0e-6)
{
    if (Left.Space() != PoseSpace::Model ||
        Right.Space() != PoseSpace::Model ||
        Left.SkeletonHash() != Right.SkeletonHash() ||
        Left.Size() != Right.Size()) return false;
    for (std::size_t Index = 0; Index < Left.Size(); ++Index)
    {
        if (!NearlyEqual(Left[Index], Right[Index],
                         PositionToleranceCm, 1.0e-9, 1.0e-9))
            return false;
    }
    return true;
}

NormalizedRuntimeSkeleton BuildShadowSkeleton(
    const NormalizedRuntimeSkeleton& Skeleton)
{
    NormalizedRuntimeSkeleton Shadow;
    auto Identity = Skeleton.GetIdentity();
    Identity.RestPoseHash += "|source_motion_foot_lock_shadow_v1";
    Shadow.SetIdentity(std::move(Identity));
    for (const core::skeleton::RuntimeBone& Original : Skeleton.Bones())
    {
        core::skeleton::RuntimeBone Bone = Original;
        Bone.LocalRest.Scale = {1.0, 1.0, 1.0};
        Shadow.AddBone(std::move(Bone));
    }
    Shadow.SetRawToNormalized(Skeleton.RawToNormalized());
    Shadow.SetNormalizedToRaw(Skeleton.NormalizedToRaw());
    return Shadow;
}

void ForceUnitScale(PoseBuffer& Pose)
{
    for (std::size_t Index = 0; Index < Pose.Size(); ++Index)
        Pose[Index].Scale = {1.0, 1.0, 1.0};
}

bool ValidImmediateChain(const NormalizedRuntimeSkeleton& Skeleton,
                         const std::array<int, 3>& Chain)
{
    for (int Index : Chain)
    {
        if (Index < 0 ||
            Index >= static_cast<int>(Skeleton.BoneCount())) return false;
    }
    return Chain[0] != Chain[1] && Chain[0] != Chain[2] &&
        Chain[1] != Chain[2] &&
        Skeleton.BoneAt(static_cast<std::size_t>(Chain[1])).ParentIndex ==
            Chain[0] &&
        Skeleton.BoneAt(static_cast<std::size_t>(Chain[2])).ParentIndex ==
            Chain[1];
}

bool OptionsValid(const NormalizedRuntimeSkeleton& SourceSkeleton,
                  const NormalizedRuntimeSkeleton& TargetSkeleton,
                  const SourceMotionFootLockOptions& Options)
{
    if (Options.RouteId !=
            "source_motion_foot_lock_no_ground_semantics_v1" ||
        Options.Feet[0].Label.empty() ||
        Options.Feet[1].Label.empty() ||
        Options.Feet[0].Label == Options.Feet[1].Label)
    {
        return false;
    }
    std::set<int> TargetWrites;
    for (const SourceMotionFootLockBinding& Binding : Options.Feet)
    {
        if (!ValidImmediateChain(SourceSkeleton, Binding.SourceIndices) ||
            !ValidImmediateChain(TargetSkeleton, Binding.TargetIndices))
            return false;
        for (int Index : Binding.TargetIndices)
            if (!TargetWrites.insert(Index).second) return false;
    }
    const double Values[] = {
        Options.PositionNoMotionToleranceCm,
        Options.RotationNoMotionToleranceDegrees,
        Options.ScaleNoiseEligibilityTolerance,
        Options.SolverEpsilon,
        Options.SolverPositionToleranceCm,
        Options.SolverLengthToleranceCm,
        Options.ShadowToRealPositionToleranceCm,
        Options.ShadowToRealRotationToleranceDegrees,
        Options.ReachResidualToleranceCm,
        Options.SegmentLengthToleranceCm};
    for (double Value : Values)
        if (!Finite(Value) || Value < 0.0) return false;
    return Options.SolverEpsilon == 1.0e-9 &&
        Options.ScaleNoiseEligibilityTolerance >= Options.SolverEpsilon &&
        Finite(Options.ConfiguredPoleFallbackAxisModel) &&
        Length(Options.ConfiguredPoleFallbackAxisModel) >
            Options.SolverEpsilon;
}

bool InputValid(const NormalizedRuntimeSkeleton& SourceSkeleton,
                const NormalizedRuntimeSkeleton& TargetSkeleton,
                const RetargetOpClip& Input)
{
    if (Input.Frames.empty()) return false;
    int Previous = std::numeric_limits<int>::min();
    for (const RetargetOpFrame& Frame : Input.Frames)
    {
        if (Frame.FrameIndex <= Previous || !Finite(Frame.TimeSeconds) ||
            Frame.SourceModelPose.Space() != PoseSpace::Model ||
            Frame.TargetLocalPose.Space() != PoseSpace::Local ||
            Frame.TargetModelPose.Space() != PoseSpace::Model ||
            !Frame.SourceModelPose.IsSizedFor(SourceSkeleton) ||
            !Frame.TargetLocalPose.IsSizedFor(TargetSkeleton) ||
            !Frame.TargetModelPose.IsSizedFor(TargetSkeleton)) return false;
        for (std::size_t Index = 0;
             Index < Frame.SourceModelPose.Size(); ++Index)
        {
            if (!Finite(Frame.SourceModelPose[Index])) return false;
        }
        PoseBuffer Rebuilt;
        if (!BuildModelPose(TargetSkeleton, Frame.TargetLocalPose,
                            Rebuilt) ||
            !ModelMatches(Rebuilt, Frame.TargetModelPose)) return false;
        Previous = Frame.FrameIndex;
    }
    return true;
}

bool NormalizeVector(Vec3 Value, double Epsilon, Vec3& Out)
{
    const double Magnitude = Length(Value);
    if (!Finite(Magnitude) || Magnitude <= Epsilon) return false;
    Out = Scale(Value, 1.0 / Magnitude);
    return Finite(Out);
}

bool BuildPole(const SourceMotionFootLockBinding& Binding,
               const RetargetOpFrame& Frame,
               const PoseBuffer& TargetShadowModel,
               const SourceMotionFootLockOptions& Options,
               Vec3& OutPole,
               bool& OutConfiguredFallbackUsed)
{
    OutConfiguredFallbackUsed = false;
    const Vec3 TargetRoot = TargetShadowModel[
        static_cast<std::size_t>(Binding.TargetIndices[0])].TranslationCm;
    const Vec3 TargetMid = TargetShadowModel[
        static_cast<std::size_t>(Binding.TargetIndices[1])].TranslationCm;
    const Vec3 TargetEnd = TargetShadowModel[
        static_cast<std::size_t>(Binding.TargetIndices[2])].TranslationCm;
    const Vec3 SourceRoot = Frame.SourceModelPose[
        static_cast<std::size_t>(Binding.SourceIndices[0])].TranslationCm;
    const Vec3 SourceMid = Frame.SourceModelPose[
        static_cast<std::size_t>(Binding.SourceIndices[1])].TranslationCm;
    const Vec3 SourceEnd = Frame.SourceModelPose[
        static_cast<std::size_t>(Binding.SourceIndices[2])].TranslationCm;

    auto BendDirection = [&](Vec3 Root, Vec3 Mid, Vec3 End,
                             Vec3& Direction)
    {
        Vec3 Axis;
        if (!NormalizeVector(Subtract(End, Root), Options.SolverEpsilon,
                             Axis)) return false;
        const Vec3 RootToMid = Subtract(Mid, Root);
        const Vec3 Perpendicular = Subtract(
            RootToMid, Scale(Axis, Dot(RootToMid, Axis)));
        return NormalizeVector(Perpendicular, Options.SolverEpsilon,
                               Direction);
    };

    Vec3 Direction;
    if (!BendDirection(SourceRoot, SourceMid, SourceEnd, Direction) &&
        !BendDirection(TargetRoot, TargetMid, TargetEnd, Direction))
    {
        if (!Options.AllowConfiguredPoleFallback ||
            !NormalizeVector(Options.ConfiguredPoleFallbackAxisModel,
                             Options.SolverEpsilon, Direction))
            return false;
        OutConfiguredFallbackUsed = true;
    }
    const double Reach = Length(Subtract(TargetMid, TargetRoot)) +
        Length(Subtract(TargetEnd, TargetMid));
    OutPole = Add(TargetRoot, Scale(Direction, std::max(Reach, 1.0)));
    return Finite(OutPole);
}

bool MutationBounded(const PoseBuffer& Before,
                     const PoseBuffer& After,
                     const std::set<int>& Declared)
{
    if (Before.Size() != After.Size()) return false;
    for (std::size_t Index = 0; Index < Before.Size(); ++Index)
    {
        const TransformRT& A = Before[Index];
        const TransformRT& B = After[Index];
        if (!Equal(A.TranslationCm, B.TranslationCm) ||
            !Equal(A.Scale, B.Scale) ||
            (Declared.count(static_cast<int>(Index)) == 0 &&
             !Equal(A.Rotation, B.Rotation))) return false;
    }
    return true;
}

void Accumulate(SourceMotionFootLockCoverage& Coverage,
                const SourceMotionFootLockChainFrameRecord& Record)
{
    Coverage.MaximumSourcePositionDeltaCm = std::max(
        Coverage.MaximumSourcePositionDeltaCm,
        Record.SourcePositionDeltaCm);
    Coverage.MaximumSourceRotationDeltaDegrees = std::max(
        Coverage.MaximumSourceRotationDeltaDegrees,
        Record.SourceRotationDeltaDegrees);
    Coverage.MaximumReleasedFoundationRotationDegrees = std::max(
        Coverage.MaximumReleasedFoundationRotationDegrees,
        Record.ReleasedFoundationRotationDegrees);
    Coverage.MaximumNoMotionTargetDriftCm = std::max(
        Coverage.MaximumNoMotionTargetDriftCm,
        Record.NoMotionTargetDriftCm);
    Coverage.MaximumTargetDeltaErrorCm = std::max(
        Coverage.MaximumTargetDeltaErrorCm,
        Record.TargetDeltaErrorCm);
    Coverage.MaximumRealEndpointGoalErrorCm = std::max(
        Coverage.MaximumRealEndpointGoalErrorCm,
        Record.RealEndpointGoalErrorCm);
    Coverage.MaximumReachResidualDeltaCm = std::max(
        Coverage.MaximumReachResidualDeltaCm,
        Record.ReachResidualDeltaCm);
    Coverage.MaximumRealEndOrientationErrorDegrees = std::max(
        Coverage.MaximumRealEndOrientationErrorDegrees,
        Record.RealEndOrientationErrorDegrees);
    Coverage.MaximumShadowToRealPositionDeltaCm = std::max(
        Coverage.MaximumShadowToRealPositionDeltaCm,
        Record.MaximumShadowToRealPositionDeltaCm);
    Coverage.MaximumSegmentLengthErrorCm = std::max(
        Coverage.MaximumSegmentLengthErrorCm,
        Record.MaximumSegmentLengthErrorCm);
    if (Record.PositionDeltaSuppressedAsNoMotion)
        ++Coverage.PositionNoMotionDeltas;
    else
        ++Coverage.PositionMotionDeltas;
    if (Record.RotationDeltaSuppressedAsNoMotion)
        ++Coverage.RotationNoMotionDeltas;
    else
    {
        ++Coverage.RotationMotionDeltas;
        ++Coverage.RotationGateReleases;
    }
    if (Record.SolverExecuted) ++Coverage.SolverExecutions;
    if (Record.SolverCommitted) ++Coverage.SolverCommits;
    if (Record.SolverClamped) ++Coverage.ClampedSolves;
    if (Record.PoleFallbackUsed) ++Coverage.PoleFallbacks;
}

bool EquivalentRecord(const SourceMotionFootLockChainFrameRecord& A,
                      const SourceMotionFootLockChainFrameRecord& B)
{
    return A.Label == B.Label &&
        A.PositionDeltaSuppressedAsNoMotion ==
            B.PositionDeltaSuppressedAsNoMotion &&
        A.RotationDeltaSuppressedAsNoMotion ==
            B.RotationDeltaSuppressedAsNoMotion &&
        A.SolverExecuted == B.SolverExecuted &&
        A.SolverCommitted == B.SolverCommitted &&
        A.SolverClamped == B.SolverClamped &&
        A.PoleFallbackUsed == B.PoleFallbackUsed &&
        A.RootPelvisCompensationApplied ==
            B.RootPelvisCompensationApplied &&
        Equal(A.SourcePositionDeltaModelCm,
              B.SourcePositionDeltaModelCm) &&
        A.SourcePositionDeltaCm == B.SourcePositionDeltaCm &&
        A.SourceRotationDeltaDegrees == B.SourceRotationDeltaDegrees &&
        A.ReleasedFoundationRotationDegrees ==
            B.ReleasedFoundationRotationDegrees &&
        Equal(A.DesiredTargetPositionModelCm,
              B.DesiredTargetPositionModelCm) &&
        Equal(A.DesiredTargetOrientationModel,
              B.DesiredTargetOrientationModel) &&
        Equal(A.PolePositionModelCm, B.PolePositionModelCm) &&
        A.TargetPositionDeltaCm == B.TargetPositionDeltaCm &&
        A.TargetDeltaErrorCm == B.TargetDeltaErrorCm &&
        A.NoMotionTargetDriftCm == B.NoMotionTargetDriftCm &&
        A.RealEndpointGoalErrorCm == B.RealEndpointGoalErrorCm &&
        A.ShadowEndpointGoalErrorCm == B.ShadowEndpointGoalErrorCm &&
        A.ReachResidualDeltaCm == B.ReachResidualDeltaCm &&
        A.RealEndOrientationErrorDegrees ==
            B.RealEndOrientationErrorDegrees &&
        A.MaximumShadowToRealPositionDeltaCm ==
            B.MaximumShadowToRealPositionDeltaCm &&
        A.MaximumSegmentLengthErrorCm ==
            B.MaximumSegmentLengthErrorCm;
}
} // namespace

SourceMotionFootLockRunResult RunSourceMotionFootLock(
    const NormalizedRuntimeSkeleton& SourceSkeleton,
    const NormalizedRuntimeSkeleton& TargetSkeleton,
    const RetargetOpClip& Input,
    const SourceMotionFootLockOptions& Options)
{
    SourceMotionFootLockRunResult Result;
    Result.RouteId = Options.RouteId;
    Result.Output = Input;
    const RetargetOpClip InputSnapshot = Input;
    Result.NoGroundOrContactSemanticsUsed = true;
    Result.Coverage.InputFrames = static_cast<int>(Input.Frames.size());

    if (SourceSkeleton.Empty() || TargetSkeleton.Empty() ||
        !SourceSkeleton.ValidateParentIndexInvariant() ||
        !TargetSkeleton.ValidateParentIndexInvariant())
    {
        Result.Errors.push_back("source or target skeleton is invalid");
        return Result;
    }
    if (!OptionsValid(SourceSkeleton, TargetSkeleton, Options))
    {
        Result.Errors.push_back(
            "source-motion FootLock options or exact leg bindings are invalid");
        return Result;
    }
    if (!InputValid(SourceSkeleton, TargetSkeleton, Input))
    {
        Result.Errors.push_back(
            "source-motion FootLock input clip is invalid");
        return Result;
    }

    const NormalizedRuntimeSkeleton ShadowTarget =
        BuildShadowSkeleton(TargetSkeleton);
    std::set<int> DeclaredIndices;
    for (const SourceMotionFootLockBinding& Binding : Options.Feet)
        DeclaredIndices.insert(Binding.TargetIndices.begin(),
                               Binding.TargetIndices.end());

    std::array<std::vector<Vec3>, 2> DesiredPositions;
    std::array<std::vector<Quat>, 2> DesiredOrientations;
    std::array<std::vector<Vec3>, 2> SourceDeltas;
    std::array<std::vector<Vec3>, 2> AppliedPositionDeltas;
    std::array<std::vector<double>, 2> AppliedRotationDeltas;
    std::array<std::vector<double>, 2> SourceRotationDeltas;
    std::array<std::vector<bool>, 2> PositionSuppressed;
    std::array<std::vector<bool>, 2> RotationSuppressed;
    for (std::size_t Foot = 0; Foot < Options.Feet.size(); ++Foot)
    {
        const auto& Binding = Options.Feet[Foot];
        const std::size_t Count = Input.Frames.size();
        DesiredPositions[Foot].resize(Count);
        DesiredOrientations[Foot].resize(Count);
        SourceDeltas[Foot].resize(Count);
        AppliedPositionDeltas[Foot].resize(Count);
        AppliedRotationDeltas[Foot].resize(Count);
        SourceRotationDeltas[Foot].resize(Count);
        PositionSuppressed[Foot].resize(Count, true);
        RotationSuppressed[Foot].resize(Count, true);
        DesiredPositions[Foot][0] = Input.Frames[0].TargetModelPose[
            static_cast<std::size_t>(Binding.TargetIndices[2])]
                .TranslationCm;
        DesiredOrientations[Foot][0] = Input.Frames[0].TargetModelPose[
            static_cast<std::size_t>(Binding.TargetIndices[2])]
                .Rotation;
        Vec3 PendingPositionDelta{};
        Quat PendingRotationDelta{0.0, 0.0, 0.0, 1.0};
        Vec3 PendingFoundationPositionDelta{};
        Quat PendingFoundationRotationDelta{0.0, 0.0, 0.0, 1.0};
        for (std::size_t Frame = 1; Frame < Count; ++Frame)
        {
            const TransformRT& PreviousSource =
                Input.Frames[Frame - 1].SourceModelPose[
                    static_cast<std::size_t>(Binding.SourceIndices[2])];
            const TransformRT& CurrentSource =
                Input.Frames[Frame].SourceModelPose[
                    static_cast<std::size_t>(Binding.SourceIndices[2])];
            const TransformRT& PreviousFoundation =
                Input.Frames[Frame - 1].TargetModelPose[
                    static_cast<std::size_t>(Binding.TargetIndices[2])];
            const TransformRT& CurrentFoundation =
                Input.Frames[Frame].TargetModelPose[
                    static_cast<std::size_t>(Binding.TargetIndices[2])];
            const Vec3 Delta = Subtract(CurrentSource.TranslationCm,
                                        PreviousSource.TranslationCm);
            const Vec3 FoundationDelta = Subtract(
                CurrentFoundation.TranslationCm,
                PreviousFoundation.TranslationCm);
            const double Distance = Length(Delta);
            const Quat DeltaRotation = Normalize(Multiply(
                CurrentSource.Rotation,
                Conjugate(PreviousSource.Rotation)));
            const Quat FoundationDeltaRotation = Normalize(Multiply(
                CurrentFoundation.Rotation,
                Conjugate(PreviousFoundation.Rotation)));
            const double Angle = RotationDeltaDegrees(
                PreviousSource.Rotation, CurrentSource.Rotation);
            if (!Finite(Delta) || !Finite(FoundationDelta) ||
                !Finite(Distance) || !Finite(DeltaRotation) ||
                !Finite(FoundationDeltaRotation) || !Finite(Angle))
            {
                Result.Errors.push_back(
                    Binding.Label +
                    ": non-finite source foot motion delta");
                Result.InputImmutable = EqualClip(Input, InputSnapshot);
                return Result;
            }
            SourceDeltas[Foot][Frame] = Delta;
            SourceRotationDeltas[Foot][Frame] = Angle;
            PendingPositionDelta = Add(PendingPositionDelta, Delta);
            PendingRotationDelta = Normalize(Multiply(
                DeltaRotation, PendingRotationDelta));
            PendingFoundationPositionDelta = Add(
                PendingFoundationPositionDelta, FoundationDelta);
            PendingFoundationRotationDelta = Normalize(Multiply(
                FoundationDeltaRotation,
                PendingFoundationRotationDelta));
            const double PendingPositionDistance =
                Length(PendingPositionDelta);
            const double PendingRotationAngle = RotationDeltaDegrees(
                {0.0, 0.0, 0.0, 1.0}, PendingRotationDelta);
            PositionSuppressed[Foot][Frame] =
                PendingPositionDistance <=
                    Options.PositionNoMotionToleranceCm;
            RotationSuppressed[Foot][Frame] =
                PendingRotationAngle <=
                    Options.RotationNoMotionToleranceDegrees;
            DesiredPositions[Foot][Frame] =
                DesiredPositions[Foot][Frame - 1];
            if (!PositionSuppressed[Foot][Frame])
            {
                AppliedPositionDeltas[Foot][Frame] =
                    PendingFoundationPositionDelta;
                DesiredPositions[Foot][Frame] = Add(
                    DesiredPositions[Foot][Frame - 1],
                    PendingFoundationPositionDelta);
                PendingPositionDelta = {};
                PendingFoundationPositionDelta = {};
            }
            DesiredOrientations[Foot][Frame] =
                DesiredOrientations[Foot][Frame - 1];
            if (!RotationSuppressed[Foot][Frame])
            {
                AppliedRotationDeltas[Foot][Frame] =
                    RotationDeltaDegrees(
                        {0.0, 0.0, 0.0, 1.0},
                        PendingFoundationRotationDelta);
                DesiredOrientations[Foot][Frame] = Normalize(Multiply(
                    PendingFoundationRotationDelta,
                    DesiredOrientations[Foot][Frame - 1]));
                PendingRotationDelta = {0.0, 0.0, 0.0, 1.0};
                PendingFoundationRotationDelta =
                    {0.0, 0.0, 0.0, 1.0};
            }
        }
    }

    std::array<QuaternionContinuityReference, 2> Continuity;
    std::array<Vec3, 2> PreviousOutputPositions;
    for (std::size_t Foot = 0; Foot < Options.Feet.size(); ++Foot)
    {
        const auto& Chain = Options.Feet[Foot].TargetIndices;
        Continuity[Foot].HasRootLocal = true;
        Continuity[Foot].HasMidLocal = true;
        Continuity[Foot].HasEndLocal = true;
        Continuity[Foot].RootLocal = Input.Frames[0].TargetLocalPose[
            static_cast<std::size_t>(Chain[0])].Rotation;
        Continuity[Foot].MidLocal = Input.Frames[0].TargetLocalPose[
            static_cast<std::size_t>(Chain[1])].Rotation;
        Continuity[Foot].EndLocal = Input.Frames[0].TargetLocalPose[
            static_cast<std::size_t>(Chain[2])].Rotation;
        PreviousOutputPositions[Foot] = DesiredPositions[Foot][0];
    }

    Result.Frames.reserve(Input.Frames.size());
    bool AllBounded = true;
    bool SeedExact = true;
    for (std::size_t FrameOffset = 0;
         FrameOffset < Input.Frames.size(); ++FrameOffset)
    {
        const RetargetOpFrame& InputFrame = Input.Frames[FrameOffset];
        SourceMotionFootLockFrameRecord Record;
        Record.FrameIndex = InputFrame.FrameIndex;
        Record.TimeSeconds = InputFrame.TimeSeconds;
        for (std::size_t Foot = 0; Foot < Options.Feet.size(); ++Foot)
        {
            auto& ChainRecord = Record.Feet[Foot];
            ChainRecord.Label = Options.Feet[Foot].Label;
            ChainRecord.SourcePositionDeltaModelCm =
                SourceDeltas[Foot][FrameOffset];
            ChainRecord.SourcePositionDeltaCm = Length(
                SourceDeltas[Foot][FrameOffset]);
            ChainRecord.SourceRotationDeltaDegrees =
                SourceRotationDeltas[Foot][FrameOffset];
            ChainRecord.ReleasedFoundationRotationDegrees =
                AppliedRotationDeltas[Foot][FrameOffset];
            ChainRecord.PositionDeltaSuppressedAsNoMotion =
                PositionSuppressed[Foot][FrameOffset];
            ChainRecord.RotationDeltaSuppressedAsNoMotion =
                RotationSuppressed[Foot][FrameOffset];
            ChainRecord.DesiredTargetPositionModelCm =
                DesiredPositions[Foot][FrameOffset];
            ChainRecord.DesiredTargetOrientationModel =
                DesiredOrientations[Foot][FrameOffset];
        }

        if (FrameOffset == 0)
        {
            Record.SeedFrameExactPassthrough = true;
            Record.TransactionCommitted = true;
            Record.FailureLeftInputUnchanged = true;
            Record.OnlyDeclaredLocalRotationsWritten = true;
            Record.AllLocalTranslationsBitwisePreserved = true;
            Record.AllLocalScalesBitwisePreserved = true;
            Record.TargetHipsBitwisePreserved = true;
            ++Result.Coverage.SeedPassthroughFrames;
            ++Result.Coverage.CommittedFrames;
            for (const auto& ChainRecord : Record.Feet)
                Accumulate(Result.Coverage, ChainRecord);
            SeedExact = SeedExact &&
                EqualPose(Result.Output.Frames[0].TargetLocalPose,
                          InputFrame.TargetLocalPose) &&
                EqualPose(Result.Output.Frames[0].TargetModelPose,
                          InputFrame.TargetModelPose);
            Result.Frames.push_back(std::move(Record));
            continue;
        }

        double MaximumScale = 0.0;
        for (std::size_t Index = 0;
             Index < InputFrame.TargetLocalPose.Size(); ++Index)
        {
            MaximumScale = std::max(
                MaximumScale,
                ScaleDeviation(InputFrame.TargetLocalPose[Index].Scale));
            MaximumScale = std::max(
                MaximumScale,
                ScaleDeviation(InputFrame.TargetModelPose[Index].Scale));
        }
        Result.Coverage.MaximumEligibleScaleDeviation = std::max(
            Result.Coverage.MaximumEligibleScaleDeviation,
            MaximumScale);
        if (MaximumScale > Options.ScaleNoiseEligibilityTolerance)
        {
            Record.FailureReason = SourceMotionFootLockFailureReason::
                ScaleOutsideShadowEligibility;
            Record.Message =
                "target pose scale exceeds FootLock shadow eligibility";
            Record.FailureLeftInputUnchanged = true;
            ++Result.Coverage.RolledBackFrames;
            AllBounded = false;
            Result.Frames.push_back(std::move(Record));
            continue;
        }

        PoseBuffer ShadowLocal = InputFrame.TargetLocalPose;
        ForceUnitScale(ShadowLocal);
        PoseBuffer ShadowModel;
        if (!BuildModelPose(ShadowTarget, ShadowLocal, ShadowModel))
        {
            Record.FailureReason =
                SourceMotionFootLockFailureReason::InvalidInput;
            Record.Message = "unit-scale shadow model rebuild failed";
            Record.FailureLeftInputUnchanged = true;
            ++Result.Coverage.RolledBackFrames;
            AllBounded = false;
            Result.Frames.push_back(std::move(Record));
            continue;
        }

        bool ShadowSolved = true;
        auto PendingContinuity = Continuity;
        for (std::size_t Foot = 0; Foot < Options.Feet.size(); ++Foot)
        {
            const SourceMotionFootLockBinding& Binding =
                Options.Feet[Foot];
            auto& ChainRecord = Record.Feet[Foot];
            const TransformRT& CurrentFoundationEnd = ShadowModel[
                static_cast<std::size_t>(Binding.TargetIndices[2])];
            const double FoundationGoalPositionDelta = Length(Subtract(
                CurrentFoundationEnd.TranslationCm,
                DesiredPositions[Foot][FrameOffset]));
            const double FoundationGoalRotationDelta =
                RotationDeltaDegrees(
                    CurrentFoundationEnd.Rotation,
                    DesiredOrientations[Foot][FrameOffset]);
            if (FoundationGoalPositionDelta <=
                    Options.ReachResidualToleranceCm &&
                FoundationGoalRotationDelta <=
                    Options.ShadowToRealRotationToleranceDegrees)
            {
                ChainRecord.ShadowEndpointGoalErrorCm =
                    FoundationGoalPositionDelta;
                PendingContinuity[Foot].RootLocal = ShadowLocal[
                    static_cast<std::size_t>(Binding.TargetIndices[0])]
                        .Rotation;
                PendingContinuity[Foot].MidLocal = ShadowLocal[
                    static_cast<std::size_t>(Binding.TargetIndices[1])]
                        .Rotation;
                PendingContinuity[Foot].EndLocal = ShadowLocal[
                    static_cast<std::size_t>(Binding.TargetIndices[2])]
                        .Rotation;
                continue;
            }
            Vec3 Pole;
            bool ConfiguredPoleFallbackUsed = false;
            if (!BuildPole(Binding, InputFrame, ShadowModel,
                           Options, Pole,
                           ConfiguredPoleFallbackUsed))
            {
                Record.FailureReason =
                    SourceMotionFootLockFailureReason::
                        PoleConstructionFailed;
                Record.Message = Binding.Label +
                    ": source/foundation knee plane is degenerate";
                ShadowSolved = false;
                break;
            }
            ChainRecord.PolePositionModelCm = Pole;
            TwoBonePoseBufferRequest Request;
            Request.Chain.RootIndex = Binding.TargetIndices[0];
            Request.Chain.MidIndex = Binding.TargetIndices[1];
            Request.Chain.EndIndex = Binding.TargetIndices[2];
            Request.Chain.MidParentIndex = Binding.TargetIndices[0];
            Request.Chain.EndParentIndex = Binding.TargetIndices[1];
            Request.TargetPositionModelCm =
                DesiredPositions[Foot][FrameOffset];
            Request.PolePositionModelCm = Pole;
            Request.PoleFallback = Options.AllowConfiguredPoleFallback
                ? PoleFallbackPolicy::AllowConfiguredAxis
                : PoleFallbackPolicy::FailClosed;
            Request.ConfiguredFallbackAxisModel =
                Options.ConfiguredPoleFallbackAxisModel;
            Request.OrientationPolicy =
                EndOrientationPolicy::ApplyExplicitModel;
            Request.ExplicitEndModelOrientation =
                DesiredOrientations[Foot][FrameOffset];
            Request.PreviousLocalRotations = PendingContinuity[Foot];
            Request.Epsilon = Options.SolverEpsilon;
            Request.PositionToleranceCm =
                Options.SolverPositionToleranceCm;
            Request.LengthToleranceCm =
                Options.SolverLengthToleranceCm;
            ChainRecord.SolverExecuted = true;
            const auto Consumer = ApplyAnalyticTwoBoneToPoseBuffer(
                ShadowTarget, Request, ShadowLocal);
            if (!Consumer.Success ||
                !Consumer.Telemetry.PoseWriteCommitted ||
                !Consumer.Telemetry.ModelPoseValid ||
                !Consumer.Telemetry.ParentLocalModelConsistent ||
                !Consumer.Telemetry.LocalTranslationsPreserved ||
                !Consumer.Telemetry.LocalScalesPreserved ||
                !Consumer.Telemetry.UnrelatedLocalTransformsPreserved ||
                !Consumer.Telemetry.OnlyChainLocalRotationsWritten ||
                Consumer.Telemetry.RootPelvisGlobalCompensationApplied)
            {
                Record.FailureReason =
                    SourceMotionFootLockFailureReason::ShadowSolverFailed;
                Record.Message = Binding.Label +
                    ": strict shadow Two-Bone IK failed";
                ShadowSolved = false;
                break;
            }
            ShadowModel = Consumer.OutputModelPose;
            ChainRecord.SolverCommitted = true;
            ChainRecord.SolverClamped =
                Consumer.Status ==
                    core::ik::TwoBonePoseBufferStatus::AppliedClamped;
            ChainRecord.PoleFallbackUsed =
                ConfiguredPoleFallbackUsed ||
                Consumer.SolverResult.Telemetry.PoleFallbackUsed;
            ChainRecord.RootPelvisCompensationApplied =
                Consumer.Telemetry.RootPelvisGlobalCompensationApplied;
            ChainRecord.ShadowEndpointGoalErrorCm =
                Consumer.Telemetry.EndpointErrorCm;
            if (ChainRecord.SolverClamped)
            {
                Record.FailureReason =
                    SourceMotionFootLockFailureReason::ShadowSolverFailed;
                Record.Message = Binding.Label +
                    ": clamped Two-Bone IK solve rejected";
                ShadowSolved = false;
                break;
            }
            PendingContinuity[Foot].RootLocal = ShadowLocal[
                static_cast<std::size_t>(Binding.TargetIndices[0])]
                    .Rotation;
            PendingContinuity[Foot].MidLocal = ShadowLocal[
                static_cast<std::size_t>(Binding.TargetIndices[1])]
                    .Rotation;
            PendingContinuity[Foot].EndLocal = ShadowLocal[
                static_cast<std::size_t>(Binding.TargetIndices[2])]
                    .Rotation;
        }
        if (!ShadowSolved)
        {
            Record.FailureLeftInputUnchanged = true;
            ++Result.Coverage.RolledBackFrames;
            AllBounded = false;
            for (const auto& ChainRecord : Record.Feet)
                Accumulate(Result.Coverage, ChainRecord);
            Result.Frames.push_back(std::move(Record));
            continue;
        }

        PoseBuffer RealLocal = InputFrame.TargetLocalPose;
        for (const SourceMotionFootLockBinding& Binding : Options.Feet)
        {
            for (int Index : Binding.TargetIndices)
                RealLocal[static_cast<std::size_t>(Index)].Rotation =
                    ShadowLocal[static_cast<std::size_t>(Index)].Rotation;
        }
        PoseBuffer RealModel;
        if (!BuildModelPose(TargetSkeleton, RealLocal, RealModel))
        {
            Record.FailureReason =
                SourceMotionFootLockFailureReason::RealProjectionFailed;
            Record.Message =
                "FootLock real-pose model rebuild failed";
            Record.FailureLeftInputUnchanged = true;
            ++Result.Coverage.RolledBackFrames;
            AllBounded = false;
            for (const auto& ChainRecord : Record.Feet)
                Accumulate(Result.Coverage, ChainRecord);
            Result.Frames.push_back(std::move(Record));
            continue;
        }
        bool Postvalid = MutationBounded(
            InputFrame.TargetLocalPose, RealLocal, DeclaredIndices);
        for (std::size_t Foot = 0;
             Foot < Options.Feet.size(); ++Foot)
        {
            const auto& Binding = Options.Feet[Foot];
            auto& ChainRecord = Record.Feet[Foot];
            double MaximumShadowDelta = 0.0;
            double MaximumSegmentError = 0.0;
            for (int Index : Binding.TargetIndices)
            {
                MaximumShadowDelta = std::max(
                    MaximumShadowDelta,
                    Length(Subtract(
                        RealModel[static_cast<std::size_t>(Index)]
                            .TranslationCm,
                        ShadowModel[static_cast<std::size_t>(Index)]
                            .TranslationCm)));
            }
            const Vec3 RealRoot = RealModel[
                static_cast<std::size_t>(Binding.TargetIndices[0])]
                    .TranslationCm;
            const Vec3 RealMid = RealModel[
                static_cast<std::size_t>(Binding.TargetIndices[1])]
                    .TranslationCm;
            const Vec3 RealEnd = RealModel[
                static_cast<std::size_t>(Binding.TargetIndices[2])]
                    .TranslationCm;
            const Vec3 InputRoot = InputFrame.TargetModelPose[
                static_cast<std::size_t>(Binding.TargetIndices[0])]
                    .TranslationCm;
            const Vec3 InputMid = InputFrame.TargetModelPose[
                static_cast<std::size_t>(Binding.TargetIndices[1])]
                    .TranslationCm;
            const Vec3 InputEnd = InputFrame.TargetModelPose[
                static_cast<std::size_t>(Binding.TargetIndices[2])]
                    .TranslationCm;
            const double FirstRest = Length(Subtract(
                InputMid, InputRoot));
            const double SecondRest = Length(Subtract(
                InputEnd, InputMid));
            MaximumSegmentError = std::max(
                std::abs(Length(Subtract(RealMid, RealRoot)) - FirstRest),
                std::abs(Length(Subtract(RealEnd, RealMid)) - SecondRest));
            ChainRecord.RealEndpointGoalErrorCm = Length(Subtract(
                RealEnd, DesiredPositions[Foot][FrameOffset]));
            ChainRecord.ReachResidualDeltaCm = std::abs(
                ChainRecord.RealEndpointGoalErrorCm -
                ChainRecord.ShadowEndpointGoalErrorCm);
            ChainRecord.RealEndOrientationErrorDegrees =
                RotationDeltaDegrees(
                    RealModel[static_cast<std::size_t>(
                        Binding.TargetIndices[2])].Rotation,
                    DesiredOrientations[Foot][FrameOffset]);
            ChainRecord.MaximumShadowToRealPositionDeltaCm =
                MaximumShadowDelta;
            ChainRecord.MaximumSegmentLengthErrorCm =
                MaximumSegmentError;
            const Vec3 ActualDelta = Subtract(
                RealEnd, PreviousOutputPositions[Foot]);
            const Vec3 ExpectedDelta =
                PositionSuppressed[Foot][FrameOffset]
                ? Vec3{}
                : AppliedPositionDeltas[Foot][FrameOffset];
            ChainRecord.TargetPositionDeltaCm = Length(ActualDelta);
            ChainRecord.TargetDeltaErrorCm = Length(Subtract(
                ActualDelta, ExpectedDelta));
            ChainRecord.NoMotionTargetDriftCm =
                PositionSuppressed[Foot][FrameOffset]
                ? ChainRecord.TargetPositionDeltaCm : 0.0;
            Postvalid = Postvalid &&
                Finite(ChainRecord.RealEndpointGoalErrorCm) &&
                Finite(ChainRecord.ReachResidualDeltaCm) &&
                Finite(ChainRecord.RealEndOrientationErrorDegrees) &&
                ChainRecord.RealEndpointGoalErrorCm <=
                    Options.ReachResidualToleranceCm &&
                ChainRecord.ShadowEndpointGoalErrorCm <=
                    Options.ReachResidualToleranceCm &&
                ChainRecord.TargetDeltaErrorCm <=
                    Options.ReachResidualToleranceCm &&
                ChainRecord.NoMotionTargetDriftCm <=
                    Options.ReachResidualToleranceCm &&
                MaximumShadowDelta <=
                    Options.ShadowToRealPositionToleranceCm &&
                ChainRecord.ReachResidualDeltaCm <=
                    Options.ReachResidualToleranceCm &&
                ChainRecord.RealEndOrientationErrorDegrees <=
                    Options.ShadowToRealRotationToleranceDegrees &&
                MaximumSegmentError <=
                    Options.SegmentLengthToleranceCm &&
                !ChainRecord.SolverClamped &&
                !ChainRecord.RootPelvisCompensationApplied;
        }

        Record.OnlyDeclaredLocalRotationsWritten =
            MutationBounded(InputFrame.TargetLocalPose,
                            RealLocal, DeclaredIndices);
        Record.AllLocalTranslationsBitwisePreserved = true;
        Record.AllLocalScalesBitwisePreserved = true;
        for (std::size_t Index = 0; Index < RealLocal.Size(); ++Index)
        {
            Record.AllLocalTranslationsBitwisePreserved =
                Record.AllLocalTranslationsBitwisePreserved &&
                Equal(InputFrame.TargetLocalPose[Index].TranslationCm,
                      RealLocal[Index].TranslationCm);
            Record.AllLocalScalesBitwisePreserved =
                Record.AllLocalScalesBitwisePreserved &&
                Equal(InputFrame.TargetLocalPose[Index].Scale,
                      RealLocal[Index].Scale);
        }
        Record.TargetHipsBitwisePreserved =
            Equal(InputFrame.TargetLocalPose[0], RealLocal[0]);
        Postvalid = Postvalid &&
            Record.OnlyDeclaredLocalRotationsWritten &&
            Record.AllLocalTranslationsBitwisePreserved &&
            Record.AllLocalScalesBitwisePreserved &&
            Record.TargetHipsBitwisePreserved;
        if (!Postvalid)
        {
            Record.FailureReason =
                SourceMotionFootLockFailureReason::PostvalidationFailed;
            Record.Message =
                "source-motion FootLock real-pose postvalidation failed";
            Record.FailureLeftInputUnchanged = true;
            ++Result.Coverage.RolledBackFrames;
            AllBounded = false;
        }
        else
        {
            Record.TransactionCommitted = true;
            Continuity = PendingContinuity;
            Result.Output.Frames[FrameOffset].TargetLocalPose =
                std::move(RealLocal);
            Result.Output.Frames[FrameOffset].TargetModelPose =
                std::move(RealModel);
            for (std::size_t Foot = 0;
                 Foot < Options.Feet.size(); ++Foot)
            {
                PreviousOutputPositions[Foot] =
                    Result.Output.Frames[FrameOffset].TargetModelPose[
                        static_cast<std::size_t>(
                            Options.Feet[Foot].TargetIndices[2])]
                        .TranslationCm;
            }
            ++Result.Coverage.CommittedFrames;
        }
        for (const auto& ChainRecord : Record.Feet)
            Accumulate(Result.Coverage, ChainRecord);
        Result.Frames.push_back(std::move(Record));
    }

    Result.InputImmutable = EqualClip(Input, InputSnapshot);
    Result.AllTransactionsBounded = AllBounded &&
        Result.Coverage.RolledBackFrames == 0 &&
        Result.Coverage.CommittedFrames ==
            static_cast<int>(Input.Frames.size());
    if (!Result.AllTransactionsBounded)
        Result.Output = Input;
    Result.FoundationSeedFramesExact = SeedExact;
    Result.Success = Result.InputImmutable &&
        Result.NoGroundOrContactSemanticsUsed &&
        Result.AllTransactionsBounded &&
        Result.FoundationSeedFramesExact &&
        Result.Output.Frames.size() == Input.Frames.size() &&
        Result.Frames.size() == Input.Frames.size();
    if (!Result.Success && Result.Errors.empty())
        Result.Errors.push_back(
            "source-motion FootLock did not satisfy every bounded transaction");
    return Result;
}

bool EquivalentSourceMotionFootLockRuns(
    const SourceMotionFootLockRunResult& Left,
    const SourceMotionFootLockRunResult& Right)
{
    if (Left.Success != Right.Success ||
        Left.InputImmutable != Right.InputImmutable ||
        Left.NoGroundOrContactSemanticsUsed !=
            Right.NoGroundOrContactSemanticsUsed ||
        Left.AllTransactionsBounded != Right.AllTransactionsBounded ||
        Left.FoundationSeedFramesExact != Right.FoundationSeedFramesExact ||
        Left.RouteId != Right.RouteId ||
        !EquivalentRetargetOpClips(Left.Output, Right.Output) ||
        Left.Errors != Right.Errors ||
        Left.Frames.size() != Right.Frames.size()) return false;
    const SourceMotionFootLockCoverage& A = Left.Coverage;
    const SourceMotionFootLockCoverage& B = Right.Coverage;
    if (A.InputFrames != B.InputFrames ||
        A.SeedPassthroughFrames != B.SeedPassthroughFrames ||
        A.CommittedFrames != B.CommittedFrames ||
        A.RolledBackFrames != B.RolledBackFrames ||
        A.SolverExecutions != B.SolverExecutions ||
        A.SolverCommits != B.SolverCommits ||
        A.ClampedSolves != B.ClampedSolves ||
        A.PositionNoMotionDeltas != B.PositionNoMotionDeltas ||
        A.PositionMotionDeltas != B.PositionMotionDeltas ||
        A.RotationNoMotionDeltas != B.RotationNoMotionDeltas ||
        A.RotationMotionDeltas != B.RotationMotionDeltas ||
        A.RotationGateReleases != B.RotationGateReleases ||
        A.PoleFallbacks != B.PoleFallbacks ||
        A.MaximumSourcePositionDeltaCm != B.MaximumSourcePositionDeltaCm ||
        A.MaximumSourceRotationDeltaDegrees !=
            B.MaximumSourceRotationDeltaDegrees ||
        A.MaximumReleasedFoundationRotationDegrees !=
            B.MaximumReleasedFoundationRotationDegrees ||
        A.MaximumNoMotionTargetDriftCm != B.MaximumNoMotionTargetDriftCm ||
        A.MaximumTargetDeltaErrorCm != B.MaximumTargetDeltaErrorCm ||
        A.MaximumRealEndpointGoalErrorCm !=
            B.MaximumRealEndpointGoalErrorCm ||
        A.MaximumReachResidualDeltaCm != B.MaximumReachResidualDeltaCm ||
        A.MaximumRealEndOrientationErrorDegrees !=
            B.MaximumRealEndOrientationErrorDegrees ||
        A.MaximumShadowToRealPositionDeltaCm !=
            B.MaximumShadowToRealPositionDeltaCm ||
        A.MaximumSegmentLengthErrorCm != B.MaximumSegmentLengthErrorCm ||
        A.MaximumEligibleScaleDeviation != B.MaximumEligibleScaleDeviation)
        return false;
    for (std::size_t Frame = 0; Frame < Left.Frames.size(); ++Frame)
    {
        const auto& X = Left.Frames[Frame];
        const auto& Y = Right.Frames[Frame];
        if (X.FrameIndex != Y.FrameIndex ||
            X.TimeSeconds != Y.TimeSeconds ||
            X.SeedFrameExactPassthrough != Y.SeedFrameExactPassthrough ||
            X.TransactionCommitted != Y.TransactionCommitted ||
            X.FailureLeftInputUnchanged != Y.FailureLeftInputUnchanged ||
            X.OnlyDeclaredLocalRotationsWritten !=
                Y.OnlyDeclaredLocalRotationsWritten ||
            X.AllLocalTranslationsBitwisePreserved !=
                Y.AllLocalTranslationsBitwisePreserved ||
            X.AllLocalScalesBitwisePreserved !=
                Y.AllLocalScalesBitwisePreserved ||
            X.TargetHipsBitwisePreserved !=
                Y.TargetHipsBitwisePreserved ||
            X.FailureReason != Y.FailureReason ||
            X.Message != Y.Message) return false;
        for (std::size_t Foot = 0; Foot < X.Feet.size(); ++Foot)
            if (!EquivalentRecord(X.Feet[Foot], Y.Feet[Foot])) return false;
    }
    return true;
}

SourceMotionFootLockOp::SourceMotionFootLockOp(
    SourceMotionFootLockOptions Options)
    : OpOptions(std::move(Options))
{
}

RetargetOpDescriptor SourceMotionFootLockOp::Descriptor() const
{
    RetargetOpDescriptor Result;
    Result.TypeId = OpOptions.RouteId;
    Result.Version = 1;
    Result.DisplayName = "Source Motion Foot Lock";
    Result.RequiresWholeClip = true;
    std::set<int> Added;
    for (const SourceMotionFootLockBinding& Binding : OpOptions.Feet)
    {
        for (int Index : Binding.TargetIndices)
        {
            if (!Added.insert(Index).second) continue;
            RetargetOpBoneWriteMask Mask;
            Mask.BoneIndex = Index;
            Mask.Rotation = true;
            Result.DeclaredWrites.push_back(Mask);
        }
    }
    return Result;
}

RetargetOpRunResult SourceMotionFootLockOp::Run(
    const NormalizedRuntimeSkeleton& SourceSkeleton,
    const NormalizedRuntimeSkeleton& TargetSkeleton,
    const RetargetOpClip& Input)
{
    Last = RunSourceMotionFootLock(
        SourceSkeleton, TargetSkeleton, Input, OpOptions);
    const auto Repeat = RunSourceMotionFootLock(
        SourceSkeleton, TargetSkeleton, Input, OpOptions);
    Last.DeterministicRepeatabilityVerified =
        EquivalentSourceMotionFootLockRuns(Last, Repeat);

    RetargetOpRunResult Result;
    Result.Success = Last.Success &&
        Last.DeterministicRepeatabilityVerified;
    Result.InputImmutable = Last.InputImmutable;
    Result.OutputModelsRebuilt = Last.AllTransactionsBounded;
    Result.MutationWithinDeclaredChannels =
        Last.AllTransactionsBounded;
    Result.FailureLeftInputUnchanged = !Result.Success &&
        EquivalentRetargetOpClips(Input, Last.Output);
    Result.RouteId = Last.RouteId;
    Result.Output = Last.Output;
    Result.Errors = Last.Errors;
    if (!Last.DeterministicRepeatabilityVerified)
        Result.Errors.push_back(
            "source-motion FootLock repeatability check failed");
    return Result;
}

const SourceMotionFootLockRunResult&
SourceMotionFootLockOp::LastRun() const
{
    return Last;
}

const char* ToString(SourceMotionFootLockFailureReason Reason)
{
    switch (Reason)
    {
    case SourceMotionFootLockFailureReason::None: return "none";
    case SourceMotionFootLockFailureReason::InvalidOptions:
        return "invalid_options";
    case SourceMotionFootLockFailureReason::InvalidSkeleton:
        return "invalid_skeleton";
    case SourceMotionFootLockFailureReason::InvalidInput:
        return "invalid_input";
    case SourceMotionFootLockFailureReason::ScaleOutsideShadowEligibility:
        return "scale_outside_shadow_eligibility";
    case SourceMotionFootLockFailureReason::PoleConstructionFailed:
        return "pole_construction_failed";
    case SourceMotionFootLockFailureReason::ShadowSolverFailed:
        return "shadow_solver_failed";
    case SourceMotionFootLockFailureReason::RealProjectionFailed:
        return "real_projection_failed";
    case SourceMotionFootLockFailureReason::PostvalidationFailed:
        return "postvalidation_failed";
    default: return "unknown";
    }
}
} // namespace skrtg::retarget::ops
