#pragma once

#include "skrtg/core/ik/two_bone_ik.h"
#include "skrtg/retarget/op_stack.h"

#include <array>
#include <string>
#include <vector>

namespace skrtg::retarget::ops
{
enum class SourceMotionFootLockFailureReason
{
    None,
    InvalidOptions,
    InvalidSkeleton,
    InvalidInput,
    ScaleOutsideShadowEligibility,
    PoleConstructionFailed,
    ShadowSolverFailed,
    RealProjectionFailed,
    PostvalidationFailed
};

struct SourceMotionFootLockBinding
{
    std::string Label;
    std::array<int, 3> SourceIndices{{-1, -1, -1}};
    std::array<int, 3> TargetIndices{{-1, -1, -1}};
};

struct SourceMotionFootLockOptions
{
    std::string RouteId =
        "source_motion_foot_lock_no_ground_semantics_v1";
    std::array<SourceMotionFootLockBinding, 2> Feet;

    // Source motion is only a numerical gate. When it moves, accumulated
    // frozen-Foundation foot motion is released; when it does not, the prior
    // target Goal is held. No height, ground, collision, contact label, or
    // terrain query participates in the route.
    double PositionNoMotionToleranceCm = 1.0e-3;
    double RotationNoMotionToleranceDegrees = 1.0e-3;

    double ScaleNoiseEligibilityTolerance = 1.0e-5;
    double SolverEpsilon = 1.0e-9;
    double SolverPositionToleranceCm = 1.0e-6;
    double SolverLengthToleranceCm = 1.0e-6;
    double ShadowToRealPositionToleranceCm = 1.0e-3;
    // Quaternion roundoff guard only; still 100x tighter than the
    // diagnostic FBX rotation roundtrip contract (1.0e-3 degrees).
    double ShadowToRealRotationToleranceDegrees = 1.0e-5;
    double ReachResidualToleranceCm = 1.0e-3;
    double SegmentLengthToleranceCm = 1.0e-6;

    bool AllowConfiguredPoleFallback = true;
    core::math::Vec3 ConfiguredPoleFallbackAxisModel{0.0, 0.0, 1.0};
};

struct SourceMotionFootLockChainFrameRecord
{
    std::string Label;
    bool PositionDeltaSuppressedAsNoMotion = false;
    bool RotationDeltaSuppressedAsNoMotion = false;
    bool SolverExecuted = false;
    bool SolverCommitted = false;
    bool SolverClamped = false;
    bool PoleFallbackUsed = false;
    bool RootPelvisCompensationApplied = false;
    core::math::Vec3 SourcePositionDeltaModelCm;
    double SourcePositionDeltaCm = 0.0;
    double SourceRotationDeltaDegrees = 0.0;
    double ReleasedFoundationRotationDegrees = 0.0;
    core::math::Vec3 DesiredTargetPositionModelCm;
    core::math::Quat DesiredTargetOrientationModel;
    core::math::Vec3 PolePositionModelCm;
    double TargetPositionDeltaCm = 0.0;
    double TargetDeltaErrorCm = 0.0;
    double NoMotionTargetDriftCm = 0.0;
    double RealEndpointGoalErrorCm = 0.0;
    double ShadowEndpointGoalErrorCm = 0.0;
    double ReachResidualDeltaCm = 0.0;
    double RealEndOrientationErrorDegrees = 0.0;
    double MaximumShadowToRealPositionDeltaCm = 0.0;
    double MaximumSegmentLengthErrorCm = 0.0;
};

struct SourceMotionFootLockFrameRecord
{
    int FrameIndex = 0;
    double TimeSeconds = 0.0;
    bool SeedFrameExactPassthrough = false;
    bool TransactionCommitted = false;
    bool FailureLeftInputUnchanged = false;
    bool OnlyDeclaredLocalRotationsWritten = false;
    bool AllLocalTranslationsBitwisePreserved = false;
    bool AllLocalScalesBitwisePreserved = false;
    bool TargetHipsBitwisePreserved = false;
    SourceMotionFootLockFailureReason FailureReason =
        SourceMotionFootLockFailureReason::None;
    std::string Message;
    std::array<SourceMotionFootLockChainFrameRecord, 2> Feet;
};

struct SourceMotionFootLockCoverage
{
    int InputFrames = 0;
    int SeedPassthroughFrames = 0;
    int CommittedFrames = 0;
    int RolledBackFrames = 0;
    int SolverExecutions = 0;
    int SolverCommits = 0;
    int ClampedSolves = 0;
    int PositionNoMotionDeltas = 0;
    int PositionMotionDeltas = 0;
    int RotationNoMotionDeltas = 0;
    int RotationMotionDeltas = 0;
    int RotationGateReleases = 0;
    int PoleFallbacks = 0;
    double MaximumSourcePositionDeltaCm = 0.0;
    double MaximumSourceRotationDeltaDegrees = 0.0;
    double MaximumReleasedFoundationRotationDegrees = 0.0;
    double MaximumNoMotionTargetDriftCm = 0.0;
    double MaximumTargetDeltaErrorCm = 0.0;
    double MaximumRealEndpointGoalErrorCm = 0.0;
    double MaximumReachResidualDeltaCm = 0.0;
    double MaximumRealEndOrientationErrorDegrees = 0.0;
    double MaximumShadowToRealPositionDeltaCm = 0.0;
    double MaximumSegmentLengthErrorCm = 0.0;
    double MaximumEligibleScaleDeviation = 0.0;
};

struct SourceMotionFootLockRunResult
{
    bool Success = false;
    bool InputImmutable = false;
    bool DeterministicRepeatabilityVerified = false;
    bool NoGroundOrContactSemanticsUsed = false;
    bool AllTransactionsBounded = false;
    bool FoundationSeedFramesExact = false;
    std::string RouteId;
    RetargetOpClip Output;
    SourceMotionFootLockCoverage Coverage;
    std::vector<SourceMotionFootLockFrameRecord> Frames;
    std::vector<std::string> Errors;
};

SourceMotionFootLockRunResult RunSourceMotionFootLock(
    const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
    const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
    const RetargetOpClip& Input,
    const SourceMotionFootLockOptions& Options);

bool EquivalentSourceMotionFootLockRuns(
    const SourceMotionFootLockRunResult& Left,
    const SourceMotionFootLockRunResult& Right);

class SourceMotionFootLockOp final : public IRetargetOp
{
public:
    explicit SourceMotionFootLockOp(SourceMotionFootLockOptions Options);

    RetargetOpDescriptor Descriptor() const override;
    RetargetOpRunResult Run(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& Input) override;

    const SourceMotionFootLockRunResult& LastRun() const;

private:
    SourceMotionFootLockOptions OpOptions;
    SourceMotionFootLockRunResult Last;
};

const char* ToString(SourceMotionFootLockFailureReason Reason);
} // namespace skrtg::retarget::ops
