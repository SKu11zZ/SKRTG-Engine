#include "skrtg/fbx/ue_fbx_import_exact.h"

#include "skrtg/core/math/transform.h"

#include <fbxsdk.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace skrtg::fbx
{
namespace
{
using Json = nlohmann::json;
using core::math::Compose;
using core::math::Length;
using core::math::Normalize;
using core::math::Quat;
using core::math::RelativeUnitScaleTransform;
using core::math::Subtract;
using core::math::TransformRT;
using core::math::Vec3;

constexpr double Pi = 3.1415926535897932384626433832795;

struct LoadedScene
{
    FbxManager* Manager = nullptr;
    FbxScene* Scene = nullptr;

    LoadedScene() = default;
    LoadedScene(const LoadedScene&) = delete;
    LoadedScene& operator=(const LoadedScene&) = delete;

    ~LoadedScene()
    {
        if (Manager != nullptr)
            Manager->Destroy();
    }
};

struct ImportSettings
{
    bool ConvertScene = true;
    bool ForceFrontXAxis = false;
    bool ConvertSceneUnit = true;
    bool BakeMeshes = false;
    bool UseT0AsReferencePose = false;
    std::string Source;
};

struct BindModel
{
    std::vector<TransformRT> ModelPose;
    std::vector<std::string> Sources;
    int BindPoseBoneCount = 0;
    int SkinClusterBoneCount = 0;
    int EvaluatorFallbackBoneCount = 0;
    double MaximumCandidateTranslationCm = 0.0;
    double MaximumCandidateRotationDegrees = 0.0;
    double MaximumCandidateScale = 0.0;
};

std::string UpperAscii(std::string Value)
{
    for (char& Character : Value)
    {
        if (Character >= 'a' && Character <= 'z')
            Character = static_cast<char>(
                Character - 'a' + 'A');
    }
    return Value;
}

bool IsSha256(const std::string& Value)
{
    if (Value.size() != 64)
        return false;
    return std::all_of(
        Value.begin(),
        Value.end(),
        [](const char Character)
        {
            return
                (Character >= '0' && Character <= '9') ||
                (Character >= 'a' && Character <= 'f') ||
                (Character >= 'A' && Character <= 'F');
        });
}

bool ComputeSha256(
    const std::filesystem::path& Path,
    std::string& OutHash,
    std::string& OutError)
{
#if defined(_WIN32)
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError =
            "failed to open input for SHA256: " +
            Path.string();
        return false;
    }

    BCRYPT_ALG_HANDLE Algorithm = nullptr;
    BCRYPT_HASH_HANDLE Hash = nullptr;
    DWORD ObjectLength = 0;
    DWORD HashLength = 0;
    DWORD ResultLength = 0;
    std::vector<unsigned char> HashObject;
    std::vector<unsigned char> Digest;
    const auto Cleanup = [&]()
    {
        if (Hash != nullptr)
            BCryptDestroyHash(Hash);
        if (Algorithm != nullptr)
            BCryptCloseAlgorithmProvider(Algorithm, 0);
    };

    NTSTATUS Status = BCryptOpenAlgorithmProvider(
        &Algorithm,
        BCRYPT_SHA256_ALGORITHM,
        nullptr,
        0);
    if (Status < 0)
    {
        OutError = "BCryptOpenAlgorithmProvider failed";
        Cleanup();
        return false;
    }
    Status = BCryptGetProperty(
        Algorithm,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&ObjectLength),
        sizeof(ObjectLength),
        &ResultLength,
        0);
    if (Status < 0)
    {
        OutError =
            "BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed";
        Cleanup();
        return false;
    }
    Status = BCryptGetProperty(
        Algorithm,
        BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&HashLength),
        sizeof(HashLength),
        &ResultLength,
        0);
    if (Status < 0)
    {
        OutError =
            "BCryptGetProperty(BCRYPT_HASH_LENGTH) failed";
        Cleanup();
        return false;
    }

    HashObject.resize(ObjectLength);
    Digest.resize(HashLength);
    Status = BCryptCreateHash(
        Algorithm,
        &Hash,
        HashObject.data(),
        ObjectLength,
        nullptr,
        0,
        0);
    if (Status < 0)
    {
        OutError = "BCryptCreateHash failed";
        Cleanup();
        return false;
    }

    std::array<unsigned char, 64 * 1024> Buffer{};
    while (Input)
    {
        Input.read(
            reinterpret_cast<char*>(Buffer.data()),
            static_cast<std::streamsize>(Buffer.size()));
        const std::streamsize ReadCount = Input.gcount();
        if (ReadCount <= 0)
            break;
        Status = BCryptHashData(
            Hash,
            Buffer.data(),
            static_cast<ULONG>(ReadCount),
            0);
        if (Status < 0)
        {
            OutError = "BCryptHashData failed";
            Cleanup();
            return false;
        }
    }
    if (!Input.eof())
    {
        OutError =
            "failed while reading input for SHA256: " +
            Path.string();
        Cleanup();
        return false;
    }

    Status = BCryptFinishHash(
        Hash,
        Digest.data(),
        HashLength,
        0);
    if (Status < 0)
    {
        OutError = "BCryptFinishHash failed";
        Cleanup();
        return false;
    }

    std::ostringstream Hex;
    Hex << std::uppercase << std::hex << std::setfill('0');
    for (const unsigned char Byte : Digest)
        Hex << std::setw(2) << static_cast<int>(Byte);
    OutHash = Hex.str();
    Cleanup();
    return true;
#else
    (void)Path;
    (void)OutHash;
    OutError =
        "SHA256 is implemented only for the authorized Windows toolchain";
    return false;
#endif
}

bool ReadJson(
    const std::filesystem::path& Path,
    Json& Out,
    std::string& OutError)
{
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError =
            "failed to open UE export JSON: " +
            Path.string();
        return false;
    }
    try
    {
        Out = Json::parse(Input);
        return true;
    }
    catch (const std::exception& Error)
    {
        OutError =
            "failed to parse UE export JSON: " +
            std::string(Error.what());
        return false;
    }
}

bool ParseVector(const Json& Value, Vec3& Out)
{
    if (!Value.is_object())
        return false;
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
    return
        std::isfinite(Out.X) &&
        std::isfinite(Out.Y) &&
        std::isfinite(Out.Z);
}

bool ParseQuaternion(const Json& Value, Quat& Out)
{
    if (!Value.is_object())
        return false;
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
    return
        std::isfinite(Out.X) &&
        std::isfinite(Out.Y) &&
        std::isfinite(Out.Z) &&
        std::isfinite(Out.W);
}

bool ParseTransform(const Json& Value, TransformRT& Out)
{
    return
        Value.is_object() &&
        ParseVector(
            Value.value("translation", Json{}),
            Out.TranslationCm) &&
        ParseQuaternion(
            Value.value("rotation", Json{}),
            Out.Rotation) &&
        ParseVector(
            Value.value("scale", Json{}),
            Out.Scale);
}

double QuaternionErrorDegrees(Quat Left, Quat Right)
{
    Left = Normalize(Left);
    Right = Normalize(Right);
    const double Dot = std::clamp(
        std::abs(
            Left.X * Right.X +
            Left.Y * Right.Y +
            Left.Z * Right.Z +
            Left.W * Right.W),
        0.0,
        1.0);
    return 2.0 * std::acos(Dot) * 180.0 / Pi;
}

double ScaleError(const Vec3& Left, const Vec3& Right)
{
    return std::max({
        std::abs(Left.X - Right.X),
        std::abs(Left.Y - Right.Y),
        std::abs(Left.Z - Right.Z)});
}

void IncludeMaximum(
    UEFbxImportExactMaximum& Out,
    const double Value,
    const int KeyIndex,
    const std::string& BoneName)
{
    if (Value > Out.Value)
    {
        Out.Value = Value;
        Out.KeyIndex = KeyIndex;
        Out.BoneName = BoneName;
    }
}

TransformRT ConvertFbxTransformToUE(
    const FbxAMatrix& Matrix)
{
    const FbxVector4 Translation = Matrix.GetT();
    const FbxQuaternion Rotation = Matrix.GetQ();
    const FbxVector4 Scale = Matrix.GetS();

    TransformRT Result;
    Result.TranslationCm = {
        Translation[0],
        -Translation[1],
        Translation[2]};
    Result.Rotation = Normalize({
        Rotation[0],
        -Rotation[1],
        Rotation[2],
        -Rotation[3]});
    Result.Scale = {
        Scale[0],
        Scale[1],
        Scale[2]};
    return Result;
}

TransformRT ConvertFbxTransformToUE(
    const FbxMatrix& Matrix)
{
    FbxVector4 Translation;
    FbxQuaternion Rotation;
    FbxVector4 Shearing;
    FbxVector4 Scale;
    double DeterminantSign = 1.0;
    Matrix.GetElements(
        Translation,
        Rotation,
        Shearing,
        Scale,
        DeterminantSign);
    TransformRT Result;
    Result.TranslationCm = {
        Translation[0],
        -Translation[1],
        Translation[2]};
    Result.Rotation = Normalize({
        Rotation[0],
        -Rotation[1],
        Rotation[2],
        -Rotation[3]});
    Result.Scale = {
        Scale[0] * DeterminantSign,
        Scale[1],
        Scale[2]};
    return Result;
}

std::string CanonicalBoneName(const std::string& Name)
{
    const std::size_t Colon = Name.find_last_of(':');
    return
        Colon == std::string::npos
        ? Name
        : Name.substr(Colon + 1);
}

void CollectNodes(
    FbxNode* Node,
    std::vector<FbxNode*>& Out)
{
    if (Node == nullptr)
        return;
    Out.push_back(Node);
    for (int Index = 0;
         Index < Node->GetChildCount();
         ++Index)
    {
        CollectNodes(Node->GetChild(Index), Out);
    }
}

void CollectSkinLinks(
    FbxNode* Node,
    std::set<FbxNode*>& Out)
{
    if (Node == nullptr)
        return;
    if (FbxMesh* Mesh = Node->GetMesh())
    {
        const int SkinCount =
            Mesh->GetDeformerCount(FbxDeformer::eSkin);
        for (int SkinIndex = 0;
             SkinIndex < SkinCount;
             ++SkinIndex)
        {
            FbxSkin* Skin = FbxCast<FbxSkin>(
                Mesh->GetDeformer(
                    SkinIndex,
                    FbxDeformer::eSkin));
            if (Skin == nullptr)
                continue;
            for (int ClusterIndex = 0;
                 ClusterIndex < Skin->GetClusterCount();
                 ++ClusterIndex)
            {
                FbxCluster* Cluster =
                    Skin->GetCluster(ClusterIndex);
                if (Cluster != nullptr &&
                    Cluster->GetLink() != nullptr)
                {
                    Out.insert(Cluster->GetLink());
                }
            }
        }
    }
    for (int Index = 0;
         Index < Node->GetChildCount();
         ++Index)
    {
        CollectSkinLinks(Node->GetChild(Index), Out);
    }
}

bool IsSkeletonNode(FbxNode* Node)
{
    const FbxNodeAttribute* Attribute =
        Node != nullptr
        ? Node->GetNodeAttribute()
        : nullptr;
    return
        Attribute != nullptr &&
        Attribute->GetAttributeType() ==
            FbxNodeAttribute::eSkeleton;
}

bool BindBones(
    FbxScene* Scene,
    const std::vector<UEFbxImportExactBone>& Bones,
    std::vector<FbxNode*>& OutNodes,
    std::string& OutError)
{
    if (Scene == nullptr || Bones.empty())
    {
        OutError = "FBX skeleton binding input is empty";
        return false;
    }

    std::vector<FbxNode*> AllNodes;
    CollectNodes(Scene->GetRootNode(), AllNodes);
    std::set<FbxNode*> SkinLinks;
    CollectSkinLinks(Scene->GetRootNode(), SkinLinks);
    std::unordered_map<std::string, std::vector<FbxNode*>>
        Candidates;
    std::set<std::string> RequiredNames;
    for (const UEFbxImportExactBone& Bone : Bones)
        RequiredNames.insert(Bone.Name);
    for (FbxNode* Node : AllNodes)
    {
        const std::string Name =
            Node != nullptr &&
                Node->GetName() != nullptr
            ? Node->GetName()
            : "";
        const std::string Canonical =
            CanonicalBoneName(Name);
        if (RequiredNames.find(Canonical) !=
            RequiredNames.end())
        {
            Candidates[Canonical].push_back(Node);
        }
    }

    std::vector<FbxNode*> CandidateNodes(
        Bones.size(),
        nullptr);
    for (std::size_t Index = 0;
         Index < Bones.size();
         ++Index)
    {
        const auto CandidateIt =
            Candidates.find(Bones[Index].Name);
        if (CandidateIt == Candidates.end())
        {
            OutError =
                "FBX bone is absent: " +
                Bones[Index].Name;
            return false;
        }
        std::vector<FbxNode*> Preferred;
        for (FbxNode* Candidate :
             CandidateIt->second)
        {
            if (IsSkeletonNode(Candidate) ||
                SkinLinks.find(Candidate) !=
                    SkinLinks.end())
            {
                Preferred.push_back(Candidate);
            }
        }
        const std::vector<FbxNode*>& Selection =
            Preferred.empty()
            ? CandidateIt->second
            : Preferred;
        if (Selection.size() != 1)
        {
            OutError =
                "FBX namespace-normalized bone is ambiguous: " +
                Bones[Index].Name;
            return false;
        }
        CandidateNodes[Index] = Selection.front();
    }

    std::unordered_map<FbxNode*, int> IndexByNode;
    for (std::size_t Index = 0;
         Index < CandidateNodes.size();
         ++Index)
    {
        IndexByNode.emplace(
            CandidateNodes[Index],
            static_cast<int>(Index));
    }
    for (std::size_t Index = 0;
         Index < CandidateNodes.size();
         ++Index)
    {
        int ActualParentIndex = -1;
        FbxNode* Parent =
            CandidateNodes[Index]->GetParent();
        while (Parent != nullptr)
        {
            const auto ParentIt =
                IndexByNode.find(Parent);
            if (ParentIt != IndexByNode.end())
            {
                ActualParentIndex = ParentIt->second;
                break;
            }
            Parent = Parent->GetParent();
        }
        if (ActualParentIndex !=
            Bones[Index].ParentIndex)
        {
            OutError =
                "FBX hierarchy does not match the UE reference skeleton at bone: " +
                Bones[Index].Name;
            return false;
        }
    }

    OutNodes = std::move(CandidateNodes);
    return true;
}

bool BuildBindModel(
    FbxScene* Scene,
    const std::vector<UEFbxImportExactBone>& Bones,
    const std::vector<FbxNode*>& Nodes,
    const double ScaleTolerance,
    BindModel& Out,
    std::string& OutError)
{
    if (Scene == nullptr ||
        Bones.empty() ||
        Nodes.size() != Bones.size())
    {
        OutError =
            "exact bind-model input is incomplete";
        return false;
    }

    struct Candidate
    {
        TransformRT Model;
        std::string Source;
        int Priority = 2;
    };
    std::vector<std::vector<Candidate>>
        Candidates(Bones.size());
    std::unordered_map<FbxNode*, std::size_t>
        BoneIndexByNode;
    for (std::size_t Index = 0;
         Index < Nodes.size();
         ++Index)
    {
        BoneIndexByNode.emplace(
            Nodes[Index],
            Index);
    }

    std::vector<FbxNode*> AllNodes;
    CollectNodes(
        Scene->GetRootNode(),
        AllNodes);
    for (FbxNode* Node : AllNodes)
    {
        FbxMesh* Mesh =
            Node != nullptr
            ? Node->GetMesh()
            : nullptr;
        if (Mesh == nullptr)
            continue;
        const int SkinCount =
            Mesh->GetDeformerCount(
                FbxDeformer::eSkin);
        for (int SkinIndex = 0;
             SkinIndex < SkinCount;
             ++SkinIndex)
        {
            FbxSkin* Skin = FbxCast<FbxSkin>(
                Mesh->GetDeformer(
                    SkinIndex,
                    FbxDeformer::eSkin));
            if (Skin == nullptr)
            {
                OutError =
                    "exact bind-model contains an invalid skin";
                return false;
            }
            for (int ClusterIndex = 0;
                 ClusterIndex <
                    Skin->GetClusterCount();
                 ++ClusterIndex)
            {
                FbxCluster* Cluster =
                    Skin->GetCluster(ClusterIndex);
                if (Cluster == nullptr ||
                    Cluster->GetLink() == nullptr)
                {
                    OutError =
                        "exact bind-model contains an invalid skin cluster";
                    return false;
                }
                const auto BoneIt =
                    BoneIndexByNode.find(
                        Cluster->GetLink());
                if (BoneIt ==
                    BoneIndexByNode.end())
                {
                    continue;
                }
                FbxAMatrix LinkBind;
                Cluster->GetTransformLinkMatrix(
                    LinkBind);
                Candidate Value;
                Value.Model =
                    ConvertFbxTransformToUE(
                        LinkBind);
                Value.Source =
                    "skin_cluster_transform_link:" +
                    std::string(
                        Node->GetName() != nullptr
                        ? Node->GetName()
                        : "") +
                    ":" +
                    std::to_string(SkinIndex) +
                    ":" +
                    std::to_string(ClusterIndex);
                Value.Priority = 1;
                Candidates[BoneIt->second].push_back(
                    std::move(Value));
            }
        }
    }

    BindModel CandidateResult;
    CandidateResult.ModelPose.resize(
        Bones.size());
    CandidateResult.Sources.resize(
        Bones.size());
    for (std::size_t Index = 0;
         Index < Bones.size();
         ++Index)
    {
        if (Candidates[Index].empty())
        {
            CandidateResult.Sources[Index].clear();
            continue;
        }

        std::stable_sort(
            Candidates[Index].begin(),
            Candidates[Index].end(),
            [](const Candidate& Left,
               const Candidate& Right)
            {
                return Left.Priority <
                    Right.Priority;
            });
        const Candidate& Selected =
            Candidates[Index].front();
        CandidateResult.ModelPose[Index] =
            Selected.Model;
        CandidateResult.Sources[Index] =
            Selected.Source;
        if (Selected.Priority == 0)
            ++CandidateResult.BindPoseBoneCount;
        else if (Selected.Priority == 1)
            ++CandidateResult.SkinClusterBoneCount;
        else
            ++CandidateResult
                .EvaluatorFallbackBoneCount;

        for (const Candidate& Other :
             Candidates[Index])
        {
            if (Other.Priority >= 2)
                continue;
            const double Translation =
                Length(Subtract(
                    Other.Model.TranslationCm,
                    Selected.Model.TranslationCm));
            const double Rotation =
                QuaternionErrorDegrees(
                    Other.Model.Rotation,
                    Selected.Model.Rotation);
            const double Scale =
                ScaleError(
                    Other.Model.Scale,
                    Selected.Model.Scale);
            CandidateResult
                .MaximumCandidateTranslationCm =
                std::max(
                    CandidateResult
                        .MaximumCandidateTranslationCm,
                    Translation);
            CandidateResult
                .MaximumCandidateRotationDegrees =
                std::max(
                    CandidateResult
                        .MaximumCandidateRotationDegrees,
                    Rotation);
            CandidateResult
                .MaximumCandidateScale =
                std::max(
                    CandidateResult
                        .MaximumCandidateScale,
                    Scale);
            if (Translation > 1.0e-3 ||
                Rotation > 1.0e-3 ||
                Scale > ScaleTolerance)
            {
                std::ostringstream Message;
                Message
                    << "FBX bind-pose candidates disagree at "
                    << Bones[Index].Name
                    << " (translation_cm="
                    << Translation
                    << ", rotation_deg="
                    << Rotation
                    << ", scale="
                    << Scale << ")";
                OutError = Message.str();
                return false;
            }
        }
        if (ScaleError(
                Selected.Model.Scale,
                {1.0, 1.0, 1.0}) >
            ScaleTolerance)
        {
            OutError =
                "exact FBX bind model is outside the unit-scale contract at bone " +
                Bones[Index].Name;
            return false;
        }
    }

    Out = std::move(CandidateResult);
    return true;
}

std::string AxisSummary(const FbxAxisSystem& Axis)
{
    int UpSign = 0;
    int FrontSign = 0;
    const FbxAxisSystem::EUpVector Up =
        Axis.GetUpVector(UpSign);
    const FbxAxisSystem::EFrontVector Front =
        Axis.GetFrontVector(FrontSign);
    const char* UpName =
        Up == FbxAxisSystem::eXAxis
        ? "X"
        : Up == FbxAxisSystem::eYAxis
            ? "Y"
            : "Z";
    const char* FrontName =
        Front == FbxAxisSystem::eParityEven
        ? "ParityEven"
        : "ParityOdd";
    const char* Handedness =
        Axis.GetCoorSystem() ==
            FbxAxisSystem::eRightHanded
        ? "RightHanded"
        : "LeftHanded";
    std::ostringstream Stream;
    Stream
        << (UpSign >= 0 ? "+" : "-")
        << UpName << ", "
        << (FrontSign >= 0 ? "+" : "-")
        << FrontName << ", "
        << Handedness;
    return Stream.str();
}

bool ValidateCoordinateContract(
    const Json& Golden,
    std::string& OutError)
{
    const Json Coordinate =
        Golden.value("coordinateContract", Json{});
    if (!Coordinate.is_object() ||
        Coordinate.value("handedness", "") != "left" ||
        Coordinate.value("forwardAxis", "") != "+X" ||
        Coordinate.value("rightAxis", "") != "+Y" ||
        Coordinate.value("upAxis", "") != "+Z" ||
        Coordinate.value("distanceUnit", "") !=
            "centimeter" ||
        Coordinate.value(
            "quaternionComponentOrder",
            "") != "x,y,z,w")
    {
        OutError =
            "UE golden coordinate contract is not the supported left-handed +X/+Y/+Z centimeter contract";
        return false;
    }
    return true;
}

bool ParseGoldenBones(
    const Json& Golden,
    std::vector<UEFbxImportExactBone>& Out,
    std::string& OutError)
{
    const Json Reference =
        Golden.value("referenceSkeleton", Json{});
    const Json Bones =
        Reference.value("bones", Json::array());
    if (!Bones.is_array() || Bones.empty())
    {
        OutError =
            "golden referenceSkeleton.bones is empty";
        return false;
    }
    std::vector<UEFbxImportExactBone> Candidate;
    Candidate.reserve(Bones.size());
    std::set<std::string> Names;
    for (std::size_t Index = 0;
         Index < Bones.size();
         ++Index)
    {
        const Json& Value = Bones[Index];
        UEFbxImportExactBone Bone;
        Bone.Index = Value.value("index", -1);
        Bone.ParentIndex =
            Value.value("parentIndex", -2);
        Bone.Name = Value.value("name", "");
        if (Bone.Index != static_cast<int>(Index) ||
            Bone.ParentIndex < -1 ||
            Bone.ParentIndex >=
                static_cast<int>(Index) ||
            Bone.Name.empty() ||
            !Names.insert(Bone.Name).second ||
            !ParseTransform(
                Value.value("local", Json{}),
                Bone.ReferenceLocal) ||
            !ParseTransform(
                Value.value("model", Json{}),
                Bone.ReferenceModel))
        {
            OutError =
                "golden skeleton hierarchy is invalid at index " +
                std::to_string(Index);
            return false;
        }
        Candidate.push_back(std::move(Bone));
    }
    Out = std::move(Candidate);
    return true;
}

bool ValidatePipelineSettings(
    const Json& Golden,
    ImportSettings& Out,
    std::string& OutError)
{
    const Json Contract =
        Golden.value("assetImportContract", Json{});
    const Json Pipelines =
        Contract.value("pipelines", Json::array());
    if (!Pipelines.is_array() ||
        Pipelines.size() != 1)
    {
        OutError =
            "golden import contract must contain exactly one persisted Interchange pipeline";
        return false;
    }

    const Json Pipeline = Pipelines.front();
    const Json PipelineObject =
        Pipeline.value("object", Json{});
    if (PipelineObject.value("classPath", "") !=
        "/Script/InterchangePipelines.InterchangeGenericAssetsPipeline")
    {
        OutError =
            "unsupported persisted Interchange pipeline class";
        return false;
    }
    const Json Properties =
        Pipeline.value("rawProperties", Json{});
    const Json CommonMeshes =
        Properties.value(
            "commonMeshesProperties",
            Json{});
    const Json CommonSkeletal =
        Properties.value(
            "commonSkeletalMeshesAndAnimationsProperties",
            Json{});
    const Json Translation =
        Properties.value(
            "importOffsetTranslation",
            Json{});
    const Json Rotation =
        Properties.value(
            "importOffsetRotation",
            Json{});
    const double UniformScale =
        Properties.value(
            "importOffsetUniformScale",
            std::numeric_limits<double>::quiet_NaN());

    Vec3 TranslationValue;
    const bool IdentityTranslation =
        ParseVector(
            Translation,
            TranslationValue) &&
        Length(TranslationValue) <= 1.0e-12;
    const bool IdentityRotation =
        Rotation.is_object() &&
        std::abs(
            Rotation.value("pitch", 1.0)) <=
            1.0e-12 &&
        std::abs(
            Rotation.value("yaw", 1.0)) <=
            1.0e-12 &&
        std::abs(
            Rotation.value("roll", 1.0)) <=
            1.0e-12;
    if (!IdentityTranslation ||
        !IdentityRotation ||
        !std::isfinite(UniformScale) ||
        std::abs(UniformScale - 1.0) >
            1.0e-12)
    {
        OutError =
            "ue_fbx_import_exact_v1 fails closed on non-identity Interchange import offsets";
        return false;
    }

    const bool UseT0AsReference =
        CommonSkeletal.value(
            "bUseT0AsRefPose",
            false);
    if (UseT0AsReference)
    {
        OutError =
            "ue_fbx_import_exact_v1 does not support T0-as-reference-pose";
        return false;
    }

    const Json Translator =
        Contract.value("translatorSettings", Json{});
    if (Translator.value("available", false))
    {
        const Json Raw =
            Translator.value("rawProperties", Json{});
        if (!Raw.is_object())
        {
            OutError =
                "persisted FBX translator settings are malformed";
            return false;
        }
        const bool ConvertScene =
            Raw.value("bConvertScene", true);
        const bool ForceFrontXAxis =
            Raw.value("bForceFrontXAxis", false);
        const bool ConvertSceneUnit =
            Raw.value("bConvertSceneUnit", true);
        if (!ConvertScene ||
            ForceFrontXAxis ||
            !ConvertSceneUnit)
        {
            OutError =
                "ue_fbx_import_exact_v1 supports only ConvertScene=true, ForceFrontXAxis=false, ConvertSceneUnit=true";
            return false;
        }
        Out.Source =
            "persisted_fbx_translator_settings_plus_full_key_golden_validation";
    }
    else
    {
        // UE 5.8 does not persist translator settings on the current
        // Interchange assets. v1 may use the documented project defaults
        // only because the full exported animation is validated key-for-key
        // below. No validation means no committed output frames.
        Out.Source =
            "ue5.8_project_defaults_proven_only_by_full_key_golden_validation";
    }

    Out.ConvertScene = true;
    Out.ForceFrontXAxis = false;
    Out.ConvertSceneUnit = true;
    Out.BakeMeshes =
        CommonMeshes.value("bBakeMeshes", false);
    Out.UseT0AsReferencePose = false;
    return true;
}

bool LoadUsingUE58InterchangeSemantics(
    const std::filesystem::path& Path,
    const ImportSettings& Settings,
    LoadedScene& Out,
    UEFbxImportExactEvidence& OutEvidence,
    std::string& OutError)
{
    Out.Manager = FbxManager::Create();
    if (Out.Manager == nullptr)
    {
        OutError = "FbxManager::Create failed";
        return false;
    }
    FbxIOSettings* IOSettings =
        FbxIOSettings::Create(
            Out.Manager,
            IOSROOT);
    Out.Manager->SetIOSettings(IOSettings);
    FbxImporter* Importer =
        FbxImporter::Create(
            Out.Manager,
            "");
    if (Importer == nullptr ||
        !Importer->Initialize(
            Path.string().c_str(),
            -1,
            Out.Manager->GetIOSettings()))
    {
        OutError =
            Importer != nullptr
            ? std::string(
                  "FBX importer initialize failed: ") +
                Importer->GetStatus().GetErrorString()
            : "FbxImporter::Create failed";
        if (Importer != nullptr)
            Importer->Destroy();
        return false;
    }

    Out.Scene =
        FbxScene::Create(
            Out.Manager,
            "SKRTG_UE58Exact");
    if (Out.Scene == nullptr ||
        !Importer->Import(Out.Scene))
    {
        OutError =
            Out.Scene != nullptr
            ? std::string("FBX import failed: ") +
                Importer->GetStatus().GetErrorString()
            : "FbxScene::Create failed";
        Importer->Destroy();
        return false;
    }
    Importer->Destroy();

    const FbxAxisSystem OriginalAxis =
        Out.Scene->GetGlobalSettings().GetAxisSystem();
    const FbxSystemUnit OriginalUnit =
        Out.Scene->GetGlobalSettings().GetSystemUnit();
    OutEvidence.OriginalAxis =
        AxisSummary(OriginalAxis);
    OutEvidence.OriginalUnitScaleFactorCm =
        OriginalUnit.GetScaleFactor();
    OutEvidence.ImportSettingsSource =
        Settings.Source;
    OutEvidence.BakeMeshes =
        Settings.BakeMeshes;
    OutEvidence.UseT0AsReferencePose =
        Settings.UseT0AsReferencePose;

    if (Settings.ConvertScene)
    {
        const auto Front =
            static_cast<
                FbxAxisSystem::EFrontVector>(
                Settings.ForceFrontXAxis
                ? FbxAxisSystem::eParityEven
                : -FbxAxisSystem::eParityOdd);
        const FbxAxisSystem UnrealImportAxis(
            FbxAxisSystem::eZAxis,
            Front,
            FbxAxisSystem::eRightHanded);
        if (OriginalAxis != UnrealImportAxis)
        {
            OutEvidence.RemoveAllFbxRootsReturned =
                FbxRootNodeUtility::
                    RemoveAllFbxRoots(Out.Scene);
            UnrealImportAxis.ConvertScene(Out.Scene);
            OutEvidence.AxisConversionApplied = true;
        }
    }

    if (Settings.ConvertSceneUnit &&
        Out.Scene->GetGlobalSettings()
                .GetSystemUnit() !=
            FbxSystemUnit::cm)
    {
        FbxSystemUnit::cm.ConvertScene(Out.Scene);
        OutEvidence.UnitConversionApplied = true;
    }
    Out.Scene->GetAnimationEvaluator()->Reset();
    OutEvidence.ConvertedAxis =
        AxisSummary(
            Out.Scene->GetGlobalSettings()
                .GetAxisSystem());
    OutEvidence.ConvertedUnitScaleFactorCm =
        Out.Scene->GetGlobalSettings()
            .GetSystemUnit()
            .GetScaleFactor();
    return true;
}

FbxTimeSpan ResolveTimeSpan(
    FbxScene* Scene,
    FbxAnimStack* Stack)
{
    FbxTimeSpan Span = Stack->GetLocalTimeSpan();
    if (Span.GetDuration().Get() <= 0)
    {
        FbxTakeInfo* TakeInfo =
            Scene != nullptr
            ? Scene->GetTakeInfo(Stack->GetName())
            : nullptr;
        if (TakeInfo != nullptr)
            Span = TakeInfo->mLocalTimeSpan;
    }
    return Span;
}

bool SelectAnimationStack(
    FbxScene* Scene,
    const std::string& RequestedName,
    FbxAnimStack*& OutStack,
    FbxTimeSpan& OutSpan,
    std::string& OutError)
{
    OutStack = nullptr;
    long long BestDuration = 0;
    for (int Index = 0;
         Index <
            Scene->GetSrcObjectCount<FbxAnimStack>();
         ++Index)
    {
        FbxAnimStack* Candidate =
            Scene->GetSrcObject<FbxAnimStack>(Index);
        if (Candidate == nullptr)
            continue;
        const std::string CandidateName =
            Candidate->GetName() != nullptr
            ? Candidate->GetName()
            : "";
        const FbxTimeSpan Span =
            ResolveTimeSpan(Scene, Candidate);
        const long long Duration =
            Span.GetDuration().Get();
        if (!RequestedName.empty())
        {
            if (CandidateName == RequestedName &&
                Duration > 0)
            {
                OutStack = Candidate;
                OutSpan = Span;
                break;
            }
            continue;
        }
        if (Duration > BestDuration)
        {
            BestDuration = Duration;
            OutStack = Candidate;
            OutSpan = Span;
        }
    }
    if (OutStack == nullptr ||
        OutSpan.GetDuration().Get() <= 0)
    {
        OutError =
            RequestedName.empty()
            ? "FBX has no positive-duration animation stack"
            : "requested animation stack is absent: " +
                RequestedName;
        return false;
    }
    Scene->SetCurrentAnimationStack(OutStack);
    return true;
}

bool BuildUEAnimationPose(
    const std::vector<UEFbxImportExactBone>& Bones,
    const std::vector<FbxNode*>& Nodes,
    const FbxTime& Time,
    const bool BakeMeshes,
    const double ScaleTolerance,
    std::vector<TransformRT>& OutLocal,
    std::vector<TransformRT>& OutModel,
    std::string& OutError)
{
    std::vector<TransformRT> CandidateLocal(
        Bones.size());
    std::vector<TransformRT> CandidateModel(
        Bones.size());
    for (std::size_t Index = 0;
         Index < Bones.size();
         ++Index)
    {
        FbxNode* Node = Nodes[Index];
        if (Node == nullptr)
        {
            OutError =
                "bound FBX node is null: " +
                Bones[Index].Name;
            return false;
        }

        const TransformRT NodeGlobal =
            ConvertFbxTransformToUE(
                Node->EvaluateGlobalTransform(Time));
        const bool IsRootJoint =
            Bones[Index].ParentIndex < 0;
        if (IsRootJoint && BakeMeshes)
        {
            CandidateLocal[Index] = NodeGlobal;
        }
        else if (FbxNode* Parent =
                     Node->GetParent())
        {
            const TransformRT ParentGlobal =
                ConvertFbxTransformToUE(
                    Parent->EvaluateGlobalTransform(Time));
            if (!RelativeUnitScaleTransform(
                    ParentGlobal,
                    NodeGlobal,
                    CandidateLocal[Index],
                    ScaleTolerance))
            {
                OutError =
                    "parent-relative transform failed at bone " +
                    Bones[Index].Name;
                return false;
            }
        }
        else
        {
            CandidateLocal[Index] =
                ConvertFbxTransformToUE(
                    Node->EvaluateLocalTransform(Time));
        }

        if (ScaleError(
                CandidateLocal[Index].Scale,
                {1.0, 1.0, 1.0}) >
            ScaleTolerance)
        {
            OutError =
                "non-unit animation scale is outside ue_fbx_import_exact_v1 at bone " +
                Bones[Index].Name;
            return false;
        }

        CandidateModel[Index] =
            Bones[Index].ParentIndex < 0
            ? CandidateLocal[Index]
            : Compose(
                CandidateModel[
                    static_cast<std::size_t>(
                        Bones[Index].ParentIndex)],
                CandidateLocal[Index]);
    }
    OutLocal = std::move(CandidateLocal);
    OutModel = std::move(CandidateModel);
    return true;
}

bool ComparePose(
    const std::vector<UEFbxImportExactBone>& Bones,
    const Json& GoldenPose,
    const std::vector<TransformRT>& Local,
    const std::vector<TransformRT>& Model,
    const int KeyIndex,
    UEFbxImportExactValidationMetrics& Out,
    std::string& OutError)
{
    if (!GoldenPose.is_array() ||
        GoldenPose.size() != Bones.size() ||
        Local.size() != Bones.size() ||
        Model.size() != Bones.size())
    {
        OutError =
            "pose inventory mismatch at key " +
            std::to_string(KeyIndex);
        return false;
    }

    for (std::size_t Index = 0;
         Index < Bones.size();
         ++Index)
    {
        const Json& BoneValue =
            GoldenPose[Index];
        if (BoneValue.value("name", "") !=
                Bones[Index].Name ||
            BoneValue.value("index", -1) !=
                static_cast<int>(Index))
        {
            OutError =
                "golden sample bone identity mismatch at key " +
                std::to_string(KeyIndex);
            return false;
        }
        TransformRT GoldenLocal;
        TransformRT GoldenModel;
        if (!ParseTransform(
                BoneValue.value("local", Json{}),
                GoldenLocal) ||
            !ParseTransform(
                BoneValue.value("model", Json{}),
                GoldenModel))
        {
            OutError =
                "golden sample transform is invalid at key " +
                std::to_string(KeyIndex) +
                ", bone " + Bones[Index].Name;
            return false;
        }

        IncludeMaximum(
            Out.LocalTranslationCm,
            Length(Subtract(
                Local[Index].TranslationCm,
                GoldenLocal.TranslationCm)),
            KeyIndex,
            Bones[Index].Name);
        IncludeMaximum(
            Out.LocalRotationDegrees,
            QuaternionErrorDegrees(
                Local[Index].Rotation,
                GoldenLocal.Rotation),
            KeyIndex,
            Bones[Index].Name);
        IncludeMaximum(
            Out.LocalScale,
            ScaleError(
                Local[Index].Scale,
                GoldenLocal.Scale),
            KeyIndex,
            Bones[Index].Name);
        IncludeMaximum(
            Out.ModelTranslationCm,
            Length(Subtract(
                Model[Index].TranslationCm,
                GoldenModel.TranslationCm)),
            KeyIndex,
            Bones[Index].Name);
        IncludeMaximum(
            Out.ModelRotationDegrees,
            QuaternionErrorDegrees(
                Model[Index].Rotation,
                GoldenModel.Rotation),
            KeyIndex,
            Bones[Index].Name);
        IncludeMaximum(
            Out.ModelScale,
            ScaleError(
                Model[Index].Scale,
                GoldenModel.Scale),
            KeyIndex,
            Bones[Index].Name);
    }
    return true;
}

bool MetricsPass(
    const UEFbxImportExactValidationMetrics& Metrics,
    const UEFbxImportExactOptions& Options)
{
    return
        Metrics.LocalTranslationCm.Value <=
            Options.TranslationToleranceCm &&
        Metrics.LocalRotationDegrees.Value <=
            Options.RotationToleranceDegrees &&
        Metrics.LocalScale.Value <=
            Options.ScaleTolerance &&
        Metrics.ModelTranslationCm.Value <=
            Options.TranslationToleranceCm &&
        Metrics.ModelRotationDegrees.Value <=
            Options.RotationToleranceDegrees &&
        Metrics.ModelScale.Value <=
            Options.ScaleTolerance;
}
} // namespace

UEFbxImportExactResult LoadUEFbxImportExactClip(
    const UEFbxImportExactOptions& Options)
{
    UEFbxImportExactResult Failure;
    const auto Fail =
        [&](const std::string& Error)
        {
            UEFbxImportExactResult Result;
            Result.Errors.push_back(Error);
            return Result;
        };

    if (Options.FbxPath.empty() ||
        Options.AnimationGoldenJsonPath.empty() ||
        !IsSha256(Options.FbxExpectedSha256) ||
        !IsSha256(
            Options.AnimationGoldenJsonExpectedSha256) ||
        !std::isfinite(Options.OutputSampleRate) ||
        Options.OutputSampleRate <= 0.0 ||
        Options.OutputSampleRate > 240.0 ||
        !std::isfinite(Options.MaximumDurationSeconds) ||
        Options.MaximumDurationSeconds <= 0.0 ||
        !std::isfinite(
            Options.TranslationToleranceCm) ||
        Options.TranslationToleranceCm <= 0.0 ||
        !std::isfinite(
            Options.RotationToleranceDegrees) ||
        Options.RotationToleranceDegrees <= 0.0 ||
        !std::isfinite(Options.ScaleTolerance) ||
        Options.ScaleTolerance <= 0.0)
    {
        return Fail(
            "ue_fbx_import_exact_v1 options are incomplete or out of bounds");
    }

    std::string Error;
    std::string FbxSha256;
    std::string GoldenSha256;
    if (!ComputeSha256(
            Options.FbxPath,
            FbxSha256,
            Error) ||
        !ComputeSha256(
            Options.AnimationGoldenJsonPath,
            GoldenSha256,
            Error))
    {
        return Fail(Error);
    }
    if (UpperAscii(FbxSha256) !=
            UpperAscii(
                Options.FbxExpectedSha256) ||
        UpperAscii(GoldenSha256) !=
            UpperAscii(
                Options
                    .AnimationGoldenJsonExpectedSha256))
    {
        return Fail(
            "ue_fbx_import_exact_v1 input SHA256 mismatch");
    }

    Json Golden;
    if (!ReadJson(
            Options.AnimationGoldenJsonPath,
            Golden,
            Error))
    {
        return Fail(Error);
    }
    if (Golden.value("schema", "") !=
            "skrtg.ue_ik_asset_export.v2" ||
        Golden.value("schemaVersion", 0) != 2 ||
        Golden.value("kind", "") !=
            "animationComponentSpaceGolden" ||
        !Golden.value("valid", false))
    {
        return Fail(
            "unsupported or invalid animation golden document");
    }
    if (!ValidateCoordinateContract(
            Golden,
            Error))
    {
        return Fail(Error);
    }
    if (Golden.value("retargetingApplied", true))
    {
        return Fail(
            "animation golden must describe the imported source animation before retargeting");
    }

    ImportSettings Settings;
    if (!ValidatePipelineSettings(
            Golden,
            Settings,
            Error))
    {
        return Fail(Error);
    }

    std::vector<UEFbxImportExactBone> Bones;
    if (!ParseGoldenBones(
            Golden,
            Bones,
            Error))
    {
        return Fail(Error);
    }

    LoadedScene Scene;
    UEFbxImportExactEvidence Evidence;
    if (!LoadUsingUE58InterchangeSemantics(
            Options.FbxPath,
            Settings,
            Scene,
            Evidence,
            Error))
    {
        return Fail(Error);
    }

    std::vector<FbxNode*> Nodes;
    if (!BindBones(
            Scene.Scene,
            Bones,
            Nodes,
            Error))
    {
        return Fail(Error);
    }
    FbxAnimStack* Stack = nullptr;
    FbxTimeSpan Span;
    if (!SelectAnimationStack(
            Scene.Scene,
            Options.AnimationStackName,
            Stack,
            Span,
            Error))
    {
        return Fail(Error);
    }
    const std::string StackName =
        Stack->GetName() != nullptr
        ? Stack->GetName()
        : "";
    const double DurationSeconds =
        Span.GetDuration().GetSecondDouble();
    if (!std::isfinite(DurationSeconds) ||
        DurationSeconds <= 0.0 ||
        DurationSeconds >
            Options.MaximumDurationSeconds)
    {
        return Fail(
            "selected animation duration is empty or exceeds the safety limit");
    }

    const Json Samples =
        Golden.value("samples", Json::array());
    if (!Samples.is_array() ||
        Samples.empty() ||
        Samples.size() !=
            Golden.value("numberOfKeys", 0))
    {
        return Fail(
            "golden sample inventory mismatch");
    }

    UEFbxImportExactValidationMetrics Metrics;
    std::vector<TransformRT> Local;
    std::vector<TransformRT> Model;
    for (std::size_t SampleIndex = 0;
         SampleIndex < Samples.size();
         ++SampleIndex)
    {
        const Json& Sample =
            Samples[SampleIndex];
        const int KeyIndex =
            Sample.value("keyIndex", -1);
        const double TimeSeconds =
            Sample.value(
                "timeSeconds",
                std::numeric_limits<
                    double>::quiet_NaN());
        if (KeyIndex !=
                static_cast<int>(SampleIndex) ||
            !std::isfinite(TimeSeconds) ||
            TimeSeconds < -1.0e-9 ||
            TimeSeconds >
                DurationSeconds + 1.0e-6)
        {
            return Fail(
                "golden sample identity or time is invalid");
        }
        FbxTime Time = Span.GetStart();
        FbxTime Offset;
        Offset.SetSecondDouble(TimeSeconds);
        Time += Offset;
        if (!BuildUEAnimationPose(
                Bones,
                Nodes,
                Time,
                Settings.BakeMeshes,
                Options.ScaleTolerance,
                Local,
                Model,
                Error) ||
            !ComparePose(
                Bones,
                Sample.value(
                    "bones",
                    Json::array()),
                Local,
                Model,
                KeyIndex,
                Metrics,
                Error))
        {
            return Fail(Error);
        }
    }
    if (!MetricsPass(Metrics, Options))
    {
        std::ostringstream Message;
        Message
            << "full-key UE golden validation failed: local_t_cm="
            << Metrics.LocalTranslationCm.Value
            << ", local_r_deg="
            << Metrics.LocalRotationDegrees.Value
            << ", model_t_cm="
            << Metrics.ModelTranslationCm.Value
            << ", model_r_deg="
            << Metrics.ModelRotationDegrees.Value;
        return Fail(Message.str());
    }

    const int LastFrame =
        static_cast<int>(
            std::ceil(
                DurationSeconds *
                    Options.OutputSampleRate -
                1.0e-9));
    if (LastFrame < 1 ||
        LastFrame >
            static_cast<int>(
                Options.MaximumDurationSeconds *
                Options.OutputSampleRate) + 1)
    {
        return Fail(
            "output sampling frame count is outside the safety limit");
    }

    std::vector<UEFbxImportExactFrame>
        CandidateFrames;
    CandidateFrames.reserve(
        static_cast<std::size_t>(
            LastFrame + 1));
    for (int FrameIndex = 0;
         FrameIndex <= LastFrame;
         ++FrameIndex)
    {
        const double RelativeSeconds =
            std::min(
                DurationSeconds,
                static_cast<double>(FrameIndex) /
                    Options.OutputSampleRate);
        FbxTime Time = Span.GetStart();
        FbxTime Offset;
        Offset.SetSecondDouble(RelativeSeconds);
        Time += Offset;
        if (!BuildUEAnimationPose(
                Bones,
                Nodes,
                Time,
                Settings.BakeMeshes,
                Options.ScaleTolerance,
                Local,
                Model,
                Error))
        {
            return Fail(
                "output frame " +
                std::to_string(FrameIndex) +
                " failed: " + Error);
        }
        UEFbxImportExactFrame Frame;
        Frame.FrameIndex = FrameIndex;
        Frame.TimeSeconds = RelativeSeconds;
        Frame.LocalPose = Local;
        Frame.ModelPose = Model;
        CandidateFrames.push_back(
            std::move(Frame));
    }

    // Commit only after hashes, import settings, skeleton binding, every UE
    // golden key, and every requested output frame have passed.
    UEFbxImportExactResult Result;
    Result.Success = true;
    Result.FbxSha256 = FbxSha256;
    Result.AnimationGoldenJsonSha256 =
        GoldenSha256;
    Result.AnimationStackName = StackName;
    Result.FbxRangeStartSeconds =
        Span.GetStart().GetSecondDouble();
    Result.DurationSeconds = DurationSeconds;
    Result.Evidence = std::move(Evidence);
    Result.GoldenValidation = std::move(Metrics);
    Result.Bones = std::move(Bones);
    Result.Frames = std::move(CandidateFrames);
    Result.Warnings.push_back(
        "ue_fbx_import_exact_v1 remains selected=false and adopted=false; full-key UE golden validation proves only this hash-bound FBX/JSON pair.");
    if (Settings.Source.find("project_defaults") !=
        std::string::npos)
    {
        Result.Warnings.push_back(
            "The Interchange asset did not persist FBX translator settings. UE 5.8 defaults are accepted only because every exported UE animation key passed numeric validation.");
    }
    return Result;
}
} // namespace skrtg::fbx
