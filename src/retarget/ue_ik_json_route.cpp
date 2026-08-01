#include "skrtg/retarget/ue_ik_json_route.h"

#include "skrtg/core/ik/two_bone_pose_buffer_consumer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace skrtg::retarget
{
namespace
{
using Json = nlohmann::json;
using core::animation::PoseBuffer;
using core::animation::PoseSpace;
using core::ik::ApplyAnalyticTwoBoneToPoseBuffer;
using core::ik::EndOrientationPolicy;
using core::ik::PoleFallbackPolicy;
using core::ik::TwoBoneChainTopology;
using core::ik::TwoBonePoseBufferRequest;
using core::math::Compose;
using core::math::Conjugate;
using core::math::Length;
using core::math::Multiply;
using core::math::Normalize;
using core::math::Quat;
using core::math::RelativeUnitScaleTransform;
using core::math::Scale;
using core::math::Subtract;
using core::math::TransformRT;
using core::math::Vec3;

constexpr double Epsilon = 1.0e-9;

bool ReadJson(
    const std::filesystem::path& Path,
    Json& Out,
    std::string& OutError)
{
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError = "failed to open UE JSON: " + Path.string();
        return false;
    }
    try
    {
        Out = Json::parse(Input);
        return true;
    }
    catch (const std::exception& Error)
    {
        OutError = "failed to parse UE JSON " + Path.string() +
            ": " + Error.what();
        return false;
    }
}

bool HasExportIdentity(
    const Json& Value,
    const char* Kind,
    std::string& OutError)
{
    const std::string Schema =
        Value.is_object()
        ? Value.value("schema", "")
        : "";
    const int SchemaVersion =
        Value.is_object()
        ? Value.value("schemaVersion", 0)
        : 0;
    const bool SupportedVersion =
        (Schema ==
             "skrtg.ue_ik_asset_export.v1" &&
         SchemaVersion == 1) ||
        (Schema ==
             "skrtg.ue_ik_asset_export.v2" &&
         SchemaVersion == 2);
    if (!Value.is_object() ||
        !SupportedVersion ||
        Value.value("kind", "") != Kind)
    {
        OutError = std::string("unsupported UE JSON schema or kind: ") +
            Kind;
        return false;
    }
    const Json Coordinate = Value.value("coordinateContract", Json{});
    if (!Coordinate.is_object() ||
        Coordinate.value("handedness", "") != "left" ||
        Coordinate.value("forwardAxis", "") != "+X" ||
        Coordinate.value("rightAxis", "") != "+Y" ||
        Coordinate.value("upAxis", "") != "+Z" ||
        Coordinate.value("distanceUnit", "") != "centimeter" ||
        Coordinate.value("quaternionComponentOrder", "") != "x,y,z,w")
    {
        OutError = "UE JSON coordinate contract is not the supported "
            "left-handed +X/+Y/+Z centimeter contract";
        return false;
    }
    return true;
}

bool ParseVector(const Json& Value, Vec3& Out)
{
    if (!Value.is_object()) return false;
    try
    {
        Out = {
            Value.at("x").get<double>(),
            Value.at("y").get<double>(),
            Value.at("z").get<double>()};
    }
    catch (...)
    {
        return false;
    }
    return std::isfinite(Out.X) && std::isfinite(Out.Y) &&
        std::isfinite(Out.Z);
}

bool ParseQuaternion(const Json& Value, Quat& Out)
{
    if (!Value.is_object()) return false;
    try
    {
        Out = Normalize({
            Value.at("x").get<double>(),
            Value.at("y").get<double>(),
            Value.at("z").get<double>(),
            Value.at("w").get<double>()});
    }
    catch (...)
    {
        return false;
    }
    return std::isfinite(Out.X) && std::isfinite(Out.Y) &&
        std::isfinite(Out.Z) && std::isfinite(Out.W);
}

bool ParseTransform(const Json& Value, TransformRT& Out)
{
    if (!Value.is_object() ||
        !ParseVector(Value.value("translation", Json{}),
                     Out.TranslationCm) ||
        !ParseQuaternion(Value.value("rotation", Json{}),
                         Out.Rotation) ||
        !ParseVector(Value.value("scale", Json{}), Out.Scale))
    {
        return false;
    }
    return true;
}

int FindBone(
    const std::vector<UEIKJsonBone>& Bones,
    const std::string& Name)
{
    const auto It = std::find_if(
        Bones.begin(), Bones.end(),
        [&](const UEIKJsonBone& Bone)
        {
            return Bone.Name == Name;
        });
    return It == Bones.end()
        ? -1
        : static_cast<int>(std::distance(Bones.begin(), It));
}

const UEIKJsonChain* FindChain(
    const UEIKJsonRig& Rig,
    const std::string& Name)
{
    const auto It = std::find_if(
        Rig.Chains.begin(), Rig.Chains.end(),
        [&](const UEIKJsonChain& Chain)
        {
            return Chain.Name == Name;
        });
    return It == Rig.Chains.end() ? nullptr : &*It;
}

bool ResolveChain(
    const std::vector<UEIKJsonBone>& Bones,
    UEIKJsonChain& Chain,
    std::string& OutError)
{
    const int Start = FindBone(Bones, Chain.StartBone);
    const int End = FindBone(Bones, Chain.EndBone);
    if (Start < 0 || End < 0)
    {
        OutError = "IK Rig chain endpoint is absent: " + Chain.Name;
        return false;
    }
    std::vector<int> Reverse;
    std::set<int> Seen;
    int Current = End;
    while (Current >= 0)
    {
        if (!Seen.insert(Current).second)
        {
            OutError = "IK Rig chain is cyclic: " + Chain.Name;
            return false;
        }
        Reverse.push_back(Current);
        if (Current == Start) break;
        Current = Bones[static_cast<std::size_t>(Current)].ParentIndex;
    }
    if (Reverse.empty() || Reverse.back() != Start)
    {
        OutError = "IK Rig chain is reversed or disconnected: " +
            Chain.Name;
        return false;
    }
    std::reverse(Reverse.begin(), Reverse.end());
    Chain.BoneIndices = std::move(Reverse);
    return true;
}

bool ParseRig(
    const Json& Root,
    UEIKJsonRig& Out,
    std::string& OutError)
{
    if (!HasExportIdentity(Root, "ikRigDefinition", OutError))
        return false;
    const Json Asset = Root.value("asset", Json{});
    Out.AssetObjectPath = Asset.value("objectPath", "");
    Out.AssetName = Asset.value("assetName", "");
    Out.RetargetRootBone = Root.value("retargetRootBone", "");
    Out.RetargetPelvisBone = Root.value("retargetPelvisBone", "");
    const Json ReferenceSkeleton =
        Root.value("referenceSkeleton", Json{});
    const Json Bones = ReferenceSkeleton.is_object()
        ? ReferenceSkeleton.value("bones", Json{})
        : Json{};
    if (Out.AssetObjectPath.empty() || Out.AssetName.empty() ||
        Out.RetargetRootBone.empty() || Out.RetargetPelvisBone.empty() ||
        !Bones.is_array() || Bones.empty())
    {
        OutError = "IK Rig JSON identity or reference skeleton is incomplete";
        return false;
    }
    Out.Bones.reserve(Bones.size());
    std::set<std::string> Names;
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const Json& Value = Bones[Index];
        UEIKJsonBone Bone;
        Bone.Index = Value.value("index", -1);
        Bone.ParentIndex = Value.value("parentIndex", -2);
        Bone.Name = Value.value("name", "");
        if (Bone.Index != static_cast<int>(Index) ||
            Bone.Name.empty() || !Names.insert(Bone.Name).second ||
            Bone.ParentIndex < -1 ||
            Bone.ParentIndex >= static_cast<int>(Index) ||
            !ParseTransform(Value.value("local", Json{}),
                            Bone.ReferenceLocal) ||
            !ParseTransform(Value.value("model", Json{}),
                            Bone.ReferenceModel))
        {
            OutError = "IK Rig reference skeleton is malformed at index " +
                std::to_string(Index);
            return false;
        }
        Bone.RetargetLocal = Bone.ReferenceLocal;
        Bone.RetargetModel = Bone.ReferenceModel;
        Out.Bones.push_back(std::move(Bone));
    }
    const Json Chains = Root.value("retargetChains", Json{});
    if (!Chains.is_array() || Chains.empty())
    {
        OutError = "IK Rig contains no retarget chains";
        return false;
    }
    std::set<std::string> ChainNames;
    for (const Json& Value : Chains)
    {
        UEIKJsonChain Chain;
        Chain.Name = Value.value("name", "");
        Chain.StartBone = Value.value("startBone", "");
        Chain.EndBone = Value.value("endBone", "");
        Chain.GoalName = Value.value("ikGoal", "");
        if (Chain.GoalName == "None") Chain.GoalName.clear();
        if (Chain.Name.empty() ||
            !ChainNames.insert(Chain.Name).second ||
            !ResolveChain(Out.Bones, Chain, OutError))
        {
            if (OutError.empty())
                OutError = "IK Rig chain name is missing or duplicated";
            return false;
        }
        Out.Chains.push_back(std::move(Chain));
    }
    if (FindBone(Out.Bones, Out.RetargetRootBone) < 0 ||
        FindBone(Out.Bones, Out.RetargetPelvisBone) < 0)
    {
        OutError = "IK Rig retarget root or pelvis is absent from skeleton";
        return false;
    }
    return true;
}

const Json* FindPose(
    const Json& Side,
    const std::string& PoseName)
{
    const auto It = Side.find("poses");
    if (It == Side.end() || !It->is_array()) return nullptr;
    for (const Json& Pose : *It)
    {
        if (Pose.value("name", "") == PoseName) return &Pose;
    }
    return nullptr;
}

bool ApplyResolvedPose(
    const Json& Side,
    UEIKJsonRig& Rig,
    std::string& OutPoseName,
    std::string& OutError)
{
    OutPoseName = Side.value("currentPoseName", "");
    const Json* Pose = FindPose(Side, OutPoseName);
    if (Pose == nullptr)
    {
        OutError = "current Retarget Pose is absent: " + OutPoseName;
        return false;
    }
    const Json Resolved = Pose->value("resolvedPose", Json{});
    const Json Bones = Resolved.value("bones", Json{});
    if (!Resolved.value("resolved", false) ||
        !Bones.is_array() || Bones.size() != Rig.Bones.size())
    {
        OutError = "current Retarget Pose is not fully resolved";
        return false;
    }
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const Json& Value = Bones[Index];
        UEIKJsonBone& Bone = Rig.Bones[Index];
        if (Value.value("index", -1) != static_cast<int>(Index) ||
            Value.value("name", "") != Bone.Name ||
            Value.value("parentIndex", -2) != Bone.ParentIndex ||
            !ParseTransform(Value.value("local", Json{}),
                            Bone.RetargetLocal) ||
            !ParseTransform(Value.value("model", Json{}),
                            Bone.RetargetModel))
        {
            OutError = "resolved Retarget Pose skeleton mismatch at index " +
                std::to_string(Index);
            return false;
        }
    }
    return true;
}

const Json* FindOp(const Json& Root, const std::string& StructType)
{
    const auto It = Root.find("retargetOpStack");
    if (It == Root.end() || !It->is_array()) return nullptr;
    for (const Json& Op : *It)
    {
        if (Op.value("structType", "") == StructType)
            return &Op;
    }
    return nullptr;
}

struct AlignmentMapEntry
{
    std::string CanonicalChain;
    std::string ActualChain;
    bool EnableFk = true;
    bool EnableIk = false;
};

bool ParseAlignmentMap(
    const Json& Root,
    std::vector<AlignmentMapEntry>& Out,
    std::string& OutError)
{
    const Json* FkOp =
        FindOp(Root, "/Script/IKRig.IKRetargetFKChainsOp");
    const Json* IkOp =
        FindOp(Root, "/Script/IKRig.IKRetargetRunIKRigOp");
    if (FkOp == nullptr || !FkOp->value("enabled", false))
    {
        OutError = "enabled UE FK Chains op is required";
        return false;
    }
    const Json ExportedChainMap =
        FkOp->value("chainMapping", Json{});
    const Json ChainMap = ExportedChainMap.is_array()
        ? ExportedChainMap
        : ExportedChainMap.value("chainMap", Json{});
    if (!ChainMap.is_array() || ChainMap.empty())
    {
        OutError = "UE FK Chains op has no explicit chain map";
        return false;
    }
    std::unordered_map<std::string, bool> FkEnabled;
    const Json FkSettings =
        FkOp->value("settings", Json{}).value(
            "chainsToRetarget", Json{});
    if (FkSettings.is_array())
    {
        for (const Json& Entry : FkSettings)
        {
            FkEnabled[Entry.value("targetChainName", "")] =
                Entry.value("enableFK", false);
        }
    }
    std::unordered_map<std::string, bool> IkEnabled;
    if (IkOp != nullptr && IkOp->value("enabled", false))
    {
        const Json IkSettings =
            IkOp->value("settings", Json{}).value("chains", Json{});
        if (IkSettings.is_array())
        {
            for (const Json& Entry : IkSettings)
            {
                IkEnabled[Entry.value("targetChainName", "")] =
                    Entry.value("bEnableIK", false);
            }
        }
    }
    std::set<std::string> CanonicalNames;
    std::set<std::string> ActualNames;
    for (const Json& Entry : ChainMap)
    {
        AlignmentMapEntry Mapping;
        Mapping.CanonicalChain = Entry.value(
            Entry.contains("sourceChain")
                ? "sourceChain"
                : "sourceChainName",
            "");
        Mapping.ActualChain = Entry.value(
            Entry.contains("targetChain")
                ? "targetChain"
                : "targetChainName",
            "");
        Mapping.EnableFk =
            FkEnabled.find(Mapping.ActualChain) == FkEnabled.end()
            ? true
            : FkEnabled.at(Mapping.ActualChain);
        Mapping.EnableIk =
            IkEnabled.find(Mapping.ActualChain) != IkEnabled.end() &&
            IkEnabled.at(Mapping.ActualChain);
        if (Mapping.CanonicalChain.empty() ||
            Mapping.ActualChain.empty() ||
            !CanonicalNames.insert(Mapping.CanonicalChain).second ||
            !ActualNames.insert(Mapping.ActualChain).second)
        {
            OutError = "UE chain map is missing or duplicates a chain";
            return false;
        }
        Out.push_back(std::move(Mapping));
    }
    return true;
}

double ChainLength(
    const UEIKJsonRig& Rig,
    const std::vector<int>& Indices)
{
    double Result = 0.0;
    for (std::size_t Index = 1; Index < Indices.size(); ++Index)
    {
        const Vec3 A = Rig.Bones[static_cast<std::size_t>(
            Indices[Index - 1])].RetargetModel.TranslationCm;
        const Vec3 B = Rig.Bones[static_cast<std::size_t>(
            Indices[Index])].RetargetModel.TranslationCm;
        Result += Length(Subtract(B, A));
    }
    return Result;
}

Quat Slerp(Quat A, Quat B, double Alpha)
{
    A = Normalize(A);
    B = Normalize(B);
    double Dot = A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W;
    if (Dot < 0.0)
    {
        B = {-B.X, -B.Y, -B.Z, -B.W};
        Dot = -Dot;
    }
    Dot = std::max(-1.0, std::min(1.0, Dot));
    if (Dot > 0.9995)
    {
        return Normalize({
            A.X + Alpha * (B.X - A.X),
            A.Y + Alpha * (B.Y - A.Y),
            A.Z + Alpha * (B.Z - A.Z),
            A.W + Alpha * (B.W - A.W)});
    }
    const double Theta = std::acos(Dot);
    const double SinTheta = std::sin(Theta);
    const double WeightA = std::sin((1.0 - Alpha) * Theta) / SinTheta;
    const double WeightB = std::sin(Alpha * Theta) / SinTheta;
    return Normalize({
        A.X * WeightA + B.X * WeightB,
        A.Y * WeightA + B.Y * WeightB,
        A.Z * WeightA + B.Z * WeightB,
        A.W * WeightA + B.W * WeightB});
}

std::vector<double> CalculateChainParameters(
    const UEIKJsonRig& Rig,
    const std::vector<int>& BoneIndices)
{
    std::vector<double> Parameters(BoneIndices.size(), 0.0);
    if (BoneIndices.size() == 1)
    {
        Parameters[0] = 1.0;
        return Parameters;
    }
    double TotalLength = 0.0;
    for (std::size_t Index = 1;
         Index < BoneIndices.size(); ++Index)
    {
        TotalLength += Length(Subtract(
            Rig.Bones[static_cast<std::size_t>(
                BoneIndices[Index])].RetargetModel.TranslationCm,
            Rig.Bones[static_cast<std::size_t>(
                BoneIndices[Index - 1])].RetargetModel.TranslationCm));
        Parameters[Index] = TotalLength;
    }
    const double Divisor =
        std::max(TotalLength, Epsilon);
    for (double& Parameter : Parameters)
        Parameter /= Divisor;
    return Parameters;
}

TransformRT SampleChainTransform(
    const std::vector<TransformRT>& Transforms,
    const std::vector<double>& Parameters,
    const double Parameter)
{
    if (Transforms.size() == 1)
        return Transforms.front();
    if (Parameter <= Epsilon)
        return Transforms.front();
    if (Parameter >= 1.0 - Epsilon)
        return Transforms.back();
    for (std::size_t Index = 1;
         Index < Parameters.size(); ++Index)
    {
        if (Parameters[Index] <= Parameter)
            continue;
        const double Previous = Parameters[Index - 1];
        const double Current = Parameters[Index];
        const double Alpha =
            Current - Previous > Epsilon
            ? (Parameter - Previous) /
                (Current - Previous)
            : 0.0;
        TransformRT Result;
        Result.TranslationCm = core::math::Add(
            Transforms[Index - 1].TranslationCm,
            Scale(
                Subtract(
                    Transforms[Index].TranslationCm,
                    Transforms[Index - 1].TranslationCm),
                Alpha));
        Result.Rotation = Slerp(
            Transforms[Index - 1].Rotation,
            Transforms[Index].Rotation,
            Alpha);
        Result.Scale = {1.0, 1.0, 1.0};
        return Result;
    }
    return Transforms.back();
}

bool Finite(const TransformRT& Value)
{
    return std::isfinite(Value.TranslationCm.X) &&
        std::isfinite(Value.TranslationCm.Y) &&
        std::isfinite(Value.TranslationCm.Z) &&
        std::isfinite(Value.Rotation.X) &&
        std::isfinite(Value.Rotation.Y) &&
        std::isfinite(Value.Rotation.Z) &&
        std::isfinite(Value.Rotation.W) &&
        std::isfinite(Value.Scale.X) &&
        std::isfinite(Value.Scale.Y) &&
        std::isfinite(Value.Scale.Z);
}

bool ModelDelta(
    const TransformRT& Current,
    const TransformRT& Rest,
    TransformRT& Out)
{
    TransformRT RestInverse;
    if (!core::math::InverseUnitScaleTransform(Rest, RestInverse))
        return false;
    Out = Compose(Current, RestInverse);
    return Finite(Out);
}

std::string SkeletonHash(const UEIKJsonRoute& Route)
{
    return Route.RouteId + "|" + Route.TargetRig.AssetObjectPath;
}
} // namespace

UEIKJsonRouteLoadResult LoadUEIKJsonCanonicalBridgeRoute(
    const UEIKJsonCanonicalBridgeLoadOptions& Options)
{
    UEIKJsonRouteLoadResult Result;
    try
    {
    Json SourceRigJson;
    Json TargetRigJson;
    Json SourceRtgJson;
    Json TargetRtgJson;
    std::string Error;
    if (!ReadJson(Options.SourceRigJson, SourceRigJson, Error) ||
        !ReadJson(Options.TargetRigJson, TargetRigJson, Error) ||
        !ReadJson(
            Options.SourceAlignmentRetargeterJson,
            SourceRtgJson, Error) ||
        !ReadJson(
            Options.TargetAlignmentRetargeterJson,
            TargetRtgJson, Error))
    {
        Result.Errors.push_back(Error);
        return Result;
    }
    if (!ParseRig(SourceRigJson, Result.Route.SourceRig, Error) ||
        !ParseRig(TargetRigJson, Result.Route.TargetRig, Error) ||
        !HasExportIdentity(
            SourceRtgJson, "ikRetargeter", Error) ||
        !HasExportIdentity(
            TargetRtgJson, "ikRetargeter", Error))
    {
        Result.Errors.push_back(Error);
        return Result;
    }
    const Json SourceCanonical = SourceRtgJson.value("source", Json{});
    const Json SourceActual = SourceRtgJson.value("target", Json{});
    const Json TargetCanonical = TargetRtgJson.value("source", Json{});
    const Json TargetActual = TargetRtgJson.value("target", Json{});
    const std::string SourceCanonicalRig =
        SourceCanonical.value("ikRig", Json{}).value("objectPath", "");
    const std::string TargetCanonicalRig =
        TargetCanonical.value("ikRig", Json{}).value("objectPath", "");
    const std::string SourceActualRig =
        SourceActual.value("ikRig", Json{}).value("objectPath", "");
    const std::string TargetActualRig =
        TargetActual.value("ikRig", Json{}).value("objectPath", "");
    if (SourceCanonicalRig.empty() ||
        SourceCanonicalRig != TargetCanonicalRig ||
        SourceActualRig != Result.Route.SourceRig.AssetObjectPath ||
        TargetActualRig != Result.Route.TargetRig.AssetObjectPath)
    {
        Result.Errors.push_back(
            "canonical bridge RTG/IK Rig asset identities do not match");
        return Result;
    }
    Result.Route.CanonicalRigObjectPath = SourceCanonicalRig;
    if (!ApplyResolvedPose(
            SourceActual, Result.Route.SourceRig,
            Result.Route.SourcePoseName, Error) ||
        !ApplyResolvedPose(
            TargetActual, Result.Route.TargetRig,
            Result.Route.TargetPoseName, Error))
    {
        Result.Errors.push_back(Error);
        return Result;
    }
    const std::string SourceCanonicalPose =
        SourceCanonical.value("currentPoseName", "");
    const std::string TargetCanonicalPose =
        TargetCanonical.value("currentPoseName", "");
    if (SourceCanonicalPose.empty() ||
        SourceCanonicalPose != TargetCanonicalPose)
    {
        Result.Errors.push_back(
            "canonical bridge RTGs do not use the same canonical pose");
        return Result;
    }

    std::vector<AlignmentMapEntry> SourceMap;
    std::vector<AlignmentMapEntry> TargetMap;
    if (!ParseAlignmentMap(SourceRtgJson, SourceMap, Error) ||
        !ParseAlignmentMap(TargetRtgJson, TargetMap, Error))
    {
        Result.Errors.push_back(Error);
        return Result;
    }
    std::unordered_map<std::string, AlignmentMapEntry> SourceByCanonical;
    for (const AlignmentMapEntry& Entry : SourceMap)
        SourceByCanonical.emplace(Entry.CanonicalChain, Entry);
    for (const AlignmentMapEntry& TargetEntry : TargetMap)
    {
        const auto SourceIt =
            SourceByCanonical.find(TargetEntry.CanonicalChain);
        if (SourceIt == SourceByCanonical.end()) continue;
        const AlignmentMapEntry& SourceEntry = SourceIt->second;
        const UEIKJsonChain* SourceChain =
            FindChain(Result.Route.SourceRig, SourceEntry.ActualChain);
        const UEIKJsonChain* TargetChain =
            FindChain(Result.Route.TargetRig, TargetEntry.ActualChain);
        if (SourceChain == nullptr || TargetChain == nullptr)
        {
            Result.Errors.push_back(
                "mapped chain is absent from exported IK Rig: " +
                TargetEntry.CanonicalChain);
            continue;
        }
        UEIKJsonChainPair Pair;
        Pair.CanonicalChainName = TargetEntry.CanonicalChain;
        Pair.SourceChainName = SourceEntry.ActualChain;
        Pair.TargetChainName = TargetEntry.ActualChain;
        Pair.SourceBoneIndices = SourceChain->BoneIndices;
        Pair.TargetBoneIndices = TargetChain->BoneIndices;
        Pair.SourceGoalName = SourceChain->GoalName;
        Pair.TargetGoalName = TargetChain->GoalName;
        Pair.EnableFk = SourceEntry.EnableFk && TargetEntry.EnableFk;
        Pair.EnableIk = SourceEntry.EnableIk && TargetEntry.EnableIk &&
            Pair.SourceBoneIndices.size() >= 3 &&
            Pair.TargetBoneIndices.size() >= 3;
        const double SourceLength =
            ChainLength(Result.Route.SourceRig, Pair.SourceBoneIndices);
        const double TargetLength =
            ChainLength(Result.Route.TargetRig, Pair.TargetBoneIndices);
        Pair.LengthScale =
            SourceLength > Epsilon && TargetLength > Epsilon
            ? TargetLength / SourceLength
            : 1.0;
        Result.Route.ChainPairs.push_back(std::move(Pair));
    }
    if (!Result.Errors.empty()) return Result;
    if (Result.Route.ChainPairs.empty())
    {
        Result.Errors.push_back(
            "canonical bridge produced no explicit chain pairs");
        return Result;
    }

    Result.Route.SourceRootIndex = FindBone(
        Result.Route.SourceRig.Bones,
        Result.Route.SourceRig.RetargetRootBone);
    Result.Route.SourcePelvisIndex = FindBone(
        Result.Route.SourceRig.Bones,
        Result.Route.SourceRig.RetargetPelvisBone);
    Result.Route.TargetRootIndex = FindBone(
        Result.Route.TargetRig.Bones,
        Result.Route.TargetRig.RetargetRootBone);
    Result.Route.TargetPelvisIndex = FindBone(
        Result.Route.TargetRig.Bones,
        Result.Route.TargetRig.RetargetPelvisBone);

    if (Result.Route.SourcePelvisIndex < 0 ||
        Result.Route.TargetPelvisIndex < 0)
    {
        Result.Errors.push_back(
            "canonical bridge pelvis indices are invalid");
        return Result;
    }
    const double SourcePelvisHeight =
        Result.Route.SourceRig.Bones[
            static_cast<std::size_t>(
                Result.Route.SourcePelvisIndex)]
            .RetargetModel.TranslationCm.Z;
    const double TargetPelvisHeight =
        Result.Route.TargetRig.Bones[
            static_cast<std::size_t>(
                Result.Route.TargetPelvisIndex)]
            .RetargetModel.TranslationCm.Z;
    Result.Route.GlobalTranslationScale =
        SourcePelvisHeight > Epsilon
        ? TargetPelvisHeight / SourcePelvisHeight
        : 0.0;
    if (!std::isfinite(Result.Route.GlobalTranslationScale) ||
        Result.Route.GlobalTranslationScale <= Epsilon)
    {
        Result.Errors.push_back(
            "canonical bridge translation scale is invalid");
        return Result;
    }
    Result.Warnings.push_back(
        "This is an explicit UE-JSON canonical bridge test route. "
        "It is not marked selected or adopted.");
    Result.Success = true;
    return Result;
    }
    catch (const std::exception& Error)
    {
        Result.Success = false;
        Result.Errors.push_back(
            std::string("invalid UE JSON canonical bridge data: ") +
            Error.what());
        return Result;
    }
}

bool BuildModelPose(
    const core::skeleton::NormalizedRuntimeSkeleton& Skeleton,
    const PoseBuffer& LocalPose,
    PoseBuffer& OutModelPose)
{
    if (LocalPose.Space() != PoseSpace::Local ||
        !LocalPose.IsSizedFor(Skeleton) ||
        !Skeleton.ValidateParentIndexInvariant())
    {
        return false;
    }
    PoseBuffer Model(PoseSpace::Model, LocalPose.SkeletonHash());
    Model.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0; Index < Skeleton.BoneCount(); ++Index)
    {
        const int Parent = Skeleton.BoneAt(Index).ParentIndex;
        Model[Index] = Parent < 0
            ? LocalPose[Index]
            : Compose(
                Model[static_cast<std::size_t>(Parent)],
                LocalPose[Index]);
        if (!Finite(Model[Index])) return false;
    }
    OutModelPose = std::move(Model);
    return true;
}

core::skeleton::NormalizedRuntimeSkeleton BuildTargetRuntimeSkeleton(
    const UEIKJsonRoute& Route)
{
    core::skeleton::NormalizedRuntimeSkeleton Result;
    core::skeleton::SkeletonIdentity Identity;
    Identity.HierarchyHash = SkeletonHash(Route);
    Identity.RestPoseHash =
        Route.FoundationRouteId + "|" + Route.TargetPoseName;
    Identity.SourceAssetId = Route.TargetRig.AssetObjectPath;
    Result.SetIdentity(std::move(Identity));
    for (const UEIKJsonBone& Bone : Route.TargetRig.Bones)
    {
        core::skeleton::RuntimeBone Runtime;
        Runtime.Name = Bone.Name;
        Runtime.RawPath = Bone.Name;
        Runtime.ParentIndex = Bone.ParentIndex;
        Runtime.RawIndex = Bone.Index;
        Runtime.LocalRest = Bone.RetargetLocal;
        Result.AddBone(std::move(Runtime));
    }
    std::vector<int> IdentityMap(Route.TargetRig.Bones.size());
    for (std::size_t Index = 0; Index < IdentityMap.size(); ++Index)
        IdentityMap[Index] = static_cast<int>(Index);
    Result.SetRawToNormalized(IdentityMap);
    Result.SetNormalizedToRaw(std::move(IdentityMap));
    return Result;
}

UEIKJsonSolveResult SolveUEIKJsonRouteFrame(
    const UEIKJsonRoute& Route,
    const std::vector<TransformRT>& SourceCurrentLocalPose)
{
    UEIKJsonSolveResult Result;
    if (SourceCurrentLocalPose.size() != Route.SourceRig.Bones.size())
    {
        Result.Errors.push_back(
            "source frame does not match the UE IK Rig skeleton");
        return Result;
    }
    std::vector<TransformRT> SourceCurrentModel(
        SourceCurrentLocalPose.size());
    for (std::size_t Index = 0;
         Index < SourceCurrentLocalPose.size(); ++Index)
    {
        if (!Finite(SourceCurrentLocalPose[Index]))
        {
            Result.Errors.push_back(
                "source frame contains a non-finite transform");
            return Result;
        }
        const int Parent =
            Route.SourceRig.Bones[Index].ParentIndex;
        SourceCurrentModel[Index] = Parent < 0
            ? SourceCurrentLocalPose[Index]
            : Compose(
                SourceCurrentModel[
                    static_cast<std::size_t>(Parent)],
                SourceCurrentLocalPose[Index]);
    }

    const core::skeleton::NormalizedRuntimeSkeleton Skeleton =
        BuildTargetRuntimeSkeleton(Route);
    const std::string Hash = Skeleton.GetIdentity().HierarchyHash;
    PoseBuffer TargetLocal(PoseSpace::Local, Hash);
    TargetLocal.ResizeToSkeleton(Skeleton);
    for (std::size_t Index = 0;
         Index < Route.TargetRig.Bones.size(); ++Index)
    {
        TargetLocal[Index] =
            Route.TargetRig.Bones[Index].RetargetLocal;
        TargetLocal[Index].Scale = {1.0, 1.0, 1.0};
    }

    if (Route.SourcePelvisIndex < 0 ||
        Route.TargetPelvisIndex < 0)
    {
        Result.Errors.push_back(
            "route pelvis indices are invalid");
        return Result;
    }
    const std::size_t SourcePelvis =
        static_cast<std::size_t>(Route.SourcePelvisIndex);
    const std::size_t TargetPelvis =
        static_cast<std::size_t>(Route.TargetPelvisIndex);

    PoseBuffer RunningModel(PoseSpace::Model, Hash);
    if (!BuildModelPose(Skeleton, TargetLocal, RunningModel))
    {
        Result.Errors.push_back(
            "target retarget pose model rebuild failed");
        return Result;
    }

    // UE 5.8 PelvisMotionOp uses the source/target retarget-pose
    // pelvis heights as a uniform global scale, then applies the source
    // pelvis model rotation delta to the target retarget pelvis.
    TransformRT PelvisDelta;
    if (!ModelDelta(
            SourceCurrentModel[SourcePelvis],
            Route.SourceRig.Bones[SourcePelvis].RetargetModel,
            PelvisDelta))
    {
        Result.Errors.push_back(
            "source pelvis model delta could not be computed");
        return Result;
    }
    TransformRT TargetPelvisModel =
        Route.TargetRig.Bones[TargetPelvis].RetargetModel;
    TargetPelvisModel.TranslationCm = Scale(
        SourceCurrentModel[SourcePelvis].TranslationCm,
        Route.GlobalTranslationScale);
    TargetPelvisModel.Rotation = Multiply(
        PelvisDelta.Rotation,
        Route.TargetRig.Bones[TargetPelvis]
            .RetargetModel.Rotation);
    TargetPelvisModel.Scale = {1.0, 1.0, 1.0};
    const int TargetPelvisParent =
        Route.TargetRig.Bones[TargetPelvis].ParentIndex;
    if (TargetPelvisParent < 0)
    {
        TargetLocal[TargetPelvis] = TargetPelvisModel;
    }
    else if (!RelativeUnitScaleTransform(
                 RunningModel[static_cast<std::size_t>(
                     TargetPelvisParent)],
                 TargetPelvisModel,
                 TargetLocal[TargetPelvis]))
    {
        Result.Errors.push_back(
            "target pelvis model transform could not be converted to local space");
        return Result;
    }
    if (!BuildModelPose(Skeleton, TargetLocal, RunningModel))
    {
        Result.Errors.push_back(
            "target pose rebuild failed after pelvis motion");
        return Result;
    }

    std::vector<const UEIKJsonChainPair*> FkPairs;
    for (const UEIKJsonChainPair& Pair : Route.ChainPairs)
    {
        if (Pair.EnableFk &&
            !Pair.SourceBoneIndices.empty() &&
            !Pair.TargetBoneIndices.empty())
        {
            FkPairs.push_back(&Pair);
        }
    }
    std::sort(
        FkPairs.begin(), FkPairs.end(),
        [](const UEIKJsonChainPair* Left,
           const UEIKJsonChainPair* Right)
        {
            const int LeftStart =
                Left->TargetBoneIndices.front();
            const int RightStart =
                Right->TargetBoneIndices.front();
            return LeftStart == RightStart
                ? Left->TargetChainName <
                    Right->TargetChainName
                : LeftStart < RightStart;
        });

    // Mirror UE 5.8 FKChainsOp Interpolated/Translation=None:
    // re-parent each source chain under the target chain's current
    // parent while preserving the source-vs-target retarget-pose
    // parent offset, then transfer the sampled model rotation delta.
    for (const UEIKJsonChainPair* PairPointer : FkPairs)
    {
        const UEIKJsonChainPair& Pair = *PairPointer;
        const int SourceFirst =
            Pair.SourceBoneIndices.front();
        const int TargetFirst =
            Pair.TargetBoneIndices.front();
        const int SourceParent =
            Route.SourceRig.Bones[
                static_cast<std::size_t>(SourceFirst)]
                .ParentIndex;
        const int TargetParent =
            Route.TargetRig.Bones[
                static_cast<std::size_t>(TargetFirst)]
                .ParentIndex;
        const TransformRT SourceParentInitial =
            SourceParent < 0
            ? TransformRT{}
            : Route.SourceRig.Bones[
                static_cast<std::size_t>(SourceParent)]
                .RetargetModel;
        const TransformRT TargetParentInitial =
            TargetParent < 0
            ? TransformRT{}
            : Route.TargetRig.Bones[
                static_cast<std::size_t>(TargetParent)]
                .RetargetModel;
        const TransformRT TargetParentCurrent =
            TargetParent < 0
            ? TransformRT{}
            : RunningModel[
                static_cast<std::size_t>(TargetParent)];
        TransformRT SourceParentInitialDelta;
        if (!RelativeUnitScaleTransform(
                TargetParentInitial,
                SourceParentInitial,
                SourceParentInitialDelta))
        {
            Result.Errors.push_back(
                "FK chain parent retarget-pose delta failed: " +
                Pair.TargetChainName);
            return Result;
        }
        const TransformRT RebasedSourceParent =
            Compose(
                TargetParentCurrent,
                SourceParentInitialDelta);

        std::vector<TransformRT> SourceCurrentChain;
        std::vector<TransformRT> SourceInitialChain;
        SourceCurrentChain.reserve(
            Pair.SourceBoneIndices.size());
        SourceInitialChain.reserve(
            Pair.SourceBoneIndices.size());
        for (std::size_t Ordinal = 0;
             Ordinal < Pair.SourceBoneIndices.size();
             ++Ordinal)
        {
            const std::size_t SourceIndex =
                static_cast<std::size_t>(
                    Pair.SourceBoneIndices[Ordinal]);
            SourceCurrentChain.push_back(
                Compose(
                    Ordinal == 0
                        ? RebasedSourceParent
                        : SourceCurrentChain.back(),
                    SourceCurrentLocalPose[SourceIndex]));
            SourceInitialChain.push_back(
                Route.SourceRig.Bones[SourceIndex]
                    .RetargetModel);
        }
        const std::vector<double> SourceParameters =
            CalculateChainParameters(
                Route.SourceRig,
                Pair.SourceBoneIndices);
        const std::vector<double> TargetParameters =
            CalculateChainParameters(
                Route.TargetRig,
                Pair.TargetBoneIndices);

        for (std::size_t TargetOrdinal = 0;
             TargetOrdinal < Pair.TargetBoneIndices.size();
             ++TargetOrdinal)
        {
            const TransformRT SourceCurrent =
                SampleChainTransform(
                    SourceCurrentChain,
                    SourceParameters,
                    TargetParameters[TargetOrdinal]);
            const TransformRT SourceInitial =
                SampleChainTransform(
                    SourceInitialChain,
                    SourceParameters,
                    TargetParameters[TargetOrdinal]);
            const Quat RotationDelta = Multiply(
                SourceCurrent.Rotation,
                Conjugate(SourceInitial.Rotation));
            const std::size_t TargetIndex =
                static_cast<std::size_t>(
                    Pair.TargetBoneIndices[
                        TargetOrdinal]);
            const Quat DesiredModelRotation =
                Normalize(Multiply(
                    RotationDelta,
                    Route.TargetRig.Bones[TargetIndex]
                        .RetargetModel.Rotation));
            const int Parent =
                Route.TargetRig.Bones[TargetIndex]
                    .ParentIndex;
            TargetLocal[TargetIndex].Rotation =
                Parent < 0
                ? DesiredModelRotation
                : Normalize(Multiply(
                    Conjugate(
                        RunningModel[
                            static_cast<std::size_t>(
                                Parent)]
                            .Rotation),
                    DesiredModelRotation));
            TargetLocal[TargetIndex].TranslationCm =
                Route.TargetRig.Bones[TargetIndex]
                    .RetargetLocal.TranslationCm;
            TargetLocal[TargetIndex].Scale =
                {1.0, 1.0, 1.0};
            RunningModel[TargetIndex] =
                Parent < 0
                ? TargetLocal[TargetIndex]
                : Compose(
                    RunningModel[
                        static_cast<std::size_t>(Parent)],
                    TargetLocal[TargetIndex]);
        }
        if (!BuildModelPose(
                Skeleton, TargetLocal, RunningModel))
        {
            Result.Errors.push_back(
                "target pose rebuild failed after FK chain: " +
                Pair.TargetChainName);
            return Result;
        }
    }
    Result.TargetFkLocalPose = TargetLocal;
    Result.TargetFkModelPose = RunningModel;

    for (const UEIKJsonChainPair& Pair : Route.ChainPairs)
    {
        if (!Pair.EnableIk ||
            Pair.SourceBoneIndices.size() < 3 ||
            Pair.TargetBoneIndices.size() < 3)
        {
            continue;
        }
        const int SourceChainRoot =
            Pair.SourceBoneIndices.front();
        const int SourceChainMid =
            Pair.SourceBoneIndices[
                Pair.SourceBoneIndices.size() / 2];
        const int SourceEnd = Pair.SourceBoneIndices.back();
        const int TargetChainRoot = Pair.TargetBoneIndices.front();
        const int TargetChainMid =
            Pair.TargetBoneIndices[Pair.TargetBoneIndices.size() / 2];
        const int TargetChainEnd = Pair.TargetBoneIndices.back();
        if (Skeleton.BoneAt(
                static_cast<std::size_t>(TargetChainMid))
                .ParentIndex != TargetChainRoot ||
            Skeleton.BoneAt(
                static_cast<std::size_t>(TargetChainEnd))
                .ParentIndex != TargetChainMid)
        {
            ++Result.FailedIkChainCount;
            Result.Errors.push_back(
                "Run IK Rig chain is not an analytic two-bone chain: " +
                Pair.TargetChainName);
            return Result;
        }

        TransformRT EndDelta;
        if (!ModelDelta(
                SourceCurrentModel[
                    static_cast<std::size_t>(SourceEnd)],
                Route.SourceRig.Bones[
                    static_cast<std::size_t>(SourceEnd)]
                    .RetargetModel,
                EndDelta))
        {
            ++Result.FailedIkChainCount;
            Result.Errors.push_back(
                "source IK end delta failed: " +
                Pair.SourceChainName);
            return Result;
        }
        PoseBuffer CurrentModel;
        if (!BuildModelPose(Skeleton, TargetLocal, CurrentModel))
        {
            Result.Errors.push_back(
                "target model pose rebuild failed before IK");
            return Result;
        }
        TransformRT DesiredEnd =
            Route.TargetRig.Bones[
                static_cast<std::size_t>(TargetChainEnd)]
                .RetargetModel;
        DesiredEnd.TranslationCm = core::math::Add(
            CurrentModel[
                static_cast<std::size_t>(TargetChainRoot)]
                .TranslationCm,
            Scale(
                Subtract(
                    SourceCurrentModel[
                        static_cast<std::size_t>(SourceEnd)]
                        .TranslationCm,
                    SourceCurrentModel[
                        static_cast<std::size_t>(
                            SourceChainRoot)]
                        .TranslationCm),
                Pair.LengthScale));
        DesiredEnd.Rotation = Multiply(
            EndDelta.Rotation,
            Route.TargetRig.Bones[
                static_cast<std::size_t>(TargetChainEnd)]
                .RetargetModel.Rotation);
        const Vec3 DesiredPole = core::math::Add(
            CurrentModel[
                static_cast<std::size_t>(TargetChainRoot)]
                .TranslationCm,
            Scale(
                Subtract(
                    SourceCurrentModel[
                        static_cast<std::size_t>(SourceChainMid)]
                        .TranslationCm,
                    SourceCurrentModel[
                        static_cast<std::size_t>(
                            SourceChainRoot)]
                        .TranslationCm),
                Pair.LengthScale));
        TwoBonePoseBufferRequest Request;
        Request.Chain = {
            TargetChainRoot,
            TargetChainMid,
            TargetChainEnd,
            TargetChainRoot,
            TargetChainMid};
        Request.TargetPositionModelCm =
            DesiredEnd.TranslationCm;
        Request.PolePositionModelCm = DesiredPole;
        Request.PoleFallback =
            PoleFallbackPolicy::AllowConfiguredAxis;
        Request.ConfiguredFallbackAxisModel =
            Pair.CanonicalChainName.find("Arm") !=
                    std::string::npos
                ? Vec3{0.0, 0.0, 1.0}
                : Vec3{0.0, 1.0, 0.0};
        Request.OrientationPolicy =
            EndOrientationPolicy::ApplyExplicitModel;
        Request.ExplicitEndModelOrientation =
            DesiredEnd.Rotation;
        Request.PositionToleranceCm = 1.0e-4;
        Request.LengthToleranceCm = 1.0e-5;
        const auto Applied =
            ApplyAnalyticTwoBoneToPoseBuffer(
                Skeleton, Request, TargetLocal);
        if (!Applied.Success)
        {
            ++Result.FailedIkChainCount;
            Result.Errors.push_back(
                "Run IK Rig solve failed for " +
                Pair.TargetChainName + ": " + Applied.Message);
            return Result;
        }
        if (!std::isfinite(Applied.Telemetry.EndpointErrorCm) ||
            Applied.Telemetry.EndpointErrorCm > 0.05)
        {
            const double SourceReach = Length(Subtract(
                SourceCurrentModel[
                    static_cast<std::size_t>(SourceEnd)]
                    .TranslationCm,
                SourceCurrentModel[
                    static_cast<std::size_t>(SourceChainRoot)]
                    .TranslationCm));
            const double DesiredReach = Length(Subtract(
                DesiredEnd.TranslationCm,
                CurrentModel[
                    static_cast<std::size_t>(TargetChainRoot)]
                    .TranslationCm));
            const double TargetReach =
                Length(Subtract(
                    CurrentModel[
                        static_cast<std::size_t>(TargetChainMid)]
                        .TranslationCm,
                    CurrentModel[
                        static_cast<std::size_t>(TargetChainRoot)]
                        .TranslationCm)) +
                Length(Subtract(
                    CurrentModel[
                        static_cast<std::size_t>(TargetChainEnd)]
                        .TranslationCm,
                    CurrentModel[
                        static_cast<std::size_t>(TargetChainMid)]
                        .TranslationCm));
            ++Result.FailedIkChainCount;
            Result.Errors.push_back(
                "Run IK Rig endpoint error exceeds 0.05 cm for " +
                Pair.TargetChainName + ": " +
                std::to_string(
                    Applied.Telemetry.EndpointErrorCm) +
                " source_reach_cm=" +
                std::to_string(SourceReach) +
                " length_scale=" +
                std::to_string(Pair.LengthScale) +
                " desired_reach_cm=" +
                std::to_string(DesiredReach) +
                " target_reach_cm=" +
                std::to_string(TargetReach));
            return Result;
        }
        ++Result.AppliedIkChainCount;
        Result.MaximumIkEndpointErrorCm = std::max(
            Result.MaximumIkEndpointErrorCm,
            Applied.Telemetry.EndpointErrorCm);
    }
    Result.TargetFoundationLocalPose = TargetLocal;
    if (!BuildModelPose(
            Skeleton, Result.TargetFoundationLocalPose,
            Result.TargetFoundationModelPose))
    {
        Result.Errors.push_back(
            "target Foundation model pose rebuild failed");
        return Result;
    }
    Result.Success = true;
    return Result;
}
} // namespace skrtg::retarget
