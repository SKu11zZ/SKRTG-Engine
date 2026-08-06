#include "skrtg/retarget/op_stack.h"
#include "skrtg/retarget/op_stack_config.h"
#include "skrtg/retarget/ops/contact_foot_plant_op.h"
#include "skrtg/retarget/ops/goal_solver_op.h"
#include "skrtg/retarget/ops/ground_floor_constraint_op.h"
#include "skrtg/retarget/ops/stride_warping_op.h"
#include "skrtg/retarget/ops/weapon_goals_op.h"

#include "skrtg/core/math/transform.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace
{
using skrtg::core::animation::PoseBuffer;
using skrtg::core::animation::PoseSpace;
using skrtg::core::math::Compose;
using skrtg::core::math::IdentityTransform;
using skrtg::core::math::NearlyEqual;
using skrtg::core::math::Vec3;
using skrtg::core::skeleton::NormalizedRuntimeSkeleton;
using skrtg::core::skeleton::RuntimeBone;
using skrtg::core::skeleton::SkeletonIdentity;
using skrtg::retarget::FindRetargetOpGoal;
using skrtg::retarget::RetargetOpClip;
using skrtg::retarget::RetargetOpFrame;
using skrtg::retarget::RetargetOpGoalSeed;
using skrtg::retarget::RetargetOpRepeatabilityMode;
using skrtg::retarget::RetargetOpStack;
using skrtg::retarget::RetargetOpStackRunOptions;
using skrtg::retarget::SeedRetargetOpGoals;
using skrtg::retarget::ops::ContactFootPlantBinding;
using skrtg::retarget::ops::ContactFootPlantOp;
using skrtg::retarget::ops::ContactFootPlantOptions;
using skrtg::retarget::ops::GroundFloorConstraintOp;
using skrtg::retarget::ops::GroundFloorConstraintOptions;
using skrtg::retarget::ops::GroundFloorGoalBinding;
using skrtg::retarget::ops::RetargetGoalSolveBinding;
using skrtg::retarget::ops::RetargetGoalSolveMode;
using skrtg::retarget::ops::RetargetGoalSolverOp;
using skrtg::retarget::ops::RetargetGoalSolverOptions;
using skrtg::retarget::ops::StrideWarpGoalBinding;
using skrtg::retarget::ops::StrideWarpingOp;
using skrtg::retarget::ops::StrideWarpingOptions;
using skrtg::retarget::ops::WeaponAnchorSkeleton;
using skrtg::retarget::ops::WeaponGoalBinding;
using skrtg::retarget::ops::WeaponGoalsOp;
using skrtg::retarget::ops::WeaponGoalsOptions;

int Failures = 0;

void Check(const bool Condition, const std::string& Message)
{
    if (Condition) return;
    ++Failures;
    std::cerr << "FAIL: " << Message << '\n';
}

int AddBone(NormalizedRuntimeSkeleton& Skeleton,
            const std::string& Name,
            const int Parent,
            const Vec3 Translation)
{
    RuntimeBone Bone;
    Bone.Name = Name;
    Bone.RawPath = "synthetic/" + Name;
    Bone.ParentIndex = Parent;
    Bone.RawIndex = static_cast<int>(Skeleton.BoneCount());
    Bone.LocalRest = IdentityTransform();
    Bone.LocalRest.TranslationCm = Translation;
    return Skeleton.AddBone(std::move(Bone));
}

NormalizedRuntimeSkeleton MakeSourceSkeleton()
{
    NormalizedRuntimeSkeleton Skeleton;
    SkeletonIdentity Identity;
    Identity.HierarchyHash = "goal-ops-source-hierarchy";
    Identity.RestPoseHash = "goal-ops-source-rest";
    Identity.SourceAssetId = "synthetic-source";
    Skeleton.SetIdentity(std::move(Identity));
    AddBone(Skeleton, "root", -1, {0.0, 0.0, 0.0});
    AddBone(Skeleton, "Grip", 0, {20.0, 0.0, 80.0});
    AddBone(Skeleton, "foot_contact_l", 0, {5.0, 10.0, 1.0});
    return Skeleton;
}

NormalizedRuntimeSkeleton MakeTargetSkeleton()
{
    NormalizedRuntimeSkeleton Skeleton;
    SkeletonIdentity Identity;
    Identity.HierarchyHash = "goal-ops-target-hierarchy";
    Identity.RestPoseHash = "goal-ops-target-rest";
    Identity.SourceAssetId = "synthetic-target";
    Skeleton.SetIdentity(std::move(Identity));
    AddBone(Skeleton, "root", -1, {0.0, 0.0, 0.0});
    AddBone(Skeleton, "pelvis", 0, {0.0, 0.0, 60.0});
    AddBone(Skeleton, "thigh_l", 1, {0.0, 10.0, -40.0});
    AddBone(Skeleton, "calf_l", 2, {0.0, 0.0, -40.0});
    AddBone(Skeleton, "foot_l", 3, {10.0, 0.0, 0.0});
    AddBone(Skeleton, "spine", 1, {0.0, 0.0, 30.0});
    AddBone(Skeleton, "hand_l", 5, {20.0, 0.0, 20.0});
    return Skeleton;
}

PoseBuffer LocalRest(const NormalizedRuntimeSkeleton& Skeleton)
{
    PoseBuffer Result(PoseSpace::Local,
                      Skeleton.GetIdentity().HierarchyHash);
    Result.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
        Result[Index] = Skeleton.BoneAt(Index).LocalRest;
    return Result;
}

PoseBuffer ToModel(const NormalizedRuntimeSkeleton& Skeleton,
                   const PoseBuffer& Local)
{
    PoseBuffer Result(PoseSpace::Model,
                      Skeleton.GetIdentity().HierarchyHash);
    Result.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        const int Parent = Skeleton.BoneAt(Index).ParentIndex;
        Result[Index] = Parent < 0
            ? Local[Index]
            : Compose(Result[static_cast<std::size_t>(Parent)],
                      Local[Index]);
    }
    return Result;
}

RetargetOpClip MakeClip(const NormalizedRuntimeSkeleton& Source,
                        const NormalizedRuntimeSkeleton& Target)
{
    RetargetOpClip Clip;
    for (int FrameIndex = 0; FrameIndex < 7; ++FrameIndex)
    {
        PoseBuffer SourceLocal = LocalRest(Source);
        SourceLocal[1].TranslationCm.X += FrameIndex * 2.0;
        if (FrameIndex >= 4)
        {
            SourceLocal[2].TranslationCm.X += (FrameIndex - 3) * 10.0;
            SourceLocal[2].TranslationCm.Z += (FrameIndex - 3) * 8.0;
        }
        PoseBuffer TargetLocal = LocalRest(Target);
        RetargetOpFrame Frame;
        Frame.FrameIndex = FrameIndex;
        Frame.TimeSeconds = FrameIndex / 30.0;
        Frame.SourceModelPose = ToModel(Source, SourceLocal);
        Frame.TargetLocalPose = TargetLocal;
        Frame.TargetModelPose = ToModel(Target, TargetLocal);
        Clip.Frames.push_back(std::move(Frame));
    }
    return Clip;
}

void TestGoalPipelineAndSingleSolve()
{
    const auto Source = MakeSourceSkeleton();
    const auto Target = MakeTargetSkeleton();
    RetargetOpClip Clip = MakeClip(Source, Target);
    std::string Error;
    Check(SeedRetargetOpGoals(
              Target, {RetargetOpGoalSeed{"foot_l_goal", 4}},
              Clip, Error),
          "target foot goal could not be seeded: " + Error);

    WeaponGoalsOptions WeaponOptions;
    WeaponGoalBinding Weapon;
    Weapon.Label = "left_hand_to_grip";
    Weapon.GoalName = "left_hand_weapon_goal";
    Weapon.AnchorSkeleton = WeaponAnchorSkeleton::Source;
    Weapon.AnchorBoneIndex = 1;
    Weapon.TargetGoalBoneIndex = 6;
    Weapon.TranslationAlpha = 1.0;
    Weapon.RotationAlpha = 1.0;
    WeaponOptions.Bindings.push_back(Weapon);

    StrideWarpingOptions StrideOptions;
    StrideOptions.TargetPivotBoneIndex = 1;
    StrideOptions.ForwardAxisLocal = {1.0, 0.0, 0.0};
    StrideOptions.UpAxisLocal = {0.0, 0.0, 1.0};
    StrideOptions.WarpForwards = 1.25;
    StrideOptions.WarpSplay = 1.0;
    StrideOptions.Goals.push_back(
        StrideWarpGoalBinding{"foot_l_goal", -1, 1.0});

    ContactFootPlantOptions ContactOptions;
    ContactOptions.EnterSpeedCmPerSecond = 5.0;
    ContactOptions.ExitSpeedCmPerSecond = 20.0;
    ContactOptions.EnterHeightCm = 2.0;
    ContactOptions.ExitHeightCm = 5.0;
    ContactOptions.EnterConfirmationFrames = 2;
    ContactOptions.MinimumPlantFrames = 1;
    ContactOptions.ReleaseBlendFrames = 2;
    ContactFootPlantBinding Contact;
    Contact.Label = "left_foot_contact";
    Contact.SourceContactBoneIndex = 2;
    Contact.GoalName = "foot_l_goal";
    Contact.TranslationAlpha = 1.0;
    Contact.RotationAlpha = 0.0;
    ContactOptions.Feet.push_back(Contact);

    GroundFloorConstraintOptions FloorOptions;
    GroundFloorGoalBinding Floor;
    Floor.GoalName = "foot_l_goal";
    Floor.ClearanceCm = 0.0;
    Floor.TranslationAlpha = 1.0;
    Floor.RotationAlpha = 0.0;
    Floor.FootprintPointsLocalCm = {
        {-5.0, -3.0, 0.0}, {10.0, -3.0, 0.0},
        {-5.0, 3.0, 0.0}, {10.0, 3.0, 0.0}};
    FloorOptions.Goals.push_back(Floor);

    RetargetGoalSolverOptions SolverOptions;
    RetargetGoalSolveBinding FootSolve;
    FootSolve.Label = "solve_left_foot";
    FootSolve.GoalName = "foot_l_goal";
    FootSolve.Mode = RetargetGoalSolveMode::DirectBone;
    FootSolve.TargetBoneIndex = 4;
    FootSolve.ApplyGoalTranslation = true;
    FootSolve.ApplyGoalRotation = false;
    SolverOptions.Bindings.push_back(FootSolve);
    RetargetGoalSolveBinding HandSolve;
    HandSolve.Label = "solve_left_hand_weapon";
    HandSolve.GoalName = "left_hand_weapon_goal";
    HandSolve.Mode = RetargetGoalSolveMode::DirectBone;
    HandSolve.TargetBoneIndex = 6;
    HandSolve.ApplyGoalTranslation = true;
    HandSolve.ApplyGoalRotation = true;
    SolverOptions.Bindings.push_back(HandSolve);

    RetargetOpStack Stack;
    Check(Stack.Add("weapon_goals", std::make_unique<WeaponGoalsOp>(
                        WeaponOptions), true),
          "weapon goals op could not be added");
    Check(Stack.Add("stride", std::make_unique<StrideWarpingOp>(
                        StrideOptions), true),
          "stride op could not be added");
    Check(Stack.Add("contact", std::make_unique<ContactFootPlantOp>(
                        ContactOptions), true),
          "contact op could not be added");
    Check(Stack.Add("floor", std::make_unique<GroundFloorConstraintOp>(
                        FloorOptions), true),
          "floor op could not be added");
    Check(Stack.Add("solver", std::make_unique<RetargetGoalSolverOp>(
                        SolverOptions), true),
          "unified solver op could not be added");

    RetargetOpStackRunOptions RunOptions;
    RunOptions.RepeatabilityMode =
        RetargetOpRepeatabilityMode::SinglePass;
    const auto Run = Stack.Run(Source, Target, Clip, RunOptions);
    Check(Run.Success && Run.Stages.size() == 5,
          "complete goal pipeline did not execute successfully");
    for (const auto& Stage : Run.Stages)
    {
        Check(Stage.Success && Stage.Executed &&
                  !Stage.RepeatabilityCheckPerformed,
              "a candidate stage did not satisfy single-pass contracts");
    }

    const RetargetOpFrame& Last = Run.FinalOutput.Frames.back();
    const auto* WeaponGoal = FindRetargetOpGoal(
        Last, "left_hand_weapon_goal");
    const auto* FootGoal = FindRetargetOpGoal(Last, "foot_l_goal");
    Check(WeaponGoal != nullptr && FootGoal != nullptr,
          "goal pipeline lost required named goals");
    if (WeaponGoal != nullptr)
    {
        Check(NearlyEqual(
                  WeaponGoal->TransformModel.TranslationCm,
                  Last.SourceModelPose[1].TranslationCm, 1.0e-9),
              "exact-name weapon goal did not follow Grip");
        Check(NearlyEqual(
                  Last.TargetModelPose[6].TranslationCm,
                  WeaponGoal->TransformModel.TranslationCm, 1.0e-6),
              "unified solver did not write the hand goal once");
    }
    if (FootGoal != nullptr)
    {
        Check(FootGoal->TransformModel.TranslationCm.Z >= -1.0e-9,
              "ground/floor constraint left the foot below the plane");
        Check(NearlyEqual(
                  Last.TargetModelPose[4].TranslationCm,
                  FootGoal->TransformModel.TranslationCm, 1.0e-6),
              "unified solver did not apply the constrained foot goal");
    }
}

void TestTwoBoneTranslationCannotBeDisabled()
{
    const auto Source = MakeSourceSkeleton();
    const auto Target = MakeTargetSkeleton();
    RetargetOpClip Clip = MakeClip(Source, Target);
    std::string Error;
    Check(SeedRetargetOpGoals(
              Target, {RetargetOpGoalSeed{"leg_goal", 4}}, Clip, Error),
          "two-bone goal seed failed: " + Error);
    RetargetGoalSolverOptions Options;
    RetargetGoalSolveBinding Binding;
    Binding.Label = "invalid_leg";
    Binding.GoalName = "leg_goal";
    Binding.Mode = RetargetGoalSolveMode::TwoBone;
    Binding.TargetChainIndices = {{2, 3, 4}};
    Binding.ApplyGoalTranslation = false;
    Binding.ApplyGoalRotation = true;
    Options.Bindings.push_back(Binding);
    RetargetOpStack Stack;
    Check(Stack.Add("invalid_solver",
                    std::make_unique<RetargetGoalSolverOp>(Options), true),
          "invalid solver could not be staged for fail-closed test");
    RetargetOpStackRunOptions RunOptions;
    RunOptions.RepeatabilityMode =
        RetargetOpRepeatabilityMode::SinglePass;
    const auto Run = Stack.Run(Source, Target, Clip, RunOptions);
    Check(!Run.Success && Run.Stages.size() == 1 &&
              !Run.Stages[0].PreflightPassed &&
              !Run.Stages[0].Executed,
          "translation-disabled two-bone solve did not fail preflight");
}

void TestOperationProgramConfig()
{
    const auto Source = MakeSourceSkeleton();
    const auto Target = MakeTargetSkeleton();
    const std::filesystem::path Config =
        std::filesystem::temp_directory_path() /
        "skrtg_goal_ops_config_test.json";
    {
        std::ofstream Output(Config, std::ios::binary | std::ios::trunc);
        Output << R"JSON({
  "schema": "skrtg.op_stack.v2",
  "schemaVersion": 2,
  "candidate": true,
  "execution": {"repeatabilityMode": "single_pass"},
  "goalSeeds": [{"name": "foot_l_goal", "targetBone": "foot_l"}],
  "operations": [
    {"instanceId":"weapon","type":"weapon_goals_exact_name_v1","enabled":true,"settings":{"bindings":[{"label":"hand","goalName":"hand_goal","anchorSkeleton":"source","anchorBone":"Grip","targetGoalBone":"hand_l"}]}},
    {"instanceId":"stride","type":"stride_warping_goal_space_v1","enabled":true,"settings":{"targetPivotBone":"pelvis","forwardAxisLocal":[1,0,0],"upAxisLocal":[0,0,1],"goals":[{"goalName":"foot_l_goal","sideSign":-1}]}},
    {"instanceId":"contact","type":"contact_foot_plant_v2","enabled":true,"settings":{"groundPlaneNormalModel":[0,0,1],"feet":[{"label":"left","sourceContactBone":"foot_contact_l","goalName":"foot_l_goal"}]}},
    {"instanceId":"floor","type":"ground_floor_constraint_explicit_plane_v1","enabled":true,"settings":{"planeNormalModel":[0,0,1],"goalUpAxisLocal":[0,0,1],"goals":[{"goalName":"foot_l_goal"}]}},
    {"instanceId":"solver","type":"unified_goal_solver_v1","enabled":true,"settings":{"bindings":[{"label":"foot","goalName":"foot_l_goal","mode":"direct_bone","targetBone":"foot_l"},{"label":"hand","goalName":"hand_goal","mode":"direct_bone","targetBone":"hand_l"}]}}
  ]
})JSON";
    }
    const auto Loaded = skrtg::retarget::LoadRetargetOpProgram(
        Config, Source, Target);
    Check(Loaded.Success && Loaded.Program != nullptr &&
              Loaded.Program->Entries.size() == 5,
          "Operation System v2 JSON did not load all five ordered stages");
    if (Loaded.Success && Loaded.Program != nullptr)
    {
        RetargetOpClip Clip = MakeClip(Source, Target);
        std::string Error;
        Check(SeedRetargetOpGoals(
                  Target, Loaded.Program->GoalSeeds, Clip, Error),
              "config goal seeds failed: " + Error);
        const auto Run = Loaded.Program->Stack.Run(
            Source, Target, Clip, Loaded.Program->RunOptions);
        Check(Run.Success && Run.Stages.size() == 5,
              "loaded Operation System v2 program failed to execute");
    }
    {
        std::ofstream Output(Config, std::ios::binary | std::ios::trunc);
        Output << R"JSON({
  "schema":"skrtg.op_stack.v2",
  "schemaVersion":2,
  "candidate":true,
  "execution":{"repeatabilityMode":"single_pass"},
  "goalSeeds":[{"name":"foot_l_goal","targetBone":"foot_l"}],
  "operations":[
    {"instanceId":"solver","type":"unified_goal_solver_v1","enabled":false,"settings":{"bindings":[{"label":"foot","goalName":"foot_l_goal","mode":"direct_bone","targetBone":"foot_l","targetChainBones":["thigh_l","calf_l","foot_l"]}]}}
  ]
})JSON";
    }
    const auto RejectedModeFields =
        skrtg::retarget::LoadRetargetOpProgram(Config, Source, Target);
    Check(!RejectedModeFields.Success,
          "mode-incompatible solver fields must fail closed even when "
          "the operator is disabled");
    std::error_code RemoveError;
    std::filesystem::remove(Config, RemoveError);
}
} // namespace

int main()
{
    TestGoalPipelineAndSingleSolve();
    TestTwoBoneTranslationCannotBeDisabled();
    TestOperationProgramConfig();
    if (Failures != 0)
    {
        std::cerr << "retarget_goal_ops_tests failed: "
                  << Failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "retarget_goal_ops_tests passed: 3 contract groups\n";
    return EXIT_SUCCESS;
}
