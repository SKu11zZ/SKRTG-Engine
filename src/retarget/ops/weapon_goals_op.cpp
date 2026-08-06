#include "skrtg/retarget/ops/weapon_goals_op.h"

#include "skrtg/core/math/transform.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace skrtg::retarget::ops
{
namespace
{
using core::math::Compose;
using core::math::Normalize;
using core::math::Quat;
using core::math::RelativeUnitScaleTransform;
using core::math::Scale;
using core::math::TransformRT;
using core::math::Vec3;
using core::skeleton::NormalizedRuntimeSkeleton;

bool Finite(const double Value) { return std::isfinite(Value); }
bool Finite(const Vec3 Value)
{
    return Finite(Value.X) && Finite(Value.Y) && Finite(Value.Z);
}
bool Finite(const Quat Value)
{
    return Finite(Value.X) && Finite(Value.Y) &&
        Finite(Value.Z) && Finite(Value.W);
}
bool Finite(const TransformRT& Value)
{
    return Finite(Value.TranslationCm) && Finite(Value.Rotation) &&
        Finite(Value.Scale);
}

bool UnitQuaternion(const Quat Value)
{
    const double Squared = Value.X * Value.X + Value.Y * Value.Y +
        Value.Z * Value.Z + Value.W * Value.W;
    return Finite(Squared) && std::abs(Squared - 1.0) <= 1.0e-4;
}

bool UnitScale(const Vec3 Value)
{
    return std::abs(Value.X - 1.0) <= 1.0e-9 &&
        std::abs(Value.Y - 1.0) <= 1.0e-9 &&
        std::abs(Value.Z - 1.0) <= 1.0e-9;
}

Vec3 Lerp(const Vec3 A, const Vec3 B, const double Alpha)
{
    return core::math::Add(
        Scale(A, 1.0 - Alpha), Scale(B, Alpha));
}

Quat Nlerp(Quat A, Quat B, const double Alpha)
{
    const double Dot = A.X * B.X + A.Y * B.Y +
        A.Z * B.Z + A.W * B.W;
    if (Dot < 0.0)
        B = {-B.X, -B.Y, -B.Z, -B.W};
    return Normalize({
        A.X + (B.X - A.X) * Alpha,
        A.Y + (B.Y - A.Y) * Alpha,
        A.Z + (B.Z - A.Z) * Alpha,
        A.W + (B.W - A.W) * Alpha});
}

const TransformRT* AnchorTransform(
    const WeaponGoalBinding& Binding,
    const RetargetOpFrame& Frame)
{
    if (Binding.AnchorSkeleton == WeaponAnchorSkeleton::Source)
        return &Frame.SourceModelPose[
            static_cast<std::size_t>(Binding.AnchorBoneIndex)];
    return &Frame.TargetModelPose[
        static_cast<std::size_t>(Binding.AnchorBoneIndex)];
}
} // namespace

WeaponGoalsOp::WeaponGoalsOp(WeaponGoalsOptions Options)
    : OpOptions(std::move(Options))
{
}

RetargetOpDescriptor WeaponGoalsOp::Descriptor() const
{
    RetargetOpDescriptor Result;
    Result.TypeId = OpOptions.RouteId;
    Result.Version = 1;
    Result.DisplayName = "Weapon Goals (Exact Name)";
    Result.Phase = RetargetOpPhase::GoalGeneration;
    for (const WeaponGoalBinding& Binding : OpOptions.Bindings)
    {
        RetargetOpGoalWriteMask Mask;
        Mask.GoalName = Binding.GoalName;
        Mask.Translation = Binding.TranslationAlpha > 0.0;
        Mask.Rotation = Binding.RotationAlpha > 0.0;
        Mask.AllowCreate = true;
        Result.DeclaredGoalWrites.push_back(std::move(Mask));
    }
    return Result;
}

RetargetOpPreflightResult WeaponGoalsOp::Preflight(
    const NormalizedRuntimeSkeleton& Source,
    const NormalizedRuntimeSkeleton& Target,
    const RetargetOpClip& Input) const
{
    RetargetOpPreflightResult Result;
    if (OpOptions.RouteId.empty() || OpOptions.Bindings.empty())
    {
        Result.Available = false;
        Result.Errors.push_back("weapon goal options are incomplete");
        return Result;
    }
    std::set<std::string> Labels;
    std::set<std::string> Goals;
    for (const WeaponGoalBinding& Binding : OpOptions.Bindings)
    {
        const std::size_t AnchorCount =
            Binding.AnchorSkeleton == WeaponAnchorSkeleton::Source
            ? Source.BoneCount()
            : Target.BoneCount();
        if (Binding.Label.empty() || Binding.GoalName.empty() ||
            !Labels.insert(Binding.Label).second ||
            !Goals.insert(Binding.GoalName).second ||
            Binding.AnchorBoneIndex < 0 ||
            Binding.AnchorBoneIndex >= static_cast<int>(AnchorCount) ||
            Binding.TargetGoalBoneIndex < -1 ||
            Binding.TargetGoalBoneIndex >=
                static_cast<int>(Target.BoneCount()) ||
            !Finite(Binding.AnchorOffset) ||
            !UnitQuaternion(Binding.AnchorOffset.Rotation) ||
            !UnitScale(Binding.AnchorOffset.Scale) ||
            !Finite(Binding.TranslationAlpha) ||
            !Finite(Binding.RotationAlpha) ||
            Binding.TranslationAlpha < 0.0 ||
            Binding.TranslationAlpha > 1.0 ||
            Binding.RotationAlpha < 0.0 ||
            Binding.RotationAlpha > 1.0 ||
            (Binding.TranslationAlpha == 0.0 &&
             Binding.RotationAlpha == 0.0))
        {
            Result.Available = false;
            Result.Errors.push_back(
                "weapon goal binding is invalid or duplicated: " +
                Binding.Label);
        }
    }
    for (const RetargetOpFrame& Frame : Input.Frames)
    {
        for (const WeaponGoalBinding& Binding : OpOptions.Bindings)
        {
            const RetargetOpGoal* Existing =
                FindRetargetOpGoal(Frame, Binding.GoalName);
            if (Existing != nullptr &&
                Binding.TargetGoalBoneIndex >= 0 &&
                Existing->TargetBoneIndex !=
                    Binding.TargetGoalBoneIndex)
            {
                Result.Available = false;
                Result.Errors.push_back(
                    "weapon goal target bone conflicts with an existing "
                    "goal: " + Binding.GoalName);
                return Result;
            }
        }
    }
    return Result;
}

RetargetOpRunResult WeaponGoalsOp::Run(
    const NormalizedRuntimeSkeleton&,
    const NormalizedRuntimeSkeleton&,
    const RetargetOpClip& Input)
{
    RetargetOpRunResult Result;
    Result.InputImmutable = true;
    Result.RouteId = OpOptions.RouteId;
    RetargetOpClip Candidate = Input;
    std::vector<TransformRT> MaintainedOffsets(
        OpOptions.Bindings.size());
    std::vector<bool> HasMaintainedOffset(
        OpOptions.Bindings.size(), false);

    for (RetargetOpFrame& Frame : Candidate.Frames)
    {
        for (std::size_t Index = 0;
             Index < OpOptions.Bindings.size(); ++Index)
        {
            const WeaponGoalBinding& Binding = OpOptions.Bindings[Index];
            const TransformRT Anchor = *AnchorTransform(Binding, Frame);
            RetargetOpGoal* Goal =
                FindRetargetOpGoal(Frame, Binding.GoalName);
            if (Goal == nullptr)
            {
                RetargetOpGoal Created;
                Created.Name = Binding.GoalName;
                Created.TargetBoneIndex = Binding.TargetGoalBoneIndex;
                Created.TransformModel =
                    Binding.TargetGoalBoneIndex >= 0
                    ? Frame.TargetModelPose[static_cast<std::size_t>(
                        Binding.TargetGoalBoneIndex)]
                    : Compose(Anchor, Binding.AnchorOffset);
                Frame.Goals.push_back(std::move(Created));
                Goal = &Frame.Goals.back();
            }
            if (Binding.MaintainInputOffset &&
                !HasMaintainedOffset[Index])
            {
                if (!RelativeUnitScaleTransform(
                        Anchor, Goal->TransformModel,
                        MaintainedOffsets[Index]))
                {
                    Result.Errors.push_back(
                        "weapon goal rest offset is not unit-scale: " +
                        Binding.Label);
                    Result.Output = Input;
                    Result.FailureLeftInputUnchanged = true;
                    return Result;
                }
                HasMaintainedOffset[Index] = true;
            }
            const TransformRT Desired = Compose(
                Anchor,
                Binding.MaintainInputOffset
                    ? MaintainedOffsets[Index]
                    : Binding.AnchorOffset);
            Goal->TransformModel.TranslationCm = Lerp(
                Goal->TransformModel.TranslationCm,
                Desired.TranslationCm,
                Binding.TranslationAlpha);
            Goal->TransformModel.Rotation = Nlerp(
                Goal->TransformModel.Rotation,
                Desired.Rotation,
                Binding.RotationAlpha);
        }
    }
    Result.Success = true;
    Result.OutputModelsRebuilt = true;
    Result.MutationWithinDeclaredChannels = true;
    Result.Output = std::move(Candidate);
    return Result;
}

const char* ToString(const WeaponAnchorSkeleton Skeleton)
{
    switch (Skeleton)
    {
    case WeaponAnchorSkeleton::Source:
        return "source";
    case WeaponAnchorSkeleton::Target:
        return "target";
    default:
        return "unknown";
    }
}
} // namespace skrtg::retarget::ops
