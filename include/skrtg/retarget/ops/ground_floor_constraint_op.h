#pragma once

#include "skrtg/retarget/op_stack.h"

#include <string>
#include <vector>

namespace skrtg::retarget::ops
{
struct GroundFloorGoalBinding
{
    std::string GoalName;
    double ClearanceCm = 0.0;
    double TranslationAlpha = 1.0;
    double RotationAlpha = 0.0;
    // Explicit points on the bottom of the foot, expressed in the goal's
    // local space. Empty means the goal origin is constrained as a point.
    std::vector<core::math::Vec3> FootprintPointsLocalCm;
};

struct GroundFloorConstraintOptions
{
    std::string RouteId = "ground_floor_constraint_explicit_plane_v1";
    core::math::Vec3 PlaneNormalModel{0.0, 0.0, 1.0};
    // Plane equation: dot(normalizedNormal, position) = PlaneDistanceCm.
    double PlaneDistanceCm = 0.0;
    core::math::Vec3 GoalUpAxisLocal{0.0, 0.0, 1.0};
    std::vector<GroundFloorGoalBinding> Goals;
};

class GroundFloorConstraintOp final : public IRetargetOp
{
public:
    explicit GroundFloorConstraintOp(
        GroundFloorConstraintOptions Options);

    RetargetOpDescriptor Descriptor() const override;
    RetargetOpPreflightResult Preflight(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& Input) const override;
    RetargetOpRunResult Run(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& Input) override;

private:
    GroundFloorConstraintOptions OpOptions;
};
} // namespace skrtg::retarget::ops
