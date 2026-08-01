#pragma once

#include "skrtg/core/math/transform.h"
#include "skrtg/core/skeleton/runtime_skeleton.h"

#include <cstddef>
#include <string>
#include <vector>

namespace skrtg::core::animation
{
enum class PoseSpace
{
    Local,
    Model
};

class PoseBuffer
{
public:
    PoseBuffer() = default;
    PoseBuffer(PoseSpace SpaceValue, std::string SkeletonHierarchyHash);

    PoseSpace Space() const;
    const std::string& SkeletonHash() const;

    void ResizeToSkeleton(const skeleton::NormalizedRuntimeSkeleton& Skeleton, math::TransformRT Fill = math::IdentityTransform());
    bool IsSizedFor(const skeleton::NormalizedRuntimeSkeleton& Skeleton) const;

    std::size_t Size() const;
    bool Empty() const;

    math::TransformRT& operator[](std::size_t Index);
    const math::TransformRT& operator[](std::size_t Index) const;

    const std::vector<math::TransformRT>& Transforms() const;
    std::vector<math::TransformRT>& Transforms();

private:
    PoseSpace SpaceValue = PoseSpace::Local;
    std::string SkeletonHierarchyHash;
    std::vector<math::TransformRT> TransformRecords;
};

struct SampledClip
{
    std::string Name;
    std::string SourceStackName;
    double DurationSeconds = 0.0;
    double SampleRate = 0.0;
    std::vector<PoseBuffer> LocalFrames;
};

struct SamplingContext
{
    std::string RunId;
    std::string Notes;
};

const char* ToString(PoseSpace Space);
} // namespace skrtg::core::animation
