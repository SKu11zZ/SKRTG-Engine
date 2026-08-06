#include "skrtg/retarget/ops/contact_foot_plant_op.h"

#include "skrtg/core/math/transform.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace skrtg::retarget::ops
{
namespace
{
using core::math::Add;
using core::math::Dot;
using core::math::Length;
using core::math::MultiplyComponents;
using core::math::Normalize;
using core::math::Quat;
using core::math::RotateVector;
using core::math::Scale;
using core::math::Subtract;
using core::math::TransformRT;
using core::math::Vec3;
using core::skeleton::NormalizedRuntimeSkeleton;

bool Finite(const double Value) { return std::isfinite(Value); }
bool Finite(const Vec3 Value)
{
    return Finite(Value.X) && Finite(Value.Y) && Finite(Value.Z);
}

bool NormalizeVector(const Vec3 Value, Vec3& Out)
{
    const double Magnitude = Length(Value);
    if (!Finite(Magnitude) || Magnitude <= 1.0e-9) return false;
    Out = Scale(Value, 1.0 / Magnitude);
    return true;
}

Vec3 Lerp(const Vec3 A, const Vec3 B, const double Alpha)
{
    return Add(Scale(A, 1.0 - Alpha), Scale(B, Alpha));
}

Quat Nlerp(Quat A, Quat B, const double Alpha)
{
    const double Product = A.X * B.X + A.Y * B.Y +
        A.Z * B.Z + A.W * B.W;
    if (Product < 0.0)
        B = {-B.X, -B.Y, -B.Z, -B.W};
    return Normalize({
        A.X + (B.X - A.X) * Alpha,
        A.Y + (B.Y - A.Y) * Alpha,
        A.Z + (B.Z - A.Z) * Alpha,
        A.W + (B.W - A.W) * Alpha});
}

double SmoothStep(const double Value)
{
    const double T = std::clamp(Value, 0.0, 1.0);
    return T * T * (3.0 - 2.0 * T);
}

TransformRT BlendTransform(const TransformRT& A,
                           const TransformRT& B,
                           const double Alpha)
{
    TransformRT Result = A;
    Result.TranslationCm = Lerp(A.TranslationCm, B.TranslationCm, Alpha);
    Result.Rotation = Nlerp(A.Rotation, B.Rotation, Alpha);
    return Result;
}

Vec3 ContactPoint(const ContactFootPlantBinding& Binding,
                  const RetargetOpFrame& Frame)
{
    const TransformRT& Bone = Frame.SourceModelPose[
        static_cast<std::size_t>(Binding.SourceContactBoneIndex)];
    return Add(
        Bone.TranslationCm,
        RotateVector(
            Bone.Rotation,
            MultiplyComponents(
                Binding.SourceContactPointLocalCm, Bone.Scale)));
}

void ApplyHeldTransform(RetargetOpGoal& Goal,
                        const TransformRT& Desired,
                        const ContactFootPlantBinding& Binding)
{
    Goal.TransformModel.TranslationCm = Lerp(
        Goal.TransformModel.TranslationCm,
        Desired.TranslationCm,
        Binding.TranslationAlpha);
    Goal.TransformModel.Rotation = Nlerp(
        Goal.TransformModel.Rotation,
        Desired.Rotation,
        Binding.RotationAlpha);
}
} // namespace

ContactFootPlantOp::ContactFootPlantOp(ContactFootPlantOptions Options)
    : OpOptions(std::move(Options))
{
}

RetargetOpDescriptor ContactFootPlantOp::Descriptor() const
{
    RetargetOpDescriptor Result;
    Result.TypeId = OpOptions.RouteId;
    Result.Version = 2;
    Result.DisplayName = "Contact Foot Plant v2";
    Result.Phase = RetargetOpPhase::TemporalGoalConstraint;
    for (const ContactFootPlantBinding& Foot : OpOptions.Feet)
    {
        RetargetOpGoalWriteMask Mask;
        Mask.GoalName = Foot.GoalName;
        Mask.Translation = Foot.TranslationAlpha > 0.0;
        Mask.Rotation = Foot.RotationAlpha > 0.0;
        Result.DeclaredGoalWrites.push_back(std::move(Mask));
    }
    return Result;
}

RetargetOpPreflightResult ContactFootPlantOp::Preflight(
    const NormalizedRuntimeSkeleton& Source,
    const NormalizedRuntimeSkeleton&,
    const RetargetOpClip& Input) const
{
    RetargetOpPreflightResult Result;
    Vec3 GroundNormal;
    if (OpOptions.RouteId.empty() || OpOptions.Feet.empty() ||
        !Finite(OpOptions.GroundPlaneNormalModel) ||
        !NormalizeVector(OpOptions.GroundPlaneNormalModel, GroundNormal) ||
        !Finite(OpOptions.GroundPlaneDistanceCm) ||
        !Finite(OpOptions.EnterSpeedCmPerSecond) ||
        !Finite(OpOptions.ExitSpeedCmPerSecond) ||
        !Finite(OpOptions.EnterHeightCm) ||
        !Finite(OpOptions.ExitHeightCm) ||
        !Finite(OpOptions.MaximumAnchorDriftCm) ||
        OpOptions.EnterSpeedCmPerSecond < 0.0 ||
        OpOptions.ExitSpeedCmPerSecond <
            OpOptions.EnterSpeedCmPerSecond ||
        OpOptions.ExitHeightCm < OpOptions.EnterHeightCm ||
        OpOptions.EnterConfirmationFrames < 1 ||
        OpOptions.MinimumPlantFrames < 1 ||
        OpOptions.ReleaseBlendFrames < 1 ||
        OpOptions.MaximumAnchorDriftCm <= 0.0)
    {
        Result.Available = false;
        Result.Errors.push_back("contact foot plant options are invalid");
        return Result;
    }
    std::set<std::string> Labels;
    std::set<std::string> Goals;
    for (const ContactFootPlantBinding& Foot : OpOptions.Feet)
    {
        if (Foot.Label.empty() || Foot.GoalName.empty() ||
            !Labels.insert(Foot.Label).second ||
            !Goals.insert(Foot.GoalName).second ||
            Foot.SourceContactBoneIndex < 0 ||
            Foot.SourceContactBoneIndex >=
                static_cast<int>(Source.BoneCount()) ||
            !Finite(Foot.SourceContactPointLocalCm) ||
            !Finite(Foot.TranslationAlpha) ||
            Foot.TranslationAlpha < 0.0 ||
            Foot.TranslationAlpha > 1.0 ||
            !Finite(Foot.RotationAlpha) ||
            Foot.RotationAlpha < 0.0 || Foot.RotationAlpha > 1.0 ||
            (Foot.TranslationAlpha == 0.0 &&
             Foot.RotationAlpha == 0.0))
        {
            Result.Available = false;
            Result.Errors.push_back(
                "contact foot binding is invalid or duplicated: " +
                Foot.Label);
        }
    }
    for (const RetargetOpFrame& Frame : Input.Frames)
    {
        for (const ContactFootPlantBinding& Foot : OpOptions.Feet)
        {
            if (FindRetargetOpGoal(Frame, Foot.GoalName) == nullptr)
            {
                Result.Available = false;
                Result.Errors.push_back(
                    "foot plant goal is missing: " + Foot.GoalName);
                return Result;
            }
        }
    }
    return Result;
}

RetargetOpRunResult ContactFootPlantOp::Run(
    const NormalizedRuntimeSkeleton&,
    const NormalizedRuntimeSkeleton&,
    const RetargetOpClip& Input)
{
    RetargetOpRunResult Result;
    Result.InputImmutable = true;
    Result.RouteId = OpOptions.RouteId;
    RetargetOpClip Candidate = Input;
    Telemetry = {};
    Telemetry.InputFrames = static_cast<int>(Input.Frames.size());
    Vec3 GroundNormal;
    NormalizeVector(OpOptions.GroundPlaneNormalModel, GroundNormal);

    for (const ContactFootPlantBinding& Foot : OpOptions.Feet)
    {
        bool HasPreviousContact = false;
        Vec3 PreviousContact;
        double PreviousTime = 0.0;
        int CandidateFrames = 0;
        TransformRT CandidateAnchor;
        bool Planted = false;
        int PlantedFrames = 0;
        TransformRT HeldAnchor;
        bool Releasing = false;
        int ReleaseFrame = 0;
        TransformRT ReleaseStart;

        for (RetargetOpFrame& Frame : Candidate.Frames)
        {
            RetargetOpGoal* Goal =
                FindRetargetOpGoal(Frame, Foot.GoalName);
            if (Goal == nullptr)
            {
                Result.Errors.push_back(
                    "foot plant goal disappeared: " + Foot.GoalName);
                Result.Output = Input;
                Result.FailureLeftInputUnchanged = true;
                return Result;
            }
            const TransformRT AnimatedGoal = Goal->TransformModel;
            const Vec3 CurrentContact = ContactPoint(Foot, Frame);
            const double Height = Dot(GroundNormal, CurrentContact) -
                OpOptions.GroundPlaneDistanceCm;
            double Speed = std::numeric_limits<double>::infinity();
            if (HasPreviousContact)
            {
                const double DeltaTime = Frame.TimeSeconds - PreviousTime;
                if (!Finite(DeltaTime) || DeltaTime <= 0.0)
                {
                    Result.Errors.push_back(
                        "foot plant frame time is not strictly increasing");
                    Result.Output = Input;
                    Result.FailureLeftInputUnchanged = true;
                    return Result;
                }
                Speed = Length(Subtract(
                    CurrentContact, PreviousContact)) / DeltaTime;
                Telemetry.MaximumObservedSpeedCmPerSecond = std::max(
                    Telemetry.MaximumObservedSpeedCmPerSecond, Speed);
            }
            const bool EnterContact = HasPreviousContact &&
                Speed <= OpOptions.EnterSpeedCmPerSecond &&
                Height <= OpOptions.EnterHeightCm;
            const bool ExitContact = HasPreviousContact &&
                (Speed >= OpOptions.ExitSpeedCmPerSecond ||
                 Height >= OpOptions.ExitHeightCm);

            if (Planted)
            {
                const double Drift = Length(Subtract(
                    AnimatedGoal.TranslationCm,
                    HeldAnchor.TranslationCm));
                Telemetry.MaximumHeldGoalDeltaCm = std::max(
                    Telemetry.MaximumHeldGoalDeltaCm, Drift);
                const bool DriftRelease =
                    Drift > OpOptions.MaximumAnchorDriftCm;
                if ((ExitContact || DriftRelease) &&
                    PlantedFrames >= OpOptions.MinimumPlantFrames)
                {
                    Planted = false;
                    Releasing = true;
                    ReleaseFrame = 0;
                    ReleaseStart = HeldAnchor;
                    ++Telemetry.ReleaseTransitions;
                    if (DriftRelease) ++Telemetry.DriftGuardReleases;
                }
                else
                {
                    ApplyHeldTransform(*Goal, HeldAnchor, Foot);
                    ++PlantedFrames;
                    ++Telemetry.PlantedFrames;
                }
            }

            if (Releasing)
            {
                ++ReleaseFrame;
                const double ReleaseAlpha = SmoothStep(
                    static_cast<double>(ReleaseFrame) /
                    static_cast<double>(OpOptions.ReleaseBlendFrames));
                const TransformRT Desired = BlendTransform(
                    ReleaseStart, AnimatedGoal, ReleaseAlpha);
                ApplyHeldTransform(*Goal, Desired, Foot);
                ++Telemetry.ReleaseFrames;
                if (ReleaseFrame >= OpOptions.ReleaseBlendFrames)
                {
                    Releasing = false;
                    CandidateFrames = 0;
                }
            }

            if (!Planted && !Releasing)
            {
                if (EnterContact)
                {
                    ++Telemetry.ContactCandidates;
                    if (CandidateFrames == 0)
                        CandidateAnchor = AnimatedGoal;
                    ++CandidateFrames;
                    if (CandidateFrames >=
                        OpOptions.EnterConfirmationFrames)
                    {
                        Planted = true;
                        PlantedFrames = 1;
                        HeldAnchor = CandidateAnchor;
                        CandidateFrames = 0;
                        ApplyHeldTransform(*Goal, HeldAnchor, Foot);
                        ++Telemetry.PlantTransitions;
                        ++Telemetry.PlantedFrames;
                    }
                }
                else
                {
                    CandidateFrames = 0;
                }
            }

            PreviousContact = CurrentContact;
            PreviousTime = Frame.TimeSeconds;
            HasPreviousContact = true;
        }
    }
    Result.Success = true;
    Result.OutputModelsRebuilt = true;
    Result.MutationWithinDeclaredChannels = true;
    Result.Output = std::move(Candidate);
    return Result;
}

const ContactFootPlantTelemetry& ContactFootPlantOp::LastTelemetry() const
{
    return Telemetry;
}
} // namespace skrtg::retarget::ops
