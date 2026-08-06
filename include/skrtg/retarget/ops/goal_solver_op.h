#pragma once

#include "skrtg/core/ik/two_bone_pose_buffer_consumer.h"
#include "skrtg/retarget/op_stack.h"

#include <array>
#include <string>
#include <vector>

namespace skrtg::retarget::ops
{
enum class RetargetGoalSolveMode
{
    TwoBone,
    DirectBone
};

struct RetargetGoalSolveBinding
{
    std::string Label;
    std::string GoalName;
    RetargetGoalSolveMode Mode = RetargetGoalSolveMode::TwoBone;

    // Two-bone mode. Indices must describe root -> mid -> end with direct
    // parent links. A configured pole bone is preferred; otherwise the
    // explicit fallback offset is used relative to the chain root.
    std::array<int, 3> TargetChainIndices{{-1, -1, -1}};
    int TargetPoleBoneIndex = -1;
    std::string PoleGoalName;
    core::math::Vec3 PoleFallbackOffsetModelCm{0.0, 0.0, 100.0};

    // Direct-bone mode.
    int TargetBoneIndex = -1;

    bool ApplyGoalTranslation = true;
    bool ApplyGoalRotation = true;
};

struct RetargetGoalSolverOptions
{
    std::string RouteId = "unified_goal_solver_v1";
    std::vector<RetargetGoalSolveBinding> Bindings;
    double SolverEpsilon = 1.0e-9;
    double SolverPositionToleranceCm = 1.0e-6;
    double SolverLengthToleranceCm = 1.0e-6;
};

class RetargetGoalSolverOp final : public IRetargetOp
{
public:
    explicit RetargetGoalSolverOp(RetargetGoalSolverOptions Options);

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
    RetargetGoalSolverOptions OpOptions;
};

const char* ToString(RetargetGoalSolveMode Mode);
} // namespace skrtg::retarget::ops
