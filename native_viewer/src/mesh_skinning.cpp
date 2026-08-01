#include "skrtg/viewer/mesh_skinning.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <sstream>

namespace skrtg::viewer
{
namespace
{
Vec3 RotateByQuaternion(const Quaternion& Rotation, const Vec3& Value)
{
    const Vec3 Q = {Rotation.X, Rotation.Y, Rotation.Z};
    const Vec3 FirstCross = {
        Q.Y * Value.Z - Q.Z * Value.Y,
        Q.Z * Value.X - Q.X * Value.Z,
        Q.X * Value.Y - Q.Y * Value.X};
    const Vec3 SecondCross = {
        Q.Y * FirstCross.Z - Q.Z * FirstCross.Y,
        Q.Z * FirstCross.X - Q.X * FirstCross.Z,
        Q.X * FirstCross.Y - Q.Y * FirstCross.X};
    return {
        Value.X + 2.0F *
            (Rotation.W * FirstCross.X + SecondCross.X),
        Value.Y + 2.0F *
            (Rotation.W * FirstCross.Y + SecondCross.Y),
        Value.Z + 2.0F *
            (Rotation.W * FirstCross.Z + SecondCross.Z)};
}

Vec3 ApplyTransform(
    const Vec3& Translation,
    const Quaternion& Rotation,
    const Vec3& Scale,
    const Vec3& Point)
{
    const Vec3 Scaled = {
        Point.X * Scale.X,
        Point.Y * Scale.Y,
        Point.Z * Scale.Z};
    const Vec3 Rotated = RotateByQuaternion(Rotation, Scaled);
    return {
        Rotated.X + Translation.X,
        Rotated.Y + Translation.Y,
        Rotated.Z + Translation.Z};
}

Vec3 ApplyAffine(
    const std::array<float, 12>& Affine,
    const Vec3& Point)
{
    return {
        Affine[0] + Affine[3] * Point.X +
            Affine[6] * Point.Y + Affine[9] * Point.Z,
        Affine[1] + Affine[4] * Point.X +
            Affine[7] * Point.Y + Affine[10] * Point.Z,
        Affine[2] + Affine[5] * Point.X +
            Affine[8] * Point.Y + Affine[11] * Point.Z};
}

bool IsFinite(const Vec3& Value)
{
    return std::isfinite(Value.X) && std::isfinite(Value.Y) &&
        std::isfinite(Value.Z);
}
} // namespace

MeshSkinningResult SkinMeshPackage(
    const MeshPackage& Package,
    const PoseLane& Pose)
{
    MeshSkinningResult Result;
    if (Pose.ModelPositions.size() != Pose.ModelRotations.size() ||
        Pose.ModelPositions.size() != Pose.ModelScales.size())
    {
        Result.Errors.emplace_back(
            "pose translation/rotation/scale arrays have different sizes");
        return Result;
    }

    Result.MeshPositions.reserve(Package.Meshes.size());
    for (const Mesh& MeshValue : Package.Meshes)
    {
        if (MeshValue.InfluenceOffsets.size() !=
                MeshValue.BindPositions.size() + 1 ||
            MeshValue.InfluenceClusterIndices.size() !=
                MeshValue.InfluenceWeights.size())
        {
            Result.Errors.emplace_back(
                "mesh influence arrays are inconsistent: " +
                MeshValue.Path);
            return Result;
        }

        std::vector<Vec3> Positions;
        Positions.reserve(MeshValue.BindPositions.size());
        for (std::size_t ControlPoint = 0;
             ControlPoint < MeshValue.BindPositions.size(); ++ControlPoint)
        {
            const Vec3 BindPosition =
                MeshValue.BindPositions[ControlPoint];
            const std::uint32_t InfluenceBegin =
                MeshValue.InfluenceOffsets[ControlPoint];
            const std::uint32_t InfluenceEnd =
                MeshValue.InfluenceOffsets[ControlPoint + 1];
            Vec3 Position{};
            float WeightSum = 0.0F;
            if (InfluenceBegin == InfluenceEnd)
            {
                Position = ApplyAffine(
                    MeshValue.FallbackAffine, BindPosition);
            }
            else
            {
                for (std::uint32_t Influence = InfluenceBegin;
                     Influence < InfluenceEnd; ++Influence)
                {
                    const std::uint32_t Cluster =
                        MeshValue.InfluenceClusterIndices[Influence];
                    if (Cluster >= MeshValue.ClusterBoneIndices.size() ||
                        Cluster >= MeshValue.ClusterBindOffsets.size())
                    {
                        Result.Errors.emplace_back(
                            "mesh influence references an invalid cluster: " +
                            MeshValue.Path);
                        return Result;
                    }
                    const std::uint32_t Bone =
                        MeshValue.ClusterBoneIndices[Cluster];
                    if (Bone >= Pose.ModelPositions.size())
                    {
                        Result.Errors.emplace_back(
                            "mesh cluster references an invalid pose bone: " +
                            MeshValue.Path);
                        return Result;
                    }
                    const float Weight =
                        MeshValue.InfluenceWeights[Influence];
                    const Vec3 Bound = ApplyAffine(
                        MeshValue.ClusterBindOffsets[Cluster], BindPosition);
                    const Vec3 World = ApplyTransform(
                        Pose.ModelPositions[Bone],
                        Pose.ModelRotations[Bone],
                        Pose.ModelScales[Bone],
                        Bound);
                    Position.X += World.X * Weight;
                    Position.Y += World.Y * Weight;
                    Position.Z += World.Z * Weight;
                    WeightSum += Weight;
                }

                if (MeshValue.SkinMode == "normalize" &&
                    WeightSum > 1.0e-8F)
                {
                    Position.X /= WeightSum;
                    Position.Y /= WeightSum;
                    Position.Z /= WeightSum;
                }
                else if (MeshValue.SkinMode == "total_one" &&
                         WeightSum < 1.0F)
                {
                    const float Remaining =
                        std::max(0.0F, 1.0F - WeightSum);
                    const Vec3 Fallback = ApplyAffine(
                        MeshValue.FallbackAffine, BindPosition);
                    Position.X += Fallback.X * Remaining;
                    Position.Y += Fallback.Y * Remaining;
                    Position.Z += Fallback.Z * Remaining;
                }
            }
            if (!IsFinite(Position))
            {
                std::ostringstream Message;
                Message << "non-finite skinned position in "
                        << MeshValue.Path << " at control point "
                        << ControlPoint;
                Result.Errors.push_back(Message.str());
                return Result;
            }
            Positions.push_back(Position);
        }
        Result.MeshPositions.push_back(std::move(Positions));
    }
    Result.Success = true;
    return Result;
}

std::vector<std::vector<Vec3>> BuildAnchorAlignedSourceMesh(
    const ReviewScene& Scene,
    const PoseLane& TargetLane,
    const std::vector<std::vector<Vec3>>& SourceMeshPositions)
{
    std::vector<std::vector<Vec3>> Result;
    Result.reserve(SourceMeshPositions.size());
    for (const std::vector<Vec3>& Positions : SourceMeshPositions)
    {
        Result.push_back(BuildAnchorAlignedSourcePoints(
            Scene, TargetLane, Positions));
        if (Result.back().size() != Positions.size())
            return {};
    }
    return Result;
}

} // namespace skrtg::viewer
