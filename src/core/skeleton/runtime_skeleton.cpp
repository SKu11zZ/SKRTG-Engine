#include "skrtg/core/skeleton/runtime_skeleton.h"

#include <sstream>
#include <utility>

namespace skrtg::core::skeleton
{
void NormalizedRuntimeSkeleton::SetIdentity(SkeletonIdentity IdentityValue)
{
    Identity = std::move(IdentityValue);
}

const SkeletonIdentity& NormalizedRuntimeSkeleton::GetIdentity() const
{
    return Identity;
}

int NormalizedRuntimeSkeleton::AddBone(RuntimeBone Bone)
{
    BoneRecords.push_back(std::move(Bone));
    return static_cast<int>(BoneRecords.size() - 1);
}

std::size_t NormalizedRuntimeSkeleton::BoneCount() const
{
    return BoneRecords.size();
}

bool NormalizedRuntimeSkeleton::Empty() const
{
    return BoneRecords.empty();
}

const RuntimeBone& NormalizedRuntimeSkeleton::BoneAt(std::size_t Index) const
{
    return BoneRecords.at(Index);
}

const std::vector<RuntimeBone>& NormalizedRuntimeSkeleton::Bones() const
{
    return BoneRecords;
}

void NormalizedRuntimeSkeleton::SetRawToNormalized(std::vector<int> Mapping)
{
    RawToNormalizedMap = std::move(Mapping);
}

void NormalizedRuntimeSkeleton::SetNormalizedToRaw(std::vector<int> Mapping)
{
    NormalizedToRawMap = std::move(Mapping);
}

const std::vector<int>& NormalizedRuntimeSkeleton::RawToNormalized() const
{
    return RawToNormalizedMap;
}

const std::vector<int>& NormalizedRuntimeSkeleton::NormalizedToRaw() const
{
    return NormalizedToRawMap;
}

bool NormalizedRuntimeSkeleton::ValidateParentIndexInvariant(std::vector<std::string>* Errors) const
{
    bool bValid = true;
    for (std::size_t Index = 0; Index < BoneRecords.size(); ++Index)
    {
        const int ParentIndex = BoneRecords[Index].ParentIndex;
        if (ParentIndex < -1 || ParentIndex >= static_cast<int>(Index))
        {
            bValid = false;
            if (Errors != nullptr)
            {
                std::ostringstream Message;
                Message << "bone index " << Index << " has invalid parent index " << ParentIndex
                        << "; parent must be -1 or less than child index";
                Errors->push_back(Message.str());
            }
        }
    }
    return bValid;
}

SkeletonIdentityCheckResult CheckSkeletonIdentity(const NormalizedRuntimeSkeleton& Reference,
                                                  const NormalizedRuntimeSkeleton& Candidate)
{
    SkeletonIdentityCheckResult Result;
    Result.Report.JobName = "SkeletonIdentityCheck";

    if (Reference.BoneCount() != Candidate.BoneCount())
    {
        Result.Status = SkeletonIdentityStatus::BoneCountMismatch;
        Result.Report.Status = "blocked";
        Result.Report.AddIssue(validation::Severity::Error,
                               "SKELETON_BONE_COUNT_MISMATCH",
                               "Skeleton bone counts differ; hierarchy reuse is not safe.",
                               "block_animation_sampling_until_identity_is_reviewed");
        return Result;
    }

    const SkeletonIdentity& ReferenceId = Reference.GetIdentity();
    const SkeletonIdentity& CandidateId = Candidate.GetIdentity();
    if (ReferenceId.HierarchyHash != CandidateId.HierarchyHash)
    {
        Result.Status = SkeletonIdentityStatus::DifferentHierarchy;
        Result.Report.Status = "blocked";
        Result.Report.AddIssue(validation::Severity::Error,
                               "SKELETON_HIERARCHY_HASH_MISMATCH",
                               "Hierarchy hashes differ; this is not the same topology.",
                               "block_animation_sampling_until_mapping_is_resolved");
        return Result;
    }

    Result.SameTopology = true;
    if (ReferenceId.RestPoseHash != CandidateId.RestPoseHash)
    {
        Result.Status = SkeletonIdentityStatus::SameHierarchyDifferentRestPose;
        Result.Report.Status = "needs_validation";
        Result.Report.AddIssue(validation::Severity::Warning,
                               "SAME_HIERARCHY_DIFFERENT_REST_POSE",
                               "Hierarchy hashes match but rest pose hashes differ; this is same topology only, not full skeleton identity.",
                               "review_rest_pose_delta_before_sampling_or_retarget_readiness");
        return Result;
    }

    Result.Status = SkeletonIdentityStatus::SameHierarchyAndRestPose;
    Result.SameRestPose = true;
    Result.ReadyForReuse = true;
    Result.Report.Status = "ready_for_reuse";
    Result.Report.AddIssue(validation::Severity::Info,
                           "SKELETON_IDENTITY_MATCH",
                           "Hierarchy and rest pose hashes match.",
                           "allow_reuse_for_non_writing_sampling_scaffold");
    return Result;
}

const char* ToString(SkeletonIdentityStatus Status)
{
    switch (Status)
    {
    case SkeletonIdentityStatus::SameHierarchyAndRestPose:
        return "same_hierarchy_and_rest_pose";
    case SkeletonIdentityStatus::SameHierarchyDifferentRestPose:
        return "same_hierarchy_different_rest_pose";
    case SkeletonIdentityStatus::DifferentHierarchy:
        return "different_hierarchy";
    case SkeletonIdentityStatus::BoneCountMismatch:
        return "bone_count_mismatch";
    default:
        return "unknown";
    }
}
} // namespace skrtg::core::skeleton
