#include "skrtg/core/animation/pose.h"

#include <utility>

namespace skrtg::core::animation
{
PoseBuffer::PoseBuffer(PoseSpace SpaceValueIn, std::string SkeletonHierarchyHashIn)
    : SpaceValue(SpaceValueIn)
    , SkeletonHierarchyHash(std::move(SkeletonHierarchyHashIn))
{
}

PoseSpace PoseBuffer::Space() const
{
    return SpaceValue;
}

const std::string& PoseBuffer::SkeletonHash() const
{
    return SkeletonHierarchyHash;
}

void PoseBuffer::ResizeToSkeleton(const skeleton::NormalizedRuntimeSkeleton& Skeleton, math::TransformRT Fill)
{
    SkeletonHierarchyHash = Skeleton.GetIdentity().HierarchyHash;
    TransformRecords.assign(Skeleton.BoneCount(), Fill);
}

bool PoseBuffer::IsSizedFor(const skeleton::NormalizedRuntimeSkeleton& Skeleton) const
{
    return SkeletonHierarchyHash == Skeleton.GetIdentity().HierarchyHash && TransformRecords.size() == Skeleton.BoneCount();
}

std::size_t PoseBuffer::Size() const
{
    return TransformRecords.size();
}

bool PoseBuffer::Empty() const
{
    return TransformRecords.empty();
}

math::TransformRT& PoseBuffer::operator[](std::size_t Index)
{
    return TransformRecords.at(Index);
}

const math::TransformRT& PoseBuffer::operator[](std::size_t Index) const
{
    return TransformRecords.at(Index);
}

const std::vector<math::TransformRT>& PoseBuffer::Transforms() const
{
    return TransformRecords;
}

std::vector<math::TransformRT>& PoseBuffer::Transforms()
{
    return TransformRecords;
}

const char* ToString(PoseSpace Space)
{
    switch (Space)
    {
    case PoseSpace::Local:
        return "local";
    case PoseSpace::Model:
        return "model";
    default:
        return "unknown";
    }
}
} // namespace skrtg::core::animation
