#include "skrtg/core/animation/jobs.h"
#include "skrtg/core/math/transform.h"
#include "skrtg/core/skeleton/runtime_skeleton.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using skrtg::core::animation::LocalToModelJob;
using skrtg::core::animation::LocalToModelJobInput;
using skrtg::core::animation::PoseBuffer;
using skrtg::core::animation::PoseSpace;
using skrtg::core::math::FromAxisAngleDegrees;
using skrtg::core::math::IdentityTransform;
using skrtg::core::math::InverseUnitScaleTransform;
using skrtg::core::math::NearlyEqual;
using skrtg::core::math::RelativeUnitScaleTransform;
using skrtg::core::math::TransformRT;
using skrtg::core::math::Vec3;
using skrtg::core::skeleton::CheckSkeletonIdentity;
using skrtg::core::skeleton::NormalizedRuntimeSkeleton;
using skrtg::core::skeleton::RuntimeBone;
using skrtg::core::skeleton::SkeletonIdentity;
using skrtg::core::skeleton::SkeletonIdentityStatus;

int Fail(const std::string& Message)
{
    std::cerr << "FAIL: " << Message << "\n";
    return EXIT_FAILURE;
}

NormalizedRuntimeSkeleton MakeTwoBoneSkeleton(std::string RestPoseHash)
{
    NormalizedRuntimeSkeleton Skeleton;
    Skeleton.SetIdentity({"hierarchy:root_child", std::move(RestPoseHash), "test_asset", "test_artifact"});
    Skeleton.AddBone({"Root", "Root", -1, 10, IdentityTransform()});
    Skeleton.AddBone({"Child", "Root/Child", 0, 11, IdentityTransform()});
    Skeleton.SetRawToNormalized({0, 1});
    Skeleton.SetNormalizedToRaw({10, 11});
    return Skeleton;
}

int TestParentIndexInvariant()
{
    const NormalizedRuntimeSkeleton Valid = MakeTwoBoneSkeleton("rest:a");
    std::vector<std::string> Errors;
    if (!Valid.ValidateParentIndexInvariant(&Errors))
    {
        return Fail("valid two-bone skeleton failed parent-index invariant");
    }

    NormalizedRuntimeSkeleton Invalid;
    Invalid.SetIdentity({"hierarchy:invalid", "rest:invalid", "test_asset", "test_artifact"});
    Invalid.AddBone({"Root", "Root", -1, 0, IdentityTransform()});
    Invalid.AddBone({"Child", "Root/Child", 1, 1, IdentityTransform()});
    if (Invalid.ValidateParentIndexInvariant(&Errors))
    {
        return Fail("invalid child parent index was accepted");
    }
    if (Errors.empty())
    {
        return Fail("invalid parent-index check did not report an error");
    }

    return EXIT_SUCCESS;
}

int TestLocalToModel()
{
    const NormalizedRuntimeSkeleton Skeleton = MakeTwoBoneSkeleton("rest:a");

    PoseBuffer LocalPose(PoseSpace::Local, {});
    LocalPose.ResizeToSkeleton(Skeleton);

    TransformRT Root = IdentityTransform();
    Root.TranslationCm = {1.0, 0.0, 0.0};
    Root.Rotation = FromAxisAngleDegrees({0.0, 0.0, 1.0}, 90.0);

    TransformRT Child = IdentityTransform();
    Child.TranslationCm = {2.0, 0.0, 0.0};

    LocalPose[0] = Root;
    LocalPose[1] = Child;

    const auto Output = LocalToModelJob::Run({&Skeleton, &LocalPose});
    if (Output.Report.HasErrors())
    {
        return Fail("LocalToModelJob reported errors for valid input");
    }
    if (!Output.ModelPose.IsSizedFor(Skeleton))
    {
        return Fail("LocalToModelJob output pose is not sized for skeleton");
    }

    const Vec3 ExpectedChildTranslation{1.0, 2.0, 0.0};
    if (!NearlyEqual(Output.ModelPose[1].TranslationCm, ExpectedChildTranslation, 1.0e-9))
    {
        return Fail("LocalToModelJob did not apply parent rotation before child translation");
    }

    return EXIT_SUCCESS;
}

int TestSkeletonIdentityRestMismatch()
{
    const NormalizedRuntimeSkeleton Reference = MakeTwoBoneSkeleton("rest:a");
    const NormalizedRuntimeSkeleton Candidate = MakeTwoBoneSkeleton("rest:b");
    const auto Result = CheckSkeletonIdentity(Reference, Candidate);
    if (Result.Status != SkeletonIdentityStatus::SameHierarchyDifferentRestPose)
    {
        return Fail("same hierarchy with different rest pose did not produce distinct status");
    }
    if (!Result.SameTopology || Result.SameRestPose || Result.ReadyForReuse)
    {
        return Fail("identity result incorrectly treated rest mismatch as ready");
    }
    if (Result.Report.Issues.empty())
    {
        return Fail("identity rest mismatch did not report a validation issue");
    }

    return EXIT_SUCCESS;
}

int TestSkeletonIdentityFullMatch()
{
    const NormalizedRuntimeSkeleton Reference = MakeTwoBoneSkeleton("rest:a");
    const NormalizedRuntimeSkeleton Candidate = MakeTwoBoneSkeleton("rest:a");
    const auto Result = CheckSkeletonIdentity(Reference, Candidate);
    if (Result.Status != SkeletonIdentityStatus::SameHierarchyAndRestPose)
    {
        return Fail("matching hierarchy and rest pose did not produce ready identity status");
    }
    if (!Result.SameTopology || !Result.SameRestPose || !Result.ReadyForReuse)
    {
        return Fail("matching identity was not marked ready for reuse");
    }

    return EXIT_SUCCESS;
}

int TestSkeletonIdentityHierarchyMismatch()
{
    const NormalizedRuntimeSkeleton Reference = MakeTwoBoneSkeleton("rest:a");
    NormalizedRuntimeSkeleton Candidate = MakeTwoBoneSkeleton("rest:a");
    Candidate.SetIdentity({"hierarchy:other", "rest:a", "test_asset", "test_artifact"});

    const auto Result = CheckSkeletonIdentity(Reference, Candidate);
    if (Result.Status != SkeletonIdentityStatus::DifferentHierarchy)
    {
        return Fail("different hierarchy did not produce different_hierarchy status");
    }
    if (Result.SameTopology || Result.ReadyForReuse)
    {
        return Fail("different hierarchy was incorrectly marked reusable");
    }

    return EXIT_SUCCESS;
}

int TestPoseBufferSizing()
{
    const NormalizedRuntimeSkeleton Skeleton = MakeTwoBoneSkeleton("rest:a");
    PoseBuffer Pose(PoseSpace::Local, {});
    if (Pose.IsSizedFor(Skeleton))
    {
        return Fail("empty pose buffer was considered sized for skeleton");
    }

    Pose.ResizeToSkeleton(Skeleton);
    if (!Pose.IsSizedFor(Skeleton))
    {
        return Fail("resized pose buffer was not considered sized for skeleton");
    }
    if (Pose.Size() != Skeleton.BoneCount())
    {
        return Fail("pose buffer size did not match skeleton bone count");
    }

    return EXIT_SUCCESS;
}

int TestPrototypeBoundaries()
{
    const auto SamplingReport = skrtg::core::animation::SampleAnimationLocalJob::DescribeContract();
    if (SamplingReport.Status != "prototype_boundary_no_sampling_in_D1_3A" || SamplingReport.Issues.empty())
    {
        return Fail("SampleAnimationLocalJob boundary did not report D1-3A non-implementation status");
    }
    return EXIT_SUCCESS;
}

int TestUnitScaleInverseAndRelativeTransform()
{
    TransformRT Parent = IdentityTransform();
    Parent.TranslationCm = {4.0, -2.0, 7.0};
    Parent.Rotation = FromAxisAngleDegrees({0.0, 1.0, 0.0}, 37.0);

    TransformRT Local = IdentityTransform();
    Local.TranslationCm = {3.0, 5.0, -1.0};
    Local.Rotation = FromAxisAngleDegrees({1.0, 0.0, 0.0}, -22.0);
    const TransformRT Child = skrtg::core::math::Compose(Parent, Local);

    TransformRT Inverse;
    TransformRT Recovered;
    if (!InverseUnitScaleTransform(Parent, Inverse) ||
        !RelativeUnitScaleTransform(Parent, Child, Recovered) ||
        !NearlyEqual(skrtg::core::math::Compose(Inverse, Parent),
                     IdentityTransform(), 1.0e-9, 1.0e-9, 1.0e-9) ||
        !NearlyEqual(Recovered, Local, 1.0e-9, 1.0e-9, 1.0e-9))
    {
        return Fail("unit-scale inverse/relative transform did not round-trip");
    }

    TransformRT Unsupported = Parent;
    Unsupported.Scale = {2.0, 1.0, 1.0};
    if (InverseUnitScaleTransform(Unsupported, Inverse) ||
        RelativeUnitScaleTransform(Parent, Unsupported, Recovered))
    {
        return Fail("unit-scale inverse/relative transform accepted unsupported scale");
    }

    TransformRT InvalidRotation = Parent;
    InvalidRotation.Rotation = {};
    InvalidRotation.Rotation.W = 0.0;
    if (InverseUnitScaleTransform(InvalidRotation, Inverse))
    {
        return Fail("unit-scale inverse accepted a zero quaternion");
    }
    return EXIT_SUCCESS;
}
} // namespace

int main()
{
    const int Results[] = {
        TestParentIndexInvariant(),
        TestLocalToModel(),
        TestSkeletonIdentityRestMismatch(),
        TestSkeletonIdentityFullMatch(),
        TestSkeletonIdentityHierarchyMismatch(),
        TestPoseBufferSizing(),
        TestPrototypeBoundaries(),
        TestUnitScaleInverseAndRelativeTransform(),
    };

    for (const int Result : Results)
    {
        if (Result != EXIT_SUCCESS)
        {
            return Result;
        }
    }

    std::cout << "core_contract_tests passed\n";
    return EXIT_SUCCESS;
}
