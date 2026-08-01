#pragma once

#include "skrtg/core/math/transform.h"

#include <string>

namespace skrtg::core::ik
{
enum class PoleFallbackPolicy
{
    FailClosed,
    AllowConfiguredAxis
};

enum class EndOrientationPolicy
{
    PreserveInputLocal,
    ApplyExplicitModel
};

enum class TwoBoneSolveStatus
{
    Failed,
    Solved,
    SolvedClamped
};

enum class TwoBoneFailureReason
{
    None,
    InvalidTopology,
    NonFiniteInput,
    InvalidQuaternion,
    UnsupportedScale,
    DegenerateSegment,
    DegenerateTargetDirection,
    CollinearPole,
    InvalidConfiguredFallback,
    InvalidJointLimits,
    InvalidSoftReach,
    InvalidTolerance,
    RotationRebuildFailed,
    PositionPostconditionFailed,
    NumericalFailure
};

struct TwoBoneChainTopology
{
    int RootIndex = 0;
    int MidIndex = 1;
    int EndIndex = 2;
    int MidParentIndex = 0;
    int EndParentIndex = 1;
};

struct JointBendLimits
{
    bool Enabled = false;
    // Bend convention: 0 degrees is straight, 180 degrees is fully folded.
    double MinimumBendDegrees = 0.0;
    double MaximumBendDegrees = 180.0;
};

struct SoftReachPolicy
{
    bool Enabled = false;
    // Softening starts at MaxReach * StartRatio and asymptotically approaches
    // MaxReach * MaximumExtensionRatio.
    double StartRatio = 0.85;
    double MaximumExtensionRatio = 0.98;
};

struct QuaternionContinuityReference
{
    bool HasRootLocal = false;
    bool HasMidLocal = false;
    bool HasEndLocal = false;
    math::Quat RootLocal;
    math::Quat MidLocal;
    math::Quat EndLocal;
};

struct TwoBoneIkInput
{
    TwoBoneChainTopology Chain;

    math::TransformRT RootParentModel;
    math::TransformRT RootLocal;
    math::TransformRT MidLocal;
    math::TransformRT EndLocal;

    math::Vec3 TargetPositionModelCm;
    math::Vec3 PolePositionModelCm;
    PoleFallbackPolicy PoleFallback = PoleFallbackPolicy::FailClosed;
    math::Vec3 ConfiguredFallbackAxisModel{0.0, 0.0, 1.0};

    JointBendLimits BendLimits;
    SoftReachPolicy SoftReach;

    EndOrientationPolicy OrientationPolicy = EndOrientationPolicy::PreserveInputLocal;
    math::Quat ExplicitEndModelOrientation;

    QuaternionContinuityReference PreviousLocalRotations;

    double Epsilon = 1.0e-9;
    double PositionToleranceCm = 1.0e-6;
    double LengthToleranceCm = 1.0e-6;
};

struct TwoBoneIkTelemetry
{
    bool TargetGeometricallyReachable = false;
    bool ReachedTarget = false;
    bool InnerReachClamped = false;
    bool OuterReachClamped = false;
    bool SoftReachApplied = false;
    bool JointLimitClamped = false;
    bool PoleFallbackUsed = false;
    bool EndOrientationApplied = false;
    bool EndLocalOrientationPreserved = false;
    bool LocalTranslationsPreserved = false;
    bool RootPelvisGlobalCompensationApplied = false;
    bool ContinuityReferenceUsed = false;
    int QuaternionSignCorrections = 0;

    double FirstSegmentLengthCm = 0.0;
    double SecondSegmentLengthCm = 0.0;
    double OutputFirstSegmentLengthCm = 0.0;
    double OutputSecondSegmentLengthCm = 0.0;
    double FirstSegmentLengthErrorCm = 0.0;
    double SecondSegmentLengthErrorCm = 0.0;
    double MinimumReachCm = 0.0;
    double MaximumReachCm = 0.0;
    double TargetDistanceCm = 0.0;
    double EffectiveSolveDistanceCm = 0.0;
    double BendDegrees = 0.0;
    double EndpointErrorCm = 0.0;

    double RootQuaternionDotPrevious = 1.0;
    double MidQuaternionDotPrevious = 1.0;
    double EndQuaternionDotPrevious = 1.0;
    double MinimumQuaternionDotPrevious = 1.0;

    math::Vec3 InputRootPositionModelCm;
    math::Vec3 OutputRootPositionModelCm;
    math::Vec3 OutputMidPositionModelCm;
    math::Vec3 OutputEndPositionModelCm;
};

struct TwoBoneIkResult
{
    bool Success = false;
    TwoBoneSolveStatus Status = TwoBoneSolveStatus::Failed;
    TwoBoneFailureReason FailureReason = TwoBoneFailureReason::None;
    std::string Message;

    math::TransformRT OutputRootLocal;
    math::TransformRT OutputMidLocal;
    math::TransformRT OutputEndLocal;
    math::TransformRT OutputRootModel;
    math::TransformRT OutputMidModel;
    math::TransformRT OutputEndModel;

    TwoBoneIkTelemetry Telemetry;
};

TwoBoneIkResult SolveAnalyticTwoBone(const TwoBoneIkInput& Input);

const char* ToString(PoleFallbackPolicy Policy);
const char* ToString(EndOrientationPolicy Policy);
const char* ToString(TwoBoneSolveStatus Status);
const char* ToString(TwoBoneFailureReason Reason);
} // namespace skrtg::core::ik
