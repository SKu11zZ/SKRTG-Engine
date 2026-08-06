#pragma once

#include "skrtg/retarget/op_stack.h"

#include <string>
#include <vector>

namespace skrtg::retarget::ops
{
struct ContactFootPlantBinding
{
    std::string Label;
    int SourceContactBoneIndex = -1;
    core::math::Vec3 SourceContactPointLocalCm;
    std::string GoalName;
    double TranslationAlpha = 1.0;
    double RotationAlpha = 1.0;
};

struct ContactFootPlantOptions
{
    std::string RouteId = "contact_foot_plant_v2";
    core::math::Vec3 GroundPlaneNormalModel{0.0, 0.0, 1.0};
    double GroundPlaneDistanceCm = 0.0;
    double EnterSpeedCmPerSecond = 15.0;
    double ExitSpeedCmPerSecond = 30.0;
    double EnterHeightCm = 4.0;
    double ExitHeightCm = 8.0;
    int EnterConfirmationFrames = 2;
    int MinimumPlantFrames = 2;
    int ReleaseBlendFrames = 6;
    double MaximumAnchorDriftCm = 100.0;
    std::vector<ContactFootPlantBinding> Feet;
};

struct ContactFootPlantTelemetry
{
    int InputFrames = 0;
    int ContactCandidates = 0;
    int PlantTransitions = 0;
    int PlantedFrames = 0;
    int ReleaseTransitions = 0;
    int ReleaseFrames = 0;
    int DriftGuardReleases = 0;
    double MaximumObservedSpeedCmPerSecond = 0.0;
    double MaximumHeldGoalDeltaCm = 0.0;
};

class ContactFootPlantOp final : public IRetargetOp
{
public:
    explicit ContactFootPlantOp(ContactFootPlantOptions Options);

    RetargetOpDescriptor Descriptor() const override;
    RetargetOpPreflightResult Preflight(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& Input) const override;
    RetargetOpRunResult Run(
        const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
        const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton,
        const RetargetOpClip& Input) override;

    const ContactFootPlantTelemetry& LastTelemetry() const;

private:
    ContactFootPlantOptions OpOptions;
    ContactFootPlantTelemetry Telemetry;
};
} // namespace skrtg::retarget::ops
