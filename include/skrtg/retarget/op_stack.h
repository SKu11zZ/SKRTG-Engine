#pragma once

#include "skrtg/core/animation/pose.h"
#include "skrtg/core/skeleton/runtime_skeleton.h"

#include <memory>
#include <string>
#include <vector>

namespace skrtg::retarget
{
enum class RetargetOpPhase
{
    GoalGeneration = 100,
    GoalWarp = 200,
    TemporalGoalConstraint = 300,
    SpatialGoalConstraint = 350,
    PoseSolve = 400,
    PostSolve = 500
};

enum class RetargetOpRepeatabilityMode
{
    // Compatibility/default audit contract: execute every enabled operator
    // twice and compare the complete result bit-for-bit.
    PerOperatorAudit,
    // Production/iteration contract: execute once. Determinism is expected to
    // have been established by tests or a separate audit run and is reported
    // as "not checked", never fabricated as verified.
    SinglePass
};

struct RetargetOpBoneWriteMask
{
    int BoneIndex = -1;
    bool Translation = false;
    bool Rotation = false;
    bool Scale = false;
};

struct RetargetOpGoalWriteMask
{
    std::string GoalName;
    bool Translation = false;
    bool Rotation = false;
    bool AllowCreate = false;
};

struct RetargetOpDescriptor
{
    std::string TypeId;
    int Version = 1;
    std::string DisplayName;
    RetargetOpPhase Phase = RetargetOpPhase::PostSolve;
    bool RequiresWholeClip = true;
    std::vector<std::string> RequiredEarlierTypeIds;
    std::vector<RetargetOpBoneWriteMask> DeclaredWrites;
    std::vector<RetargetOpGoalWriteMask> DeclaredGoalWrites;
};

struct RetargetOpGoal
{
    std::string Name;
    int TargetBoneIndex = -1;
    core::math::TransformRT TransformModel;
};

struct RetargetOpGoalSeed
{
    std::string GoalName;
    int TargetBoneIndex = -1;
};

struct RetargetOpFrame
{
    int FrameIndex = 0;
    double TimeSeconds = 0.0;
    core::animation::PoseBuffer SourceModelPose;
    core::animation::PoseBuffer TargetLocalPose;
    core::animation::PoseBuffer TargetModelPose;
    std::vector<RetargetOpGoal> Goals;
};

struct RetargetOpClip
{
    std::vector<RetargetOpFrame> Frames;
};

struct RetargetOpRunResult
{
    bool Success = false;
    bool InputImmutable = false;
    bool OutputModelsRebuilt = false;
    bool MutationWithinDeclaredChannels = false;
    bool FailureLeftInputUnchanged = false;
    std::string RouteId;
    RetargetOpClip Output;
    std::vector<std::string> Errors;
};

struct RetargetOpPreflightResult
{
    bool Available = true;
    std::vector<std::string> Warnings;
    std::vector<std::string> Errors;
};

class IRetargetOp
{
public:
    virtual ~IRetargetOp() = default;
    virtual RetargetOpDescriptor Descriptor() const = 0;
    virtual RetargetOpPreflightResult Preflight(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& Input) const;
    virtual RetargetOpRunResult Run(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& Input) = 0;
};

struct RetargetOpStackEntry
{
    std::string InstanceId;
    bool Enabled = true;
    std::unique_ptr<IRetargetOp> Op;
};

struct RetargetOpStackStageResult
{
    std::string InstanceId;
    std::string TypeId;
    int Version = 0;
    RetargetOpPhase Phase = RetargetOpPhase::PostSolve;
    bool Enabled = false;
    bool Executed = false;
    bool PreflightPassed = false;
    bool Success = false;
    bool DisabledExactPassthrough = false;
    bool MutationWithinDeclaredChannels = false;
    bool OutputModelsRebuilt = false;
    bool RepeatabilityCheckPerformed = false;
    bool DeterministicRepeatabilityVerified = false;
    bool FailureLeftInputUnchanged = false;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

struct RetargetOpStackRunOptions
{
    RetargetOpRepeatabilityMode RepeatabilityMode =
        RetargetOpRepeatabilityMode::PerOperatorAudit;
};

struct RetargetOpStackRunResult
{
    bool Success = false;
    bool FoundationInputImmutable = false;
    bool AllDisabledEntriesExactPassthrough = false;
    bool AllExecutedEntriesBounded = false;
    RetargetOpClip FinalOutput;
    std::vector<RetargetOpStackStageResult> Stages;
    std::vector<std::string> Errors;
};

class RetargetOpStack
{
public:
    bool Add(std::string InstanceId,
             std::unique_ptr<IRetargetOp> Op,
             bool Enabled = true);
    bool SetEnabled(const std::string& InstanceId, bool Enabled);
    std::size_t Size() const;
    const std::vector<RetargetOpStackEntry>& Entries() const;

    RetargetOpStackRunResult Run(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& FoundationInput);

    RetargetOpStackRunResult Run(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& FoundationInput,
        const RetargetOpStackRunOptions& Options);

private:
    std::vector<RetargetOpStackEntry> StackEntries;
};

bool EquivalentRetargetOpClips(const RetargetOpClip& Left,
                               const RetargetOpClip& Right);

const RetargetOpGoal* FindRetargetOpGoal(
    const RetargetOpFrame& Frame, const std::string& GoalName);
RetargetOpGoal* FindRetargetOpGoal(
    RetargetOpFrame& Frame, const std::string& GoalName);

bool SeedRetargetOpGoals(
    const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
    const std::vector<RetargetOpGoalSeed>& Seeds,
    RetargetOpClip& InOutClip,
    std::string& OutError);

const char* ToString(RetargetOpPhase Phase);
const char* ToString(RetargetOpRepeatabilityMode Mode);
} // namespace skrtg::retarget
