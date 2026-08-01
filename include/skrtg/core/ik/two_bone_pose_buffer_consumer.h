#pragma once

#include "skrtg/core/animation/pose.h"
#include "skrtg/core/ik/two_bone_ik.h"
#include "skrtg/core/skeleton/runtime_skeleton.h"

#include <cstddef>
#include <string>

namespace skrtg::core::ik
{
enum class TwoBonePoseBufferStatus
{
    Failed,
    Applied,
    AppliedClamped
};

enum class TwoBonePoseBufferFailureReason
{
    None,
    InvalidSkeletonTopology,
    InvalidPoseSpace,
    PoseSkeletonMismatch,
    InvalidChainIndices,
    InvalidChainTopology,
    InvalidPoseTransform,
    SolverFailed,
    ModelRebuildFailed,
    SolverModelMismatch,
    MutationScopeViolation
};

struct TwoBonePoseBufferRequest
{
    TwoBoneChainTopology Chain;

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

struct TwoBonePoseBufferTelemetry
{
    bool ChainIdentityValidated = false;
    bool PoseWriteCommitted = false;
    bool FailureLeftInputPoseUnchanged = false;
    bool ModelPoseValid = false;
    bool ParentLocalModelConsistent = false;
    bool LocalTranslationsPreserved = false;
    bool LocalScalesPreserved = false;
    bool UnrelatedLocalTransformsPreserved = false;
    bool OnlyChainLocalRotationsWritten = false;
    bool EndLocalOrientationPreserved = false;
    bool RootPelvisGlobalCompensationApplied = false;

    std::size_t ModifiedLocalRotationCount = 0;
    double EndpointErrorCm = 0.0;
    double FirstSegmentLengthErrorCm = 0.0;
    double SecondSegmentLengthErrorCm = 0.0;
    double SolverModelMaximumTranslationErrorCm = 0.0;
    double SolverModelMaximumRotationError = 0.0;
    double SolverModelMaximumScaleError = 0.0;
    double UnrelatedLocalMaximumComponentDelta = 0.0;
};

struct TwoBonePoseBufferResult
{
    bool Success = false;
    TwoBonePoseBufferStatus Status = TwoBonePoseBufferStatus::Failed;
    TwoBonePoseBufferFailureReason FailureReason = TwoBonePoseBufferFailureReason::None;
    std::string Message;

    TwoBoneIkResult SolverResult;
    animation::PoseBuffer OutputModelPose;
    TwoBonePoseBufferTelemetry Telemetry;
};

TwoBonePoseBufferResult ApplyAnalyticTwoBoneToPoseBuffer(
    const skeleton::NormalizedRuntimeSkeleton& Skeleton,
    const TwoBonePoseBufferRequest& Request,
    animation::PoseBuffer& InOutLocalPose);

const char* ToString(TwoBonePoseBufferStatus Status);
const char* ToString(TwoBonePoseBufferFailureReason Reason);
} // namespace skrtg::core::ik
