#include "skrtg/retarget/ops/goal_solver_op.h"

#include "skrtg/core/math/transform.h"

#include <cmath>
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
using core::math::Length;
using core::math::RelativeUnitScaleTransform;
using core::math::TransformRT;
using core::math::Vec3;
using core::skeleton::NormalizedRuntimeSkeleton;

bool Finite(const double Value) { return std::isfinite(Value); }
bool Finite(const Vec3 Value)
{
    return Finite(Value.X) && Finite(Value.Y) && Finite(Value.Z);
}

bool RebuildModelPose(const NormalizedRuntimeSkeleton& Skeleton,
                      const PoseBuffer& LocalPose,
                      PoseBuffer& OutModelPose)
{
    if (LocalPose.Space() != PoseSpace::Local ||
        !LocalPose.IsSizedFor(Skeleton) ||
        !Skeleton.ValidateParentIndexInvariant())
    {
        return false;
    }
    PoseBuffer Model(
        PoseSpace::Model, Skeleton.GetIdentity().HierarchyHash);
    Model.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        const int Parent = Skeleton.BoneAt(Index).ParentIndex;
        Model[Index] = Parent < 0
            ? LocalPose[Index]
            : core::math::Compose(
                Model[static_cast<std::size_t>(Parent)],
                LocalPose[Index]);
    }
    OutModelPose = std::move(Model);
    return true;
}

bool ValidBinding(const RetargetGoalSolveBinding& Binding,
                  const NormalizedRuntimeSkeleton& Target,
                  std::string& OutError)
{
    if (Binding.Label.empty() || Binding.GoalName.empty())
    {
        OutError = "goal solver binding identity is empty";
        return false;
    }
    if (!Binding.ApplyGoalTranslation && !Binding.ApplyGoalRotation)
    {
        OutError = "goal solver binding writes no channel: " +
            Binding.Label;
        return false;
    }
    if (Binding.Mode == RetargetGoalSolveMode::DirectBone)
    {
        if (Binding.TargetBoneIndex < 0 ||
            Binding.TargetBoneIndex >=
                static_cast<int>(Target.BoneCount()))
        {
            OutError = "direct goal binding target bone is invalid: " +
                Binding.Label;
            return false;
        }
        return true;
    }
    const int Root = Binding.TargetChainIndices[0];
    const int Mid = Binding.TargetChainIndices[1];
    const int End = Binding.TargetChainIndices[2];
    // A two-bone solve is position driven by definition. Rotation may be
    // disabled, but accepting a translation-disabled binding would silently
    // contradict the authored write contract.
    if (!Binding.ApplyGoalTranslation ||
        Root < 0 || Mid < 0 || End < 0 ||
        Root >= static_cast<int>(Target.BoneCount()) ||
        Mid >= static_cast<int>(Target.BoneCount()) ||
        End >= static_cast<int>(Target.BoneCount()) ||
        Target.BoneAt(static_cast<std::size_t>(Mid)).ParentIndex != Root ||
        Target.BoneAt(static_cast<std::size_t>(End)).ParentIndex != Mid)
    {
        OutError = "two-bone goal binding topology is invalid: " +
            Binding.Label;
        return false;
    }
    if (Binding.TargetPoleBoneIndex < -1 ||
        Binding.TargetPoleBoneIndex >=
            static_cast<int>(Target.BoneCount()))
    {
        OutError = "goal solver pole bone is invalid: " + Binding.Label;
        return false;
    }
    if (Binding.PoleGoalName.empty() &&
        Binding.TargetPoleBoneIndex < 0 &&
        (!Finite(Binding.PoleFallbackOffsetModelCm) ||
         Length(Binding.PoleFallbackOffsetModelCm) <= 1.0e-9))
    {
        OutError = "goal solver fallback pole is invalid: " +
            Binding.Label;
        return false;
    }
    return true;
}

bool ApplyDirectBinding(
    const RetargetGoalSolveBinding& Binding,
    const RetargetOpGoal& Goal,
    const NormalizedRuntimeSkeleton& Target,
    RetargetOpFrame& Frame,
    std::string& OutError)
{
    const std::size_t BoneIndex =
        static_cast<std::size_t>(Binding.TargetBoneIndex);
    TransformRT Desired = Frame.TargetModelPose[BoneIndex];
    if (Binding.ApplyGoalTranslation)
        Desired.TranslationCm = Goal.TransformModel.TranslationCm;
    if (Binding.ApplyGoalRotation)
        Desired.Rotation = Goal.TransformModel.Rotation;

    TransformRT Local = Frame.TargetLocalPose[BoneIndex];
    const int Parent = Target.BoneAt(BoneIndex).ParentIndex;
    if (Parent < 0)
    {
        if (Binding.ApplyGoalTranslation)
            Local.TranslationCm = Desired.TranslationCm;
        if (Binding.ApplyGoalRotation)
            Local.Rotation = Desired.Rotation;
    }
    else
    {
        TransformRT DesiredLocal;
        if (!RelativeUnitScaleTransform(
                Frame.TargetModelPose[
                    static_cast<std::size_t>(Parent)],
                Desired, DesiredLocal))
        {
            OutError = "direct goal local reconstruction failed: " +
                Binding.Label;
            return false;
        }
        if (Binding.ApplyGoalTranslation)
            Local.TranslationCm = DesiredLocal.TranslationCm;
        if (Binding.ApplyGoalRotation)
            Local.Rotation = DesiredLocal.Rotation;
    }
    Frame.TargetLocalPose[BoneIndex] = Local;
    if (!RebuildModelPose(
            Target, Frame.TargetLocalPose, Frame.TargetModelPose))
    {
        OutError = "direct goal model rebuild failed: " + Binding.Label;
        return false;
    }
    return true;
}
} // namespace

RetargetGoalSolverOp::RetargetGoalSolverOp(
    RetargetGoalSolverOptions Options)
    : OpOptions(std::move(Options))
{
}

RetargetOpDescriptor RetargetGoalSolverOp::Descriptor() const
{
    RetargetOpDescriptor Result;
    Result.TypeId = OpOptions.RouteId;
    Result.Version = 1;
    Result.DisplayName = "Unified Goal Solver";
    Result.Phase = RetargetOpPhase::PoseSolve;
    std::set<int> Seen;
    for (const RetargetGoalSolveBinding& Binding : OpOptions.Bindings)
    {
        if (Binding.Mode == RetargetGoalSolveMode::DirectBone)
        {
            if (Seen.insert(Binding.TargetBoneIndex).second)
            {
                RetargetOpBoneWriteMask Mask;
                Mask.BoneIndex = Binding.TargetBoneIndex;
                Mask.Translation = Binding.ApplyGoalTranslation;
                Mask.Rotation = Binding.ApplyGoalRotation;
                Result.DeclaredWrites.push_back(Mask);
            }
            continue;
        }
        for (const int Index : Binding.TargetChainIndices)
        {
            if (!Seen.insert(Index).second) continue;
            RetargetOpBoneWriteMask Mask;
            Mask.BoneIndex = Index;
            Mask.Rotation = true;
            Result.DeclaredWrites.push_back(Mask);
        }
    }
    return Result;
}

RetargetOpPreflightResult RetargetGoalSolverOp::Preflight(
    const NormalizedRuntimeSkeleton&,
    const NormalizedRuntimeSkeleton& Target,
    const RetargetOpClip& Input) const
{
    RetargetOpPreflightResult Result;
    if (OpOptions.RouteId.empty() || OpOptions.Bindings.empty() ||
        !Finite(OpOptions.SolverEpsilon) ||
        !Finite(OpOptions.SolverPositionToleranceCm) ||
        !Finite(OpOptions.SolverLengthToleranceCm) ||
        OpOptions.SolverEpsilon <= 0.0 ||
        OpOptions.SolverPositionToleranceCm <= 0.0 ||
        OpOptions.SolverLengthToleranceCm <= 0.0)
    {
        Result.Available = false;
        Result.Errors.push_back("goal solver options are incomplete");
        return Result;
    }
    std::set<std::string> Labels;
    std::set<int> WrittenBones;
    for (const RetargetGoalSolveBinding& Binding : OpOptions.Bindings)
    {
        std::string Error;
        if (!Labels.insert(Binding.Label).second ||
            !ValidBinding(Binding, Target, Error))
        {
            Result.Available = false;
            Result.Errors.push_back(Error.empty()
                ? "duplicate goal solver binding label"
                : Error);
            continue;
        }
        const auto Claim = [&](const int Bone)
        {
            if (!WrittenBones.insert(Bone).second)
            {
                Result.Available = false;
                Result.Errors.push_back(
                    "goal solver bindings overlap target bone writes: " +
                    Binding.Label);
            }
        };
        if (Binding.Mode == RetargetGoalSolveMode::DirectBone)
            Claim(Binding.TargetBoneIndex);
        else
            for (const int Bone : Binding.TargetChainIndices) Claim(Bone);
    }
    for (const RetargetOpFrame& Frame : Input.Frames)
    {
        for (const RetargetGoalSolveBinding& Binding : OpOptions.Bindings)
        {
            if (FindRetargetOpGoal(Frame, Binding.GoalName) == nullptr)
            {
                Result.Available = false;
                Result.Errors.push_back(
                    "required goal is missing: " + Binding.GoalName);
                return Result;
            }
            if (!Binding.PoleGoalName.empty() &&
                FindRetargetOpGoal(Frame, Binding.PoleGoalName) == nullptr)
            {
                Result.Available = false;
                Result.Errors.push_back(
                    "required pole goal is missing: " +
                    Binding.PoleGoalName);
                return Result;
            }
        }
    }
    return Result;
}

RetargetOpRunResult RetargetGoalSolverOp::Run(
    const NormalizedRuntimeSkeleton&,
    const NormalizedRuntimeSkeleton& Target,
    const RetargetOpClip& Input)
{
    RetargetOpRunResult Result;
    Result.InputImmutable = true;
    Result.RouteId = OpOptions.RouteId;
    RetargetOpClip Candidate = Input;
    std::vector<QuaternionContinuityReference> Previous(
        OpOptions.Bindings.size());

    for (RetargetOpFrame& Frame : Candidate.Frames)
    {
        for (std::size_t BindingIndex = 0;
             BindingIndex < OpOptions.Bindings.size(); ++BindingIndex)
        {
            const RetargetGoalSolveBinding& Binding =
                OpOptions.Bindings[BindingIndex];
            const RetargetOpGoal* Goal =
                FindRetargetOpGoal(Frame, Binding.GoalName);
            if (Goal == nullptr)
            {
                Result.Errors.push_back(
                    "goal disappeared during solve: " +
                    Binding.GoalName);
                Result.Output = Input;
                Result.FailureLeftInputUnchanged = true;
                return Result;
            }
            if (Binding.Mode == RetargetGoalSolveMode::DirectBone)
            {
                std::string Error;
                if (!ApplyDirectBinding(
                        Binding, *Goal, Target, Frame, Error))
                {
                    Result.Errors.push_back(Error);
                    Result.Output = Input;
                    Result.FailureLeftInputUnchanged = true;
                    return Result;
                }
                continue;
            }

            TwoBonePoseBufferRequest Request;
            Request.Chain.RootIndex = Binding.TargetChainIndices[0];
            Request.Chain.MidIndex = Binding.TargetChainIndices[1];
            Request.Chain.EndIndex = Binding.TargetChainIndices[2];
            Request.Chain.MidParentIndex = Binding.TargetChainIndices[0];
            Request.Chain.EndParentIndex = Binding.TargetChainIndices[1];
            Request.TargetPositionModelCm =
                Goal->TransformModel.TranslationCm;
            const RetargetOpGoal* PoleGoal = Binding.PoleGoalName.empty()
                ? nullptr
                : FindRetargetOpGoal(Frame, Binding.PoleGoalName);
            if (PoleGoal != nullptr)
            {
                Request.PolePositionModelCm =
                    PoleGoal->TransformModel.TranslationCm;
                Request.PoleFallback = PoleFallbackPolicy::FailClosed;
            }
            else if (Binding.TargetPoleBoneIndex >= 0)
            {
                Request.PolePositionModelCm = Frame.TargetModelPose[
                    static_cast<std::size_t>(
                        Binding.TargetPoleBoneIndex)].TranslationCm;
                Request.PoleFallback = PoleFallbackPolicy::FailClosed;
            }
            else
            {
                const auto RootPosition = Frame.TargetModelPose[
                    static_cast<std::size_t>(
                        Binding.TargetChainIndices[0])].TranslationCm;
                Request.PolePositionModelCm = core::math::Add(
                    RootPosition, Binding.PoleFallbackOffsetModelCm);
                Request.PoleFallback =
                    PoleFallbackPolicy::AllowConfiguredAxis;
                Request.ConfiguredFallbackAxisModel =
                    Binding.PoleFallbackOffsetModelCm;
            }
            Request.OrientationPolicy = Binding.ApplyGoalRotation
                ? EndOrientationPolicy::ApplyExplicitModel
                : EndOrientationPolicy::PreserveInputLocal;
            Request.ExplicitEndModelOrientation =
                Goal->TransformModel.Rotation;
            Request.PreviousLocalRotations = Previous[BindingIndex];
            Request.Epsilon = OpOptions.SolverEpsilon;
            Request.PositionToleranceCm =
                OpOptions.SolverPositionToleranceCm;
            Request.LengthToleranceCm =
                OpOptions.SolverLengthToleranceCm;
            const auto Solve = ApplyAnalyticTwoBoneToPoseBuffer(
                Target, Request, Frame.TargetLocalPose);
            if (!Solve.Success)
            {
                Result.Errors.push_back(
                    "goal solve failed for " + Binding.Label + ": " +
                    Solve.Message);
                Result.Output = Input;
                Result.FailureLeftInputUnchanged = true;
                return Result;
            }
            Frame.TargetModelPose = Solve.OutputModelPose;
            const std::size_t Root = static_cast<std::size_t>(
                Binding.TargetChainIndices[0]);
            const std::size_t Mid = static_cast<std::size_t>(
                Binding.TargetChainIndices[1]);
            const std::size_t End = static_cast<std::size_t>(
                Binding.TargetChainIndices[2]);
            Previous[BindingIndex].HasRootLocal = true;
            Previous[BindingIndex].HasMidLocal = true;
            Previous[BindingIndex].HasEndLocal = true;
            Previous[BindingIndex].RootLocal =
                Frame.TargetLocalPose[Root].Rotation;
            Previous[BindingIndex].MidLocal =
                Frame.TargetLocalPose[Mid].Rotation;
            Previous[BindingIndex].EndLocal =
                Frame.TargetLocalPose[End].Rotation;
        }
    }
    Result.Success = true;
    Result.OutputModelsRebuilt = true;
    Result.MutationWithinDeclaredChannels = true;
    Result.Output = std::move(Candidate);
    return Result;
}

const char* ToString(const RetargetGoalSolveMode Mode)
{
    switch (Mode)
    {
    case RetargetGoalSolveMode::TwoBone:
        return "two_bone";
    case RetargetGoalSolveMode::DirectBone:
        return "direct_bone";
    default:
        return "unknown";
    }
}
} // namespace skrtg::retarget::ops
