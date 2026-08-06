#include "skrtg/retarget/ops/stride_warping_op.h"

#include "skrtg/core/math/transform.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace skrtg::retarget::ops
{
namespace
{
using core::math::Add;
using core::math::Dot;
using core::math::Length;
using core::math::RotateVector;
using core::math::Scale;
using core::math::Subtract;
using core::math::Vec3;
using core::skeleton::NormalizedRuntimeSkeleton;

bool Finite(const double Value) { return std::isfinite(Value); }
bool Finite(const Vec3 Value)
{
    return Finite(Value.X) && Finite(Value.Y) && Finite(Value.Z);
}

Vec3 Cross(const Vec3 A, const Vec3 B)
{
    return {
        A.Y * B.Z - A.Z * B.Y,
        A.Z * B.X - A.X * B.Z,
        A.X * B.Y - A.Y * B.X};
}

bool NormalizeVector(const Vec3 Value, Vec3& Out)
{
    const double Magnitude = Length(Value);
    if (!Finite(Magnitude) || Magnitude <= 1.0e-9) return false;
    Out = Scale(Value, 1.0 / Magnitude);
    return true;
}

Vec3 Lerp(const Vec3 A, const Vec3 B, const double Alpha)
{
    return Add(Scale(A, 1.0 - Alpha), Scale(B, Alpha));
}
} // namespace

StrideWarpingOp::StrideWarpingOp(StrideWarpingOptions Options)
    : OpOptions(std::move(Options))
{
}

RetargetOpDescriptor StrideWarpingOp::Descriptor() const
{
    RetargetOpDescriptor Result;
    Result.TypeId = OpOptions.RouteId;
    Result.Version = 1;
    Result.DisplayName = "Stride Warping";
    Result.Phase = RetargetOpPhase::GoalWarp;
    for (const StrideWarpGoalBinding& Goal : OpOptions.Goals)
    {
        RetargetOpGoalWriteMask Mask;
        Mask.GoalName = Goal.GoalName;
        Mask.Translation = true;
        Result.DeclaredGoalWrites.push_back(std::move(Mask));
    }
    return Result;
}

RetargetOpPreflightResult StrideWarpingOp::Preflight(
    const NormalizedRuntimeSkeleton&,
    const NormalizedRuntimeSkeleton& Target,
    const RetargetOpClip& Input) const
{
    RetargetOpPreflightResult Result;
    Vec3 Forward;
    Vec3 Up;
    Vec3 Right;
    if (OpOptions.RouteId.empty() || OpOptions.Goals.empty() ||
        OpOptions.TargetPivotBoneIndex < 0 ||
        OpOptions.TargetPivotBoneIndex >=
            static_cast<int>(Target.BoneCount()) ||
        !Finite(OpOptions.ForwardAxisLocal) ||
        !Finite(OpOptions.UpAxisLocal) ||
        !NormalizeVector(OpOptions.ForwardAxisLocal, Forward) ||
        !NormalizeVector(OpOptions.UpAxisLocal, Up) ||
        !NormalizeVector(Cross(Up, Forward), Right) ||
        std::abs(Dot(Forward, Up)) > 1.0e-4 ||
        !Finite(OpOptions.WarpForwards) ||
        !Finite(OpOptions.WarpSplay) ||
        !Finite(OpOptions.SidewaysOffsetCm) ||
        !Finite(OpOptions.Alpha) || OpOptions.WarpForwards < 0.0 ||
        OpOptions.WarpSplay < 0.0 || OpOptions.Alpha < 0.0 ||
        OpOptions.Alpha > 1.0)
    {
        Result.Available = false;
        Result.Errors.push_back("stride warping options are invalid");
        return Result;
    }
    std::set<std::string> GoalNames;
    for (const StrideWarpGoalBinding& Goal : OpOptions.Goals)
    {
        if (Goal.GoalName.empty() ||
            !GoalNames.insert(Goal.GoalName).second ||
            Goal.SideSign < -1 || Goal.SideSign > 1 ||
            !Finite(Goal.Alpha) || Goal.Alpha < 0.0 || Goal.Alpha > 1.0)
        {
            Result.Available = false;
            Result.Errors.push_back(
                "stride goal binding is invalid or duplicated");
        }
    }
    for (const RetargetOpFrame& Frame : Input.Frames)
    {
        for (const StrideWarpGoalBinding& Goal : OpOptions.Goals)
        {
            if (FindRetargetOpGoal(Frame, Goal.GoalName) == nullptr)
            {
                Result.Available = false;
                Result.Errors.push_back(
                    "stride goal is missing: " + Goal.GoalName);
                return Result;
            }
        }
    }
    return Result;
}

RetargetOpRunResult StrideWarpingOp::Run(
    const NormalizedRuntimeSkeleton&,
    const NormalizedRuntimeSkeleton&,
    const RetargetOpClip& Input)
{
    RetargetOpRunResult Result;
    Result.InputImmutable = true;
    Result.RouteId = OpOptions.RouteId;
    RetargetOpClip Candidate = Input;
    for (RetargetOpFrame& Frame : Candidate.Frames)
    {
        const auto& Pivot = Frame.TargetModelPose[
            static_cast<std::size_t>(
                OpOptions.TargetPivotBoneIndex)];
        Vec3 Forward = OpOptions.ForwardAxisLocal;
        Vec3 Up = OpOptions.UpAxisLocal;
        if (OpOptions.RotateAxesWithPivot)
        {
            Forward = RotateVector(Pivot.Rotation, Forward);
            Up = RotateVector(Pivot.Rotation, Up);
        }
        NormalizeVector(Forward, Forward);
        NormalizeVector(Up, Up);
        Vec3 Right;
        NormalizeVector(Cross(Up, Forward), Right);
        // Re-orthogonalize forward so a slightly imperfect authored basis
        // cannot accumulate vertical drift.
        NormalizeVector(Cross(Right, Up), Forward);

        for (const StrideWarpGoalBinding& Binding : OpOptions.Goals)
        {
            RetargetOpGoal* Goal =
                FindRetargetOpGoal(Frame, Binding.GoalName);
            if (Goal == nullptr)
            {
                Result.Errors.push_back(
                    "stride goal disappeared during execution: " +
                    Binding.GoalName);
                Result.Output = Input;
                Result.FailureLeftInputUnchanged = true;
                return Result;
            }
            const Vec3 Delta = Subtract(
                Goal->TransformModel.TranslationCm,
                Pivot.TranslationCm);
            const double ForwardDistance = Dot(Delta, Forward);
            const double RightDistance = Dot(Delta, Right);
            const double UpDistance = Dot(Delta, Up);
            Vec3 Warped = Pivot.TranslationCm;
            Warped = Add(Warped, Scale(
                Forward, ForwardDistance * OpOptions.WarpForwards));
            Warped = Add(Warped, Scale(
                Right,
                RightDistance * OpOptions.WarpSplay +
                OpOptions.SidewaysOffsetCm * Binding.SideSign));
            Warped = Add(Warped, Scale(Up, UpDistance));
            const double Alpha = std::clamp(
                OpOptions.Alpha * Binding.Alpha, 0.0, 1.0);
            Goal->TransformModel.TranslationCm = Lerp(
                Goal->TransformModel.TranslationCm, Warped, Alpha);
        }
    }
    Result.Success = true;
    Result.OutputModelsRebuilt = true;
    Result.MutationWithinDeclaredChannels = true;
    Result.Output = std::move(Candidate);
    return Result;
}
} // namespace skrtg::retarget::ops
