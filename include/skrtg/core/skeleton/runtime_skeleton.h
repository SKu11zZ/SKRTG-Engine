#pragma once

#include "skrtg/core/math/transform.h"
#include "skrtg/core/validation/report.h"

#include <string>
#include <vector>

namespace skrtg::core::skeleton
{
struct SkeletonIdentity
{
    std::string HierarchyHash;
    std::string RestPoseHash;
    std::string SourceAssetId;
    std::string SourceArtifactPath;
};

struct RuntimeBone
{
    std::string Name;
    std::string RawPath;
    int ParentIndex = -1;
    int RawIndex = -1;
    math::TransformRT LocalRest;
};

class NormalizedRuntimeSkeleton
{
public:
    void SetIdentity(SkeletonIdentity IdentityValue);
    const SkeletonIdentity& GetIdentity() const;

    int AddBone(RuntimeBone Bone);
    std::size_t BoneCount() const;
    bool Empty() const;

    const RuntimeBone& BoneAt(std::size_t Index) const;
    const std::vector<RuntimeBone>& Bones() const;

    void SetRawToNormalized(std::vector<int> Mapping);
    void SetNormalizedToRaw(std::vector<int> Mapping);
    const std::vector<int>& RawToNormalized() const;
    const std::vector<int>& NormalizedToRaw() const;

    bool ValidateParentIndexInvariant(std::vector<std::string>* Errors = nullptr) const;

private:
    SkeletonIdentity Identity;
    std::vector<RuntimeBone> BoneRecords;
    std::vector<int> RawToNormalizedMap;
    std::vector<int> NormalizedToRawMap;
};

enum class SkeletonIdentityStatus
{
    SameHierarchyAndRestPose,
    SameHierarchyDifferentRestPose,
    DifferentHierarchy,
    BoneCountMismatch
};

struct SkeletonIdentityCheckResult
{
    SkeletonIdentityStatus Status = SkeletonIdentityStatus::DifferentHierarchy;
    bool SameTopology = false;
    bool SameRestPose = false;
    bool ReadyForReuse = false;
    validation::JobReport Report;
};

SkeletonIdentityCheckResult CheckSkeletonIdentity(const NormalizedRuntimeSkeleton& Reference,
                                                  const NormalizedRuntimeSkeleton& Candidate);

const char* ToString(SkeletonIdentityStatus Status);
} // namespace skrtg::core::skeleton
