#include "skrtg/viewer/review_scene.h"

#include "skrtg/viewer/skrv/package.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace skrtg::viewer
{
namespace
{
using Json = nlohmann::json;
constexpr std::size_t TransformStrideFloat32 = 10;
constexpr std::size_t Float32Bytes = 4;

std::string PathText(const std::filesystem::path& Path)
{
    const std::u8string Value = Path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(Value.data()), Value.size());
}

bool CheckedMultiply(
    const std::uint64_t Left,
    const std::uint64_t Right,
    std::uint64_t& Out)
{
    if (Left != 0 &&
        Right > std::numeric_limits<std::uint64_t>::max() / Left)
    {
        return false;
    }
    Out = Left * Right;
    return true;
}

float DecodeLittleEndianFloat32(const std::byte* Bytes)
{
    const auto* Values = reinterpret_cast<const unsigned char*>(Bytes);
    const std::uint32_t Bits =
        static_cast<std::uint32_t>(Values[0]) |
        (static_cast<std::uint32_t>(Values[1]) << 8U) |
        (static_cast<std::uint32_t>(Values[2]) << 16U) |
        (static_cast<std::uint32_t>(Values[3]) << 24U);
    return std::bit_cast<float>(Bits);
}

std::uint32_t DecodeLittleEndianUint32(const std::byte* Bytes)
{
    const auto* Values = reinterpret_cast<const unsigned char*>(Bytes);
    return static_cast<std::uint32_t>(Values[0]) |
        (static_cast<std::uint32_t>(Values[1]) << 8U) |
        (static_cast<std::uint32_t>(Values[2]) << 16U) |
        (static_cast<std::uint32_t>(Values[3]) << 24U);
}

bool ReadVerifiedBlob(
    const std::filesystem::path& PackageDirectory,
    const Json& Descriptor,
    const std::string& ScalarType,
    std::vector<std::byte>& OutBytes,
    std::size_t& OutElementCount,
    std::string& OutError)
{
    if (Descriptor.at("scalarType").get<std::string>() != ScalarType ||
        Descriptor.at("byteOrder").get<std::string>() != "little_endian")
    {
        OutError = "unsupported verified mesh scalar encoding";
        return false;
    }
    const std::uint64_t ElementCount =
        Descriptor.at("elementCount").get<std::uint64_t>();
    const std::uint64_t ByteCount =
        Descriptor.at("byteCount").get<std::uint64_t>();
    std::uint64_t ExpectedBytes = 0;
    if (!CheckedMultiply(ElementCount, Float32Bytes, ExpectedBytes) ||
        ExpectedBytes != ByteCount ||
        ElementCount > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()) ||
        ByteCount > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()))
    {
        OutError = "verified mesh array byte count is inconsistent";
        return false;
    }

    const std::filesystem::path RelativePath(
        Descriptor.at("path").get<std::string>());
    const std::filesystem::path Path = PackageDirectory / RelativePath;
    std::ifstream Stream(Path, std::ios::binary);
    if (!Stream)
    {
        OutError = "unable to open verified mesh blob: " + PathText(Path);
        return false;
    }
    OutBytes.assign(static_cast<std::size_t>(ByteCount), std::byte{});
    Stream.read(
        reinterpret_cast<char*>(OutBytes.data()),
        static_cast<std::streamsize>(OutBytes.size()));
    if (Stream.gcount() != static_cast<std::streamsize>(OutBytes.size()))
    {
        OutError = "verified mesh blob ended early: " + PathText(Path);
        return false;
    }
    OutElementCount = static_cast<std::size_t>(ElementCount);
    return true;
}

bool ReadFloatArray(
    const std::filesystem::path& PackageDirectory,
    const Json& Descriptor,
    std::vector<float>& OutValues,
    std::string& OutError)
{
    std::vector<std::byte> Bytes;
    std::size_t ElementCount = 0;
    if (!ReadVerifiedBlob(
            PackageDirectory, Descriptor, "float32", Bytes,
            ElementCount, OutError))
    {
        return false;
    }
    OutValues.clear();
    OutValues.reserve(ElementCount);
    for (std::size_t Index = 0; Index < ElementCount; ++Index)
    {
        const float Value = DecodeLittleEndianFloat32(
            Bytes.data() + Index * Float32Bytes);
        if (!std::isfinite(Value))
        {
            OutError = "verified mesh float array contains a non-finite value";
            return false;
        }
        OutValues.push_back(Value);
    }
    return true;
}

bool ReadUint32Array(
    const std::filesystem::path& PackageDirectory,
    const Json& Descriptor,
    std::vector<std::uint32_t>& OutValues,
    std::string& OutError)
{
    std::vector<std::byte> Bytes;
    std::size_t ElementCount = 0;
    if (!ReadVerifiedBlob(
            PackageDirectory, Descriptor, "uint32", Bytes,
            ElementCount, OutError))
    {
        return false;
    }
    OutValues.clear();
    OutValues.reserve(ElementCount);
    for (std::size_t Index = 0; Index < ElementCount; ++Index)
    {
        OutValues.push_back(DecodeLittleEndianUint32(
            Bytes.data() + Index * Float32Bytes));
    }
    return true;
}

bool ReadManifest(
    const std::filesystem::path& Path,
    Json& OutManifest,
    std::string& OutError)
{
    try
    {
        std::ifstream Stream(Path, std::ios::binary);
        if (!Stream)
        {
            OutError = "unable to open verified manifest: " + PathText(Path);
            return false;
        }
        Stream >> OutManifest;
        if (!Stream)
        {
            OutError = "unable to read verified manifest: " + PathText(Path);
            return false;
        }
        return true;
    }
    catch (const std::exception& Exception)
    {
        OutError = "unable to parse verified manifest: " +
            std::string(Exception.what());
        return false;
    }
}

std::vector<Bone> ParseBones(const Json& Values)
{
    std::vector<Bone> Bones;
    Bones.reserve(Values.size());
    for (const Json& Value : Values)
    {
        Bone Item;
        Item.ParentIndex = Value.at(0).get<int>();
        Item.Name = Value.at(1).get<std::string>();
        Item.Path = Value.at(2).get<std::string>();
        Item.ParticipatesInIk = Value.at(3).get<bool>();
        Item.RestPosition = {
            Value.at(4).get<float>(),
            Value.at(5).get<float>(),
            Value.at(6).get<float>()};
        Bones.push_back(std::move(Item));
    }
    return Bones;
}

std::vector<RetargetChain> ParseRetargetChains(const Json& Values)
{
    std::vector<RetargetChain> Chains;
    Chains.reserve(Values.size());
    for (const Json& Value : Values)
    {
        RetargetChain Chain;
        Chain.Label = Value.at("label").get<std::string>();
        Chain.IkMode = Value.at("ikMode").get<std::string>();
        Chain.SourceGoalName =
            Value.at("sourceGoalName").get<std::string>();
        Chain.TargetGoalName =
            Value.at("targetGoalName").get<std::string>();
        Chain.SourceGoalBone = Value.at("sourceGoalBone").get<int>();
        Chain.TargetGoalBone = Value.at("targetGoalBone").get<int>();
        Chain.SourcePoleBone = Value.at("sourcePoleBone").get<int>();
        Chain.TargetPoleBone = Value.at("targetPoleBone").get<int>();
        Chains.push_back(std::move(Chain));
    }
    return Chains;
}

bool ParseMeshPackage(
    const std::filesystem::path& PackageDirectory,
    const Json& Value,
    const std::size_t BoneCount,
    MeshPackage& OutPackage,
    std::string& OutError)
{
    MeshPackage Package;
    Package.Label = Value.at("label").get<std::string>();
    Package.ControlPointCount =
        Value.at("controlPointCount").get<std::uint64_t>();
    Package.TriangleCount =
        Value.at("triangleCount").get<std::uint64_t>();
    const Json& MeshValues = Value.at("meshes");
    if (MeshValues.size() != Value.at("meshCount").get<std::size_t>())
    {
        OutError = "verified mesh package count is inconsistent";
        return false;
    }

    std::uint64_t ControlPointTotal = 0;
    std::uint64_t TriangleTotal = 0;
    Package.Meshes.reserve(MeshValues.size());
    for (const Json& MeshValue : MeshValues)
    {
        Mesh Item;
        Item.Name = MeshValue.at("name").get<std::string>();
        Item.Path = MeshValue.at("path").get<std::string>();
        Item.SkinMode = MeshValue.at("skinMode").get<std::string>();
        if (Item.SkinMode != "normalize" &&
            Item.SkinMode != "total_one")
        {
            OutError = "unsupported verified skin mode: " + Item.SkinMode;
            return false;
        }
        const std::size_t ControlPointCount =
            MeshValue.at("controlPointCount").get<std::size_t>();
        const std::size_t TriangleCount =
            MeshValue.at("triangleCount").get<std::size_t>();
        const std::size_t ClusterCount =
            MeshValue.at("clusterCount").get<std::size_t>();
        const Json& Arrays = MeshValue.at("arrays");

        std::vector<float> FlatPositions;
        std::vector<float> FlatBindOffsets;
        if (!ReadFloatArray(
                PackageDirectory, Arrays.at("positions"),
                FlatPositions, OutError) ||
            !ReadUint32Array(
                PackageDirectory, Arrays.at("triangleIndices"),
                Item.TriangleIndices, OutError) ||
            !ReadUint32Array(
                PackageDirectory, Arrays.at("influenceOffsets"),
                Item.InfluenceOffsets, OutError) ||
            !ReadUint32Array(
                PackageDirectory,
                Arrays.at("influenceClusterIndices"),
                Item.InfluenceClusterIndices, OutError) ||
            !ReadFloatArray(
                PackageDirectory, Arrays.at("influenceWeights"),
                Item.InfluenceWeights, OutError) ||
            !ReadUint32Array(
                PackageDirectory, Arrays.at("clusterBoneIndices"),
                Item.ClusterBoneIndices, OutError) ||
            !ReadFloatArray(
                PackageDirectory, Arrays.at("clusterBindOffsets3x4"),
                FlatBindOffsets, OutError))
        {
            return false;
        }

        if (FlatPositions.size() != ControlPointCount * 3U ||
            Item.TriangleIndices.size() != TriangleCount * 3U ||
            Item.InfluenceOffsets.size() != ControlPointCount + 1U ||
            Item.InfluenceClusterIndices.size() !=
                Item.InfluenceWeights.size() ||
            Item.ClusterBoneIndices.size() != ClusterCount ||
            FlatBindOffsets.size() != ClusterCount * 12U)
        {
            OutError = "verified mesh array shape is inconsistent: " +
                Item.Path;
            return false;
        }

        Item.BindPositions.reserve(ControlPointCount);
        for (std::size_t Index = 0; Index < ControlPointCount; ++Index)
        {
            Item.BindPositions.push_back({
                FlatPositions[Index * 3U],
                FlatPositions[Index * 3U + 1U],
                FlatPositions[Index * 3U + 2U]});
        }
        Item.ClusterBindOffsets.reserve(ClusterCount);
        for (std::size_t Cluster = 0; Cluster < ClusterCount; ++Cluster)
        {
            std::array<float, 12> Affine{};
            std::copy_n(
                FlatBindOffsets.begin() +
                    static_cast<std::ptrdiff_t>(Cluster * 12U),
                12U, Affine.begin());
            Item.ClusterBindOffsets.push_back(Affine);
        }
        const Json& Fallback = MeshValue.at("fallback");
        if (Fallback.size() != Item.FallbackAffine.size())
        {
            OutError = "verified mesh fallback affine is not 3x4";
            return false;
        }
        for (std::size_t Index = 0;
             Index < Item.FallbackAffine.size(); ++Index)
        {
            Item.FallbackAffine[Index] = Fallback.at(Index).get<float>();
            if (!std::isfinite(Item.FallbackAffine[Index]))
            {
                OutError = "verified mesh fallback affine is non-finite";
                return false;
            }
        }

        if (Item.InfluenceOffsets.empty() ||
            Item.InfluenceOffsets.front() != 0U ||
            Item.InfluenceOffsets.back() !=
                Item.InfluenceWeights.size())
        {
            OutError = "verified mesh influence offsets are inconsistent";
            return false;
        }
        for (std::size_t Index = 1;
             Index < Item.InfluenceOffsets.size(); ++Index)
        {
            if (Item.InfluenceOffsets[Index] <
                    Item.InfluenceOffsets[Index - 1] ||
                Item.InfluenceOffsets[Index] >
                    Item.InfluenceWeights.size())
            {
                OutError =
                    "verified mesh influence offsets are not monotonic";
                return false;
            }
        }
        for (const std::uint32_t Index : Item.TriangleIndices)
        {
            if (Index >= ControlPointCount)
            {
                OutError = "verified mesh triangle index is out of range";
                return false;
            }
        }
        for (const std::uint32_t Cluster :
             Item.InfluenceClusterIndices)
        {
            if (Cluster >= ClusterCount)
            {
                OutError = "verified mesh influence cluster is out of range";
                return false;
            }
        }
        for (const float Weight : Item.InfluenceWeights)
        {
            if (!std::isfinite(Weight) || Weight < 0.0F)
            {
                OutError = "verified mesh influence weight is invalid";
                return false;
            }
        }
        for (const std::uint32_t BoneIndex : Item.ClusterBoneIndices)
        {
            if (BoneIndex >= BoneCount)
            {
                OutError = "verified mesh cluster bone is out of range";
                return false;
            }
        }

        ControlPointTotal += ControlPointCount;
        TriangleTotal += TriangleCount;
        Package.Meshes.push_back(std::move(Item));
    }
    if (ControlPointTotal != Package.ControlPointCount ||
        TriangleTotal != Package.TriangleCount)
    {
        OutError = "verified mesh package totals are inconsistent";
        return false;
    }
    OutPackage = std::move(Package);
    return true;
}

bool ReadFramePose(
    const std::filesystem::path& PackageDirectory,
    const std::filesystem::path& RelativePath,
    const std::uint64_t FrameIndex,
    const std::size_t BoneCount,
    PoseLane& OutLane,
    std::string& OutError)
{
    std::uint64_t FrameFloats = 0;
    std::uint64_t FrameBytes = 0;
    std::uint64_t FrameOffset = 0;
    if (!CheckedMultiply(
            static_cast<std::uint64_t>(BoneCount),
            TransformStrideFloat32,
            FrameFloats) ||
        !CheckedMultiply(FrameFloats, Float32Bytes, FrameBytes) ||
        !CheckedMultiply(FrameIndex, FrameBytes, FrameOffset) ||
        FrameBytes > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()))
    {
        OutError = "pose frame byte range overflows";
        return false;
    }

    // SKRV v1 portable paths are validated as printable ASCII before this
    // projection layer runs, so the native narrow path constructor is exact.
    const std::filesystem::path Path = PackageDirectory / RelativePath;
    std::ifstream Stream(Path, std::ios::binary);
    if (!Stream)
    {
        OutError = "unable to open verified pose lane: " + PathText(Path);
        return false;
    }
    Stream.seekg(static_cast<std::streamoff>(FrameOffset));
    if (!Stream)
    {
        OutError = "unable to seek verified pose lane: " + PathText(Path);
        return false;
    }

    std::vector<std::byte> Bytes(static_cast<std::size_t>(FrameBytes));
    Stream.read(
        reinterpret_cast<char*>(Bytes.data()),
        static_cast<std::streamsize>(Bytes.size()));
    if (Stream.gcount() != static_cast<std::streamsize>(Bytes.size()))
    {
        OutError = "verified pose lane ended before requested frame: " +
            PathText(Path);
        return false;
    }

    OutLane.ModelPositions.clear();
    OutLane.ModelRotations.clear();
    OutLane.ModelScales.clear();
    OutLane.ModelPositions.reserve(BoneCount);
    OutLane.ModelRotations.reserve(BoneCount);
    OutLane.ModelScales.reserve(BoneCount);
    for (std::size_t BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const std::size_t Base =
            BoneIndex * TransformStrideFloat32 * Float32Bytes;
        const Vec3 Position = {
            DecodeLittleEndianFloat32(Bytes.data() + Base),
            DecodeLittleEndianFloat32(Bytes.data() + Base + Float32Bytes),
            DecodeLittleEndianFloat32(Bytes.data() + Base + 2 * Float32Bytes)};
        const Quaternion Rotation = {
            DecodeLittleEndianFloat32(Bytes.data() + Base + 3 * Float32Bytes),
            DecodeLittleEndianFloat32(Bytes.data() + Base + 4 * Float32Bytes),
            DecodeLittleEndianFloat32(Bytes.data() + Base + 5 * Float32Bytes),
            DecodeLittleEndianFloat32(Bytes.data() + Base + 6 * Float32Bytes)};
        const Vec3 Scale = {
            DecodeLittleEndianFloat32(Bytes.data() + Base + 7 * Float32Bytes),
            DecodeLittleEndianFloat32(Bytes.data() + Base + 8 * Float32Bytes),
            DecodeLittleEndianFloat32(Bytes.data() + Base + 9 * Float32Bytes)};
        if (!std::isfinite(Position.X) ||
            !std::isfinite(Position.Y) ||
            !std::isfinite(Position.Z) ||
            !std::isfinite(Rotation.X) ||
            !std::isfinite(Rotation.Y) ||
            !std::isfinite(Rotation.Z) ||
            !std::isfinite(Rotation.W) ||
            !std::isfinite(Scale.X) ||
            !std::isfinite(Scale.Y) ||
            !std::isfinite(Scale.Z))
        {
            std::ostringstream Message;
            Message << "non-finite model-space pose transform in "
                    << PathText(RelativePath)
                    << " at bone " << BoneIndex;
            OutError = Message.str();
            return false;
        }
        OutLane.ModelPositions.push_back(Position);
        OutLane.ModelRotations.push_back(Rotation);
        OutLane.ModelScales.push_back(Scale);
    }
    return true;
}

bool ReadPoseRange(
    const std::filesystem::path& PackageDirectory,
    const std::filesystem::path& RelativePath,
    const std::uint64_t FirstFrame,
    const std::size_t SampleCount,
    const std::size_t BoneCount,
    std::vector<PoseLane>& OutFrames,
    std::string& OutError)
{
    std::uint64_t FrameFloats = 0;
    std::uint64_t FrameBytes = 0;
    std::uint64_t FirstOffset = 0;
    std::uint64_t RangeBytes = 0;
    if (!CheckedMultiply(
            static_cast<std::uint64_t>(BoneCount),
            TransformStrideFloat32, FrameFloats) ||
        !CheckedMultiply(FrameFloats, Float32Bytes, FrameBytes) ||
        !CheckedMultiply(FirstFrame, FrameBytes, FirstOffset) ||
        !CheckedMultiply(
            static_cast<std::uint64_t>(SampleCount),
            FrameBytes, RangeBytes) ||
        RangeBytes > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()))
    {
        OutError = "pose history byte range overflows";
        return false;
    }

    const std::filesystem::path Path = PackageDirectory / RelativePath;
    std::ifstream Stream(Path, std::ios::binary);
    if (!Stream)
    {
        OutError = "unable to open verified pose history: " + PathText(Path);
        return false;
    }
    Stream.seekg(static_cast<std::streamoff>(FirstOffset));
    if (!Stream)
    {
        OutError = "unable to seek verified pose history: " + PathText(Path);
        return false;
    }
    std::vector<std::byte> Bytes(static_cast<std::size_t>(RangeBytes));
    Stream.read(
        reinterpret_cast<char*>(Bytes.data()),
        static_cast<std::streamsize>(Bytes.size()));
    if (Stream.gcount() != static_cast<std::streamsize>(Bytes.size()))
    {
        OutError = "verified pose history ended before requested range: " +
            PathText(Path);
        return false;
    }

    std::vector<PoseLane> Frames;
    Frames.reserve(SampleCount);
    const std::size_t FrameByteCount =
        static_cast<std::size_t>(FrameBytes);
    for (std::size_t Sample = 0; Sample < SampleCount; ++Sample)
    {
        PoseLane Lane;
        Lane.ModelPositions.reserve(BoneCount);
        Lane.ModelRotations.reserve(BoneCount);
        for (std::size_t BoneIndex = 0;
             BoneIndex < BoneCount; ++BoneIndex)
        {
            const std::size_t Base = Sample * FrameByteCount +
                BoneIndex * TransformStrideFloat32 * Float32Bytes;
            const Vec3 Position = {
                DecodeLittleEndianFloat32(Bytes.data() + Base),
                DecodeLittleEndianFloat32(
                    Bytes.data() + Base + Float32Bytes),
                DecodeLittleEndianFloat32(
                    Bytes.data() + Base + 2U * Float32Bytes)};
            const Quaternion Rotation = {
                DecodeLittleEndianFloat32(
                    Bytes.data() + Base + 3U * Float32Bytes),
                DecodeLittleEndianFloat32(
                    Bytes.data() + Base + 4U * Float32Bytes),
                DecodeLittleEndianFloat32(
                    Bytes.data() + Base + 5U * Float32Bytes),
                DecodeLittleEndianFloat32(
                    Bytes.data() + Base + 6U * Float32Bytes)};
            if (!std::isfinite(Position.X) ||
                !std::isfinite(Position.Y) ||
                !std::isfinite(Position.Z) ||
                !std::isfinite(Rotation.X) ||
                !std::isfinite(Rotation.Y) ||
                !std::isfinite(Rotation.Z) ||
                !std::isfinite(Rotation.W))
            {
                OutError = "verified pose history contains a non-finite "
                    "transform";
                return false;
            }
            Lane.ModelPositions.push_back(Position);
            Lane.ModelRotations.push_back(Rotation);
        }
        Frames.push_back(std::move(Lane));
    }
    OutFrames = std::move(Frames);
    return true;
}

Vec3 UEIKJsonToViewerVector(const Vec3& Value)
{
    // UE export contract: +X forward, +Y right, +Z up, left-handed.
    // Native Viewer contract: +X right, +Y up, +Z toward the camera,
    // right-handed.
    return {Value.Y, Value.Z, -Value.X};
}

Quaternion UEIKJsonToViewerQuaternion(const Quaternion& Value)
{
    // For the improper orthogonal basis C (det(C) = -1), quaternion vector
    // components transform as the axial vector det(C) * C * q.xyz.
    return {-Value.Y, -Value.Z, Value.X, Value.W};
}

Vec3 UEIKJsonToViewerScale(const Vec3& Value)
{
    return {Value.Y, Value.Z, Value.X};
}

std::array<float, 12> UEIKJsonToViewerAffine(
    const std::array<float, 12>& Value)
{
    const auto ApplyLinear = [&](const Vec3& Point)
    {
        return Vec3{
            Value[3] * Point.X + Value[6] * Point.Y +
                Value[9] * Point.Z,
            Value[4] * Point.X + Value[7] * Point.Y +
                Value[10] * Point.Z,
            Value[5] * Point.X + Value[8] * Point.Y +
                Value[11] * Point.Z};
    };
    const Vec3 Translation =
        UEIKJsonToViewerVector({Value[0], Value[1], Value[2]});
    // C^-1 maps viewer X/Y/Z to UE +Y/+Z/-X respectively.
    const Vec3 ColumnX =
        UEIKJsonToViewerVector(ApplyLinear({0.0F, 1.0F, 0.0F}));
    const Vec3 ColumnY =
        UEIKJsonToViewerVector(ApplyLinear({0.0F, 0.0F, 1.0F}));
    const Vec3 ColumnZ =
        UEIKJsonToViewerVector(ApplyLinear({-1.0F, 0.0F, 0.0F}));
    return {
        Translation.X, Translation.Y, Translation.Z,
        ColumnX.X, ColumnX.Y, ColumnX.Z,
        ColumnY.X, ColumnY.Y, ColumnY.Z,
        ColumnZ.X, ColumnZ.Y, ColumnZ.Z};
}

void ConvertUEIKJsonPoseToViewerSpace(PoseLane& Lane)
{
    for (Vec3& Position : Lane.ModelPositions)
        Position = UEIKJsonToViewerVector(Position);
    for (Quaternion& Rotation : Lane.ModelRotations)
        Rotation = UEIKJsonToViewerQuaternion(Rotation);
    for (Vec3& Scale : Lane.ModelScales)
        Scale = UEIKJsonToViewerScale(Scale);
}

void ConvertUEIKJsonMeshToViewerSpace(MeshPackage& Package)
{
    for (Mesh& Item : Package.Meshes)
    {
        for (Vec3& Position : Item.BindPositions)
            Position = UEIKJsonToViewerVector(Position);
        for (std::array<float, 12>& Affine : Item.ClusterBindOffsets)
            Affine = UEIKJsonToViewerAffine(Affine);
        Item.FallbackAffine =
            UEIKJsonToViewerAffine(Item.FallbackAffine);
    }
}

void ConvertUEIKJsonSceneToViewerSpace(ReviewScene& Scene)
{
    for (Bone& Item : Scene.SourceBones)
        Item.RestPosition =
            UEIKJsonToViewerVector(Item.RestPosition);
    for (Bone& Item : Scene.TargetBones)
        Item.RestPosition =
            UEIKJsonToViewerVector(Item.RestPosition);
    ConvertUEIKJsonMeshToViewerSpace(Scene.SourceMesh);
    ConvertUEIKJsonMeshToViewerSpace(Scene.TargetMesh);
    ConvertUEIKJsonPoseToViewerSpace(Scene.Source);
    ConvertUEIKJsonPoseToViewerSpace(Scene.Fk);
    ConvertUEIKJsonPoseToViewerSpace(Scene.Foundation);
    ConvertUEIKJsonPoseToViewerSpace(Scene.Final);
    const Quaternion Anchor = UEIKJsonToViewerQuaternion({
        Scene.GhostAnchor.Basis[0],
        Scene.GhostAnchor.Basis[1],
        Scene.GhostAnchor.Basis[2],
        Scene.GhostAnchor.Basis[3]});
    Scene.GhostAnchor.Basis = {
        Anchor.X, Anchor.Y, Anchor.Z, Anchor.W};
}

Vec3 RotateByQuaternion(
    const std::array<float, 4>& Quaternion,
    const Vec3& Value)
{
    const Vec3 Q = {Quaternion[0], Quaternion[1], Quaternion[2]};
    const float W = Quaternion[3];
    const Vec3 CrossQValue = {
        Q.Y * Value.Z - Q.Z * Value.Y,
        Q.Z * Value.X - Q.X * Value.Z,
        Q.X * Value.Y - Q.Y * Value.X};
    const Vec3 CrossQCross = {
        Q.Y * CrossQValue.Z - Q.Z * CrossQValue.Y,
        Q.Z * CrossQValue.X - Q.X * CrossQValue.Z,
        Q.X * CrossQValue.Y - Q.Y * CrossQValue.X};
    return {
        Value.X + 2.0F * (W * CrossQValue.X + CrossQCross.X),
        Value.Y + 2.0F * (W * CrossQValue.Y + CrossQCross.Y),
        Value.Z + 2.0F * (W * CrossQValue.Z + CrossQCross.Z)};
}

Vec3 RotateByQuaternion(
    const Quaternion& QuaternionValue,
    const Vec3& Value)
{
    const std::array<float, 4> Components = {
        QuaternionValue.X,
        QuaternionValue.Y,
        QuaternionValue.Z,
        QuaternionValue.W};
    return RotateByQuaternion(Components, Value);
}

bool AlignSourcePoint(
    const DisplayAnchor& Anchor,
    const PoseLane& SourceLane,
    const PoseLane& TargetLane,
    const Vec3& SourcePoint,
    Vec3& OutPoint)
{
    if (Anchor.SourceBone >= SourceLane.ModelPositions.size() ||
        Anchor.SourceBone >= SourceLane.ModelRotations.size() ||
        Anchor.TargetBone >= TargetLane.ModelPositions.size() ||
        Anchor.TargetBone >= TargetLane.ModelRotations.size())
    {
        return false;
    }
    const Vec3 SourceAnchor =
        SourceLane.ModelPositions[Anchor.SourceBone];
    const Vec3 TargetAnchor =
        TargetLane.ModelPositions[Anchor.TargetBone];
    const Quaternion SourceAnchorRotation =
        SourceLane.ModelRotations[Anchor.SourceBone];
    const Quaternion TargetAnchorRotation =
        TargetLane.ModelRotations[Anchor.TargetBone];
    const Quaternion InverseSourceAnchorRotation = {
        -SourceAnchorRotation.X,
        -SourceAnchorRotation.Y,
        -SourceAnchorRotation.Z,
        SourceAnchorRotation.W};
    const Vec3 Relative = {
        SourcePoint.X - SourceAnchor.X,
        SourcePoint.Y - SourceAnchor.Y,
        SourcePoint.Z - SourceAnchor.Z};
    const Vec3 AnchorLocal = RotateByQuaternion(
        InverseSourceAnchorRotation, Relative);
    const Vec3 RestBasis = RotateByQuaternion(
        Anchor.Basis, AnchorLocal);
    const Vec3 Rotated = RotateByQuaternion(
        TargetAnchorRotation, RestBasis);
    OutPoint = {
        Rotated.X + TargetAnchor.X,
        Rotated.Y + TargetAnchor.Y,
        Rotated.Z + TargetAnchor.Z};
    return true;
}
} // namespace

ReviewSceneLoadResult LoadReviewScene(
    const std::filesystem::path& PackageDirectory,
    const std::size_t ClipIndex,
    const std::uint64_t FrameIndex)
{
    ReviewSceneLoadResult Result;
    const skrv::PackageInspectResult Inspection =
        skrv::InspectDirectoryPackage(PackageDirectory);
    if (!Inspection.Success)
    {
        Result.Errors = Inspection.Errors;
        return Result;
    }

    Json Manifest;
    std::string Error;
    if (!ReadManifest(PackageDirectory / "manifest.json", Manifest, Error))
    {
        Result.Errors.push_back(std::move(Error));
        return Result;
    }

    try
    {
        const Json& Snapshot = Manifest.at("snapshot");
        const Json& Clips = Snapshot.at("clips");
        if (ClipIndex >= Clips.size())
        {
            Result.Errors.emplace_back("requested clip index is out of range");
            return Result;
        }
        const Json& Clip = Clips.at(ClipIndex);
        const std::uint64_t ClipFrameCount =
            Clip.at("frameCount").get<std::uint64_t>();
        if (FrameIndex >= ClipFrameCount)
        {
            Result.Errors.emplace_back("requested frame index is out of range");
            return Result;
        }

        ReviewScene Scene;
        Scene.PackageDirectory = Inspection.PackageDirectory;
        Scene.RouteId =
            Snapshot.at("route").get<std::string>();
        Scene.RouteSelected =
            Snapshot.at("selected").get<bool>();
        Scene.RouteAdopted =
            Snapshot.at("adopted").get<bool>();
        Scene.FoundationRouteId =
            Snapshot.at("foundationRoute").get<std::string>();
        Scene.FoundationFrozen =
            Snapshot.at("foundationFrozen").get<bool>();
        Scene.UEIKJsonDisplayBasisConversion =
            Scene.RouteId ==
                "ue_ik_json_canonical_bridge_v1";
        Scene.ClipIndex = ClipIndex;
        Scene.Clips.reserve(Clips.size());
        for (const Json& Candidate : Clips)
        {
            Scene.Clips.push_back({
                Candidate.at("id").get<std::string>(),
                Candidate.at("label").get<std::string>(),
                Candidate.at("frameCount").get<std::uint64_t>(),
                Candidate.at("fps").get<double>(),
                Candidate.at("sourceMotionFootLockEnabled").get<bool>(),
                Candidate.at("sourceMotionFootLockSuccess").get<bool>(),
                Candidate.at("foundationTrs").at("sha256")
                        .get<std::string>() !=
                    Candidate.at("finalTrs").at("sha256")
                        .get<std::string>()});
        }
        Scene.ClipId = Clip.at("id").get<std::string>();
        Scene.ClipLabel = Clip.at("label").get<std::string>();
        Scene.ClipFrameCount = ClipFrameCount;
        Scene.FrameIndex = FrameIndex;
        Scene.FramesPerSecond = Clip.at("fps").get<double>();
        Scene.SourceBones = ParseBones(Snapshot.at("sourceBones"));
        Scene.TargetBones = ParseBones(Snapshot.at("targetBones"));
        Scene.RetargetChains =
            ParseRetargetChains(Snapshot.at("retargetChains"));
        const Json& RootPelvis = Snapshot.at("rootPelvis");
        Scene.SourceRootBone =
            RootPelvis.at("sourceRoot").get<int>();
        Scene.SourcePelvisBone =
            RootPelvis.at("sourcePelvis").get<int>();
        Scene.TargetHipsBone =
            RootPelvis.at("targetHips").get<int>();
        if (Scene.SourceRootBone < 0 ||
            static_cast<std::size_t>(Scene.SourceRootBone) >=
                Scene.SourceBones.size() ||
            Scene.SourcePelvisBone < 0 ||
            static_cast<std::size_t>(Scene.SourcePelvisBone) >=
                Scene.SourceBones.size() ||
            Scene.TargetHipsBone < 0 ||
            static_cast<std::size_t>(Scene.TargetHipsBone) >=
                Scene.TargetBones.size())
        {
            Result.Errors.emplace_back(
                "verified root/pelvis mapping is out of range");
            return Result;
        }
        for (const RetargetChain& Chain : Scene.RetargetChains)
        {
            if (Chain.IkMode == "fk_only") continue;
            if (Chain.SourceGoalBone < 0 ||
                Chain.TargetGoalBone < 0 ||
                static_cast<std::size_t>(Chain.SourceGoalBone) >=
                    Scene.SourceBones.size() ||
                static_cast<std::size_t>(Chain.TargetGoalBone) >=
                    Scene.TargetBones.size() ||
                Chain.SourcePoleBone < 0 ||
                Chain.TargetPoleBone < 0 ||
                static_cast<std::size_t>(Chain.SourcePoleBone) >=
                    Scene.SourceBones.size() ||
                static_cast<std::size_t>(Chain.TargetPoleBone) >=
                    Scene.TargetBones.size())
            {
                Result.Errors.emplace_back(
                    "verified IK Goal/pole mapping is out of range: " +
                    Chain.Label);
                return Result;
            }
        }
        const Json& SourceMeshes = Snapshot.at("sourceMeshes");
        if (ClipIndex >= SourceMeshes.size() ||
            !ParseMeshPackage(
                Scene.PackageDirectory, SourceMeshes.at(ClipIndex),
                Scene.SourceBones.size(), Scene.SourceMesh, Error) ||
            !ParseMeshPackage(
                Scene.PackageDirectory, Snapshot.at("targetMesh"),
                Scene.TargetBones.size(), Scene.TargetMesh, Error))
        {
            if (Error.empty())
                Error = "verified source mesh package is unavailable";
            Result.Errors.push_back(std::move(Error));
            return Result;
        }
        Scene.SourceGhostOpacity = static_cast<float>(
            Manifest.at("displayContract")
                .at("sourceGhostOpacity").get<double>());
        Scene.SourcePoseRelativePath = std::filesystem::path(
            Clip.at("sourceTrs").at("path").get<std::string>());
        Scene.FkPoseRelativePath = std::filesystem::path(
            Clip.at("fkTrs").at("path").get<std::string>());
        Scene.FoundationPoseRelativePath = std::filesystem::path(
            Clip.at("foundationTrs").at("path").get<std::string>());
        Scene.FinalPoseRelativePath = std::filesystem::path(
            Clip.at("finalTrs").at("path").get<std::string>());

        // N2 has no anchor-selection control. It therefore consumes the first
        // manifest anchor deterministically and labels it in every overlay.
        // This is a display transform only, never an algorithm-route choice.
        const Json& Anchor = Snapshot.at("anchors").at(0);
        Scene.GhostAnchor.Label = Anchor.at("label").get<std::string>();
        Scene.GhostAnchor.SourcePath =
            Anchor.at("sourcePath").get<std::string>();
        Scene.GhostAnchor.TargetPath =
            Anchor.at("targetPath").get<std::string>();
        Scene.GhostAnchor.SourceBone =
            Anchor.at("sourceBone").get<std::size_t>();
        Scene.GhostAnchor.TargetBone =
            Anchor.at("targetBone").get<std::size_t>();
        for (std::size_t Index = 0; Index < Scene.GhostAnchor.Basis.size();
             ++Index)
        {
            Scene.GhostAnchor.Basis[Index] =
                Anchor.at("basis").at(Index).get<float>();
        }

        if (!ReadFramePose(
                Scene.PackageDirectory,
                Scene.SourcePoseRelativePath,
                FrameIndex,
                Scene.SourceBones.size(),
                Scene.Source,
                Error) ||
            !ReadFramePose(
                Scene.PackageDirectory,
                Scene.FkPoseRelativePath,
                FrameIndex,
                Scene.TargetBones.size(),
                Scene.Fk,
                Error) ||
            !ReadFramePose(
                Scene.PackageDirectory,
                Scene.FoundationPoseRelativePath,
                FrameIndex,
                Scene.TargetBones.size(),
                Scene.Foundation,
                Error) ||
            !ReadFramePose(
                Scene.PackageDirectory,
                Scene.FinalPoseRelativePath,
                FrameIndex,
                Scene.TargetBones.size(),
                Scene.Final,
                Error))
        {
            Result.Errors.push_back(std::move(Error));
            return Result;
        }
        if (Scene.UEIKJsonDisplayBasisConversion)
            ConvertUEIKJsonSceneToViewerSpace(Scene);

        Result.Success = true;
        Result.Scene = std::move(Scene);
        return Result;
    }
    catch (const std::exception& Exception)
    {
        Result.Errors.push_back(
            "verified manifest could not be projected into N2 scene: " +
            std::string(Exception.what()));
        return Result;
    }
}

bool LoadVerifiedReviewSceneFrame(
    ReviewScene& Scene,
    const std::uint64_t FrameIndex,
    std::vector<std::string>& OutErrors)
{
    OutErrors.clear();
    if (FrameIndex >= Scene.ClipFrameCount)
    {
        OutErrors.emplace_back("requested playback frame is out of range");
        return false;
    }
    PoseLane Source;
    PoseLane Fk;
    PoseLane Foundation;
    PoseLane Final;
    std::string Error;
    if (!ReadFramePose(
            Scene.PackageDirectory, Scene.SourcePoseRelativePath,
            FrameIndex, Scene.SourceBones.size(), Source, Error) ||
        !ReadFramePose(
            Scene.PackageDirectory, Scene.FkPoseRelativePath,
            FrameIndex, Scene.TargetBones.size(), Fk, Error) ||
        !ReadFramePose(
            Scene.PackageDirectory, Scene.FoundationPoseRelativePath,
            FrameIndex, Scene.TargetBones.size(), Foundation, Error) ||
        !ReadFramePose(
            Scene.PackageDirectory, Scene.FinalPoseRelativePath,
            FrameIndex, Scene.TargetBones.size(), Final, Error))
    {
        OutErrors.push_back(std::move(Error));
        return false;
    }
    if (Scene.UEIKJsonDisplayBasisConversion)
    {
        ConvertUEIKJsonPoseToViewerSpace(Source);
        ConvertUEIKJsonPoseToViewerSpace(Fk);
        ConvertUEIKJsonPoseToViewerSpace(Foundation);
        ConvertUEIKJsonPoseToViewerSpace(Final);
    }
    Scene.Source = std::move(Source);
    Scene.Fk = std::move(Fk);
    Scene.Foundation = std::move(Foundation);
    Scene.Final = std::move(Final);
    Scene.FrameIndex = FrameIndex;
    return true;
}

ReviewExportResult FindVerifiedReviewExport(
    const ReviewScene& Scene,
    const std::string& Lane)
{
    return FindVerifiedReviewExport(
        Scene.PackageDirectory, Scene.ClipId, Lane);
}

ReviewExportResult FindVerifiedReviewExport(
    const std::filesystem::path& PackageDirectory,
    const std::string& ClipId,
    const std::string& Lane)
{
    ReviewExportResult Result;
    const skrv::PackageInspectResult Inspection =
        skrv::InspectDirectoryPackage(PackageDirectory);
    if (!Inspection.Success)
    {
        Result.Errors = Inspection.Errors;
        return Result;
    }
    Json Manifest;
    std::string Error;
    if (!ReadManifest(
            Inspection.PackageDirectory / "manifest.json", Manifest, Error))
    {
        Result.Errors.push_back(std::move(Error));
        return Result;
    }
    try
    {
        for (const Json& Export : Manifest.at("verifiedExports"))
        {
            if (Export.at("clip_id").get<std::string>() != ClipId ||
                Export.at("lane").get<std::string>() != Lane)
            {
                continue;
            }
            const std::filesystem::path Relative(
                Export.at("path").get<std::string>());
            const std::filesystem::path Source =
                Inspection.PackageDirectory / Relative;
            std::error_code FileError;
            if (!std::filesystem::is_regular_file(Source, FileError) ||
                FileError)
            {
                Result.Errors.emplace_back(
                    "verified export is no longer a readable file");
                return Result;
            }
            Result.Success = true;
            Result.SourceFbx = Source;
            Result.SuggestedFileName = Relative.filename().string();
            Result.ExpectedSha256 = Export.at("sha256").get<std::string>();
            return Result;
        }
        Result.Errors.emplace_back(
            "the current clip has no verified export for lane: " + Lane);
    }
    catch (const std::exception& Exception)
    {
        Result.Errors.push_back(
            "verified export metadata could not be read: " +
            std::string(Exception.what()));
    }
    return Result;
}

const ReviewClipInfo* CurrentReviewClipInfo(const ReviewScene& Scene)
{
    if (Scene.ClipIndex >= Scene.Clips.size()) return nullptr;
    return &Scene.Clips[Scene.ClipIndex];
}

bool IsUEIKJsonCandidateRoute(const ReviewScene& Scene)
{
    return Scene.RouteId ==
            "ue_ik_json_canonical_bridge_v1" &&
        Scene.FoundationRouteId ==
            "ue_ik_json_fk_pelvis_limb_ik_candidate_v1" &&
        !Scene.FoundationFrozen &&
        !Scene.RouteSelected &&
        !Scene.RouteAdopted;
}

bool FootLockComparisonAvailable(const ReviewScene& Scene)
{
    const ReviewClipInfo* Clip = CurrentReviewClipInfo(Scene);
    return Clip != nullptr && Clip->SourceMotionFootLockEnabled &&
        Clip->SourceMotionFootLockSuccess;
}

ReviewLane ResolveFootLockDisplayLane(
    const ReviewScene& Scene,
    const bool FootLockEnabled)
{
    if (!FootLockComparisonAvailable(Scene)) return ReviewLane::Final;
    return FootLockEnabled ? ReviewLane::Final : ReviewLane::Foundation;
}

FootLockDeltaSummary MeasureFootLockDelta(
    const ReviewScene& Scene,
    const GoalHistory& History)
{
    FootLockDeltaSummary Summary;
    const std::size_t BoneCount = Scene.TargetBones.size();
    if (BoneCount == 0 ||
        Scene.Foundation.ModelPositions.size() != BoneCount ||
        Scene.Final.ModelPositions.size() != BoneCount ||
        Scene.Foundation.ModelRotations.size() != BoneCount ||
        Scene.Final.ModelRotations.size() != BoneCount)
    {
        return Summary;
    }

    const auto PositionDistance = [](const Vec3& Left, const Vec3& Right)
    {
        const float X = Left.X - Right.X;
        const float Y = Left.Y - Right.Y;
        const float Z = Left.Z - Right.Z;
        return std::sqrt(X * X + Y * Y + Z * Z);
    };
    const auto RotationDistance = [](
        const Quaternion& Left,
        const Quaternion& Right)
    {
        const float LeftLength = std::sqrt(
            Left.X * Left.X + Left.Y * Left.Y + Left.Z * Left.Z +
            Left.W * Left.W);
        const float RightLength = std::sqrt(
            Right.X * Right.X + Right.Y * Right.Y + Right.Z * Right.Z +
            Right.W * Right.W);
        if (LeftLength <= std::numeric_limits<float>::epsilon() ||
            RightLength <= std::numeric_limits<float>::epsilon())
        {
            return 0.0F;
        }
        const float Dot = std::clamp(std::fabs(
            (Left.X * Right.X + Left.Y * Right.Y +
             Left.Z * Right.Z + Left.W * Right.W) /
            (LeftLength * RightLength)), 0.0F, 1.0F);
        return 2.0F * std::acos(Dot) * 57.29577951308232F;
    };

    Summary.PoseValid = true;
    for (std::size_t BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
    {
        const float Position = PositionDistance(
            Scene.Foundation.ModelPositions[BoneIndex],
            Scene.Final.ModelPositions[BoneIndex]);
        if (Position > Summary.MaximumBonePositionCm)
        {
            Summary.MaximumBonePositionCm = Position;
            Summary.MaximumBoneName = Scene.TargetBones[BoneIndex].Name;
        }
        Summary.MaximumBoneRotationDegrees = std::max(
            Summary.MaximumBoneRotationDegrees,
            RotationDistance(
                Scene.Foundation.ModelRotations[BoneIndex],
                Scene.Final.ModelRotations[BoneIndex]));
    }

    for (const RetargetChain& Chain : Scene.RetargetChains)
    {
        if (Chain.Label != "LeftLeg" && Chain.Label != "RightLeg")
            continue;
        if (Chain.TargetGoalBone < 0 ||
            static_cast<std::size_t>(Chain.TargetGoalBone) >= BoneCount)
            continue;
        const std::size_t Goal =
            static_cast<std::size_t>(Chain.TargetGoalBone);
        const float Delta = PositionDistance(
            Scene.Foundation.ModelPositions[Goal],
            Scene.Final.ModelPositions[Goal]);
        if (Chain.Label == "LeftLeg")
            Summary.LeftFootGoalPositionCm = Delta;
        else
            Summary.RightFootGoalPositionCm = Delta;
    }

    bool SawFootHistory = false;
    for (const GoalTrail& Trail : History.Trails)
    {
        if (Trail.ChainIndex >= Scene.RetargetChains.size()) continue;
        const std::string& Label =
            Scene.RetargetChains[Trail.ChainIndex].Label;
        if (Label != "LeftLeg" && Label != "RightLeg") continue;
        if (Trail.TargetFoundation.size() != Trail.TargetFinal.size())
            return Summary;
        SawFootHistory = true;
        for (std::size_t Sample = 0;
             Sample < Trail.TargetFoundation.size(); ++Sample)
        {
            Summary.MaximumRecentFootGoalPositionCm = std::max(
                Summary.MaximumRecentFootGoalPositionCm,
                PositionDistance(
                    Trail.TargetFoundation[Sample],
                    Trail.TargetFinal[Sample]));
        }
    }
    Summary.GoalHistoryValid = SawFootHistory;
    return Summary;
}

std::vector<Vec3> BuildAnchorAlignedSourceGhost(
    const ReviewScene& Scene,
    const PoseLane& TargetLane)
{
    return BuildAnchorAlignedSourcePoints(
        Scene, TargetLane, Scene.Source.ModelPositions);
}

std::vector<Vec3> BuildAnchorAlignedSourcePoints(
    const ReviewScene& Scene,
    const PoseLane& TargetLane,
    const std::vector<Vec3>& SourcePoints)
{
    std::vector<Vec3> Result;
    Result.reserve(SourcePoints.size());
    for (const Vec3& Position : SourcePoints)
    {
        Vec3 Aligned;
        if (!AlignSourcePoint(
                Scene.GhostAnchor, Scene.Source, TargetLane,
                Position, Aligned))
        {
            return {};
        }
        Result.push_back(Aligned);
    }
    return Result;
}

std::vector<CameraFollowTarget> BuildCameraFollowTargets(
    const ReviewScene& Scene)
{
    std::vector<CameraFollowTarget> Result;
    Result.push_back({
        "Root", Scene.SourceRootBone, Scene.TargetHipsBone});
    Result.push_back({
        "Pelvis / Hips", Scene.SourcePelvisBone,
        Scene.TargetHipsBone});
    for (const RetargetChain& Chain : Scene.RetargetChains)
    {
        if (Chain.IkMode == "fk_only" ||
            Chain.SourceGoalBone < 0 || Chain.TargetGoalBone < 0)
        {
            continue;
        }
        Result.push_back({
            "IK Goal · " + Chain.Label,
            Chain.SourceGoalBone,
            Chain.TargetGoalBone});
    }
    return Result;
}

bool ResolveCameraFollowPoint(
    const ReviewScene& Scene,
    const ReviewLane Lane,
    const CameraFollowTarget& Target,
    Vec3& OutPoint)
{
    if (Lane == ReviewLane::Original)
    {
        if (Target.SourceBone < 0 ||
            static_cast<std::size_t>(Target.SourceBone) >=
                Scene.Source.ModelPositions.size())
        {
            return false;
        }
        OutPoint = Scene.Source.ModelPositions[
            static_cast<std::size_t>(Target.SourceBone)];
        return true;
    }
    if (Target.TargetBone < 0)
        return false;
    const PoseLane* Pose = nullptr;
    switch (Lane)
    {
    case ReviewLane::Fk:
        Pose = &Scene.Fk;
        break;
    case ReviewLane::Foundation:
        Pose = &Scene.Foundation;
        break;
    case ReviewLane::Final:
        Pose = &Scene.Final;
        break;
    case ReviewLane::Original:
        break;
    }
    if (Pose == nullptr ||
        static_cast<std::size_t>(Target.TargetBone) >=
            Pose->ModelPositions.size())
    {
        return false;
    }
    OutPoint = Pose->ModelPositions[
        static_cast<std::size_t>(Target.TargetBone)];
    return true;
}

GoalHistoryLoadResult LoadReviewGoalHistory(
    const ReviewScene& Scene,
    const std::size_t MaximumSamples)
{
    GoalHistoryLoadResult Result;
    if (Scene.ClipFrameCount == 0 ||
        Scene.FrameIndex >= Scene.ClipFrameCount ||
        MaximumSamples == 0)
    {
        Result.Errors.emplace_back("Goal history request is out of range");
        return Result;
    }
    // The Viewer contract is deliberately fixed to the latest 50 clip-local
    // samples. Callers may request fewer for tests, but never more.
    const std::size_t SampleCount = std::min<std::size_t>(
        {MaximumSamples, 50U,
         static_cast<std::size_t>(Scene.FrameIndex + 1U)});
    const std::uint64_t FirstFrame =
        Scene.FrameIndex + 1U - SampleCount;

    std::vector<PoseLane> SourceFrames;
    std::vector<PoseLane> FkFrames;
    std::vector<PoseLane> FoundationFrames;
    std::vector<PoseLane> FinalFrames;
    std::string Error;
    if (!ReadPoseRange(
            Scene.PackageDirectory, Scene.SourcePoseRelativePath,
            FirstFrame, SampleCount, Scene.SourceBones.size(),
            SourceFrames, Error) ||
        !ReadPoseRange(
            Scene.PackageDirectory, Scene.FkPoseRelativePath,
            FirstFrame, SampleCount, Scene.TargetBones.size(),
            FkFrames, Error) ||
        !ReadPoseRange(
            Scene.PackageDirectory, Scene.FoundationPoseRelativePath,
            FirstFrame, SampleCount, Scene.TargetBones.size(),
            FoundationFrames, Error) ||
        !ReadPoseRange(
            Scene.PackageDirectory, Scene.FinalPoseRelativePath,
            FirstFrame, SampleCount, Scene.TargetBones.size(),
            FinalFrames, Error))
    {
        Result.Errors.push_back(std::move(Error));
        return Result;
    }
    if (Scene.UEIKJsonDisplayBasisConversion)
    {
        for (PoseLane& Pose : SourceFrames)
            ConvertUEIKJsonPoseToViewerSpace(Pose);
        for (PoseLane& Pose : FkFrames)
            ConvertUEIKJsonPoseToViewerSpace(Pose);
        for (PoseLane& Pose : FoundationFrames)
            ConvertUEIKJsonPoseToViewerSpace(Pose);
        for (PoseLane& Pose : FinalFrames)
            ConvertUEIKJsonPoseToViewerSpace(Pose);
    }

    GoalHistory History;
    History.FirstFrame = FirstFrame;
    History.LastFrame = Scene.FrameIndex;
    History.SampleCount = SampleCount;
    for (std::size_t ChainIndex = 0;
         ChainIndex < Scene.RetargetChains.size(); ++ChainIndex)
    {
        const RetargetChain& Chain = Scene.RetargetChains[ChainIndex];
        if (Chain.IkMode == "fk_only") continue;
        const std::size_t SourceGoal =
            static_cast<std::size_t>(Chain.SourceGoalBone);
        const std::size_t TargetGoal =
            static_cast<std::size_t>(Chain.TargetGoalBone);
        GoalTrail Trail;
        Trail.ChainIndex = ChainIndex;
        Trail.SourceOriginal.reserve(SampleCount);
        Trail.SourceAlignedFk.reserve(SampleCount);
        Trail.TargetFk.reserve(SampleCount);
        Trail.SourceAlignedFoundation.reserve(SampleCount);
        Trail.TargetFoundation.reserve(SampleCount);
        Trail.TargetFinal.reserve(SampleCount);
        for (std::size_t Sample = 0; Sample < SampleCount; ++Sample)
        {
            const Vec3 SourcePoint =
                SourceFrames[Sample].ModelPositions[SourceGoal];
            Vec3 AlignedFk;
            Vec3 AlignedFoundation;
            if (!AlignSourcePoint(
                    Scene.GhostAnchor, SourceFrames[Sample],
                    FkFrames[Sample], SourcePoint, AlignedFk) ||
                !AlignSourcePoint(
                    Scene.GhostAnchor, SourceFrames[Sample],
                    FoundationFrames[Sample], SourcePoint,
                    AlignedFoundation))
            {
                Result.Errors.emplace_back(
                    "Goal history anchor alignment failed");
                return Result;
            }
            Trail.SourceOriginal.push_back(SourcePoint);
            Trail.SourceAlignedFk.push_back(AlignedFk);
            Trail.TargetFk.push_back(
                FkFrames[Sample].ModelPositions[TargetGoal]);
            Trail.SourceAlignedFoundation.push_back(
                AlignedFoundation);
            Trail.TargetFoundation.push_back(
                FoundationFrames[Sample].ModelPositions[TargetGoal]);
            Trail.TargetFinal.push_back(
                FinalFrames[Sample].ModelPositions[TargetGoal]);
        }
        History.Trails.push_back(std::move(Trail));
    }

    Result.Success = true;
    Result.History = std::move(History);
    return Result;
}

} // namespace skrtg::viewer
