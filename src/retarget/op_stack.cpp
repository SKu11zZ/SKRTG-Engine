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

bool Equal(const RetargetOpGoal& Left, const RetargetOpGoal& Right)
{
    return Left.Name == Right.Name &&
        Left.TargetBoneIndex == Right.TargetBoneIndex &&
        Equal(Left.TransformModel, Right.TransformModel);
}

bool EqualGoals(const std::vector<RetargetOpGoal>& Left,
                const std::vector<RetargetOpGoal>& Right)
{
    if (Left.size() != Right.size()) return false;
    for (std::size_t Index = 0; Index < Left.size(); ++Index)
        if (!Equal(Left[Index], Right[Index])) return false;
    return true;
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
        std::set<std::string> GoalNames;
        for (const RetargetOpGoal& Goal : Frame.Goals)
        {
            if (Goal.Name.empty() ||
                !GoalNames.insert(Goal.Name).second ||
                Goal.TargetBoneIndex < -1 ||
                Goal.TargetBoneIndex >=
                    static_cast<int>(TargetSkeleton.BoneCount()) ||
                !Finite(Goal.TransformModel))
            {
                return false;
            }
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
    const int Phase = static_cast<int>(Descriptor.Phase);
    if (Descriptor.TypeId.empty() || Descriptor.Version <= 0 ||
        Descriptor.DisplayName.empty() ||
        Phase < static_cast<int>(RetargetOpPhase::GoalGeneration) ||
        Phase > static_cast<int>(RetargetOpPhase::PostSolve))
    {
        return false;
    }
    std::set<std::string> RequiredTypes;
    for (const std::string& TypeId : Descriptor.RequiredEarlierTypeIds)
    {
        if (TypeId.empty() || TypeId == Descriptor.TypeId ||
            !RequiredTypes.insert(TypeId).second)
        {
            return false;
        }
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
    std::set<std::string> GoalNames;
    for (const RetargetOpGoalWriteMask& Mask :
         Descriptor.DeclaredGoalWrites)
    {
        if (Mask.GoalName.empty() ||
            (!Mask.Translation && !Mask.Rotation) ||
            !GoalNames.insert(Mask.GoalName).second)
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
    std::map<std::string, RetargetOpGoalWriteMask> GoalMasks;
    for (const RetargetOpGoalWriteMask& Mask :
         Descriptor.DeclaredGoalWrites)
    {
        GoalMasks.emplace(Mask.GoalName, Mask);
    }
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

        std::map<std::string, const RetargetOpGoal*> BeforeGoals;
        std::map<std::string, const RetargetOpGoal*> AfterGoals;
        for (const RetargetOpGoal& Goal : A.Goals)
            BeforeGoals.emplace(Goal.Name, &Goal);
        for (const RetargetOpGoal& Goal : B.Goals)
            AfterGoals.emplace(Goal.Name, &Goal);
        std::set<std::string> AllGoalNames;
        for (const auto& Pair : BeforeGoals) AllGoalNames.insert(Pair.first);
        for (const auto& Pair : AfterGoals) AllGoalNames.insert(Pair.first);
        for (const std::string& Name : AllGoalNames)
        {
            const auto BeforeIt = BeforeGoals.find(Name);
            const auto AfterIt = AfterGoals.find(Name);
            const auto MaskIt = GoalMasks.find(Name);
            if (BeforeIt == BeforeGoals.end())
            {
                if (AfterIt == AfterGoals.end() ||
                    MaskIt == GoalMasks.end() ||
                    !MaskIt->second.AllowCreate)
                {
                    return false;
                }
                continue;
            }
            if (AfterIt == AfterGoals.end()) return false;
            const RetargetOpGoal& X = *BeforeIt->second;
            const RetargetOpGoal& Y = *AfterIt->second;
            if (MaskIt == GoalMasks.end())
            {
                if (!Equal(X, Y)) return false;
                continue;
            }
            const RetargetOpGoalWriteMask& Mask = MaskIt->second;
            if (X.TargetBoneIndex != Y.TargetBoneIndex ||
                !Equal(X.TransformModel.Scale,
                       Y.TransformModel.Scale) ||
                (!Mask.Translation &&
                 !Equal(X.TransformModel.TranslationCm,
                        Y.TransformModel.TranslationCm)) ||
                (!Mask.Rotation &&
                 !Equal(X.TransformModel.Rotation,
                        Y.TransformModel.Rotation)))
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
        std::set<std::string> GoalNames;
        for (const RetargetOpGoal& Goal : Frame.Goals)
        {
            if (Goal.Name.empty() ||
                !GoalNames.insert(Goal.Name).second ||
                Goal.TargetBoneIndex < -1 ||
                Goal.TargetBoneIndex >=
                    static_cast<int>(TargetSkeleton.BoneCount()) ||
                !Finite(Goal.TransformModel))
            {
                return false;
            }
        }
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
            !EqualPose(A.TargetModelPose, B.TargetModelPose) ||
            !EqualGoals(A.Goals, B.Goals))
        {
            return false;
        }
    }
    return true;
}

RetargetOpPreflightResult IRetargetOp::Preflight(
    const NormalizedRuntimeSkeleton&,
    const NormalizedRuntimeSkeleton&,
    const RetargetOpClip&) const
{
    return {};
}

const RetargetOpGoal* FindRetargetOpGoal(
    const RetargetOpFrame& Frame, const std::string& GoalName)
{
    for (const RetargetOpGoal& Goal : Frame.Goals)
        if (Goal.Name == GoalName) return &Goal;
    return nullptr;
}

RetargetOpGoal* FindRetargetOpGoal(
    RetargetOpFrame& Frame, const std::string& GoalName)
{
    for (RetargetOpGoal& Goal : Frame.Goals)
        if (Goal.Name == GoalName) return &Goal;
    return nullptr;
}

bool SeedRetargetOpGoals(
    const NormalizedRuntimeSkeleton& TargetSkeleton,
    const std::vector<RetargetOpGoalSeed>& Seeds,
    RetargetOpClip& InOutClip,
    std::string& OutError)
{
    std::set<std::string> Names;
    for (const RetargetOpGoalSeed& Seed : Seeds)
    {
        if (Seed.GoalName.empty() ||
            !Names.insert(Seed.GoalName).second ||
            Seed.TargetBoneIndex < 0 ||
            Seed.TargetBoneIndex >=
                static_cast<int>(TargetSkeleton.BoneCount()))
        {
            OutError = "goal seed is invalid or duplicated";
            return false;
        }
    }
    RetargetOpClip Candidate = InOutClip;
    for (RetargetOpFrame& Frame : Candidate.Frames)
    {
        for (const RetargetOpGoalSeed& Seed : Seeds)
        {
            if (FindRetargetOpGoal(Frame, Seed.GoalName) != nullptr)
            {
                OutError = "goal seed collides with an existing goal: " +
                    Seed.GoalName;
                return false;
            }
            RetargetOpGoal Goal;
            Goal.Name = Seed.GoalName;
            Goal.TargetBoneIndex = Seed.TargetBoneIndex;
            Goal.TransformModel = Frame.TargetModelPose[
                static_cast<std::size_t>(Seed.TargetBoneIndex)];
            Frame.Goals.push_back(std::move(Goal));
        }
    }
    InOutClip = std::move(Candidate);
    return true;
}

const char* ToString(const RetargetOpPhase Phase)
{
    switch (Phase)
    {
    case RetargetOpPhase::GoalGeneration:
        return "goal_generation";
    case RetargetOpPhase::GoalWarp:
        return "goal_warp";
    case RetargetOpPhase::TemporalGoalConstraint:
        return "temporal_goal_constraint";
    case RetargetOpPhase::SpatialGoalConstraint:
        return "spatial_goal_constraint";
    case RetargetOpPhase::PoseSolve:
        return "pose_solve";
    case RetargetOpPhase::PostSolve:
        return "post_solve";
    default:
        return "unknown";
    }
}

const char* ToString(const RetargetOpRepeatabilityMode Mode)
{
    switch (Mode)
    {
    case RetargetOpRepeatabilityMode::PerOperatorAudit:
        return "per_operator_audit";
    case RetargetOpRepeatabilityMode::SinglePass:
        return "single_pass";
    default:
        return "unknown";
    }
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
    return Run(SourceSkeleton, TargetSkeleton, FoundationInput, {});
}

RetargetOpStackRunResult RetargetOpStack::Run(
    const NormalizedRuntimeSkeleton& SourceSkeleton,
    const NormalizedRuntimeSkeleton& TargetSkeleton,
    const RetargetOpClip& FoundationInput,
    const RetargetOpStackRunOptions& Options)
{
    RetargetOpStackRunResult Result;
    Result.FinalOutput = FoundationInput;
    const bool AuditRepeatability =
        Options.RepeatabilityMode ==
        RetargetOpRepeatabilityMode::PerOperatorAudit;
    const RetargetOpClip FoundationSnapshot = AuditRepeatability
        ? FoundationInput
        : RetargetOpClip{};
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
    int PreviousEnabledPhase =
        static_cast<int>(RetargetOpPhase::GoalGeneration);
    std::set<std::string> SuccessfulTypeIds;
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
        Stage.Phase = Descriptor.Phase;
        const RetargetOpClip StageInput = Result.FinalOutput;
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

        const int Phase = static_cast<int>(Descriptor.Phase);
        if (Phase < PreviousEnabledPhase)
        {
            Stage.Errors.push_back(
                "operator phase appears after a later phase");
            Result.Errors.push_back(
                Entry.InstanceId + ": invalid operator phase ordering");
            ExecutedBounded = false;
            Result.Stages.push_back(std::move(Stage));
            break;
        }
        bool DependenciesSatisfied = true;
        for (const std::string& RequiredType :
             Descriptor.RequiredEarlierTypeIds)
        {
            if (SuccessfulTypeIds.find(RequiredType) ==
                SuccessfulTypeIds.end())
            {
                Stage.Errors.push_back(
                    "required earlier operator is unavailable: " +
                    RequiredType);
                DependenciesSatisfied = false;
            }
        }
        if (!DependenciesSatisfied)
        {
            Result.Errors.push_back(
                Entry.InstanceId + ": operator dependency check failed");
            ExecutedBounded = false;
            Result.Stages.push_back(std::move(Stage));
            break;
        }
        const RetargetOpPreflightResult Preflight =
            Entry.Op->Preflight(
                SourceSkeleton, TargetSkeleton, StageInput);
        Stage.PreflightPassed = Preflight.Available &&
            Preflight.Errors.empty();
        Stage.Warnings = Preflight.Warnings;
        Stage.Errors.insert(
            Stage.Errors.end(), Preflight.Errors.begin(),
            Preflight.Errors.end());
        if (!Stage.PreflightPassed)
        {
            Result.Errors.push_back(
                Entry.InstanceId + ": operator is unavailable for input");
            ExecutedBounded = false;
            Result.Stages.push_back(std::move(Stage));
            break;
        }

        Stage.Executed = true;
        RetargetOpRunResult OpResult = Entry.Op->Run(
            SourceSkeleton, TargetSkeleton, StageInput);
        Stage.RepeatabilityCheckPerformed = AuditRepeatability;
        Stage.DeterministicRepeatabilityVerified = false;
        bool InputImmutable = true;
        if (AuditRepeatability)
        {
            const RetargetOpClip StageInputSnapshot = StageInput;
            const RetargetOpRunResult RepeatResult = Entry.Op->Run(
                SourceSkeleton, TargetSkeleton, StageInputSnapshot);
            Stage.DeterministicRepeatabilityVerified =
                EquivalentRunResults(OpResult, RepeatResult);
            InputImmutable =
                EquivalentRetargetOpClips(StageInput,
                                          StageInputSnapshot) &&
                EquivalentRetargetOpClips(Result.FinalOutput,
                                          StageInputSnapshot);
        }
        const bool RepeatabilitySatisfied = !AuditRepeatability ||
            Stage.DeterministicRepeatabilityVerified;
        const bool Scope = OpResult.Success &&
            RepeatabilitySatisfied && InputImmutable &&
            OpResult.InputImmutable &&
            MutationWithin(StageInput, OpResult.Output, Descriptor);
        const bool Models = OpResult.Success &&
            OutputModelsValid(TargetSkeleton, OpResult.Output);
        Stage.MutationWithinDeclaredChannels = Scope;
        Stage.OutputModelsRebuilt = Models;
        Stage.Success = OpResult.Success &&
            RepeatabilitySatisfied && Scope && Models;
        Stage.FailureLeftInputUnchanged =
            OpResult.FailureLeftInputUnchanged;
        Stage.Errors = OpResult.Errors;
        if (AuditRepeatability &&
            !Stage.DeterministicRepeatabilityVerified)
        {
            Stage.Errors.push_back(
                "operator repeatability check failed");
        }
        if (!InputImmutable || !OpResult.InputImmutable)
        {
            Stage.Errors.push_back(
                "operator did not satisfy the immutable-input contract");
        }
        if (OpResult.Success && !Scope)
        {
            Stage.Errors.push_back(
                "operator wrote outside its declared bone/goal channels");
        }
        if (OpResult.Success && !Models)
        {
            Stage.Errors.push_back(
                "operator output failed model-pose rebuild validation");
        }
        if (!OpResult.Success && OpResult.Errors.empty())
        {
            Stage.Errors.push_back(
                "operator reported failure without diagnostic details");
        }
        if (!Stage.Success)
        {
            if (!EquivalentRetargetOpClips(StageInput,
                                           Result.FinalOutput))
            {
                Result.FinalOutput = StageInput;
            }
            Result.Errors.push_back(
                Entry.InstanceId + ": operator failed bounded execution");
            for (const std::string& Error : Stage.Errors)
                Result.Errors.push_back(Entry.InstanceId + ": " + Error);
            ExecutedBounded = false;
            Result.Stages.push_back(std::move(Stage));
            break;
        }
        Result.FinalOutput = std::move(OpResult.Output);
        PreviousEnabledPhase = Phase;
        SuccessfulTypeIds.insert(Descriptor.TypeId);
        Result.Stages.push_back(std::move(Stage));
    }

    Result.FoundationInputImmutable = !AuditRepeatability ||
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
