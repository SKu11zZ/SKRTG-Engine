#include "skrtg/fbx/retarget_review_package.h"

#include "skrtg/core/math/transform.h"

#include <fbxsdk.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skrtg::fbx
{
namespace
{
using core::math::Normalize;
using core::math::Quat;
using core::math::TransformRT;
using core::math::Vec3;

constexpr const char* ViewerFileName =
    "SKRTG_UEIK_Retarget_Review_Viewer.html";
constexpr const char* AnimationManifestFileName =
    "SKRTG_UEIK_Animation_Manifest.json";
constexpr const char* VerificationFileName =
    "SKRTG_UEIK_Mesh_And_FBX_Export_Verification.json";
constexpr double Pi = 3.1415926535897932384626433832795;

struct LoadedScene
{
    FbxManager* Manager = nullptr;
    FbxScene* Scene = nullptr;
};

struct FileIdentity
{
    std::filesystem::path Path;
    std::string ExpectedSha256;
    std::string PreSha256;
    std::string PostSha256;
    std::uintmax_t PreSize = 0;
    std::uintmax_t PostSize = 0;
    long long PreLastWriteTicks = 0;
    long long PostLastWriteTicks = 0;
};

enum class ReviewSkinMode
{
    Normalize,
    TotalOne
};

struct ReviewMeshCluster
{
    int BoneIndex = -1;
    std::array<float, 12> BindOffset{};
};

struct ReviewMesh
{
    std::string Name;
    std::string Path;
    ReviewSkinMode SkinMode = ReviewSkinMode::Normalize;
    std::array<float, 12> FallbackGlobal{};
    std::vector<float> ControlPoints;
    std::vector<std::uint32_t> TriangleIndices;
    std::vector<std::uint32_t> InfluenceOffsets;
    std::vector<std::uint32_t> InfluenceClusters;
    std::vector<float> InfluenceWeights;
    std::vector<ReviewMeshCluster> Clusters;
    int SkinDeformerCount = 0;
    int BlendShapeDeformerCount = 0;
    int MaterialSlotCount = 0;
    int MaximumInfluencesPerControlPoint = 0;
    double MaximumBindReconstructionErrorCm = 0.0;
};

struct ReviewMeshPackage
{
    std::string Label;
    std::vector<ReviewMesh> Meshes;
    int ControlPointCount = 0;
    int TriangleCount = 0;
    int SkinDeformerCount = 0;
    int SkinClusterCount = 0;
    int InfluenceCount = 0;
    int BlendShapeDeformerCount = 0;
    int MaterialSlotCount = 0;
    int MaximumInfluencesPerControlPoint = 0;
    double MaximumBindReconstructionErrorCm = 0.0;
};

struct MeshSceneFingerprint
{
    int MeshCount = 0;
    int ControlPointCount = 0;
    int PolygonCount = 0;
    int MaterialSlotCount = 0;
    int SkinDeformerCount = 0;
    int SkinClusterCount = 0;
    int SkinControlPointIndexCount = 0;
    int BindPoseCount = 0;
    std::string CanonicalText;
    std::string Sha256;
};

struct ExportVerification
{
    bool Success = false;
    std::string InputMeshFingerprint;
    std::string OutputMeshFingerprint;
    int ComparedSamples = 0;
    int LocalMismatchCount = 0;
    int ModelMismatchCount = 0;
    double MaximumLocalTranslationCm = 0.0;
    double MaximumLocalRotationDegrees = 0.0;
    double MaximumLocalScale = 0.0;
    double MaximumModelTranslationCm = 0.0;
    double MaximumModelRotationDegrees = 0.0;
    double MaximumModelScale = 0.0;
    std::vector<std::string> Errors;
};

enum class ReviewPoseLane
{
    Foundation,
    Final
};

const char* PoseLaneName(ReviewPoseLane Lane)
{
    return Lane == ReviewPoseLane::Foundation
        ? "foundation" : "final";
}

const core::animation::PoseBuffer* LocalPoseForLane(
    const RetargetReviewFrameView& Frame,
    ReviewPoseLane Lane)
{
    return Lane == ReviewPoseLane::Foundation
        ? Frame.TargetFoundationLocalPose
        : Frame.TargetFinalLocalPose;
}

const core::animation::PoseBuffer* ModelPoseForLane(
    const RetargetReviewFrameView& Frame,
    ReviewPoseLane Lane)
{
    return Lane == ReviewPoseLane::Foundation
        ? Frame.TargetFoundationModelPose
        : Frame.TargetFinalModelPose;
}

std::string UpperAscii(std::string Value)
{
    std::transform(
        Value.begin(), Value.end(), Value.begin(),
        [](unsigned char Character)
        {
            return static_cast<char>(std::toupper(Character));
        });
    return Value;
}

std::string JsonEscape(const std::string& Value)
{
    std::ostringstream Output;
    for (const unsigned char Character : Value)
    {
        switch (Character)
        {
        case '"': Output << "\\\""; break;
        case '\\': Output << "\\\\"; break;
        case '\n': Output << "\\n"; break;
        case '\r': Output << "\\r"; break;
        case '\t': Output << "\\t"; break;
        default:
            if (Character < 0x20)
            {
                Output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<int>(Character)
                       << std::dec;
            }
            else
            {
                Output << static_cast<char>(Character);
            }
            break;
        }
    }
    return Output.str();
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
        const std::size_t Remaining = Size - Offset;
        const ULONG Chunk = static_cast<ULONG>(
            std::min<std::size_t>(Remaining, 1024 * 1024));
        Status = BCryptHashData(
            HashHandle,
            const_cast<PUCHAR>(Data + Offset),
            Chunk, 0);
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
        Input.read(
            reinterpret_cast<char*>(Bytes.data()),
            static_cast<std::streamsize>(Bytes.size()));
    if (!Input && !Bytes.empty())
    {
        OutError = "failed while reading input for SHA256: " + Path.string();
        return false;
    }
    return ComputeSha256Bytes(
        Bytes.data(), Bytes.size(), OutHash, OutError);
}

bool CaptureIdentityBefore(
    const std::filesystem::path& Path,
    const std::string& ExpectedSha256,
    FileIdentity& Out,
    std::string& OutError)
{
    Out = {};
    Out.Path = std::filesystem::absolute(Path).lexically_normal();
    Out.ExpectedSha256 = UpperAscii(ExpectedSha256);
    std::error_code Error;
    Out.PreSize = std::filesystem::file_size(Out.Path, Error);
    if (Error)
    {
        OutError = "failed to read input size: " + Out.Path.string();
        return false;
    }
    Out.PreLastWriteTicks = std::filesystem::last_write_time(
        Out.Path, Error).time_since_epoch().count();
    if (Error ||
        !ComputeSha256(Out.Path, Out.PreSha256, OutError))
    {
        if (OutError.empty())
            OutError = "failed to capture input identity";
        return false;
    }
    if (!Out.ExpectedSha256.empty() &&
        UpperAscii(Out.PreSha256) != Out.ExpectedSha256)
    {
        OutError = "input SHA256 mismatch: " + Out.Path.string();
        return false;
    }
    return true;
}

bool CaptureIdentityAfter(
    FileIdentity& Identity,
    std::string& OutError)
{
    std::error_code Error;
    Identity.PostSize = std::filesystem::file_size(
        Identity.Path, Error);
    if (Error)
    {
        OutError = "failed to read post-use input size";
        return false;
    }
    Identity.PostLastWriteTicks = std::filesystem::last_write_time(
        Identity.Path, Error).time_since_epoch().count();
    if (Error ||
        !ComputeSha256(
            Identity.Path, Identity.PostSha256, OutError))
    {
        return false;
    }
    if (Identity.PreSize != Identity.PostSize ||
        Identity.PreLastWriteTicks != Identity.PostLastWriteTicks ||
        UpperAscii(Identity.PreSha256) !=
            UpperAscii(Identity.PostSha256))
    {
        OutError = "input changed during read: " +
            Identity.Path.string();
        return false;
    }
    return true;
}

bool LoadScene(
    const std::filesystem::path& Path,
    const char* SceneName,
    bool NormalizeToUEJsonSpace,
    LoadedScene& Out,
    std::string& OutError)
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
        Out.Manager->Destroy();
        Out = {};
        return false;
    }
    Out.Scene = FbxScene::Create(Out.Manager, SceneName);
    const bool Imported =
        Out.Scene != nullptr && Importer->Import(Out.Scene);
    if (!Imported)
    {
        OutError = std::string("FBX import failed: ") +
            Importer->GetStatus().GetErrorString();
    }
    Importer->Destroy();
    if (!Imported)
    {
        Out.Manager->Destroy();
        Out = {};
        return false;
    }
    if (NormalizeToUEJsonSpace)
    {
        FbxNode* RootNode = Out.Scene->GetRootNode();
        if (RootNode == nullptr)
        {
            OutError = "UE JSON FBX scene has no root node";
            Out.Manager->Destroy();
            Out = {};
            return false;
        }
        FbxGlobalSettings& SceneSettings =
            Out.Scene->GetGlobalSettings();
        const FbxAxisSystem Axis = SceneSettings.GetAxisSystem();
        int UpSign = 0;
        int FrontSign = 0;
        const FbxAxisSystem::EUpVector Up =
            Axis.GetUpVector(UpSign);
        const FbxAxisSystem::EFrontVector Front =
            Axis.GetFrontVector(FrontSign);
        const FbxSystemUnit Unit = SceneSettings.GetSystemUnit();
        const bool AxisMatchesUE58Export =
            Up == FbxAxisSystem::eZAxis &&
            UpSign == 1 &&
            Front == FbxAxisSystem::eParityOdd &&
            FrontSign == -1 &&
            Axis.GetCoorSystem() == FbxAxisSystem::eRightHanded;
        const bool UnitMatchesUE58Export =
            std::abs(Unit.GetScaleFactor() - 1.0) <= 1.0e-12 &&
            std::abs(Unit.GetMultiplier() - 1.0) <= 1.0e-12;
        if (!AxisMatchesUE58Export || !UnitMatchesUE58Export)
        {
            OutError =
                "UE JSON review FBX does not match the hash-bound UE 5.8 "
                "export axis/unit contract";
            Out.Manager->Destroy();
            Out = {};
            return false;
        }
        // Keep the UE-exported FBX scene, mesh bind matrices, pivots and
        // animation curves in their native right-handed FBX basis. The UE
        // JSON route crosses the handedness boundary explicitly at mesh
        // extraction, animation writeback and verification. DeepConvertScene
        // previously rotated the bind data while UE-local curves were written
        // unchanged, which produced a self-consistent TRS roundtrip but
        // invalid skin deformation.
    }
    return true;
}

void DestroyScene(LoadedScene& Scene)
{
    if (Scene.Manager != nullptr) Scene.Manager->Destroy();
    Scene = {};
}

bool Finite(const Vec3& Value)
{
    return std::isfinite(Value.X) &&
        std::isfinite(Value.Y) &&
        std::isfinite(Value.Z);
}

bool Finite(const Quat& Value)
{
    return std::isfinite(Value.X) &&
        std::isfinite(Value.Y) &&
        std::isfinite(Value.Z) &&
        std::isfinite(Value.W);
}

bool Finite(const TransformRT& Value)
{
    return Finite(Value.TranslationCm) &&
        Finite(Value.Rotation) &&
        Finite(Value.Scale);
}

bool Exact(double Left, double Right)
{
    return std::memcmp(&Left, &Right, sizeof(double)) == 0;
}

bool Exact(const TransformRT& Left, const TransformRT& Right)
{
    return Exact(Left.TranslationCm.X, Right.TranslationCm.X) &&
        Exact(Left.TranslationCm.Y, Right.TranslationCm.Y) &&
        Exact(Left.TranslationCm.Z, Right.TranslationCm.Z) &&
        Exact(Left.Rotation.X, Right.Rotation.X) &&
        Exact(Left.Rotation.Y, Right.Rotation.Y) &&
        Exact(Left.Rotation.Z, Right.Rotation.Z) &&
        Exact(Left.Rotation.W, Right.Rotation.W) &&
        Exact(Left.Scale.X, Right.Scale.X) &&
        Exact(Left.Scale.Y, Right.Scale.Y) &&
        Exact(Left.Scale.Z, Right.Scale.Z);
}

bool ExactPose(const core::animation::PoseBuffer& Left,
               const core::animation::PoseBuffer& Right)
{
    if (Left.Space() != Right.Space() ||
        Left.SkeletonHash() != Right.SkeletonHash() ||
        Left.Size() != Right.Size()) return false;
    for (std::size_t Index = 0; Index < Left.Size(); ++Index)
        if (!Exact(Left[Index], Right[Index])) return false;
    return true;
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

// UE JSON uses Unreal's left-handed +X forward, +Y right, +Z up model
// space. The UE 5.8 FBX export evidence used by this route is right-handed
// with the same X/Z axes and the opposite Y axis. This reflection is its own
// inverse, so the same adapter is used for FBX -> UE and UE -> FBX.
Vec3 ReflectFbxYBasis(const Vec3& Value)
{
    return {Value.X, -Value.Y, Value.Z};
}

TransformRT ReflectFbxYBasis(const TransformRT& Value)
{
    TransformRT Result = Value;
    Result.TranslationCm = ReflectFbxYBasis(Value.TranslationCm);
    Result.Rotation = Normalize({
        -Value.Rotation.X,
        Value.Rotation.Y,
        -Value.Rotation.Z,
        Value.Rotation.W});
    return Result;
}

FbxAMatrix ReflectFbxYBasis(const FbxAMatrix& Value)
{
    FbxAMatrix Reflection;
    Reflection.SetIdentity();
    Reflection.SetS(FbxVector4(1.0, -1.0, 1.0));
    return Reflection * Value * Reflection;
}

Vec3 TransformPoint(const TransformRT& Transform, Vec3 Point)
{
    const Vec3 Scaled{
        Point.X * Transform.Scale.X,
        Point.Y * Transform.Scale.Y,
        Point.Z * Transform.Scale.Z};
    return core::math::Add(
        Transform.TranslationCm,
        core::math::RotateVector(
            Transform.Rotation, Scaled));
}

std::array<float, 12> AffineBasis(const FbxAMatrix& Matrix)
{
    const FbxVector4 Origin =
        Matrix.MultT(FbxVector4(0.0, 0.0, 0.0, 1.0));
    const FbxVector4 XPoint =
        Matrix.MultT(FbxVector4(1.0, 0.0, 0.0, 1.0));
    const FbxVector4 YPoint =
        Matrix.MultT(FbxVector4(0.0, 1.0, 0.0, 1.0));
    const FbxVector4 ZPoint =
        Matrix.MultT(FbxVector4(0.0, 0.0, 1.0, 1.0));
    return {
        static_cast<float>(Origin[0]),
        static_cast<float>(Origin[1]),
        static_cast<float>(Origin[2]),
        static_cast<float>(XPoint[0] - Origin[0]),
        static_cast<float>(XPoint[1] - Origin[1]),
        static_cast<float>(XPoint[2] - Origin[2]),
        static_cast<float>(YPoint[0] - Origin[0]),
        static_cast<float>(YPoint[1] - Origin[1]),
        static_cast<float>(YPoint[2] - Origin[2]),
        static_cast<float>(ZPoint[0] - Origin[0]),
        static_cast<float>(ZPoint[1] - Origin[1]),
        static_cast<float>(ZPoint[2] - Origin[2])};
}

bool Finite(const std::array<float, 12>& Value)
{
    return std::all_of(
        Value.begin(), Value.end(),
        [](float Component)
        {
            return std::isfinite(Component);
        });
}

Vec3 TransformPoint(
    const std::array<float, 12>& Affine,
    Vec3 Point)
{
    return {
        Affine[0] +
            Affine[3] * Point.X +
            Affine[6] * Point.Y +
            Affine[9] * Point.Z,
        Affine[1] +
            Affine[4] * Point.X +
            Affine[7] * Point.Y +
            Affine[10] * Point.Z,
        Affine[2] +
            Affine[5] * Point.X +
            Affine[8] * Point.Y +
            Affine[11] * Point.Z};
}

double Distance(Vec3 Left, Vec3 Right)
{
    return core::math::Length(
        core::math::Subtract(Left, Right));
}

FbxAMatrix GeometryTransform(FbxNode* Node)
{
    FbxAMatrix Geometry;
    Geometry.SetIdentity();
    Geometry.SetT(
        Node->GetGeometricTranslation(FbxNode::eSourcePivot));
    Geometry.SetR(
        Node->GetGeometricRotation(FbxNode::eSourcePivot));
    Geometry.SetS(
        Node->GetGeometricScaling(FbxNode::eSourcePivot));
    return Geometry;
}

void BuildPathMapsRecursive(
    FbxNode* Node,
    const std::string& ParentPath,
    std::map<std::string, FbxNode*>& ByPath,
    std::map<FbxNode*, std::string>& Paths)
{
    if (Node == nullptr) return;
    const std::string Name =
        Node->GetName() != nullptr ? Node->GetName() : "";
    const std::string Path =
        ParentPath.empty() ? Name : ParentPath + "/" + Name;
    if (!Name.empty())
    {
        ByPath[Path] = Node;
        Paths[Node] = Path;
    }
    for (int Index = 0; Index < Node->GetChildCount(); ++Index)
        BuildPathMapsRecursive(
            Node->GetChild(Index), Path, ByPath, Paths);
}

void BuildPathMaps(
    FbxScene* Scene,
    std::map<std::string, FbxNode*>& ByPath,
    std::map<FbxNode*, std::string>& Paths)
{
    FbxNode* Root = Scene != nullptr ? Scene->GetRootNode() : nullptr;
    if (Root == nullptr) return;
    for (int Index = 0; Index < Root->GetChildCount(); ++Index)
        BuildPathMapsRecursive(
            Root->GetChild(Index), "", ByPath, Paths);
}

void CollectMeshNodes(FbxNode* Node, std::vector<FbxNode*>& Out)
{
    if (Node == nullptr) return;
    if (Node->GetMesh() != nullptr) Out.push_back(Node);
    for (int Index = 0; Index < Node->GetChildCount(); ++Index)
        CollectMeshNodes(Node->GetChild(Index), Out);
}

bool ExtractMeshPackage(
    FbxScene* Scene,
    FbxManager* Manager,
    const std::vector<RetargetReviewBone>& SkeletonBones,
    const std::string& Label,
    bool RequireAllSkeletonPaths,
    bool ReflectNativeFbxYIntoUEJson,
    bool AllowUniqueSkeletonBoneNameFallback,
    ReviewMeshPackage& Out,
    std::string& OutError)
{
    if (Scene == nullptr || Manager == nullptr ||
        SkeletonBones.empty())
    {
        OutError = "mesh extraction input is incomplete";
        return false;
    }
    FbxGeometryConverter Converter(Manager);
    if (!Converter.Triangulate(Scene, true, false))
    {
        OutError = Label + ": FBX SDK triangulation failed";
        return false;
    }
    std::map<std::string, FbxNode*> ByPath;
    std::map<FbxNode*, std::string> Paths;
    BuildPathMaps(Scene, ByPath, Paths);
    std::unordered_map<FbxNode*, int> BoneIndexByNode;
    for (std::size_t Index = 0;
         Index < SkeletonBones.size(); ++Index)
    {
        FbxNode* BoneNode = nullptr;
        const auto It = ByPath.find(SkeletonBones[Index].Path);
        if (It != ByPath.end())
        {
            BoneNode = It->second;
        }
        else if (AllowUniqueSkeletonBoneNameFallback)
        {
            for (const auto& Entry : Paths)
            {
                FbxNode* Candidate = Entry.first;
                if (Candidate == nullptr ||
                    Candidate->GetSkeleton() == nullptr ||
                    Candidate->GetName() == nullptr ||
                    SkeletonBones[Index].Name !=
                        Candidate->GetName())
                {
                    continue;
                }
                if (BoneNode != nullptr)
                {
                    OutError = Label +
                        ": skeleton bone-name fallback is ambiguous: " +
                        SkeletonBones[Index].Name;
                    return false;
                }
                BoneNode = Candidate;
            }
        }
        if (BoneNode == nullptr)
        {
            if (RequireAllSkeletonPaths)
            {
                OutError = Label +
                    ": skeleton path missing during mesh extraction: " +
                    SkeletonBones[Index].Path;
                return false;
            }
            continue;
        }
        BoneIndexByNode[BoneNode] = static_cast<int>(Index);
    }

    std::vector<FbxNode*> MeshNodes;
    CollectMeshNodes(Scene->GetRootNode(), MeshNodes);
    if (MeshNodes.empty())
    {
        OutError = Label + ": no FBX Mesh nodes were found";
        return false;
    }
    Out.Label = Label;
    for (FbxNode* MeshNode : MeshNodes)
    {
        FbxMesh* Mesh = MeshNode->GetMesh();
        if (Mesh == nullptr) continue;
        ReviewMesh Result;
        Result.Name =
            MeshNode->GetName() != nullptr ? MeshNode->GetName() : "";
        const auto PathIt = Paths.find(MeshNode);
        Result.Path =
            PathIt != Paths.end() ? PathIt->second : Result.Name;
        Result.MaterialSlotCount = MeshNode->GetMaterialCount();
        Result.BlendShapeDeformerCount =
            Mesh->GetDeformerCount(FbxDeformer::eBlendShape);
        const int ControlPointCount =
            Mesh->GetControlPointsCount();
        if (ControlPointCount <= 0)
        {
            OutError = Label + ": empty mesh control-point inventory: " +
                Result.Path;
            return false;
        }
        Result.ControlPoints.reserve(
            static_cast<std::size_t>(ControlPointCount) * 3);
        const FbxVector4* ControlPoints = Mesh->GetControlPoints();
        for (int Index = 0; Index < ControlPointCount; ++Index)
        {
            const FbxVector4& Point = ControlPoints[Index];
            if (!std::isfinite(Point[0]) ||
                !std::isfinite(Point[1]) ||
                !std::isfinite(Point[2]))
            {
                OutError = Label + ": non-finite mesh control point";
                return false;
            }
            const Vec3 ReviewPoint = ReflectNativeFbxYIntoUEJson
                ? ReflectFbxYBasis(
                    Vec3{Point[0], Point[1], Point[2]})
                : Vec3{Point[0], Point[1], Point[2]};
            Result.ControlPoints.push_back(
                static_cast<float>(ReviewPoint.X));
            Result.ControlPoints.push_back(
                static_cast<float>(ReviewPoint.Y));
            Result.ControlPoints.push_back(
                static_cast<float>(ReviewPoint.Z));
        }
        const int PolygonCount = Mesh->GetPolygonCount();
        Result.TriangleIndices.reserve(
            static_cast<std::size_t>(PolygonCount) * 3);
        for (int PolygonIndex = 0;
             PolygonIndex < PolygonCount; ++PolygonIndex)
        {
            if (Mesh->GetPolygonSize(PolygonIndex) != 3)
            {
                OutError = Label +
                    ": triangulated mesh still contains a non-triangle";
                return false;
            }
            for (int OutputCorner = 0; OutputCorner < 3;
                 ++OutputCorner)
            {
                // A handedness reflection reverses triangle winding.
                const int Corner =
                    ReflectNativeFbxYIntoUEJson &&
                            OutputCorner != 0
                    ? 3 - OutputCorner
                    : OutputCorner;
                const int ControlPointIndex =
                    Mesh->GetPolygonVertex(PolygonIndex, Corner);
                if (ControlPointIndex < 0 ||
                    ControlPointIndex >= ControlPointCount)
                {
                    OutError = Label +
                        ": polygon control-point index is invalid";
                    return false;
                }
                Result.TriangleIndices.push_back(
                    static_cast<std::uint32_t>(
                        ControlPointIndex));
            }
        }

        std::vector<std::vector<std::pair<std::uint32_t, float>>>
            Influences(static_cast<std::size_t>(ControlPointCount));
        Result.SkinDeformerCount =
            Mesh->GetDeformerCount(FbxDeformer::eSkin);
        bool HaveSkinMode = false;
        bool HaveFallback = false;
        for (int SkinIndex = 0;
             SkinIndex < Result.SkinDeformerCount; ++SkinIndex)
        {
            FbxSkin* Skin = FbxCast<FbxSkin>(
                Mesh->GetDeformer(
                    SkinIndex, FbxDeformer::eSkin));
            if (Skin == nullptr)
            {
                OutError = Label + ": invalid skin deformer";
                return false;
            }
            for (int ClusterIndex = 0;
                 ClusterIndex < Skin->GetClusterCount();
                 ++ClusterIndex)
            {
                FbxCluster* Cluster = Skin->GetCluster(ClusterIndex);
                if (Cluster == nullptr ||
                    Cluster->GetLink() == nullptr)
                {
                    OutError = Label +
                        ": skin cluster or link is unavailable";
                    return false;
                }
                const auto BoneIt =
                    BoneIndexByNode.find(Cluster->GetLink());
                if (BoneIt == BoneIndexByNode.end())
                {
                    OutError = Label +
                        ": skin cluster link is outside the declared skeleton: " +
                        std::string(Cluster->GetLink()->GetName());
                    return false;
                }
                ReviewSkinMode SkinMode;
                switch (Cluster->GetLinkMode())
                {
                case FbxCluster::eNormalize:
                    SkinMode = ReviewSkinMode::Normalize;
                    break;
                case FbxCluster::eTotalOne:
                    SkinMode = ReviewSkinMode::TotalOne;
                    break;
                default:
                    OutError = Label +
                        ": additive or unsupported skin link mode";
                    return false;
                }
                if (HaveSkinMode && Result.SkinMode != SkinMode)
                {
                    OutError = Label +
                        ": mixed skin link modes in one mesh";
                    return false;
                }
                Result.SkinMode = SkinMode;
                HaveSkinMode = true;

                FbxAMatrix MeshBind;
                FbxAMatrix LinkBind;
                Cluster->GetTransformMatrix(MeshBind);
                Cluster->GetTransformLinkMatrix(LinkBind);
                MeshBind *= GeometryTransform(MeshNode);
                if (!HaveFallback)
                {
                    Result.FallbackGlobal = AffineBasis(
                        ReflectNativeFbxYIntoUEJson
                            ? ReflectFbxYBasis(MeshBind)
                            : MeshBind);
                    HaveFallback = true;
                }
                const FbxAMatrix BindOffsetMatrix =
                    LinkBind.Inverse() * MeshBind;
                ReviewMeshCluster Binding;
                Binding.BoneIndex = BoneIt->second;
                Binding.BindOffset = AffineBasis(
                    ReflectNativeFbxYIntoUEJson
                        ? ReflectFbxYBasis(BindOffsetMatrix)
                        : BindOffsetMatrix);
                if (!Finite(Binding.BindOffset))
                {
                    OutError = Label +
                        ": non-finite cluster bind offset";
                    return false;
                }
                const std::uint32_t ReviewClusterIndex =
                    static_cast<std::uint32_t>(
                        Result.Clusters.size());
                Result.Clusters.push_back(Binding);
                const int InfluenceCount =
                    Cluster->GetControlPointIndicesCount();
                const int* Indices =
                    Cluster->GetControlPointIndices();
                const double* Weights =
                    Cluster->GetControlPointWeights();
                if (InfluenceCount < 0 ||
                    (InfluenceCount > 0 &&
                     (Indices == nullptr || Weights == nullptr)))
                {
                    OutError = Label +
                        ": skin influence arrays are unavailable";
                    return false;
                }
                for (int InfluenceIndex = 0;
                     InfluenceIndex < InfluenceCount;
                     ++InfluenceIndex)
                {
                    const int ControlPointIndex =
                        Indices[InfluenceIndex];
                    const double Weight = Weights[InfluenceIndex];
                    if (ControlPointIndex < 0 ||
                        ControlPointIndex >= ControlPointCount ||
                        !std::isfinite(Weight) || Weight < 0.0)
                    {
                        OutError = Label +
                            ": invalid skin influence";
                        return false;
                    }
                    if (Weight <= 0.0) continue;
                    Influences[
                        static_cast<std::size_t>(
                            ControlPointIndex)]
                        .push_back(
                            {ReviewClusterIndex,
                             static_cast<float>(Weight)});
                }
                if (InfluenceCount > 0)
                {
                    const int SampleIndex = Indices[0];
                    const FbxVector4 SamplePoint =
                        ControlPoints[SampleIndex];
                    const FbxVector4 Expected =
                        MeshBind.MultT(SamplePoint);
                    const FbxVector4 BoundPoint =
                        BindOffsetMatrix.MultT(SamplePoint);
                    const FbxVector4 ReconstructedFbx =
                        LinkBind.MultT(BoundPoint);
                    const Vec3 Reconstructed{
                        ReconstructedFbx[0],
                        ReconstructedFbx[1],
                        ReconstructedFbx[2]};
                    const double ErrorCm = Distance(
                        Reconstructed,
                        {Expected[0], Expected[1], Expected[2]});
                    Result.MaximumBindReconstructionErrorCm =
                        std::max(
                            Result.MaximumBindReconstructionErrorCm,
                            ErrorCm);
                }
            }
        }
        if (!HaveFallback)
        {
            FbxTime Time;
            Time.SetSecondDouble(0.0);
            FbxAMatrix Fallback =
                MeshNode->EvaluateGlobalTransform(
                    Time, FbxNode::eSourcePivot);
            Fallback *= GeometryTransform(MeshNode);
            Result.FallbackGlobal = AffineBasis(
                ReflectNativeFbxYIntoUEJson
                    ? ReflectFbxYBasis(Fallback)
                    : Fallback);
        }
        if (!Finite(Result.FallbackGlobal))
        {
            OutError = Label +
                ": mesh fallback transform is non-finite";
            return false;
        }
        Result.InfluenceOffsets.reserve(
            static_cast<std::size_t>(ControlPointCount) + 1);
        Result.InfluenceOffsets.push_back(0);
        for (auto& ControlPointInfluences : Influences)
        {
            std::sort(
                ControlPointInfluences.begin(),
                ControlPointInfluences.end(),
                [](const auto& Left, const auto& Right)
                {
                    return Left.first < Right.first;
                });
            Result.MaximumInfluencesPerControlPoint =
                std::max(
                    Result.MaximumInfluencesPerControlPoint,
                    static_cast<int>(
                        ControlPointInfluences.size()));
            for (const auto& Influence :
                 ControlPointInfluences)
            {
                Result.InfluenceClusters.push_back(
                    Influence.first);
                Result.InfluenceWeights.push_back(
                    Influence.second);
            }
            Result.InfluenceOffsets.push_back(
                static_cast<std::uint32_t>(
                    Result.InfluenceClusters.size()));
        }
        if (Result.MaximumBindReconstructionErrorCm > 1.0e-4)
        {
            OutError = Label +
                ": cluster bind reconstruction exceeds tolerance";
            return false;
        }

        Out.ControlPointCount += ControlPointCount;
        Out.TriangleCount += PolygonCount;
        Out.SkinDeformerCount += Result.SkinDeformerCount;
        Out.SkinClusterCount +=
            static_cast<int>(Result.Clusters.size());
        Out.InfluenceCount +=
            static_cast<int>(Result.InfluenceWeights.size());
        Out.BlendShapeDeformerCount +=
            Result.BlendShapeDeformerCount;
        Out.MaterialSlotCount += Result.MaterialSlotCount;
        Out.MaximumInfluencesPerControlPoint =
            std::max(
                Out.MaximumInfluencesPerControlPoint,
                Result.MaximumInfluencesPerControlPoint);
        Out.MaximumBindReconstructionErrorCm =
            std::max(
                Out.MaximumBindReconstructionErrorCm,
                Result.MaximumBindReconstructionErrorCm);
        Out.Meshes.push_back(std::move(Result));
    }
    if (Out.Meshes.empty() ||
        Out.ControlPointCount <= 0 ||
        Out.TriangleCount <= 0 ||
        Out.SkinClusterCount <= 0 ||
        Out.InfluenceCount <= 0)
    {
        OutError = Label +
            ": mesh/skin extraction produced no usable geometry";
        return false;
    }
    return true;
}

template <typename T>
std::vector<unsigned char> ToBytes(const std::vector<T>& Values)
{
    std::vector<unsigned char> Bytes(
        Values.size() * sizeof(T));
    if (!Bytes.empty())
        std::memcpy(
            Bytes.data(), Values.data(), Bytes.size());
    return Bytes;
}

std::string Base64Encode(
    const std::vector<unsigned char>& Bytes)
{
    static constexpr char Alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    std::string Output;
    Output.reserve(((Bytes.size() + 2) / 3) * 4);
    std::size_t Index = 0;
    while (Index + 3 <= Bytes.size())
    {
        const std::uint32_t Value =
            (static_cast<std::uint32_t>(Bytes[Index]) << 16) |
            (static_cast<std::uint32_t>(Bytes[Index + 1]) << 8) |
            static_cast<std::uint32_t>(Bytes[Index + 2]);
        Output.push_back(Alphabet[(Value >> 18) & 63]);
        Output.push_back(Alphabet[(Value >> 12) & 63]);
        Output.push_back(Alphabet[(Value >> 6) & 63]);
        Output.push_back(Alphabet[Value & 63]);
        Index += 3;
    }
    const std::size_t Remaining = Bytes.size() - Index;
    if (Remaining == 1)
    {
        const std::uint32_t Value =
            static_cast<std::uint32_t>(Bytes[Index]) << 16;
        Output.push_back(Alphabet[(Value >> 18) & 63]);
        Output.push_back(Alphabet[(Value >> 12) & 63]);
        Output.push_back('=');
        Output.push_back('=');
    }
    else if (Remaining == 2)
    {
        const std::uint32_t Value =
            (static_cast<std::uint32_t>(Bytes[Index]) << 16) |
            (static_cast<std::uint32_t>(Bytes[Index + 1]) << 8);
        Output.push_back(Alphabet[(Value >> 18) & 63]);
        Output.push_back(Alphabet[(Value >> 12) & 63]);
        Output.push_back(Alphabet[(Value >> 6) & 63]);
        Output.push_back('=');
    }
    return Output;
}

void AppendTransform(
    std::vector<float>& Out,
    const TransformRT& Value)
{
    Out.push_back(static_cast<float>(Value.TranslationCm.X));
    Out.push_back(static_cast<float>(Value.TranslationCm.Y));
    Out.push_back(static_cast<float>(Value.TranslationCm.Z));
    const Quat Rotation = Normalize(Value.Rotation);
    Out.push_back(static_cast<float>(Rotation.X));
    Out.push_back(static_cast<float>(Rotation.Y));
    Out.push_back(static_cast<float>(Rotation.Z));
    Out.push_back(static_cast<float>(Rotation.W));
    Out.push_back(static_cast<float>(Value.Scale.X));
    Out.push_back(static_cast<float>(Value.Scale.Y));
    Out.push_back(static_cast<float>(Value.Scale.Z));
}

void WriteTransformArray(
    std::ostringstream& Json,
    const TransformRT& Value)
{
    const Quat Rotation = Normalize(Value.Rotation);
    Json << "[" << Value.TranslationCm.X << ","
         << Value.TranslationCm.Y << ","
         << Value.TranslationCm.Z << ","
         << Rotation.X << "," << Rotation.Y << ","
         << Rotation.Z << "," << Rotation.W << ","
         << Value.Scale.X << "," << Value.Scale.Y << ","
         << Value.Scale.Z << "]";
}

void WriteAffineArray(
    std::ostringstream& Json,
    const std::array<float, 12>& Value)
{
    Json << "[";
    for (std::size_t Index = 0;
         Index < Value.size(); ++Index)
    {
        if (Index) Json << ",";
        Json << Value[Index];
    }
    Json << "]";
}

void WriteMeshPackageJson(
    std::ostringstream& Json,
    const ReviewMeshPackage& Package)
{
    Json << "{\"label\":\"" << JsonEscape(Package.Label)
         << "\",\"meshCount\":" << Package.Meshes.size()
         << ",\"controlPointCount\":" << Package.ControlPointCount
         << ",\"triangleCount\":" << Package.TriangleCount
         << ",\"skinDeformerCount\":"
         << Package.SkinDeformerCount
         << ",\"skinClusterCount\":"
         << Package.SkinClusterCount
         << ",\"influenceCount\":" << Package.InfluenceCount
         << ",\"blendShapeDeformerCount\":"
         << Package.BlendShapeDeformerCount
         << ",\"materialSlotCount\":"
         << Package.MaterialSlotCount
         << ",\"maximumInfluencesPerControlPoint\":"
         << Package.MaximumInfluencesPerControlPoint
         << ",\"maximumBindReconstructionErrorCm\":"
         << Package.MaximumBindReconstructionErrorCm
         << ",\"meshes\":[";
    for (std::size_t MeshIndex = 0;
         MeshIndex < Package.Meshes.size(); ++MeshIndex)
    {
        if (MeshIndex) Json << ",";
        const ReviewMesh& Mesh = Package.Meshes[MeshIndex];
        std::vector<std::uint32_t> ClusterBones;
        std::vector<float> ClusterOffsets;
        ClusterBones.reserve(Mesh.Clusters.size());
        ClusterOffsets.reserve(Mesh.Clusters.size() * 12);
        for (const ReviewMeshCluster& Cluster : Mesh.Clusters)
        {
            ClusterBones.push_back(
                static_cast<std::uint32_t>(
                    Cluster.BoneIndex));
            ClusterOffsets.insert(
                ClusterOffsets.end(),
                Cluster.BindOffset.begin(),
                Cluster.BindOffset.end());
        }
        Json << "{\"name\":\"" << JsonEscape(Mesh.Name)
             << "\",\"path\":\"" << JsonEscape(Mesh.Path)
             << "\",\"skinMode\":\""
             << (Mesh.SkinMode == ReviewSkinMode::Normalize
                     ? "normalize"
                     : "total_one")
             << "\",\"controlPointCount\":"
             << Mesh.ControlPoints.size() / 3
             << ",\"triangleCount\":"
             << Mesh.TriangleIndices.size() / 3
             << ",\"clusterCount\":" << Mesh.Clusters.size()
             << ",\"skinDeformerCount\":"
             << Mesh.SkinDeformerCount
             << ",\"blendShapeDeformerCount\":"
             << Mesh.BlendShapeDeformerCount
             << ",\"materialSlotCount\":"
             << Mesh.MaterialSlotCount
             << ",\"maximumInfluencesPerControlPoint\":"
             << Mesh.MaximumInfluencesPerControlPoint
             << ",\"fallback\":";
        WriteAffineArray(Json, Mesh.FallbackGlobal);
        Json << ",\"p\":\""
             << Base64Encode(ToBytes(Mesh.ControlPoints))
             << "\",\"tri\":\""
             << Base64Encode(ToBytes(Mesh.TriangleIndices))
             << "\",\"io\":\""
             << Base64Encode(ToBytes(Mesh.InfluenceOffsets))
             << "\",\"ic\":\""
             << Base64Encode(ToBytes(Mesh.InfluenceClusters))
             << "\",\"iw\":\""
             << Base64Encode(ToBytes(Mesh.InfluenceWeights))
             << "\",\"cb\":\""
             << Base64Encode(ToBytes(ClusterBones))
             << "\",\"co\":\""
             << Base64Encode(ToBytes(ClusterOffsets))
             << "\"}";
    }
    Json << "]}";
}

constexpr double LimbIkTelemetryToleranceCm = 1.0e-3;

bool LimbIkTelemetrySane(const RetargetReviewClipView& Clip)
{
    return Clip.LimbIkFamilyTransactions >= 0 &&
        Clip.LimbIkCommittedFamilyTransactions >= 0 &&
        Clip.LimbIkRolledBackFamilyTransactions >= 0 &&
        Clip.LimbIkAppliedChainRecords >= 0 &&
        Clip.LimbIkFailClosedChainRecords >= 0 &&
        std::isfinite(Clip.LimbIkMaximumEndpointErrorCm) &&
        Clip.LimbIkMaximumEndpointErrorCm >= 0.0 &&
        std::isfinite(
            Clip.LimbIkMaximumShadowToRealPositionDeltaCm) &&
        Clip.LimbIkMaximumShadowToRealPositionDeltaCm >= 0.0;
}

bool LimbIkTelemetryFullCommit(const RetargetReviewClipView& Clip)
{
    if (!LimbIkTelemetrySane(Clip) ||
        Clip.Frames.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max() / 4))
    {
        return false;
    }
    const int FrameCount = static_cast<int>(Clip.Frames.size());
    const int ExpectedFamilies = FrameCount * 2;
    const int ExpectedChains = FrameCount * 4;
    return ExpectedFamilies > 0 &&
        Clip.LimbIkFamilyTransactions == ExpectedFamilies &&
        Clip.LimbIkCommittedFamilyTransactions == ExpectedFamilies &&
        Clip.LimbIkRolledBackFamilyTransactions == 0 &&
        Clip.LimbIkAppliedChainRecords == ExpectedChains &&
        Clip.LimbIkAppliedChainRecords ==
            Clip.LimbIkFamilyTransactions * 2 &&
        Clip.LimbIkFailClosedChainRecords == 0 &&
        Clip.LimbIkMaximumEndpointErrorCm <=
            LimbIkTelemetryToleranceCm &&
        Clip.LimbIkMaximumShadowToRealPositionDeltaCm <=
        LimbIkTelemetryToleranceCm;
}

bool UEJsonLimbIkTelemetryFullCommit(
    const RetargetReviewClipView& Clip)
{
    return LimbIkTelemetrySane(Clip) &&
        Clip.Frames.size() <=
            static_cast<std::size_t>(
                std::numeric_limits<int>::max() / 4) &&
        Clip.LimbIkAppliedChainRecords ==
            static_cast<int>(Clip.Frames.size()) * 4 &&
        Clip.LimbIkFailClosedChainRecords == 0 &&
        Clip.LimbIkMaximumEndpointErrorCm <= 0.05;
}

bool LimbIkTelemetryCommittedForContract(
    const RetargetReviewPackageOptions& Options,
    const RetargetReviewClipView& Clip)
{
    return Options.ContractKind == "ue_ik_json_v1"
        ? UEJsonLimbIkTelemetryFullCommit(Clip)
        : LimbIkTelemetryFullCommit(Clip);
}

bool SourceMotionFootLockTelemetrySane(
    const RetargetReviewClipView& Clip)
{
    if (!Clip.SourceMotionFootLockEnabled)
        return true;
    if (Clip.Frames.size() >
        static_cast<std::size_t>(
            std::numeric_limits<int>::max() / 2))
    {
        return false;
    }
    const int FrameCount = static_cast<int>(Clip.Frames.size());
    return Clip.SourceMotionFootLockSuccess &&
        Clip.SourceMotionFootLockDeterministic &&
        Clip.SourceMotionFootLockNoGroundOrContactSemanticsUsed &&
        Clip.SourceMotionFootLockCommittedFrames == FrameCount &&
        Clip.SourceMotionFootLockRolledBackFrames == 0 &&
        Clip.SourceMotionFootLockPositionNoMotionDeltas >= 0 &&
        Clip.SourceMotionFootLockPositionMotionDeltas >= 0 &&
        Clip.SourceMotionFootLockPositionNoMotionDeltas +
                Clip.SourceMotionFootLockPositionMotionDeltas ==
            FrameCount * 2 &&
        Clip.SourceMotionFootLockRotationNoMotionDeltas >= 0 &&
        Clip.SourceMotionFootLockRotationMotionDeltas >= 0 &&
        Clip.SourceMotionFootLockRotationNoMotionDeltas +
                Clip.SourceMotionFootLockRotationMotionDeltas ==
            FrameCount * 2 &&
        Clip.SourceMotionFootLockRotationGateReleases ==
            Clip.SourceMotionFootLockRotationMotionDeltas &&
        std::isfinite(
            Clip.SourceMotionFootLockMaximumReleasedFoundationRotationDegrees) &&
        Clip.SourceMotionFootLockMaximumReleasedFoundationRotationDegrees >=
            0.0 &&
        std::isfinite(
            Clip.SourceMotionFootLockMaximumRealEndOrientationErrorDegrees) &&
        Clip.SourceMotionFootLockMaximumRealEndOrientationErrorDegrees >=
            0.0 &&
        Clip.SourceMotionFootLockMaximumRealEndOrientationErrorDegrees <=
            1.0e-5 &&
        std::isfinite(
            Clip.SourceMotionFootLockMaximumNoMotionTargetDriftCm) &&
        Clip.SourceMotionFootLockMaximumNoMotionTargetDriftCm >= 0.0 &&
        Clip.SourceMotionFootLockMaximumNoMotionTargetDriftCm <= 1.0e-3 &&
        std::isfinite(
            Clip.SourceMotionFootLockMaximumTargetDeltaErrorCm) &&
        Clip.SourceMotionFootLockMaximumTargetDeltaErrorCm >= 0.0 &&
        Clip.SourceMotionFootLockMaximumTargetDeltaErrorCm <= 1.0e-3;
}

bool ValidateClip(
    const RetargetReviewPackageOptions& Options,
    const RetargetReviewClipView& Clip,
    std::string& OutError)
{
    if (Clip.Id.empty() || Clip.Label.empty() ||
        Clip.FoundationExportFbxFileName.empty() ||
        Clip.ExportFbxFileName.empty() ||
        !std::isfinite(Clip.FramesPerSecond) ||
        Clip.FramesPerSecond <= 0.0 ||
        Clip.Frames.empty() ||
        Clip.SourceDirectBoneCount < 0 ||
        Clip.SourceRestPassthroughBoneCount < 0 ||
        ((Clip.SourceDirectBoneCount != 0 ||
          Clip.SourceRestPassthroughBoneCount != 0) &&
         Clip.SourceDirectBoneCount +
                 Clip.SourceRestPassthroughBoneCount !=
             static_cast<int>(Options.SourceBones.size())))
    {
        OutError = "review clip metadata is incomplete";
        return false;
    }
    const bool RequiresFullCommitLimbIk =
        Options.RouteId.find(
            "limb_ik_unit_scale_shadow_pose_rotation_only_writeback_v1") !=
        std::string::npos;
    const bool UEJsonFullCommit =
        Options.ContractKind == "ue_ik_json_v1" &&
        UEJsonLimbIkTelemetryFullCommit(Clip);
    if (!LimbIkTelemetrySane(Clip) ||
        (RequiresFullCommitLimbIk &&
         !LimbIkTelemetryFullCommit(Clip)) ||
        (Options.ContractKind == "ue_ik_json_v1" &&
         !UEJsonFullCommit))
    {
        OutError =
            "review clip limb IK telemetry is incomplete, non-finite, out of bounds, or not fully committed";
        return false;
    }
    if (Clip.SourceMotionFootLockEnabled !=
            Options.SourceMotionFootLockCandidateEnabled ||
        !SourceMotionFootLockTelemetrySane(Clip))
    {
        OutError =
            "review clip source-motion FootLock telemetry or enabled state is invalid";
        return false;
    }
    int PreviousFrame = std::numeric_limits<int>::min();
    for (const RetargetReviewFrameView& Frame : Clip.Frames)
    {
        if (Frame.SourceModelPose == nullptr ||
            Frame.TargetFkModelPose == nullptr ||
            Frame.TargetFoundationLocalPose == nullptr ||
            Frame.TargetFoundationModelPose == nullptr ||
            Frame.TargetFinalLocalPose == nullptr ||
            Frame.TargetFinalModelPose == nullptr ||
            Frame.SourceModelPose->size() !=
                Options.SourceBones.size() ||
            Frame.TargetFkModelPose->Size() !=
                Options.TargetBones.size() ||
            Frame.TargetFoundationLocalPose->Size() !=
                Options.TargetBones.size() ||
            Frame.TargetFoundationModelPose->Size() !=
                Options.TargetBones.size() ||
            Frame.TargetFinalLocalPose->Size() !=
                Options.TargetBones.size() ||
            Frame.TargetFinalModelPose->Size() !=
                Options.TargetBones.size() ||
            Frame.FrameIndex <= PreviousFrame ||
            !std::isfinite(Frame.TimeSeconds))
        {
            OutError = "review clip frame inventory is invalid";
            return false;
        }
        for (const TransformRT& Value : *Frame.SourceModelPose)
            if (!Finite(Value))
            {
                OutError =
                    "source review pose contains a non-finite transform";
                return false;
            }
        for (const TransformRT& Value :
             Frame.TargetFkModelPose->Transforms())
            if (!Finite(Value))
            {
                OutError =
                    "target FK review pose contains a non-finite transform";
                return false;
            }
        for (const TransformRT& Value :
             Frame.TargetFoundationLocalPose->Transforms())
            if (!Finite(Value))
            {
                OutError =
                    "target Foundation local pose contains a non-finite transform";
                return false;
            }
        for (const TransformRT& Value :
             Frame.TargetFoundationModelPose->Transforms())
            if (!Finite(Value))
            {
                OutError =
                    "target Foundation model pose contains a non-finite transform";
                return false;
            }
        for (const TransformRT& Value :
             Frame.TargetFinalLocalPose->Transforms())
            if (!Finite(Value))
            {
                OutError =
                    "target Final local pose contains a non-finite transform";
                return false;
            }
        for (const TransformRT& Value :
             Frame.TargetFinalModelPose->Transforms())
            if (!Finite(Value))
            {
                OutError =
                    "target Final model pose contains a non-finite transform";
                return false;
            }
        if (!Clip.SourceMotionFootLockEnabled &&
            (!ExactPose(*Frame.TargetFoundationLocalPose,
                        *Frame.TargetFinalLocalPose) ||
             !ExactPose(*Frame.TargetFoundationModelPose,
                        *Frame.TargetFinalModelPose)))
        {
            OutError =
                "disabled source-motion FootLock did not exactly passthrough Foundation";
            return false;
        }
        PreviousFrame = Frame.FrameIndex;
    }
    return true;
}

std::string BuildViewerData(
    const RetargetReviewPackageOptions& Options,
    const std::vector<ReviewMeshPackage>& SourceMeshes,
    const std::vector<bool>& SourceMeshFallbackUsed,
    const ReviewMeshPackage& TargetMesh)
{
    std::ostringstream Json;
    Json << std::setprecision(12);
    Json << "{\"schema\":\"skrtg.d1_17b_retarget_review_viewer.v3\","
         << "\"route\":\"" << JsonEscape(Options.RouteId)
         << "\",\"selected\":false,\"adopted\":false,"
         << "\"stageComplete\":false,\"route_selected\":false,"
         << "\"route_adopted\":false,\"stage_complete\":false,"
         << "\"foundationRoute\":\""
         << JsonEscape(Options.FoundationRouteId)
         << "\",\"foundationFrozen\":"
         << (Options.FoundationFrozen ? "true" : "false")
         << ",\"sourceMotionFootLockRoute\":\""
         << JsonEscape(Options.SourceMotionFootLockRouteId)
         << "\",\"sourceMotionFootLockCandidateEnabled\":"
         << (Options.SourceMotionFootLockCandidateEnabled
                 ? "true" : "false")
         << ",\"sourceMotionFootLockCandidateSelected\":"
         << (Options.SourceMotionFootLockCandidateSelected
                 ? "true" : "false")
         << ",\"sourceMotionFootLockCandidateAdopted\":"
         << (Options.SourceMotionFootLockCandidateAdopted
                 ? "true" : "false") << ","
         << "\"upstreamLimbIkRouteSelected\":"
         << (Options.UpstreamLimbIkRouteSelected ? "true" : "false")
         << ",\"upstreamLimbIkRouteAdopted\":"
         << (Options.UpstreamLimbIkRouteAdopted ? "true" : "false")
         << ",\"spinePelvisFollowCandidateEnabled\":"
         << (Options.SpinePelvisFollowCandidateEnabled
                 ? "true" : "false")
         << ",\"spinePelvisFollowCandidateSelected\":"
         << (Options.SpinePelvisFollowCandidateSelected
                 ? "true" : "false")
         << ",\"spinePelvisFollowCandidateAdopted\":"
         << (Options.SpinePelvisFollowCandidateAdopted
                 ? "true" : "false") << ","
         << "\"sourceBones\":[";
    for (std::size_t Index = 0;
         Index < Options.SourceBones.size(); ++Index)
    {
        if (Index) Json << ",";
        const RetargetReviewBone& Bone = Options.SourceBones[Index];
        Json << "[" << Bone.ParentIndex << ",\""
             << JsonEscape(Bone.Name) << "\",\""
             << JsonEscape(Bone.Path) << "\","
             << (Bone.ParticipatesInIk ? "true" : "false")
             << "," << Bone.RestModelPositionCm.X
             << "," << Bone.RestModelPositionCm.Y
             << "," << Bone.RestModelPositionCm.Z
             << "]";
    }
    Json << "],\"targetBones\":[";
    for (std::size_t Index = 0;
         Index < Options.TargetBones.size(); ++Index)
    {
        if (Index) Json << ",";
        const RetargetReviewBone& Bone = Options.TargetBones[Index];
        Json << "[" << Bone.ParentIndex << ",\""
             << JsonEscape(Bone.Name) << "\",\""
             << JsonEscape(Bone.Path) << "\","
             << (Bone.ParticipatesInIk ? "true" : "false")
             << "," << Bone.RestModelPositionCm.X
             << "," << Bone.RestModelPositionCm.Y
             << "," << Bone.RestModelPositionCm.Z
             << "]";
    }
    const RetargetReviewRootPelvisContract& RootPelvis =
        Options.RootPelvisContract;
    Json << "],\"rootPelvis\":{"
         << "\"sourceRoot\":" << RootPelvis.SourceRootBoneIndex << ","
         << "\"sourcePelvis\":" << RootPelvis.SourcePelvisBoneIndex << ","
         << "\"targetHips\":" << RootPelvis.TargetHipsBoneIndex << ","
         << "\"rootOwnership\":\""
         << JsonEscape(RootPelvis.RootOwnership) << "\","
         << "\"pelvisOwnership\":\""
         << JsonEscape(RootPelvis.PelvisOwnership) << "\","
         << "\"scaleOwnership\":\""
         << JsonEscape(RootPelvis.ScaleOwnership) << "\"},"
         << "\"retargetChains\":[";
    for (std::size_t ChainIndex = 0;
         ChainIndex < Options.RetargetChains.size(); ++ChainIndex)
    {
        if (ChainIndex) Json << ",";
        const RetargetReviewChain& Chain =
            Options.RetargetChains[ChainIndex];
        Json << "{\"label\":\"" << JsonEscape(Chain.Label)
             << "\",\"source\":[";
        for (std::size_t Index = 0;
             Index < Chain.SourceBoneIndices.size(); ++Index)
        {
            if (Index) Json << ",";
            Json << Chain.SourceBoneIndices[Index];
        }
        Json << "],\"target\":[";
        for (std::size_t Index = 0;
             Index < Chain.TargetBoneIndices.size(); ++Index)
        {
            if (Index) Json << ",";
            Json << Chain.TargetBoneIndices[Index];
        }
        Json << "],\"ikMode\":\"" << JsonEscape(Chain.IkMode)
             << "\",\"sourceGoalName\":\""
             << JsonEscape(Chain.SourceGoalName)
             << "\",\"targetGoalName\":\""
             << JsonEscape(Chain.TargetGoalName)
             << "\",\"sourceGoalBone\":"
             << Chain.SourceGoalBoneIndex
             << ",\"targetGoalBone\":"
             << Chain.TargetGoalBoneIndex
             << ",\"sourcePoleBone\":"
             << Chain.SourcePoleBoneIndex
             << ",\"targetPoleBone\":"
             << Chain.TargetPoleBoneIndex << "}";
    }
    Json << "],\"anchors\":[";
    for (std::size_t Index = 0;
         Index < Options.Anchors.size(); ++Index)
    {
        if (Index) Json << ",";
        const RetargetReviewAnchor& Anchor =
            Options.Anchors[Index];
        const Quat Basis =
            Normalize(Anchor.SourceToTargetRestBasis);
        Json << "{\"label\":\"" << JsonEscape(Anchor.Label)
             << "\",\"sourceBone\":" << Anchor.SourceBoneIndex
             << ",\"targetBone\":" << Anchor.TargetBoneIndex
             << ",\"sourcePath\":\""
             << JsonEscape(Anchor.SourcePath)
             << "\",\"targetPath\":\""
             << JsonEscape(Anchor.TargetPath)
             << "\",\"basis\":[" << Basis.X << ","
             << Basis.Y << "," << Basis.Z << ","
             << Basis.W << "]}";
    }
    Json << "],\"sourceMeshes\":[";
    for (std::size_t Index = 0;
         Index < SourceMeshes.size(); ++Index)
    {
        if (Index) Json << ",";
        WriteMeshPackageJson(Json, SourceMeshes[Index]);
    }
    Json << "],\"targetMesh\":";
    WriteMeshPackageJson(Json, TargetMesh);
    Json << ",\"clips\":[";
    for (std::size_t ClipIndex = 0;
         ClipIndex < Options.Clips.size(); ++ClipIndex)
    {
        if (ClipIndex) Json << ",";
        const RetargetReviewClipView& Clip =
            Options.Clips[ClipIndex];
        std::vector<float> SourceTransforms;
        std::vector<float> FkTransforms;
        std::vector<float> FoundationTransforms;
        std::vector<float> FinalTransforms;
        SourceTransforms.reserve(
            Clip.Frames.size() *
            Options.SourceBones.size() * 10);
        FkTransforms.reserve(
            Clip.Frames.size() *
            Options.TargetBones.size() * 10);
        FoundationTransforms.reserve(
            Clip.Frames.size() *
            Options.TargetBones.size() * 10);
        FinalTransforms.reserve(
            Clip.Frames.size() *
            Options.TargetBones.size() * 10);
        for (const RetargetReviewFrameView& Frame : Clip.Frames)
        {
            for (const TransformRT& Value :
                 *Frame.SourceModelPose)
                AppendTransform(SourceTransforms, Value);
            for (const TransformRT& Value :
                 Frame.TargetFkModelPose->Transforms())
                AppendTransform(FkTransforms, Value);
            for (const TransformRT& Value :
                 Frame.TargetFoundationModelPose->Transforms())
                AppendTransform(FoundationTransforms, Value);
            for (const TransformRT& Value :
                 Frame.TargetFinalModelPose->Transforms())
                AppendTransform(FinalTransforms, Value);
        }
        Json << "{\"id\":\"" << JsonEscape(Clip.Id)
             << "\",\"label\":\"" << JsonEscape(Clip.Label)
             << "\",\"sourceMeshIndex\":" << ClipIndex
             << ",\"sourceMeshFallbackUsed\":"
             << (SourceMeshFallbackUsed[ClipIndex]
                     ? "true" : "false")
             << ",\"sourceDirectBoneCount\":"
             << Clip.SourceDirectBoneCount
              << ",\"sourceRestPassthroughBoneCount\":"
              << Clip.SourceRestPassthroughBoneCount
               << ",\"limbIkStatus\":\""
               << (LimbIkTelemetryCommittedForContract(Options, Clip)
                       ? "committed"
                       : "fail_closed")
              << "\",\"limbIkUnitScaleShadowProjectionApplied\":"
              << (Clip.LimbIkUnitScaleShadowProjectionApplied
                      ? "true" : "false")
              << ",\"limbIkFamilyTransactions\":"
              << Clip.LimbIkFamilyTransactions
              << ",\"limbIkCommittedFamilyTransactions\":"
              << Clip.LimbIkCommittedFamilyTransactions
              << ",\"limbIkRolledBackFamilyTransactions\":"
              << Clip.LimbIkRolledBackFamilyTransactions
              << ",\"limbIkAppliedChainRecords\":"
              << Clip.LimbIkAppliedChainRecords
              << ",\"limbIkFailClosedChainRecords\":"
              << Clip.LimbIkFailClosedChainRecords
              << ",\"limbIkMaximumEndpointErrorCm\":"
              << Clip.LimbIkMaximumEndpointErrorCm
               << ",\"limbIkMaximumShadowToRealPositionDeltaCm\":"
               << Clip.LimbIkMaximumShadowToRealPositionDeltaCm
               << ",\"sourceMotionFootLockEnabled\":"
               << (Clip.SourceMotionFootLockEnabled ? "true" : "false")
               << ",\"sourceMotionFootLockSuccess\":"
               << (Clip.SourceMotionFootLockSuccess ? "true" : "false")
               << ",\"sourceMotionFootLockDeterministic\":"
               << (Clip.SourceMotionFootLockDeterministic ? "true" : "false")
               << ",\"sourceMotionFootLockNoGroundOrContactSemanticsUsed\":"
               << (Clip.SourceMotionFootLockNoGroundOrContactSemanticsUsed
                       ? "true" : "false")
               << ",\"sourceMotionFootLockCommittedFrames\":"
               << Clip.SourceMotionFootLockCommittedFrames
               << ",\"sourceMotionFootLockRolledBackFrames\":"
               << Clip.SourceMotionFootLockRolledBackFrames
               << ",\"sourceMotionFootLockPositionNoMotionDeltas\":"
               << Clip.SourceMotionFootLockPositionNoMotionDeltas
               << ",\"sourceMotionFootLockPositionMotionDeltas\":"
               << Clip.SourceMotionFootLockPositionMotionDeltas
               << ",\"sourceMotionFootLockRotationNoMotionDeltas\":"
               << Clip.SourceMotionFootLockRotationNoMotionDeltas
               << ",\"sourceMotionFootLockRotationMotionDeltas\":"
               << Clip.SourceMotionFootLockRotationMotionDeltas
               << ",\"sourceMotionFootLockRotationGateReleases\":"
               << Clip.SourceMotionFootLockRotationGateReleases
               << ",\"sourceMotionFootLockMaximumReleasedFoundationRotationDegrees\":"
               << Clip.SourceMotionFootLockMaximumReleasedFoundationRotationDegrees
               << ",\"sourceMotionFootLockMaximumRealEndOrientationErrorDegrees\":"
               << Clip.SourceMotionFootLockMaximumRealEndOrientationErrorDegrees
               << ",\"sourceMotionFootLockMaximumNoMotionTargetDriftCm\":"
               << Clip.SourceMotionFootLockMaximumNoMotionTargetDriftCm
               << ",\"sourceMotionFootLockMaximumTargetDeltaErrorCm\":"
               << Clip.SourceMotionFootLockMaximumTargetDeltaErrorCm
              << ",\"fps\":" << Clip.FramesPerSecond
             << ",\"startFrame\":" << Clip.Frames.front().FrameIndex
             << ",\"stopFrame\":" << Clip.Frames.back().FrameIndex
             << ",\"frameCount\":" << Clip.Frames.size()
             << ",\"sourceAnimationSha256\":\""
             << JsonEscape(Clip.SourceAnimationSha256)
             << "\",\"foundationExportFbx\":\""
             << JsonEscape(Clip.FoundationExportFbxFileName)
             << "\",\"exportFbx\":\""
             << JsonEscape(Clip.ExportFbxFileName)
             << "\",\"sourceTrs\":\""
             << Base64Encode(ToBytes(SourceTransforms))
              << "\",\"fkTrs\":\""
              << Base64Encode(ToBytes(FkTransforms))
              << "\",\"foundationTrs\":\""
              << Base64Encode(ToBytes(FoundationTransforms))
              << "\",\"finalTrs\":\""
             << Base64Encode(ToBytes(FinalTransforms))
             << "\"}";
    }
    Json << "]}";
    return Json.str();
}

std::string BuildViewerHtml(const std::string& Data)
{
    std::ostringstream Html;
    Html << R"HTML(<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SKRTG UE IK Retarget Review</title><style>
:root{color-scheme:light dark;--bg:#ececef;--panel:#f8f8fa;--ink:#25252a;--line:#b8b8c1;--source:#516f8d;--fk:#8b6b45;--ik:#765f8e}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.35 system-ui,sans-serif}header{position:sticky;top:0;z-index:10;padding:10px 14px;background:color-mix(in srgb,var(--panel) 95%,transparent);border-bottom:1px solid var(--line)}h1{font-size:18px;margin:0}.title{display:flex;align-items:end;justify-content:space-between;gap:12px;flex-wrap:wrap}.guard{font:12px ui-monospace,monospace}.controls{display:flex;gap:9px;align-items:center;flex-wrap:wrap;margin-top:8px}.controls label{display:flex;gap:5px;align-items:center}button,select,input,.button{font:inherit}.button{display:inline-block;padding:2px 8px;border:1px solid buttonborder;border-radius:2px;background:buttonface;color:buttontext;text-decoration:none}.matrix{display:grid;grid-template-columns:repeat(4,minmax(300px,1fr));gap:8px;padding:10px;min-width:1280px}.colhead{background:var(--panel);border:1px solid var(--line);padding:8px;font-weight:700;text-align:center}.panel{background:var(--panel);border:1px solid var(--line);padding:6px}.viewport{position:relative;height:410px;background:var(--panel);overflow:hidden}.viewport canvas{position:absolute;inset:0;width:100%;height:100%;display:block}.skeleton-canvas{pointer-events:none}.legend{display:flex;gap:14px;flex-wrap:wrap;padding:0 14px 10px;font-size:12px}.sw{display:inline-block;width:18px;border-top:3px solid;margin-right:5px;vertical-align:middle}.source{border-color:var(--source)}.fk{border-color:var(--fk)}.ik{border-color:var(--ik)}.note{padding:0 14px 16px;max-width:1700px}.readout{font:12px ui-monospace,monospace;white-space:pre-wrap}@media(prefers-color-scheme:dark){:root{--bg:#17171b;--panel:#222228;--ink:#e5e5e9;--line:#4a4a55;--source:#83a8ca;--fk:#d3a96f;--ik:#b19ac9}}</style></head><body><header><div class="title"><h1>SKRTG UE IK Mesh Retarget Review</h1><div class="guard">candidate route only | route_selected=false | route_adopted=false | stage_complete=false | independent review pending</div></div><div class="controls"><button id="play">Play</button><button id="prev">-1</button><button id="next">+1</button><label>frame <input id="frame" type="number" min="1" value="1"></label><input id="seek" type="range" min="0" value="0"><label>speed <select id="speed"><option>.25</option><option>.5</option><option selected>1</option><option>2</option></select></label><label><input id="loop" type="checkbox" checked>loop</label><label><input id="meshToggle" type="checkbox" checked>Mesh</label><label><input id="skeletonToggle" type="checkbox" checked>Skeleton</label><label>animation <select id="clip"></select></label><button id="nextClip">切换动画</button><a id="exportFbx" class="button" download>导出 FBX</a><label>ghost anchor <select id="anchor"></select></label><label>3D yaw <input id="yaw" type="range" min="-180" max="180" value="-28"></label><label>3D pitch <input id="pitch" type="range" min="-80" max="80" value="18"></label><label>display scale <input id="zoom" type="range" min="60" max="180" value="100"></label><span id="readout" class="readout"></span></div></header><main id="matrix" class="matrix"></main><div class="legend"><span><i class="sw source"></i>Original / anchor-aligned source ghost</span><span><i class="sw fk"></i>FK</span><span><i class="sw ik"></i>FK + IK / Final Result</span><span>Original ghost opacity = 50%</span><span>result opacity = 100%</span><span>Mesh uses original FBX geometry and skin clusters; neutral flat shading, no texture claim</span></div><div class="note"><b>Synchronization:</b> all four views use the same selected animation, frame/time, orthographic camera, yaw/pitch, projection and display scale. Original remains in source model space. Both overlay views use the explicitly labelled anchor-aligned source ghost at 50%, with rest-basis rotation, translation alignment and whole-ghost scale 1.0. FK and Final use the target Mesh from the original target T-pose FBX. Final Result contains no source overlay. Non-participating skeleton bones use 30% lane opacity. Source blend shapes are preserved in the FBX source but are not evaluated by this neutral skin-cluster viewer. The export button points to the pre-generated target Mesh + skeleton + Final animation FBX for the selected clip. With one supplied source animation, animation switching is present but disabled until another reviewed clip is added to the manifest.</div><script>const DATA=)HTML";
    Html << Data;
    Html << R"HTML(;const $=id=>document.getElementById(id),columns=[{id:'original',label:'Original',contract:'source_original_only'},{id:'fkOverlay',label:'FK + Original 50%',contract:'anchor_aligned_source_ghost_50_plus_fk_100'},{id:'ikOverlay',label:'FK + IK + Original 50%',contract:'anchor_aligned_source_ghost_50_plus_fk_ik_100'},{id:'final',label:'Final Result',contract:'final_only_no_overlay'}];function bytes64(s){const b=atob(s),u=new Uint8Array(b.length);for(let i=0;i<b.length;i++)u[i]=b.charCodeAt(i);return u}function f32(s){const u=bytes64(s);return new Float32Array(u.buffer,u.byteOffset,u.byteLength/4)}function u32(s){const u=bytes64(s);return new Uint32Array(u.buffer,u.byteOffset,u.byteLength/4)}function hydrateMeshPackage(p){for(const m of p.meshes){m.p=f32(m.p);m.tri=u32(m.tri);m.io=u32(m.io);m.ic=u32(m.ic);m.iw=f32(m.iw);m.cb=u32(m.cb);m.co=f32(m.co)}return p}hydrateMeshPackage(DATA.sourceMesh);hydrateMeshPackage(DATA.targetMesh);for(const c of DATA.clips){c.sourceTrs=f32(c.sourceTrs);c.fkTrs=f32(c.fkTrs);c.finalTrs=f32(c.finalTrs)}const matrix=$('matrix');matrix.innerHTML=columns.map(c=>`<div class="colhead">${c.label}</div>`).join('')+columns.map(c=>`<section class="panel" data-contract="${c.contract}"><div class="viewport"><canvas class="mesh-canvas" data-lane="${c.id}" data-proj="3D"></canvas><canvas class="skeleton-canvas" data-lane="${c.id}" data-proj="3D"></canvas></div></section>`).join('');const meshCanvases=[...document.querySelectorAll('.mesh-canvas')],skeletonCanvases=[...document.querySelectorAll('.skeleton-canvas')],panels=[...document.querySelectorAll('.panel')];for(const [i,c] of DATA.clips.entries())$('clip').add(new Option(c.label,String(i)));for(const [i,a] of DATA.anchors.entries())$('anchor').add(new Option(`${a.label}: ${a.sourcePath} -> ${a.targetPath}`,String(i)));$('nextClip').disabled=DATA.clips.length<2;let clipIndex=0,slot=0,timer=null,bounds=null,radiusCache=new Map(),skinCache=null,ghostCache=null,drawCalls=0,webglErrors=[];function trBase(frame,bones,bone){return(frame*bones+bone)*10}function qrot(qx,qy,qz,qw,x,y,z){const tx=2*(qy*z-qz*y),ty=2*(qz*x-qx*z),tz=2*(qx*y-qy*x);return[x+qw*tx+(qy*tz-qz*ty),y+qw*ty+(qz*tx-qx*tz),z+qw*tz+(qx*ty-qy*tx)]}function applyTr(a,o,p){const x=p[0]*a[o+7],y=p[1]*a[o+8],z=p[2]*a[o+9],r=qrot(a[o+3],a[o+4],a[o+5],a[o+6],x,y,z);return[r[0]+a[o],r[1]+a[o+1],r[2]+a[o+2]]}function applyTrArray(t,p){const x=p[0]*t[7],y=p[1]*t[8],z=p[2]*t[9],r=qrot(t[3],t[4],t[5],t[6],x,y,z);return[r[0]+t[0],r[1]+t[1],r[2]+t[2]]}function pointFromTr(a,o){return[a[o],a[o+1],a[o+2]]}function alignPoint(p,clip,frame,anchor){const so=trBase(frame,DATA.sourceBones.length,anchor.sourceBone),to=trBase(frame,DATA.targetBones.length,anchor.targetBone),s=clip.sourceTrs,t=clip.fkTrs,dx=p[0]-s[so],dy=p[1]-s[so+1],dz=p[2]-s[so+2],local=qrot(-s[so+3],-s[so+4],-s[so+5],s[so+6],dx,dy,dz),basis=qrot(anchor.basis[0],anchor.basis[1],anchor.basis[2],anchor.basis[3],local[0],local[1],local[2]),world=qrot(t[to+3],t[to+4],t[to+5],t[to+6],basis[0],basis[1],basis[2]);return[world[0]+t[to],world[1]+t[to+1],world[2]+t[to+2]]}function skinPackage(pkg,trs,frame,boneCount){return pkg.meshes.map(m=>{const out=new Float32Array(m.p.length);for(let cp=0;cp<m.controlPointCount;cp++){const px=m.p[cp*3],py=m.p[cp*3+1],pz=m.p[cp*3+2],start=m.io[cp],end=m.io[cp+1];let x=0,y=0,z=0,sum=0;if(start===end){const p=applyTrArray(m.fallback,[px,py,pz]);x=p[0];y=p[1];z=p[2]}else{for(let j=start;j<end;j++){const ci=m.ic[j],w=m.iw[j],bo=ci*10,bone=m.cb[ci],co=trBase(frame,boneCount,bone),bound=applyTr(m.co,bo,[px,py,pz]),world=applyTr(trs,co,bound);x+=world[0]*w;y+=world[1]*w;z+=world[2]*w;sum+=w}if(m.skinMode==='normalize'&&sum>1e-8){x/=sum;y/=sum;z/=sum}else if(m.skinMode==='total_one'&&sum<1){const p=applyTrArray(m.fallback,[px,py,pz]),left=Math.max(0,1-sum);x+=p[0]*left;y+=p[1]*left;z+=p[2]*left}}out[cp*3]=x;out[cp*3+1]=y;out[cp*3+2]=z}return out})}function currentClip(){return DATA.clips[clipIndex]}function currentAnchor(){return DATA.anchors[Number($('anchor').value)||0]}function ensureSkin(){if(skinCache&&skinCache.clip===clipIndex&&skinCache.slot===slot)return skinCache;const c=currentClip();skinCache={clip:clipIndex,slot,source:skinPackage(DATA.sourceMesh,c.sourceTrs,slot,DATA.sourceBones.length),fk:skinPackage(DATA.targetMesh,c.fkTrs,slot,DATA.targetBones.length),final:skinPackage(DATA.targetMesh,c.finalTrs,slot,DATA.targetBones.length)};ghostCache=null;return skinCache}function ensureGhost(){const a=Number($('anchor').value)||0;if(ghostCache&&ghostCache.clip===clipIndex&&ghostCache.slot===slot&&ghostCache.anchor===a)return ghostCache.positions;const c=currentClip(),anchor=currentAnchor(),src=ensureSkin().source;const positions=src.map(v=>{const out=new Float32Array(v.length);for(let i=0;i<v.length;i+=3){const p=alignPoint([v[i],v[i+1],v[i+2]],c,slot,anchor);out[i]=p[0];out[i+1]=p[1];out[i+2]=p[2]}return out});ghostCache={clip:clipIndex,slot,anchor:a,positions};return positions}function frameEnvelope(frame){const c=currentClip(),a=currentAnchor(),lo=[Infinity,Infinity,Infinity],hi=[-Infinity,-Infinity,-Infinity],visit=p=>{for(let k=0;k<3;k++){lo[k]=Math.min(lo[k],p[k]);hi[k]=Math.max(hi[k],p[k])}};for(let i=0;i<DATA.sourceBones.length;i++)if(DATA.sourceBones[i][3]){const so=trBase(frame,DATA.sourceBones.length,i),p=pointFromTr(c.sourceTrs,so);visit(p);visit(alignPoint(p,c,frame,a))}for(let i=0;i<DATA.targetBones.length;i++)if(DATA.targetBones[i][3]){visit(pointFromTr(c.fkTrs,trBase(frame,DATA.targetBones.length,i)));visit(pointFromTr(c.finalTrs,trBase(frame,DATA.targetBones.length,i)))}const center=lo.map((v,k)=>(v+hi[k])/2),r=Math.max(...lo.map((v,k)=>(hi[k]-v)/2),1)*1.24;return{center,r}}function computeBounds(){const key=`${clipIndex}:${$('anchor').value}`;let radius=radiusCache.get(key);if(radius===undefined){radius=0;for(let i=0;i<currentClip().frameCount;i++)radius=Math.max(radius,frameEnvelope(i).r);radiusCache.set(key,radius)}bounds={center:frameEnvelope(slot).center,radius}}function viewMatrix(w,h){if(!bounds)computeBounds();const yaw=Number($('yaw').value)*Math.PI/180,pitch=Number($('pitch').value)*Math.PI/180,ca=Math.cos(yaw),sa=Math.sin(yaw),cb=Math.cos(pitch),sb=Math.sin(pitch),min=Math.min(w,h),zoom=Number($('zoom').value)/100,sx=min/w/bounds.radius*zoom,sy=min/h/bounds.radius*zoom,sz=.25/bounds.radius,c=bounds.center,m=new Float32Array(16);m[0]=ca*sx;m[4]=0;m[8]=sa*sx;m[12]=-(ca*c[0]+sa*c[2])*sx;m[1]=sb*sa*sy;m[5]=cb*sy;m[9]=-sb*ca*sy;m[13]=-(sb*sa*c[0]+cb*c[1]-sb*ca*c[2])*sy;m[2]=-cb*sa*sz;m[6]=sb*sz;m[10]=cb*ca*sz;m[14]=-(-cb*sa*c[0]+sb*c[1]+cb*ca*c[2])*sz;m[15]=1;return m}class Renderer{constructor(canvas){this.canvas=canvas;this.gl=canvas.getContext('webgl2',{alpha:true,antialias:true,preserveDrawingBuffer:true});if(!this.gl){webglErrors.push(`WebGL2 unavailable: ${canvas.dataset.lane}`);return}const gl=this.gl,vs=`#version 300 es\nin vec3 aPosition;uniform mat4 uViewProj;out vec3 vWorld;void main(){vWorld=aPosition;gl_Position=uViewProj*vec4(aPosition,1.0);}`,fs=`#version 300 es\nprecision highp float;in vec3 vWorld;uniform vec4 uColor;out vec4 outColor;void main(){vec3 n=normalize(cross(dFdx(vWorld),dFdy(vWorld)));float l=.35+.65*abs(dot(n,normalize(vec3(.38,.72,.58))));outColor=vec4(uColor.rgb*l,uColor.a);}`;const compile=(type,src)=>{const s=gl.createShader(type);gl.shaderSource(s,src);gl.compileShader(s);if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw new Error(gl.getShaderInfoLog(s));return s};try{this.program=gl.createProgram();gl.attachShader(this.program,compile(gl.VERTEX_SHADER,vs));gl.attachShader(this.program,compile(gl.FRAGMENT_SHADER,fs));gl.linkProgram(this.program);if(!gl.getProgramParameter(this.program,gl.LINK_STATUS))throw new Error(gl.getProgramInfoLog(this.program));this.position=gl.getAttribLocation(this.program,'aPosition');this.view=gl.getUniformLocation(this.program,'uViewProj');this.color=gl.getUniformLocation(this.program,'uColor');this.resources=new Map()}catch(e){webglErrors.push(String(e));this.gl=null}}resize(){if(!this.gl)return;const d=devicePixelRatio||1,w=Math.max(320,Math.round(this.canvas.clientWidth*d)),h=Math.max(280,Math.round(this.canvas.clientHeight*d));if(this.canvas.width!==w||this.canvas.height!==h){this.canvas.width=w;this.canvas.height=h}this.gl.viewport(0,0,w,h)}resourcesFor(pkg){if(this.resources.has(pkg))return this.resources.get(pkg);const gl=this.gl,r=pkg.meshes.map(m=>{const pos=gl.createBuffer(),idx=gl.createBuffer();gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,idx);gl.bufferData(gl.ELEMENT_ARRAY_BUFFER,m.tri,gl.STATIC_DRAW);return{pos,idx,count:m.tri.length}});this.resources.set(pkg,r);return r}clear(){if(!this.gl)return;this.resize();const gl=this.gl;gl.clearColor(0,0,0,0);gl.clearDepth(1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT)}draw(pkg,positions,color,alpha,xray=false){if(!this.gl)return;const gl=this.gl,res=this.resourcesFor(pkg);gl.useProgram(this.program);gl.uniformMatrix4fv(this.view,false,viewMatrix(this.canvas.width,this.canvas.height));gl.uniform4f(this.color,color[0],color[1],color[2],alpha);gl.enable(gl.BLEND);gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);if(xray){gl.disable(gl.DEPTH_TEST);gl.depthMask(false)}else{gl.enable(gl.DEPTH_TEST);gl.depthMask(true)}gl.disable(gl.CULL_FACE);for(let i=0;i<pkg.meshes.length;i++){gl.bindBuffer(gl.ARRAY_BUFFER,res[i].pos);gl.bufferData(gl.ARRAY_BUFFER,positions[i],gl.DYNAMIC_DRAW);gl.enableVertexAttribArray(this.position);gl.vertexAttribPointer(this.position,3,gl.FLOAT,false,0,0);gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER,res[i].idx);gl.drawElements(gl.TRIANGLES,res[i].count,gl.UNSIGNED_INT,0);drawCalls++}gl.depthMask(true)}}const renderers=meshCanvases.map(c=>new Renderer(c));function project(canvas,p){if(!bounds)computeBounds();const d=devicePixelRatio||1,w=Math.max(320,Math.round(canvas.clientWidth*d)),h=Math.max(280,Math.round(canvas.clientHeight*d));if(canvas.width!==w||canvas.height!==h){canvas.width=w;canvas.height=h}const yaw=Number($('yaw').value)*Math.PI/180,pitch=Number($('pitch').value)*Math.PI/180,ca=Math.cos(yaw),sa=Math.sin(yaw),cb=Math.cos(pitch),sb=Math.sin(pitch),x=p[0]-bounds.center[0],y=p[1]-bounds.center[1],z=p[2]-bounds.center[2],x1=ca*x+sa*z,z1=-sa*x+ca*z,y1=cb*y-sb*z1,scale=Math.min(w,h)/(2*bounds.radius)*Number($('zoom').value)/100;return[w/2+x1*scale,h/2-y1*scale]}function bonePoint(trs,bones,bone){return pointFromTr(trs,trBase(slot,bones.length,bone))}function drawSkeleton(canvas,lane){const ctx=canvas.getContext('2d'),d=devicePixelRatio||1,w=canvas.width,h=canvas.height,style=getComputedStyle(document.documentElement),source=style.getPropertyValue('--source'),fk=style.getPropertyValue('--fk'),ik=style.getPropertyValue('--ik'),c=currentClip(),anchor=currentAnchor();ctx.clearRect(0,0,w,h);if(!$('skeletonToggle').checked)return;const line=(bones,point,color,alpha)=>{for(let i=0;i<bones.length;i++){const parent=bones[i][0];if(parent<0)continue;const a=project(canvas,point(parent)),b=project(canvas,point(i)),active=bones[i][3];ctx.globalAlpha=alpha*(active?1:.3);ctx.strokeStyle=color;ctx.lineWidth=(active?2:1)*d;ctx.beginPath();ctx.moveTo(a[0],a[1]);ctx.lineTo(b[0],b[1]);ctx.stroke()}ctx.globalAlpha=1};const sourcePoint=i=>bonePoint(c.sourceTrs,DATA.sourceBones,i),ghostPoint=i=>alignPoint(sourcePoint(i),c,slot,anchor),fkPoint=i=>bonePoint(c.fkTrs,DATA.targetBones,i),finalPoint=i=>bonePoint(c.finalTrs,DATA.targetBones,i);if(lane==='original')line(DATA.sourceBones,sourcePoint,source,1);else if(lane==='fkOverlay'){line(DATA.sourceBones,ghostPoint,source,.5);line(DATA.targetBones,fkPoint,fk,1)}else if(lane==='ikOverlay'){line(DATA.sourceBones,ghostPoint,source,.5);line(DATA.targetBones,finalPoint,ik,1)}else line(DATA.targetBones,finalPoint,ik,1);ctx.globalAlpha=1;ctx.fillStyle=style.getPropertyValue('--ink');ctx.font=`${11*d}px ui-monospace,monospace`;ctx.fillText(`3D orthographic | frame ${c.startFrame+slot} | ${(slot/c.fps+c.startFrame/c.fps).toFixed(3)} s`,8*d,15*d)}function render(){if(!bounds)computeBounds();drawCalls=0;const meshOn=$('meshToggle').checked,skin=meshOn?ensureSkin():null,ghost=meshOn?ensureGhost():null;for(let i=0;i<renderers.length;i++){const lane=meshCanvases[i].dataset.lane,r=renderers[i];r.clear();if(meshOn&&r.gl){if(lane==='original')r.draw(DATA.sourceMesh,skin.source,[.32,.44,.55],1);else if(lane==='fkOverlay'){r.draw(DATA.targetMesh,skin.fk,[.55,.42,.27],1);r.draw(DATA.sourceMesh,ghost,[.32,.44,.55],.5,true)}else if(lane==='ikOverlay'){r.draw(DATA.targetMesh,skin.final,[.46,.37,.56],1);r.draw(DATA.sourceMesh,ghost,[.32,.44,.55],.5,true)}else r.draw(DATA.targetMesh,skin.final,[.46,.37,.56],1)}}for(const c of skeletonCanvases)drawSkeleton(c,c.dataset.lane);const clip=currentClip();$('seek').value=slot;$('frame').value=clip.startFrame+slot;$('exportFbx').href=encodeURI(clip.exportFbx);$('readout').textContent=`${clip.label} | ${slot+1}/${clip.frameCount} | ${(clip.startFrame+slot)/clip.fps.toFixed?.(3)??''} | ${clip.fps} fps | Mesh=${meshOn?'on':'off'} | Skeleton=${$('skeletonToggle').checked?'on':'off'} | ghost=${currentAnchor().label} | clips=${DATA.clips.length}`;window.__d117bReady=true}function configureClip(){const c=currentClip();slot=0;$('frame').min=c.startFrame;$('frame').max=c.stopFrame;$('seek').max=c.frameCount-1;$('clip').value=String(clipIndex);skinCache=null;ghostCache=null;bounds=null;stop();render()}function setSlot(v){slot=Math.max(0,Math.min(currentClip().frameCount-1,Number(v)));bounds=null;render()}function stop(){if(timer){clearInterval(timer);timer=null;$('play').textContent='Play'}}function start(){stop();$('play').textContent='Pause';timer=setInterval(()=>{if(slot===currentClip().frameCount-1){if($('loop').checked)setSlot(0);else stop()}else setSlot(slot+1)},1000/(currentClip().fps*Number($('speed').value)))}$('play').onclick=()=>timer?stop():start();$('prev').onclick=()=>{stop();setSlot(slot-1)};$('next').onclick=()=>{stop();setSlot(slot+1)};$('seek').oninput=e=>{stop();setSlot(e.target.value)};$('frame').onchange=e=>{stop();setSlot(Number(e.target.value)-currentClip().startFrame)};$('speed').onchange=()=>{if(timer)start()};$('meshToggle').onchange=render;$('skeletonToggle').onchange=render;$('clip').onchange=e=>{clipIndex=Number(e.target.value);configureClip()};$('nextClip').onclick=()=>{if(DATA.clips.length>1){clipIndex=(clipIndex+1)%DATA.clips.length;configureClip()}};$('anchor').onchange=()=>{ghostCache=null;bounds=null;render()};for(const id of ['yaw','pitch','zoom'])$(id).oninput=render;addEventListener('resize',()=>{bounds=null;render()});window.__d117bQa={frameCount:()=>currentClip().frameCount,sourceBoneCount:DATA.sourceBones.length,targetBoneCount:DATA.targetBones.length,sourceMeshCount:DATA.sourceMesh.meshCount,targetMeshCount:DATA.targetMesh.meshCount,sourceControlPointCount:DATA.sourceMesh.controlPointCount,targetControlPointCount:DATA.targetMesh.controlPointCount,sourceTriangleCount:DATA.sourceMesh.triangleCount,targetTriangleCount:DATA.targetMesh.triangleCount,clipCount:DATA.clips.length,meshCanvasCount:meshCanvases.length,skeletonCanvasCount:skeletonCanvases.length,panelCount:panels.length,laneContracts:columns.map(c=>c.contract),webglErrors,drawCalls:()=>drawCalls,setFrame:n=>setSlot(n-currentClip().startFrame),setAnchor:n=>{$('anchor').value=String(n);ghostCache=null;bounds=null;render()},setMesh:v=>{$('meshToggle').checked=!!v;render()},setSkeleton:v=>{$('skeletonToggle').checked=!!v;render()},switchAnimation:()=>{$('nextClip').click()},camera:()=>({center:[...bounds.center],radius:bounds.radius}),exportHref:()=>$('exportFbx').getAttribute('href'),play:start,pause:stop};configureClip();</script></body></html>)HTML";
    std::string Result = Html.str();
    const auto ReplaceAll =
        [&Result](const std::string& From,
                  const std::string& To)
    {
        std::size_t Position = 0;
        while ((Position = Result.find(
                    From, Position)) != std::string::npos)
        {
            Result.replace(Position, From.size(), To);
            Position += To.size();
        }
    };
    if (Data.find(
            "\"spinePelvisFollowCandidateEnabled\":true") !=
        std::string::npos)
    {
        ReplaceAll(
            "candidate route only | route_selected=false | route_adopted=false | stage_complete=false | independent review pending",
            "upstream Limb IK selected=true adopted=true | Spine/Pelvis candidate enabled=true route_selected=false route_adopted=false | stage_complete=false | independent review pending");
    }
    ReplaceAll(
        "${(clip.startFrame+slot)/clip.fps.toFixed?.(3)??''}",
        "${((clip.startFrame+slot)/clip.fps).toFixed(3)} s");
    ReplaceAll(
        "function pointFromTr",
        "function applyAffine(a,o,p){const x=p[0],y=p[1],z=p[2];return[a[o]+a[o+3]*x+a[o+6]*y+a[o+9]*z,a[o+1]+a[o+4]*x+a[o+7]*y+a[o+10]*z,a[o+2]+a[o+5]*x+a[o+8]*y+a[o+11]*z]}function pointFromTr");
    ReplaceAll("bo=ci*10", "bo=ci*12");
    ReplaceAll(
        "bound=applyTr(m.co,bo,[px,py,pz])",
        "bound=applyAffine(m.co,bo,[px,py,pz])");
    ReplaceAll(
        "applyTrArray(m.fallback,[px,py,pz])",
        "applyAffine(m.fallback,0,[px,py,pz])");
    ReplaceAll(
        "FK + Original 50%",
        "FK + Original 10%");
    ReplaceAll(
        "FK + IK + Original 50%",
        "FK + IK + Original 10%");
    ReplaceAll(
        "source_ghost_50_plus",
        "source_ghost_10_plus");
    ReplaceAll(
        "Original ghost opacity = 50%",
        "Original ghost opacity = 10%");
    ReplaceAll(
        "source ghost at 50%",
        "source ghost at 10%");
    ReplaceAll(
        "hydrateMeshPackage(DATA.sourceMesh);",
        "for(const p of DATA.sourceMeshes)hydrateMeshPackage(p);");
    ReplaceAll(
        "function currentClip(){return DATA.clips[clipIndex]}",
        "function currentClip(){return DATA.clips[clipIndex]}function currentSourceMesh(){return DATA.sourceMeshes[currentClip().sourceMeshIndex]}");
    ReplaceAll(
        "source:skinPackage(DATA.sourceMesh,c.sourceTrs",
        "source:skinPackage(currentSourceMesh(),c.sourceTrs");
    ReplaceAll(
        "function render(){if(!bounds)computeBounds();drawCalls=0;const meshOn=",
        "function render(){if(!bounds)computeBounds();drawCalls=0;const sourcePkg=currentSourceMesh(),meshOn=");
    ReplaceAll(
        "r.draw(DATA.sourceMesh,",
        "r.draw(sourcePkg,");
    ReplaceAll(
        "sourceMeshCount:DATA.sourceMesh.meshCount,targetMeshCount:",
        "sourceMeshCount:currentSourceMesh().meshCount,currentSourceMeshCount:()=>currentSourceMesh().meshCount,targetMeshCount:");
    ReplaceAll(
        "sourceControlPointCount:DATA.sourceMesh.controlPointCount,targetControlPointCount:",
        "sourceControlPointCount:currentSourceMesh().controlPointCount,currentSourceControlPointCount:()=>currentSourceMesh().controlPointCount,targetControlPointCount:");
    ReplaceAll(
        "sourceTriangleCount:DATA.sourceMesh.triangleCount,targetTriangleCount:",
        "sourceTriangleCount:currentSourceMesh().triangleCount,currentSourceTriangleCount:()=>currentSourceMesh().triangleCount,targetTriangleCount:");
    ReplaceAll(
        "Mesh uses original FBX geometry and skin clusters; neutral flat shading, no texture claim",
        "Original uses the hash-bound source character Mesh; meshless motion-only clips use display-only parent-current x T-pose-local-rest passthrough for missing bones; neutral flat shading, no texture claim");
    ReplaceAll(
        "Source blend shapes are preserved in the FBX source but are not evaluated by this neutral skin-cluster viewer.",
        "The three added motion FBXs are meshless; their present source bones remain direct samples, while missing display-only bones use parent-current x T-pose-local-rest passthrough and are never treated as FK/IK evidence. Blend shapes are not evaluated by this neutral skin-cluster viewer.");
    ReplaceAll(
        "| clips=${DATA.clips.length}`;",
        "| clips=${DATA.clips.length} | source direct/pass=${clip.sourceDirectBoneCount}/${clip.sourceRestPassthroughBoneCount} | sharedMesh=${clip.sourceMeshFallbackUsed?'yes':'no'}`;");
    ReplaceAll(
        "| sharedMesh=${clip.sourceMeshFallbackUsed?'yes':'no'}`;",
        "| sharedMesh=${clip.sourceMeshFallbackUsed?'yes':'no'} | limbIK=${clip.limbIkStatus} families=${clip.limbIkCommittedFamilyTransactions}/${clip.limbIkFamilyTransactions} rollback=${clip.limbIkRolledBackFamilyTransactions} shadow=${clip.limbIkUnitScaleShadowProjectionApplied?'yes':'bypass'} endpoint=${clip.limbIkMaximumEndpointErrorCm.toExponential(2)}cm`;");
    ReplaceAll(
        "exportHref:()=>$('exportFbx').getAttribute('href'),play:start",
        "exportHref:()=>$('exportFbx').getAttribute('href'),sourceDisplayContract:()=>({direct:currentClip().sourceDirectBoneCount,passthrough:currentClip().sourceRestPassthroughBoneCount,sharedMesh:currentClip().sourceMeshFallbackUsed}),limbIkStatus:()=>({status:currentClip().limbIkStatus,families:currentClip().limbIkFamilyTransactions,committed:currentClip().limbIkCommittedFamilyTransactions,rolledBack:currentClip().limbIkRolledBackFamilyTransactions,appliedChains:currentClip().limbIkAppliedChainRecords,failClosedChains:currentClip().limbIkFailClosedChainRecords,shadowProjection:currentClip().limbIkUnitScaleShadowProjectionApplied}),play:start");
    ReplaceAll(
        "With one supplied source animation, animation switching is present but disabled until another reviewed clip is added to the manifest.",
        "The animation control switches among all supplied reviewed clips; it is disabled only when the manifest contains one clip.");
    ReplaceAll(
        "FK and Final use the target Mesh from the original target T-pose FBX. Final Result contains no source overlay.",
        "FK and Final use the target Mesh from the original target T-pose FBX. All 51 mapped non-root target local translations remain at target T-pose rest (21 body + 30 finger), while Hips keeps animated root motion. FK + IK and Final include the existing four-limb Two-Bone solve plus ten independent hand-anchor-local finger IK solves using source-rest-normalized segment lengths and explicit distal orientation. Final Result contains no source overlay.");
    ReplaceAll(
        "ghostPoint,source,.5",
        "ghostPoint,source,.1");
    ReplaceAll(
        "ghost,[.32,.44,.55],.5,true",
        "ghost,[.32,.44,.55],.1,true");
    ReplaceAll(
        "window.__d117bQa={",
        "window.__d117bQa={sourceGhostOpacity:.1,");
    const std::string TposeContractCss = R"TPOSECSS(
.tpose-contract{margin:0 10px 12px;padding:14px;background:var(--panel);border:1px solid var(--line)}.contract-title{display:flex;align-items:end;justify-content:space-between;gap:16px;flex-wrap:wrap;margin-bottom:10px}.contract-title h2{font-size:17px;margin:0}.contract-title p{margin:0;color:color-mix(in srgb,var(--ink) 68%,transparent);font:12px ui-monospace,monospace}.contract-grid{display:grid;grid-template-columns:repeat(2,minmax(540px,1fr));gap:12px}.contract-card{border:1px solid var(--line);background:color-mix(in srgb,var(--panel) 94%,var(--bg));padding:10px;min-width:0}.contract-card h3{font-size:14px;margin:0 0 6px}.tpose-svg{display:block;width:100%;height:440px;border:1px solid color-mix(in srgb,var(--line) 78%,transparent);background:color-mix(in srgb,var(--panel) 85%,var(--bg))}.contract-context{stroke:color-mix(in srgb,var(--ink) 24%,transparent);stroke-width:1.1;fill:none}.contract-joint{fill:color-mix(in srgb,var(--ink) 40%,transparent)}.contract-chain{fill:none;stroke-width:3;stroke-linecap:round;stroke-linejoin:round}.chain-spine{stroke:#50a7d9}.chain-arm{stroke:#e3a54f}.chain-leg{stroke:#6fc487}.chain-finger{stroke:#c48ee8}.chain-other{stroke:#9aa1ac}.contract-goal{fill:#ffcf4d;stroke:#17171b;stroke-width:1.5}.contract-pole{fill:#ef7272;stroke:#17171b;stroke-width:1.3}.contract-anchor{fill:#5ed3d0;stroke:#17171b;stroke-width:1.3}.contract-root{fill:#ffffff;stroke:#ef7272;stroke-width:3}.contract-pelvis{fill:#ffffff;stroke:#50a7d9;stroke-width:3}.contract-role-line{stroke:color-mix(in srgb,var(--ink) 55%,transparent);stroke-width:1;stroke-dasharray:3 2}.contract-label{fill:var(--ink);font:11px ui-monospace,monospace;paint-order:stroke;stroke:var(--panel);stroke-width:3px;stroke-linejoin:round}.contract-marker-letter{fill:#17171b;font:bold 9px ui-monospace,monospace;text-anchor:middle;dominant-baseline:central}.contract-summary{margin-top:7px;font:12px ui-monospace,monospace;white-space:pre-wrap}.contract-legend{display:flex;gap:12px;flex-wrap:wrap;margin:10px 0 0;font-size:12px}.contract-key{display:inline-flex;align-items:center;gap:5px}.contract-dot{display:inline-grid;place-items:center;width:18px;height:18px;border-radius:50%;color:#17171b;font:bold 9px ui-monospace,monospace}.contract-goal-key{background:#ffcf4d}.contract-pole-key{background:#ef7272;transform:rotate(45deg)}.contract-pole-key span{transform:rotate(-45deg)}.contract-anchor-key{background:#5ed3d0;clip-path:polygon(50% 0,100% 100%,0 100%)}.chain-map{display:grid;grid-template-columns:repeat(3,minmax(300px,1fr));gap:6px;margin-top:10px}.chain-row{border:1px solid color-mix(in srgb,var(--line) 72%,transparent);padding:6px 8px;font:11px/1.35 ui-monospace,monospace;overflow-wrap:anywhere}.chain-row strong{font-size:12px}.ik-badge{display:inline-block;margin-left:5px;padding:0 4px;border:1px solid currentColor;border-radius:8px;font-size:10px}@media(max-width:1180px){.contract-grid{grid-template-columns:1fr}.chain-map{grid-template-columns:1fr 1fr}}
)TPOSECSS";
    ReplaceAll(
        ".readout{font:12px ui-monospace,monospace;white-space:pre-wrap}",
        ".readout{font:12px ui-monospace,monospace;white-space:pre-wrap}" +
            TposeContractCss);
    const std::string TposeContractHtml = R"TPOSEHTML(<section id="tposeContract" class="tpose-contract" data-static-contract="true"><div class="contract-title"><h2>Source / Target T-pose Retarget Contract</h2><p>static rest pose | shared centimeter scale | independent of animation, frame, overlay and camera</p></div><div class="contract-grid"><article class="contract-card"><h3>Source T-pose - Source Root and Source Pelvis are separate inputs</h3><svg id="sourceTpose" class="tpose-svg" viewBox="0 0 620 440" role="img" aria-label="Source T-pose retarget chains, IK Goals, poles, anchors, Root and Pelvis"></svg><div id="sourceContractSummary" class="contract-summary"></div></article><article class="contract-card"><h3>Target T-pose - Hips owns Root T/S plus Pelvis model rotation</h3><svg id="targetTpose" class="tpose-svg" viewBox="0 0 620 440" role="img" aria-label="Target T-pose retarget chains, IK Goals, poles, anchors and Hips ownership"></svg><div id="targetContractSummary" class="contract-summary"></div></article></div><div class="contract-legend"><span class="contract-key"><i class="contract-dot contract-goal-key">G</i>IK Goal</span><span class="contract-key"><i class="contract-dot contract-pole-key"><span>P</span></i>pole</span><span class="contract-key"><i class="contract-dot contract-anchor-key">A</i>anchor-local family</span><span class="contract-key"><i class="contract-dot" style="border:3px solid #ef7272;background:#fff">R</i>Root translation / scale</span><span class="contract-key"><i class="contract-dot" style="border:3px solid #50a7d9;background:#fff">P</i>Pelvis model rotation</span><span>chain colors: spine / arm / leg / finger / other</span></div><div id="chainMap" class="chain-map" aria-label="Mapped source to target chain inventory"></div></section>)TPOSEHTML";
    ReplaceAll(
        "<main id=\"matrix\" class=\"matrix\"></main><div class=\"legend\">",
        "<main id=\"matrix\" class=\"matrix\"></main>" +
            TposeContractHtml + "<div class=\"legend\">");
    const std::string TposeContractJavascript = R"TPOSEJS(
const SVG_NS='http://www.w3.org/2000/svg';
function svgNode(tag,attrs={},textValue=''){const n=document.createElementNS(SVG_NS,tag);for(const [k,v] of Object.entries(attrs))n.setAttribute(k,String(v));if(textValue)n.textContent=textValue;return n}
function contractChainClass(label){const s=label.toLowerCase();if(/spine|neck|head/.test(s))return'chain-spine';if(/arm|hand|clavicle/.test(s))return'chain-arm';if(/leg|foot|toe/.test(s))return'chain-leg';if(/thumb|index|middle|ring|pinky/.test(s))return'chain-finger';return'chain-other'}
function contractIndexSet(side){const bones=side==='source'?DATA.sourceBones:DATA.targetBones,key=side==='source'?'source':'target',goalKey=side==='source'?'sourceGoalBone':'targetGoalBone',poleKey=side==='source'?'sourcePoleBone':'targetPoleBone',set=new Set();for(const c of DATA.retargetChains){for(const i of c[key])set.add(i);if(c[goalKey]>=0)set.add(c[goalKey]);if(c[poleKey]>=0)set.add(c[poleKey])}for(const a of DATA.anchors)set.add(side==='source'?a.sourceBone:a.targetBone);const rp=DATA.rootPelvis;set.add(side==='source'?rp.sourceRoot:rp.targetHips);set.add(side==='source'?rp.sourcePelvis:rp.targetHips);for(const i of [...set]){let p=bones[i]?.[0]??-1;while(p>=0){set.add(p);p=bones[p]?.[0]??-1}}return set}
function contractBounds(bones,set){const pts=[...set].map(i=>bones[i]).filter(Boolean);const xs=pts.map(b=>b[4]),ys=pts.map(b=>b[5]);return{minX:Math.min(...xs),maxX:Math.max(...xs),minY:Math.min(...ys),maxY:Math.max(...ys)}}
const sourceContractSet=contractIndexSet('source'),targetContractSet=contractIndexSet('target'),sourceContractBounds=contractBounds(DATA.sourceBones,sourceContractSet),targetContractBounds=contractBounds(DATA.targetBones,targetContractSet),contractSpanX=Math.max(sourceContractBounds.maxX-sourceContractBounds.minX,targetContractBounds.maxX-targetContractBounds.minX,1),contractSpanY=Math.max(sourceContractBounds.maxY-sourceContractBounds.minY,targetContractBounds.maxY-targetContractBounds.minY,1),contractCmScale=Math.min(550/contractSpanX,370/contractSpanY);
function contractProject(bones,bounds,index){const b=bones[index],cx=(bounds.minX+bounds.maxX)/2,cy=(bounds.minY+bounds.maxY)/2;return[310+(b[4]-cx)*contractCmScale,220-(b[5]-cy)*contractCmScale]}
function contractTitle(node,textValue){node.appendChild(svgNode('title',{},textValue));return node}
function contractMarker(svg,kind,p,label,titleText,offsetX=0,offsetY=0){const x=p[0]+offsetX,y=p[1]+offsetY,g=svgNode('g',{'data-marker':kind,'data-label':label});if(offsetX||offsetY)g.appendChild(svgNode('line',{x1:p[0],y1:p[1],x2:x,y2:y,class:'contract-role-line'}));if(kind==='goal'){g.appendChild(contractTitle(svgNode('circle',{cx:x,cy:y,r:7,class:'contract-goal'}),titleText));g.appendChild(svgNode('text',{x,y,class:'contract-marker-letter'},'G'))}else if(kind==='pole'){g.appendChild(contractTitle(svgNode('path',{d:`M ${x} ${y-7} L ${x+7} ${y} L ${x} ${y+7} L ${x-7} ${y} Z`,class:'contract-pole'}),titleText));g.appendChild(svgNode('text',{x,y,class:'contract-marker-letter'},'P'))}else if(kind==='anchor'){g.appendChild(contractTitle(svgNode('path',{d:`M ${x} ${y-8} L ${x+8} ${y+7} L ${x-8} ${y+7} Z`,class:'contract-anchor'}),titleText));g.appendChild(svgNode('text',{x,y:y+2,class:'contract-marker-letter'},'A'))}svg.appendChild(g);return g}
    function renderContractSide(side){const source=side==='source',bones=source?DATA.sourceBones:DATA.targetBones,set=source?sourceContractSet:targetContractSet,bounds=source?sourceContractBounds:targetContractBounds,svg=$(source?'sourceTpose':'targetTpose'),point=i=>contractProject(bones,bounds,i);svg.replaceChildren();const context=svgNode('g',{'data-layer':'context'});for(const i of set){const parent=bones[i][0];if(parent<0||!set.has(parent))continue;const a=point(parent),b=point(i);context.appendChild(svgNode('line',{x1:a[0],y1:a[1],x2:b[0],y2:b[1],class:'contract-context'}))}for(const i of set){const p=point(i);context.appendChild(svgNode('circle',{cx:p[0],cy:p[1],r:2.1,class:'contract-joint'}))}svg.appendChild(context);const key=source?'source':'target';for(const c of DATA.retargetChains){const pts=c[key].map(point),poly=svgNode('polyline',{points:pts.map(p=>p.join(',')).join(' '),class:`contract-chain ${contractChainClass(c.label)}`,'data-chain':c.label,'data-ik-mode':c.ikMode});contractTitle(poly,`${c.label}: ${c[key].map(i=>bones[i][1]).join(' -> ')} | ${c.ikMode}`);svg.appendChild(poly)}const goalKey=source?'sourceGoalBone':'targetGoalBone',goalNameKey=source?'sourceGoalName':'targetGoalName',poleKey=source?'sourcePoleBone':'targetPoleBone';for(const c of DATA.retargetChains.filter(c=>c.ikMode!=='fk_only')){const gp=point(c[goalKey]);contractMarker(svg,'goal',gp,c.label,`${c[goalNameKey]} | ${bones[c[goalKey]][2]}`);const pp=point(c[poleKey]),chainRoot=point(c[key][0]),dx=gp[0]-chainRoot[0],dy=gp[1]-chainRoot[1],len=Math.hypot(dx,dy)||1,sign=c.label.toLowerCase().includes('left')?-1:1;contractMarker(svg,'pole',pp,c.label,`${c.label} pole | ${bones[c[poleKey]][2]}`,sign*(-dy/len)*12,sign*(dx/len)*12)}for(const [i,a] of DATA.anchors.entries()){const index=source?a.sourceBone:a.targetBone;contractMarker(svg,'anchor',point(index),a.label,`${a.label} anchor | ${source?a.sourcePath:a.targetPath}`,i?12:-12,-12)}const rp=DATA.rootPelvis;if(source){const root=point(rp.sourceRoot),pelvis=point(rp.sourcePelvis);const rg=svgNode('g',{'data-marker':'root','data-bone':rp.sourceRoot});rg.appendChild(svgNode('circle',{cx:root[0],cy:root[1],r:10,class:'contract-root'}));rg.appendChild(svgNode('text',{x:root[0],y:root[1],class:'contract-marker-letter'},'R'));rg.appendChild(svgNode('text',{x:root[0]+13,y:root[1]+4,class:'contract-label'},'Source Root - T / S'));svg.appendChild(rg);const pg=svgNode('g',{'data-marker':'pelvis','data-bone':rp.sourcePelvis});pg.appendChild(svgNode('circle',{cx:pelvis[0],cy:pelvis[1],r:10,class:'contract-pelvis'}));pg.appendChild(svgNode('text',{x:pelvis[0],y:pelvis[1],class:'contract-marker-letter'},'P'));pg.appendChild(svgNode('text',{x:pelvis[0]+13,y:pelvis[1]+4,class:'contract-label'},'Source Pelvis - model R'));svg.appendChild(pg)}else{const hips=point(rp.targetHips),g=svgNode('g',{'data-marker':'hips','data-bone':rp.targetHips});g.appendChild(svgNode('circle',{cx:hips[0],cy:hips[1],r:11,class:'contract-root'}));g.appendChild(svgNode('circle',{cx:hips[0],cy:hips[1],r:7,class:'contract-pelvis'}));g.appendChild(svgNode('text',{x:hips[0],y:hips[1],class:'contract-marker-letter'},'H'));g.appendChild(svgNode('text',{x:hips[0]+14,y:hips[1]+4,class:'contract-label'},'Hips - Root T/S + Pelvis model R'));svg.appendChild(g)}svg.appendChild(svgNode('text',{x:12,y:20,class:'contract-label'},`shared display scale: ${contractCmScale.toFixed(3)} px/cm`));const goalCount=DATA.retargetChains.filter(c=>c.ikMode!=='fk_only').length,$summary=$(source?'sourceContractSummary':'targetContractSummary');$summary.textContent=`21 mapped chains | 4 limb Two-Bone Goals | 10 finger Goals | ${goalCount} Goal markers | ${goalCount} pole markers | ${DATA.anchors.length} anchor families`}
function renderTposeContracts(){renderContractSide('source');renderContractSide('target');const map=$('chainMap');map.replaceChildren();for(const c of DATA.retargetChains){const row=document.createElement('div');row.className='chain-row';row.dataset.chain=c.label;const strong=document.createElement('strong');strong.textContent=c.label;row.append(strong);const badge=document.createElement('span');badge.className='ik-badge';badge.textContent=c.ikMode;row.append(badge);const s=document.createElement('div');s.textContent=`S: ${c.source.map(i=>DATA.sourceBones[i][1]).join(' -> ')}`;row.append(s);const t=document.createElement('div');t.textContent=`T: ${c.target.map(i=>DATA.targetBones[i][1]).join(' -> ')}`;row.append(t);if(c.ikMode!=='fk_only'){const goal=document.createElement('div');goal.textContent=`Goal: ${c.sourceGoalName} -> ${c.targetGoalName} | pole: ${DATA.sourceBones[c.sourcePoleBone][1]} -> ${DATA.targetBones[c.targetPoleBone][1]}`;row.append(goal)}map.append(row)}const section=$('tposeContract');section.dataset.chainCount=String(DATA.retargetChains.length);section.dataset.goalCountPerSide=String(DATA.retargetChains.filter(c=>c.ikMode!=='fk_only').length);section.dataset.poleCountPerSide=section.dataset.goalCountPerSide;section.dataset.anchorCountPerSide=String(DATA.anchors.length);window.__tposeContractDigest=[...section.querySelectorAll('svg')].map(s=>s.innerHTML).join('|')+'|'+map.textContent}
function tposeContractDigest(){return window.__tposeContractDigest}
renderTposeContracts();
)TPOSEJS";
    ReplaceAll(
        "for(const c of DATA.clips){c.sourceTrs=f32(c.sourceTrs);c.fkTrs=f32(c.fkTrs);c.finalTrs=f32(c.finalTrs)}const matrix=",
        "for(const c of DATA.clips){c.sourceTrs=f32(c.sourceTrs);c.fkTrs=f32(c.fkTrs);c.finalTrs=f32(c.finalTrs)}" +
            TposeContractJavascript + "const matrix=");
    ReplaceAll(
        "const SVG_NS='http://www.w3.org/2000/svg';",
        "const SVG_NS='http://www.w3.org/2000/svg';const rootPelvisSplitActive=DATA.route.includes('root_translation_scale_pelvis_model_rotation_split_v1');");
    ReplaceAll(
        "'Source Root - T / S'",
        "(rootPelvisSplitActive?'Source Root - T / S':'Source Root - legacy TRS')");
    ReplaceAll(
        "'Source Pelvis - model R'",
        "(rootPelvisSplitActive?'Source Pelvis - model R':'Source Pelvis - visible, not routed')");
    ReplaceAll(
        "'Hips - Root T/S + Pelvis model R'",
        "(rootPelvisSplitActive?'Hips - Root T/S + Pelvis model R':'Hips - historical Root-only TRS')");
    ReplaceAll(
        "Target T-pose - Hips owns Root T/S plus Pelvis model rotation",
        "Target T-pose - Hips role collapse and route ownership");
    ReplaceAll(
        "rg.appendChild(svgNode('circle',{cx:root[0],cy:root[1],r:10,class:'contract-root'}));",
        "rg.appendChild(svgNode('line',{x1:root[0],y1:root[1],x2:root[0]-22,y2:root[1]+18,class:'contract-role-line'}));rg.appendChild(svgNode('circle',{cx:root[0]-22,cy:root[1]+18,r:10,class:'contract-root'}));");
    ReplaceAll(
        "rg.appendChild(svgNode('text',{x:root[0],y:root[1],class:'contract-marker-letter'},'R'));",
        "rg.appendChild(svgNode('text',{x:root[0]-22,y:root[1]+18,class:'contract-marker-letter'},'R'));");
    ReplaceAll(
        "rg.appendChild(svgNode('text',{x:root[0]+13,y:root[1]+4,class:'contract-label'},'Source Root - T / S'));",
        "rg.appendChild(svgNode('text',{x:root[0]-36,y:root[1]+22,class:'contract-label','text-anchor':'end'},'Source Root - T / S'));");
    ReplaceAll(
        "pg.appendChild(svgNode('circle',{cx:pelvis[0],cy:pelvis[1],r:10,class:'contract-pelvis'}));",
        "pg.appendChild(svgNode('line',{x1:pelvis[0],y1:pelvis[1],x2:pelvis[0]+22,y2:pelvis[1]-12,class:'contract-role-line'}));pg.appendChild(svgNode('circle',{cx:pelvis[0]+22,cy:pelvis[1]-12,r:10,class:'contract-pelvis'}));");
    ReplaceAll(
        "pg.appendChild(svgNode('text',{x:pelvis[0],y:pelvis[1],class:'contract-marker-letter'},'P'));",
        "pg.appendChild(svgNode('text',{x:pelvis[0]+22,y:pelvis[1]-12,class:'contract-marker-letter'},'P'));");
    ReplaceAll(
        "pg.appendChild(svgNode('text',{x:pelvis[0]+13,y:pelvis[1]+4,class:'contract-label'},'Source Pelvis - model R'));",
        "pg.appendChild(svgNode('text',{x:pelvis[0]+36,y:pelvis[1]-8,class:'contract-label'},'Source Pelvis - model R'));");
    ReplaceAll(
        "window.__d117bQa={sourceGhostOpacity:.1,",
        "window.__d117bQa={sourceGhostOpacity:.1,tposeDiagramCount:document.querySelectorAll('.tpose-svg').length,retargetChainCount:DATA.retargetChains.length,limbIkGoalCount:DATA.retargetChains.filter(c=>c.ikMode==='two_bone').length,fingerIkGoalCount:DATA.retargetChains.filter(c=>c.ikMode==='finger').length,goalMarkerCount:document.querySelectorAll('[data-marker=goal]').length,poleMarkerCount:document.querySelectorAll('[data-marker=pole]').length,anchorMarkerCount:document.querySelectorAll('[data-marker=anchor]').length,rootPelvisContract:DATA.rootPelvis,tposeContractDigest,");
    ReplaceAll(
        "All 51 mapped non-root target local translations remain at target T-pose rest (21 body + 30 finger), while Hips keeps animated root motion.",
        "All 51 mapped non-root target local translations remain at target T-pose rest (21 body + 30 finger). In the active split route, Target Hips takes translation and scale from Source Root and rotation from the Source Pelvis model pose; the two candidates are selected by channel and are never composed sequentially. Historical root-only routes are explicitly labelled and do not claim this split.");
    ReplaceAll(
        "Final Result contains no source overlay.",
        "Final Result contains no source overlay. The static T-pose contract below lists all mapped chains, limb and finger IK Goals, poles, anchor families, and the explicit Root/Pelvis ownership split; it uses one shared centimeter display scale and does not react to animation, frame or camera controls.");
    ReplaceAll(
        "<label><input id=\"skeletonToggle\" type=\"checkbox\" checked>Skeleton</label><label>animation",
        "<label><input id=\"skeletonToggle\" type=\"checkbox\" checked>Skeleton</label><label><input id=\"footLockToggle\" type=\"checkbox\">FootLock Op</label><label>animation");
    ReplaceAll(
        "c.fkTrs=f32(c.fkTrs);c.finalTrs=f32(c.finalTrs)",
        "c.fkTrs=f32(c.fkTrs);c.foundationTrs=f32(c.foundationTrs);c.finalTrs=f32(c.finalTrs)");
    ReplaceAll(
        "label:'FK + IK + Original 10%',contract:'anchor_aligned_source_ghost_10_plus_fk_ik_100'",
        "label:'Foundation (FK + IK) + Original 10%',contract:'anchor_aligned_source_ghost_10_plus_foundation_100'");
    ReplaceAll(
        "{id:'final',label:'Final Result',contract:'final_only_no_overlay'}",
        "{id:'final',label:'Final Result (FootLock toggle)',contract:'toggle_selected_final_only_no_overlay'}");
    ReplaceAll(
        "$('nextClip').disabled=DATA.clips.length<2;let clipIndex",
        "$('nextClip').disabled=DATA.clips.length<2;$('footLockToggle').checked=DATA.sourceMotionFootLockCandidateEnabled;$('footLockToggle').disabled=!DATA.sourceMotionFootLockCandidateEnabled;let clipIndex");
    ReplaceAll(
        "fk:skinPackage(DATA.targetMesh,c.fkTrs,slot,DATA.targetBones.length),final:skinPackage(DATA.targetMesh,c.finalTrs,slot,DATA.targetBones.length)",
        "fk:skinPackage(DATA.targetMesh,c.fkTrs,slot,DATA.targetBones.length),foundation:skinPackage(DATA.targetMesh,c.foundationTrs,slot,DATA.targetBones.length),final:skinPackage(DATA.targetMesh,c.finalTrs,slot,DATA.targetBones.length)");
    ReplaceAll(
        "visit(pointFromTr(c.fkTrs,trBase(frame,DATA.targetBones.length,i)));visit(pointFromTr(c.finalTrs,trBase(frame,DATA.targetBones.length,i)))",
        "visit(pointFromTr(c.fkTrs,trBase(frame,DATA.targetBones.length,i)));visit(pointFromTr(c.foundationTrs,trBase(frame,DATA.targetBones.length,i)));visit(pointFromTr(c.finalTrs,trBase(frame,DATA.targetBones.length,i)))");
    ReplaceAll(
        "fkPoint=i=>bonePoint(c.fkTrs,DATA.targetBones,i),finalPoint=i=>bonePoint(c.finalTrs,DATA.targetBones,i);",
        "fkPoint=i=>bonePoint(c.fkTrs,DATA.targetBones,i),foundationPoint=i=>bonePoint(c.foundationTrs,DATA.targetBones,i),finalPoint=i=>bonePoint(c.finalTrs,DATA.targetBones,i),selectedPoint=$('footLockToggle').checked?finalPoint:foundationPoint;");
    ReplaceAll(
        "else if(lane==='ikOverlay'){line(DATA.sourceBones,ghostPoint,source,.1);line(DATA.targetBones,finalPoint,ik,1)}else line(DATA.targetBones,finalPoint,ik,1)",
        "else if(lane==='ikOverlay'){line(DATA.sourceBones,ghostPoint,source,.1);line(DATA.targetBones,foundationPoint,ik,1)}else line(DATA.targetBones,selectedPoint,ik,1)");
    ReplaceAll(
        "else if(lane==='ikOverlay'){r.draw(DATA.targetMesh,skin.final,[.46,.37,.56],1);",
        "else if(lane==='ikOverlay'){r.draw(DATA.targetMesh,skin.foundation,[.46,.37,.56],1);");
    ReplaceAll(
        "else r.draw(DATA.targetMesh,skin.final,[.46,.37,.56],1)",
        "else r.draw(DATA.targetMesh,$('footLockToggle').checked?skin.final:skin.foundation,[.46,.37,.56],1)");
    ReplaceAll(
        "$('exportFbx').href=encodeURI(clip.exportFbx);",
        "$('exportFbx').href=encodeURI($('footLockToggle').checked?clip.exportFbx:clip.foundationExportFbx);$('exportFbx').dataset.lane=$('footLockToggle').checked?'final':'foundation';");
    ReplaceAll(
        "endpoint=${clip.limbIkMaximumEndpointErrorCm.toExponential(2)}cm`;",
        "endpoint=${clip.limbIkMaximumEndpointErrorCm.toExponential(2)}cm | FootLock=${$('footLockToggle').checked?'on':'off'} source-motion-only=${clip.sourceMotionFootLockNoGroundOrContactSemanticsUsed?'yes':'no'}`;");
    ReplaceAll(
        "$('skeletonToggle').onchange=render;",
        "$('skeletonToggle').onchange=render;$('footLockToggle').onchange=render;");
    ReplaceAll(
        "exportHref:()=>$('exportFbx').getAttribute('href'),sourceDisplayContract:",
        "exportHref:()=>$('exportFbx').getAttribute('href'),setFootLock:v=>{if(!$('footLockToggle').disabled){$('footLockToggle').checked=!!v;render()}},footLockState:()=>({enabled:$('footLockToggle').checked,available:DATA.sourceMotionFootLockCandidateEnabled,lane:$('exportFbx').dataset.lane}),foundationFrozen:()=>({route:DATA.foundationRoute,frozen:DATA.foundationFrozen}),opStackStatus:()=>({route:DATA.sourceMotionFootLockRoute,enabled:currentClip().sourceMotionFootLockEnabled,success:currentClip().sourceMotionFootLockSuccess,deterministic:currentClip().sourceMotionFootLockDeterministic,noGroundOrContact:currentClip().sourceMotionFootLockNoGroundOrContactSemanticsUsed,committed:currentClip().sourceMotionFootLockCommittedFrames,rolledBack:currentClip().sourceMotionFootLockRolledBackFrames}),sourceDisplayContract:");
    ReplaceAll(
        "The export button points to the pre-generated target Mesh + skeleton + Final animation FBX for the selected clip.",
        "The export button follows the FootLock Op toggle: OFF downloads the roundtrip-verified frozen Foundation FBX; ON downloads the independently roundtrip-verified FootLock Final FBX.");
    ReplaceAll(
        "FK + IK / Final Result",
        "Foundation (FK + IK) / toggle-selected Final Result");
    ReplaceAll(
        "upstream Limb IK selected=true adopted=true | Spine/Pelvis candidate enabled=true route_selected=false route_adopted=false | stage_complete=false | independent review pending",
        "Foundation v1 frozen=true | FootLock candidate selected=false adopted=false | no ground/contact semantics | stage_complete=false | independent review pending");

    // Visual-inspection controls are deliberately layered onto the frozen
    // pose lanes. They consume existing model-pose samples only and never
    // participate in FK, IK, FootLock, export, or route selection.
    const std::string VisualInspectionCss = R"VISUALCSS(
.viewport{touch-action:none;cursor:grab;user-select:none}.viewport.camera-dragging{cursor:grabbing}.grid-canvas,.skeleton-canvas{pointer-events:none}.inspection-controls{display:contents}.inspection-help{flex-basis:100%;color:color-mix(in srgb,var(--ink) 72%,transparent);font:11px ui-monospace,monospace}.inspection-help b{color:var(--ink)}#groundY{width:68px}#goalTrailLength{width:96px}.goal-trail-key{display:inline-flex;align-items:center;gap:5px}.goal-trail-key i{display:inline-block;width:22px;border-top:3px solid #35d4ff;border-radius:10px}.ground-grid-key{display:inline-block;width:22px;height:12px;background:linear-gradient(90deg,transparent 45%,#718092 46%,#718092 54%,transparent 55%),linear-gradient(0deg,transparent 45%,#718092 46%,#718092 54%,transparent 55%);background-size:8px 8px;vertical-align:middle;margin-right:5px}
)VISUALCSS";
    ReplaceAll(
        ".readout{font:12px ui-monospace,monospace;white-space:pre-wrap}",
        ".readout{font:12px ui-monospace,monospace;white-space:pre-wrap}" +
            VisualInspectionCss);
    ReplaceAll(
        "<label>3D yaw <input id=\"yaw\" type=\"range\" min=\"-180\" max=\"180\" value=\"-28\"></label>",
        "<span class=\"inspection-controls\"><label>camera <select id=\"cameraMode\"><option value=\"free\" selected>Free</option><option value=\"follow\">Follow</option></select></label><button id=\"resetCamera\" type=\"button\">Reset Camera</button><label><input id=\"groundToggle\" type=\"checkbox\" checked>Ground Grid</label><label>Ground Y <input id=\"groundY\" type=\"number\" step=\"1\" value=\"0\"> cm</label><label><input id=\"goalTrailToggle\" type=\"checkbox\" checked>Goal History</label><label>Goals <select id=\"goalScope\"><option value=\"feet\" selected>Feet</option><option value=\"limbs\">Limbs</option><option value=\"all\">All IK</option></select></label><label>History <select id=\"goalTrailLength\"><option value=\"30\">30f</option><option value=\"60\" selected>60f</option><option value=\"120\">120f</option><option value=\"0\">All</option></select></label></span><label>3D yaw <input id=\"yaw\" type=\"range\" min=\"-180\" max=\"180\" value=\"-28\"></label>");
    ReplaceAll(
        "<label>display scale <input id=\"zoom\" type=\"range\" min=\"60\" max=\"180\" value=\"100\"></label>",
        "<label>display scale <input id=\"zoom\" type=\"range\" min=\"10\" max=\"1200\" value=\"100\"></label><span class=\"inspection-help\"><b>Free camera:</b> RMB rotate | LMB pan | wheel zoom. All four views share one camera. Ground and Goal history are display-only evidence.</span>");
    ReplaceAll(
        "<div class=\"viewport\"><canvas class=\"mesh-canvas\"",
        "<div class=\"viewport\"><canvas class=\"grid-canvas\" data-layer=\"virtual-ground\"></canvas><canvas class=\"mesh-canvas\"");
    ReplaceAll(
        "const meshCanvases=[...document.querySelectorAll('.mesh-canvas')],skeletonCanvases=",
        "const gridCanvases=[...document.querySelectorAll('.grid-canvas')],meshCanvases=[...document.querySelectorAll('.mesh-canvas')],skeletonCanvases=");
    ReplaceAll(
        "drawCalls=0,webglErrors=[];function trBase",
        "drawCalls=0,webglErrors=[],cameraState={center:null,radius:null},pointerState=null,gridSpacingCm=0;function trBase");

    const std::string OriginalComputeBounds =
        "function computeBounds(){const key=`${clipIndex}:${$('anchor').value}`;let radius=radiusCache.get(key);if(radius===undefined){radius=0;for(let i=0;i<currentClip().frameCount;i++)radius=Math.max(radius,frameEnvelope(i).r);radiusCache.set(key,radius)}bounds={center:frameEnvelope(slot).center,radius}}";
    const std::string VisualComputeBounds = R"VISUALJS(function clipRadius(){const key=`${clipIndex}:${$('anchor').value}`;let radius=radiusCache.get(key);if(radius===undefined){radius=0;for(let i=0;i<currentClip().frameCount;i++)radius=Math.max(radius,frameEnvelope(i).r);radiusCache.set(key,radius)}return radius}function computeBounds(){const radius=clipRadius(),envelope=frameEnvelope(slot);if($('cameraMode').value==='follow'){bounds={center:[...envelope.center],radius};cameraState.center=[...bounds.center];cameraState.radius=radius}else{if(!cameraState.center)cameraState.center=[...envelope.center];if(!cameraState.radius)cameraState.radius=radius;bounds={center:[...cameraState.center],radius:cameraState.radius}}})VISUALJS";
    ReplaceAll(OriginalComputeBounds, VisualComputeBounds);

    const std::string VisualInspectionJavascript = R"VISUALJS(
function resizeOverlayCanvas(canvas){const d=devicePixelRatio||1,w=Math.max(320,Math.round(canvas.clientWidth*d)),h=Math.max(280,Math.round(canvas.clientHeight*d));if(canvas.width!==w||canvas.height!==h){canvas.width=w;canvas.height=h}return{d,w,h}}
function niceGridStep(value){const safe=Math.max(1e-6,value),power=Math.pow(10,Math.floor(Math.log10(safe))),fraction=safe/power,nice=fraction<=1?1:fraction<=2?2:fraction<=5?5:10;return nice*power}
function drawGround(canvas){const size=resizeOverlayCanvas(canvas),ctx=canvas.getContext('2d');ctx.clearRect(0,0,size.w,size.h);if(!$('groundToggle').checked)return;if(!bounds)computeBounds();const y=Number($('groundY').value)||0,zoom=Math.max(.1,Number($('zoom').value)/100),viewRadius=bounds.radius/zoom,spacing=niceGridStep(viewRadius/9),extent=viewRadius*Math.max(2.4,size.w/Math.max(1,size.h)*1.6),minX=Math.floor((bounds.center[0]-extent)/spacing)*spacing,maxX=Math.ceil((bounds.center[0]+extent)/spacing)*spacing,minZ=Math.floor((bounds.center[2]-extent)/spacing)*spacing,maxZ=Math.ceil((bounds.center[2]+extent)/spacing)*spacing;gridSpacingCm=spacing;ctx.lineCap='butt';const line=(a,b,color,width,alpha)=>{const pa=project(canvas,a),pb=project(canvas,b);ctx.globalAlpha=alpha;ctx.strokeStyle=color;ctx.lineWidth=width*size.d;ctx.beginPath();ctx.moveTo(pa[0],pa[1]);ctx.lineTo(pb[0],pb[1]);ctx.stroke()};let count=0;for(let x=minX;x<=maxX+spacing*.25&&count<180;x+=spacing,count++){const major=Math.abs(Math.round(x/spacing))%5===0;line([x,y,minZ],[x,y,maxZ],major?'#7e8998':'#657181',major?1.15:.7,major?.42:.22)}count=0;for(let z=minZ;z<=maxZ+spacing*.25&&count<180;z+=spacing,count++){const major=Math.abs(Math.round(z/spacing))%5===0;line([minX,y,z],[maxX,y,z],major?'#7e8998':'#657181',major?1.15:.7,major?.42:.22)}line([minX,y,0],[maxX,y,0],'#4f82c2',1.5,.72);line([0,y,minZ],[0,y,maxZ],'#c65e5e',1.5,.72);ctx.globalAlpha=1}
function activeGoalChains(){const scope=$('goalScope').value;return DATA.retargetChains.filter(chain=>{if(chain.ikMode==='fk_only')return false;if(scope==='all')return true;if(scope==='limbs')return chain.ikMode==='two_bone';const text=`${chain.label} ${chain.sourceGoalName} ${chain.targetGoalName}`.toLowerCase();return chain.ikMode==='two_bone'&&/(foot|leg)/.test(text)})}
function goalColor(chain,index){const text=`${chain.label} ${chain.targetGoalName}`.toLowerCase();if(/left/.test(text)&&/(foot|leg)/.test(text))return'#25d2ff';if(/right/.test(text)&&/(foot|leg)/.test(text))return'#ffb52e';if(/left/.test(text)&&/(hand|arm)/.test(text))return'#ff6eaa';if(/right/.test(text)&&/(hand|arm)/.test(text))return'#78e76f';const hue=(index*47+196)%360;return`hsl(${hue} 78% 62%)`}
function trsPointAt(trs,bones,bone,frame){return pointFromTr(trs,trBase(frame,bones.length,bone))}
function selectedTargetTrsForLane(lane){const c=currentClip();if(lane==='fkOverlay')return c.fkTrs;if(lane==='ikOverlay')return c.foundationTrs;return $('footLockToggle').checked?c.finalTrs:c.foundationTrs}
function strokeGoalPath(ctx,canvas,points,color,alpha,dashed,d){if(points.length===0)return;ctx.save();ctx.strokeStyle=color;ctx.fillStyle=color;ctx.globalAlpha=alpha;ctx.lineWidth=2*d;ctx.lineJoin='round';ctx.lineCap='round';ctx.setLineDash(dashed?[5*d,4*d]:[]);ctx.beginPath();for(let i=0;i<points.length;i++){const p=project(canvas,points[i]);if(i===0)ctx.moveTo(p[0],p[1]);else ctx.lineTo(p[0],p[1])}ctx.stroke();ctx.setLineDash([]);const end=project(canvas,points[points.length-1]);ctx.globalAlpha=Math.min(1,alpha+.28);ctx.beginPath();ctx.arc(end[0],end[1],4.2*d,0,Math.PI*2);ctx.fill();ctx.globalAlpha=1;ctx.lineWidth=1.5*d;ctx.strokeStyle='#111820';ctx.stroke();ctx.restore()}
function drawGoalTrails(canvas,lane){if(!$('goalTrailToggle').checked)return;const ctx=canvas.getContext('2d'),d=devicePixelRatio||1,c=currentClip(),anchor=currentAnchor(),requested=Number($('goalTrailLength').value),first=requested===0?0:Math.max(0,slot-requested+1),chains=activeGoalChains();for(const [index,chain] of chains.entries()){const color=goalColor(chain,index),sourcePoints=[];for(let frame=first;frame<=slot;frame++){const p=trsPointAt(c.sourceTrs,DATA.sourceBones,chain.sourceGoalBone,frame);sourcePoints.push(lane==='original'?p:alignPoint(p,c,frame,anchor))}if(lane==='original'){strokeGoalPath(ctx,canvas,sourcePoints,color,.92,false,d);continue}const targetTrs=selectedTargetTrsForLane(lane),targetPoints=[];for(let frame=first;frame<=slot;frame++)targetPoints.push(trsPointAt(targetTrs,DATA.targetBones,chain.targetGoalBone,frame));strokeGoalPath(ctx,canvas,sourcePoints,color,.24,true,d);strokeGoalPath(ctx,canvas,targetPoints,color,.96,false,d)}}
function drawInspectionHud(ctx,canvas,lane){const d=devicePixelRatio||1,c=currentClip();ctx.save();ctx.globalAlpha=1;ctx.fillStyle=getComputedStyle(document.documentElement).getPropertyValue('--ink');ctx.font=`${11*d}px ui-monospace,monospace`;ctx.fillText(`3D orthographic | frame ${c.startFrame+slot} | ${((c.startFrame+slot)/c.fps).toFixed(3)} s`,8*d,15*d);ctx.fillText(`camera=${$('cameraMode').value} | grid=${$('groundToggle').checked?`${gridSpacingCm.toPrecision(3)}cm @ Y=${Number($('groundY').value)||0}`:'off'} | goal history=${$('goalTrailToggle').checked?`${$('goalScope').value}/${$('goalTrailLength').value==='0'?'all':$('goalTrailLength').value+'f'}`:'off'} | lane=${lane}`,8*d,30*d);ctx.restore()}
function resetCameraView(renderNow=true){const envelope=frameEnvelope(slot);cameraState.center=[...envelope.center];cameraState.radius=clipRadius();$('yaw').value='-28';$('pitch').value='18';$('zoom').value='100';bounds=null;if(renderNow)render()}
function setFreeCameraFromCurrentBounds(){if(!bounds)computeBounds();cameraState.center=[...bounds.center];cameraState.radius=bounds.radius;bounds=null}
function cameraPan(view,dx,dy){if(!bounds)computeBounds();const canvas=view.querySelector('.mesh-canvas'),d=devicePixelRatio||1,w=Math.max(320,Math.round(canvas.clientWidth*d)),h=Math.max(280,Math.round(canvas.clientHeight*d)),zoom=Math.max(.1,Number($('zoom').value)/100),pixelsPerCm=Math.min(w,h)/(2*bounds.radius)*zoom,yaw=Number($('yaw').value)*Math.PI/180,pitch=Number($('pitch').value)*Math.PI/180,right=[Math.cos(yaw),0,Math.sin(yaw)],up=[Math.sin(pitch)*Math.sin(yaw),Math.cos(pitch),-Math.sin(pitch)*Math.cos(yaw)],sx=dx*d/pixelsPerCm,sy=dy*d/pixelsPerCm;for(let i=0;i<3;i++)cameraState.center[i]+=-right[i]*sx+up[i]*sy;bounds=null}
function bindCameraInteractions(){for(const view of document.querySelectorAll('.viewport')){view.addEventListener('contextmenu',event=>event.preventDefault());view.addEventListener('pointerdown',event=>{if($('cameraMode').value!=='free'||(event.button!==0&&event.button!==2))return;pointerState={id:event.pointerId,view,action:event.button===2?'orbit':'pan',x:event.clientX,y:event.clientY};view.setPointerCapture(event.pointerId);view.classList.add('camera-dragging');event.preventDefault()});view.addEventListener('pointermove',event=>{if(!pointerState||pointerState.id!==event.pointerId||pointerState.view!==view)return;const dx=event.clientX-pointerState.x,dy=event.clientY-pointerState.y;pointerState.x=event.clientX;pointerState.y=event.clientY;if(pointerState.action==='orbit'){$('yaw').value=String(((Number($('yaw').value)+dx*.42+180)%360+360)%360-180);$('pitch').value=String(Math.max(-89,Math.min(89,Number($('pitch').value)+dy*.34)))}else cameraPan(view,dx,dy);bounds=null;render();event.preventDefault()});const finish=event=>{if(!pointerState||pointerState.id!==event.pointerId)return;pointerState.view.classList.remove('camera-dragging');pointerState=null};view.addEventListener('pointerup',finish);view.addEventListener('pointercancel',finish);view.addEventListener('wheel',event=>{if($('cameraMode').value!=='free')return;const next=Math.max(10,Math.min(1200,Number($('zoom').value)*Math.exp(-event.deltaY*.0015)));$('zoom').value=String(next);bounds=null;render();event.preventDefault()},{passive:false})}}
)VISUALJS";
    ReplaceAll(
        "function bonePoint",
        VisualInspectionJavascript + "function bonePoint");
    ReplaceAll(
        "function drawSkeleton(canvas,lane){const ctx=canvas.getContext('2d'),d=devicePixelRatio||1,w=canvas.width,h=canvas.height,",
        "function drawSkeleton(canvas,lane){const size=resizeOverlayCanvas(canvas),ctx=canvas.getContext('2d'),d=size.d,w=size.w,h=size.h,");
    ReplaceAll(
        "ctx.clearRect(0,0,w,h);if(!$('skeletonToggle').checked)return;",
        "ctx.clearRect(0,0,w,h);drawGoalTrails(canvas,lane);if(!$('skeletonToggle').checked){drawInspectionHud(ctx,canvas,lane);return}");
    ReplaceAll(
        "ctx.fillText(`3D orthographic | frame ${c.startFrame+slot} | ${(slot/c.fps+c.startFrame/c.fps).toFixed(3)} s`,8*d,15*d)",
        "drawInspectionHud(ctx,canvas,lane)");
    ReplaceAll(
        "}}for(const c of skeletonCanvases)drawSkeleton(c,c.dataset.lane);",
        "}}for(const c of gridCanvases)drawGround(c);for(const c of skeletonCanvases)drawSkeleton(c,c.dataset.lane);");
    ReplaceAll(
        "skinCache=null;ghostCache=null;bounds=null;stop();render()",
        "skinCache=null;ghostCache=null;cameraState.center=null;cameraState.radius=null;bounds=null;stop();render()");
    ReplaceAll(
        "for(const id of ['yaw','pitch','zoom'])$(id).oninput=render;",
        "for(const id of ['yaw','pitch','zoom'])$(id).oninput=()=>{bounds=null;render()};$('cameraMode').onchange=()=>{if($('cameraMode').value==='free')setFreeCameraFromCurrentBounds();else bounds=null;render()};$('resetCamera').onclick=()=>resetCameraView(true);for(const id of ['groundToggle','groundY','goalTrailToggle','goalScope','goalTrailLength'])$(id).onchange=render;bindCameraInteractions();");
    ReplaceAll(
        ";window.__d117bReady=true}",
        ";$('readout').textContent+=` | Camera=${$('cameraMode').value} | Grid=${$('groundToggle').checked?`on/Y=${Number($('groundY').value)||0}`:'off'} | GoalHistory=${$('goalTrailToggle').checked?`${$('goalScope').value}/${$('goalTrailLength').value==='0'?'all':$('goalTrailLength').value+'f'}`:'off'}`;window.__d117bReady=true}");
    ReplaceAll(
        "camera:()=>({center:[...bounds.center],radius:bounds.radius}),",
        "camera:()=>({mode:$('cameraMode').value,center:[...bounds.center],radius:bounds.radius,yaw:Number($('yaw').value),pitch:Number($('pitch').value),zoom:Number($('zoom').value)/100}),setCameraMode:v=>{$('cameraMode').value=v==='follow'?'follow':'free';$('cameraMode').onchange()},resetCamera:()=>resetCameraView(true),setGround:(enabled,y=0)=>{$('groundToggle').checked=!!enabled;$('groundY').value=String(y);render()},setGoalHistory:(enabled,scope='feet',length=60)=>{$('goalTrailToggle').checked=!!enabled;$('goalScope').value=scope;$('goalTrailLength').value=String(length);render()},visualInspectionContract:()=>({freeCamera:true,rightMouseOrbit:true,leftMousePan:true,wheelZoom:true,sharedAcrossFourViews:true,virtualGroundDisplayOnly:true,goalHistoryDisplayOnly:true,goalSampleSource:'declared_goal_bone_evaluated_model_pose',gridCanvasCount:gridCanvases.length}),");
    ReplaceAll(
        "<span>result opacity = 100%</span>",
        "<span>result opacity = 100%</span><span><i class=\"ground-grid-key\"></i>Virtual world-space Ground Grid</span><span class=\"goal-trail-key\"><i></i>Goal history: solid lane / dashed aligned source</span>");
    ReplaceAll(
        "all four views use the same selected animation, frame/time, orthographic camera, yaw/pitch, projection and display scale.",
        "all four views use the same selected animation, frame/time, orthographic camera, yaw/pitch, projection, display scale and free-camera transform. Follow mode tracks the animated envelope; Free mode keeps a world-space camera center so character/root motion remains observable.");
    ReplaceAll(
        "The export button follows the FootLock Op toggle:",
        "The virtual XZ Ground Grid is a user-adjustable display reference at the labelled Y value; it is not ground/contact/terrain evidence. Goal History reads declared Goal-bone evaluated model positions from the existing source, FK, frozen Foundation and toggle-selected Final lanes; it does not alter or claim access to hidden solver inputs. The export button follows the FootLock Op toggle:");
    const auto ReplaceElementText =
        [&Result](const std::string& Id,
                  const std::string& Text)
    {
        const std::string Marker =
            "id=\"" + Id + "\"";
        const std::size_t MarkerPosition =
            Result.find(Marker);
        if (MarkerPosition == std::string::npos) return;
        const std::size_t TextBegin =
            Result.find('>', MarkerPosition);
        if (TextBegin == std::string::npos) return;
        const std::size_t TextEnd =
            Result.find('<', TextBegin + 1);
        if (TextEnd == std::string::npos) return;
        Result.replace(
            TextBegin + 1,
            TextEnd - TextBegin - 1,
            Text);
    };
    ReplaceElementText(
        "nextClip",
        "&#20999;&#25442;&#21160;&#30011;");
    ReplaceElementText(
        "exportFbx",
        "&#23548;&#20986; FBX");
    return Result;
}

FbxVector4 ToFbxEulerXYZ(const TransformRT& Value)
{
    const Quat Rotation = Normalize(Value.Rotation);
    FbxAMatrix Matrix;
    Matrix.SetIdentity();
    Matrix.SetQ(FbxQuaternion(
        Rotation.X, Rotation.Y, Rotation.Z, Rotation.W));
    return Matrix.GetR();
}

double UnrollDegrees(double Value, double Previous)
{
    while (Value - Previous > 180.0) Value -= 360.0;
    while (Value - Previous < -180.0) Value += 360.0;
    return Value;
}

bool VectorZero(const FbxVector4& Value)
{
    return std::abs(Value[0]) <= 1.0e-12 &&
        std::abs(Value[1]) <= 1.0e-12 &&
        std::abs(Value[2]) <= 1.0e-12;
}

bool VectorOne(const FbxVector4& Value)
{
    return std::abs(Value[0] - 1.0) <= 1.0e-12 &&
        std::abs(Value[1] - 1.0) <= 1.0e-12 &&
        std::abs(Value[2] - 1.0) <= 1.0e-12;
}

bool TargetNodeSupportsDirectCurves(FbxNode* Node)
{
    if (Node == nullptr) return false;
    EFbxRotationOrder Order = eEulerXYZ;
    Node->GetRotationOrder(FbxNode::eSourcePivot, Order);
    return Order == eEulerXYZ &&
        VectorZero(Node->GetPreRotation(
            FbxNode::eSourcePivot)) &&
        VectorZero(Node->GetPostRotation(
            FbxNode::eSourcePivot)) &&
        VectorZero(Node->GetRotationPivot(
            FbxNode::eSourcePivot)) &&
        VectorZero(Node->GetRotationOffset(
            FbxNode::eSourcePivot)) &&
        VectorZero(Node->GetScalingPivot(
            FbxNode::eSourcePivot)) &&
        VectorZero(Node->GetScalingOffset(
            FbxNode::eSourcePivot)) &&
        VectorZero(Node->GetGeometricTranslation(
            FbxNode::eSourcePivot)) &&
        VectorZero(Node->GetGeometricRotation(
            FbxNode::eSourcePivot)) &&
        VectorOne(Node->GetGeometricScaling(
            FbxNode::eSourcePivot));
}

FbxTime::EMode TimeModeForFps(double FramesPerSecond)
{
    if (std::abs(FramesPerSecond - 24.0) < 1.0e-9)
        return FbxTime::eFrames24;
    if (std::abs(FramesPerSecond - 25.0) < 1.0e-9)
        return FbxTime::ePAL;
    if (std::abs(FramesPerSecond - 30.0) < 1.0e-9)
        return FbxTime::eFrames30;
    if (std::abs(FramesPerSecond - 48.0) < 1.0e-9)
        return FbxTime::eFrames48;
    if (std::abs(FramesPerSecond - 50.0) < 1.0e-9)
        return FbxTime::eFrames50;
    if (std::abs(FramesPerSecond - 60.0) < 1.0e-9)
        return FbxTime::eFrames60;
    return FbxTime::eDefaultMode;
}

template <typename MatrixType>
std::string MatrixCanonical(
    const MatrixType& Matrix,
    double NumericQuantum)
{
    std::ostringstream Text;
    Text << std::setprecision(17);
    for (int Row = 0; Row < 4; ++Row)
        for (int Column = 0; Column < 4; ++Column)
        {
            const double Value = Matrix.Get(Row, Column);
            if (NumericQuantum > 0.0)
                Text << std::llround(Value / NumericQuantum);
            else
                Text << Value;
            Text << ",";
        }
    return Text.str();
}

bool BuildMeshFingerprint(
    FbxScene* Scene,
    MeshSceneFingerprint& Out,
    double NumericQuantum,
    std::string& OutError)
{
    if (Scene == nullptr)
    {
        OutError = "mesh fingerprint scene is null";
        return false;
    }
    std::map<std::string, FbxNode*> ByPath;
    std::map<FbxNode*, std::string> Paths;
    BuildPathMaps(Scene, ByPath, Paths);
    std::vector<FbxNode*> MeshNodes;
    CollectMeshNodes(Scene->GetRootNode(), MeshNodes);
    std::sort(
        MeshNodes.begin(), MeshNodes.end(),
        [&](FbxNode* Left, FbxNode* Right)
        {
            return Paths[Left] < Paths[Right];
        });
    std::ostringstream Canonical;
    Canonical << std::setprecision(17);
    for (FbxNode* Node : MeshNodes)
    {
        FbxMesh* Mesh = Node->GetMesh();
        if (Mesh == nullptr) continue;
        ++Out.MeshCount;
        Out.ControlPointCount += Mesh->GetControlPointsCount();
        Out.PolygonCount += Mesh->GetPolygonCount();
        Out.MaterialSlotCount += Node->GetMaterialCount();
        Canonical << "mesh|" << Paths[Node] << "|"
                  << Mesh->GetControlPointsCount() << "|"
                  << Mesh->GetPolygonCount() << "|"
                  << Node->GetMaterialCount() << "\n";
        const FbxVector4* Points = Mesh->GetControlPoints();
        for (int Index = 0;
             Index < Mesh->GetControlPointsCount(); ++Index)
        {
            for (int Axis = 0; Axis < 3; ++Axis)
            {
                if (NumericQuantum > 0.0)
                    Canonical << std::llround(
                        Points[Index][Axis] / NumericQuantum);
                else
                    Canonical << Points[Index][Axis];
                Canonical << (Axis == 2 ? ";" : ",");
            }
        }
        Canonical << "\n";
        for (int Polygon = 0;
             Polygon < Mesh->GetPolygonCount(); ++Polygon)
        {
            Canonical << Mesh->GetPolygonSize(Polygon) << ":";
            for (int Corner = 0;
                 Corner < Mesh->GetPolygonSize(Polygon); ++Corner)
                Canonical << Mesh->GetPolygonVertex(
                    Polygon, Corner) << ",";
            Canonical << ";";
        }
        Canonical << "\n";
        const int SkinCount =
            Mesh->GetDeformerCount(FbxDeformer::eSkin);
        Out.SkinDeformerCount += SkinCount;
        for (int SkinIndex = 0;
             SkinIndex < SkinCount; ++SkinIndex)
        {
            FbxSkin* Skin = FbxCast<FbxSkin>(
                Mesh->GetDeformer(
                    SkinIndex, FbxDeformer::eSkin));
            if (Skin == nullptr)
            {
                OutError = "invalid skin during fingerprint";
                return false;
            }
            Canonical << "skin|" << SkinIndex << "|"
                      << Skin->GetClusterCount() << "\n";
            Out.SkinClusterCount += Skin->GetClusterCount();
            for (int ClusterIndex = 0;
                 ClusterIndex < Skin->GetClusterCount();
                 ++ClusterIndex)
            {
                FbxCluster* Cluster =
                    Skin->GetCluster(ClusterIndex);
                if (Cluster == nullptr ||
                    Cluster->GetLink() == nullptr)
                {
                    OutError =
                        "invalid cluster during fingerprint";
                    return false;
                }
                const auto LinkPath =
                    Paths.find(Cluster->GetLink());
                FbxAMatrix MeshBind;
                FbxAMatrix LinkBind;
                Cluster->GetTransformMatrix(MeshBind);
                Cluster->GetTransformLinkMatrix(LinkBind);
                Canonical << "cluster|"
                          << (LinkPath != Paths.end()
                                  ? LinkPath->second
                                  : Cluster->GetLink()->GetName())
                          << "|"
                          << static_cast<int>(
                                 Cluster->GetLinkMode())
                          << "|" << MatrixCanonical(
                                 MeshBind, NumericQuantum)
                          << "|" << MatrixCanonical(
                                 LinkBind, NumericQuantum)
                          << "\n";
                const int Count =
                    Cluster->GetControlPointIndicesCount();
                Out.SkinControlPointIndexCount += Count;
                const int* Indices =
                    Cluster->GetControlPointIndices();
                const double* Weights =
                    Cluster->GetControlPointWeights();
                for (int Influence = 0;
                     Influence < Count; ++Influence)
                {
                    Canonical << Indices[Influence] << ":";
                    if (NumericQuantum > 0.0)
                        Canonical << std::llround(
                            Weights[Influence] / NumericQuantum);
                    else
                        Canonical << Weights[Influence];
                    Canonical << ",";
                }
                Canonical << "\n";
            }
        }
    }
    Out.BindPoseCount = 0;
    for (int PoseIndex = 0;
         PoseIndex < Scene->GetPoseCount(); ++PoseIndex)
    {
        FbxPose* Pose = Scene->GetPose(PoseIndex);
        if (Pose == nullptr || !Pose->IsBindPose()) continue;
        ++Out.BindPoseCount;
        Canonical << "bindpose|"
                  << (Pose->GetName() != nullptr
                          ? Pose->GetName()
                          : "")
                  << "|" << Pose->GetCount() << "\n";
        for (int Entry = 0; Entry < Pose->GetCount(); ++Entry)
        {
            FbxNode* Node = Pose->GetNode(Entry);
            const auto PathIt = Paths.find(Node);
            Canonical << (PathIt != Paths.end()
                              ? PathIt->second
                              : Node != nullptr
                                  ? Node->GetName()
                                  : "")
                      << "|"
                      << MatrixCanonical(
                             Pose->GetMatrix(Entry),
                             NumericQuantum)
                      << "\n";
        }
    }
    Out.CanonicalText = Canonical.str();
    return ComputeSha256Bytes(
        reinterpret_cast<const unsigned char*>(
            Out.CanonicalText.data()),
        Out.CanonicalText.size(), Out.Sha256, OutError);
}

bool WriteAnimationToTargetScene(
    FbxScene* Scene,
    const std::vector<RetargetReviewBone>& TargetBones,
    const RetargetReviewClipView& Clip,
    ReviewPoseLane Lane,
    bool ReplaceExistingAnimationStacks,
    bool ConvertUEJsonPoseToNativeFbx,
    std::string& OutError)
{
    if (Scene == nullptr)
    {
        OutError = "target T-pose scene is null";
        return false;
    }
    if (Scene->GetSrcObjectCount<FbxAnimStack>() != 0)
    {
        if (!ReplaceExistingAnimationStacks)
        {
            OutError =
                "target T-pose must contain no pre-existing animation stack";
            return false;
        }
        while (Scene->GetSrcObjectCount<FbxAnimStack>() > 0)
        {
            FbxAnimStack* Existing =
                Scene->GetSrcObject<FbxAnimStack>(0);
            if (Existing == nullptr)
            {
                OutError =
                    "target animation stack inventory is malformed";
                return false;
            }
            Existing->Destroy();
        }
    }
    std::map<std::string, FbxNode*> ByPath;
    std::map<FbxNode*, std::string> Paths;
    BuildPathMaps(Scene, ByPath, Paths);
    std::vector<FbxNode*> Nodes;
    Nodes.reserve(TargetBones.size());
    for (const RetargetReviewBone& Bone : TargetBones)
    {
        const auto It = ByPath.find(Bone.Path);
        if (It == ByPath.end() ||
            !TargetNodeSupportsDirectCurves(It->second))
        {
            OutError =
                "target skeleton node cannot receive direct TRS curves: " +
                Bone.Path;
            return false;
        }
        Nodes.push_back(It->second);
    }
    const FbxTime::EMode TimeMode =
        TimeModeForFps(Clip.FramesPerSecond);
    if (TimeMode == FbxTime::eDefaultMode)
    {
        OutError =
            "clip frame rate is unsupported by the diagnostic exporter";
        return false;
    }
    Scene->GetGlobalSettings().SetTimeMode(TimeMode);
    const std::string StackName =
        std::string("SKRTG_UEIK_") + PoseLaneName(Lane) +
        "_" + Clip.Id;
    const std::string LayerName =
        StackName + "_Layer";
    FbxAnimStack* Stack =
        FbxAnimStack::Create(Scene, StackName.c_str());
    FbxAnimLayer* Layer =
        FbxAnimLayer::Create(Scene, LayerName.c_str());
    if (Stack == nullptr || Layer == nullptr ||
        !Stack->AddMember(Layer))
    {
        OutError =
            "failed to create SKRTG UE IK animation stack/layer";
        return false;
    }
    Scene->SetCurrentAnimationStack(Stack);
    FbxTime Start;
    FbxTime Stop;
    Start.SetFrame(
        Clip.Frames.front().FrameIndex, TimeMode);
    Stop.SetFrame(
        Clip.Frames.back().FrameIndex, TimeMode);
    FbxTimeSpan Span(Start, Stop);
    Stack->SetLocalTimeSpan(Span);
    Scene->GetGlobalSettings().SetTimelineDefaultTimeSpan(Span);
    const char* Channels[3] = {
        FBXSDK_CURVENODE_COMPONENT_X,
        FBXSDK_CURVENODE_COMPONENT_Y,
        FBXSDK_CURVENODE_COMPONENT_Z};
    for (std::size_t BoneIndex = 0;
         BoneIndex < Nodes.size(); ++BoneIndex)
    {
        FbxNode* Node = Nodes[BoneIndex];
        std::array<FbxAnimCurve*, 9> Curves{};
        for (int Axis = 0; Axis < 3; ++Axis)
        {
            Curves[static_cast<std::size_t>(Axis)] =
                Node->LclTranslation.GetCurve(
                    Layer, Channels[Axis], true);
            Curves[static_cast<std::size_t>(3 + Axis)] =
                Node->LclRotation.GetCurve(
                    Layer, Channels[Axis], true);
            Curves[static_cast<std::size_t>(6 + Axis)] =
                Node->LclScaling.GetCurve(
                    Layer, Channels[Axis], true);
        }
        if (std::any_of(
                Curves.begin(), Curves.end(),
                [](FbxAnimCurve* Curve)
                {
                    return Curve == nullptr;
                }))
        {
            OutError =
                "failed to create all target TRS curves";
            return false;
        }
        for (FbxAnimCurve* Curve : Curves)
            Curve->KeyModifyBegin();
        FbxVector4 PreviousEuler;
        bool HavePreviousEuler = false;
        for (const RetargetReviewFrameView& Frame : Clip.Frames)
        {
            const core::animation::PoseBuffer* LaneLocal =
                LocalPoseForLane(Frame, Lane);
            if (LaneLocal == nullptr)
            {
                for (FbxAnimCurve* Curve : Curves)
                    Curve->KeyModifyEnd();
                OutError =
                    "selected target pose lane has no local pose";
                return false;
            }
            TransformRT Local = (*LaneLocal)[BoneIndex];
            if (ConvertUEJsonPoseToNativeFbx)
                Local = ReflectFbxYBasis(Local);
            FbxVector4 Euler = ToFbxEulerXYZ(Local);
            if (HavePreviousEuler)
            {
                Euler[0] =
                    UnrollDegrees(Euler[0], PreviousEuler[0]);
                Euler[1] =
                    UnrollDegrees(Euler[1], PreviousEuler[1]);
                Euler[2] =
                    UnrollDegrees(Euler[2], PreviousEuler[2]);
            }
            PreviousEuler = Euler;
            HavePreviousEuler = true;
            const double Values[9] = {
                Local.TranslationCm.X,
                Local.TranslationCm.Y,
                Local.TranslationCm.Z,
                Euler[0], Euler[1], Euler[2],
                Local.Scale.X, Local.Scale.Y, Local.Scale.Z};
            FbxTime Time;
            Time.SetFrame(Frame.FrameIndex, TimeMode);
            for (std::size_t CurveIndex = 0;
                 CurveIndex < Curves.size(); ++CurveIndex)
            {
                const int KeyIndex =
                    Curves[CurveIndex]->KeyAdd(Time);
                if (KeyIndex < 0)
                {
                    for (FbxAnimCurve* Curve : Curves)
                        Curve->KeyModifyEnd();
                    OutError =
                        "failed to write complete target TRS key grid";
                    return false;
                }
                Curves[CurveIndex]->KeySet(
                    KeyIndex, Time,
                    static_cast<float>(Values[CurveIndex]),
                    FbxAnimCurveDef::eInterpolationLinear);
            }
        }
        for (FbxAnimCurve* Curve : Curves)
            Curve->KeyModifyEnd();
    }
    return true;
}

bool ExportScene(
    LoadedScene& Scene,
    const std::filesystem::path& OutputPath,
    std::string& OutError)
{
    std::error_code Error;
    std::filesystem::remove(OutputPath, Error);
    FbxExporter* Exporter =
        FbxExporter::Create(Scene.Manager, "");
    if (Exporter == nullptr ||
        !Exporter->Initialize(
            OutputPath.string().c_str(), -1,
            Scene.Manager->GetIOSettings()))
    {
        OutError = Exporter != nullptr
            ? std::string("FBX exporter initialize failed: ") +
                Exporter->GetStatus().GetErrorString()
            : "FbxExporter::Create failed";
        if (Exporter != nullptr) Exporter->Destroy();
        return false;
    }
    const bool Success = Exporter->Export(Scene.Scene);
    if (!Success)
        OutError = std::string("FBX export failed: ") +
            Exporter->GetStatus().GetErrorString();
    Exporter->Destroy();
    if (!Success)
    {
        std::filesystem::remove(OutputPath, Error);
        return false;
    }
    return true;
}

double TranslationError(
    const TransformRT& Left,
    const TransformRT& Right)
{
    return Distance(Left.TranslationCm, Right.TranslationCm);
}

double RotationErrorDegrees(
    const TransformRT& Left,
    const TransformRT& Right)
{
    const Quat A = Normalize(Left.Rotation);
    const Quat B = Normalize(Right.Rotation);
    const double DirectSquared =
        (A.X - B.X) * (A.X - B.X) +
        (A.Y - B.Y) * (A.Y - B.Y) +
        (A.Z - B.Z) * (A.Z - B.Z) +
        (A.W - B.W) * (A.W - B.W);
    const double NegatedSquared =
        (A.X + B.X) * (A.X + B.X) +
        (A.Y + B.Y) * (A.Y + B.Y) +
        (A.Z + B.Z) * (A.Z + B.Z) +
        (A.W + B.W) * (A.W + B.W);
    const double Chord =
        std::sqrt(std::min(
            DirectSquared, NegatedSquared));
    return 4.0 * std::asin(
        std::clamp(Chord * 0.5, 0.0, 1.0)) *
        180.0 / Pi;
}

double ScaleError(
    const TransformRT& Left,
    const TransformRT& Right)
{
    return std::max({
        std::abs(Left.Scale.X - Right.Scale.X),
        std::abs(Left.Scale.Y - Right.Scale.Y),
        std::abs(Left.Scale.Z - Right.Scale.Z)});
}

bool VerifyExport(
    const std::filesystem::path& OutputPath,
    const std::vector<RetargetReviewBone>& TargetBones,
    const RetargetReviewClipView& Clip,
    ReviewPoseLane Lane,
    const RetargetReviewPackageOptions& Options,
    const MeshSceneFingerprint& InputFingerprint,
    ExportVerification& Out,
    std::string& OutError)
{
    LoadedScene Imported;
    if (!LoadScene(
            OutputPath, "skrtg_ueik_export_reimport",
            Options.NormalizeFbxToUEJsonSpace,
            Imported, OutError))
        return false;
    auto Cleanup = [&]() { DestroyScene(Imported); };
    MeshSceneFingerprint OutputFingerprint;
    if (!BuildMeshFingerprint(
            Imported.Scene, OutputFingerprint,
            Options.ContractKind == "ue_ik_json_v1"
                ? 1.0e-5
                : 0.0,
            OutError))
    {
        Cleanup();
        return false;
    }
    Out.InputMeshFingerprint = InputFingerprint.Sha256;
    Out.OutputMeshFingerprint = OutputFingerprint.Sha256;
    if (InputFingerprint.Sha256 != OutputFingerprint.Sha256 ||
        InputFingerprint.MeshCount != OutputFingerprint.MeshCount ||
        InputFingerprint.ControlPointCount !=
            OutputFingerprint.ControlPointCount ||
        InputFingerprint.PolygonCount !=
            OutputFingerprint.PolygonCount ||
        InputFingerprint.SkinClusterCount !=
            OutputFingerprint.SkinClusterCount ||
        InputFingerprint.SkinControlPointIndexCount !=
            OutputFingerprint.SkinControlPointIndexCount ||
        InputFingerprint.BindPoseCount !=
            OutputFingerprint.BindPoseCount)
    {
        Out.Errors.push_back(
            "TARGET_MESH_SKIN_BIND_FINGERPRINT_MISMATCH");
    }
    if (Imported.Scene->GetSrcObjectCount<FbxAnimStack>() != 1)
        Out.Errors.push_back("EXPORT_STACK_COUNT_MISMATCH");
    FbxAnimStack* Stack =
        Imported.Scene->GetSrcObject<FbxAnimStack>(0);
    if (Stack == nullptr ||
        Stack->GetMemberCount<FbxAnimLayer>() != 1)
        Out.Errors.push_back("EXPORT_LAYER_COUNT_MISMATCH");
    if (Stack != nullptr)
    {
        Imported.Scene->SetCurrentAnimationStack(Stack);
        Imported.Scene->GetAnimationEvaluator()->Reset();
    }
    std::map<std::string, FbxNode*> ByPath;
    std::map<FbxNode*, std::string> Paths;
    BuildPathMaps(Imported.Scene, ByPath, Paths);
    std::vector<FbxNode*> Nodes;
    for (const RetargetReviewBone& Bone : TargetBones)
    {
        const auto It = ByPath.find(Bone.Path);
        if (It == ByPath.end())
        {
            Out.Errors.push_back(
                "EXPORT_TARGET_HIERARCHY_PATH_MISSING");
            break;
        }
        Nodes.push_back(It->second);
    }
    const FbxTime::EMode TimeMode =
        TimeModeForFps(Clip.FramesPerSecond);
    if (Nodes.size() == TargetBones.size())
    {
        for (const RetargetReviewFrameView& Frame : Clip.Frames)
        {
            FbxTime Time;
            Time.SetFrame(Frame.FrameIndex, TimeMode);
            for (std::size_t BoneIndex = 0;
                 BoneIndex < Nodes.size(); ++BoneIndex)
            {
                TransformRT Local = ToTransformRT(
                    Nodes[BoneIndex]->EvaluateLocalTransform(
                        Time, FbxNode::eSourcePivot,
                        false, true));
                TransformRT Model = ToTransformRT(
                    Nodes[BoneIndex]->EvaluateGlobalTransform(
                        Time, FbxNode::eSourcePivot,
                        false, true));
                if (Options.NormalizeFbxToUEJsonSpace)
                {
                    Local = ReflectFbxYBasis(Local);
                    Model = ReflectFbxYBasis(Model);
                }
                const core::animation::PoseBuffer* ExpectedLocalPose =
                    LocalPoseForLane(Frame, Lane);
                const core::animation::PoseBuffer* ExpectedModelPose =
                    ModelPoseForLane(Frame, Lane);
                if (ExpectedLocalPose == nullptr ||
                    ExpectedModelPose == nullptr)
                {
                    Out.Errors.push_back(
                        "EXPORT_EXPECTED_POSE_LANE_MISSING");
                    break;
                }
                const TransformRT& ExpectedLocal =
                    (*ExpectedLocalPose)[BoneIndex];
                const TransformRT& ExpectedModel =
                    (*ExpectedModelPose)[BoneIndex];
                const double LT =
                    TranslationError(ExpectedLocal, Local);
                const double LR =
                    RotationErrorDegrees(ExpectedLocal, Local);
                const double LS =
                    ScaleError(ExpectedLocal, Local);
                const double MT =
                    TranslationError(ExpectedModel, Model);
                const double MR =
                    RotationErrorDegrees(ExpectedModel, Model);
                const double MS =
                    ScaleError(ExpectedModel, Model);
                ++Out.ComparedSamples;
                Out.MaximumLocalTranslationCm =
                    std::max(
                        Out.MaximumLocalTranslationCm, LT);
                Out.MaximumLocalRotationDegrees =
                    std::max(
                        Out.MaximumLocalRotationDegrees, LR);
                Out.MaximumLocalScale =
                    std::max(Out.MaximumLocalScale, LS);
                Out.MaximumModelTranslationCm =
                    std::max(
                        Out.MaximumModelTranslationCm, MT);
                Out.MaximumModelRotationDegrees =
                    std::max(
                        Out.MaximumModelRotationDegrees, MR);
                Out.MaximumModelScale =
                    std::max(Out.MaximumModelScale, MS);
                if (LT > Options.ExportLocalTranslationToleranceCm ||
                    LR > Options.ExportRotationToleranceDegrees ||
                    LS > Options.ExportScaleTolerance)
                    ++Out.LocalMismatchCount;
                if (MT > Options.ExportModelTranslationToleranceCm ||
                    MR > Options.ExportRotationToleranceDegrees ||
                    MS > Options.ExportScaleTolerance)
                    ++Out.ModelMismatchCount;
            }
        }
    }
    if (Out.LocalMismatchCount != 0)
        Out.Errors.push_back(
            "EXPORT_LOCAL_TRANSFORM_MISMATCH");
    if (Out.ModelMismatchCount != 0)
        Out.Errors.push_back(
            "EXPORT_MODEL_TRANSFORM_MISMATCH");
    Out.Success = Out.Errors.empty();
    Cleanup();
    return true;
}

std::string BuildAnimationManifest(
    const RetargetReviewPackageOptions& Options,
    const std::vector<std::string>& FoundationExportHashes,
    const std::vector<std::string>& FinalExportHashes)
{
    std::ostringstream Json;
    Json << std::setprecision(12);
    Json << "{\n  \"schema\":\"skrtg.d1_17b_animation_manifest.v1\",\n"
         << "  \"provided_clip_count\":" << Options.Clips.size()
         << ",\n  \"animation_switch_control_created\":true,\n"
         << "  \"shared_source_mesh_fallback_for_meshless_clips_enabled\":"
         << (Options.AllowSharedSourceMeshFallbackForMeshlessClips
                 ? "true" : "false")
         << ",\n"
         << "  \"visible_switch_requires_multiple_clips\":"
         << (Options.Clips.size() > 1 ? "false" : "true")
         << ",\n  \"clips\":[";
    for (std::size_t Index = 0;
         Index < Options.Clips.size(); ++Index)
    {
        if (Index) Json << ",";
        const RetargetReviewClipView& Clip =
            Options.Clips[Index];
        Json << "{\"id\":\"" << JsonEscape(Clip.Id)
             << "\",\"label\":\"" << JsonEscape(Clip.Label)
             << "\",\"fps\":" << Clip.FramesPerSecond
             << ",\"start_frame\":"
             << Clip.Frames.front().FrameIndex
             << ",\"stop_frame\":"
             << Clip.Frames.back().FrameIndex
             << ",\"frame_count\":" << Clip.Frames.size()
             << ",\"source_direct_bone_count\":"
             << Clip.SourceDirectBoneCount
              << ",\"source_rest_passthrough_bone_count\":"
               << Clip.SourceRestPassthroughBoneCount
               << ",\"limb_ik\":{\"status\":\""
               << (LimbIkTelemetryCommittedForContract(Options, Clip)
                       ? "committed"
                       : "fail_closed")
              << "\",\"unit_scale_shadow_projection_applied\":"
              << (Clip.LimbIkUnitScaleShadowProjectionApplied
                      ? "true" : "false")
              << ",\"family_transactions\":"
              << Clip.LimbIkFamilyTransactions
              << ",\"committed_family_transactions\":"
              << Clip.LimbIkCommittedFamilyTransactions
              << ",\"rolled_back_family_transactions\":"
              << Clip.LimbIkRolledBackFamilyTransactions
              << ",\"applied_chain_records\":"
              << Clip.LimbIkAppliedChainRecords
              << ",\"fail_closed_chain_records\":"
              << Clip.LimbIkFailClosedChainRecords
              << ",\"maximum_endpoint_error_cm\":"
              << Clip.LimbIkMaximumEndpointErrorCm
              << ",\"maximum_shadow_to_real_position_delta_cm\":"
              << Clip.LimbIkMaximumShadowToRealPositionDeltaCm
              << "}"
              << ",\"source_animation_path\":\""
             << JsonEscape(
                    (Clip.SourceAnimationFbxPath.empty()
                        ? Options.SourceAnimationFbxPath
                         : Clip.SourceAnimationFbxPath)
                        .string())
             << "\",\"source_animation_sha256\":\""
             << JsonEscape(Clip.SourceAnimationSha256)
             << "\",\"foundation_export_fbx\":\""
             << JsonEscape(Clip.FoundationExportFbxFileName)
             << "\",\"foundation_export_fbx_sha256\":\""
             << JsonEscape(FoundationExportHashes[Index])
             << "\",\"final_export_fbx\":\""
             << JsonEscape(Clip.ExportFbxFileName)
             << "\",\"final_export_fbx_sha256\":\""
             << JsonEscape(FinalExportHashes[Index])
             << "\",\"source_motion_foot_lock\":{"
             << "\"enabled\":"
             << (Clip.SourceMotionFootLockEnabled ? "true" : "false")
             << ",\"success\":"
             << (Clip.SourceMotionFootLockSuccess ? "true" : "false")
             << ",\"deterministic\":"
             << (Clip.SourceMotionFootLockDeterministic ? "true" : "false")
             << ",\"no_ground_or_contact_semantics_used\":"
             << (Clip.SourceMotionFootLockNoGroundOrContactSemanticsUsed
                     ? "true" : "false")
             << ",\"committed_frames\":"
             << Clip.SourceMotionFootLockCommittedFrames
             << ",\"rolled_back_frames\":"
             << Clip.SourceMotionFootLockRolledBackFrames
             << ",\"source_no_motion_deltas\":"
             << Clip.SourceMotionFootLockPositionNoMotionDeltas
             << ",\"source_motion_deltas\":"
             << Clip.SourceMotionFootLockPositionMotionDeltas
             << ",\"source_rotation_no_motion_deltas\":"
             << Clip.SourceMotionFootLockRotationNoMotionDeltas
             << ",\"source_rotation_motion_deltas\":"
             << Clip.SourceMotionFootLockRotationMotionDeltas
             << ",\"rotation_gate_releases\":"
             << Clip.SourceMotionFootLockRotationGateReleases
             << ",\"maximum_released_foundation_rotation_degrees\":"
             << Clip.SourceMotionFootLockMaximumReleasedFoundationRotationDegrees
             << ",\"maximum_real_end_orientation_error_degrees\":"
             << Clip.SourceMotionFootLockMaximumRealEndOrientationErrorDegrees
             << "}}";
    }
    Json << "],\n  \"foundation_route\":\""
         << JsonEscape(Options.FoundationRouteId)
         << "\",\n  \"foundation_frozen\":"
         << (Options.FoundationFrozen ? "true" : "false")
         << ",\n  \"source_motion_foot_lock_route\":\""
         << JsonEscape(Options.SourceMotionFootLockRouteId)
         << "\",\n  \"source_motion_foot_lock_candidate_enabled\":"
         << (Options.SourceMotionFootLockCandidateEnabled
                 ? "true" : "false")
         << ",\n  \"source_motion_foot_lock_candidate_selected\":"
         << (Options.SourceMotionFootLockCandidateSelected
                 ? "true" : "false")
         << ",\n  \"source_motion_foot_lock_candidate_adopted\":"
         << (Options.SourceMotionFootLockCandidateAdopted
                 ? "true" : "false")
         << ",\n  \"route_selected\":false,\n"
         << "  \"upstream_limb_ik_route_selected\":"
         << (Options.UpstreamLimbIkRouteSelected ? "true" : "false")
         << ",\n  \"upstream_limb_ik_route_adopted\":"
         << (Options.UpstreamLimbIkRouteAdopted ? "true" : "false")
         << ",\n  \"spine_pelvis_candidate_enabled\":"
         << (Options.SpinePelvisFollowCandidateEnabled
                 ? "true" : "false")
         << ",\n  \"spine_pelvis_candidate_selected\":"
         << (Options.SpinePelvisFollowCandidateSelected
                 ? "true" : "false")
         << ",\n  \"spine_pelvis_candidate_adopted\":"
         << (Options.SpinePelvisFollowCandidateAdopted
                 ? "true" : "false") << ",\n"
         << "  \"scale_policy_selected_by_user\":"
         << (Options.ScalePolicySelectedByUser ? "true" : "false")
         << ",\n"
         << "  \"route_adopted\":false,\n"
         << "  \"stage_complete\":false\n}\n";
    return Json.str();
}

std::string BuildVerificationJson(
    const RetargetReviewPackageOptions& Options,
    const std::vector<FileIdentity>& SourceIdentities,
    const FileIdentity& TargetIdentity,
    const std::vector<ReviewMeshPackage>& SourceMeshes,
    const std::vector<bool>& SourceMeshFallbackUsed,
    const ReviewMeshPackage& TargetMesh,
    const std::vector<std::filesystem::path>& FoundationExportPaths,
    const std::vector<std::string>& FoundationExportHashes,
    const std::vector<ExportVerification>& FoundationVerifications,
    const std::vector<std::filesystem::path>& FinalExportPaths,
    const std::vector<std::string>& FinalExportHashes,
    const std::vector<ExportVerification>& FinalVerifications)
{
    const int LimbIkGoalCount = static_cast<int>(std::count_if(
        Options.RetargetChains.begin(),
        Options.RetargetChains.end(),
        [](const RetargetReviewChain& Chain)
        {
            return Chain.IkMode == "two_bone";
        }));
    const int FingerIkGoalCount = static_cast<int>(std::count_if(
        Options.RetargetChains.begin(),
        Options.RetargetChains.end(),
        [](const RetargetReviewChain& Chain)
        {
            return Chain.IkMode == "finger";
        }));
    std::ostringstream Json;
    Json << std::setprecision(17);
    Json << "{\n  \"schema\":\"skrtg.d1_17b_mesh_and_fbx_export_verification.v1\",\n"
         << "  \"status\":\"pass\",\n"
         << "  \"source_animation\":{\"path\":\""
         << JsonEscape(SourceIdentities.front().Path.string())
         << "\",\"sha256\":\""
         << SourceIdentities.front().PreSha256
         << "\",\"unchanged\":true},\n"
         << "  \"source_animations\":[";
    for (std::size_t Index = 0;
         Index < SourceIdentities.size(); ++Index)
    {
        if (Index) Json << ",";
        Json << "{\"clip_id\":\""
             << JsonEscape(Options.Clips[Index].Id)
             << "\",\"path\":\""
             << JsonEscape(SourceIdentities[Index].Path.string())
             << "\",\"sha256\":\""
             << SourceIdentities[Index].PreSha256
             << "\",\"unchanged\":true}";
    }
    Json << "],\n"
         << "  \"target_tpose\":{\"path\":\""
         << JsonEscape(TargetIdentity.Path.string())
         << "\",\"sha256\":\"" << TargetIdentity.PreSha256
         << "\",\"unchanged\":true},\n"
         << "  \"source_mesh\":{\"mesh_count\":"
         << SourceMeshes.front().Meshes.size()
         << ",\"control_points\":"
         << SourceMeshes.front().ControlPointCount
         << ",\"triangles\":"
         << SourceMeshes.front().TriangleCount
         << ",\"skin_clusters\":"
         << SourceMeshes.front().SkinClusterCount
         << ",\"influences\":"
         << SourceMeshes.front().InfluenceCount
         << ",\"maximum_influences_per_control_point\":"
         << SourceMeshes.front().MaximumInfluencesPerControlPoint
         << ",\"maximum_bind_reconstruction_error_cm\":"
         << SourceMeshes.front().MaximumBindReconstructionErrorCm
         << "},\n  \"source_meshes\":[";
    for (std::size_t Index = 0;
         Index < SourceMeshes.size(); ++Index)
    {
        if (Index) Json << ",";
        const ReviewMeshPackage& SourceMesh = SourceMeshes[Index];
        Json << "{\"clip_id\":\""
             << JsonEscape(Options.Clips[Index].Id)
             << "\",\"shared_mesh_fallback_used\":"
             << (SourceMeshFallbackUsed[Index]
                     ? "true" : "false")
             << ",\"source_direct_bone_count\":"
             << Options.Clips[Index].SourceDirectBoneCount
             << ",\"source_rest_passthrough_bone_count\":"
             << Options.Clips[Index].SourceRestPassthroughBoneCount
             << ",\"mesh_count\":" << SourceMesh.Meshes.size()
             << ",\"control_points\":" << SourceMesh.ControlPointCount
             << ",\"triangles\":" << SourceMesh.TriangleCount
             << ",\"skin_clusters\":" << SourceMesh.SkinClusterCount
             << ",\"influences\":" << SourceMesh.InfluenceCount
             << ",\"maximum_influences_per_control_point\":"
             << SourceMesh.MaximumInfluencesPerControlPoint
             << ",\"maximum_bind_reconstruction_error_cm\":"
             << SourceMesh.MaximumBindReconstructionErrorCm << "}";
    }
    Json << "],\n  \"target_mesh\":{\"mesh_count\":"
         << TargetMesh.Meshes.size()
         << ",\"control_points\":" << TargetMesh.ControlPointCount
         << ",\"triangles\":" << TargetMesh.TriangleCount
         << ",\"skin_clusters\":" << TargetMesh.SkinClusterCount
         << ",\"influences\":" << TargetMesh.InfluenceCount
         << ",\"maximum_influences_per_control_point\":"
         << TargetMesh.MaximumInfluencesPerControlPoint
         << ",\"maximum_bind_reconstruction_error_cm\":"
         << TargetMesh.MaximumBindReconstructionErrorCm
         << "},\n  \"exports\":[";
    bool FirstExport = true;
    const auto WriteExports =
        [&](const char* Lane,
            const std::vector<std::filesystem::path>& Paths,
            const std::vector<std::string>& Hashes,
            const std::vector<ExportVerification>& Verifications)
    {
        for (std::size_t Index = 0;
             Index < Verifications.size(); ++Index)
        {
        if (!FirstExport) Json << ",";
        FirstExport = false;
        const ExportVerification& V = Verifications[Index];
        Json << "{\"clip_id\":\""
             << JsonEscape(Options.Clips[Index].Id)
             << "\",\"lane\":\"" << Lane
             << "\",\"path\":\""
             << JsonEscape(Paths[Index].string())
             << "\",\"sha256\":\"" << Hashes[Index]
             << "\",\"mesh_fingerprint_before\":\""
             << V.InputMeshFingerprint
             << "\",\"mesh_fingerprint_after\":\""
             << V.OutputMeshFingerprint
             << "\",\"samples_compared\":" << V.ComparedSamples
             << ",\"local_mismatch_count\":"
             << V.LocalMismatchCount
             << ",\"model_mismatch_count\":"
             << V.ModelMismatchCount
             << ",\"max_local_translation_cm\":"
             << V.MaximumLocalTranslationCm
             << ",\"max_local_rotation_degrees\":"
             << V.MaximumLocalRotationDegrees
             << ",\"max_local_scale\":"
             << V.MaximumLocalScale
             << ",\"max_model_translation_cm\":"
             << V.MaximumModelTranslationCm
             << ",\"max_model_rotation_degrees\":"
             << V.MaximumModelRotationDegrees
             << ",\"max_model_scale\":"
             << V.MaximumModelScale << "}";
        }
    };
    WriteExports("foundation", FoundationExportPaths,
                 FoundationExportHashes, FoundationVerifications);
    WriteExports("final", FinalExportPaths,
                 FinalExportHashes, FinalVerifications);
    Json << "],\n  \"roundtrip_tolerances\":{"
         << "\"local_translation_cm\":"
         << Options.ExportLocalTranslationToleranceCm << ","
         << "\"model_translation_cm\":"
         << Options.ExportModelTranslationToleranceCm << ","
         << "\"rotation_degrees\":"
         << Options.ExportRotationToleranceDegrees << ","
         << "\"scale\":"
         << Options.ExportScaleTolerance << "},\n"
         << "  \"viewer_contract\":{"
         << "\"mesh_toggle\":true,"
         << "\"skeleton_toggle\":true,"
         << "\"animation_switch_control\":true,"
         << "\"export_button\":true,"
         << "\"foot_lock_op_toggle\":true,"
         << "\"third_column_fixed_to_foundation\":true,"
         << "\"fourth_column_and_export_follow_toggle\":true,"
         << "\"foundation_and_final_fbx_both_roundtrip_verified\":true,"
         << "\"source_motion_only_no_ground_or_contact_semantics\":true,"
         << "\"four_synchronized_columns\":true,"
         << "\"meshless_motion_shared_source_mesh_fallback\":"
         << (Options.AllowSharedSourceMeshFallbackForMeshlessClips
                 ? "true" : "false") << ","
         << "\"missing_display_bones_rest_local_passthrough\":true,"
         << "\"display_passthrough_used_as_solver_evidence\":false,"
         << "\"source_ghost_opacity\":0.1,"
          << "\"result_opacity\":1.0,"
          << "\"limb_ik_execution_status_visible\":true,"
          << "\"limb_ik_full_commit_required_for_generation\":true,"
          << "\"viewer_data_schema\":\"skrtg.d1_17b_retarget_review_viewer.v3\","
         << "\"static_tpose_contract_below_four_columns\":true,"
         << "\"source_target_tpose_diagram_count\":2,"
         << "\"mapped_chain_count\":"
         << Options.RetargetChains.size() << ","
         << "\"limb_ik_goal_count_per_side\":"
         << LimbIkGoalCount << ","
         << "\"finger_ik_goal_count_per_side\":"
         << FingerIkGoalCount << ","
         << "\"goal_marker_count_per_side\":"
         << LimbIkGoalCount + FingerIkGoalCount << ","
         << "\"pole_marker_count_per_side\":"
         << LimbIkGoalCount + FingerIkGoalCount << ","
         << "\"root_pelvis_hips_roles_visible\":true,"
         << "\"static_contract_ignores_animation_frame_camera\":true},\n"
         << "  \"foundation_route\":\""
         << JsonEscape(Options.FoundationRouteId)
         << "\",\n  \"foundation_frozen\":"
         << (Options.FoundationFrozen ? "true" : "false")
         << ",\n  \"source_motion_foot_lock_route\":\""
         << JsonEscape(Options.SourceMotionFootLockRouteId)
         << "\",\n  \"source_motion_foot_lock_candidate_enabled\":"
         << (Options.SourceMotionFootLockCandidateEnabled
                 ? "true" : "false")
         << ",\n  \"source_motion_foot_lock_candidate_selected\":"
         << (Options.SourceMotionFootLockCandidateSelected
                 ? "true" : "false")
         << ",\n  \"source_motion_foot_lock_candidate_adopted\":"
         << (Options.SourceMotionFootLockCandidateAdopted
                 ? "true" : "false")
         << ",\n  \"scale_policy_selected_by_user\":"
         << (Options.ScalePolicySelectedByUser ? "true" : "false")
         << ",\n"
         << "  \"upstream_limb_ik_route_selected\":"
         << (Options.UpstreamLimbIkRouteSelected ? "true" : "false")
         << ",\n  \"upstream_limb_ik_route_adopted\":"
         << (Options.UpstreamLimbIkRouteAdopted ? "true" : "false")
         << ",\n  \"spine_pelvis_candidate_enabled\":"
         << (Options.SpinePelvisFollowCandidateEnabled
                 ? "true" : "false")
         << ",\n  \"spine_pelvis_candidate_selected\":"
         << (Options.SpinePelvisFollowCandidateSelected
                 ? "true" : "false")
         << ",\n  \"spine_pelvis_candidate_adopted\":"
         << (Options.SpinePelvisFollowCandidateAdopted
                 ? "true" : "false") << ",\n"
         << "  \"route_selected\":false,\n"
         << "  \"route_adopted\":false,\n"
         << "  \"stage_complete\":false,\n"
         << "  \"errors\":[]\n}\n";
    return Json.str();
}
} // namespace

RetargetReviewPackageResult GenerateRetargetReviewPackage(
    const RetargetReviewPackageOptions& Options)
{
    RetargetReviewPackageResult Result;
    std::vector<std::filesystem::path> PartialOutputs;
    auto Fail = [&](const std::string& Message)
    {
        for (const std::filesystem::path& Path : PartialOutputs)
        {
            std::error_code Error;
            std::filesystem::remove(Path, Error);
        }
        Result.Success = false;
        Result.Errors.push_back(Message);
        Result.ConsoleSummary =
            "SKRTG UE IK review package failed closed: " + Message;
        Result.Artifacts.clear();
        return Result;
    };
    if (Options.SourceAnimationFbxPath.empty() ||
        Options.TargetTposeFbxPath.empty() ||
        Options.OutputDirectory.empty() ||
        Options.RouteId.empty() ||
        Options.SourceBones.empty() ||
        Options.TargetBones.empty() ||
        Options.Anchors.size() != 2 ||
        Options.Clips.empty() ||
        !std::isfinite(Options.ExportLocalTranslationToleranceCm) ||
        !std::isfinite(Options.ExportModelTranslationToleranceCm) ||
        !std::isfinite(Options.ExportRotationToleranceDegrees) ||
        !std::isfinite(Options.ExportScaleTolerance) ||
        Options.ExportLocalTranslationToleranceCm <= 0.0 ||
        Options.ExportModelTranslationToleranceCm <= 0.0 ||
        Options.ExportRotationToleranceDegrees <= 0.0 ||
        Options.ExportScaleTolerance <= 0.0)
    {
        return Fail("review package options are incomplete");
    }
    const bool UEJsonContract =
        Options.ContractKind == "ue_ik_json_v1";
    const bool FrozenContract =
        Options.ContractKind == "foundation_v1";
    const bool ContractValid = FrozenContract
        ? Options.FoundationRouteId ==
                "skrtg_fkik_foundation_v1" &&
            Options.FoundationFrozen &&
            Options.UpstreamLimbIkRouteSelected &&
            Options.UpstreamLimbIkRouteAdopted &&
            Options.SpinePelvisFollowCandidateEnabled &&
            Options.SourceMotionFootLockRouteId ==
                "source_motion_foot_lock_no_ground_semantics_v1" &&
            !Options.SourceMotionFootLockCandidateSelected &&
            !Options.SourceMotionFootLockCandidateAdopted &&
            (!Options.SourceMotionFootLockCandidateEnabled ||
             Options.RouteId.find(
                 Options.SourceMotionFootLockRouteId) !=
                 std::string::npos)
        : UEJsonContract &&
            Options.RouteId ==
                "ue_ik_json_canonical_bridge_v1" &&
            Options.FoundationRouteId ==
                "ue_ik_json_fk_pelvis_limb_ik_candidate_v1" &&
            !Options.FoundationFrozen &&
            !Options.SourceMotionFootLockCandidateEnabled &&
            !Options.SourceMotionFootLockCandidateSelected &&
            !Options.SourceMotionFootLockCandidateAdopted &&
            Options.NormalizeFbxToUEJsonSpace;
    if (!ContractValid)
    {
        return Fail(
            "review Foundation or route contract is invalid");
    }
    const auto FinitePosition = [](const Vec3& Value)
    {
        return std::isfinite(Value.X) &&
            std::isfinite(Value.Y) &&
            std::isfinite(Value.Z);
    };
    if (std::any_of(
            Options.SourceBones.begin(), Options.SourceBones.end(),
            [&](const RetargetReviewBone& Bone)
            {
                return !FinitePosition(Bone.RestModelPositionCm);
            }) ||
        std::any_of(
            Options.TargetBones.begin(), Options.TargetBones.end(),
            [&](const RetargetReviewBone& Bone)
            {
                return !FinitePosition(Bone.RestModelPositionCm);
            }))
    {
        return Fail("review T-pose rest positions are missing or non-finite");
    }
    const RetargetReviewRootPelvisContract& RootPelvis =
        Options.RootPelvisContract;
    const auto SourceIndexValid = [&](int Index)
    {
        return Index >= 0 &&
            Index < static_cast<int>(Options.SourceBones.size());
    };
    const auto TargetIndexValid = [&](int Index)
    {
        return Index >= 0 &&
            Index < static_cast<int>(Options.TargetBones.size());
    };
    if (!SourceIndexValid(RootPelvis.SourceRootBoneIndex) ||
        !SourceIndexValid(RootPelvis.SourcePelvisBoneIndex) ||
        !TargetIndexValid(RootPelvis.TargetHipsBoneIndex) ||
        Options.SourceBones[static_cast<std::size_t>(
            RootPelvis.SourceRootBoneIndex)].ParentIndex != -1 ||
        (!UEJsonContract &&
         (RootPelvis.SourceRootBoneIndex ==
              RootPelvis.SourcePelvisBoneIndex ||
          Options.SourceBones[static_cast<std::size_t>(
              RootPelvis.SourcePelvisBoneIndex)].ParentIndex !=
              RootPelvis.SourceRootBoneIndex ||
          Options.TargetBones[static_cast<std::size_t>(
              RootPelvis.TargetHipsBoneIndex)].ParentIndex != -1)) ||
        (UEJsonContract &&
         RootPelvis.SourceRootBoneIndex !=
             RootPelvis.SourcePelvisBoneIndex &&
         Options.SourceBones[static_cast<std::size_t>(
             RootPelvis.SourcePelvisBoneIndex)].ParentIndex < 0) ||
        RootPelvis.RootOwnership.empty() ||
        RootPelvis.PelvisOwnership.empty() ||
        RootPelvis.ScaleOwnership.empty())
    {
        return Fail("review Root/Pelvis/Hips ownership contract is invalid");
    }
    std::set<std::string> ReviewChainLabels;
    int TwoBoneChainCount = 0;
    int FingerChainCount = 0;
    for (const RetargetReviewChain& Chain : Options.RetargetChains)
    {
        const bool ModeValid = Chain.IkMode == "fk_only" ||
            Chain.IkMode == "two_bone" ||
            Chain.IkMode == "finger";
        if (Chain.Label.empty() || !ModeValid ||
            Chain.SourceBoneIndices.empty() ||
            Chain.TargetBoneIndices.empty() ||
            !ReviewChainLabels.insert(Chain.Label).second ||
            std::any_of(
                Chain.SourceBoneIndices.begin(),
                Chain.SourceBoneIndices.end(),
                [&](int Index) { return !SourceIndexValid(Index); }) ||
            std::any_of(
                Chain.TargetBoneIndices.begin(),
                Chain.TargetBoneIndices.end(),
                [&](int Index) { return !TargetIndexValid(Index); }))
        {
            return Fail("review retarget chain inventory is invalid");
        }
        for (std::size_t Index = 1;
             Index < Chain.SourceBoneIndices.size(); ++Index)
        {
            if (Options.SourceBones[static_cast<std::size_t>(
                    Chain.SourceBoneIndices[Index])].ParentIndex !=
                Chain.SourceBoneIndices[Index - 1])
            {
                return Fail("review source chain is not parent-contiguous: " +
                            Chain.Label);
            }
        }
        for (std::size_t Index = 1;
             Index < Chain.TargetBoneIndices.size(); ++Index)
        {
            if (Options.TargetBones[static_cast<std::size_t>(
                    Chain.TargetBoneIndices[Index])].ParentIndex !=
                Chain.TargetBoneIndices[Index - 1])
            {
                return Fail("review target chain is not parent-contiguous: " +
                            Chain.Label);
            }
        }
        if (Chain.IkMode == "two_bone" ||
            Chain.IkMode == "finger")
        {
            if (!SourceIndexValid(Chain.SourceGoalBoneIndex) ||
                !TargetIndexValid(Chain.TargetGoalBoneIndex) ||
                !SourceIndexValid(Chain.SourcePoleBoneIndex) ||
                !TargetIndexValid(Chain.TargetPoleBoneIndex) ||
                Chain.SourceGoalName.empty() ||
                Chain.TargetGoalName.empty())
            {
                return Fail("review IK Goal/pole contract is invalid: " +
                            Chain.Label);
            }
            if (Chain.IkMode == "two_bone") ++TwoBoneChainCount;
            else ++FingerChainCount;
        }
    }
    const bool ChainInventoryValid = UEJsonContract
        ? !Options.RetargetChains.empty() &&
            TwoBoneChainCount == 4 &&
            FingerChainCount == 0
        : Options.RetargetChains.size() == 21 &&
            TwoBoneChainCount == 4 &&
            FingerChainCount == 10;
    if (!ChainInventoryValid)
    {
        return Fail(UEJsonContract
            ? "UE IK JSON review contract requires explicit mapped chains, four limb IK Goals, and no implicit finger IK"
            : "review contract requires 21 mapped chains, four limb IK Goals, and ten finger IK Goals");
    }
    for (const RetargetReviewAnchor& Anchor : Options.Anchors)
    {
        if (Anchor.SourceBoneIndex < 0 ||
            Anchor.SourceBoneIndex >=
                static_cast<int>(Options.SourceBones.size()) ||
            Anchor.TargetBoneIndex < 0 ||
            Anchor.TargetBoneIndex >=
                static_cast<int>(Options.TargetBones.size()) ||
            !Finite(Anchor.SourceToTargetRestBasis))
        {
            return Fail("review anchor contract is invalid");
        }
    }
    std::set<std::string> ClipIds;
    std::set<std::string> ExportNames;
    for (const RetargetReviewClipView& Clip : Options.Clips)
    {
        std::string Error;
        if (!ValidateClip(Options, Clip, Error))
            return Fail(Error);
        if (!ClipIds.insert(Clip.Id).second ||
            !ExportNames.insert(
                Clip.FoundationExportFbxFileName).second ||
            !ExportNames.insert(Clip.ExportFbxFileName).second ||
            std::filesystem::path(
                Clip.FoundationExportFbxFileName).filename() !=
                std::filesystem::path(
                    Clip.FoundationExportFbxFileName) ||
            std::filesystem::path(
                Clip.ExportFbxFileName).filename() !=
                std::filesystem::path(Clip.ExportFbxFileName))
        {
            return Fail(
                "clip ID or export filename is duplicated/unsafe");
        }
    }
    std::error_code DirectoryError;
    std::filesystem::create_directories(
        Options.OutputDirectory, DirectoryError);
    if (DirectoryError)
        return Fail("failed to access review output directory");

    std::string Error;
    std::vector<FileIdentity> SourceIdentities;
    SourceIdentities.reserve(Options.Clips.size());
    FileIdentity SourceMeshFallbackIdentity;
    const bool HasSourceMeshFallback =
        !Options.SourceMeshFallbackFbxPath.empty() ||
        !Options.SourceMeshFallbackExpectedSha256.empty();
    if (HasSourceMeshFallback &&
        (Options.SourceMeshFallbackFbxPath.empty() ||
         Options.SourceMeshFallbackExpectedSha256.empty()))
    {
        return Fail(
            "source Mesh fallback requires both a path and expected SHA256");
    }
    FileIdentity TargetIdentity;
    for (const RetargetReviewClipView& Clip : Options.Clips)
    {
        FileIdentity Identity;
        const std::filesystem::path SourcePath =
            Clip.SourceAnimationFbxPath.empty()
                ? Options.SourceAnimationFbxPath
                : Clip.SourceAnimationFbxPath;
        const std::string ExpectedSha256 =
            Clip.SourceAnimationSha256.empty()
                ? Options.SourceAnimationExpectedSha256
                : Clip.SourceAnimationSha256;
        if (!CaptureIdentityBefore(
                SourcePath, ExpectedSha256,
                Identity, Error))
            return Fail(Error);
        SourceIdentities.push_back(std::move(Identity));
    }
    if (HasSourceMeshFallback &&
        !CaptureIdentityBefore(
            Options.SourceMeshFallbackFbxPath,
            Options.SourceMeshFallbackExpectedSha256,
            SourceMeshFallbackIdentity, Error))
    {
        return Fail(Error);
    }
    if (!CaptureIdentityBefore(
            Options.TargetTposeFbxPath,
            Options.TargetTposeExpectedSha256,
            TargetIdentity, Error))
    {
        return Fail(Error);
    }

    std::vector<ReviewMeshPackage> SourceMeshes;
    SourceMeshes.reserve(SourceIdentities.size());
    std::vector<bool> SourceMeshFallbackUsed;
    SourceMeshFallbackUsed.reserve(SourceIdentities.size());
    for (std::size_t Index = 0;
         Index < SourceIdentities.size(); ++Index)
    {
        // Exact UE animation frames are sourced from the UE golden JSON,
        // while the original Mixamo animation FBX may remain Y-up and carry
        // PreRotation. Use the hash-bound UE-exported source rest FBX as the
        // display mesh/bind provider so the viewer never mixes that native
        // Mixamo basis with UE model poses.
        const bool UseExplicitUEJsonMeshProvider =
            Options.NormalizeFbxToUEJsonSpace &&
            HasSourceMeshFallback;
        const std::filesystem::path& SourceMeshPath =
            UseExplicitUEJsonMeshProvider
            ? SourceMeshFallbackIdentity.Path
            : SourceIdentities[Index].Path;
        LoadedScene SourceScene;
        if (!LoadScene(
                SourceMeshPath,
                "skrtg_ueik_source_mesh_extraction",
                Options.NormalizeFbxToUEJsonSpace,
                SourceScene, Error))
        {
            DestroyScene(SourceScene);
            return Fail(Error);
        }
        std::vector<FbxNode*> SourceMeshNodes;
        CollectMeshNodes(
            SourceScene.Scene->GetRootNode(), SourceMeshNodes);
        if (SourceMeshNodes.empty())
        {
            DestroyScene(SourceScene);
            if (!Options.AllowSharedSourceMeshFallbackForMeshlessClips)
            {
                return Fail(
                    "source animation contains no Mesh and shared source Mesh fallback is unavailable: " +
                    Options.Clips[Index].Id);
            }
            if (!SourceMeshes.empty())
            {
                SourceMeshes.push_back(SourceMeshes.front());
                SourceMeshFallbackUsed.push_back(true);
                continue;
            }
            if (!HasSourceMeshFallback)
            {
                return Fail(
                    "the first source animation contains no Mesh and no explicit source Mesh fallback was supplied: " +
                    Options.Clips[Index].Id);
            }
            LoadedScene FallbackScene;
            if (!LoadScene(
                    SourceMeshFallbackIdentity.Path,
                    "skrtg_ueik_source_mesh_fallback",
                    Options.NormalizeFbxToUEJsonSpace,
                    FallbackScene, Error))
            {
                DestroyScene(FallbackScene);
                return Fail(Error);
            }
            std::vector<FbxNode*> FallbackMeshNodes;
            CollectMeshNodes(
                FallbackScene.Scene->GetRootNode(), FallbackMeshNodes);
            if (FallbackMeshNodes.empty())
            {
                DestroyScene(FallbackScene);
                return Fail(
                    "explicit source Mesh fallback contains no Mesh");
            }
            ReviewMeshPackage SourceMesh;
            if (!ExtractMeshPackage(
                    FallbackScene.Scene, FallbackScene.Manager,
                    Options.SourceBones,
                    "source_mesh_fallback", false,
                    Options.NormalizeFbxToUEJsonSpace,
                    Options.NormalizeFbxToUEJsonSpace,
                    SourceMesh, Error))
            {
                DestroyScene(FallbackScene);
                return Fail(Error);
            }
            DestroyScene(FallbackScene);
            SourceMeshes.push_back(std::move(SourceMesh));
            SourceMeshFallbackUsed.push_back(true);
            continue;
        }
        ReviewMeshPackage SourceMesh;
        if (!ExtractMeshPackage(
                SourceScene.Scene, SourceScene.Manager,
                Options.SourceBones,
                UseExplicitUEJsonMeshProvider
                    ? "source_mesh_ue_export_provider"
                    : "source_animation_" +
                        Options.Clips[Index].Id,
                false, Options.NormalizeFbxToUEJsonSpace,
                UseExplicitUEJsonMeshProvider,
                SourceMesh, Error))
        {
            DestroyScene(SourceScene);
            return Fail(Error);
        }
        DestroyScene(SourceScene);
        SourceMeshes.push_back(std::move(SourceMesh));
        SourceMeshFallbackUsed.push_back(
            UseExplicitUEJsonMeshProvider);
    }

    LoadedScene TargetExtractionScene;
    if (!LoadScene(
            TargetIdentity.Path,
            "skrtg_ueik_target_mesh_extraction",
            Options.NormalizeFbxToUEJsonSpace,
            TargetExtractionScene, Error))
    {
        DestroyScene(TargetExtractionScene);
        return Fail(Error);
    }
    ReviewMeshPackage TargetMesh;
    if (!ExtractMeshPackage(
            TargetExtractionScene.Scene,
            TargetExtractionScene.Manager,
            Options.TargetBones, "target_tpose", true,
            Options.NormalizeFbxToUEJsonSpace,
            false,
            TargetMesh, Error))
    {
        DestroyScene(TargetExtractionScene);
        return Fail(Error);
    }
    DestroyScene(TargetExtractionScene);

    std::vector<std::filesystem::path> FoundationExportPaths;
    std::vector<std::string> FoundationExportHashes;
    std::vector<ExportVerification> FoundationVerifications;
    std::vector<std::filesystem::path> FinalExportPaths;
    std::vector<std::string> FinalExportHashes;
    std::vector<ExportVerification> FinalVerifications;
    for (const RetargetReviewClipView& Clip : Options.Clips)
    {
        for (const ReviewPoseLane Lane :
             {ReviewPoseLane::Foundation, ReviewPoseLane::Final})
        {
            LoadedScene ExportSceneData;
            if (!LoadScene(
                    TargetIdentity.Path,
                    "skrtg_ueik_target_mesh_export",
                    Options.NormalizeFbxToUEJsonSpace,
                    ExportSceneData, Error))
            {
                return Fail(Error);
            }
            MeshSceneFingerprint InputFingerprint;
            if (!BuildMeshFingerprint(
                    ExportSceneData.Scene, InputFingerprint,
                    Options.ContractKind == "ue_ik_json_v1"
                        ? 1.0e-5
                        : 0.0,
                    Error) ||
                !WriteAnimationToTargetScene(
                    ExportSceneData.Scene, Options.TargetBones,
                    Clip, Lane,
                    Options.ContractKind == "ue_ik_json_v1",
                    Options.NormalizeFbxToUEJsonSpace,
                    Error))
            {
                DestroyScene(ExportSceneData);
                return Fail(Error);
            }
            const std::string& ExportName =
                Lane == ReviewPoseLane::Foundation
                ? Clip.FoundationExportFbxFileName
                : Clip.ExportFbxFileName;
            const std::filesystem::path OutputPath =
                Options.OutputDirectory / ExportName;
            if (!ExportScene(
                    ExportSceneData, OutputPath, Error))
            {
                DestroyScene(ExportSceneData);
                return Fail(Error);
            }
            DestroyScene(ExportSceneData);
            PartialOutputs.push_back(OutputPath);
            std::string OutputHash;
            if (!ComputeSha256(OutputPath, OutputHash, Error))
                return Fail(Error);
            ExportVerification Verification;
            if (!VerifyExport(
                    OutputPath, Options.TargetBones, Clip, Lane,
                    Options, InputFingerprint, Verification, Error))
                return Fail(Error);
            if (!Verification.Success)
            {
                std::ostringstream Detail;
                Detail << std::setprecision(17)
                       << (!Verification.Errors.empty()
                               ? Verification.Errors.front()
                               : "FBX export verification failed")
                       << " clip=" << Clip.Id
                       << " lane=" << PoseLaneName(Lane)
                       << " local_mismatches="
                       << Verification.LocalMismatchCount
                       << " model_mismatches="
                       << Verification.ModelMismatchCount
                       << " max_local_t_cm="
                       << Verification.MaximumLocalTranslationCm
                       << " max_local_r_deg="
                       << Verification.MaximumLocalRotationDegrees
                       << " max_local_scale="
                       << Verification.MaximumLocalScale
                       << " max_model_t_cm="
                       << Verification.MaximumModelTranslationCm
                       << " max_model_r_deg="
                       << Verification.MaximumModelRotationDegrees
                       << " max_model_scale="
                       << Verification.MaximumModelScale;
                return Fail(Detail.str());
            }
            if (Lane == ReviewPoseLane::Foundation)
            {
                FoundationExportPaths.push_back(OutputPath);
                FoundationExportHashes.push_back(OutputHash);
                FoundationVerifications.push_back(
                    std::move(Verification));
            }
            else
            {
                FinalExportPaths.push_back(OutputPath);
                FinalExportHashes.push_back(OutputHash);
                FinalVerifications.push_back(
                    std::move(Verification));
            }
        }
    }
    for (FileIdentity& SourceIdentity : SourceIdentities)
    {
        if (!CaptureIdentityAfter(SourceIdentity, Error))
            return Fail(Error);
    }
    if (HasSourceMeshFallback &&
        !CaptureIdentityAfter(SourceMeshFallbackIdentity, Error))
    {
        return Fail(Error);
    }
    if (!CaptureIdentityAfter(TargetIdentity, Error))
    {
        return Fail(Error);
    }

    const std::string ViewerData =
        BuildViewerData(
            Options, SourceMeshes,
            SourceMeshFallbackUsed, TargetMesh);
    const std::string ViewerHtml =
        BuildViewerHtml(ViewerData);
    const std::filesystem::path ViewerPath =
        Options.OutputDirectory / ViewerFileName;
    Result.Artifacts.push_back({
        ViewerPath, ViewerHtml});
    Result.Artifacts.push_back({
        Options.OutputDirectory / AnimationManifestFileName,
        BuildAnimationManifest(
            Options, FoundationExportHashes, FinalExportHashes)});
    Result.Artifacts.push_back({
        Options.OutputDirectory / VerificationFileName,
        BuildVerificationJson(
            Options,
            SourceIdentities, TargetIdentity,
            SourceMeshes, SourceMeshFallbackUsed, TargetMesh,
            FoundationExportPaths, FoundationExportHashes,
            FoundationVerifications,
            FinalExportPaths, FinalExportHashes,
            FinalVerifications)});
    Result.Success = true;
    Result.ViewerPath = ViewerPath;
    Result.ExportedFoundationFbxPath = FoundationExportPaths.front();
    Result.ExportedFoundationFbxSha256 =
        FoundationExportHashes.front();
    Result.ExportedFbxPath = FinalExportPaths.front();
    Result.ExportedFbxSha256 = FinalExportHashes.front();
    Result.ConsoleSummary =
        "SKRTG UE IK review package generated: per-clip source mesh packages=" +
        std::to_string(SourceMeshes.size()) +
        " target meshes=" +
        std::to_string(TargetMesh.Meshes.size()) +
        " clips=" + std::to_string(Options.Clips.size()) +
        " target Mesh+Foundation/Final FBX roundtrips verified";
    Result.Warnings = {
        "The viewer evaluates skin clusters with neutral flat shading; it does not claim texture, material, blend-shape, cloth, or final visual fidelity.",
        Options.Clips.size() == 1
            ? "Only one source animation FBX is currently supplied, so the animation switch control is present but disabled until another reviewed clip is added."
            : "Animation switching is enabled for the supplied reviewed clip manifest.",
        "Both Foundation and configured Final FBXs are pre-generated and roundtrip-verified; the viewer FootLock toggle switches both the fourth viewport and export link. Route selection, independent review, production quality, and readiness remain open."};
    if (std::any_of(
            SourceMeshFallbackUsed.begin(),
            SourceMeshFallbackUsed.end(),
            [](const bool Used) { return Used; }))
    {
        Result.Warnings.push_back(
            Options.ContractKind == "ue_ik_json_v1"
                ? "The exact UE-JSON route displays Original with the explicitly SHA-256-bound UE-exported source-rest Mesh provider, so Viewer skinning and UE-canonical poses use one declared basis; this provider is display-only and is not solver evidence."
                : "At least one meshless source animation is displayed with the explicitly hash-bound source rest Mesh fallback; this fallback is display-only and is not solver evidence.");
    }
    return Result;
}
} // namespace skrtg::fbx
