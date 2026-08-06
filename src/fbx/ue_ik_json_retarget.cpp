#include "skrtg/fbx/ue_ik_json_retarget.h"

#include "skrtg/core/math/transform.h"
#include "skrtg/fbx/ue_fbx_import_exact.h"
#include "skrtg/reconciliation/rest_reconciliation.h"
#include "skrtg/retarget/op_stack_config.h"

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
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace skrtg::fbx
{
namespace
{
using Json = nlohmann::json;
using core::animation::PoseBuffer;
using core::animation::PoseSpace;
using core::math::Compose;
using core::math::Conjugate;
using core::math::Add;
using core::math::Length;
using core::math::Multiply;
using core::math::Normalize;
using core::math::Quat;
using core::math::RelativeUnitScaleTransform;
using core::math::RotateVector;
using core::math::Subtract;
using core::math::TransformRT;
using core::math::Vec3;

constexpr double Pi = 3.1415926535897932384626433832795;

struct LoadedFbx
{
    FbxManager* Manager = nullptr;
    FbxScene* Scene = nullptr;
    int OriginalUpAxis = -1;
    int OriginalUpSign = 0;
    int OriginalFrontParity = -1;
    int OriginalFrontSign = 0;
    int OriginalCoordinateSystem = -1;
    double OriginalUnitScaleCm = 0.0;
    double OriginalUnitMultiplier = 0.0;
};

struct RestFbxCoordinateEvidence
{
    bool AxisContractMatched = false;
    bool UnitContractMatched = false;
    int BoneCount = 0;
    double MaximumLocalTranslationCm = 0.0;
    double MaximumLocalRotationDegrees = 0.0;
    double MaximumLocalScale = 0.0;
    double MaximumModelTranslationCm = 0.0;
    double MaximumModelRotationDegrees = 0.0;
    double MaximumModelScale = 0.0;
    int OriginalUpAxis = -1;
    int OriginalUpSign = 0;
    int OriginalFrontParity = -1;
    int OriginalFrontSign = 0;
    int OriginalCoordinateSystem = -1;
    double OriginalUnitScaleCm = 0.0;
    double OriginalUnitMultiplier = 0.0;
};

struct BoundInput
{
    std::filesystem::path Path;
    std::string ExpectedSha256;
    std::string Sha256;
    std::uintmax_t Size = 0;
    long long LastWriteTicks = 0;
};

struct BoundSkeleton
{
    std::vector<FbxNode*> Nodes;
    std::vector<std::string> Paths;
    std::vector<TransformRT> ReferenceLocal;
    std::vector<TransformRT> ReferenceModel;
};

struct RestValidation
{
    double MaximumRawLocalTranslationCm = 0.0;
    double MaximumRawLocalRotationDegrees = 0.0;
    double MaximumRawLocalScale = 0.0;
    double MaximumRawModelTranslationCm = 0.0;
    double MaximumRawModelRotationDegrees = 0.0;
    double MaximumRawModelScale = 0.0;
    double MaximumLocalTranslationCm = 0.0;
    double MaximumLocalRotationDegrees = 0.0;
    double MaximumLocalScale = 0.0;
    double MaximumModelTranslationCm = 0.0;
    double MaximumModelRotationDegrees = 0.0;
    double MaximumModelScale = 0.0;
};

struct AnimationBindModel
{
    std::vector<TransformRT> Model;
    std::vector<std::string> Sources;
    int SkinClusterBoneCount = 0;
    int BindPoseBoneCount = 0;
    int DerivedLeafBoneCount = 0;
    int GoldenReferenceBoneCount = 0;
    bool UsedGoldenReferenceFallback = false;
    double MaximumDuplicateTranslationCm = 0.0;
    double MaximumDuplicateRotationDegrees = 0.0;
    double MaximumDuplicateScale = 0.0;
    double MaximumUEReferencePositionResidualCm = 0.0;
    double MaximumGoldenLocalTranslationCm = 0.0;
    double MaximumGoldenLocalRotationDegrees = 0.0;
    double MaximumGoldenLocalScale = 0.0;
    double MaximumGoldenModelTranslationCm = 0.0;
    double MaximumGoldenModelRotationDegrees = 0.0;
    double MaximumGoldenModelScale = 0.0;
};

struct OwnedFrame
{
    int FrameIndex = 0;
    double TimeSeconds = 0.0;
    std::vector<TransformRT> SourceModelPose;
    std::vector<TransformRT> SourceRetargetModelPose;
    PoseBuffer TargetFkModelPose;
    PoseBuffer TargetFoundationLocalPose;
    PoseBuffer TargetFoundationModelPose;
    PoseBuffer TargetFinalLocalPose;
    PoseBuffer TargetFinalModelPose;
};

std::string UpperAscii(std::string Value)
{
    for (char& Character : Value)
    {
        if (Character >= 'a' && Character <= 'z')
            Character = static_cast<char>(Character - 'a' + 'A');
    }
    return Value;
}

bool IsSha256(const std::string& Value)
{
    if (Value.size() != 64) return false;
    return std::all_of(
        Value.begin(), Value.end(),
        [](const char Character)
        {
            return (Character >= '0' && Character <= '9') ||
                (Character >= 'a' && Character <= 'f') ||
                (Character >= 'A' && Character <= 'F');
        });
}

bool ComputeSha256Bytes(
    const unsigned char* Data,
    std::size_t Size,
    std::string& OutHash,
    std::string& OutError)
{
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE Algorithm = nullptr;
    BCRYPT_HASH_HANDLE HashHandle = nullptr;
    DWORD ObjectLength = 0;
    DWORD HashLength = 0;
    DWORD ResultLength = 0;
    std::vector<unsigned char> HashObject;
    std::vector<unsigned char> Digest;
    auto Cleanup = [&]()
    {
        if (HashHandle != nullptr) BCryptDestroyHash(HashHandle);
        if (Algorithm != nullptr)
            BCryptCloseAlgorithmProvider(Algorithm, 0);
    };
    NTSTATUS Status = BCryptOpenAlgorithmProvider(
        &Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (Status < 0)
    {
        OutError = "BCryptOpenAlgorithmProvider failed";
        Cleanup();
        return false;
    }
    Status = BCryptGetProperty(
        Algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&ObjectLength), sizeof(ObjectLength),
        &ResultLength, 0);
    if (Status < 0)
    {
        OutError = "BCryptGetProperty object length failed";
        Cleanup();
        return false;
    }
    Status = BCryptGetProperty(
        Algorithm, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&HashLength), sizeof(HashLength),
        &ResultLength, 0);
    if (Status < 0)
    {
        OutError = "BCryptGetProperty hash length failed";
        Cleanup();
        return false;
    }
    HashObject.resize(ObjectLength);
    Digest.resize(HashLength);
    Status = BCryptCreateHash(
        Algorithm, &HashHandle, HashObject.data(), ObjectLength,
        nullptr, 0, 0);
    if (Status < 0)
    {
        OutError = "BCryptCreateHash failed";
        Cleanup();
        return false;
    }
    std::size_t Offset = 0;
    while (Offset < Size)
    {
        const ULONG Chunk = static_cast<ULONG>(
            std::min<std::size_t>(Size - Offset, 1024 * 1024));
        Status = BCryptHashData(
            HashHandle, const_cast<PUCHAR>(Data + Offset), Chunk, 0);
        if (Status < 0)
        {
            OutError = "BCryptHashData failed";
            Cleanup();
            return false;
        }
        Offset += Chunk;
    }
    Status = BCryptFinishHash(
        HashHandle, Digest.data(), HashLength, 0);
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
    (void)Data;
    (void)Size;
    (void)OutHash;
    OutError =
        "SHA256 is implemented only for the authorized Windows toolchain";
    return false;
#endif
}

bool ComputeSha256(
    const std::filesystem::path& Path,
    std::string& OutHash,
    std::string& OutError)
{
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        OutError = "failed to open input for SHA256: " + Path.string();
        return false;
    }
    Input.seekg(0, std::ios::end);
    const std::streamoff Size = Input.tellg();
    if (Size < 0)
    {
        OutError = "failed to size input for SHA256: " + Path.string();
        return false;
    }
    std::vector<unsigned char> Bytes(static_cast<std::size_t>(Size));
    Input.seekg(0, std::ios::beg);
    if (!Bytes.empty())
    {
        Input.read(
            reinterpret_cast<char*>(Bytes.data()),
            static_cast<std::streamsize>(Bytes.size()));
    }
    if (!Input && !Bytes.empty())
    {
        OutError = "failed while reading input for SHA256: " +
            Path.string();
        return false;
    }
    return ComputeSha256Bytes(
        Bytes.data(), Bytes.size(), OutHash, OutError);
}

bool CaptureBoundInput(
    const std::filesystem::path& Path,
    const std::string& ExpectedSha256,
    BoundInput& Out,
    std::string& OutError)
{
    if (Path.empty() || !IsSha256(ExpectedSha256))
    {
        OutError =
            "input path and explicit SHA256 are required for every UE JSON/FBX input";
        return false;
    }
    Out = {};
    Out.Path = std::filesystem::absolute(Path).lexically_normal();
    Out.ExpectedSha256 = UpperAscii(ExpectedSha256);
    std::error_code Error;
    Out.Size = std::filesystem::file_size(Out.Path, Error);
    if (Error)
    {
        OutError = "failed to read input size: " + Out.Path.string();
        return false;
    }
    Out.LastWriteTicks = std::filesystem::last_write_time(
        Out.Path, Error).time_since_epoch().count();
    if (Error ||
        !ComputeSha256(Out.Path, Out.Sha256, OutError))
    {
        if (OutError.empty())
            OutError = "failed to capture input identity";
        return false;
    }
    if (UpperAscii(Out.Sha256) != Out.ExpectedSha256)
    {
        OutError = "input SHA256 mismatch: " + Out.Path.string();
        return false;
    }
    return true;
}

bool VerifyBoundInputUnchanged(
    const BoundInput& Input,
    std::string& OutError)
{
    std::error_code Error;
    const std::uintmax_t Size =
        std::filesystem::file_size(Input.Path, Error);
    if (Error)
    {
        OutError = "failed to read post-use input size: " +
            Input.Path.string();
        return false;
    }
    const long long LastWriteTicks =
        std::filesystem::last_write_time(
            Input.Path, Error).time_since_epoch().count();
    std::string Hash;
    if (Error || !ComputeSha256(Input.Path, Hash, OutError))
    {
        if (OutError.empty())
            OutError = "failed to recapture input identity";
        return false;
    }
    if (Size != Input.Size ||
        LastWriteTicks != Input.LastWriteTicks ||
        UpperAscii(Hash) != UpperAscii(Input.Sha256))
    {
        OutError = "input changed during use: " + Input.Path.string();
        return false;
    }
    return true;
}

void DestroyFbx(LoadedFbx& Loaded)
{
    if (Loaded.Manager != nullptr) Loaded.Manager->Destroy();
    Loaded = {};
}

bool NormalizeLoadedFbxToUEJsonAxis(
    LoadedFbx& Loaded,
    std::string& OutError)
{
    if (Loaded.Scene == nullptr)
    {
        OutError = "UE JSON FBX scene is null";
        return false;
    }
    FbxAxisSystem UEJsonAxis;
    if (!FbxAxisSystem::ParseAxisSystem("yzx", UEJsonAxis))
    {
        OutError = "failed to construct the UE JSON +X/+Y/+Z axis";
        return false;
    }
    FbxSystemUnit::cm.ConvertScene(Loaded.Scene);
    UEJsonAxis.DeepConvertScene(Loaded.Scene);
    FbxNode* RootNode = Loaded.Scene->GetRootNode();
    if (RootNode == nullptr)
    {
        OutError = "UE JSON FBX scene has no root node";
        return false;
    }
    double FrameRate = FbxTime::GetFrameRate(
        Loaded.Scene->GetGlobalSettings().GetTimeMode());
    if (!std::isfinite(FrameRate) || FrameRate <= 0.0)
        FrameRate = 30.0;
    RootNode->ResetPivotSetAndConvertAnimation(
        FrameRate, false, true, false);
    return true;
}

bool LoadFbx(
    const std::filesystem::path& Path,
    const char* SceneName,
    LoadedFbx& Out,
    std::string& OutError,
    const bool NormalizeToUEJsonAxis)
{
    Out.Manager = FbxManager::Create();
    if (Out.Manager == nullptr)
    {
        OutError = "FbxManager::Create failed";
        return false;
    }
    FbxIOSettings* Settings =
        FbxIOSettings::Create(Out.Manager, IOSROOT);
    Out.Manager->SetIOSettings(Settings);
    FbxImporter* Importer = FbxImporter::Create(Out.Manager, "");
    if (Importer == nullptr ||
        !Importer->Initialize(
            Path.string().c_str(), -1,
            Out.Manager->GetIOSettings()))
    {
        OutError = Importer != nullptr
            ? std::string("FBX importer initialize failed: ") +
                Importer->GetStatus().GetErrorString()
            : "FbxImporter::Create failed";
        if (Importer != nullptr) Importer->Destroy();
        DestroyFbx(Out);
        return false;
    }
    Out.Scene = FbxScene::Create(Out.Manager, SceneName);
    if (Out.Scene == nullptr || !Importer->Import(Out.Scene))
    {
        OutError = Out.Scene != nullptr
            ? std::string("FBX import failed: ") +
                Importer->GetStatus().GetErrorString()
            : "FbxScene::Create failed";
        Importer->Destroy();
        DestroyFbx(Out);
        return false;
    }
    Importer->Destroy();
    FbxGlobalSettings& GlobalSettings =
        Out.Scene->GetGlobalSettings();
    const FbxAxisSystem OriginalAxis =
        GlobalSettings.GetAxisSystem();
    int UpSign = 0;
    int FrontSign = 0;
    Out.OriginalUpAxis = static_cast<int>(
        OriginalAxis.GetUpVector(UpSign));
    Out.OriginalUpSign = UpSign;
    Out.OriginalFrontParity = static_cast<int>(
        OriginalAxis.GetFrontVector(FrontSign));
    Out.OriginalFrontSign = FrontSign;
    Out.OriginalCoordinateSystem = static_cast<int>(
        OriginalAxis.GetCoorSystem());
    const FbxSystemUnit OriginalUnit =
        GlobalSettings.GetSystemUnit();
    Out.OriginalUnitScaleCm = OriginalUnit.GetScaleFactor();
    Out.OriginalUnitMultiplier = OriginalUnit.GetMultiplier();
    if (!NormalizeToUEJsonAxis)
    {
        if (Out.Scene->GetRootNode() == nullptr)
        {
            OutError = "FBX scene has no root node";
            DestroyFbx(Out);
            return false;
        }
        return true;
    }
    if (NormalizeLoadedFbxToUEJsonAxis(Out, OutError))
        return true;
    DestroyFbx(Out);
    return false;
}

TransformRT ToTransformRT(const FbxAMatrix& Matrix)
{
    const FbxVector4 Translation = Matrix.GetT();
    const FbxQuaternion Rotation = Matrix.GetQ();
    const FbxVector4 Scale = Matrix.GetS();
    TransformRT Result;
    Result.TranslationCm =
        {Translation[0], Translation[1], Translation[2]};
    Result.Rotation = Normalize(
        {Rotation[0], Rotation[1], Rotation[2], Rotation[3]});
    Result.Scale = {Scale[0], Scale[1], Scale[2]};
    return Result;
}

TransformRT ToTransformRT(const FbxMatrix& Matrix)
{
    FbxVector4 Translation;
    FbxQuaternion Rotation;
    FbxVector4 Shearing;
    FbxVector4 Scale;
    double DeterminantSign = 1.0;
    Matrix.GetElements(
        Translation, Rotation, Shearing, Scale,
        DeterminantSign);
    TransformRT Result;
    Result.TranslationCm =
        {Translation[0], Translation[1], Translation[2]};
    Result.Rotation = Normalize(
        {Rotation[0], Rotation[1], Rotation[2], Rotation[3]});
    Result.Scale = {
        Scale[0] * DeterminantSign,
        Scale[1], Scale[2]};
    return Result;
}

std::string CanonicalFbxBoneName(const std::string& Name)
{
    const std::size_t Colon = Name.find_last_of(':');
    return Colon == std::string::npos
        ? Name
        : Name.substr(Colon + 1);
}

void BuildPathMapRecursive(
    FbxNode* Node,
    const std::string& ParentPath,
    std::map<FbxNode*, std::string>& Out)
{
    if (Node == nullptr) return;
    const std::string Name =
        Node->GetName() != nullptr ? Node->GetName() : "";
    const std::string Path =
        ParentPath.empty() ? Name : ParentPath + "/" + Name;
    if (!Name.empty()) Out[Node] = Path;
    for (int Index = 0; Index < Node->GetChildCount(); ++Index)
        BuildPathMapRecursive(Node->GetChild(Index), Path, Out);
}

std::map<FbxNode*, std::string> BuildPathMap(FbxScene* Scene)
{
    std::map<FbxNode*, std::string> Result;
    FbxNode* Root = Scene != nullptr ? Scene->GetRootNode() : nullptr;
    if (Root == nullptr) return Result;
    for (int Index = 0; Index < Root->GetChildCount(); ++Index)
        BuildPathMapRecursive(Root->GetChild(Index), "", Result);
    return Result;
}

void CollectSkinLinks(
    FbxNode* Node,
    std::set<FbxNode*>& Out)
{
    if (Node == nullptr) return;
    FbxMesh* Mesh = Node->GetMesh();
    if (Mesh != nullptr)
    {
        const int SkinCount =
            Mesh->GetDeformerCount(FbxDeformer::eSkin);
        for (int SkinIndex = 0; SkinIndex < SkinCount; ++SkinIndex)
        {
            FbxSkin* Skin = static_cast<FbxSkin*>(
                Mesh->GetDeformer(SkinIndex, FbxDeformer::eSkin));
            if (Skin == nullptr) continue;
            for (int ClusterIndex = 0;
                 ClusterIndex < Skin->GetClusterCount();
                 ++ClusterIndex)
            {
                FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
                if (Cluster != nullptr && Cluster->GetLink() != nullptr)
                    Out.insert(Cluster->GetLink());
            }
        }
    }
    for (int Index = 0; Index < Node->GetChildCount(); ++Index)
        CollectSkinLinks(Node->GetChild(Index), Out);
}

void CollectAllNodes(FbxNode* Node, std::vector<FbxNode*>& Out)
{
    if (Node == nullptr) return;
    Out.push_back(Node);
    for (int Index = 0; Index < Node->GetChildCount(); ++Index)
        CollectAllNodes(Node->GetChild(Index), Out);
}

bool IsSkeletonNode(FbxNode* Node)
{
    const FbxNodeAttribute* Attribute =
        Node != nullptr ? Node->GetNodeAttribute() : nullptr;
    return Attribute != nullptr &&
        Attribute->GetAttributeType() ==
            FbxNodeAttribute::eSkeleton;
}

template <typename BoneType>
bool BindSkeleton(
    FbxScene* Scene,
    const std::vector<BoneType>& Bones,
    BoundSkeleton& Out,
    std::string& OutError)
{
    if (Scene == nullptr || Bones.empty())
    {
        OutError = "FBX skeleton binding input is empty";
        return false;
    }
    const std::map<FbxNode*, std::string> Paths =
        BuildPathMap(Scene);
    std::set<FbxNode*> SkinLinks;
    CollectSkinLinks(Scene->GetRootNode(), SkinLinks);
    std::vector<FbxNode*> AllNodes;
    CollectAllNodes(Scene->GetRootNode(), AllNodes);

    std::unordered_map<std::string, std::vector<FbxNode*>> Candidates;
    std::set<std::string> RequiredNames;
    for (const BoneType& Bone : Bones)
        RequiredNames.insert(Bone.Name);
    for (FbxNode* Node : AllNodes)
    {
        const std::string Name =
            Node->GetName() != nullptr ? Node->GetName() : "";
        const std::string Canonical = CanonicalFbxBoneName(Name);
        if (RequiredNames.find(Canonical) != RequiredNames.end())
            Candidates[Canonical].push_back(Node);
    }

    Out = {};
    Out.Nodes.resize(Bones.size());
    Out.Paths.resize(Bones.size());
    Out.ReferenceLocal.resize(Bones.size());
    Out.ReferenceModel.resize(Bones.size());
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const BoneType& Bone = Bones[Index];
        const auto CandidateIt = Candidates.find(Bone.Name);
        if (CandidateIt == Candidates.end())
        {
            OutError = "FBX bone is absent: " + Bone.Name;
            return false;
        }
        std::vector<FbxNode*> Preferred;
        for (FbxNode* Candidate : CandidateIt->second)
        {
            if (IsSkeletonNode(Candidate) ||
                SkinLinks.find(Candidate) != SkinLinks.end())
            {
                Preferred.push_back(Candidate);
            }
        }
        const std::vector<FbxNode*>& Selection =
            Preferred.empty() ? CandidateIt->second : Preferred;
        if (Selection.size() != 1)
        {
            OutError =
                "FBX namespace-normalized bone is ambiguous: " +
                Bone.Name;
            return false;
        }
        FbxNode* Node = Selection.front();
        const auto PathIt = Paths.find(Node);
        if (PathIt == Paths.end())
        {
            OutError = "FBX bone path is unavailable: " + Bone.Name;
            return false;
        }
        Out.Nodes[Index] = Node;
        Out.Paths[Index] = PathIt->second;
        Out.ReferenceLocal[Index] = ToTransformRT(
            Node->EvaluateLocalTransform(
                FBXSDK_TIME_INFINITE,
                FbxNode::eSourcePivot, false, true));
    }

    std::unordered_map<FbxNode*, int> BoneIndexByNode;
    for (std::size_t Index = 0; Index < Out.Nodes.size(); ++Index)
        BoneIndexByNode.emplace(
            Out.Nodes[Index], static_cast<int>(Index));
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        int ActualParent = -1;
        FbxNode* Parent = Out.Nodes[Index]->GetParent();
        while (Parent != nullptr)
        {
            const auto It = BoneIndexByNode.find(Parent);
            if (It != BoneIndexByNode.end())
            {
                ActualParent = It->second;
                break;
            }
            Parent = Parent->GetParent();
        }
        if (ActualParent != Bones[Index].ParentIndex)
        {
            OutError =
                "FBX parent hierarchy does not match UE JSON at bone: " +
                Bones[Index].Name;
            return false;
        }
        // FBX local transforms cannot be safely re-composed here: inherit
        // modes, pivots, and any non-skeleton ancestors are part of the
        // evaluated node contract.  The SDK's global evaluation is the
        // authoritative model-space reference used for animation deltas.
        Out.ReferenceModel[Index] = ToTransformRT(
            Out.Nodes[Index]->EvaluateGlobalTransform(
                FBXSDK_TIME_INFINITE,
                FbxNode::eSourcePivot, false, true));
    }
    return true;
}

double QuaternionErrorDegrees(Quat Left, Quat Right)
{
    Left = Normalize(Left);
    Right = Normalize(Right);
    const double Dot = std::clamp(
        std::abs(
            Left.X * Right.X + Left.Y * Right.Y +
            Left.Z * Right.Z + Left.W * Right.W),
        0.0, 1.0);
    return 2.0 * std::acos(Dot) * 180.0 / Pi;
}

double ScaleError(Vec3 Left, Vec3 Right)
{
    return std::max({
        std::abs(Left.X - Right.X),
        std::abs(Left.Y - Right.Y),
        std::abs(Left.Z - Right.Z)});
}

TransformRT ReflectFbxYIntoUE(const TransformRT& Value)
{
    TransformRT Result = Value;
    Result.TranslationCm.Y = -Result.TranslationCm.Y;
    Result.Rotation = Normalize({
        -Value.Rotation.X,
        Value.Rotation.Y,
        -Value.Rotation.Z,
        Value.Rotation.W});
    return Result;
}

template <typename BoneType>
bool ValidateUE58ExportedRestFbx(
    const std::filesystem::path& Path,
    const std::vector<BoneType>& Bones,
    const UEIKJsonRetargetOptions& Options,
    const std::string& Label,
    RestFbxCoordinateEvidence& Out,
    LoadedFbx& OutNormalizedScene,
    std::string& OutError)
{
    LoadedFbx Raw;
    if (!LoadFbx(
            Path, "SKRTG_UE58_exported_rest_evidence",
            Raw, OutError, false))
    {
        return false;
    }
    Out = {};
    Out.OriginalUpAxis = Raw.OriginalUpAxis;
    Out.OriginalUpSign = Raw.OriginalUpSign;
    Out.OriginalFrontParity = Raw.OriginalFrontParity;
    Out.OriginalFrontSign = Raw.OriginalFrontSign;
    Out.OriginalCoordinateSystem =
        Raw.OriginalCoordinateSystem;
    Out.OriginalUnitScaleCm = Raw.OriginalUnitScaleCm;
    Out.OriginalUnitMultiplier =
        Raw.OriginalUnitMultiplier;
    Out.AxisContractMatched =
        Raw.OriginalUpAxis ==
            static_cast<int>(FbxAxisSystem::eZAxis) &&
        Raw.OriginalUpSign == 1 &&
        Raw.OriginalFrontParity ==
            static_cast<int>(FbxAxisSystem::eParityOdd) &&
        Raw.OriginalFrontSign == -1 &&
        Raw.OriginalCoordinateSystem ==
            static_cast<int>(FbxAxisSystem::eRightHanded);
    Out.UnitContractMatched =
        std::abs(Raw.OriginalUnitScaleCm - 1.0) <= 1.0e-9 &&
        std::abs(Raw.OriginalUnitMultiplier - 1.0) <= 1.0e-9;
    if (!Out.AxisContractMatched || !Out.UnitContractMatched)
    {
        std::ostringstream Message;
        Message << Label
                << ": UE 5.8 exported-rest contract requires "
                   "centimeters, right-handed Z-up, front=-Y "
                   "(up_axis="
                << Raw.OriginalUpAxis
                << ", up_sign=" << Raw.OriginalUpSign
                << ", front_parity="
                << Raw.OriginalFrontParity
                << ", front_sign=" << Raw.OriginalFrontSign
                << ", coordinate_system="
                << Raw.OriginalCoordinateSystem
                << ", unit_scale_cm="
                << Raw.OriginalUnitScaleCm
                << ", unit_multiplier="
                << Raw.OriginalUnitMultiplier << ")";
        OutError = Message.str();
        DestroyFbx(Raw);
        return false;
    }

    BoundSkeleton RawSkeleton;
    if (!BindSkeleton(Raw.Scene, Bones, RawSkeleton, OutError))
    {
        DestroyFbx(Raw);
        return false;
    }
    Out.BoneCount = static_cast<int>(Bones.size());
    const double TranslationTolerance =
        std::min(Options.RestTranslationToleranceCm, 1.0e-3);
    const double RotationTolerance =
        std::min(Options.RestRotationToleranceDegrees, 1.0e-3);
    const double ScaleTolerance =
        std::min(Options.RestScaleTolerance, 1.0e-5);
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const TransformRT ActualLocal =
            ReflectFbxYIntoUE(
                RawSkeleton.ReferenceLocal[Index]);
        const TransformRT ActualModel =
            ReflectFbxYIntoUE(
                RawSkeleton.ReferenceModel[Index]);
        const TransformRT& ExpectedLocal =
            Bones[Index].ReferenceLocal;
        const TransformRT& ExpectedModel =
            Bones[Index].ReferenceModel;
        const double LocalTranslation = Length(Subtract(
            ActualLocal.TranslationCm,
            ExpectedLocal.TranslationCm));
        const double LocalRotation = QuaternionErrorDegrees(
            ActualLocal.Rotation, ExpectedLocal.Rotation);
        const double LocalScale = ScaleError(
            ActualLocal.Scale, ExpectedLocal.Scale);
        const double ModelTranslation = Length(Subtract(
            ActualModel.TranslationCm,
            ExpectedModel.TranslationCm));
        const double ModelRotation = QuaternionErrorDegrees(
            ActualModel.Rotation, ExpectedModel.Rotation);
        const double ModelScale = ScaleError(
            ActualModel.Scale, ExpectedModel.Scale);
        Out.MaximumLocalTranslationCm = std::max(
            Out.MaximumLocalTranslationCm, LocalTranslation);
        Out.MaximumLocalRotationDegrees = std::max(
            Out.MaximumLocalRotationDegrees, LocalRotation);
        Out.MaximumLocalScale = std::max(
            Out.MaximumLocalScale, LocalScale);
        Out.MaximumModelTranslationCm = std::max(
            Out.MaximumModelTranslationCm, ModelTranslation);
        Out.MaximumModelRotationDegrees = std::max(
            Out.MaximumModelRotationDegrees, ModelRotation);
        Out.MaximumModelScale = std::max(
            Out.MaximumModelScale, ModelScale);
        if (LocalTranslation > TranslationTolerance ||
            LocalRotation > RotationTolerance ||
            LocalScale > ScaleTolerance ||
            ModelTranslation > TranslationTolerance ||
            ModelRotation > RotationTolerance ||
            ModelScale > ScaleTolerance)
        {
            std::ostringstream Message;
            Message << Label
                    << ": fixed FBX-to-UE Y-reflection validation "
                       "failed at "
                    << Bones[Index].Name
                    << " (local_t_cm=" << LocalTranslation
                    << ", local_r_deg=" << LocalRotation
                    << ", local_s=" << LocalScale
                    << ", model_t_cm=" << ModelTranslation
                    << ", model_r_deg=" << ModelRotation
                    << ", model_s=" << ModelScale << ")";
            OutError = Message.str();
            DestroyFbx(Raw);
            return false;
        }
    }
    if (!NormalizeLoadedFbxToUEJsonAxis(Raw, OutError))
    {
        DestroyFbx(Raw);
        return false;
    }
    OutNormalizedScene = Raw;
    Raw = {};
    return true;
}

template <typename BoneType>
bool BuildAnimationBindModel(
    FbxScene* Scene,
    const std::vector<BoneType>& Bones,
    const BoundSkeleton& Skeleton,
    const double ScaleTolerance,
    AnimationBindModel& Out,
    std::string& OutError)
{
    if (Scene == nullptr || Bones.empty() ||
        Skeleton.Nodes.size() != Bones.size() ||
        Skeleton.ReferenceLocal.size() != Bones.size())
    {
        OutError = "animation bind-model input is incomplete";
        return false;
    }

    struct Candidate
    {
        TransformRT Model;
        std::string Source;
        bool FromSkinCluster = false;
    };
    std::vector<std::vector<Candidate>> Candidates(Bones.size());
    std::unordered_map<FbxNode*, std::size_t> BoneIndexByNode;
    for (std::size_t Index = 0; Index < Skeleton.Nodes.size(); ++Index)
        BoneIndexByNode.emplace(Skeleton.Nodes[Index], Index);

    std::vector<FbxNode*> Nodes;
    CollectAllNodes(Scene->GetRootNode(), Nodes);
    for (FbxNode* Node : Nodes)
    {
        FbxMesh* Mesh = Node != nullptr ? Node->GetMesh() : nullptr;
        if (Mesh == nullptr) continue;
        const int SkinCount =
            Mesh->GetDeformerCount(FbxDeformer::eSkin);
        for (int SkinIndex = 0; SkinIndex < SkinCount; ++SkinIndex)
        {
            FbxSkin* Skin = FbxCast<FbxSkin>(
                Mesh->GetDeformer(SkinIndex, FbxDeformer::eSkin));
            if (Skin == nullptr)
            {
                OutError = "animation bind-model contains an invalid skin";
                return false;
            }
            for (int ClusterIndex = 0;
                 ClusterIndex < Skin->GetClusterCount();
                 ++ClusterIndex)
            {
                FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
                if (Cluster == nullptr || Cluster->GetLink() == nullptr)
                {
                    OutError =
                        "animation bind-model contains an invalid skin cluster";
                    return false;
                }
                const auto BoneIt =
                    BoneIndexByNode.find(Cluster->GetLink());
                if (BoneIt == BoneIndexByNode.end()) continue;
                FbxAMatrix LinkBind;
                Cluster->GetTransformLinkMatrix(LinkBind);
                Candidate Value;
                Value.Model = ToTransformRT(LinkBind);
                Value.Source =
                    "skin_cluster:" +
                    std::string(
                        Node->GetName() != nullptr
                            ? Node->GetName()
                            : "") +
                    ":" + std::to_string(SkinIndex) +
                    ":" + std::to_string(ClusterIndex);
                Value.FromSkinCluster = true;
                Candidates[BoneIt->second].push_back(
                    std::move(Value));
            }
        }
    }

    for (int PoseIndex = 0;
         PoseIndex < Scene->GetPoseCount(); ++PoseIndex)
    {
        FbxPose* Pose = Scene->GetPose(PoseIndex);
        if (Pose == nullptr || !Pose->IsBindPose()) continue;
        for (int Entry = 0; Entry < Pose->GetCount(); ++Entry)
        {
            const auto BoneIt =
                BoneIndexByNode.find(Pose->GetNode(Entry));
            if (BoneIt == BoneIndexByNode.end()) continue;
            Candidate Value;
            Value.Model = ToTransformRT(Pose->GetMatrix(Entry));
            Value.Source =
                "bind_pose:" +
                std::string(
                    Pose->GetName() != nullptr
                        ? Pose->GetName()
                        : "") +
                ":" + std::to_string(Entry);
            Candidates[BoneIt->second].push_back(std::move(Value));
        }
    }

    Out = {};
    Out.Model.resize(Bones.size());
    Out.Sources.resize(Bones.size());
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const auto ClusterIt = std::find_if(
            Candidates[Index].begin(), Candidates[Index].end(),
            [](const Candidate& Value)
            {
                return Value.FromSkinCluster;
            });
        const Candidate* Selected =
            ClusterIt != Candidates[Index].end()
            ? &*ClusterIt
            : Candidates[Index].empty()
                ? nullptr
                : &Candidates[Index].front();
        if (Selected != nullptr)
        {
            Out.Model[Index] = Selected->Model;
            Out.Sources[Index] = Selected->Source;
            if (Selected->FromSkinCluster)
                ++Out.SkinClusterBoneCount;
            else
                ++Out.BindPoseBoneCount;

            for (const Candidate& Other : Candidates[Index])
            {
                const double Translation = Length(Subtract(
                    Other.Model.TranslationCm,
                    Selected->Model.TranslationCm));
                const double Rotation = QuaternionErrorDegrees(
                    Other.Model.Rotation,
                    Selected->Model.Rotation);
                const double Scale = ScaleError(
                    Other.Model.Scale,
                    Selected->Model.Scale);
                Out.MaximumDuplicateTranslationCm = std::max(
                    Out.MaximumDuplicateTranslationCm, Translation);
                Out.MaximumDuplicateRotationDegrees = std::max(
                    Out.MaximumDuplicateRotationDegrees, Rotation);
                Out.MaximumDuplicateScale = std::max(
                    Out.MaximumDuplicateScale, Scale);
                if (Translation > 1.0e-3 ||
                    Rotation > 1.0e-3 ||
                    Scale > ScaleTolerance)
                {
                    std::ostringstream Message;
                    Message
                        << "animation skin/bind-pose matrices disagree at "
                        << Bones[Index].Name
                        << " (translation_cm=" << Translation
                        << ", rotation_deg=" << Rotation
                        << ", scale=" << Scale << ")";
                    OutError = Message.str();
                    return false;
                }
            }
            if (ScaleError(
                    Selected->Model.Scale,
                    {1.0, 1.0, 1.0}) > ScaleTolerance)
            {
                OutError =
                    "animation bind model is outside the unit-scale route at bone " +
                    Bones[Index].Name;
                return false;
            }
            continue;
        }

        const int Parent = Bones[Index].ParentIndex;
        if (Parent < 0 ||
            Parent >= static_cast<int>(Index) ||
            Out.Sources[static_cast<std::size_t>(Parent)].empty())
        {
            OutError =
                "animation bind model is unavailable for non-leaf bone " +
                Bones[Index].Name;
            return false;
        }
        const bool IsLeaf = std::none_of(
            Bones.begin(), Bones.end(),
            [Index](const BoneType& CandidateBone)
            {
                return CandidateBone.ParentIndex ==
                    static_cast<int>(Index);
            });
        if (!IsLeaf)
        {
            OutError =
                "animation bind model lacks direct evidence for non-leaf bone " +
                Bones[Index].Name;
            return false;
        }
        Out.Model[Index] = Compose(
            Out.Model[static_cast<std::size_t>(Parent)],
            Skeleton.ReferenceLocal[Index]);
        Out.Model[Index].Scale = {1.0, 1.0, 1.0};
        Out.Sources[Index] =
            "derived_leaf_from_parent_bind_and_fbx_reference_local";
        ++Out.DerivedLeafBoneCount;
    }
    return true;
}

bool SceneRequiresDirectAnimationBindAudit(
    FbxScene* Scene,
    const BoundSkeleton& Skeleton)
{
    if (Scene == nullptr || Skeleton.Nodes.empty())
        return false;

    std::vector<FbxNode*> Nodes;
    CollectAllNodes(Scene->GetRootNode(), Nodes);
    for (FbxNode* Node : Nodes)
    {
        // A Mesh of any kind disqualifies the animation-only fallback.
        // Missing or damaged Skin data must reach the original bind audit
        // and fail closed instead of being treated as no bind payload.
        if (Node != nullptr && Node->GetMesh() != nullptr)
            return true;
    }

    for (int PoseIndex = 0;
         PoseIndex < Scene->GetPoseCount(); ++PoseIndex)
    {
        FbxPose* Pose = Scene->GetPose(PoseIndex);
        // The BindPose marker itself is evidence. An empty or damaged pose
        // must not be allowed to bypass the direct bind audit.
        if (Pose != nullptr && Pose->IsBindPose())
            return true;
    }
    return false;
}

template <typename BoneType>
bool BuildGoldenReferenceBindEvidence(
    const std::vector<BoneType>& RigBones,
    const std::vector<UEFbxImportExactBone>& GoldenBones,
    const UEIKJsonRetargetOptions& Options,
    AnimationBindModel& Out,
    std::string& OutError)
{
    if (RigBones.empty() ||
        RigBones.size() != GoldenBones.size())
    {
        OutError =
            "UE golden reference inventory does not match the source IK Rig";
        return false;
    }

    const double TranslationTolerance =
        std::min(Options.RestTranslationToleranceCm, 1.0e-3);
    const double RotationTolerance =
        std::min(Options.RestRotationToleranceDegrees, 1.0e-3);
    const double ScaleTolerance =
        std::min(Options.RestScaleTolerance, 1.0e-5);

    AnimationBindModel Candidate;
    Candidate.Model.resize(RigBones.size());
    Candidate.Sources.assign(
        RigBones.size(),
        "hash_bound_ue_golden_reference_no_fbx_bind_payload");
    Candidate.GoldenReferenceBoneCount =
        static_cast<int>(RigBones.size());
    Candidate.UsedGoldenReferenceFallback = true;

    for (std::size_t Index = 0;
         Index < RigBones.size(); ++Index)
    {
        const BoneType& RigBone = RigBones[Index];
        const UEFbxImportExactBone& GoldenBone =
            GoldenBones[Index];
        if (GoldenBone.Index != static_cast<int>(Index) ||
            GoldenBone.ParentIndex != RigBone.ParentIndex ||
            GoldenBone.Name != RigBone.Name)
        {
            OutError =
                "UE golden reference hierarchy does not match the source IK Rig at bone " +
                RigBone.Name;
            return false;
        }

        const double LocalTranslation = Length(Subtract(
            GoldenBone.ReferenceLocal.TranslationCm,
            RigBone.ReferenceLocal.TranslationCm));
        const double LocalRotation = QuaternionErrorDegrees(
            GoldenBone.ReferenceLocal.Rotation,
            RigBone.ReferenceLocal.Rotation);
        const double LocalScale = ScaleError(
            GoldenBone.ReferenceLocal.Scale,
            RigBone.ReferenceLocal.Scale);
        const double ModelTranslation = Length(Subtract(
            GoldenBone.ReferenceModel.TranslationCm,
            RigBone.ReferenceModel.TranslationCm));
        const double ModelRotation = QuaternionErrorDegrees(
            GoldenBone.ReferenceModel.Rotation,
            RigBone.ReferenceModel.Rotation);
        const double ModelScale = ScaleError(
            GoldenBone.ReferenceModel.Scale,
            RigBone.ReferenceModel.Scale);

        Candidate.MaximumGoldenLocalTranslationCm = std::max(
            Candidate.MaximumGoldenLocalTranslationCm,
            LocalTranslation);
        Candidate.MaximumGoldenLocalRotationDegrees = std::max(
            Candidate.MaximumGoldenLocalRotationDegrees,
            LocalRotation);
        Candidate.MaximumGoldenLocalScale = std::max(
            Candidate.MaximumGoldenLocalScale,
            LocalScale);
        Candidate.MaximumGoldenModelTranslationCm = std::max(
            Candidate.MaximumGoldenModelTranslationCm,
            ModelTranslation);
        Candidate.MaximumGoldenModelRotationDegrees = std::max(
            Candidate.MaximumGoldenModelRotationDegrees,
            ModelRotation);
        Candidate.MaximumGoldenModelScale = std::max(
            Candidate.MaximumGoldenModelScale,
            ModelScale);

        if (!std::isfinite(LocalTranslation) ||
            !std::isfinite(LocalRotation) ||
            !std::isfinite(LocalScale) ||
            !std::isfinite(ModelTranslation) ||
            !std::isfinite(ModelRotation) ||
            !std::isfinite(ModelScale) ||
            LocalTranslation > TranslationTolerance ||
            LocalRotation > RotationTolerance ||
            LocalScale > ScaleTolerance ||
            ModelTranslation > TranslationTolerance ||
            ModelRotation > RotationTolerance ||
            ModelScale > ScaleTolerance)
        {
            std::ostringstream Message;
            Message
                << "animation-only FBX lacks bind payload and its "
                   "hash-bound UE golden reference does not match "
                   "the source IK Rig at "
                << RigBone.Name
                << " (local_t_cm=" << LocalTranslation
                << ", local_r_deg=" << LocalRotation
                << ", local_s=" << LocalScale
                << ", model_t_cm=" << ModelTranslation
                << ", model_r_deg=" << ModelRotation
                << ", model_s=" << ModelScale << ")";
            OutError = Message.str();
            return false;
        }
        Candidate.Model[Index] = RigBone.ReferenceModel;
    }

    Out = std::move(Candidate);
    return true;
}

template <typename BoneType>
bool ValidateAnimationBindAgainstUEReference(
    const std::vector<BoneType>& Bones,
    const AnimationBindModel& Bind,
    const int RootIndex,
    const Quat FbxToUEModelRotation,
    AnimationBindModel& OutMetrics,
    std::string& OutError)
{
    if (Bones.empty() ||
        Bind.Model.size() != Bones.size() ||
        RootIndex < 0 ||
        RootIndex >= static_cast<int>(Bones.size()))
    {
        OutError =
            "animation bind/reference validation input is incomplete";
        return false;
    }
    const std::size_t Root =
        static_cast<std::size_t>(RootIndex);
    const Vec3 FbxRoot =
        Bind.Model[Root].TranslationCm;
    const Vec3 UERoot =
        Bones[Root].ReferenceModel.TranslationCm;
    double MaximumResidual = 0.0;
    std::string MaximumBone;
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const Vec3 Aligned = Add(
            UERoot,
            RotateVector(
                FbxToUEModelRotation,
                Subtract(
                    Bind.Model[Index].TranslationCm,
                    FbxRoot)));
        const double Residual = Length(Subtract(
            Aligned,
            Bones[Index].ReferenceModel.TranslationCm));
        if (Residual > MaximumResidual)
        {
            MaximumResidual = Residual;
            MaximumBone = Bones[Index].Name;
        }
    }
    OutMetrics.MaximumUEReferencePositionResidualCm =
        MaximumResidual;
    if (!std::isfinite(MaximumResidual) ||
        MaximumResidual > 0.1)
    {
        std::ostringstream Message;
        Message
            << "animation bind pose does not match the hash-bound UE source reference at "
            << MaximumBone << " (position residual "
            << MaximumResidual << " cm, limit 0.1 cm)";
        OutError = Message.str();
        return false;
    }
    return true;
}

TransformRT ApplyReferenceReconciliation(
    TransformRT Candidate,
    const reconciliation::RetargetLocalDelta& Delta,
    bool IsRoot)
{
    if (IsRoot)
    {
        Candidate.TranslationCm = Subtract(
            Candidate.TranslationCm,
            Delta.IsolatedRootPlacementDeltaCm);
    }
    return reconciliation::ApplyRestPoseLocalDelta(
        Candidate, Delta.Delta);
}

template <typename BoneType>
bool ValidateReferencePoseWithReconciliation(
    const std::vector<BoneType>& Bones,
    const BoundSkeleton& Fbx,
    const std::vector<reconciliation::RetargetLocalDelta>& Deltas,
    const UEIKJsonRetargetOptions& Options,
    const std::string& Label,
    RestValidation& Out,
    std::string& OutError)
{
    if (Bones.size() != Fbx.ReferenceLocal.size() ||
        Bones.size() != Fbx.ReferenceModel.size() ||
        Bones.size() != Deltas.size())
    {
        OutError = Label + ": reference pose inventory mismatch";
        return false;
    }
    std::vector<TransformRT> ReconciledModel(Bones.size());
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const TransformRT& ExpectedLocal = Bones[Index].ReferenceLocal;
        const TransformRT& ActualLocal = Fbx.ReferenceLocal[Index];
        const TransformRT& ExpectedModel = Bones[Index].ReferenceModel;
        const TransformRT& ActualModel = Fbx.ReferenceModel[Index];
        const TransformRT ReconciledLocal =
            ApplyReferenceReconciliation(
                ActualLocal, Deltas[Index],
                Bones[Index].ParentIndex < 0);
        ReconciledModel[Index] =
            Bones[Index].ParentIndex < 0
            ? ReconciledLocal
            : Compose(
                ReconciledModel[static_cast<std::size_t>(
                    Bones[Index].ParentIndex)],
                ReconciledLocal);

        const double RawLocalTranslation = Length(Subtract(
            ActualLocal.TranslationCm,
            ExpectedLocal.TranslationCm));
        const double RawLocalRotation = QuaternionErrorDegrees(
            ActualLocal.Rotation, ExpectedLocal.Rotation);
        const double RawLocalScale =
            ScaleError(ActualLocal.Scale, ExpectedLocal.Scale);
        const double RawModelTranslation = Length(Subtract(
            ActualModel.TranslationCm,
            ExpectedModel.TranslationCm));
        const double RawModelRotation = QuaternionErrorDegrees(
            ActualModel.Rotation, ExpectedModel.Rotation);
        const double RawModelScale =
            ScaleError(ActualModel.Scale, ExpectedModel.Scale);
        Out.MaximumRawLocalTranslationCm = std::max(
            Out.MaximumRawLocalTranslationCm, RawLocalTranslation);
        Out.MaximumRawLocalRotationDegrees = std::max(
            Out.MaximumRawLocalRotationDegrees, RawLocalRotation);
        Out.MaximumRawLocalScale = std::max(
            Out.MaximumRawLocalScale, RawLocalScale);
        Out.MaximumRawModelTranslationCm = std::max(
            Out.MaximumRawModelTranslationCm, RawModelTranslation);
        Out.MaximumRawModelRotationDegrees = std::max(
            Out.MaximumRawModelRotationDegrees, RawModelRotation);
        Out.MaximumRawModelScale = std::max(
            Out.MaximumRawModelScale, RawModelScale);

        const double LocalTranslation = Length(Subtract(
            ReconciledLocal.TranslationCm,
            ExpectedLocal.TranslationCm));
        const double LocalRotation = QuaternionErrorDegrees(
            ReconciledLocal.Rotation, ExpectedLocal.Rotation);
        const double LocalScale =
            ScaleError(ReconciledLocal.Scale, ExpectedLocal.Scale);
        const double ModelTranslation = Length(Subtract(
            ReconciledModel[Index].TranslationCm,
            ExpectedModel.TranslationCm));
        const double ModelRotation = QuaternionErrorDegrees(
            ReconciledModel[Index].Rotation, ExpectedModel.Rotation);
        const double ModelScale =
            ScaleError(ReconciledModel[Index].Scale, ExpectedModel.Scale);
        Out.MaximumLocalTranslationCm = std::max(
            Out.MaximumLocalTranslationCm, LocalTranslation);
        Out.MaximumLocalRotationDegrees = std::max(
            Out.MaximumLocalRotationDegrees, LocalRotation);
        Out.MaximumLocalScale = std::max(
            Out.MaximumLocalScale, LocalScale);
        Out.MaximumModelTranslationCm = std::max(
            Out.MaximumModelTranslationCm, ModelTranslation);
        Out.MaximumModelRotationDegrees = std::max(
            Out.MaximumModelRotationDegrees, ModelRotation);
        Out.MaximumModelScale = std::max(
            Out.MaximumModelScale, ModelScale);

        const double SegmentLengthError = std::abs(
            Length(ActualLocal.TranslationCm) -
            Length(ExpectedLocal.TranslationCm));
        const double UnitScaleError =
            ScaleError(ActualLocal.Scale, {1.0, 1.0, 1.0});
        if (SegmentLengthError >
                Options.RestTranslationToleranceCm ||
            UnitScaleError > Options.RestScaleTolerance ||
            LocalTranslation > Options.RestTranslationToleranceCm ||
            LocalRotation > Options.RestRotationToleranceDegrees ||
            LocalScale > Options.RestScaleTolerance ||
            ModelTranslation > Options.RestTranslationToleranceCm ||
            ModelRotation > Options.RestRotationToleranceDegrees ||
            ModelScale > Options.RestScaleTolerance)
        {
            std::ostringstream Message;
            Message << Label
                    << ": hash-bound reference reconciliation failed at "
                    << Bones[Index].Name
                    << " (segment_length_cm=" << SegmentLengthError
                    << ", unit_scale=" << UnitScaleError
                    << ", reconciled_local_t_cm=" << LocalTranslation
                    << ", reconciled_local_r_deg=" << LocalRotation
                    << ", reconciled_local_s=" << LocalScale
                    << ", reconciled_model_t_cm=" << ModelTranslation
                    << ", reconciled_model_r_deg=" << ModelRotation
                    << ", reconciled_model_s=" << ModelScale << ")";
            OutError = Message.str();
            return false;
        }
    }
    return true;
}

template <typename BoneType>
bool BuildReferenceReconciliation(
    const std::vector<BoneType>& Bones,
    const BoundSkeleton& Fbx,
    const UEIKJsonRetargetOptions& Options,
    const std::string& Label,
    std::vector<reconciliation::RetargetLocalDelta>& OutDeltas,
    RestValidation& OutValidation,
    std::string& OutError)
{
    if (Bones.size() != Fbx.ReferenceLocal.size())
    {
        OutError = Label + ": reference pose inventory mismatch";
        return false;
    }
    OutDeltas.clear();
    OutDeltas.reserve(Bones.size());
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        OutDeltas.push_back(
            reconciliation::ComputeRestPoseLocalDelta(
                Bones[Index].ReferenceLocal,
                Fbx.ReferenceLocal[Index],
                Bones[Index].ParentIndex < 0,
                true));
    }
    return ValidateReferencePoseWithReconciliation(
        Bones, Fbx, OutDeltas, Options, Label,
        OutValidation, OutError);
}

bool SceneContainsMesh(FbxNode* Node)
{
    if (Node == nullptr) return false;
    if (Node->GetMesh() != nullptr) return true;
    for (int Index = 0; Index < Node->GetChildCount(); ++Index)
    {
        if (SceneContainsMesh(Node->GetChild(Index))) return true;
    }
    return false;
}

FbxTimeSpan ResolveTimeSpan(FbxScene* Scene, FbxAnimStack* Stack)
{
    FbxTimeSpan Span = Stack->GetLocalTimeSpan();
    if (Span.GetDuration().Get() <= 0)
    {
        FbxTakeInfo* TakeInfo =
            Scene != nullptr ? Scene->GetTakeInfo(Stack->GetName()) : nullptr;
        if (TakeInfo != nullptr) Span = TakeInfo->mLocalTimeSpan;
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
         Scene != nullptr &&
         Index < Scene->GetSrcObjectCount<FbxAnimStack>();
         ++Index)
    {
        FbxAnimStack* Candidate =
            Scene->GetSrcObject<FbxAnimStack>(Index);
        if (Candidate == nullptr) continue;
        const std::string Name =
            Candidate->GetName() != nullptr ? Candidate->GetName() : "";
        const FbxTimeSpan Span = ResolveTimeSpan(Scene, Candidate);
        const long long Duration = Span.GetDuration().Get();
        if (!RequestedName.empty())
        {
            if (Name == RequestedName)
            {
                OutStack = Candidate;
                OutSpan = Span;
                break;
            }
        }
        else if (Duration > BestDuration)
        {
            BestDuration = Duration;
            OutStack = Candidate;
            OutSpan = Span;
        }
    }
    if (OutStack == nullptr)
    {
        OutError = RequestedName.empty()
            ? "source FBX has no positive-duration animation stack"
            : "requested animation stack is absent: " + RequestedName;
        return false;
    }
    if (OutSpan.GetDuration().Get() <= 0)
    {
        OutError = "selected animation stack has no positive duration";
        return false;
    }
    Scene->SetCurrentAnimationStack(OutStack);
    return true;
}

Vec3 CrossVector(const Vec3& Left, const Vec3& Right)
{
    return {
        Left.Y * Right.Z - Left.Z * Right.Y,
        Left.Z * Right.X - Left.X * Right.Z,
        Left.X * Right.Y - Left.Y * Right.X};
}

bool NormalizeVector(const Vec3& Value, Vec3& Out)
{
    const double Magnitude = Length(Value);
    if (!std::isfinite(Magnitude) || Magnitude <= 1.0e-9)
        return false;
    Out = core::math::Scale(Value, 1.0 / Magnitude);
    return true;
}

Quat QuaternionFromRotationMatrix(
    const std::array<std::array<double, 3>, 3>& Matrix)
{
    Quat Result;
    const double Trace =
        Matrix[0][0] + Matrix[1][1] + Matrix[2][2];
    if (Trace > 0.0)
    {
        const double S = std::sqrt(Trace + 1.0) * 2.0;
        Result.W = 0.25 * S;
        Result.X = (Matrix[2][1] - Matrix[1][2]) / S;
        Result.Y = (Matrix[0][2] - Matrix[2][0]) / S;
        Result.Z = (Matrix[1][0] - Matrix[0][1]) / S;
    }
    else if (Matrix[0][0] > Matrix[1][1] &&
             Matrix[0][0] > Matrix[2][2])
    {
        const double S = std::sqrt(
            1.0 + Matrix[0][0] -
            Matrix[1][1] - Matrix[2][2]) * 2.0;
        Result.W = (Matrix[2][1] - Matrix[1][2]) / S;
        Result.X = 0.25 * S;
        Result.Y = (Matrix[0][1] + Matrix[1][0]) / S;
        Result.Z = (Matrix[0][2] + Matrix[2][0]) / S;
    }
    else if (Matrix[1][1] > Matrix[2][2])
    {
        const double S = std::sqrt(
            1.0 + Matrix[1][1] -
            Matrix[0][0] - Matrix[2][2]) * 2.0;
        Result.W = (Matrix[0][2] - Matrix[2][0]) / S;
        Result.X = (Matrix[0][1] + Matrix[1][0]) / S;
        Result.Y = 0.25 * S;
        Result.Z = (Matrix[1][2] + Matrix[2][1]) / S;
    }
    else
    {
        const double S = std::sqrt(
            1.0 + Matrix[2][2] -
            Matrix[0][0] - Matrix[1][1]) * 2.0;
        Result.W = (Matrix[1][0] - Matrix[0][1]) / S;
        Result.X = (Matrix[0][2] + Matrix[2][0]) / S;
        Result.Y = (Matrix[1][2] + Matrix[2][1]) / S;
        Result.Z = 0.25 * S;
    }
    return Normalize(Result);
}

bool BuildBodyModelBasis(
    const Vec3& Root,
    const Vec3& Head,
    const Vec3& LeftHand,
    const Vec3& RightHand,
    std::array<Vec3, 3>& Out)
{
    Vec3 Right;
    if (!NormalizeVector(Subtract(RightHand, LeftHand), Right))
        return false;
    const Vec3 UpCandidate = Subtract(Head, Root);
    const Vec3 UpWithoutRight = Subtract(
        UpCandidate,
        core::math::Scale(
            Right, core::math::Dot(UpCandidate, Right)));
    Vec3 Up;
    Vec3 Forward;
    if (!NormalizeVector(UpWithoutRight, Up) ||
        !NormalizeVector(CrossVector(Right, Up), Forward))
    {
        return false;
    }
    if (!NormalizeVector(CrossVector(Forward, Right), Up))
        return false;
    Out = {Forward, Right, Up};
    return true;
}

bool BuildSourceFbxToUEModelRotation(
    const retarget::UEIKJsonRoute& Route,
    const std::vector<TransformRT>& SourceFbxRestModel,
    Quat& OutRotation,
    double& OutMaximumResidualCm,
    std::string& OutError)
{
    const auto FindPairEnd =
        [&](const std::string& CanonicalName) -> int
        {
            const auto It = std::find_if(
                Route.ChainPairs.begin(), Route.ChainPairs.end(),
                [&](const retarget::UEIKJsonChainPair& Pair)
                {
                    return Pair.CanonicalChainName == CanonicalName &&
                        !Pair.SourceBoneIndices.empty();
                });
            return It == Route.ChainPairs.end()
                ? -1 : It->SourceBoneIndices.back();
        };
    const int Root = Route.SourceRootIndex;
    const int Head = FindPairEnd("Head");
    const int LeftHand = FindPairEnd("LeftArm");
    const int RightHand = FindPairEnd("RightArm");
    const auto Valid = [&](const int Index)
    {
        return Index >= 0 &&
            static_cast<std::size_t>(Index) <
                SourceFbxRestModel.size() &&
            static_cast<std::size_t>(Index) <
                Route.SourceRig.Bones.size();
    };
    if (!Valid(Root) || !Valid(Head) ||
        !Valid(LeftHand) || !Valid(RightHand))
    {
        OutError =
            "Head/LeftArm/RightArm chain endpoints are required for the explicit FBX-to-UE body basis";
        return false;
    }

    const auto FbxPosition = [&](const int Index)
    {
        return SourceFbxRestModel[
            static_cast<std::size_t>(Index)].TranslationCm;
    };
    const auto UEPosition = [&](const int Index)
    {
        return Route.SourceRig.Bones[
            static_cast<std::size_t>(Index)]
            .ReferenceModel.TranslationCm;
    };
    std::array<Vec3, 3> SourceBasis;
    std::array<Vec3, 3> TargetBasis;
    if (!BuildBodyModelBasis(
            FbxPosition(Root), FbxPosition(Head),
            FbxPosition(LeftHand), FbxPosition(RightHand),
            SourceBasis) ||
        !BuildBodyModelBasis(
            UEPosition(Root), UEPosition(Head),
            UEPosition(LeftHand), UEPosition(RightHand),
            TargetBasis))
    {
        OutError =
            "FBX-to-UE body basis is degenerate";
        return false;
    }

    std::array<std::array<double, 3>, 3> Rotation{};
    const auto Component = [](const Vec3& Value, const int Axis)
    {
        return Axis == 0 ? Value.X :
            (Axis == 1 ? Value.Y : Value.Z);
    };
    for (int Row = 0; Row < 3; ++Row)
    {
        for (int Column = 0; Column < 3; ++Column)
        {
            for (int BasisIndex = 0;
                 BasisIndex < 3; ++BasisIndex)
            {
                Rotation[Row][Column] +=
                    Component(TargetBasis[BasisIndex], Row) *
                    Component(SourceBasis[BasisIndex], Column);
            }
        }
    }
    OutRotation = QuaternionFromRotationMatrix(Rotation);

    OutMaximumResidualCm = 0.0;
    for (std::size_t Index = 0;
         Index < SourceFbxRestModel.size(); ++Index)
    {
        const Vec3 Rotated = RotateVector(
            OutRotation,
            Subtract(
                SourceFbxRestModel[Index].TranslationCm,
                FbxPosition(Root)));
        const Vec3 Expected =
            Subtract(
                Route.SourceRig.Bones[Index]
                    .ReferenceModel.TranslationCm,
                UEPosition(Root));
        OutMaximumResidualCm = std::max(
            OutMaximumResidualCm,
            Length(Subtract(Rotated, Expected)));
    }
    if (!std::isfinite(OutMaximumResidualCm) ||
        OutMaximumResidualCm > 0.1)
    {
        std::ostringstream Message;
        Message << "explicit all-bone FBX-to-UE reference alignment residual is "
                << OutMaximumResidualCm
                << " cm (limit 0.1 cm)";
        OutError = Message.str();
        return false;
    }
    return true;
}

bool AlignSourceAnimationModelPose(
    const retarget::UEIKJsonRoute& Route,
    const AnimationBindModel& SourceAnimationBind,
    const Quat SourceFbxToUEModelRotation,
    const std::vector<TransformRT>& RawSourceModel,
    const double ScaleTolerance,
    std::vector<TransformRT>& OutLocal,
    std::vector<TransformRT>& OutModel,
    std::string& OutError)
{
    const std::size_t BoneCount =
        Route.SourceRig.Bones.size();
    if (BoneCount == 0 ||
        RawSourceModel.size() != BoneCount ||
        SourceAnimationBind.Model.size() != BoneCount ||
        Route.SourceRootIndex < 0 ||
        Route.SourceRootIndex >=
            static_cast<int>(BoneCount))
    {
        OutError =
            "source animation alignment inventory mismatch";
        return false;
    }

    const std::size_t Root =
        static_cast<std::size_t>(
            Route.SourceRootIndex);
    const Vec3 FbxReferenceRoot =
        SourceAnimationBind.Model[Root]
            .TranslationCm;
    const Vec3 UEReferenceRoot =
        Route.SourceRig.Bones[Root]
            .ReferenceModel.TranslationCm;
    const Quat FbxToUEInverse =
        Conjugate(SourceFbxToUEModelRotation);
    std::vector<TransformRT> CandidateLocal(
        BoneCount);
    std::vector<TransformRT> CandidateModel(
        BoneCount);
    for (std::size_t BoneIndex = 0;
         BoneIndex < BoneCount;
         ++BoneIndex)
    {
        if (ScaleError(
                RawSourceModel[BoneIndex].Scale,
                {1.0, 1.0, 1.0}) >
            ScaleTolerance)
        {
            OutError =
                "animated source global scale is outside the unit-scale route at bone " +
                Route.SourceRig.Bones[BoneIndex].Name;
            return false;
        }
        const Quat FbxModelMotionDelta =
            Normalize(Multiply(
                RawSourceModel[BoneIndex].Rotation,
                Conjugate(
                    SourceAnimationBind.Model[BoneIndex]
                        .Rotation)));
        const Quat UEModelMotionDelta =
            Normalize(Multiply(
                Multiply(
                    SourceFbxToUEModelRotation,
                    FbxModelMotionDelta),
                FbxToUEInverse));
        TransformRT Model;
        Model.TranslationCm = Add(
            UEReferenceRoot,
            RotateVector(
                SourceFbxToUEModelRotation,
                Subtract(
                    RawSourceModel[BoneIndex]
                        .TranslationCm,
                    FbxReferenceRoot)));
        Model.Rotation =
            Normalize(Multiply(
                UEModelMotionDelta,
                Route.SourceRig.Bones[BoneIndex]
                    .ReferenceModel.Rotation));
        Model.Scale = {1.0, 1.0, 1.0};
        CandidateModel[BoneIndex] = Model;
        const int Parent =
            Route.SourceRig.Bones[BoneIndex]
                .ParentIndex;
        if (Parent < 0)
        {
            CandidateLocal[BoneIndex] = Model;
        }
        else if (!RelativeUnitScaleTransform(
                     CandidateModel[
                         static_cast<std::size_t>(
                             Parent)],
                     Model,
                     CandidateLocal[BoneIndex],
                     ScaleTolerance))
        {
            OutError =
                "aligned source pose could not be converted to local space at bone " +
                Route.SourceRig.Bones[BoneIndex].Name;
            return false;
        }
    }

    std::vector<TransformRT> RebuiltModel(
        BoneCount);
    double MaximumPositionErrorCm = 0.0;
    double MaximumRotationErrorDegrees = 0.0;
    for (std::size_t BoneIndex = 0;
         BoneIndex < BoneCount;
         ++BoneIndex)
    {
        const int Parent =
            Route.SourceRig.Bones[BoneIndex]
                .ParentIndex;
        RebuiltModel[BoneIndex] =
            Parent < 0
            ? CandidateLocal[BoneIndex]
            : Compose(
                RebuiltModel[
                    static_cast<std::size_t>(
                        Parent)],
                CandidateLocal[BoneIndex]);
        MaximumPositionErrorCm = std::max(
            MaximumPositionErrorCm,
            Length(Subtract(
                RebuiltModel[BoneIndex].TranslationCm,
                CandidateModel[BoneIndex].TranslationCm)));
        MaximumRotationErrorDegrees = std::max(
            MaximumRotationErrorDegrees,
            QuaternionErrorDegrees(
                RebuiltModel[BoneIndex].Rotation,
                CandidateModel[BoneIndex].Rotation));
    }
    if (!std::isfinite(MaximumPositionErrorCm) ||
        !std::isfinite(
            MaximumRotationErrorDegrees) ||
        MaximumPositionErrorCm > 1.0e-5 ||
        MaximumRotationErrorDegrees > 1.0e-5)
    {
        std::ostringstream Message;
        Message
            << "aligned source local/model round-trip residual exceeds limits: position="
            << MaximumPositionErrorCm
            << " cm rotation="
            << MaximumRotationErrorDegrees
            << " degrees";
        OutError = Message.str();
        return false;
    }

    OutLocal = std::move(CandidateLocal);
    OutModel = std::move(CandidateModel);
    return true;
}

bool BuildReconciledSourceAnimationPose(
    const retarget::UEIKJsonRoute& Route,
    const BoundSkeleton& SourceAnimation,
    const AnimationBindModel& SourceAnimationBind,
    const Quat SourceFbxToUEModelRotation,
    const FbxTime& Time,
    const double ScaleTolerance,
    std::vector<TransformRT>& OutLocal,
    std::vector<TransformRT>& OutModel,
    std::vector<TransformRT>* OutRawFbxModel,
    std::string& OutError)
{
    const std::size_t BoneCount = Route.SourceRig.Bones.size();
    if (BoneCount == 0 ||
        SourceAnimation.Nodes.size() != BoneCount ||
        SourceAnimationBind.Model.size() != BoneCount)
    {
        OutError = "source animation pose inventory mismatch";
        return false;
    }

    std::vector<TransformRT> FbxModel(BoneCount);
    for (std::size_t BoneIndex = 0;
         BoneIndex < BoneCount; ++BoneIndex)
    {
        FbxNode* Node = SourceAnimation.Nodes[BoneIndex];
        if (Node == nullptr)
        {
            OutError =
                "source animation bone node is unavailable: " +
                Route.SourceRig.Bones[BoneIndex].Name;
            return false;
        }
        FbxModel[BoneIndex] = ToTransformRT(
            Node->EvaluateGlobalTransform(
                Time, FbxNode::eSourcePivot, false, true));
        if (ScaleError(
                FbxModel[BoneIndex].Scale,
                {1.0, 1.0, 1.0}) > ScaleTolerance)
        {
            OutError =
                "animated source global scale is outside the unit-scale route at bone " +
                Route.SourceRig.Bones[BoneIndex].Name;
            return false;
        }
    }
    if (OutRawFbxModel != nullptr)
        *OutRawFbxModel = FbxModel;

    return AlignSourceAnimationModelPose(
        Route,
        SourceAnimationBind,
        SourceFbxToUEModelRotation,
        FbxModel,
        ScaleTolerance,
        OutLocal,
        OutModel,
        OutError);
}

std::vector<RetargetReviewBone> BuildReviewBones(
    const std::vector<retarget::UEIKJsonBone>& Bones,
    const BoundSkeleton& Paths,
    const std::set<int>& IkIndices,
    const std::vector<TransformRT>* ModelRestOverride = nullptr)
{
    std::vector<RetargetReviewBone> Result;
    Result.reserve(Bones.size());
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        RetargetReviewBone Bone;
        Bone.ParentIndex = Bones[Index].ParentIndex;
        Bone.Name = Bones[Index].Name;
        Bone.Path = Paths.Paths[Index];
        Bone.ParticipatesInIk =
            IkIndices.find(static_cast<int>(Index)) != IkIndices.end();
        Bone.RestModelPositionCm =
            ModelRestOverride != nullptr &&
                    ModelRestOverride->size() == Bones.size()
            ? (*ModelRestOverride)[Index].TranslationCm
            : Bones[Index].RetargetModel.TranslationCm;
        Result.push_back(std::move(Bone));
    }
    return Result;
}

bool BuildReviewChains(
    const retarget::UEIKJsonRoute& Route,
    std::vector<RetargetReviewChain>& Out,
    std::set<int>& OutSourceIk,
    std::set<int>& OutTargetIk,
    std::string& OutError)
{
    for (const retarget::UEIKJsonChainPair& Pair :
         Route.ChainPairs)
    {
        RetargetReviewChain Chain;
        Chain.Label = Pair.CanonicalChainName;
        Chain.SourceBoneIndices = Pair.SourceBoneIndices;
        Chain.TargetBoneIndices = Pair.TargetBoneIndices;
        Chain.IkMode = Pair.EnableIk ? "two_bone" : "fk_only";
        if (Pair.EnableIk)
        {
            if (Pair.SourceBoneIndices.size() != 3 ||
                Pair.TargetBoneIndices.size() != 3 ||
                Pair.SourceGoalName.empty() ||
                Pair.TargetGoalName.empty())
            {
                OutError =
                    "UE Run IK Rig limb chain is not an explicit analytic two-bone Goal chain: " +
                    Pair.CanonicalChainName;
                return false;
            }
            Chain.SourceGoalName = Pair.SourceGoalName;
            Chain.TargetGoalName = Pair.TargetGoalName;
            Chain.SourceGoalBoneIndex =
                Pair.SourceBoneIndices.back();
            Chain.TargetGoalBoneIndex =
                Pair.TargetBoneIndices.back();
            Chain.SourcePoleBoneIndex =
                Pair.SourceBoneIndices[1];
            Chain.TargetPoleBoneIndex =
                Pair.TargetBoneIndices[1];
            OutSourceIk.insert(
                Pair.SourceBoneIndices.begin(),
                Pair.SourceBoneIndices.end());
            OutTargetIk.insert(
                Pair.TargetBoneIndices.begin(),
                Pair.TargetBoneIndices.end());
        }
        Out.push_back(std::move(Chain));
    }
    return true;
}

bool BuildAnchor(
    const std::string& Label,
    int SourceIndex,
    int TargetIndex,
    const retarget::UEIKJsonRoute& Route,
    const BoundSkeleton& SourcePaths,
    const BoundSkeleton& TargetPaths,
    const std::vector<TransformRT>& SourceFbxRestModel,
    const Quat SourceFbxToUEModelRotation,
    RetargetReviewAnchor& Out,
    std::string& OutError)
{
    if (SourceIndex < 0 ||
        SourceIndex >= static_cast<int>(Route.SourceRig.Bones.size()) ||
        TargetIndex < 0 ||
        TargetIndex >= static_cast<int>(Route.TargetRig.Bones.size()) ||
        SourceFbxRestModel.size() !=
            Route.SourceRig.Bones.size())
    {
        OutError = "UE JSON anchor index is invalid: " + Label;
        return false;
    }
    Out.Label = Label;
    Out.SourceBoneIndex = SourceIndex;
    Out.TargetBoneIndex = TargetIndex;
    Out.SourcePath =
        SourcePaths.Paths[static_cast<std::size_t>(SourceIndex)];
    Out.TargetPath =
        TargetPaths.Paths[static_cast<std::size_t>(TargetIndex)];
    Out.SourceToTargetRestBasis = Normalize(Multiply(
        Conjugate(
            Route.TargetRig.Bones[
                static_cast<std::size_t>(TargetIndex)]
                .RetargetModel.Rotation),
        Multiply(
            SourceFbxToUEModelRotation,
            SourceFbxRestModel[
                static_cast<std::size_t>(SourceIndex)]
                .Rotation)));
    return true;
}

bool ResolveHeadAnchorIndices(
    const retarget::UEIKJsonRoute& Route,
    int& OutSource,
    int& OutTarget,
    std::string& OutError)
{
    const auto It = std::find_if(
        Route.ChainPairs.begin(), Route.ChainPairs.end(),
        [](const retarget::UEIKJsonChainPair& Pair)
        {
            return Pair.CanonicalChainName == "Head";
        });
    if (It == Route.ChainPairs.end() ||
        It->SourceBoneIndices.empty() ||
        It->TargetBoneIndices.empty())
    {
        OutError =
            "canonical Head chain is required for the review anchor contract";
        return false;
    }
    OutSource = It->SourceBoneIndices.back();
    OutTarget = It->TargetBoneIndices.back();
    return true;
}

Json RestValidationJson(const RestValidation& Value)
{
    return {
        {"maximumRawLocalTranslationCm",
         Value.MaximumRawLocalTranslationCm},
        {"maximumRawLocalRotationDegrees",
         Value.MaximumRawLocalRotationDegrees},
        {"maximumRawLocalScale",
         Value.MaximumRawLocalScale},
        {"maximumRawModelTranslationCm",
         Value.MaximumRawModelTranslationCm},
        {"maximumRawModelRotationDegrees",
         Value.MaximumRawModelRotationDegrees},
        {"maximumRawModelScale",
         Value.MaximumRawModelScale},
        {"maximumLocalTranslationCm",
         Value.MaximumLocalTranslationCm},
        {"maximumLocalRotationDegrees",
         Value.MaximumLocalRotationDegrees},
        {"maximumLocalScale", Value.MaximumLocalScale},
        {"maximumModelTranslationCm",
         Value.MaximumModelTranslationCm},
        {"maximumModelRotationDegrees",
         Value.MaximumModelRotationDegrees},
        {"maximumModelScale", Value.MaximumModelScale}};
}

Json RestFbxCoordinateEvidenceJson(
    const RestFbxCoordinateEvidence& Value)
{
    return {
        {"axisContractMatched", Value.AxisContractMatched},
        {"unitContractMatched", Value.UnitContractMatched},
        {"boneCount", Value.BoneCount},
        {"originalAxis",
         {{"upAxis", Value.OriginalUpAxis},
          {"upSign", Value.OriginalUpSign},
          {"frontParity", Value.OriginalFrontParity},
          {"frontSign", Value.OriginalFrontSign},
          {"coordinateSystem",
           Value.OriginalCoordinateSystem}}},
        {"originalUnit",
         {{"scaleCm", Value.OriginalUnitScaleCm},
          {"multiplier", Value.OriginalUnitMultiplier}}},
        {"maximumLocalTranslationCm",
         Value.MaximumLocalTranslationCm},
        {"maximumLocalRotationDegrees",
         Value.MaximumLocalRotationDegrees},
        {"maximumLocalScale", Value.MaximumLocalScale},
        {"maximumModelTranslationCm",
         Value.MaximumModelTranslationCm},
        {"maximumModelRotationDegrees",
         Value.MaximumModelRotationDegrees},
        {"maximumModelScale", Value.MaximumModelScale}};
}

Json BoundInputJson(const BoundInput& Input)
{
    return {
        {"path", Input.Path.string()},
        {"sha256", Input.Sha256},
        {"sizeBytes", Input.Size}};
}
} // namespace

UEIKJsonRetargetResult GenerateUEIKJsonRetargetReview(
    const UEIKJsonRetargetOptions& Options)
{
    UEIKJsonRetargetResult Result;
    LoadedFbx SourceRestScene;
    LoadedFbx SourceAnimationScene;
    LoadedFbx TargetRestScene;
    auto CleanupScenes = [&]()
    {
        DestroyFbx(SourceRestScene);
        DestroyFbx(SourceAnimationScene);
        DestroyFbx(TargetRestScene);
    };
    auto Fail = [&](const std::string& Message)
    {
        CleanupScenes();
        Result.Success = false;
        Result.Errors.push_back(Message);
        Result.ConsoleSummary =
            "UE IK JSON retarget worker failed closed: " + Message;
        Result.Artifacts.clear();
        return Result;
    };

    if (Options.OutputDirectory.empty() ||
        Options.ClipId.empty() || Options.ClipLabel.empty() ||
        !std::isfinite(Options.SampleRate) ||
        Options.SampleRate <= 0.0 || Options.SampleRate > 240.0 ||
        !std::isfinite(Options.RestTranslationToleranceCm) ||
        !std::isfinite(Options.RestRotationToleranceDegrees) ||
        !std::isfinite(Options.RestScaleTolerance) ||
        Options.RestTranslationToleranceCm <= 0.0 ||
        Options.RestRotationToleranceDegrees <= 0.0 ||
        Options.RestScaleTolerance <= 0.0 ||
        (Options.OperationStackJsonPath.empty() !=
         Options.OperationStackJsonExpectedSha256.empty()))
    {
        return Fail("worker options are incomplete or out of bounds");
    }

    std::string Error;
    std::vector<
        std::pair<std::filesystem::path, std::string>>
        RequestedInputs{
            {Options.Route.SourceRigJson,
             Options.SourceRigJsonExpectedSha256},
            {Options.Route.TargetRigJson,
             Options.TargetRigJsonExpectedSha256},
            {Options.Route.SourceAlignmentRetargeterJson,
             Options.SourceAlignmentRetargeterJsonExpectedSha256},
            {Options.Route.TargetAlignmentRetargeterJson,
             Options.TargetAlignmentRetargeterJsonExpectedSha256},
            {Options.SourceRestFbxPath,
             Options.SourceRestFbxExpectedSha256},
            {Options.SourceAnimationFbxPath,
             Options.SourceAnimationFbxExpectedSha256},
            {Options.TargetRestFbxPath,
             Options.TargetRestFbxExpectedSha256}};
    if (Options.SourceFbxImportMode ==
        UEIKSourceFbxImportMode::UE58ExactGoldenV1)
    {
        RequestedInputs.push_back({
            Options.SourceAnimationGoldenJsonPath,
            Options
                .SourceAnimationGoldenJsonExpectedSha256});
    }
    const bool HasOperationStack =
        !Options.OperationStackJsonPath.empty();
    if (HasOperationStack)
    {
        RequestedInputs.push_back({
            Options.OperationStackJsonPath,
            Options.OperationStackJsonExpectedSha256});
    }
    std::vector<BoundInput> Inputs(
        RequestedInputs.size());
    for (std::size_t Index = 0;
         Index < RequestedInputs.size(); ++Index)
    {
        if (!CaptureBoundInput(
                RequestedInputs[Index].first,
                RequestedInputs[Index].second,
                Inputs[Index], Error))
        {
            return Fail(Error);
        }
    }

    const retarget::UEIKJsonRouteLoadResult RouteLoad =
        retarget::LoadUEIKJsonCanonicalBridgeRoute(Options.Route);
    if (!RouteLoad.Success)
    {
        const std::string Message = RouteLoad.Errors.empty()
            ? "UE IK JSON route load failed"
            : RouteLoad.Errors.front();
        return Fail(Message);
    }
    const retarget::UEIKJsonRoute& Route = RouteLoad.Route;
    const bool UseExactSourceImport =
        Options.SourceFbxImportMode ==
        UEIKSourceFbxImportMode::UE58ExactGoldenV1;
    const bool UseUE58ExportedRestImport =
        Options.RestFbxImportMode ==
        UEIKRestFbxImportMode::UE58ExportedYReflectionV1;
    if (UseUE58ExportedRestImport && !UseExactSourceImport)
    {
        return Fail(
            "ue5.8_exported_y_reflection_v1 rest import requires "
            "ue5.8_exact_golden_v1 source animation import");
    }
    UEFbxImportExactResult ExactSourceClip;
    if (UseExactSourceImport)
    {
        UEFbxImportExactOptions ExactOptions;
        ExactOptions.FbxPath =
            Options.SourceAnimationFbxPath;
        ExactOptions.FbxExpectedSha256 =
            Options.SourceAnimationFbxExpectedSha256;
        ExactOptions.AnimationGoldenJsonPath =
            Options.SourceAnimationGoldenJsonPath;
        ExactOptions.AnimationGoldenJsonExpectedSha256 =
            Options
                .SourceAnimationGoldenJsonExpectedSha256;
        ExactOptions.AnimationStackName =
            Options.AnimationStackName;
        ExactOptions.OutputSampleRate =
            Options.SampleRate;
        ExactOptions.TranslationToleranceCm =
            std::min(
                Options.RestTranslationToleranceCm,
                1.0e-3);
        ExactOptions.RotationToleranceDegrees =
            std::min(
                Options.RestRotationToleranceDegrees,
                1.0e-3);
        ExactOptions.ScaleTolerance =
            std::min(
                Options.RestScaleTolerance,
                1.0e-5);
        ExactSourceClip =
            LoadUEFbxImportExactClip(ExactOptions);
        if (!ExactSourceClip.Success)
        {
            const std::string Message =
                ExactSourceClip.Errors.empty()
                ? "ue_fbx_import_exact_v1 failed"
                : ExactSourceClip.Errors.front();
            return Fail(Message);
        }
        if (ExactSourceClip.Bones.size() !=
                Route.SourceRig.Bones.size() ||
            ExactSourceClip.Frames.empty())
        {
            return Fail(
                "ue_fbx_import_exact_v1 output does not match the source IK Rig inventory");
        }
        for (std::size_t BoneIndex = 0;
             BoneIndex < ExactSourceClip.Bones.size();
             ++BoneIndex)
        {
            const UEFbxImportExactBone& ExactBone =
                ExactSourceClip.Bones[BoneIndex];
            const retarget::UEIKJsonBone& RouteBone =
                Route.SourceRig.Bones[BoneIndex];
            if (ExactBone.Index != RouteBone.Index ||
                ExactBone.ParentIndex !=
                    RouteBone.ParentIndex ||
                ExactBone.Name != RouteBone.Name)
            {
                return Fail(
                    "animation golden hierarchy does not match the source IK Rig at bone " +
                    RouteBone.Name);
            }
        }
    }

    RestFbxCoordinateEvidence SourceRestCoordinateEvidence;
    RestFbxCoordinateEvidence TargetRestCoordinateEvidence;
    if (UseUE58ExportedRestImport &&
        (!ValidateUE58ExportedRestFbx(
             Options.SourceRestFbxPath,
             Route.SourceRig.Bones, Options,
             "source rest FBX",
             SourceRestCoordinateEvidence,
             SourceRestScene, Error) ||
         !ValidateUE58ExportedRestFbx(
             Options.TargetRestFbxPath,
             Route.TargetRig.Bones, Options,
             "target rest FBX",
             TargetRestCoordinateEvidence,
             TargetRestScene, Error)))
    {
        return Fail(Error);
    }

    const bool ExactSourceRequiresDirectBindAudit =
        UseExactSourceImport &&
        (ExactSourceClip.HasMeshPayload ||
         ExactSourceClip.HasBindPosePayload);
    if ((!UseUE58ExportedRestImport && !LoadFbx(
            Options.SourceRestFbxPath,
            "SKRTG_UEIK_source_rest",
            SourceRestScene, Error, true)) ||
        ((!UseExactSourceImport ||
          ExactSourceRequiresDirectBindAudit) &&
         !LoadFbx(
             Options.SourceAnimationFbxPath,
             "SKRTG_UEIK_source_animation",
             SourceAnimationScene, Error, true)) ||
        (!UseUE58ExportedRestImport && !LoadFbx(
            Options.TargetRestFbxPath,
            "SKRTG_UEIK_target_rest",
            TargetRestScene, Error, true)))
    {
        return Fail(Error);
    }

    BoundSkeleton SourceRest;
    BoundSkeleton SourceAnimation;
    BoundSkeleton TargetRest;
    if (!BindSkeleton(
            SourceRestScene.Scene, Route.SourceRig.Bones,
            SourceRest, Error) ||
        ((!UseExactSourceImport ||
          ExactSourceRequiresDirectBindAudit) &&
         !BindSkeleton(
             SourceAnimationScene.Scene, Route.SourceRig.Bones,
             SourceAnimation, Error)) ||
        !BindSkeleton(
            TargetRestScene.Scene, Route.TargetRig.Bones,
            TargetRest, Error))
    {
        return Fail(Error);
    }
    RestValidation SourceRestValidation;
    RestValidation SourceAnimationValidation;
    RestValidation TargetRestValidation;
    std::vector<reconciliation::RetargetLocalDelta>
        SourceReferenceDeltas;
    std::vector<reconciliation::RetargetLocalDelta>
        TargetReferenceDeltas;
    if (!BuildReferenceReconciliation(
            Route.SourceRig.Bones, SourceRest, Options,
            "source rest FBX", SourceReferenceDeltas,
            SourceRestValidation, Error) ||
        !BuildReferenceReconciliation(
            Route.TargetRig.Bones, TargetRest, Options,
            "target rest FBX", TargetReferenceDeltas,
            TargetRestValidation, Error))
    {
        return Fail(Error);
    }
    if (!UseExactSourceImport &&
        !ValidateReferencePoseWithReconciliation(
            Route.SourceRig.Bones, SourceAnimation,
            SourceReferenceDeltas, Options,
            "source animation FBX", SourceAnimationValidation,
            Error))
    {
        return Fail(Error);
    }

    FbxAnimStack* Stack = nullptr;
    FbxTimeSpan Span;
    std::string SelectedStackName;
    double DurationSeconds = 0.0;
    int LastFrame = 0;
    if (UseExactSourceImport)
    {
        SelectedStackName =
            ExactSourceClip.AnimationStackName;
        DurationSeconds =
            ExactSourceClip.DurationSeconds;
        LastFrame =
            static_cast<int>(
                ExactSourceClip.Frames.size()) - 1;
    }
    else
    {
        if (!SelectAnimationStack(
                SourceAnimationScene.Scene,
                Options.AnimationStackName,
                Stack, Span, Error))
        {
            return Fail(Error);
        }
        SelectedStackName =
            Stack->GetName() != nullptr
            ? Stack->GetName()
            : "";
        DurationSeconds =
            Span.GetDuration().GetSecondDouble();
        LastFrame = static_cast<int>(
            std::ceil(
                DurationSeconds *
                    Options.SampleRate -
                1.0e-9));
    }
    if (LastFrame < 1 ||
        LastFrame > 60 * 60 * static_cast<int>(Options.SampleRate))
    {
        return Fail(
            "selected animation duration is empty or exceeds the one-hour safety limit");
    }

    std::vector<TransformRT> SourceMotionReferenceModel(
        Route.SourceRig.Bones.size());
    Quat SourceFbxToUEModelRotation = core::math::IdentityQuat();
    double SourceFbxToUEBasisMaximumResidualCm = 0.0;
    Quat SourceRestEvidenceModelRotation =
        core::math::IdentityQuat();
    double SourceRestEvidenceBasisMaximumResidualCm = 0.0;
    AnimationBindModel SourceAnimationBind;
    if (UseExactSourceImport)
    {
        for (std::size_t BoneIndex = 0;
             BoneIndex <
                SourceMotionReferenceModel.size();
             ++BoneIndex)
        {
            SourceMotionReferenceModel[BoneIndex] =
                Route.SourceRig.Bones[BoneIndex]
                    .ReferenceModel;
            if (ScaleError(
                    SourceMotionReferenceModel[BoneIndex]
                        .Scale,
                    {1.0, 1.0, 1.0}) >
                Options.RestScaleTolerance)
            {
                return Fail(
                    "exact UE source reference is outside the unit-scale route at bone " +
                    Route.SourceRig.Bones[BoneIndex]
                        .Name);
            }
        }
        std::vector<TransformRT> ImportedRestModel(
            Route.SourceRig.Bones.size());
        FbxTime SourceReferenceTime;
        SourceReferenceTime.SetSecondDouble(0.0);
        for (std::size_t BoneIndex = 0;
             BoneIndex < ImportedRestModel.size();
             ++BoneIndex)
        {
            ImportedRestModel[BoneIndex] =
                ToTransformRT(
                    SourceRest.Nodes[BoneIndex]
                        ->EvaluateGlobalTransform(
                            SourceReferenceTime,
                            FbxNode::eSourcePivot,
                            false,
                            true));
        }
        AnimationBindModel ImportedBindEvidence;
        if (!BuildSourceFbxToUEModelRotation(
                Route,
                ImportedRestModel,
                SourceRestEvidenceModelRotation,
                SourceRestEvidenceBasisMaximumResidualCm,
                Error))
        {
            return Fail(Error);
        }
        if (ExactSourceRequiresDirectBindAudit)
        {
            if (!BuildAnimationBindModel(
                    SourceAnimationScene.Scene,
                    Route.SourceRig.Bones,
                    SourceAnimation,
                    Options.RestScaleTolerance,
                    ImportedBindEvidence,
                    Error) ||
                !ValidateAnimationBindAgainstUEReference(
                    Route.SourceRig.Bones,
                    ImportedBindEvidence,
                    Route.SourceRootIndex,
                    SourceRestEvidenceModelRotation,
                    ImportedBindEvidence,
                    Error))
            {
                return Fail(Error);
            }
        }
        else if (!BuildGoldenReferenceBindEvidence(
                     Route.SourceRig.Bones,
                     ExactSourceClip.Bones,
                     Options,
                     ImportedBindEvidence,
                     Error))
        {
            return Fail(Error);
        }
        // Full-key golden frames and the UE IK Rig reference are already in
        // UE model space.  Keep the applied transform explicitly identity;
        // the normalized FBX body-basis result is evidence only.
        SourceFbxToUEModelRotation =
            core::math::IdentityQuat();
        SourceFbxToUEBasisMaximumResidualCm = 0.0;
        SourceAnimationBind = ImportedBindEvidence;
        SourceAnimationBind.Model.resize(
            Route.SourceRig.Bones.size());
        SourceAnimationBind.Sources.assign(
            Route.SourceRig.Bones.size(),
            ImportedBindEvidence.UsedGoldenReferenceFallback
                ? "source_ik_rig_reference_model_after_hash_bound_golden_reference_audit"
                : "source_ik_rig_reference_model_after_imported_bind_identity_audit");
        for (std::size_t BoneIndex = 0;
             BoneIndex <
                SourceAnimationBind.Model.size();
             ++BoneIndex)
        {
            SourceAnimationBind.Model[BoneIndex] =
                Route.SourceRig.Bones[BoneIndex]
                    .ReferenceModel;
        }
    }
    else
    {
        FbxTime SourceReferenceTime;
        SourceReferenceTime.SetSecondDouble(0.0);
        for (std::size_t BoneIndex = 0;
             BoneIndex <
                SourceMotionReferenceModel.size();
             ++BoneIndex)
        {
            SourceMotionReferenceModel[BoneIndex] =
                ToTransformRT(
                    SourceRest.Nodes[BoneIndex]
                        ->EvaluateGlobalTransform(
                            SourceReferenceTime,
                            FbxNode::eSourcePivot,
                            false,
                            true));
            if (ScaleError(
                    SourceMotionReferenceModel[BoneIndex]
                        .Scale,
                    {1.0, 1.0, 1.0}) >
                Options.RestScaleTolerance)
            {
                return Fail(
                    "source rest FBX model reference is outside the unit-scale route at bone " +
                    Route.SourceRig.Bones[BoneIndex]
                        .Name);
            }
        }
        if (!BuildSourceFbxToUEModelRotation(
                Route,
                SourceMotionReferenceModel,
                SourceFbxToUEModelRotation,
                SourceFbxToUEBasisMaximumResidualCm,
                Error) ||
            !BuildAnimationBindModel(
                SourceAnimationScene.Scene,
                Route.SourceRig.Bones,
                SourceAnimation,
                Options.RestScaleTolerance,
                SourceAnimationBind,
                Error) ||
            !ValidateAnimationBindAgainstUEReference(
                Route.SourceRig.Bones,
                SourceAnimationBind,
                Route.SourceRootIndex,
                SourceFbxToUEModelRotation,
                SourceAnimationBind,
                Error))
        {
            return Fail(Error);
        }
        SourceRestEvidenceModelRotation =
            SourceFbxToUEModelRotation;
        SourceRestEvidenceBasisMaximumResidualCm =
            SourceFbxToUEBasisMaximumResidualCm;
    }

    std::vector<OwnedFrame> Frames;
    Frames.reserve(static_cast<std::size_t>(LastFrame + 1));
    std::vector<TransformRT> FirstRawSourceModelPose;
    std::vector<TransformRT> FirstSourceRetargetModelPose;
    double MaximumEndpointErrorCm = 0.0;
    int AppliedIkChainRecords = 0;
    for (int FrameIndex = 0;
         FrameIndex <= LastFrame; ++FrameIndex)
    {
        const double RelativeSeconds = std::min(
            DurationSeconds,
            static_cast<double>(FrameIndex) / Options.SampleRate);
        std::vector<TransformRT> SourceLocal;
        std::vector<TransformRT> SourceRetargetModel;
        std::vector<TransformRT> RawSourceModel;
        if (UseExactSourceImport)
        {
            if (static_cast<std::size_t>(FrameIndex) >=
                    ExactSourceClip.Frames.size())
            {
                return Fail(
                    "ue_fbx_import_exact_v1 frame inventory changed during solve");
            }
            const UEFbxImportExactFrame& ExactFrame =
                ExactSourceClip.Frames[
                    static_cast<std::size_t>(
                        FrameIndex)];
            if (ExactFrame.FrameIndex != FrameIndex ||
                std::abs(
                    ExactFrame.TimeSeconds -
                    RelativeSeconds) > 1.0e-9 ||
                ExactFrame.LocalPose.size() !=
                    Route.SourceRig.Bones.size() ||
                ExactFrame.ModelPose.size() !=
                    Route.SourceRig.Bones.size())
            {
                return Fail(
                    "ue_fbx_import_exact_v1 frame identity does not match the solver request");
            }
            RawSourceModel = ExactFrame.ModelPose;
            if (!AlignSourceAnimationModelPose(
                    Route,
                    SourceAnimationBind,
                    SourceFbxToUEModelRotation,
                    RawSourceModel,
                    Options.RestScaleTolerance,
                    SourceLocal,
                    SourceRetargetModel,
                    Error))
            {
                return Fail(
                    "frame " +
                    std::to_string(FrameIndex) +
                    " ue_fbx_import_exact_v1 bind/reference alignment failed: " +
                    Error);
            }
        }
        else
        {
            FbxTime Time;
            Time.SetSecondDouble(
                Span.GetStart().GetSecondDouble() +
                RelativeSeconds);
            if (!BuildReconciledSourceAnimationPose(
                    Route, SourceAnimation,
                    SourceAnimationBind,
                    SourceFbxToUEModelRotation, Time,
                    Options.RestScaleTolerance,
                    SourceLocal, SourceRetargetModel,
                    &RawSourceModel, Error))
            {
                return Fail(
                    "frame " +
                    std::to_string(FrameIndex) +
                    " source animation reconciliation failed: " +
                    Error);
            }
        }
        retarget::UEIKJsonSolveResult Solved =
            retarget::SolveUEIKJsonRouteFrame(Route, SourceLocal);
        if (!Solved.Success)
        {
            const std::string Message = Solved.Errors.empty()
                ? "route solve failed"
                : Solved.Errors.front();
            return Fail(
                "frame " + std::to_string(FrameIndex) +
                " failed: " + Message);
        }
        OwnedFrame Frame;
        Frame.FrameIndex = FrameIndex;
        Frame.TimeSeconds = RelativeSeconds;
        if (FrameIndex == 0)
        {
            FirstRawSourceModelPose = RawSourceModel;
            FirstSourceRetargetModelPose = SourceRetargetModel;
        }
        // The Original lane is an audit view of the source FBX in its own
        // evaluated model space.  The reconciled UE Retarget Pose is used
        // only as solver input and must not be paired with FBX bind offsets.
        Frame.SourceModelPose = std::move(RawSourceModel);
        Frame.SourceRetargetModelPose =
            std::move(SourceRetargetModel);
        Frame.TargetFkModelPose =
            std::move(Solved.TargetFkModelPose);
        Frame.TargetFoundationLocalPose =
            std::move(Solved.TargetFoundationLocalPose);
        Frame.TargetFoundationModelPose =
            std::move(Solved.TargetFoundationModelPose);
        Frame.TargetFinalLocalPose =
            Frame.TargetFoundationLocalPose;
        Frame.TargetFinalModelPose =
            Frame.TargetFoundationModelPose;
        MaximumEndpointErrorCm = std::max(
            MaximumEndpointErrorCm,
            Solved.MaximumIkEndpointErrorCm);
        AppliedIkChainRecords += Solved.AppliedIkChainCount;
        Frames.push_back(std::move(Frame));
    }

    bool OperationStackExecuted = false;
    bool OperationStackChangedFinal = false;
    std::string OperationStackRepeatabilityMode = "not_configured";
    Json OperationStackStages = Json::array();
    if (HasOperationStack)
    {
        const core::skeleton::NormalizedRuntimeSkeleton SourceRuntime =
            retarget::BuildSourceRuntimeSkeleton(Route);
        const core::skeleton::NormalizedRuntimeSkeleton TargetRuntime =
            retarget::BuildTargetRuntimeSkeleton(Route);
        retarget::RetargetOpClip OpClip;
        OpClip.Frames.reserve(Frames.size());
        for (const OwnedFrame& Frame : Frames)
        {
            if (Frame.SourceRetargetModelPose.size() !=
                    SourceRuntime.BoneCount())
            {
                return Fail(
                    "Operation System source pose does not match the exact source runtime skeleton");
            }
            retarget::RetargetOpFrame OpFrame;
            OpFrame.FrameIndex = Frame.FrameIndex;
            OpFrame.TimeSeconds = Frame.TimeSeconds;
            OpFrame.SourceModelPose = PoseBuffer(
                PoseSpace::Model,
                SourceRuntime.GetIdentity().HierarchyHash);
            OpFrame.SourceModelPose.ResizeToSkeleton(SourceRuntime);
            for (std::size_t BoneIndex = 0;
                 BoneIndex < SourceRuntime.BoneCount(); ++BoneIndex)
            {
                OpFrame.SourceModelPose[BoneIndex] =
                    Frame.SourceRetargetModelPose[BoneIndex];
            }
            OpFrame.TargetLocalPose =
                Frame.TargetFoundationLocalPose;
            OpFrame.TargetModelPose =
                Frame.TargetFoundationModelPose;
            OpClip.Frames.push_back(std::move(OpFrame));
        }
        retarget::RetargetOpProgramLoadResult ProgramLoad =
            retarget::LoadRetargetOpProgram(
                Options.OperationStackJsonPath,
                SourceRuntime, TargetRuntime);
        if (!ProgramLoad.Success || !ProgramLoad.Program)
        {
            return Fail(
                ProgramLoad.Errors.empty()
                ? "Operation System v2 program load failed"
                : ProgramLoad.Errors.front());
        }
        OperationStackRepeatabilityMode = retarget::ToString(
            ProgramLoad.Program->RunOptions.RepeatabilityMode);
        if (!retarget::SeedRetargetOpGoals(
                TargetRuntime, ProgramLoad.Program->GoalSeeds,
                OpClip, Error))
        {
            return Fail("Operation System v2 goal seeding failed: " + Error);
        }
        const retarget::RetargetOpStackRunResult OpRun =
            ProgramLoad.Program->Stack.Run(
                SourceRuntime, TargetRuntime, OpClip,
                ProgramLoad.Program->RunOptions);
        if (!OpRun.Success)
        {
            return Fail(
                OpRun.Errors.empty()
                ? "Operation System v2 failed bounded execution"
                : OpRun.Errors.front());
        }
        OperationStackExecuted = std::any_of(
            OpRun.Stages.begin(), OpRun.Stages.end(),
            [](const retarget::RetargetOpStackStageResult& Stage)
            {
                return Stage.Executed;
            });
        OperationStackChangedFinal =
            !retarget::EquivalentRetargetOpClips(
                OpClip, OpRun.FinalOutput);
        for (const retarget::RetargetOpStackStageResult& Stage :
             OpRun.Stages)
        {
            OperationStackStages.push_back({
                {"instanceId", Stage.InstanceId},
                {"type", Stage.TypeId},
                {"version", Stage.Version},
                {"phase", retarget::ToString(Stage.Phase)},
                {"enabled", Stage.Enabled},
                {"executed", Stage.Executed},
                {"preflightPassed", Stage.PreflightPassed},
                {"success", Stage.Success},
                {"disabledExactPassthrough",
                 Stage.DisabledExactPassthrough},
                {"mutationWithinDeclaredChannels",
                 Stage.MutationWithinDeclaredChannels},
                {"outputModelsRebuilt", Stage.OutputModelsRebuilt},
                {"repeatabilityCheckPerformed",
                 Stage.RepeatabilityCheckPerformed},
                {"deterministicRepeatabilityVerified",
                 Stage.DeterministicRepeatabilityVerified},
                {"warnings", Stage.Warnings},
                {"errors", Stage.Errors}});
        }
        if (OpRun.FinalOutput.Frames.size() != Frames.size())
            return Fail("Operation System v2 changed the frame inventory");
        for (std::size_t FrameIndex = 0;
             FrameIndex < Frames.size(); ++FrameIndex)
        {
            Frames[FrameIndex].TargetFinalLocalPose =
                OpRun.FinalOutput.Frames[FrameIndex].TargetLocalPose;
            Frames[FrameIndex].TargetFinalModelPose =
                OpRun.FinalOutput.Frames[FrameIndex].TargetModelPose;
        }
    }

    std::vector<RetargetReviewChain> ReviewChains;
    std::set<int> SourceIkIndices;
    std::set<int> TargetIkIndices;
    if (!BuildReviewChains(
            Route, ReviewChains,
            SourceIkIndices, TargetIkIndices, Error))
    {
        return Fail(Error);
    }
    const bool SourceAnimationHasMesh =
        UseExactSourceImport
        ? ExactSourceClip.HasMeshPayload
        : SceneContainsMesh(
              SourceAnimationScene.Scene->GetRootNode());
    const BoundSkeleton& SourceDisplayPaths =
        SourceAnimationHasMesh ? SourceAnimation : SourceRest;

    RetargetReviewPackageOptions Review;
    Review.ContractKind = "ue_ik_json_v1";
    Review.SourceAnimationFbxPath =
        Options.SourceAnimationFbxPath;
    Review.SourceAnimationExpectedSha256 =
        Inputs[5].Sha256;
    Review.SourceMeshFallbackFbxPath =
        Options.SourceRestFbxPath;
    Review.SourceMeshFallbackExpectedSha256 =
        Inputs[4].Sha256;
    Review.TargetTposeFbxPath = Options.TargetRestFbxPath;
    Review.TargetTposeExpectedSha256 = Inputs[6].Sha256;
    Review.OutputDirectory = Options.OutputDirectory / "Review";
    Review.RouteId = Route.RouteId;
    Review.FoundationRouteId = Route.FoundationRouteId;
    Review.FoundationFrozen = false;
    Review.SourceMotionFootLockCandidateEnabled = false;
    Review.SourceMotionFootLockCandidateSelected = false;
    Review.SourceMotionFootLockCandidateAdopted = false;
    Review.OperationStackCandidateEnabled = OperationStackExecuted;
    Review.OperationStackCandidateSelected = false;
    Review.OperationStackCandidateAdopted = false;
    Review.NormalizeFbxToUEJsonSpace = true;
    Review.AllowSharedSourceMeshFallbackForMeshlessClips = true;
    Review.SourceBones = BuildReviewBones(
        Route.SourceRig.Bones,
        SourceDisplayPaths, SourceIkIndices,
        &SourceMotionReferenceModel);
    Review.TargetBones = BuildReviewBones(
        Route.TargetRig.Bones, TargetRest, TargetIkIndices);
    Review.RetargetChains = std::move(ReviewChains);
    Review.RootPelvisContract.SourceRootBoneIndex =
        Route.SourceRootIndex;
    Review.RootPelvisContract.SourcePelvisBoneIndex =
        Route.SourcePelvisIndex;
    Review.RootPelvisContract.TargetHipsBoneIndex =
        Route.TargetPelvisIndex;
    Review.RootPelvisContract.RootOwnership =
        "UE IK JSON route: source retarget root motion owns target root translation";
    Review.RootPelvisContract.PelvisOwnership =
        "UE IK JSON route: source retarget pelvis model delta owns target pelvis rotation";
    Review.RootPelvisContract.ScaleOwnership =
        "UE IK JSON route: explicit leg-chain rest-length ratio owns root motion scale";

    RetargetReviewAnchor PelvisAnchor;
    if (!BuildAnchor(
            "pelvis", Route.SourcePelvisIndex,
            Route.TargetPelvisIndex, Route,
            SourceDisplayPaths, TargetRest,
            SourceMotionReferenceModel,
            SourceFbxToUEModelRotation,
            PelvisAnchor, Error))
    {
        return Fail(Error);
    }
    Review.Anchors.push_back(std::move(PelvisAnchor));
    int SourceHead = -1;
    int TargetHead = -1;
    if (!ResolveHeadAnchorIndices(
            Route, SourceHead, TargetHead, Error))
    {
        return Fail(Error);
    }
    RetargetReviewAnchor HeadAnchor;
    if (!BuildAnchor(
            "head", SourceHead, TargetHead, Route,
            SourceDisplayPaths, TargetRest,
            SourceMotionReferenceModel,
            SourceFbxToUEModelRotation,
            HeadAnchor, Error))
    {
        return Fail(Error);
    }
    Review.Anchors.push_back(std::move(HeadAnchor));

    RetargetReviewClipView Clip;
    Clip.Id = Options.ClipId;
    Clip.Label = Options.ClipLabel;
    Clip.FramesPerSecond = Options.SampleRate;
    Clip.SourceAnimationFbxPath =
        Options.SourceAnimationFbxPath;
    Clip.SourceAnimationSha256 = Inputs[5].Sha256;
    Clip.SourceDirectBoneCount =
        static_cast<int>(Route.SourceRig.Bones.size());
    Clip.SourceRestPassthroughBoneCount = 0;
    Clip.LimbIkUnitScaleShadowProjectionApplied = false;
    Clip.LimbIkFamilyTransactions = 0;
    Clip.LimbIkCommittedFamilyTransactions = 0;
    Clip.LimbIkRolledBackFamilyTransactions = 0;
    Clip.LimbIkAppliedChainRecords = AppliedIkChainRecords;
    Clip.LimbIkFailClosedChainRecords = 0;
    Clip.LimbIkMaximumEndpointErrorCm =
        MaximumEndpointErrorCm;
    Clip.LimbIkMaximumShadowToRealPositionDeltaCm = 0.0;
    Clip.OperationStackEnabled = OperationStackExecuted;
    Clip.SourceMotionFootLockEnabled = false;
    Clip.FoundationExportFbxFileName =
        Options.FoundationExportFbxFileName;
    Clip.ExportFbxFileName = Options.ExportFbxFileName;
    Clip.Frames.reserve(Frames.size());
    for (const OwnedFrame& Frame : Frames)
    {
        RetargetReviewFrameView View;
        View.FrameIndex = Frame.FrameIndex;
        View.TimeSeconds = Frame.TimeSeconds;
        View.SourceModelPose = &Frame.SourceModelPose;
        View.TargetFkModelPose = &Frame.TargetFkModelPose;
        View.TargetFoundationLocalPose =
            &Frame.TargetFoundationLocalPose;
        View.TargetFoundationModelPose =
            &Frame.TargetFoundationModelPose;
        View.TargetFinalLocalPose = &Frame.TargetFinalLocalPose;
        View.TargetFinalModelPose = &Frame.TargetFinalModelPose;
        Clip.Frames.push_back(View);
    }
    Review.Clips.push_back(std::move(Clip));

    CleanupScenes();
    RetargetReviewPackageResult Package =
        GenerateRetargetReviewPackage(Review);
    if (!Package.Success)
    {
        const std::string Message = Package.Errors.empty()
            ? "review package generation failed"
            : Package.Errors.front();
        return Fail(Message);
    }

    for (const BoundInput& Input : Inputs)
    {
        if (!VerifyBoundInputUnchanged(Input, Error))
        {
            std::error_code RemoveError;
            std::filesystem::remove(
                Package.ExportedFoundationFbxPath, RemoveError);
            RemoveError.clear();
            std::filesystem::remove(
                Package.ExportedFbxPath, RemoveError);
            return Fail(Error);
        }
    }

    Json Provenance;
    Provenance["schema"] = "skrtg.ue_ik_json_retarget_provenance.v1";
    Provenance["routeId"] = Route.RouteId;
    Provenance["foundationRouteId"] = Route.FoundationRouteId;
    Provenance["selected"] = false;
    Provenance["adopted"] = false;
    Provenance["canonicalRig"] = Route.CanonicalRigObjectPath;
    Provenance["sourceRetargetPose"] = Route.SourcePoseName;
    Provenance["targetRetargetPose"] = Route.TargetPoseName;
    Provenance["globalTranslationScale"] =
        Route.GlobalTranslationScale;
    Provenance["sampleRate"] = Options.SampleRate;
    Provenance["frameCount"] = Frames.size();
    Provenance["animationStack"] = SelectedStackName;
    Provenance["mappedChainCount"] = Route.ChainPairs.size();
    Provenance["appliedIkChainRecords"] =
        AppliedIkChainRecords;
    Provenance["importReuse"] = {
        {"sourceRestSceneReusedAfterCoordinateValidation",
         UseUE58ExportedRestImport},
        {"targetRestSceneReusedAfterCoordinateValidation",
         UseUE58ExportedRestImport},
        {"sourceAnimationSecondImportSkipped",
         UseExactSourceImport &&
             !ExactSourceRequiresDirectBindAudit},
        {"sourceAnimationHasMeshPayload",
         UseExactSourceImport && ExactSourceClip.HasMeshPayload},
        {"sourceAnimationHasBindPosePayload",
         UseExactSourceImport && ExactSourceClip.HasBindPosePayload},
        {"workerProcessSharedAcrossBatch", false}};
    Provenance["operationStack"] = {
        {"schema", "skrtg.op_stack.v2"},
        {"configured", HasOperationStack},
        {"candidate", HasOperationStack},
        {"selected", false},
        {"adopted", false},
        {"executed", OperationStackExecuted},
        {"changedFinal", OperationStackChangedFinal},
        {"repeatabilityMode", OperationStackRepeatabilityMode},
        {"configPath", HasOperationStack
            ? Options.OperationStackJsonPath.string()
            : std::string()},
        {"configSha256", HasOperationStack
            ? Inputs.back().Sha256
            : std::string()},
        {"stages", OperationStackStages}};
    Provenance["solverParity"] = {
        {"ueFKScheduling",
         "candidate_implementation_of_ue_5_8_fk_chains_op_interpolated_translation_none"},
        {"uePelvisMotion",
         "candidate_implementation_of_ue_5_8_pelvis_motion_op"},
        {"ueRunIKRigSolver",
         "analytic_two_bone_approximation"},
        {"fullBodyIKEquivalent", false}};
    Provenance["maximumIkEndpointErrorCm"] =
        MaximumEndpointErrorCm;
    Provenance["sourceRestValidation"] =
        RestValidationJson(SourceRestValidation);
    Provenance["sourceAnimationValidation"] =
        RestValidationJson(SourceAnimationValidation);
    Provenance["sourceAnimationValidationMode"] =
        UseExactSourceImport
        ? "full_key_ue_animation_golden"
        : "rest_reconciliation_v7";
    Provenance["targetRestValidation"] =
        RestValidationJson(TargetRestValidation);
    Provenance["restFbxImport"] = {
        {"mode",
         UseUE58ExportedRestImport
         ? "ue5.8_exported_y_reflection_v1"
         : "reconciled_rest_v1"},
        {"enabled", UseUE58ExportedRestImport},
        {"selected", false},
        {"adopted", false},
        {"coordinateRule",
         UseUE58ExportedRestImport
         ? "p_ue=(x,-y,z); q_ue=(-qx,qy,-qz,qw); "
           "scale_passthrough"
         : "hash_bound_reference_reconciliation"}};
    if (UseUE58ExportedRestImport)
    {
        Provenance["restFbxImport"]["sourceEvidence"] =
            RestFbxCoordinateEvidenceJson(
                SourceRestCoordinateEvidence);
        Provenance["restFbxImport"]["targetEvidence"] =
            RestFbxCoordinateEvidenceJson(
                TargetRestCoordinateEvidence);
    }
    Provenance["sourceAnimationApplication"] =
        UseExactSourceImport
        ? UEFbxImportExactVersion
        : "hash_bound_fbx_skin_bind_model_motion_delta_to_ue_reference_v7";
    if (UseExactSourceImport)
    {
        Provenance["sourceAnimationExactImport"] = {
            {"selected", ExactSourceClip.Selected},
            {"adopted", ExactSourceClip.Adopted},
            {"goldenJson",
             Options.SourceAnimationGoldenJsonPath.string()},
            {"goldenJsonSha256",
             ExactSourceClip.AnimationGoldenJsonSha256},
            {"settingsSource",
             ExactSourceClip.Evidence.ImportSettingsSource},
            {"originalAxis",
             ExactSourceClip.Evidence.OriginalAxis},
            {"convertedAxis",
             ExactSourceClip.Evidence.ConvertedAxis},
            {"axisConversionApplied",
             ExactSourceClip.Evidence.AxisConversionApplied},
            {"unitConversionApplied",
             ExactSourceClip.Evidence.UnitConversionApplied},
            {"bBakeMeshes",
             ExactSourceClip.Evidence.BakeMeshes},
            {"fullKeyGoldenValidation",
             {{"maximumLocalTranslationCm",
               ExactSourceClip.GoldenValidation
                   .LocalTranslationCm.Value},
              {"maximumLocalRotationDegrees",
               ExactSourceClip.GoldenValidation
                   .LocalRotationDegrees.Value},
              {"maximumModelTranslationCm",
               ExactSourceClip.GoldenValidation
                   .ModelTranslationCm.Value},
              {"maximumModelRotationDegrees",
               ExactSourceClip.GoldenValidation
                   .ModelRotationDegrees.Value}}},
            {"bindReferenceEvidence",
             {{"mode",
               SourceAnimationBind.UsedGoldenReferenceFallback
                   ? "hash_bound_ue_golden_reference_for_animation_only_fbx"
                   : "fbx_skin_cluster_or_bind_pose"},
              {"skinClusterBoneCount",
               SourceAnimationBind.SkinClusterBoneCount},
              {"bindPoseBoneCount",
               SourceAnimationBind.BindPoseBoneCount},
              {"derivedLeafBoneCount",
               SourceAnimationBind.DerivedLeafBoneCount},
              {"goldenReferenceBoneCount",
               SourceAnimationBind.GoldenReferenceBoneCount},
              {"maximumUEReferencePositionResidualCm",
               SourceAnimationBind
                   .MaximumUEReferencePositionResidualCm},
              {"maximumGoldenLocalTranslationCm",
               SourceAnimationBind
                   .MaximumGoldenLocalTranslationCm},
              {"maximumGoldenLocalRotationDegrees",
               SourceAnimationBind
                   .MaximumGoldenLocalRotationDegrees},
              {"maximumGoldenLocalScale",
               SourceAnimationBind.MaximumGoldenLocalScale},
              {"maximumGoldenModelTranslationCm",
               SourceAnimationBind
                   .MaximumGoldenModelTranslationCm},
              {"maximumGoldenModelRotationDegrees",
               SourceAnimationBind
                   .MaximumGoldenModelRotationDegrees},
              {"maximumGoldenModelScale",
               SourceAnimationBind.MaximumGoldenModelScale}}}};
    }
    else
    {
        Provenance["sourceAnimationBindModel"] = {
            {"skinClusterBoneCount",
             SourceAnimationBind.SkinClusterBoneCount},
            {"bindPoseBoneCount",
             SourceAnimationBind.BindPoseBoneCount},
            {"derivedLeafBoneCount",
             SourceAnimationBind.DerivedLeafBoneCount},
            {"maximumDuplicateTranslationCm",
             SourceAnimationBind
                 .MaximumDuplicateTranslationCm},
            {"maximumDuplicateRotationDegrees",
             SourceAnimationBind
                 .MaximumDuplicateRotationDegrees},
            {"maximumDuplicateScale",
             SourceAnimationBind.MaximumDuplicateScale},
            {"maximumUEReferencePositionResidualCm",
             SourceAnimationBind
                 .MaximumUEReferencePositionResidualCm}};
    }
    Provenance["sourceRetargetPoseApplication"] =
        "ue_runtime_baseline_only_not_preapplied_to_source_animation";
    Provenance["sourceFbxToUEModelRotation"] = {
        SourceFbxToUEModelRotation.X,
        SourceFbxToUEModelRotation.Y,
        SourceFbxToUEModelRotation.Z,
        SourceFbxToUEModelRotation.W};
    Provenance["sourceFbxToUEBasisMaximumResidualCm"] =
        SourceFbxToUEBasisMaximumResidualCm;
    Provenance["sourceRestEvidenceModelRotation"] = {
        SourceRestEvidenceModelRotation.X,
        SourceRestEvidenceModelRotation.Y,
        SourceRestEvidenceModelRotation.Z,
        SourceRestEvidenceModelRotation.W};
    Provenance["sourceRestEvidenceBasisMaximumResidualCm"] =
        SourceRestEvidenceBasisMaximumResidualCm;
    Provenance["sourceAnimationFrame0RawModelPose"] = Json::array();
    if (FirstRawSourceModelPose.size() ==
            Route.SourceRig.Bones.size() &&
        FirstSourceRetargetModelPose.size() ==
            Route.SourceRig.Bones.size())
    {
        const std::size_t Root =
            static_cast<std::size_t>(Route.SourceRootIndex);
        const Vec3 FbxRestRoot =
            SourceMotionReferenceModel[Root].TranslationCm;
        const Vec3 UEReferenceRoot =
            Route.SourceRig.Bones[Root]
                .ReferenceModel.TranslationCm;
        for (std::size_t Index = 0;
             Index < FirstRawSourceModelPose.size(); ++Index)
        {
            const Vec3 Reference =
                SourceMotionReferenceModel[Index]
                    .TranslationCm;
            const Vec3 Current =
                FirstRawSourceModelPose[Index].TranslationCm;
            const Vec3 AlignedRaw = Add(
                UEReferenceRoot,
                RotateVector(
                    SourceFbxToUEModelRotation,
                    Subtract(Current, FbxRestRoot)));
            const Vec3 RetargetCurrent =
                FirstSourceRetargetModelPose[Index]
                    .TranslationCm;
            Provenance["sourceAnimationFrame0RawModelPose"]
                .push_back({
                    {"name", Route.SourceRig.Bones[Index].Name},
                    {"referencePositionCm",
                     {Reference.X, Reference.Y, Reference.Z}},
                    {"currentPositionCm",
                     {Current.X, Current.Y, Current.Z}},
                    {"alignedRawPositionCm",
                     {AlignedRaw.X, AlignedRaw.Y,
                      AlignedRaw.Z}},
                    {"solverInputPositionCm",
                     {RetargetCurrent.X, RetargetCurrent.Y,
                      RetargetCurrent.Z}}});
        }
    }
    Provenance["inputs"] = Json::array();
    for (const BoundInput& Input : Inputs)
        Provenance["inputs"].push_back(BoundInputJson(Input));
    Provenance["outputs"] = {
        {"viewer", Package.ViewerPath.string()},
        {"foundationFbx",
         Package.ExportedFoundationFbxPath.string()},
        {"foundationFbxSha256",
         Package.ExportedFoundationFbxSha256},
        {"finalFbx", Package.ExportedFbxPath.string()},
        {"finalFbxSha256", Package.ExportedFbxSha256}};

    Result.Success = true;
    Result.ViewerPath = Package.ViewerPath;
    Result.ExportedFoundationFbxPath =
        Package.ExportedFoundationFbxPath;
    Result.ExportedFoundationFbxSha256 =
        Package.ExportedFoundationFbxSha256;
    Result.ExportedFbxPath = Package.ExportedFbxPath;
    Result.ExportedFbxSha256 = Package.ExportedFbxSha256;
    Result.Artifacts = std::move(Package.Artifacts);
    Result.Artifacts.push_back({
        Options.OutputDirectory /
            "UEIK_Route_Provenance.json",
        Provenance.dump(2) + "\n"});
    Result.Warnings = std::move(Package.Warnings);
    Result.Warnings.insert(
        Result.Warnings.end(),
        RouteLoad.Warnings.begin(), RouteLoad.Warnings.end());
    if (UseExactSourceImport)
    {
        Result.Warnings.insert(
            Result.Warnings.end(),
            ExactSourceClip.Warnings.begin(),
            ExactSourceClip.Warnings.end());
        if (SourceAnimationBind.UsedGoldenReferenceFallback)
        {
            Result.Warnings.push_back(
                "The source FBX is animation-only and contains no skin-cluster or bind-pose payload. Its hash-bound UE Golden reference skeleton matched the source IK Rig within strict rest tolerances before the result was committed.");
        }
    }
    Result.Warnings.push_back(
        "Source/target rest-pose differences, including T-pose to A-pose calibration, are sourced only from exported UE Retarget Poses; no automatic bone-name inference or uasset parsing is used.");
    Result.Warnings.push_back(
        "The exported IK Rigs declare Unreal FullBodyIK, while this unselected candidate currently uses independent analytic two-bone limb solves. FullBodyIK parity is not claimed.");
    Result.ConsoleSummary =
        "UE IK JSON retarget review generated: route=" +
        Route.RouteId + " source_pose=" + Route.SourcePoseName +
        " target_pose=" + Route.TargetPoseName +
        " frames=" + std::to_string(Frames.size()) +
        " mapped_chains=" +
        std::to_string(Route.ChainPairs.size()) +
        " ik_records=" +
        std::to_string(AppliedIkChainRecords) +
        " max_ik_error_cm=" +
        std::to_string(MaximumEndpointErrorCm);
    return Result;
}
} // namespace skrtg::fbx
