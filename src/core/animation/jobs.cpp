#include "skrtg/core/animation/jobs.h"

#include <string>
#include <vector>

namespace skrtg::core::animation
{
LocalToModelJobOutput LocalToModelJob::Run(const LocalToModelJobInput& Input)
{
    LocalToModelJobOutput Output;
    Output.Report.JobName = "LocalToModelJob";

    if (Input.Skeleton == nullptr)
    {
        Output.Report.Status = "failed";
        Output.Report.AddIssue(validation::Severity::Error,
                               "MISSING_SKELETON",
                               "LocalToModelJob requires a normalized runtime skeleton.",
                               "provide_normalized_runtime_skeleton");
        return Output;
    }

    if (Input.LocalPose == nullptr)
    {
        Output.Report.Status = "failed";
        Output.Report.AddIssue(validation::Severity::Error,
                               "MISSING_LOCAL_POSE",
                               "LocalToModelJob requires a local pose buffer.",
                               "provide_local_pose_buffer");
        return Output;
    }

    if (Input.LocalPose->Space() != PoseSpace::Local)
    {
        Output.Report.Status = "failed";
        Output.Report.AddIssue(validation::Severity::Error,
                               "POSE_SPACE_NOT_LOCAL",
                               "LocalToModelJob input pose must be local space.",
                               "run_sampling_or_conversion_to_local_pose_first");
        return Output;
    }

    if (!Input.LocalPose->IsSizedFor(*Input.Skeleton))
    {
        Output.Report.Status = "failed";
        Output.Report.AddIssue(validation::Severity::Error,
                               "POSE_BUFFER_SIZE_MISMATCH",
                               "Local pose buffer does not match skeleton hierarchy hash and bone count.",
                               "resize_or_rebuild_pose_for_the_skeleton");
        return Output;
    }

    std::vector<std::string> ParentErrors;
    if (!Input.Skeleton->ValidateParentIndexInvariant(&ParentErrors))
    {
        Output.Report.Status = "failed";
        for (const std::string& Error : ParentErrors)
        {
            Output.Report.AddIssue(validation::Severity::Error,
                                   "PARENT_INDEX_INVARIANT_FAILED",
                                   Error,
                                   "fix_normalized_runtime_skeleton_order");
        }
        return Output;
    }

    Output.ModelPose = PoseBuffer(PoseSpace::Model, Input.Skeleton->GetIdentity().HierarchyHash);
    Output.ModelPose.ResizeToSkeleton(*Input.Skeleton);

    for (std::size_t Index = 0; Index < Input.Skeleton->BoneCount(); ++Index)
    {
        const skeleton::RuntimeBone& Bone = Input.Skeleton->BoneAt(Index);
        if (Bone.ParentIndex < 0)
        {
            Output.ModelPose[Index] = (*Input.LocalPose)[Index];
        }
        else
        {
            Output.ModelPose[Index] = math::Compose(Output.ModelPose[static_cast<std::size_t>(Bone.ParentIndex)], (*Input.LocalPose)[Index]);
        }
    }

    Output.Report.Status = "success";
    Output.Report.AddIssue(validation::Severity::Info,
                           "LOCAL_TO_MODEL_CONSISTENCY_PRODUCED",
                           "Model pose was rebuilt with model[child] = model[parent] * local[child].",
                           "use_for_contract_tests_only_until_sampling_slice");
    return Output;
}

validation::JobReport SampleAnimationLocalJob::DescribeContract()
{
    validation::JobReport Report;
    Report.JobName = "SampleAnimationLocalJob";
    Report.Status = "prototype_boundary_no_sampling_in_D1_3A";
    Report.AddIssue(validation::Severity::Info,
                    "ANIMATION_SAMPLING_NOT_IMPLEMENTED_D1_3A",
                    "D1-3A defines the boundary only; FBX curve sampling and animation sampling are out of scope.",
                    "assign_a_future_sampling_slice_before_producing_sampled_local_poses");
    return Report;
}

validation::JobReport ModelToLocalJob::DescribeContract()
{
    validation::JobReport Report;
    Report.JobName = "ModelToLocalJob";
    Report.Status = "placeholder_boundary_no_behavior_in_D1_3A";
    Report.AddIssue(validation::Severity::Info,
                    "MODEL_TO_LOCAL_NOT_IMPLEMENTED_D1_3A",
                    "D1-3A keeps model-to-local as a future boundary only.",
                    "implement_only_if_a_future_slice_requires_it");
    return Report;
}
} // namespace skrtg::core::animation
