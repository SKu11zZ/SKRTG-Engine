#include "skrtg/core/ik/two_bone_ik.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace
{
using skrtg::core::ik::EndOrientationPolicy;
using skrtg::core::ik::PoleFallbackPolicy;
using skrtg::core::ik::SolveAnalyticTwoBone;
using skrtg::core::ik::TwoBoneFailureReason;
using skrtg::core::ik::TwoBoneIkInput;
using skrtg::core::ik::TwoBoneIkResult;
using skrtg::core::ik::TwoBoneSolveStatus;
using skrtg::core::math::FromAxisAngleDegrees;
using skrtg::core::math::IdentityTransform;
using skrtg::core::math::Length;
using skrtg::core::math::NearlyEqual;
using skrtg::core::math::Quat;
using skrtg::core::math::Subtract;
using skrtg::core::math::TransformRT;
using skrtg::core::math::Vec3;

constexpr double Pi = 3.141592653589793238462643383279502884;

int Fail(const std::string& Message)
{
    std::cerr << "FAIL: " << Message << "\n";
    return EXIT_FAILURE;
}

TwoBoneIkInput MakeDefaultInput()
{
    TwoBoneIkInput Input;
    Input.RootParentModel = IdentityTransform();
    Input.RootLocal = IdentityTransform();
    Input.MidLocal = IdentityTransform();
    Input.EndLocal = IdentityTransform();
    Input.MidLocal.TranslationCm = {10.0, 0.0, 0.0};
    Input.EndLocal.TranslationCm = {10.0, 0.0, 0.0};
    Input.TargetPositionModelCm = {12.0, 0.0, 0.0};
    Input.PolePositionModelCm = {0.0, 10.0, 0.0};
    Input.Epsilon = 1.0e-9;
    Input.PositionToleranceCm = 1.0e-6;
    Input.LengthToleranceCm = 1.0e-6;
    return Input;
}

bool SegmentLengthsPreserved(const TwoBoneIkResult& Result, double Tolerance = 1.0e-6)
{
    return Result.Success && Result.Telemetry.FirstSegmentLengthErrorCm <= Tolerance &&
           Result.Telemetry.SecondSegmentLengthErrorCm <= Tolerance;
}

bool HasExplicitClampReason(const TwoBoneIkResult& Result)
{
    return Result.Telemetry.InnerReachClamped || Result.Telemetry.OuterReachClamped ||
           Result.Telemetry.SoftReachApplied || Result.Telemetry.JointLimitClamped;
}

double QuatDot(Quat Left, Quat Right)
{
    const Quat A = skrtg::core::math::Normalize(Left);
    const Quat B = skrtg::core::math::Normalize(Right);
    return A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W;
}

int TestReachableTargetAndLocalModelRebuild()
{
    const TwoBoneIkInput Input = MakeDefaultInput();
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || Result.Status != TwoBoneSolveStatus::Solved || !Result.Telemetry.ReachedTarget)
    {
        return Fail("reachable target was not solved exactly");
    }
    if (Result.Telemetry.EndpointErrorCm > 1.0e-6 || !SegmentLengthsPreserved(Result))
    {
        return Fail("reachable target exceeded endpoint or segment-length tolerance");
    }
    if (!Result.Telemetry.LocalTranslationsPreserved || Result.Telemetry.RootPelvisGlobalCompensationApplied)
    {
        return Fail("position solve changed local translations or reported forbidden global compensation");
    }
    if (!NearlyEqual(Result.OutputRootModel.TranslationCm, Input.RootLocal.TranslationCm, 1.0e-9))
    {
        return Fail("root model position moved during rotation-only solve");
    }
    return EXIT_SUCCESS;
}

int TestOuterUnreachableNoStretch()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.TargetPositionModelCm = {30.0, 0.0, 0.0};
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || Result.Status != TwoBoneSolveStatus::SolvedClamped ||
        !Result.Telemetry.OuterReachClamped || Result.Telemetry.TargetGeometricallyReachable ||
        Result.Telemetry.ReachedTarget)
    {
        return Fail("outer unreachable target did not report the no-stretch clamp");
    }
    if (!NearlyEqual(Result.Telemetry.EffectiveSolveDistanceCm, 20.0, 1.0e-9) ||
        !NearlyEqual(Result.Telemetry.EndpointErrorCm, 10.0, 1.0e-6) ||
        !SegmentLengthsPreserved(Result))
    {
        return Fail("outer unreachable solve stretched a segment or produced the wrong endpoint");
    }
    return EXIT_SUCCESS;
}

int TestInnerUnreachableNoStretch()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.EndLocal.TranslationCm = {4.0, 0.0, 0.0};
    Input.TargetPositionModelCm = {2.0, 0.0, 0.0};
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || !Result.Telemetry.InnerReachClamped || Result.Telemetry.ReachedTarget ||
        !NearlyEqual(Result.Telemetry.MinimumReachCm, 6.0, 1.0e-9) ||
        !NearlyEqual(Result.Telemetry.EndpointErrorCm, 4.0, 1.0e-6) ||
        !SegmentLengthsPreserved(Result))
    {
        return Fail("inner unreachable target did not clamp to the rigid minimum reach");
    }
    return EXIT_SUCCESS;
}

int TestFullyExtendedTarget()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.TargetPositionModelCm = {20.0, 0.0, 0.0};
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || !Result.Telemetry.ReachedTarget ||
        !NearlyEqual(Result.Telemetry.BendDegrees, 0.0, 1.0e-7) ||
        Result.Telemetry.EndpointErrorCm > 1.0e-6 || !SegmentLengthsPreserved(Result))
    {
        return Fail("fully extended target was unstable or changed segment length");
    }
    return EXIT_SUCCESS;
}

int TestNearParallelReachableAccuracy()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    constexpr double NearParallelRadians = 1.0e-5;
    Input.TargetPositionModelCm = {
        20.0 * std::cos(NearParallelRadians),
        20.0 * std::sin(NearParallelRadians),
        0.0,
    };
    Input.PolePositionModelCm = {0.0, 0.0, 10.0};
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || Result.Status != TwoBoneSolveStatus::Solved ||
        !Result.Telemetry.TargetGeometricallyReachable || !Result.Telemetry.ReachedTarget ||
        Result.Telemetry.EndpointErrorCm > Input.PositionToleranceCm ||
        HasExplicitClampReason(Result) || !SegmentLengthsPreserved(Result))
    {
        return Fail("near-parallel reachable rotation was discarded or mislabeled as clamped");
    }
    return EXIT_SUCCESS;
}

int TestNearZeroNonDegenerateTarget()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.TargetPositionModelCm = {0.001, 0.0, 0.0};
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || !Result.Telemetry.ReachedTarget ||
        Result.Telemetry.EndpointErrorCm > 1.0e-6 || !SegmentLengthsPreserved(Result))
    {
        return Fail("near-root non-zero target did not remain finite and exact");
    }
    return EXIT_SUCCESS;
}

int TestCollinearPoleFailsClosed()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.PolePositionModelCm = {30.0, 0.0, 0.0};
    const auto Result = SolveAnalyticTwoBone(Input);
    if (Result.Success || Result.FailureReason != TwoBoneFailureReason::CollinearPole)
    {
        return Fail("collinear pole was accepted without configured fallback");
    }
    return EXIT_SUCCESS;
}

int TestConfiguredPoleFallbackIsAudited()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.PolePositionModelCm = {30.0, 0.0, 0.0};
    Input.PoleFallback = PoleFallbackPolicy::AllowConfiguredAxis;
    Input.ConfiguredFallbackAxisModel = {0.0, 0.0, 1.0};
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || !Result.Telemetry.PoleFallbackUsed ||
        Result.Telemetry.OutputMidPositionModelCm.Z <= 0.0 || !Result.Telemetry.ReachedTarget)
    {
        return Fail("explicit configured pole fallback was not used and audited deterministically");
    }

    Input.ConfiguredFallbackAxisModel = {1.0, 0.0, 0.0};
    const auto Invalid = SolveAnalyticTwoBone(Input);
    if (Invalid.Success || Invalid.FailureReason != TwoBoneFailureReason::InvalidConfiguredFallback)
    {
        return Fail("collinear configured fallback axis did not fail closed");
    }
    return EXIT_SUCCESS;
}

int TestMirroredPoleBehavior()
{
    TwoBoneIkInput Left = MakeDefaultInput();
    TwoBoneIkInput Right = Left;
    Left.PolePositionModelCm = {0.0, 10.0, 0.0};
    Right.PolePositionModelCm = {0.0, -10.0, 0.0};
    const auto LeftResult = SolveAnalyticTwoBone(Left);
    const auto RightResult = SolveAnalyticTwoBone(Right);
    if (!LeftResult.Success || !RightResult.Success ||
        !LeftResult.Telemetry.ReachedTarget || !RightResult.Telemetry.ReachedTarget)
    {
        return Fail("mirrored pole fixtures did not both solve");
    }
    const Vec3 LeftMid = LeftResult.OutputMidModel.TranslationCm;
    const Vec3 RightMid = RightResult.OutputMidModel.TranslationCm;
    if (!NearlyEqual(LeftMid.X, RightMid.X, 1.0e-6) ||
        !NearlyEqual(LeftMid.Y, -RightMid.Y, 1.0e-6) ||
        !NearlyEqual(LeftMid.Z, RightMid.Z, 1.0e-6))
    {
        return Fail("mirrored pole controls were not geometrically symmetric");
    }
    return EXIT_SUCCESS;
}

int TestDifferingRestOrientationsAndParentTransform()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.RootParentModel.TranslationCm = {3.0, -4.0, 2.0};
    Input.RootParentModel.Rotation = FromAxisAngleDegrees({0.0, 0.0, 1.0}, 35.0);
    Input.RootLocal.TranslationCm = {1.0, 2.0, 0.0};
    Input.RootLocal.Rotation = FromAxisAngleDegrees({0.0, 1.0, 0.0}, 20.0);
    Input.MidLocal.TranslationCm = {0.0, 10.0, 0.0};
    Input.MidLocal.Rotation = FromAxisAngleDegrees({1.0, 0.0, 0.0}, 30.0);
    Input.EndLocal.TranslationCm = {0.0, 0.0, 10.0};
    const TransformRT RootModel = skrtg::core::math::Compose(Input.RootParentModel, Input.RootLocal);
    Input.TargetPositionModelCm = {
        RootModel.TranslationCm.X + 8.0,
        RootModel.TranslationCm.Y + 6.0,
        RootModel.TranslationCm.Z + 5.0,
    };
    Input.PolePositionModelCm = {
        RootModel.TranslationCm.X - 3.0,
        RootModel.TranslationCm.Y + 5.0,
        RootModel.TranslationCm.Z + 15.0,
    };

    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || !Result.Telemetry.ReachedTarget ||
        Result.Telemetry.EndpointErrorCm > 1.0e-6 || !SegmentLengthsPreserved(Result))
    {
        return Fail("differing rest orientations or parent transform broke local/model reconstruction");
    }
    if (!NearlyEqual(Result.OutputRootModel.TranslationCm, RootModel.TranslationCm, 1.0e-9))
    {
        return Fail("differing-rest fixture changed the root model position");
    }
    return EXIT_SUCCESS;
}

int TestJointBendLimitClamp()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.TargetPositionModelCm = {1.0, 0.0, 0.0};
    Input.BendLimits.Enabled = true;
    Input.BendLimits.MinimumBendDegrees = 0.0;
    Input.BendLimits.MaximumBendDegrees = 90.0;
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || !Result.Telemetry.JointLimitClamped || Result.Telemetry.ReachedTarget ||
        !NearlyEqual(Result.Telemetry.BendDegrees, 90.0, 1.0e-8) ||
        !NearlyEqual(Result.Telemetry.EffectiveSolveDistanceCm, std::sqrt(200.0), 1.0e-6) ||
        !SegmentLengthsPreserved(Result))
    {
        return Fail("explicit hinge bend limit was not clamped with documented semantics");
    }
    return EXIT_SUCCESS;
}

int TestOptInBoundedSoftReach()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.TargetPositionModelCm = {19.0, 0.0, 0.0};
    Input.SoftReach.Enabled = true;
    Input.SoftReach.StartRatio = 0.8;
    Input.SoftReach.MaximumExtensionRatio = 0.95;
    const auto Result = SolveAnalyticTwoBone(Input);
    if (!Result.Success || !Result.Telemetry.SoftReachApplied || Result.Telemetry.ReachedTarget ||
        Result.Telemetry.EffectiveSolveDistanceCm <= 16.0 ||
        Result.Telemetry.EffectiveSolveDistanceCm >= 19.0 ||
        !SegmentLengthsPreserved(Result))
    {
        return Fail("opt-in soft reach did not remain bounded below its configured extension cap");
    }
    return EXIT_SUCCESS;
}

int TestOrientationPolicySeparation()
{
    TwoBoneIkInput Preserve = MakeDefaultInput();
    Preserve.EndLocal.Rotation = FromAxisAngleDegrees({1.0, 0.0, 0.0}, 37.0);
    const Quat OriginalLocal = Preserve.EndLocal.Rotation;
    const auto PreservedResult = SolveAnalyticTwoBone(Preserve);
    if (!PreservedResult.Success || PreservedResult.Telemetry.EndOrientationApplied ||
        !PreservedResult.Telemetry.EndLocalOrientationPreserved ||
        PreservedResult.OutputEndLocal.Rotation.X != OriginalLocal.X ||
        PreservedResult.OutputEndLocal.Rotation.Y != OriginalLocal.Y ||
        PreservedResult.OutputEndLocal.Rotation.Z != OriginalLocal.Z ||
        PreservedResult.OutputEndLocal.Rotation.W != OriginalLocal.W)
    {
        return Fail("disabled end-orientation policy performed a hidden local rotation write");
    }

    TwoBoneIkInput Explicit = MakeDefaultInput();
    Explicit.OrientationPolicy = EndOrientationPolicy::ApplyExplicitModel;
    Explicit.ExplicitEndModelOrientation = FromAxisAngleDegrees({0.0, 0.0, 1.0}, 45.0);
    const auto ExplicitResult = SolveAnalyticTwoBone(Explicit);
    if (!ExplicitResult.Success || !ExplicitResult.Telemetry.EndOrientationApplied ||
        !NearlyEqual(ExplicitResult.OutputEndModel.Rotation, Explicit.ExplicitEndModelOrientation, 1.0e-8))
    {
        return Fail("explicit end model orientation policy did not apply exactly");
    }
    return EXIT_SUCCESS;
}

int TestQuaternionContinuityPath()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Quat PreviousRoot;
    Quat PreviousMid;
    Quat PreviousEnd;
    bool HasPrevious = false;
    double MinimumDot = 1.0;
    double MaximumStepDegrees = 0.0;

    for (int Step = 0; Step < 41; ++Step)
    {
        const double Angle = -20.0 + static_cast<double>(Step);
        const double Radians = Angle * Pi / 180.0;
        Input.TargetPositionModelCm = {12.0 * std::cos(Radians), 12.0 * std::sin(Radians), 2.0};
        Input.PolePositionModelCm = {0.0, 0.0, 10.0};
        Input.PreviousLocalRotations.HasRootLocal = HasPrevious;
        Input.PreviousLocalRotations.HasMidLocal = HasPrevious;
        Input.PreviousLocalRotations.HasEndLocal = HasPrevious;
        Input.PreviousLocalRotations.RootLocal = PreviousRoot;
        Input.PreviousLocalRotations.MidLocal = PreviousMid;
        Input.PreviousLocalRotations.EndLocal = PreviousEnd;

        const auto Result = SolveAnalyticTwoBone(Input);
        if (!Result.Success || !Result.Telemetry.ReachedTarget || !SegmentLengthsPreserved(Result))
        {
            return Fail("continuity target path produced a failed or inexact frame");
        }
        if (HasPrevious)
        {
            const double RootDot = QuatDot(Result.OutputRootLocal.Rotation, PreviousRoot);
            const double MidDot = QuatDot(Result.OutputMidLocal.Rotation, PreviousMid);
            const double EndDot = QuatDot(Result.OutputEndLocal.Rotation, PreviousEnd);
            MinimumDot = std::min({MinimumDot, RootDot, MidDot, EndDot,
                                   Result.Telemetry.MinimumQuaternionDotPrevious});
            const double FrameDot = std::min({RootDot, MidDot, EndDot});
            MaximumStepDegrees = std::max(MaximumStepDegrees,
                                          2.0 * std::acos(std::clamp(FrameDot, -1.0, 1.0)) * 180.0 / Pi);
        }
        PreviousRoot = Result.OutputRootLocal.Rotation;
        PreviousMid = Result.OutputMidLocal.Rotation;
        PreviousEnd = Result.OutputEndLocal.Rotation;
        HasPrevious = true;
    }

    if (MinimumDot < 0.0 || MaximumStepDegrees > 5.0)
    {
        return Fail("quaternion continuity telemetry reported a sign flip or unstable step");
    }
    return EXIT_SUCCESS;
}

int TestDeterministicRepeatability()
{
    TwoBoneIkInput Input = MakeDefaultInput();
    Input.TargetPositionModelCm = {9.0, 7.0, 3.0};
    Input.PolePositionModelCm = {-2.0, 4.0, 11.0};
    const auto First = SolveAnalyticTwoBone(Input);
    const auto Second = SolveAnalyticTwoBone(Input);
    if (!First.Success || !Second.Success ||
        !NearlyEqual(First.OutputRootLocal, Second.OutputRootLocal, 0.0, 0.0, 0.0) ||
        !NearlyEqual(First.OutputMidLocal, Second.OutputMidLocal, 0.0, 0.0, 0.0) ||
        !NearlyEqual(First.OutputEndLocal, Second.OutputEndLocal, 0.0, 0.0, 0.0) ||
        First.Telemetry.EndpointErrorCm != Second.Telemetry.EndpointErrorCm ||
        First.Telemetry.BendDegrees != Second.Telemetry.BendDegrees)
    {
        return Fail("identical solver inputs did not produce bit-stable observable results");
    }
    return EXIT_SUCCESS;
}

int TestNonFiniteAndDegenerateFailClose()
{
    TwoBoneIkInput NonFinite = MakeDefaultInput();
    NonFinite.TargetPositionModelCm.X = std::numeric_limits<double>::infinity();
    const auto NonFiniteResult = SolveAnalyticTwoBone(NonFinite);
    if (NonFiniteResult.Success || NonFiniteResult.FailureReason != TwoBoneFailureReason::NonFiniteInput)
    {
        return Fail("non-finite target did not fail closed");
    }

    TwoBoneIkInput Degenerate = MakeDefaultInput();
    Degenerate.MidLocal.TranslationCm = {0.0, 0.0, 0.0};
    const auto DegenerateResult = SolveAnalyticTwoBone(Degenerate);
    if (DegenerateResult.Success || DegenerateResult.FailureReason != TwoBoneFailureReason::DegenerateSegment)
    {
        return Fail("zero-length segment did not fail closed");
    }

    TwoBoneIkInput CoincidentTarget = MakeDefaultInput();
    CoincidentTarget.TargetPositionModelCm = {0.0, 0.0, 0.0};
    const auto CoincidentResult = SolveAnalyticTwoBone(CoincidentTarget);
    if (CoincidentResult.Success ||
        CoincidentResult.FailureReason != TwoBoneFailureReason::DegenerateTargetDirection)
    {
        return Fail("root-coincident target did not fail closed");
    }
    return EXIT_SUCCESS;
}

int TestTopologyScaleAndPolicyValidation()
{
    TwoBoneIkInput Topology = MakeDefaultInput();
    Topology.Chain.EndParentIndex = Topology.Chain.RootIndex;
    const auto TopologyResult = SolveAnalyticTwoBone(Topology);
    if (TopologyResult.Success || TopologyResult.FailureReason != TwoBoneFailureReason::InvalidTopology)
    {
        return Fail("mixed topology did not fail closed");
    }

    TwoBoneIkInput Scale = MakeDefaultInput();
    Scale.RootLocal.Scale = {2.0, 1.0, 1.0};
    Scale.LengthToleranceCm = 100.0;
    const auto ScaleResult = SolveAnalyticTwoBone(Scale);
    if (ScaleResult.Success || ScaleResult.FailureReason != TwoBoneFailureReason::UnsupportedScale)
    {
        return Fail("large length-error tolerance masked the fixed unit-scale precondition");
    }

    TwoBoneIkInput Limits = MakeDefaultInput();
    Limits.BendLimits.Enabled = true;
    Limits.BendLimits.MinimumBendDegrees = 120.0;
    Limits.BendLimits.MaximumBendDegrees = 90.0;
    const auto LimitsResult = SolveAnalyticTwoBone(Limits);
    if (LimitsResult.Success || LimitsResult.FailureReason != TwoBoneFailureReason::InvalidJointLimits)
    {
        return Fail("invalid bend-limit interval did not fail closed");
    }

    TwoBoneIkInput Soft = MakeDefaultInput();
    Soft.SoftReach.Enabled = true;
    Soft.SoftReach.StartRatio = 0.99;
    Soft.SoftReach.MaximumExtensionRatio = 0.95;
    const auto SoftResult = SolveAnalyticTwoBone(Soft);
    if (SoftResult.Success || SoftResult.FailureReason != TwoBoneFailureReason::InvalidSoftReach)
    {
        return Fail("invalid soft-reach interval did not fail closed");
    }
    return EXIT_SUCCESS;
}


int TestSolvedClampedRequiresExplicitReason()
{
    TwoBoneIkInput Outer = MakeDefaultInput();
    Outer.TargetPositionModelCm = {30.0, 0.0, 0.0};

    TwoBoneIkInput Inner = MakeDefaultInput();
    Inner.EndLocal.TranslationCm = {4.0, 0.0, 0.0};
    Inner.TargetPositionModelCm = {2.0, 0.0, 0.0};

    TwoBoneIkInput Soft = MakeDefaultInput();
    Soft.TargetPositionModelCm = {19.0, 0.0, 0.0};
    Soft.SoftReach.Enabled = true;
    Soft.SoftReach.StartRatio = 0.8;
    Soft.SoftReach.MaximumExtensionRatio = 0.95;

    TwoBoneIkInput Limited = MakeDefaultInput();
    Limited.TargetPositionModelCm = {1.0, 0.0, 0.0};
    Limited.BendLimits.Enabled = true;
    Limited.BendLimits.MinimumBendDegrees = 0.0;
    Limited.BendLimits.MaximumBendDegrees = 90.0;

    const TwoBoneIkResult ClampedResults[] = {
        SolveAnalyticTwoBone(Outer),
        SolveAnalyticTwoBone(Inner),
        SolveAnalyticTwoBone(Soft),
        SolveAnalyticTwoBone(Limited),
    };
    for (const TwoBoneIkResult& Result : ClampedResults)
    {
        if (!Result.Success || Result.Status != TwoBoneSolveStatus::SolvedClamped ||
            !HasExplicitClampReason(Result))
        {
            return Fail("successful SolvedClamped result lacked an explicit reach/soft/limit reason");
        }
    }

    TwoBoneIkInput UnconstrainedMiss = MakeDefaultInput();
    UnconstrainedMiss.TargetPositionModelCm = {9.0, 7.0, 3.0};
    UnconstrainedMiss.PolePositionModelCm = {-2.0, 4.0, 11.0};
    UnconstrainedMiss.PositionToleranceCm = 1.0e-30;
    const auto MissResult = SolveAnalyticTwoBone(UnconstrainedMiss);
    if (MissResult.Success || MissResult.Status != TwoBoneSolveStatus::Failed ||
        MissResult.FailureReason != TwoBoneFailureReason::PositionPostconditionFailed ||
        HasExplicitClampReason(MissResult))
    {
        return Fail("unconstrained numerical miss returned success or a reasonless SolvedClamped status");
    }
    return EXIT_SUCCESS;
}
} // namespace

int main()
{
    const int Results[] = {
        TestReachableTargetAndLocalModelRebuild(),
        TestOuterUnreachableNoStretch(),
        TestInnerUnreachableNoStretch(),
        TestFullyExtendedTarget(),
        TestNearParallelReachableAccuracy(),
        TestNearZeroNonDegenerateTarget(),
        TestCollinearPoleFailsClosed(),
        TestConfiguredPoleFallbackIsAudited(),
        TestMirroredPoleBehavior(),
        TestDifferingRestOrientationsAndParentTransform(),
        TestJointBendLimitClamp(),
        TestOptInBoundedSoftReach(),
        TestOrientationPolicySeparation(),
        TestQuaternionContinuityPath(),
        TestDeterministicRepeatability(),
        TestNonFiniteAndDegenerateFailClose(),
        TestTopologyScaleAndPolicyValidation(),
        TestSolvedClampedRequiresExplicitReason(),
    };

    for (const int Result : Results)
    {
        if (Result != EXIT_SUCCESS)
        {
            return Result;
        }
    }

    std::cout << "two_bone_ik_tests passed: 18 contract groups\n";
    return EXIT_SUCCESS;
}
