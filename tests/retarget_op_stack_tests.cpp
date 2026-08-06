#include "skrtg/retarget/op_stack.h"

#include "skrtg/core/math/transform.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace
{
using skrtg::core::animation::PoseBuffer;
using skrtg::core::animation::PoseSpace;
using skrtg::core::math::Compose;
using skrtg::core::math::FromAxisAngleDegrees;
using skrtg::core::math::IdentityTransform;
using skrtg::core::skeleton::NormalizedRuntimeSkeleton;
using skrtg::core::skeleton::RuntimeBone;
using skrtg::core::skeleton::SkeletonIdentity;
using skrtg::retarget::EquivalentRetargetOpClips;
using skrtg::retarget::IRetargetOp;
using skrtg::retarget::RetargetOpBoneWriteMask;
using skrtg::retarget::RetargetOpClip;
using skrtg::retarget::RetargetOpDescriptor;
using skrtg::retarget::RetargetOpFrame;
using skrtg::retarget::RetargetOpPhase;
using skrtg::retarget::RetargetOpRepeatabilityMode;
using skrtg::retarget::RetargetOpRunResult;
using skrtg::retarget::RetargetOpStack;
using skrtg::retarget::RetargetOpStackRunOptions;

int Failures = 0;

void Check(bool Condition, const std::string& Message)
{
    if (Condition) return;
    ++Failures;
    std::cerr << "FAIL: " << Message << '\n';
}

NormalizedRuntimeSkeleton MakeSkeleton(const std::string& Label)
{
    NormalizedRuntimeSkeleton Skeleton;
    SkeletonIdentity Identity;
    Identity.HierarchyHash = "op-stack-" + Label + "-hierarchy";
    Identity.RestPoseHash = "op-stack-" + Label + "-rest";
    Identity.SourceAssetId = "synthetic";
    Skeleton.SetIdentity(Identity);

    RuntimeBone Hips;
    Hips.Name = "Hips";
    Hips.RawPath = "synthetic/Hips";
    Hips.ParentIndex = -1;
    Hips.RawIndex = 0;
    Hips.LocalRest = IdentityTransform();
    Hips.LocalRest.TranslationCm = {0.0, 90.0, 0.0};
    Skeleton.AddBone(Hips);
    return Skeleton;
}

PoseBuffer Local(const NormalizedRuntimeSkeleton& Skeleton)
{
    PoseBuffer Result(PoseSpace::Local,
                      Skeleton.GetIdentity().HierarchyHash);
    Result.ResizeToSkeleton(Skeleton);
    Result[0] = Skeleton.BoneAt(0).LocalRest;
    return Result;
}

PoseBuffer Model(const NormalizedRuntimeSkeleton& Skeleton,
                 const PoseBuffer& LocalPose)
{
    PoseBuffer Result(PoseSpace::Model,
                      Skeleton.GetIdentity().HierarchyHash);
    Result.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        const int Parent = Skeleton.BoneAt(Index).ParentIndex;
        Result[Index] = Parent < 0
            ? LocalPose[Index]
            : Compose(Result[static_cast<std::size_t>(Parent)],
                      LocalPose[Index]);
    }
    return Result;
}

RetargetOpClip MakeClip(const NormalizedRuntimeSkeleton& Source,
                        const NormalizedRuntimeSkeleton& Target)
{
    RetargetOpClip Clip;
    for (int FrameIndex = 0; FrameIndex < 2; ++FrameIndex)
    {
        PoseBuffer SourceLocal = Local(Source);
        SourceLocal[0].TranslationCm.X += FrameIndex * 0.5;
        PoseBuffer TargetLocal = Local(Target);
        TargetLocal[0].TranslationCm.Z += FrameIndex * 0.25;
        RetargetOpFrame Frame;
        Frame.FrameIndex = FrameIndex;
        Frame.TimeSeconds = FrameIndex / 30.0;
        Frame.SourceModelPose = Model(Source, SourceLocal);
        Frame.TargetLocalPose = TargetLocal;
        Frame.TargetModelPose = Model(Target, TargetLocal);
        Clip.Frames.push_back(std::move(Frame));
    }
    return Clip;
}

class NoOp final : public IRetargetOp
{
public:
    RetargetOpDescriptor Descriptor() const override
    {
        RetargetOpDescriptor Result;
        Result.TypeId = "synthetic_no_op_v1";
        Result.Version = 1;
        Result.DisplayName = "Synthetic No Op";
        return Result;
    }

    RetargetOpRunResult Run(
        const NormalizedRuntimeSkeleton&,
        const NormalizedRuntimeSkeleton&,
        const RetargetOpClip& Input) override
    {
        RetargetOpRunResult Result;
        Result.Success = true;
        Result.InputImmutable = true;
        Result.OutputModelsRebuilt = true;
        Result.MutationWithinDeclaredChannels = true;
        Result.RouteId = "synthetic_no_op_v1";
        Result.Output = Input;
        return Result;
    }
};

class RotationOp final : public IRetargetOp
{
public:
    explicit RotationOp(bool ViolateScope) : Violate(ViolateScope) {}

    RetargetOpDescriptor Descriptor() const override
    {
        RetargetOpDescriptor Result;
        Result.TypeId = Violate
            ? "synthetic_scope_violation_v1"
            : "synthetic_declared_rotation_v1";
        Result.Version = 1;
        Result.DisplayName = "Synthetic Rotation";
        RetargetOpBoneWriteMask Mask;
        Mask.BoneIndex = 0;
        Mask.Rotation = true;
        Result.DeclaredWrites.push_back(Mask);
        return Result;
    }

    RetargetOpRunResult Run(
        const NormalizedRuntimeSkeleton&,
        const NormalizedRuntimeSkeleton& Target,
        const RetargetOpClip& Input) override
    {
        RetargetOpRunResult Result;
        Result.Success = true;
        Result.InputImmutable = true;
        Result.OutputModelsRebuilt = true;
        Result.MutationWithinDeclaredChannels = true;
        Result.Output = Input;
        if (Violate)
            Result.Output.Frames[1].TargetLocalPose[0].TranslationCm.X += 1.0;
        else
            Result.Output.Frames[1].TargetLocalPose[0].Rotation =
                FromAxisAngleDegrees({0.0, 1.0, 0.0}, 17.0);
        Result.Output.Frames[1].TargetModelPose = Model(
            Target, Result.Output.Frames[1].TargetLocalPose);
        return Result;
    }

private:
    bool Violate = false;
};

class InvalidDescriptorOp final : public IRetargetOp
{
public:
    RetargetOpDescriptor Descriptor() const override
    {
        return {};
    }

    RetargetOpRunResult Run(
        const NormalizedRuntimeSkeleton&,
        const NormalizedRuntimeSkeleton&,
        const RetargetOpClip& Input) override
    {
        ++RunCount;
        RetargetOpRunResult Result;
        Result.Success = true;
        Result.InputImmutable = true;
        Result.OutputModelsRebuilt = true;
        Result.MutationWithinDeclaredChannels = true;
        Result.Output = Input;
        return Result;
    }

    int RunCount = 0;
};

class NonDeterministicOp final : public IRetargetOp
{
public:
    RetargetOpDescriptor Descriptor() const override
    {
        RetargetOpDescriptor Result;
        Result.TypeId = "synthetic_nondeterministic_v1";
        Result.Version = 1;
        Result.DisplayName = "Synthetic Non-Deterministic Op";
        RetargetOpBoneWriteMask Mask;
        Mask.BoneIndex = 0;
        Mask.Rotation = true;
        Result.DeclaredWrites.push_back(Mask);
        return Result;
    }

    RetargetOpRunResult Run(
        const NormalizedRuntimeSkeleton&,
        const NormalizedRuntimeSkeleton& Target,
        const RetargetOpClip& Input) override
    {
        RetargetOpRunResult Result;
        Result.Success = true;
        Result.InputImmutable = true;
        Result.OutputModelsRebuilt = true;
        Result.MutationWithinDeclaredChannels = true;
        Result.RouteId = "synthetic_nondeterministic_v1";
        Result.Output = Input;
        const double Angle = (++RunCount % 2 == 0) ? 23.0 : 11.0;
        Result.Output.Frames[1].TargetLocalPose[0].Rotation =
            FromAxisAngleDegrees({0.0, 1.0, 0.0}, Angle);
        Result.Output.Frames[1].TargetModelPose = Model(
            Target, Result.Output.Frames[1].TargetLocalPose);
        return Result;
    }

private:
    int RunCount = 0;
};

class CountingPhaseOp final : public IRetargetOp
{
public:
    CountingPhaseOp(RetargetOpPhase PhaseValue, int& CounterValue)
        : Phase(PhaseValue), Counter(CounterValue)
    {
    }

    RetargetOpDescriptor Descriptor() const override
    {
        RetargetOpDescriptor Result;
        Result.TypeId = Phase == RetargetOpPhase::GoalGeneration
            ? "synthetic_goal_generation_v1"
            : "synthetic_goal_warp_v1";
        Result.Version = 1;
        Result.DisplayName = "Synthetic Phase Op";
        Result.Phase = Phase;
        return Result;
    }

    RetargetOpRunResult Run(
        const NormalizedRuntimeSkeleton&,
        const NormalizedRuntimeSkeleton&,
        const RetargetOpClip& Input) override
    {
        ++Counter;
        RetargetOpRunResult Result;
        Result.Success = true;
        Result.InputImmutable = true;
        Result.OutputModelsRebuilt = true;
        Result.MutationWithinDeclaredChannels = true;
        Result.RouteId = Descriptor().TypeId;
        Result.Output = Input;
        return Result;
    }

private:
    RetargetOpPhase Phase;
    int& Counter;
};

void TestDisabledExactPassthrough()
{
    const auto Source = MakeSkeleton("source");
    const auto Target = MakeSkeleton("target");
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    RetargetOpStack Stack;
    Check(Stack.Add("noop", std::make_unique<NoOp>(), false),
          "disabled no-op could not be added");
    Check(!Stack.Add("noop", std::make_unique<NoOp>(), false),
          "duplicate instance id was accepted");
    const auto Run = Stack.Run(Source, Target, Foundation);
    Check(Run.Success && Run.FoundationInputImmutable &&
              Run.AllDisabledEntriesExactPassthrough &&
              Run.Stages.size() == 1 && !Run.Stages[0].Executed &&
              Run.Stages[0].DisabledExactPassthrough,
          "disabled operator was not an exact bounded passthrough");
    Check(EquivalentRetargetOpClips(Foundation, Run.FinalOutput),
          "disabled operator changed the foundation clip");
}

void TestDisabledInvalidDescriptorStillPassthrough()
{
    const auto Source = MakeSkeleton("source-disabled-invalid");
    const auto Target = MakeSkeleton("target-disabled-invalid");
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    RetargetOpStack Stack;
    auto Invalid = std::make_unique<InvalidDescriptorOp>();
    InvalidDescriptorOp* InvalidRaw = Invalid.get();
    Check(Stack.Add("invalid_disabled", std::move(Invalid), false),
          "disabled invalid-descriptor op could not be added");
    const auto Run = Stack.Run(Source, Target, Foundation);
    Check(Run.Success && Run.Stages.size() == 1 &&
              !Run.Stages[0].Executed &&
              Run.Stages[0].DisabledExactPassthrough &&
              InvalidRaw->RunCount == 0 &&
              EquivalentRetargetOpClips(Foundation, Run.FinalOutput),
          "disabled invalid descriptor was validated or executed");
}

void TestDeclaredScopeAndOrdering()
{
    const auto Source = MakeSkeleton("source-order");
    const auto Target = MakeSkeleton("target-order");
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    RetargetOpStack Stack;
    Check(Stack.Add("noop", std::make_unique<NoOp>(), true),
          "enabled no-op could not be added");
    Check(Stack.Add("rotation", std::make_unique<RotationOp>(false), true),
          "declared rotation op could not be added");
    const auto Run = Stack.Run(Source, Target, Foundation);
    Check(Run.Success && Run.Stages.size() == 2 &&
              Run.Stages[0].Success && Run.Stages[1].Success &&
              Run.Stages[1].MutationWithinDeclaredChannels &&
              Run.Stages[1].OutputModelsRebuilt,
          "ordered enabled operators did not satisfy bounded execution");
    Check(!EquivalentRetargetOpClips(Foundation, Run.FinalOutput),
          "declared rotation op did not produce an output change");
    Check(EquivalentRetargetOpClips(
              Foundation, MakeClip(Source, Target)),
          "foundation input was mutated by enabled operators");
}

void TestUndeclaredMutationRejected()
{
    const auto Source = MakeSkeleton("source-reject");
    const auto Target = MakeSkeleton("target-reject");
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    RetargetOpStack Stack;
    Check(Stack.Add("scope_violation",
                    std::make_unique<RotationOp>(true), true),
          "scope-violation op could not be added");
    const auto Run = Stack.Run(Source, Target, Foundation);
    Check(!Run.Success && Run.Stages.size() == 1 &&
              !Run.Stages[0].Success &&
              !Run.Stages[0].MutationWithinDeclaredChannels,
          "undeclared local translation mutation was not rejected");
    Check(EquivalentRetargetOpClips(Foundation, Run.FinalOutput),
          "failed stage escaped into final output");
}

void TestNonDeterministicOperatorRejected()
{
    const auto Source = MakeSkeleton("source-nondeterministic");
    const auto Target = MakeSkeleton("target-nondeterministic");
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    RetargetOpStack Stack;
    Check(Stack.Add("nondeterministic",
                    std::make_unique<NonDeterministicOp>(), true),
          "non-deterministic op could not be added");
    const auto Run = Stack.Run(Source, Target, Foundation);
    Check(!Run.Success && Run.Stages.size() == 1 &&
              Run.Stages[0].Executed &&
              !Run.Stages[0].DeterministicRepeatabilityVerified &&
              !Run.Stages[0].Success &&
              EquivalentRetargetOpClips(Foundation, Run.FinalOutput),
          "non-deterministic operator escaped OpStack fail-closed policy");
}

void TestSinglePassExecutesExactlyOnce()
{
    const auto Source = MakeSkeleton("source-single-pass");
    const auto Target = MakeSkeleton("target-single-pass");
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    int RunCount = 0;
    RetargetOpStack Stack;
    Check(Stack.Add(
              "single_pass",
              std::make_unique<CountingPhaseOp>(
                  RetargetOpPhase::GoalGeneration, RunCount), true),
          "single-pass op could not be added");
    RetargetOpStackRunOptions Options;
    Options.RepeatabilityMode = RetargetOpRepeatabilityMode::SinglePass;
    const auto Run = Stack.Run(Source, Target, Foundation, Options);
    Check(Run.Success && RunCount == 1 && Run.Stages.size() == 1 &&
              Run.Stages[0].Executed &&
              !Run.Stages[0].RepeatabilityCheckPerformed &&
              !Run.Stages[0].DeterministicRepeatabilityVerified,
          "single-pass mode did not execute exactly once or fabricated audit state");
}

void TestPhaseOrderingFailsClosed()
{
    const auto Source = MakeSkeleton("source-phase-order");
    const auto Target = MakeSkeleton("target-phase-order");
    const RetargetOpClip Foundation = MakeClip(Source, Target);
    int WarpRuns = 0;
    int GenerationRuns = 0;
    RetargetOpStack Stack;
    Check(Stack.Add(
              "warp_first",
              std::make_unique<CountingPhaseOp>(
                  RetargetOpPhase::GoalWarp, WarpRuns), true),
          "warp phase op could not be added");
    Check(Stack.Add(
              "generation_late",
              std::make_unique<CountingPhaseOp>(
                  RetargetOpPhase::GoalGeneration, GenerationRuns), true),
          "late generation op could not be added");
    RetargetOpStackRunOptions Options;
    Options.RepeatabilityMode = RetargetOpRepeatabilityMode::SinglePass;
    const auto Run = Stack.Run(Source, Target, Foundation, Options);
    Check(!Run.Success && WarpRuns == 1 && GenerationRuns == 0 &&
              Run.Stages.size() == 2 && !Run.Stages[1].Executed &&
              EquivalentRetargetOpClips(
                  Foundation, Run.FinalOutput),
          "out-of-order phases did not fail closed before execution");
}
} // namespace

int main()
{
    TestDisabledExactPassthrough();
    TestDisabledInvalidDescriptorStillPassthrough();
    TestDeclaredScopeAndOrdering();
    TestUndeclaredMutationRejected();
    TestNonDeterministicOperatorRejected();
    TestSinglePassExecutesExactlyOnce();
    TestPhaseOrderingFailsClosed();
    if (Failures != 0)
    {
        std::cerr << "retarget_op_stack_tests failed: "
                  << Failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "retarget_op_stack_tests passed: 7 contract groups\n";
    return EXIT_SUCCESS;
}
