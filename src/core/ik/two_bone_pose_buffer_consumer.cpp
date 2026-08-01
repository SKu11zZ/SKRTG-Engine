#include "skrtg/core/ik/two_bone_pose_buffer_consumer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace skrtg::core::ik
{
namespace
{
constexpr double ModelRotationTolerance = 1.0e-12;
constexpr double ModelScaleTolerance = 1.0e-12;

bool IsFinite(double Value)
{
    return std::isfinite(Value);
}

bool IsFinite(math::Vec3 Value)
{
    return IsFinite(Value.X) && IsFinite(Value.Y) && IsFinite(Value.Z);
}

bool IsFinite(math::Quat Value)
{
    return IsFinite(Value.X) && IsFinite(Value.Y) && IsFinite(Value.Z) && IsFinite(Value.W);
}

bool IsFinite(math::TransformRT Value)
{
    return IsFinite(Value.TranslationCm) && IsFinite(Value.Rotation) && IsFinite(Value.Scale);
}

bool EqualComponents(math::Vec3 Left, math::Vec3 Right)
{
    return Left.X == Right.X && Left.Y == Right.Y && Left.Z == Right.Z;
}

bool EqualComponents(math::Quat Left, math::Quat Right)
{
    return Left.X == Right.X && Left.Y == Right.Y && Left.Z == Right.Z && Left.W == Right.W;
}

bool EqualComponents(math::TransformRT Left, math::TransformRT Right)
{
    return EqualComponents(Left.TranslationCm, Right.TranslationCm) &&
           EqualComponents(Left.Rotation, Right.Rotation) &&
           EqualComponents(Left.Scale, Right.Scale);
}

double MaximumComponentDelta(math::Vec3 Left, math::Vec3 Right)
{
    return std::max({std::abs(Left.X - Right.X),
                     std::abs(Left.Y - Right.Y),
                     std::abs(Left.Z - Right.Z)});
}

double MaximumComponentDelta(math::Quat Left, math::Quat Right)
{
    const math::Quat A = math::Normalize(Left);
    const math::Quat B = math::Normalize(Right);
    const double Direct = std::max({std::abs(A.X - B.X),
                                    std::abs(A.Y - B.Y),
                                    std::abs(A.Z - B.Z),
                                    std::abs(A.W - B.W)});
    const double Negated = std::max({std::abs(A.X + B.X),
                                     std::abs(A.Y + B.Y),
                                     std::abs(A.Z + B.Z),
                                     std::abs(A.W + B.W)});
    return std::min(Direct, Negated);
}

double MaximumComponentDelta(math::TransformRT Left, math::TransformRT Right)
{
    return std::max({MaximumComponentDelta(Left.TranslationCm, Right.TranslationCm),
                     MaximumComponentDelta(Left.Rotation, Right.Rotation),
                     MaximumComponentDelta(Left.Scale, Right.Scale)});
}

double QuaternionRotationError(math::Quat Left, math::Quat Right)
{
    const math::Quat A = math::Normalize(Left);
    const math::Quat B = math::Normalize(Right);
    const double Dot = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W;
    return std::max(0.0, 1.0 - std::abs(Dot));
}

bool IsChainIndex(int Index, const TwoBoneChainTopology& Chain)
{
    return Index == Chain.RootIndex || Index == Chain.MidIndex || Index == Chain.EndIndex;
}

bool BuildModelPose(const skeleton::NormalizedRuntimeSkeleton& Skeleton,
                    const animation::PoseBuffer& LocalPose,
                    animation::PoseBuffer& OutModelPose)
{
    if (LocalPose.Space() != animation::PoseSpace::Local || !LocalPose.IsSizedFor(Skeleton) ||
        !Skeleton.ValidateParentIndexInvariant())
    {
        return false;
    }

    animation::PoseBuffer ModelPose(animation::PoseSpace::Model,
                                    Skeleton.GetIdentity().HierarchyHash);
    ModelPose.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        const math::TransformRT Local = LocalPose[Index];
        if (!IsFinite(Local))
        {
            return false;
        }
        const int ParentIndex = Skeleton.BoneAt(Index).ParentIndex;
        ModelPose[Index] = ParentIndex < 0
                               ? Local
                               : math::Compose(ModelPose[static_cast<std::size_t>(ParentIndex)], Local);
        if (!IsFinite(ModelPose[Index]))
        {
            return false;
        }
    }
    OutModelPose = std::move(ModelPose);
    return true;
}

TwoBonePoseBufferResult Fail(TwoBonePoseBufferResult Result,
                             TwoBonePoseBufferFailureReason Reason,
                             const char* Message)
{
    Result.Success = false;
    Result.Status = TwoBonePoseBufferStatus::Failed;
    Result.FailureReason = Reason;
    Result.Message = Message;
    Result.Telemetry.PoseWriteCommitted = false;
    Result.Telemetry.FailureLeftInputPoseUnchanged = true;
    return Result;
}
} // namespace

TwoBonePoseBufferResult ApplyAnalyticTwoBoneToPoseBuffer(
    const skeleton::NormalizedRuntimeSkeleton& Skeleton,
    const TwoBonePoseBufferRequest& Request,
    animation::PoseBuffer& InOutLocalPose)
{
    TwoBonePoseBufferResult Result;
    if (!Skeleton.ValidateParentIndexInvariant())
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::InvalidSkeletonTopology,
                    "skeleton parent indices must precede their children");
    }
    if (InOutLocalPose.Space() != animation::PoseSpace::Local)
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::InvalidPoseSpace,
                    "D1-16B requires a local-space PoseBuffer");
    }
    if (!InOutLocalPose.IsSizedFor(Skeleton))
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::PoseSkeletonMismatch,
                    "PoseBuffer size and hierarchy identity must match the supplied skeleton");
    }

    const TwoBoneChainTopology& Chain = Request.Chain;
    const int BoneCount = static_cast<int>(Skeleton.BoneCount());
    const bool IndicesInRange = Chain.RootIndex >= 0 && Chain.RootIndex < BoneCount &&
                                Chain.MidIndex >= 0 && Chain.MidIndex < BoneCount &&
                                Chain.EndIndex >= 0 && Chain.EndIndex < BoneCount;
    const bool IndicesDistinct = Chain.RootIndex != Chain.MidIndex &&
                                 Chain.RootIndex != Chain.EndIndex &&
                                 Chain.MidIndex != Chain.EndIndex;
    if (!IndicesInRange || !IndicesDistinct)
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::InvalidChainIndices,
                    "root/mid/end indices must be distinct and within the PoseBuffer");
    }
    if (Chain.MidParentIndex != Chain.RootIndex || Chain.EndParentIndex != Chain.MidIndex ||
        Skeleton.BoneAt(static_cast<std::size_t>(Chain.MidIndex)).ParentIndex != Chain.MidParentIndex ||
        Skeleton.BoneAt(static_cast<std::size_t>(Chain.EndIndex)).ParentIndex != Chain.EndParentIndex)
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::InvalidChainTopology,
                    "request and skeleton must identify the same root->mid->end parent chain");
    }
    Result.Telemetry.ChainIdentityValidated = true;

    animation::PoseBuffer InputModelPose;
    if (!BuildModelPose(Skeleton, InOutLocalPose, InputModelPose))
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::InvalidPoseTransform,
                    "input local pose could not produce a finite model pose");
    }

    TwoBoneIkInput SolverInput;
    SolverInput.Chain = Chain;
    const int RootParentIndex = Skeleton.BoneAt(static_cast<std::size_t>(Chain.RootIndex)).ParentIndex;
    SolverInput.RootParentModel = RootParentIndex < 0
                                      ? math::IdentityTransform()
                                      : InputModelPose[static_cast<std::size_t>(RootParentIndex)];
    SolverInput.RootLocal = InOutLocalPose[static_cast<std::size_t>(Chain.RootIndex)];
    SolverInput.MidLocal = InOutLocalPose[static_cast<std::size_t>(Chain.MidIndex)];
    SolverInput.EndLocal = InOutLocalPose[static_cast<std::size_t>(Chain.EndIndex)];
    SolverInput.TargetPositionModelCm = Request.TargetPositionModelCm;
    SolverInput.PolePositionModelCm = Request.PolePositionModelCm;
    SolverInput.PoleFallback = Request.PoleFallback;
    SolverInput.ConfiguredFallbackAxisModel = Request.ConfiguredFallbackAxisModel;
    SolverInput.BendLimits = Request.BendLimits;
    SolverInput.SoftReach = Request.SoftReach;
    SolverInput.OrientationPolicy = Request.OrientationPolicy;
    SolverInput.ExplicitEndModelOrientation = Request.ExplicitEndModelOrientation;
    SolverInput.PreviousLocalRotations = Request.PreviousLocalRotations;
    SolverInput.Epsilon = Request.Epsilon;
    SolverInput.PositionToleranceCm = Request.PositionToleranceCm;
    SolverInput.LengthToleranceCm = Request.LengthToleranceCm;

    Result.SolverResult = SolveAnalyticTwoBone(SolverInput);
    if (!Result.SolverResult.Success)
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::SolverFailed,
                    "D1-16A solver failed; no PoseBuffer write was committed");
    }

    const animation::PoseBuffer InputLocalSnapshot = InOutLocalPose;
    animation::PoseBuffer CandidateLocalPose = InOutLocalPose;
    CandidateLocalPose[static_cast<std::size_t>(Chain.RootIndex)].Rotation =
        Result.SolverResult.OutputRootLocal.Rotation;
    CandidateLocalPose[static_cast<std::size_t>(Chain.MidIndex)].Rotation =
        Result.SolverResult.OutputMidLocal.Rotation;
    CandidateLocalPose[static_cast<std::size_t>(Chain.EndIndex)].Rotation =
        Result.SolverResult.OutputEndLocal.Rotation;

    Result.Telemetry.LocalTranslationsPreserved = true;
    Result.Telemetry.LocalScalesPreserved = true;
    Result.Telemetry.UnrelatedLocalTransformsPreserved = true;
    Result.Telemetry.OnlyChainLocalRotationsWritten = true;
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        const math::TransformRT Before = InputLocalSnapshot[Index];
        const math::TransformRT After = CandidateLocalPose[Index];
        Result.Telemetry.LocalTranslationsPreserved =
            Result.Telemetry.LocalTranslationsPreserved &&
            EqualComponents(Before.TranslationCm, After.TranslationCm);
        Result.Telemetry.LocalScalesPreserved =
            Result.Telemetry.LocalScalesPreserved && EqualComponents(Before.Scale, After.Scale);
        if (!EqualComponents(Before.Rotation, After.Rotation))
        {
            ++Result.Telemetry.ModifiedLocalRotationCount;
            if (!IsChainIndex(static_cast<int>(Index), Chain))
            {
                Result.Telemetry.OnlyChainLocalRotationsWritten = false;
            }
        }
        if (!IsChainIndex(static_cast<int>(Index), Chain))
        {
            Result.Telemetry.UnrelatedLocalMaximumComponentDelta = std::max(
                Result.Telemetry.UnrelatedLocalMaximumComponentDelta,
                MaximumComponentDelta(Before, After));
            Result.Telemetry.UnrelatedLocalTransformsPreserved =
                Result.Telemetry.UnrelatedLocalTransformsPreserved &&
                EqualComponents(Before, After);
        }
    }
    Result.Telemetry.EndLocalOrientationPreserved =
        Request.OrientationPolicy == EndOrientationPolicy::PreserveInputLocal &&
        EqualComponents(InputLocalSnapshot[static_cast<std::size_t>(Chain.EndIndex)].Rotation,
                        CandidateLocalPose[static_cast<std::size_t>(Chain.EndIndex)].Rotation);
    Result.Telemetry.RootPelvisGlobalCompensationApplied =
        !Result.Telemetry.LocalTranslationsPreserved ||
        Result.SolverResult.Telemetry.RootPelvisGlobalCompensationApplied;

    if (!Result.Telemetry.LocalTranslationsPreserved || !Result.Telemetry.LocalScalesPreserved ||
        !Result.Telemetry.UnrelatedLocalTransformsPreserved ||
        !Result.Telemetry.OnlyChainLocalRotationsWritten ||
        (Request.OrientationPolicy == EndOrientationPolicy::PreserveInputLocal &&
         !Result.Telemetry.EndLocalOrientationPreserved) ||
        Result.Telemetry.RootPelvisGlobalCompensationApplied)
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::MutationScopeViolation,
                    "candidate pose violated the bounded local-rotation-only write contract");
    }

    animation::PoseBuffer CandidateModelPose;
    if (!BuildModelPose(Skeleton, CandidateLocalPose, CandidateModelPose))
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::ModelRebuildFailed,
                    "candidate local pose could not rebuild a finite model pose");
    }

    const int ChainIndices[] = {Chain.RootIndex, Chain.MidIndex, Chain.EndIndex};
    const math::TransformRT SolverModels[] = {Result.SolverResult.OutputRootModel,
                                              Result.SolverResult.OutputMidModel,
                                              Result.SolverResult.OutputEndModel};
    Result.Telemetry.ParentLocalModelConsistent = true;
    for (std::size_t ChainOffset = 0; ChainOffset < 3; ++ChainOffset)
    {
        const math::TransformRT Rebuilt = CandidateModelPose[static_cast<std::size_t>(ChainIndices[ChainOffset])];
        Result.Telemetry.SolverModelMaximumTranslationErrorCm = std::max(
            Result.Telemetry.SolverModelMaximumTranslationErrorCm,
            math::Length(math::Subtract(Rebuilt.TranslationCm,
                                        SolverModels[ChainOffset].TranslationCm)));
        Result.Telemetry.SolverModelMaximumRotationError = std::max(
            Result.Telemetry.SolverModelMaximumRotationError,
            QuaternionRotationError(Rebuilt.Rotation, SolverModels[ChainOffset].Rotation));
        Result.Telemetry.SolverModelMaximumScaleError = std::max(
            Result.Telemetry.SolverModelMaximumScaleError,
            MaximumComponentDelta(Rebuilt.Scale, SolverModels[ChainOffset].Scale));
    }
    Result.Telemetry.ParentLocalModelConsistent =
        Result.Telemetry.SolverModelMaximumTranslationErrorCm <= Request.PositionToleranceCm &&
        Result.Telemetry.SolverModelMaximumRotationError <= ModelRotationTolerance &&
        Result.Telemetry.SolverModelMaximumScaleError <= ModelScaleTolerance;
    if (!Result.Telemetry.ParentLocalModelConsistent)
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::SolverModelMismatch,
                    "PoseBuffer hierarchy rebuild did not match the D1-16A model outputs");
    }

    const math::Vec3 RootPosition =
        CandidateModelPose[static_cast<std::size_t>(Chain.RootIndex)].TranslationCm;
    const math::Vec3 MidPosition =
        CandidateModelPose[static_cast<std::size_t>(Chain.MidIndex)].TranslationCm;
    const math::Vec3 EndPosition =
        CandidateModelPose[static_cast<std::size_t>(Chain.EndIndex)].TranslationCm;
    const double FirstLength = math::Length(InputLocalSnapshot[static_cast<std::size_t>(Chain.MidIndex)].TranslationCm);
    const double SecondLength = math::Length(InputLocalSnapshot[static_cast<std::size_t>(Chain.EndIndex)].TranslationCm);
    Result.Telemetry.EndpointErrorCm = math::Length(math::Subtract(EndPosition,
                                                                  Request.TargetPositionModelCm));
    Result.Telemetry.FirstSegmentLengthErrorCm =
        std::abs(math::Length(math::Subtract(MidPosition, RootPosition)) - FirstLength);
    Result.Telemetry.SecondSegmentLengthErrorCm =
        std::abs(math::Length(math::Subtract(EndPosition, MidPosition)) - SecondLength);
    if (!IsFinite(Result.Telemetry.EndpointErrorCm) ||
        Result.Telemetry.FirstSegmentLengthErrorCm > Request.LengthToleranceCm ||
        Result.Telemetry.SecondSegmentLengthErrorCm > Request.LengthToleranceCm ||
        std::abs(Result.Telemetry.EndpointErrorCm - Result.SolverResult.Telemetry.EndpointErrorCm) >
            Request.PositionToleranceCm)
    {
        return Fail(std::move(Result), TwoBonePoseBufferFailureReason::SolverModelMismatch,
                    "consumer FK metrics did not match the validated solver result");
    }

    InOutLocalPose = std::move(CandidateLocalPose);
    Result.OutputModelPose = std::move(CandidateModelPose);
    Result.Success = true;
    Result.Status = Result.SolverResult.Status == TwoBoneSolveStatus::SolvedClamped
                        ? TwoBonePoseBufferStatus::AppliedClamped
                        : TwoBonePoseBufferStatus::Applied;
    Result.FailureReason = TwoBonePoseBufferFailureReason::None;
    Result.Message = Result.Status == TwoBonePoseBufferStatus::Applied
                         ? "D1-16A result committed to the bounded PoseBuffer chain"
                         : "explicitly clamped D1-16A result committed to the bounded PoseBuffer chain";
    Result.Telemetry.PoseWriteCommitted = true;
    Result.Telemetry.FailureLeftInputPoseUnchanged = false;
    Result.Telemetry.ModelPoseValid = true;
    return Result;
}

const char* ToString(TwoBonePoseBufferStatus Status)
{
    switch (Status)
    {
    case TwoBonePoseBufferStatus::Failed: return "failed";
    case TwoBonePoseBufferStatus::Applied: return "applied";
    case TwoBonePoseBufferStatus::AppliedClamped: return "applied_clamped";
    default: return "unknown";
    }
}

const char* ToString(TwoBonePoseBufferFailureReason Reason)
{
    switch (Reason)
    {
    case TwoBonePoseBufferFailureReason::None: return "none";
    case TwoBonePoseBufferFailureReason::InvalidSkeletonTopology: return "invalid_skeleton_topology";
    case TwoBonePoseBufferFailureReason::InvalidPoseSpace: return "invalid_pose_space";
    case TwoBonePoseBufferFailureReason::PoseSkeletonMismatch: return "pose_skeleton_mismatch";
    case TwoBonePoseBufferFailureReason::InvalidChainIndices: return "invalid_chain_indices";
    case TwoBonePoseBufferFailureReason::InvalidChainTopology: return "invalid_chain_topology";
    case TwoBonePoseBufferFailureReason::InvalidPoseTransform: return "invalid_pose_transform";
    case TwoBonePoseBufferFailureReason::SolverFailed: return "solver_failed";
    case TwoBonePoseBufferFailureReason::ModelRebuildFailed: return "model_rebuild_failed";
    case TwoBonePoseBufferFailureReason::SolverModelMismatch: return "solver_model_mismatch";
    case TwoBonePoseBufferFailureReason::MutationScopeViolation: return "mutation_scope_violation";
    default: return "unknown";
    }
}
} // namespace skrtg::core::ik
