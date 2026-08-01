#include "skrtg/retarget/op_stack.h"

#include "skrtg/core/math/transform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace skrtg::retarget
{
namespace
{
using core::animation::PoseBuffer;
using core::animation::PoseSpace;
using core::math::Compose;
using core::math::NearlyEqual;
using core::math::Quat;
using core::math::TransformRT;
using core::math::Vec3;
using core::skeleton::NormalizedRuntimeSkeleton;

bool Finite(double Value) { return std::isfinite(Value); }
bool Finite(Vec3 Value)
{
    return Finite(Value.X) && Finite(Value.Y) && Finite(Value.Z);
}
bool Finite(Quat Value)
{
    return Finite(Value.X) && Finite(Value.Y) &&
        Finite(Value.Z) && Finite(Value.W);
}
bool Finite(TransformRT Value)
{
    return Finite(Value.TranslationCm) && Finite(Value.Rotation) &&
        Finite(Value.Scale);
}

bool Equal(double Left, double Right)
{
    return std::memcmp(&Left, &Right, sizeof(double)) == 0;
}

bool Equal(Vec3 Left, Vec3 Right)
{
    return Equal(Left.X, Right.X) && Equal(Left.Y, Right.Y) &&
        Equal(Left.Z, Right.Z);
}
bool Equal(Quat Left, Quat Right)
{
    return Equal(Left.X, Right.X) && Equal(Left.Y, Right.Y) &&
        Equal(Left.Z, Right.Z) && Equal(Left.W, Right.W);
}
bool Equal(TransformRT Left, TransformRT Right)
{
    return Equal(Left.TranslationCm, Right.TranslationCm) &&
        Equal(Left.Rotation, Right.Rotation) &&
        Equal(Left.Scale, Right.Scale);
}
bool EqualPose(const PoseBuffer& Left, const PoseBuffer& Right)
{
    if (Left.Space() != Right.Space() ||
        Left.SkeletonHash() != Right.SkeletonHash() ||
        Left.Size() != Right.Size())
    {
        return false;
    }
    for (std::size_t Index = 0; Index < Left.Size(); ++Index)
        if (!Equal(Left[Index], Right[Index])) return false;
    return true;
}

bool BuildModelPose(const NormalizedRuntimeSkeleton& Skeleton,
                    const PoseBuffer& LocalPose,
                    PoseBuffer& OutModelPose)
{
    if (LocalPose.Space() != PoseSpace::Local ||
        !LocalPose.IsSizedFor(Skeleton) ||
        !Skeleton.ValidateParentIndexInvariant())
    {
        return false;
    }
    PoseBuffer Model(PoseSpace::Model,
                     Skeleton.GetIdentity().HierarchyHash);
    Model.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        if (!Finite(LocalPose[Index])) return false;
        const int Parent = Skeleton.BoneAt(Index).ParentIndex;
        Model[Index] = Parent < 0
            ? LocalPose[Index]
            : Compose(Model[static_cast<std::size_t>(Parent)],
                      LocalPose[Index]);
        if (!Finite(Model[Index])) return false;
    }
    OutModelPose = std::move(Model);
    return true;
}

bool ModelMatches(const PoseBuffer& Left, const PoseBuffer& Right)
{
    if (Left.Space() != PoseSpace::Model ||
        Right.Space() != PoseSpace::Model ||
        Left.SkeletonHash() != Right.SkeletonHash() ||
        Left.Size() != Right.Size())
    {
        return false;
    }
    for (std::size_t Index = 0; Index < Left.Size(); ++Index)
    {
        if (!NearlyEqual(Left[Index], Right[Index],
                         1.0e-6, 1.0e-9, 1.0e-9))
        {
            return false;
        }
    }
    return true;
}

bool ValidateClipShape(const NormalizedRuntimeSkeleton& SourceSkeleton,
                       const NormalizedRuntimeSkeleton& TargetSkeleton,
                       const RetargetOpClip& Clip)
{
    int PreviousFrame = std::numeric_limits<int>::min();
    for (const RetargetOpFrame& Frame : Clip.Frames)
    {
        if (Frame.FrameIndex <= PreviousFrame ||
            !Finite(Frame.TimeSeconds) ||
            Frame.SourceModelPose.Space() != PoseSpace::Model ||
            Frame.TargetLocalPose.Space() != PoseSpace::Local ||
            Frame.TargetModelPose.Space() != PoseSpace::Model ||
            !Frame.SourceModelPose.IsSizedFor(SourceSkeleton) ||
            !Frame.TargetLocalPose.IsSizedFor(TargetSkeleton) ||
            !Frame.TargetModelPose.IsSizedFor(TargetSkeleton))
        {
            return false;
        }
        for (std::size_t Index = 0;
             Index < Frame.SourceModelPose.Size(); ++Index)
        {
            if (!Finite(Frame.SourceModelPose[Index])) return false;
        }
        PoseBuffer Rebuilt;
        if (!BuildModelPose(TargetSkeleton,
                            Frame.TargetLocalPose, Rebuilt) ||
            !ModelMatches(Rebuilt, Frame.TargetModelPose))
        {
            return false;
        }
        PreviousFrame = Frame.FrameIndex;
    }
    return !Clip.Frames.empty();
}

bool DescriptorValid(const NormalizedRuntimeSkeleton& TargetSkeleton,
                     const RetargetOpDescriptor& Descriptor)
{
    if (Descriptor.TypeId.empty() || Descriptor.Version <= 0 ||
        Descriptor.DisplayName.empty())
    {
        return false;
    }
    std::set<int> Indices;
    for (const RetargetOpBoneWriteMask& Mask : Descriptor.DeclaredWrites)
    {
        if (Mask.BoneIndex < 0 ||
            Mask.BoneIndex >= static_cast<int>(TargetSkeleton.BoneCount()) ||
            (!Mask.Translation && !Mask.Rotation && !Mask.Scale) ||
            !Indices.insert(Mask.BoneIndex).second)
        {
            return false;
        }
    }
    return true;
}

bool MutationWithin(const RetargetOpClip& Before,
                    const RetargetOpClip& After,
                    const RetargetOpDescriptor& Descriptor)
{
    if (Before.Frames.size() != After.Frames.size()) return false;
    std::map<int, RetargetOpBoneWriteMask> Masks;
    for (const RetargetOpBoneWriteMask& Mask : Descriptor.DeclaredWrites)
        Masks.emplace(Mask.BoneIndex, Mask);
    for (std::size_t FrameIndex = 0;
         FrameIndex < Before.Frames.size(); ++FrameIndex)
    {
        const RetargetOpFrame& A = Before.Frames[FrameIndex];
        const RetargetOpFrame& B = After.Frames[FrameIndex];
        if (A.FrameIndex != B.FrameIndex ||
            A.TimeSeconds != B.TimeSeconds ||
            !EqualPose(A.SourceModelPose, B.SourceModelPose) ||
            A.TargetLocalPose.Size() != B.TargetLocalPose.Size())
        {
            return false;
        }
        for (std::size_t BoneIndex = 0;
             BoneIndex < A.TargetLocalPose.Size(); ++BoneIndex)
        {
            const TransformRT& X = A.TargetLocalPose[BoneIndex];
            const TransformRT& Y = B.TargetLocalPose[BoneIndex];
            const auto It = Masks.find(static_cast<int>(BoneIndex));
            const RetargetOpBoneWriteMask Empty;
            const RetargetOpBoneWriteMask& Mask =
                It == Masks.end() ? Empty : It->second;
            if ((!Mask.Translation && !Equal(X.TranslationCm, Y.TranslationCm)) ||
                (!Mask.Rotation && !Equal(X.Rotation, Y.Rotation)) ||
                (!Mask.Scale && !Equal(X.Scale, Y.Scale)))
            {
                return false;
            }
        }
    }
    return true;
}

bool OutputModelsValid(const NormalizedRuntimeSkeleton& TargetSkeleton,
                       const RetargetOpClip& Clip)
{
    for (const RetargetOpFrame& Frame : Clip.Frames)
    {
        PoseBuffer Rebuilt;
        if (!BuildModelPose(TargetSkeleton,
                            Frame.TargetLocalPose, Rebuilt) ||
            !ModelMatches(Rebuilt, Frame.TargetModelPose))
        {
            return false;
        }
    }
    return true;
}

bool EquivalentRunResults(const RetargetOpRunResult& Left,
                          const RetargetOpRunResult& Right)
{
    return Left.Success == Right.Success &&
        Left.InputImmutable == Right.InputImmutable &&
        Left.OutputModelsRebuilt == Right.OutputModelsRebuilt &&
        Left.MutationWithinDeclaredChannels ==
            Right.MutationWithinDeclaredChannels &&
        Left.FailureLeftInputUnchanged ==
            Right.FailureLeftInputUnchanged &&
        Left.RouteId == Right.RouteId &&
        EquivalentRetargetOpClips(Left.Output, Right.Output) &&
        Left.Errors == Right.Errors;
}
} // namespace

bool EquivalentRetargetOpClips(const RetargetOpClip& Left,
                               const RetargetOpClip& Right)
{
    if (Left.Frames.size() != Right.Frames.size()) return false;
    for (std::size_t Index = 0; Index < Left.Frames.size(); ++Index)
    {
        const RetargetOpFrame& A = Left.Frames[Index];
        const RetargetOpFrame& B = Right.Frames[Index];
        if (A.FrameIndex != B.FrameIndex ||
            A.TimeSeconds != B.TimeSeconds ||
            !EqualPose(A.SourceModelPose, B.SourceModelPose) ||
            !EqualPose(A.TargetLocalPose, B.TargetLocalPose) ||
            !EqualPose(A.TargetModelPose, B.TargetModelPose))
        {
            return false;
        }
    }
    return true;
}

bool RetargetOpStack::Add(std::string InstanceId,
                          std::unique_ptr<IRetargetOp> Op,
                          bool Enabled)
{
    if (InstanceId.empty() || !Op) return false;
    for (const RetargetOpStackEntry& Entry : StackEntries)
        if (Entry.InstanceId == InstanceId) return false;
    RetargetOpStackEntry Entry;
    Entry.InstanceId = std::move(InstanceId);
    Entry.Enabled = Enabled;
    Entry.Op = std::move(Op);
    StackEntries.push_back(std::move(Entry));
    return true;
}

bool RetargetOpStack::SetEnabled(const std::string& InstanceId,
                                 bool Enabled)
{
    for (RetargetOpStackEntry& Entry : StackEntries)
    {
        if (Entry.InstanceId == InstanceId)
        {
            Entry.Enabled = Enabled;
            return true;
        }
    }
    return false;
}

std::size_t RetargetOpStack::Size() const { return StackEntries.size(); }

const std::vector<RetargetOpStackEntry>& RetargetOpStack::Entries() const
{
    return StackEntries;
}

RetargetOpStackRunResult RetargetOpStack::Run(
    const NormalizedRuntimeSkeleton& SourceSkeleton,
    const NormalizedRuntimeSkeleton& TargetSkeleton,
    const RetargetOpClip& FoundationInput)
{
    RetargetOpStackRunResult Result;
    Result.FinalOutput = FoundationInput;
    const RetargetOpClip FoundationSnapshot = FoundationInput;
    if (!ValidateClipShape(SourceSkeleton, TargetSkeleton,
                           FoundationInput))
    {
        Result.Errors.push_back(
            "foundation clip does not satisfy source/model/local pose contracts");
        Result.FoundationInputImmutable = true;
        return Result;
    }

    bool DisabledExact = true;
    bool ExecutedBounded = true;
    for (RetargetOpStackEntry& Entry : StackEntries)
    {
        RetargetOpStackStageResult Stage;
        Stage.InstanceId = Entry.InstanceId;
        Stage.Enabled = Entry.Enabled;
        if (!Entry.Op)
        {
            Stage.Errors.push_back("stack entry has no operator instance");
            Result.Errors.push_back(Entry.InstanceId +
                                    ": missing operator instance");
            ExecutedBounded = false;
            Result.Stages.push_back(std::move(Stage));
            break;
        }
        const RetargetOpDescriptor Descriptor = Entry.Op->Descriptor();
        Stage.TypeId = Descriptor.TypeId;
        Stage.Version = Descriptor.Version;
        const RetargetOpClip StageInput = Result.FinalOutput;
        const RetargetOpClip StageInputSnapshot = StageInput;
        if (!Entry.Enabled)
        {
            Stage.Success = true;
            Stage.DisabledExactPassthrough =
                EquivalentRetargetOpClips(StageInput,
                                          Result.FinalOutput);
            DisabledExact = DisabledExact &&
                Stage.DisabledExactPassthrough;
            Result.Stages.push_back(std::move(Stage));
            continue;
        }
        if (!DescriptorValid(TargetSkeleton, Descriptor))
        {
            Stage.Errors.push_back("operator descriptor or write mask is invalid");
            Result.Errors.push_back(Entry.InstanceId +
                                    ": invalid descriptor");
            ExecutedBounded = false;
            Result.Stages.push_back(std::move(Stage));
            break;
        }

        Stage.Executed = true;
        RetargetOpRunResult OpResult = Entry.Op->Run(
            SourceSkeleton, TargetSkeleton, StageInput);
        const RetargetOpRunResult RepeatResult = Entry.Op->Run(
            SourceSkeleton, TargetSkeleton, StageInputSnapshot);
        Stage.DeterministicRepeatabilityVerified =
            EquivalentRunResults(OpResult, RepeatResult);
        const bool InputImmutable =
            EquivalentRetargetOpClips(StageInput,
                                      StageInputSnapshot) &&
            EquivalentRetargetOpClips(Result.FinalOutput,
                                      StageInputSnapshot);
        const bool Scope = OpResult.Success &&
            Stage.DeterministicRepeatabilityVerified && InputImmutable &&
            OpResult.InputImmutable &&
            MutationWithin(StageInput, OpResult.Output, Descriptor);
        const bool Models = OpResult.Success &&
            OutputModelsValid(TargetSkeleton, OpResult.Output);
        Stage.MutationWithinDeclaredChannels = Scope;
        Stage.OutputModelsRebuilt = Models;
        Stage.Success = OpResult.Success &&
            Stage.DeterministicRepeatabilityVerified && Scope && Models;
        Stage.FailureLeftInputUnchanged =
            OpResult.FailureLeftInputUnchanged;
        Stage.Errors = OpResult.Errors;
        if (!Stage.DeterministicRepeatabilityVerified)
            Stage.Errors.push_back(
                "operator repeatability check failed");
        if (!Stage.Success)
        {
            if (!EquivalentRetargetOpClips(StageInput,
                                           Result.FinalOutput))
            {
                Result.FinalOutput = StageInput;
            }
            Result.Errors.push_back(
                Entry.InstanceId + ": operator failed bounded execution");
            for (const std::string& Error : OpResult.Errors)
                Result.Errors.push_back(Entry.InstanceId + ": " + Error);
            ExecutedBounded = false;
            Result.Stages.push_back(std::move(Stage));
            break;
        }
        Result.FinalOutput = std::move(OpResult.Output);
        Result.Stages.push_back(std::move(Stage));
    }

    Result.FoundationInputImmutable =
        EquivalentRetargetOpClips(FoundationInput,
                                  FoundationSnapshot);
    Result.AllDisabledEntriesExactPassthrough = DisabledExact;
    Result.AllExecutedEntriesBounded = ExecutedBounded;
    Result.Success = Result.FoundationInputImmutable &&
        Result.AllDisabledEntriesExactPassthrough &&
        Result.AllExecutedEntriesBounded &&
        Result.Stages.size() == StackEntries.size();
    return Result;
}
} // namespace skrtg::retarget
