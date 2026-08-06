#include "skrtg/retarget/ops/ground_floor_constraint_op.h"

#include "skrtg/core/math/transform.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace skrtg::retarget::ops
{
namespace
{
using core::math::Add;
using core::math::Dot;
using core::math::Length;
using core::math::Multiply;
using core::math::MultiplyComponents;
using core::math::Normalize;
using core::math::Quat;
using core::math::RotateVector;
using core::math::Scale;
using core::math::Vec3;

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

Quat Nlerp(Quat A, Quat B, const double Alpha)
{
    const double Product = A.X * B.X + A.Y * B.Y +
        A.Z * B.Z + A.W * B.W;
    if (Product < 0.0)
        B = {-B.X, -B.Y, -B.Z, -B.W};
    return Normalize({
        A.X + (B.X - A.X) * Alpha,
        A.Y + (B.Y - A.Y) * Alpha,
        A.Z + (B.Z - A.Z) * Alpha,
        A.W + (B.W - A.W) * Alpha});
}

bool FromToRotation(const Vec3 FromValue,
                    const Vec3 ToValue,
                    Quat& Out)
{
    Vec3 From;
    Vec3 To;
    if (!NormalizeVector(FromValue, From) ||
        !NormalizeVector(ToValue, To))
    {
        return false;
    }
    const double Product = std::clamp(Dot(From, To), -1.0, 1.0);
    if (Product > 1.0 - 1.0e-10)
    {
        Out = {0.0, 0.0, 0.0, 1.0};
        return true;
    }
    if (Product < -1.0 + 1.0e-10)
    {
        Vec3 Candidate = std::abs(From.X) < 0.75
            ? Vec3{1.0, 0.0, 0.0}
            : Vec3{0.0, 1.0, 0.0};
        Vec3 Axis;
        if (!NormalizeVector(Cross(From, Candidate), Axis)) return false;
        Out = {Axis.X, Axis.Y, Axis.Z, 0.0};
        return true;
    }
    const Vec3 Axis = Cross(From, To);
    Out = Normalize({Axis.X, Axis.Y, Axis.Z, 1.0 + Product});
    return true;
}

double MinimumFootprintDistance(
    const RetargetOpGoal& Goal,
    const GroundFloorGoalBinding& Binding,
    const Vec3 PlaneNormal,
    const double PlaneDistance)
{
    if (Binding.FootprintPointsLocalCm.empty())
    {
        return Dot(PlaneNormal,
                   Goal.TransformModel.TranslationCm) - PlaneDistance;
    }
    double Minimum = std::numeric_limits<double>::infinity();
    for (const Vec3 Point : Binding.FootprintPointsLocalCm)
    {
        const Vec3 Scaled = MultiplyComponents(
            Point, Goal.TransformModel.Scale);
        const Vec3 World = Add(
            Goal.TransformModel.TranslationCm,
            RotateVector(Goal.TransformModel.Rotation, Scaled));
        Minimum = std::min(
            Minimum, Dot(PlaneNormal, World) - PlaneDistance);
    }
    return Minimum;
}
} // namespace

GroundFloorConstraintOp::GroundFloorConstraintOp(
    GroundFloorConstraintOptions Options)
    : OpOptions(std::move(Options))
{
}

RetargetOpDescriptor GroundFloorConstraintOp::Descriptor() const
{
    RetargetOpDescriptor Result;
    Result.TypeId = OpOptions.RouteId;
    Result.Version = 1;
    Result.DisplayName = "Ground / Floor Constraint";
    Result.Phase = RetargetOpPhase::SpatialGoalConstraint;
    for (const GroundFloorGoalBinding& Goal : OpOptions.Goals)
    {
        RetargetOpGoalWriteMask Mask;
        Mask.GoalName = Goal.GoalName;
        Mask.Translation = Goal.TranslationAlpha > 0.0;
        Mask.Rotation = Goal.RotationAlpha > 0.0;
        Result.DeclaredGoalWrites.push_back(std::move(Mask));
    }
    return Result;
}

RetargetOpPreflightResult GroundFloorConstraintOp::Preflight(
    const core::skeleton::NormalizedRuntimeSkeleton&,
    const core::skeleton::NormalizedRuntimeSkeleton&,
    const RetargetOpClip& Input) const
{
    RetargetOpPreflightResult Result;
    Vec3 Normal;
    Vec3 Up;
    if (OpOptions.RouteId.empty() || OpOptions.Goals.empty() ||
        !Finite(OpOptions.PlaneDistanceCm) ||
        !Finite(OpOptions.PlaneNormalModel) ||
        !Finite(OpOptions.GoalUpAxisLocal) ||
        !NormalizeVector(OpOptions.PlaneNormalModel, Normal) ||
        !NormalizeVector(OpOptions.GoalUpAxisLocal, Up))
    {
        Result.Available = false;
        Result.Errors.push_back("ground/floor options are invalid");
        return Result;
    }
    std::set<std::string> Names;
    for (const GroundFloorGoalBinding& Goal : OpOptions.Goals)
    {
        bool FootprintFinite = true;
        for (const Vec3 Point : Goal.FootprintPointsLocalCm)
            FootprintFinite = FootprintFinite && Finite(Point);
        if (Goal.GoalName.empty() ||
            !Names.insert(Goal.GoalName).second ||
            !Finite(Goal.ClearanceCm) || Goal.ClearanceCm < 0.0 ||
            !Finite(Goal.TranslationAlpha) ||
            Goal.TranslationAlpha < 0.0 ||
            Goal.TranslationAlpha > 1.0 ||
            !Finite(Goal.RotationAlpha) ||
            Goal.RotationAlpha < 0.0 || Goal.RotationAlpha > 1.0 ||
            (Goal.TranslationAlpha == 0.0 &&
             Goal.RotationAlpha == 0.0) || !FootprintFinite)
        {
            Result.Available = false;
            Result.Errors.push_back(
                "ground/floor goal binding is invalid or duplicated");
        }
    }
    for (const RetargetOpFrame& Frame : Input.Frames)
    {
        for (const GroundFloorGoalBinding& Goal : OpOptions.Goals)
        {
            if (FindRetargetOpGoal(Frame, Goal.GoalName) == nullptr)
            {
                Result.Available = false;
                Result.Errors.push_back(
                    "floor-constrained goal is missing: " +
                    Goal.GoalName);
                return Result;
            }
        }
    }
    return Result;
}

RetargetOpRunResult GroundFloorConstraintOp::Run(
    const core::skeleton::NormalizedRuntimeSkeleton&,
    const core::skeleton::NormalizedRuntimeSkeleton&,
    const RetargetOpClip& Input)
{
    RetargetOpRunResult Result;
    Result.InputImmutable = true;
    Result.RouteId = OpOptions.RouteId;
    RetargetOpClip Candidate = Input;
    Vec3 PlaneNormal;
    Vec3 GoalUp;
    NormalizeVector(OpOptions.PlaneNormalModel, PlaneNormal);
    NormalizeVector(OpOptions.GoalUpAxisLocal, GoalUp);

    for (RetargetOpFrame& Frame : Candidate.Frames)
    {
        for (const GroundFloorGoalBinding& Binding : OpOptions.Goals)
        {
            RetargetOpGoal* Goal =
                FindRetargetOpGoal(Frame, Binding.GoalName);
            if (Goal == nullptr)
            {
                Result.Errors.push_back(
                    "floor-constrained goal disappeared: " +
                    Binding.GoalName);
                Result.Output = Input;
                Result.FailureLeftInputUnchanged = true;
                return Result;
            }
            if (Binding.RotationAlpha > 0.0)
            {
                const Vec3 CurrentUp = RotateVector(
                    Goal->TransformModel.Rotation, GoalUp);
                Quat Alignment;
                if (!FromToRotation(CurrentUp, PlaneNormal, Alignment))
                {
                    Result.Errors.push_back(
                        "floor orientation alignment failed: " +
                        Binding.GoalName);
                    Result.Output = Input;
                    Result.FailureLeftInputUnchanged = true;
                    return Result;
                }
                const Quat Desired = Multiply(
                    Alignment, Goal->TransformModel.Rotation);
                Goal->TransformModel.Rotation = Nlerp(
                    Goal->TransformModel.Rotation, Desired,
                    Binding.RotationAlpha);
            }
            const double MinimumDistance = MinimumFootprintDistance(
                *Goal, Binding, PlaneNormal,
                OpOptions.PlaneDistanceCm);
            const double Correction = std::max(
                0.0, Binding.ClearanceCm - MinimumDistance);
            Goal->TransformModel.TranslationCm = Add(
                Goal->TransformModel.TranslationCm,
                Scale(PlaneNormal,
                      Correction * Binding.TranslationAlpha));
        }
    }
    Result.Success = true;
    Result.OutputModelsRebuilt = true;
    Result.MutationWithinDeclaredChannels = true;
    Result.Output = std::move(Candidate);
    return Result;
}
} // namespace skrtg::retarget::ops
