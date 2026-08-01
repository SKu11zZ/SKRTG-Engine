#pragma once

#include "skrtg/core/animation/pose.h"
#include "skrtg/core/skeleton/runtime_skeleton.h"

#include <memory>
#include <string>
#include <vector>

namespace skrtg::retarget
{
struct RetargetOpBoneWriteMask
{
    int BoneIndex = -1;
    bool Translation = false;
    bool Rotation = false;
    bool Scale = false;
};

struct RetargetOpDescriptor
{
    std::string TypeId;
    int Version = 1;
    std::string DisplayName;
    bool RequiresWholeClip = true;
    std::vector<RetargetOpBoneWriteMask> DeclaredWrites;
};

struct RetargetOpFrame
{
    int FrameIndex = 0;
    double TimeSeconds = 0.0;
    core::animation::PoseBuffer SourceModelPose;
    core::animation::PoseBuffer TargetLocalPose;
    core::animation::PoseBuffer TargetModelPose;
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

class IRetargetOp
{
public:
    virtual ~IRetargetOp() = default;
    virtual RetargetOpDescriptor Descriptor() const = 0;
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
    bool Enabled = false;
    bool Executed = false;
    bool Success = false;
    bool DisabledExactPassthrough = false;
    bool MutationWithinDeclaredChannels = false;
    bool OutputModelsRebuilt = false;
    bool DeterministicRepeatabilityVerified = false;
    bool FailureLeftInputUnchanged = false;
    std::vector<std::string> Errors;
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

private:
    std::vector<RetargetOpStackEntry> StackEntries;
};

bool EquivalentRetargetOpClips(const RetargetOpClip& Left,
                               const RetargetOpClip& Right);
} // namespace skrtg::retarget
