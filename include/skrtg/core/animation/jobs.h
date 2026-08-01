#pragma once

#include "skrtg/core/animation/pose.h"
#include "skrtg/core/validation/report.h"

namespace skrtg::core::animation
{
struct LocalToModelJobInput
{
    const skeleton::NormalizedRuntimeSkeleton* Skeleton = nullptr;
    const PoseBuffer* LocalPose = nullptr;
};

struct LocalToModelJobOutput
{
    PoseBuffer ModelPose{PoseSpace::Model, {}};
    validation::JobReport Report;
};

class LocalToModelJob
{
public:
    static LocalToModelJobOutput Run(const LocalToModelJobInput& Input);
};

class SampleAnimationLocalJob
{
public:
    static validation::JobReport DescribeContract();
};

class ModelToLocalJob
{
public:
    static validation::JobReport DescribeContract();
};
} // namespace skrtg::core::animation
