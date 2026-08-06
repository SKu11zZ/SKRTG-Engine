#include "skrtg/retarget/op_stack_config.h"

#include "skrtg/core/math/transform.h"
#include "skrtg/retarget/ops/contact_foot_plant_op.h"
#include "skrtg/retarget/ops/goal_solver_op.h"
#include "skrtg/retarget/ops/ground_floor_constraint_op.h"
#include "skrtg/retarget/ops/stride_warping_op.h"
#include "skrtg/retarget/ops/weapon_goals_op.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

namespace skrtg::retarget
{
namespace
{
using Json = nlohmann::json;
using core::math::IdentityTransform;
using core::math::Quat;
using core::math::TransformRT;
using core::math::Vec3;
using core::skeleton::NormalizedRuntimeSkeleton;
using ops::ContactFootPlantBinding;
using ops::ContactFootPlantOp;
using ops::ContactFootPlantOptions;
using ops::GroundFloorConstraintOp;
using ops::GroundFloorConstraintOptions;
using ops::GroundFloorGoalBinding;
using ops::RetargetGoalSolveBinding;
using ops::RetargetGoalSolveMode;
using ops::RetargetGoalSolverOp;
using ops::RetargetGoalSolverOptions;
using ops::StrideWarpGoalBinding;
using ops::StrideWarpingOp;
using ops::StrideWarpingOptions;
using ops::WeaponAnchorSkeleton;
using ops::WeaponGoalBinding;
using ops::WeaponGoalsOp;
using ops::WeaponGoalsOptions;

constexpr std::uintmax_t MaximumConfigBytes = 1024U * 1024U;
constexpr std::size_t MaximumOperations = 64;
constexpr std::size_t MaximumBindings = 256;
constexpr std::size_t MaximumFootprintPoints = 32;

bool Finite(const double Value) { return std::isfinite(Value); }

bool ContractId(const std::string& Value)
{
    return !Value.empty() && Value.size() <= 128 &&
        std::all_of(
            Value.begin(), Value.end(),
            [](const unsigned char Character)
            {
                return (Character >= 'a' && Character <= 'z') ||
                    (Character >= '0' && Character <= '9') ||
                    Character == '_';
            });
}

bool Shape(const Json& Value,
           const std::set<std::string>& Allowed,
           const std::set<std::string>& Required,
           const std::string& Label,
           std::string& OutError)
{
    if (!Value.is_object())
    {
        OutError = Label + " must be an object";
        return false;
    }
    for (auto It = Value.begin(); It != Value.end(); ++It)
    {
        if (Allowed.find(It.key()) == Allowed.end())
        {
            OutError = Label + " contains unsupported field: " + It.key();
            return false;
        }
    }
    for (const std::string& Field : Required)
    {
        if (!Value.contains(Field))
        {
            OutError = Label + " is missing field: " + Field;
            return false;
        }
    }
    return true;
}

bool ArrayWithin(const Json& Value,
                 const std::size_t Maximum,
                 const std::string& Label,
                 std::string& OutError,
                 const bool AllowEmpty = false)
{
    if (!Value.is_array() || Value.size() > Maximum ||
        (!AllowEmpty && Value.empty()))
    {
        OutError = Label + " must be a bounded" +
            (AllowEmpty ? "" : " non-empty") + " array";
        return false;
    }
    return true;
}

bool ReadVec3(const Json& Value,
              Vec3& Out,
              const std::string& Label,
              std::string& OutError)
{
    if (!Value.is_array() || Value.size() != 3 ||
        !Value[0].is_number() || !Value[1].is_number() ||
        !Value[2].is_number())
    {
        OutError = Label + " must be a three-number array";
        return false;
    }
    Out = {Value[0].get<double>(), Value[1].get<double>(),
           Value[2].get<double>()};
    if (!Finite(Out.X) || !Finite(Out.Y) || !Finite(Out.Z))
    {
        OutError = Label + " contains a non-finite component";
        return false;
    }
    return true;
}

bool ReadQuat(const Json& Value,
              Quat& Out,
              const std::string& Label,
              std::string& OutError)
{
    if (!Value.is_array() || Value.size() != 4)
    {
        OutError = Label + " must be a four-number quaternion array";
        return false;
    }
    for (const Json& Component : Value)
    {
        if (!Component.is_number())
        {
            OutError = Label + " contains a non-number component";
            return false;
        }
    }
    Out = {Value[0].get<double>(), Value[1].get<double>(),
           Value[2].get<double>(), Value[3].get<double>()};
    const double NormSquared = Out.X * Out.X + Out.Y * Out.Y +
        Out.Z * Out.Z + Out.W * Out.W;
    if (!Finite(NormSquared) || std::abs(NormSquared - 1.0) > 1.0e-4)
    {
        OutError = Label + " is not a finite unit quaternion";
        return false;
    }
    Out = core::math::Normalize(Out);
    return true;
}

bool ReadTransform(const Json& Value,
                   TransformRT& Out,
                   const std::string& Label,
                   std::string& OutError)
{
    if (!Shape(
            Value,
            {"translationCm", "rotation", "scale"},
            {"translationCm", "rotation", "scale"},
            Label, OutError))
    {
        return false;
    }
    return ReadVec3(Value.at("translationCm"), Out.TranslationCm,
                    Label + ".translationCm", OutError) &&
        ReadQuat(Value.at("rotation"), Out.Rotation,
                 Label + ".rotation", OutError) &&
        ReadVec3(Value.at("scale"), Out.Scale,
                 Label + ".scale", OutError);
}

int ResolveBone(const NormalizedRuntimeSkeleton& Skeleton,
                const std::string& Name,
                const std::string& Label,
                std::string& OutError)
{
    if (Name.empty())
    {
        OutError = Label + " bone name is empty";
        return -1;
    }
    int Result = -1;
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        if (Skeleton.BoneAt(Index).Name != Name) continue;
        if (Result >= 0)
        {
            OutError = Label + " bone name is ambiguous: " + Name;
            return -1;
        }
        Result = static_cast<int>(Index);
    }
    if (Result < 0)
        OutError = Label + " bone does not exist: " + Name;
    return Result;
}

bool ReadFile(const std::filesystem::path& Path,
              Json& Out,
              std::string& OutError)
{
    std::error_code Error;
    const std::uintmax_t Size = std::filesystem::file_size(Path, Error);
    if (Error || Size == 0 || Size > MaximumConfigBytes)
    {
        OutError = "OpStack config is missing, empty, or exceeds 1 MiB";
        return false;
    }
    std::ifstream Stream(Path, std::ios::binary);
    if (!Stream)
    {
        OutError = "OpStack config is not readable";
        return false;
    }
    Out = Json::parse(Stream, nullptr, true, true);
    return true;
}

bool ParseGoalSeeds(const Json& Value,
                    const NormalizedRuntimeSkeleton& Target,
                    std::vector<RetargetOpGoalSeed>& Out,
                    std::string& OutError)
{
    if (!ArrayWithin(Value, MaximumBindings, "goalSeeds", OutError, true))
        return false;
    std::set<std::string> Names;
    for (std::size_t Index = 0; Index < Value.size(); ++Index)
    {
        const Json& SeedJson = Value[Index];
        const std::string Label =
            "goalSeeds[" + std::to_string(Index) + "]";
        if (!Shape(SeedJson, {"name", "targetBone"},
                   {"name", "targetBone"}, Label, OutError))
            return false;
        RetargetOpGoalSeed Seed;
        Seed.GoalName = SeedJson.at("name").get<std::string>();
        if (Seed.GoalName.empty() || Seed.GoalName.size() > 256 ||
            !Names.insert(Seed.GoalName).second)
        {
            OutError = Label + " has an invalid or duplicate goal name";
            return false;
        }
        Seed.TargetBoneIndex = ResolveBone(
            Target, SeedJson.at("targetBone").get<std::string>(),
            Label + ".targetBone", OutError);
        if (Seed.TargetBoneIndex < 0) return false;
        Out.push_back(std::move(Seed));
    }
    return true;
}

bool ParseWeapon(const Json& Settings,
                 const NormalizedRuntimeSkeleton& Source,
                 const NormalizedRuntimeSkeleton& Target,
                 std::unique_ptr<IRetargetOp>& Out,
                 std::string& OutError)
{
    if (!Shape(Settings, {"bindings"}, {"bindings"},
               "weapon settings", OutError) ||
        !ArrayWithin(Settings.at("bindings"), MaximumBindings,
                     "weapon bindings", OutError))
        return false;
    WeaponGoalsOptions Options;
    for (std::size_t Index = 0;
         Index < Settings.at("bindings").size(); ++Index)
    {
        const Json& Value = Settings.at("bindings")[Index];
        const std::string Label =
            "weapon.bindings[" + std::to_string(Index) + "]";
        if (!Shape(
                Value,
                {"label", "goalName", "anchorSkeleton", "anchorBone",
                 "targetGoalBone", "anchorOffset", "maintainInputOffset",
                 "translationAlpha", "rotationAlpha"},
                {"label", "goalName", "anchorSkeleton", "anchorBone"},
                Label, OutError))
            return false;
        WeaponGoalBinding Binding;
        Binding.Label = Value.at("label").get<std::string>();
        Binding.GoalName = Value.at("goalName").get<std::string>();
        const std::string SkeletonName =
            Value.at("anchorSkeleton").get<std::string>();
        const NormalizedRuntimeSkeleton* AnchorSkeleton = nullptr;
        if (SkeletonName == "source")
        {
            Binding.AnchorSkeleton = WeaponAnchorSkeleton::Source;
            AnchorSkeleton = &Source;
        }
        else if (SkeletonName == "target")
        {
            Binding.AnchorSkeleton = WeaponAnchorSkeleton::Target;
            AnchorSkeleton = &Target;
        }
        else
        {
            OutError = Label + ".anchorSkeleton is unsupported";
            return false;
        }
        Binding.AnchorBoneIndex = ResolveBone(
            *AnchorSkeleton, Value.at("anchorBone").get<std::string>(),
            Label + ".anchorBone", OutError);
        if (Binding.AnchorBoneIndex < 0) return false;
        if (Value.contains("targetGoalBone"))
        {
            Binding.TargetGoalBoneIndex = ResolveBone(
                Target, Value.at("targetGoalBone").get<std::string>(),
                Label + ".targetGoalBone", OutError);
            if (Binding.TargetGoalBoneIndex < 0) return false;
        }
        Binding.AnchorOffset = IdentityTransform();
        if (Value.contains("anchorOffset") &&
            !ReadTransform(Value.at("anchorOffset"), Binding.AnchorOffset,
                           Label + ".anchorOffset", OutError))
            return false;
        Binding.MaintainInputOffset =
            Value.value("maintainInputOffset", false);
        Binding.TranslationAlpha =
            Value.value("translationAlpha", 1.0);
        Binding.RotationAlpha = Value.value("rotationAlpha", 1.0);
        Options.Bindings.push_back(std::move(Binding));
    }
    Out = std::make_unique<WeaponGoalsOp>(std::move(Options));
    return true;
}

bool ParseStride(const Json& Settings,
                 const NormalizedRuntimeSkeleton& Target,
                 std::unique_ptr<IRetargetOp>& Out,
                 std::string& OutError)
{
    if (!Shape(
            Settings,
            {"targetPivotBone", "forwardAxisLocal", "upAxisLocal",
             "rotateAxesWithPivot", "warpForwards", "warpSplay",
             "sidewaysOffsetCm", "alpha", "goals"},
            {"targetPivotBone", "forwardAxisLocal", "upAxisLocal",
             "goals"}, "stride settings", OutError) ||
        !ArrayWithin(Settings.at("goals"), MaximumBindings,
                     "stride goals", OutError))
        return false;
    StrideWarpingOptions Options;
    Options.TargetPivotBoneIndex = ResolveBone(
        Target, Settings.at("targetPivotBone").get<std::string>(),
        "stride.targetPivotBone", OutError);
    if (Options.TargetPivotBoneIndex < 0 ||
        !ReadVec3(Settings.at("forwardAxisLocal"),
                  Options.ForwardAxisLocal,
                  "stride.forwardAxisLocal", OutError) ||
        !ReadVec3(Settings.at("upAxisLocal"), Options.UpAxisLocal,
                  "stride.upAxisLocal", OutError))
        return false;
    Options.RotateAxesWithPivot =
        Settings.value("rotateAxesWithPivot", true);
    Options.WarpForwards = Settings.value("warpForwards", 1.0);
    Options.WarpSplay = Settings.value("warpSplay", 1.0);
    Options.SidewaysOffsetCm = Settings.value("sidewaysOffsetCm", 0.0);
    Options.Alpha = Settings.value("alpha", 1.0);
    for (std::size_t Index = 0; Index < Settings.at("goals").size(); ++Index)
    {
        const Json& Value = Settings.at("goals")[Index];
        const std::string Label =
            "stride.goals[" + std::to_string(Index) + "]";
        if (!Shape(Value, {"goalName", "sideSign", "alpha"},
                   {"goalName", "sideSign"}, Label, OutError))
            return false;
        StrideWarpGoalBinding Goal;
        Goal.GoalName = Value.at("goalName").get<std::string>();
        Goal.SideSign = Value.at("sideSign").get<int>();
        Goal.Alpha = Value.value("alpha", 1.0);
        Options.Goals.push_back(std::move(Goal));
    }
    Out = std::make_unique<StrideWarpingOp>(std::move(Options));
    return true;
}

bool ParseContact(const Json& Settings,
                  const NormalizedRuntimeSkeleton& Source,
                  std::unique_ptr<IRetargetOp>& Out,
                  std::string& OutError)
{
    if (!Shape(
            Settings,
            {"groundPlaneNormalModel", "groundPlaneDistanceCm",
             "enterSpeedCmPerSecond", "exitSpeedCmPerSecond",
             "enterHeightCm", "exitHeightCm", "enterConfirmationFrames",
             "minimumPlantFrames", "releaseBlendFrames",
             "maximumAnchorDriftCm", "feet"},
            {"groundPlaneNormalModel", "feet"},
            "contact foot plant settings", OutError) ||
        !ArrayWithin(Settings.at("feet"), MaximumBindings,
                     "contact feet", OutError))
        return false;
    ContactFootPlantOptions Options;
    if (!ReadVec3(Settings.at("groundPlaneNormalModel"),
                  Options.GroundPlaneNormalModel,
                  "contact.groundPlaneNormalModel", OutError))
        return false;
    Options.GroundPlaneDistanceCm =
        Settings.value("groundPlaneDistanceCm", 0.0);
    Options.EnterSpeedCmPerSecond =
        Settings.value("enterSpeedCmPerSecond", 15.0);
    Options.ExitSpeedCmPerSecond =
        Settings.value("exitSpeedCmPerSecond", 30.0);
    Options.EnterHeightCm = Settings.value("enterHeightCm", 4.0);
    Options.ExitHeightCm = Settings.value("exitHeightCm", 8.0);
    Options.EnterConfirmationFrames =
        Settings.value("enterConfirmationFrames", 2);
    Options.MinimumPlantFrames =
        Settings.value("minimumPlantFrames", 2);
    Options.ReleaseBlendFrames =
        Settings.value("releaseBlendFrames", 6);
    Options.MaximumAnchorDriftCm =
        Settings.value("maximumAnchorDriftCm", 100.0);
    for (std::size_t Index = 0; Index < Settings.at("feet").size(); ++Index)
    {
        const Json& Value = Settings.at("feet")[Index];
        const std::string Label =
            "contact.feet[" + std::to_string(Index) + "]";
        if (!Shape(
                Value,
                {"label", "sourceContactBone", "sourceContactPointLocalCm",
                 "goalName", "translationAlpha", "rotationAlpha"},
                {"label", "sourceContactBone", "goalName"},
                Label, OutError))
            return false;
        ContactFootPlantBinding Foot;
        Foot.Label = Value.at("label").get<std::string>();
        Foot.SourceContactBoneIndex = ResolveBone(
            Source,
            Value.at("sourceContactBone").get<std::string>(),
            Label + ".sourceContactBone", OutError);
        if (Foot.SourceContactBoneIndex < 0) return false;
        if (Value.contains("sourceContactPointLocalCm") &&
            !ReadVec3(
                Value.at("sourceContactPointLocalCm"),
                Foot.SourceContactPointLocalCm,
                Label + ".sourceContactPointLocalCm", OutError))
            return false;
        Foot.GoalName = Value.at("goalName").get<std::string>();
        Foot.TranslationAlpha = Value.value("translationAlpha", 1.0);
        Foot.RotationAlpha = Value.value("rotationAlpha", 1.0);
        Options.Feet.push_back(std::move(Foot));
    }
    Out = std::make_unique<ContactFootPlantOp>(std::move(Options));
    return true;
}

bool ParseFloor(const Json& Settings,
                std::unique_ptr<IRetargetOp>& Out,
                std::string& OutError)
{
    if (!Shape(
            Settings,
            {"planeNormalModel", "planeDistanceCm", "goalUpAxisLocal",
             "goals"},
            {"planeNormalModel", "goalUpAxisLocal", "goals"},
            "floor settings", OutError) ||
        !ArrayWithin(Settings.at("goals"), MaximumBindings,
                     "floor goals", OutError))
        return false;
    GroundFloorConstraintOptions Options;
    if (!ReadVec3(Settings.at("planeNormalModel"),
                  Options.PlaneNormalModel,
                  "floor.planeNormalModel", OutError) ||
        !ReadVec3(Settings.at("goalUpAxisLocal"),
                  Options.GoalUpAxisLocal,
                  "floor.goalUpAxisLocal", OutError))
        return false;
    Options.PlaneDistanceCm = Settings.value("planeDistanceCm", 0.0);
    for (std::size_t Index = 0; Index < Settings.at("goals").size(); ++Index)
    {
        const Json& Value = Settings.at("goals")[Index];
        const std::string Label =
            "floor.goals[" + std::to_string(Index) + "]";
        if (!Shape(
                Value,
                {"goalName", "clearanceCm", "translationAlpha",
                 "rotationAlpha", "footprintPointsLocalCm"},
                {"goalName"}, Label, OutError))
            return false;
        GroundFloorGoalBinding Goal;
        Goal.GoalName = Value.at("goalName").get<std::string>();
        Goal.ClearanceCm = Value.value("clearanceCm", 0.0);
        Goal.TranslationAlpha = Value.value("translationAlpha", 1.0);
        Goal.RotationAlpha = Value.value("rotationAlpha", 0.0);
        if (Value.contains("footprintPointsLocalCm"))
        {
            if (!ArrayWithin(
                    Value.at("footprintPointsLocalCm"),
                    MaximumFootprintPoints,
                    Label + ".footprintPointsLocalCm", OutError, true))
                return false;
            for (std::size_t PointIndex = 0;
                 PointIndex < Value.at("footprintPointsLocalCm").size();
                 ++PointIndex)
            {
                Vec3 Point;
                if (!ReadVec3(
                        Value.at("footprintPointsLocalCm")[PointIndex],
                        Point,
                        Label + ".footprintPointsLocalCm[" +
                            std::to_string(PointIndex) + "]",
                        OutError))
                    return false;
                Goal.FootprintPointsLocalCm.push_back(Point);
            }
        }
        Options.Goals.push_back(std::move(Goal));
    }
    Out = std::make_unique<GroundFloorConstraintOp>(std::move(Options));
    return true;
}

bool ParseSolver(const Json& Settings,
                 const NormalizedRuntimeSkeleton& Target,
                 std::unique_ptr<IRetargetOp>& Out,
                 std::string& OutError)
{
    if (!Shape(
            Settings,
            {"solverEpsilon", "solverPositionToleranceCm",
             "solverLengthToleranceCm", "bindings"},
            {"bindings"}, "goal solver settings", OutError) ||
        !ArrayWithin(Settings.at("bindings"), MaximumBindings,
                     "goal solver bindings", OutError))
        return false;
    RetargetGoalSolverOptions Options;
    Options.SolverEpsilon = Settings.value("solverEpsilon", 1.0e-9);
    Options.SolverPositionToleranceCm =
        Settings.value("solverPositionToleranceCm", 1.0e-6);
    Options.SolverLengthToleranceCm =
        Settings.value("solverLengthToleranceCm", 1.0e-6);
    for (std::size_t Index = 0;
         Index < Settings.at("bindings").size(); ++Index)
    {
        const Json& Value = Settings.at("bindings")[Index];
        const std::string Label =
            "solver.bindings[" + std::to_string(Index) + "]";
        if (!Shape(
                Value,
                {"label", "goalName", "mode", "targetChainBones",
                 "targetPoleBone", "poleGoalName",
                 "poleFallbackOffsetModelCm", "targetBone",
                 "applyGoalTranslation", "applyGoalRotation"},
                {"label", "goalName", "mode"}, Label, OutError))
            return false;
        RetargetGoalSolveBinding Binding;
        Binding.Label = Value.at("label").get<std::string>();
        Binding.GoalName = Value.at("goalName").get<std::string>();
        const std::string Mode = Value.at("mode").get<std::string>();
        if (Mode == "two_bone")
        {
            if (Value.contains("targetBone"))
            {
                OutError = Label +
                    ".targetBone is not valid for two_bone mode";
                return false;
            }
            Binding.Mode = RetargetGoalSolveMode::TwoBone;
            if (!Value.contains("targetChainBones") ||
                !Value.at("targetChainBones").is_array() ||
                Value.at("targetChainBones").size() != 3)
            {
                OutError = Label +
                    ".targetChainBones must contain exactly three names";
                return false;
            }
            for (std::size_t Bone = 0; Bone < 3; ++Bone)
            {
                Binding.TargetChainIndices[Bone] = ResolveBone(
                    Target,
                    Value.at("targetChainBones")[Bone].get<std::string>(),
                    Label + ".targetChainBones[" +
                        std::to_string(Bone) + "]",
                    OutError);
                if (Binding.TargetChainIndices[Bone] < 0) return false;
            }
            if (Value.contains("targetPoleBone"))
            {
                Binding.TargetPoleBoneIndex = ResolveBone(
                    Target,
                    Value.at("targetPoleBone").get<std::string>(),
                    Label + ".targetPoleBone", OutError);
                if (Binding.TargetPoleBoneIndex < 0) return false;
            }
            Binding.PoleGoalName = Value.value(
                "poleGoalName", std::string());
            if (Value.contains("poleFallbackOffsetModelCm") &&
                !ReadVec3(
                    Value.at("poleFallbackOffsetModelCm"),
                    Binding.PoleFallbackOffsetModelCm,
                    Label + ".poleFallbackOffsetModelCm", OutError))
                return false;
        }
        else if (Mode == "direct_bone")
        {
            if (Value.contains("targetChainBones") ||
                Value.contains("targetPoleBone") ||
                Value.contains("poleGoalName") ||
                Value.contains("poleFallbackOffsetModelCm"))
            {
                OutError = Label +
                    " contains a two_bone-only field in direct_bone mode";
                return false;
            }
            Binding.Mode = RetargetGoalSolveMode::DirectBone;
            if (!Value.contains("targetBone"))
            {
                OutError = Label + ".targetBone is required";
                return false;
            }
            Binding.TargetBoneIndex = ResolveBone(
                Target, Value.at("targetBone").get<std::string>(),
                Label + ".targetBone", OutError);
            if (Binding.TargetBoneIndex < 0) return false;
        }
        else
        {
            OutError = Label + ".mode is unsupported";
            return false;
        }
        Binding.ApplyGoalTranslation =
            Value.value("applyGoalTranslation", true);
        Binding.ApplyGoalRotation =
            Value.value("applyGoalRotation", true);
        Options.Bindings.push_back(std::move(Binding));
    }
    Out = std::make_unique<RetargetGoalSolverOp>(std::move(Options));
    return true;
}

bool ParseOperator(const Json& Value,
                   const NormalizedRuntimeSkeleton& Source,
                   const NormalizedRuntimeSkeleton& Target,
                   RetargetOpProgram& Program,
                   std::string& OutError)
{
    if (!Shape(Value, {"instanceId", "type", "enabled", "settings"},
               {"instanceId", "type", "enabled", "settings"},
               "operation", OutError))
        return false;
    const std::string InstanceId =
        Value.at("instanceId").get<std::string>();
    const std::string Type = Value.at("type").get<std::string>();
    const bool Enabled = Value.at("enabled").get<bool>();
    if (!ContractId(InstanceId))
    {
        OutError = "operation instanceId is invalid: " + InstanceId;
        return false;
    }
    std::unique_ptr<IRetargetOp> Op;
    const Json& Settings = Value.at("settings");
    bool Parsed = false;
    if (Type == "weapon_goals_exact_name_v1")
        Parsed = ParseWeapon(Settings, Source, Target, Op, OutError);
    else if (Type == "stride_warping_goal_space_v1")
        Parsed = ParseStride(Settings, Target, Op, OutError);
    else if (Type == "contact_foot_plant_v2")
        Parsed = ParseContact(Settings, Source, Op, OutError);
    else if (Type == "ground_floor_constraint_explicit_plane_v1")
        Parsed = ParseFloor(Settings, Op, OutError);
    else if (Type == "unified_goal_solver_v1")
        Parsed = ParseSolver(Settings, Target, Op, OutError);
    else
    {
        OutError = "unsupported operation type: " + Type;
        return false;
    }
    if (!Parsed) return false;
    if (!Program.Stack.Add(InstanceId, std::move(Op), Enabled))
    {
        OutError = "duplicate or invalid operation instance: " + InstanceId;
        return false;
    }
    Program.Entries.push_back({InstanceId, Type, Enabled});
    return true;
}
} // namespace

RetargetOpProgramLoadResult LoadRetargetOpProgram(
    const std::filesystem::path& ConfigJson,
    const NormalizedRuntimeSkeleton& Source,
    const NormalizedRuntimeSkeleton& Target)
{
    RetargetOpProgramLoadResult Result;
    try
    {
        Json Root;
        std::string Error;
        if (!ReadFile(ConfigJson, Root, Error) ||
            !Shape(
                Root,
                {"schema", "schemaVersion", "candidate", "execution",
                 "goalSeeds", "operations"},
                {"schema", "schemaVersion", "candidate", "execution",
                 "goalSeeds", "operations"},
                "OpStack config", Error))
        {
            Result.Errors.push_back(Error);
            return Result;
        }
        if (Root.at("schema").get<std::string>() !=
                "skrtg.op_stack.v2" ||
            Root.at("schemaVersion").get<int>() != 2 ||
            !Root.at("candidate").get<bool>())
        {
            Result.Errors.push_back(
                "OpStack config must be schema v2 and candidate=true");
            return Result;
        }
        auto Program = std::make_unique<RetargetOpProgram>();
        Program->Candidate = true;
        const Json& Execution = Root.at("execution");
        if (!Shape(Execution, {"repeatabilityMode"},
                   {"repeatabilityMode"}, "execution", Error))
        {
            Result.Errors.push_back(Error);
            return Result;
        }
        const std::string Repeatability =
            Execution.at("repeatabilityMode").get<std::string>();
        if (Repeatability == "per_operator_audit")
            Program->RunOptions.RepeatabilityMode =
                RetargetOpRepeatabilityMode::PerOperatorAudit;
        else if (Repeatability == "single_pass")
            Program->RunOptions.RepeatabilityMode =
                RetargetOpRepeatabilityMode::SinglePass;
        else
        {
            Result.Errors.push_back(
                "execution.repeatabilityMode is unsupported");
            return Result;
        }
        if (!ParseGoalSeeds(
                Root.at("goalSeeds"), Target,
                Program->GoalSeeds, Error) ||
            !ArrayWithin(
                Root.at("operations"), MaximumOperations,
                "operations", Error))
        {
            Result.Errors.push_back(Error);
            return Result;
        }
        for (const Json& Operation : Root.at("operations"))
        {
            if (!ParseOperator(
                    Operation, Source, Target, *Program, Error))
            {
                Result.Errors.push_back(Error);
                return Result;
            }
        }
        Result.Success = true;
        Result.Program = std::move(Program);
        return Result;
    }
    catch (const std::exception& Error)
    {
        Result.Errors.push_back(
            std::string("invalid OpStack config: ") + Error.what());
        return Result;
    }
}
} // namespace skrtg::retarget
