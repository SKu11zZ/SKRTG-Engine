#pragma once

#include "skrtg/retarget/op_stack.h"

#include <string>
#include <vector>

namespace skrtg::retarget::ops
{
struct StrideWarpGoalBinding
{
    std::string GoalName;
    // -1 for the character's left side, +1 for the right side, 0 for no
    // static sideways offset. The sign is explicit; it is never inferred from
    // a bone or goal name.
    int SideSign = 0;
    double Alpha = 1.0;
};

struct StrideWarpingOptions
{
    std::string RouteId = "stride_warping_goal_space_v1";
    int TargetPivotBoneIndex = -1;
    core::math::Vec3 ForwardAxisLocal{1.0, 0.0, 0.0};
    core::math::Vec3 UpAxisLocal{0.0, 0.0, 1.0};
    bool RotateAxesWithPivot = true;
    double WarpForwards = 1.0;
    double WarpSplay = 1.0;
    double SidewaysOffsetCm = 0.0;
    double Alpha = 1.0;
    std::vector<StrideWarpGoalBinding> Goals;
};

class StrideWarpingOp final : public IRetargetOp
{
public:
    explicit StrideWarpingOp(StrideWarpingOptions Options);

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
    StrideWarpingOptions OpOptions;
};
} // namespace skrtg::retarget::ops
