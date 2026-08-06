#pragma once

#include "skrtg/retarget/op_stack.h"

#include <string>
#include <vector>

namespace skrtg::retarget::ops
{
enum class WeaponAnchorSkeleton
{
    Source,
    Target
};

struct WeaponGoalBinding
{
    std::string Label;
    std::string GoalName;
    WeaponAnchorSkeleton AnchorSkeleton = WeaponAnchorSkeleton::Source;
    int AnchorBoneIndex = -1;
    // Optional target bone represented by the virtual goal. This does not
    // solve or move the bone; the unified goal solver owns that single write.
    int TargetGoalBoneIndex = -1;
    core::math::TransformRT AnchorOffset;
    bool MaintainInputOffset = false;
    double TranslationAlpha = 1.0;
    double RotationAlpha = 1.0;
};

struct WeaponGoalsOptions
{
    std::string RouteId = "weapon_goals_exact_name_v1";
    std::vector<WeaponGoalBinding> Bindings;
};

class WeaponGoalsOp final : public IRetargetOp
{
public:
    explicit WeaponGoalsOp(WeaponGoalsOptions Options);

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
    WeaponGoalsOptions OpOptions;
};

const char* ToString(WeaponAnchorSkeleton Skeleton);
} // namespace skrtg::retarget::ops
