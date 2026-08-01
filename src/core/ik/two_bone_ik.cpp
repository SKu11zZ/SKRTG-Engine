#include "skrtg/core/ik/two_bone_ik.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace skrtg::core::ik
{
namespace
{
constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double UnitScaleTolerance = 1.0e-9;

double Clamp(double Value, double Minimum, double Maximum)
{
    return std::max(Minimum, std::min(Value, Maximum));
}

double RadiansToDegrees(double Radians)
{
    return Radians * 180.0 / Pi;
}

double DegreesToRadians(double Degrees)
{
    return Degrees * Pi / 180.0;
}

bool IsFinite(double Value)
{
    return std::isfinite(Value);
}

bool IsFinite(math::Vec3 Value)
{
    return IsFinite(Value.X) && IsFinite(Value.Y) && IsFinite(Value.Z);
}

bool IsFinite(math::Quat Value)
{
    return IsFinite(Value.X) && IsFinite(Value.Y) && IsFinite(Value.Z) && IsFinite(Value.W);
}

bool IsFinite(math::TransformRT Value)
{
    return IsFinite(Value.TranslationCm) && IsFinite(Value.Rotation) && IsFinite(Value.Scale);
}

double QuatLengthSquared(math::Quat Value)
{
    return Value.X * Value.X + Value.Y * Value.Y + Value.Z * Value.Z + Value.W * Value.W;
}

double QuatDot(math::Quat Left, math::Quat Right)
{
    const math::Quat A = math::Normalize(Left);
    const math::Quat B = math::Normalize(Right);
    return A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W;
}

math::Quat Negate(math::Quat Value)
{
    return {-Value.X, -Value.Y, -Value.Z, -Value.W};
}

math::Vec3 Cross(math::Vec3 Left, math::Vec3 Right)
{
    return {
        Left.Y * Right.Z - Left.Z * Right.Y,
        Left.Z * Right.X - Left.X * Right.Z,
        Left.X * Right.Y - Left.Y * Right.X,
    };
}

bool NormalizeVector(math::Vec3 Value, double Epsilon, math::Vec3& OutUnit)
{
    const double ValueLength = math::Length(Value);
    if (!IsFinite(ValueLength) || ValueLength <= Epsilon)
    {
        return false;
    }
    OutUnit = math::Scale(Value, 1.0 / ValueLength);
    return IsFinite(OutUnit);
}

bool IsUnitScale(math::Vec3 Scale)
{
    return std::abs(Scale.X - 1.0) <= UnitScaleTolerance &&
           std::abs(Scale.Y - 1.0) <= UnitScaleTolerance &&
           std::abs(Scale.Z - 1.0) <= UnitScaleTolerance;
}

bool FromToRotation(math::Vec3 From,
                    math::Vec3 To,
                    math::Vec3 PreferredOppositeAxis,
                    double Epsilon,
                    math::Quat& OutRotation)
{
    math::Vec3 FromUnit;
    math::Vec3 ToUnit;
    if (!NormalizeVector(From, Epsilon, FromUnit) || !NormalizeVector(To, Epsilon, ToUnit))
    {
        return false;
    }

    const double DirectionDot = Clamp(math::Dot(FromUnit, ToUnit), -1.0, 1.0);
    const math::Vec3 RotationAxis = Cross(FromUnit, ToUnit);
    const math::Quat Candidate = {
        RotationAxis.X,
        RotationAxis.Y,
        RotationAxis.Z,
        1.0 + DirectionDot,
    };
    const double CandidateLengthSquared = QuatLengthSquared(Candidate);
    if (IsFinite(CandidateLengthSquared) &&
        CandidateLengthSquared > std::numeric_limits<double>::min())
    {
        OutRotation = math::Normalize(Candidate);
        return IsFinite(OutRotation) && QuatLengthSquared(OutRotation) > 0.0;
    }

    // Only a numerically exact anti-parallel pair reaches this branch. Near-parallel
    // and near-anti-parallel rotations retain their measurable angular difference.
    {
        const math::Vec3 ProjectedAxis = math::Subtract(
            PreferredOppositeAxis,
            math::Scale(FromUnit, math::Dot(PreferredOppositeAxis, FromUnit)));
        math::Vec3 UnitAxis;
        if (!NormalizeVector(ProjectedAxis, Epsilon, UnitAxis))
        {
            return false;
        }
        OutRotation = math::FromAxisAngleDegrees(UnitAxis, 180.0);
        return IsFinite(OutRotation);
    }
}

void AlignQuaternionHemisphere(math::Quat& Value,
                               bool HasReference,
                               math::Quat Reference,
                               double& OutDot,
                               int& InOutCorrections)
{
    if (!HasReference)
    {
        OutDot = 1.0;
        return;
    }

    OutDot = QuatDot(Value, Reference);
    if (OutDot < 0.0)
    {
        Value = Negate(Value);
        OutDot = -OutDot;
        ++InOutCorrections;
    }
}

TwoBoneIkResult Fail(const TwoBoneIkInput& Input,
                     TwoBoneFailureReason Reason,
                     const char* Message)
{
    TwoBoneIkResult Result;
    Result.Success = false;
    Result.Status = TwoBoneSolveStatus::Failed;
    Result.FailureReason = Reason;
    Result.Message = Message;
    Result.OutputRootLocal = Input.RootLocal;
    Result.OutputMidLocal = Input.MidLocal;
    Result.OutputEndLocal = Input.EndLocal;
    return Result;
}

bool ValidTopology(const TwoBoneChainTopology& Chain)
{
    if (Chain.RootIndex < 0 || Chain.MidIndex < 0 || Chain.EndIndex < 0)
    {
        return false;
    }
    if (Chain.RootIndex == Chain.MidIndex || Chain.RootIndex == Chain.EndIndex || Chain.MidIndex == Chain.EndIndex)
    {
        return false;
    }
    return Chain.MidParentIndex == Chain.RootIndex && Chain.EndParentIndex == Chain.MidIndex;
}

bool ValidQuaternion(math::Quat Value, double Epsilon)
{
    return IsFinite(Value) && QuatLengthSquared(Value) > Epsilon * Epsilon;
}
} // namespace

TwoBoneIkResult SolveAnalyticTwoBone(const TwoBoneIkInput& Input)
{
    if (!IsFinite(Input.Epsilon) || !IsFinite(Input.PositionToleranceCm) ||
        !IsFinite(Input.LengthToleranceCm) || Input.Epsilon <= 0.0 ||
        Input.PositionToleranceCm <= 0.0 || Input.LengthToleranceCm <= 0.0)
    {
        return Fail(Input, TwoBoneFailureReason::InvalidTolerance, "solver tolerances must be finite and positive");
    }
    if (!ValidTopology(Input.Chain))
    {
        return Fail(Input, TwoBoneFailureReason::InvalidTopology,
                    "root/mid/end indices must be distinct and parent links must form root->mid->end");
    }
    if (!IsFinite(Input.RootParentModel) || !IsFinite(Input.RootLocal) || !IsFinite(Input.MidLocal) ||
        !IsFinite(Input.EndLocal) || !IsFinite(Input.TargetPositionModelCm) ||
        !IsFinite(Input.PolePositionModelCm) || !IsFinite(Input.ConfiguredFallbackAxisModel))
    {
        return Fail(Input, TwoBoneFailureReason::NonFiniteInput, "all transform, target, pole, and fallback values must be finite");
    }
    if (!ValidQuaternion(Input.RootParentModel.Rotation, Input.Epsilon) ||
        !ValidQuaternion(Input.RootLocal.Rotation, Input.Epsilon) ||
        !ValidQuaternion(Input.MidLocal.Rotation, Input.Epsilon) ||
        !ValidQuaternion(Input.EndLocal.Rotation, Input.Epsilon) ||
        (Input.OrientationPolicy == EndOrientationPolicy::ApplyExplicitModel &&
         !ValidQuaternion(Input.ExplicitEndModelOrientation, Input.Epsilon)))
    {
        return Fail(Input, TwoBoneFailureReason::InvalidQuaternion, "input rotations must be finite non-zero quaternions");
    }
    const QuaternionContinuityReference& Previous = Input.PreviousLocalRotations;
    if ((Previous.HasRootLocal && !ValidQuaternion(Previous.RootLocal, Input.Epsilon)) ||
        (Previous.HasMidLocal && !ValidQuaternion(Previous.MidLocal, Input.Epsilon)) ||
        (Previous.HasEndLocal && !ValidQuaternion(Previous.EndLocal, Input.Epsilon)))
    {
        return Fail(Input, TwoBoneFailureReason::InvalidQuaternion, "continuity references must be finite non-zero quaternions");
    }
    if (!IsUnitScale(Input.RootParentModel.Scale) ||
        !IsUnitScale(Input.RootLocal.Scale) ||
        !IsUnitScale(Input.MidLocal.Scale) ||
        !IsUnitScale(Input.EndLocal.Scale))
    {
        return Fail(Input, TwoBoneFailureReason::UnsupportedScale,
                    "D1-16A is a rigid rotation-only solver and requires unit scale within fixed 1e-9 dimensionless tolerance on parent/root/mid/end");
    }
    if (Input.BendLimits.Enabled &&
        (!IsFinite(Input.BendLimits.MinimumBendDegrees) || !IsFinite(Input.BendLimits.MaximumBendDegrees) ||
         Input.BendLimits.MinimumBendDegrees < 0.0 || Input.BendLimits.MaximumBendDegrees > 180.0 ||
         Input.BendLimits.MinimumBendDegrees > Input.BendLimits.MaximumBendDegrees))
    {
        return Fail(Input, TwoBoneFailureReason::InvalidJointLimits,
                    "bend limits must satisfy 0 <= minimum <= maximum <= 180 degrees");
    }
    if (Input.SoftReach.Enabled &&
        (!IsFinite(Input.SoftReach.StartRatio) || !IsFinite(Input.SoftReach.MaximumExtensionRatio) ||
         Input.SoftReach.StartRatio <= 0.0 ||
         Input.SoftReach.StartRatio >= Input.SoftReach.MaximumExtensionRatio ||
         Input.SoftReach.MaximumExtensionRatio > 1.0))
    {
        return Fail(Input, TwoBoneFailureReason::InvalidSoftReach,
                    "soft reach requires 0 < start ratio < maximum extension ratio <= 1");
    }

    const double FirstLength = math::Length(Input.MidLocal.TranslationCm);
    const double SecondLength = math::Length(Input.EndLocal.TranslationCm);
    if (!IsFinite(FirstLength) || !IsFinite(SecondLength) ||
        FirstLength <= Input.Epsilon || SecondLength <= Input.Epsilon)
    {
        return Fail(Input, TwoBoneFailureReason::DegenerateSegment,
                    "mid and end local translations must define two non-zero segment lengths");
    }

    const math::TransformRT InputRootModel = math::Compose(Input.RootParentModel, Input.RootLocal);
    const math::TransformRT InputMidModel = math::Compose(InputRootModel, Input.MidLocal);
    const math::TransformRT InputEndModel = math::Compose(InputMidModel, Input.EndLocal);
    if (!IsFinite(InputRootModel) || !IsFinite(InputMidModel) || !IsFinite(InputEndModel))
    {
        return Fail(Input, TwoBoneFailureReason::NumericalFailure, "input local-to-model rebuild produced a non-finite transform");
    }

    TwoBoneIkTelemetry Telemetry;
    Telemetry.FirstSegmentLengthCm = FirstLength;
    Telemetry.SecondSegmentLengthCm = SecondLength;
    Telemetry.MinimumReachCm = std::abs(FirstLength - SecondLength);
    Telemetry.MaximumReachCm = FirstLength + SecondLength;
    Telemetry.InputRootPositionModelCm = InputRootModel.TranslationCm;

    const math::Vec3 RootToTarget = math::Subtract(Input.TargetPositionModelCm, InputRootModel.TranslationCm);
    Telemetry.TargetDistanceCm = math::Length(RootToTarget);
    math::Vec3 TargetDirection;
    if (!NormalizeVector(RootToTarget, Input.Epsilon, TargetDirection))
    {
        return Fail(Input, TwoBoneFailureReason::DegenerateTargetDirection,
                    "target must be a finite non-zero distance from the root");
    }

    Telemetry.TargetGeometricallyReachable =
        Telemetry.TargetDistanceCm >= Telemetry.MinimumReachCm - Input.PositionToleranceCm &&
        Telemetry.TargetDistanceCm <= Telemetry.MaximumReachCm + Input.PositionToleranceCm;
    double SolveDistance = Telemetry.TargetDistanceCm;
    if (SolveDistance < Telemetry.MinimumReachCm)
    {
        SolveDistance = Telemetry.MinimumReachCm;
        Telemetry.InnerReachClamped = true;
    }
    if (SolveDistance > Telemetry.MaximumReachCm)
    {
        SolveDistance = Telemetry.MaximumReachCm;
        Telemetry.OuterReachClamped = true;
    }

    if (Input.SoftReach.Enabled)
    {
        const double SoftStart = Telemetry.MaximumReachCm * Input.SoftReach.StartRatio;
        const double SoftLimit = Telemetry.MaximumReachCm * Input.SoftReach.MaximumExtensionRatio;
        if (SolveDistance > SoftStart)
        {
            const double Span = SoftLimit - SoftStart;
            const double Softened = SoftStart + Span * (1.0 - std::exp(-(SolveDistance - SoftStart) / Span));
            const double Bounded = std::min(Softened, SoftLimit);
            Telemetry.SoftReachApplied = Bounded < SolveDistance - Input.Epsilon;
            SolveDistance = Bounded;
        }
    }

    const double BendCosine = Clamp(
        (SolveDistance * SolveDistance - FirstLength * FirstLength - SecondLength * SecondLength) /
            (2.0 * FirstLength * SecondLength),
        -1.0,
        1.0);
    double BendDegrees = RadiansToDegrees(std::acos(BendCosine));
    if (Input.BendLimits.Enabled)
    {
        const double LimitedBend = Clamp(BendDegrees,
                                         Input.BendLimits.MinimumBendDegrees,
                                         Input.BendLimits.MaximumBendDegrees);
        Telemetry.JointLimitClamped = std::abs(LimitedBend - BendDegrees) > Input.Epsilon;
        BendDegrees = LimitedBend;
        const double DistanceSquared =
            FirstLength * FirstLength + SecondLength * SecondLength +
            2.0 * FirstLength * SecondLength * std::cos(DegreesToRadians(BendDegrees));
        SolveDistance = std::sqrt(std::max(0.0, DistanceSquared));
    }
    if (!IsFinite(SolveDistance) || SolveDistance <= Input.Epsilon)
    {
        return Fail(Input, TwoBoneFailureReason::NumericalFailure,
                    "effective solve distance collapsed to a degenerate value");
    }
    Telemetry.EffectiveSolveDistanceCm = SolveDistance;
    Telemetry.BendDegrees = BendDegrees;

    math::Vec3 PoleOffset = math::Subtract(Input.PolePositionModelCm, InputRootModel.TranslationCm);
    math::Vec3 BendDirectionCandidate = math::Subtract(PoleOffset,
                                                       math::Scale(TargetDirection, math::Dot(PoleOffset, TargetDirection)));
    math::Vec3 BendDirection;
    if (!NormalizeVector(BendDirectionCandidate, Input.Epsilon, BendDirection))
    {
        if (Input.PoleFallback != PoleFallbackPolicy::AllowConfiguredAxis)
        {
            return Fail(Input, TwoBoneFailureReason::CollinearPole,
                        "pole is collinear with the target direction and configured fallback is disabled");
        }
        BendDirectionCandidate = math::Subtract(
            Input.ConfiguredFallbackAxisModel,
            math::Scale(TargetDirection, math::Dot(Input.ConfiguredFallbackAxisModel, TargetDirection)));
        if (!NormalizeVector(BendDirectionCandidate, Input.Epsilon, BendDirection))
        {
            return Fail(Input, TwoBoneFailureReason::InvalidConfiguredFallback,
                        "configured pole fallback axis is also collinear or degenerate");
        }
        Telemetry.PoleFallbackUsed = true;
    }

    math::Vec3 PlaneNormal;
    if (!NormalizeVector(Cross(TargetDirection, BendDirection), Input.Epsilon, PlaneNormal))
    {
        return Fail(Input, TwoBoneFailureReason::NumericalFailure, "bend-plane normal could not be normalized");
    }

    const double Along =
        (FirstLength * FirstLength - SecondLength * SecondLength + SolveDistance * SolveDistance) /
        (2.0 * SolveDistance);
    const double HeightSquared = std::max(0.0, FirstLength * FirstLength - Along * Along);
    const double Height = std::sqrt(HeightSquared);
    if (!IsFinite(Along) || !IsFinite(Height))
    {
        return Fail(Input, TwoBoneFailureReason::NumericalFailure, "law-of-cosines solve produced a non-finite joint position");
    }

    const math::Vec3 DesiredMidPosition = math::Add(
        InputRootModel.TranslationCm,
        math::Add(math::Scale(TargetDirection, Along), math::Scale(BendDirection, Height)));
    const math::Vec3 DesiredEndPosition = math::Add(
        InputRootModel.TranslationCm,
        math::Scale(TargetDirection, SolveDistance));

    math::Quat RootDelta;
    if (!FromToRotation(math::Subtract(InputMidModel.TranslationCm, InputRootModel.TranslationCm),
                        math::Subtract(DesiredMidPosition, InputRootModel.TranslationCm),
                        PlaneNormal,
                        Input.Epsilon,
                        RootDelta))
    {
        return Fail(Input, TwoBoneFailureReason::RotationRebuildFailed, "root rotation delta could not be reconstructed");
    }

    TwoBoneIkResult Result;
    Result.OutputRootLocal = Input.RootLocal;
    const math::Quat DesiredRootModelRotation = math::Multiply(RootDelta, InputRootModel.Rotation);
    Result.OutputRootLocal.Rotation = math::Multiply(math::Conjugate(Input.RootParentModel.Rotation),
                                                    DesiredRootModelRotation);
    AlignQuaternionHemisphere(Result.OutputRootLocal.Rotation,
                              Previous.HasRootLocal,
                              Previous.RootLocal,
                              Telemetry.RootQuaternionDotPrevious,
                              Telemetry.QuaternionSignCorrections);
    Result.OutputRootModel = math::Compose(Input.RootParentModel, Result.OutputRootLocal);

    const math::TransformRT MidBeforeCorrection = math::Compose(Result.OutputRootModel, Input.MidLocal);
    const math::TransformRT EndBeforeCorrection = math::Compose(MidBeforeCorrection, Input.EndLocal);
    math::Quat MidDelta;
    if (!FromToRotation(math::Subtract(EndBeforeCorrection.TranslationCm, MidBeforeCorrection.TranslationCm),
                        math::Subtract(DesiredEndPosition, DesiredMidPosition),
                        PlaneNormal,
                        Input.Epsilon,
                        MidDelta))
    {
        return Fail(Input, TwoBoneFailureReason::RotationRebuildFailed, "mid rotation delta could not be reconstructed");
    }

    Result.OutputMidLocal = Input.MidLocal;
    const math::Quat DesiredMidModelRotation = math::Multiply(MidDelta, MidBeforeCorrection.Rotation);
    Result.OutputMidLocal.Rotation = math::Multiply(math::Conjugate(Result.OutputRootModel.Rotation),
                                                   DesiredMidModelRotation);
    AlignQuaternionHemisphere(Result.OutputMidLocal.Rotation,
                              Previous.HasMidLocal,
                              Previous.MidLocal,
                              Telemetry.MidQuaternionDotPrevious,
                              Telemetry.QuaternionSignCorrections);
    Result.OutputMidModel = math::Compose(Result.OutputRootModel, Result.OutputMidLocal);

    Result.OutputEndLocal = Input.EndLocal;
    if (Input.OrientationPolicy == EndOrientationPolicy::ApplyExplicitModel)
    {
        Result.OutputEndLocal.Rotation = math::Multiply(math::Conjugate(Result.OutputMidModel.Rotation),
                                                       math::Normalize(Input.ExplicitEndModelOrientation));
        AlignQuaternionHemisphere(Result.OutputEndLocal.Rotation,
                                  Previous.HasEndLocal,
                                  Previous.EndLocal,
                                  Telemetry.EndQuaternionDotPrevious,
                                  Telemetry.QuaternionSignCorrections);
        Telemetry.EndOrientationApplied = true;
    }
    else if (Previous.HasEndLocal)
    {
        Telemetry.EndQuaternionDotPrevious = QuatDot(Result.OutputEndLocal.Rotation, Previous.EndLocal);
    }
    Result.OutputEndModel = math::Compose(Result.OutputMidModel, Result.OutputEndLocal);

    if (!IsFinite(Result.OutputRootLocal) || !IsFinite(Result.OutputMidLocal) || !IsFinite(Result.OutputEndLocal) ||
        !IsFinite(Result.OutputRootModel) || !IsFinite(Result.OutputMidModel) || !IsFinite(Result.OutputEndModel))
    {
        return Fail(Input, TwoBoneFailureReason::NumericalFailure, "output local/model rebuild produced a non-finite transform");
    }

    Telemetry.OutputRootPositionModelCm = Result.OutputRootModel.TranslationCm;
    Telemetry.OutputMidPositionModelCm = Result.OutputMidModel.TranslationCm;
    Telemetry.OutputEndPositionModelCm = Result.OutputEndModel.TranslationCm;
    Telemetry.OutputFirstSegmentLengthCm = math::Length(math::Subtract(
        Result.OutputMidModel.TranslationCm, Result.OutputRootModel.TranslationCm));
    Telemetry.OutputSecondSegmentLengthCm = math::Length(math::Subtract(
        Result.OutputEndModel.TranslationCm, Result.OutputMidModel.TranslationCm));
    Telemetry.FirstSegmentLengthErrorCm = std::abs(Telemetry.OutputFirstSegmentLengthCm - FirstLength);
    Telemetry.SecondSegmentLengthErrorCm = std::abs(Telemetry.OutputSecondSegmentLengthCm - SecondLength);
    Telemetry.EndpointErrorCm = math::Length(math::Subtract(Result.OutputEndModel.TranslationCm,
                                                           Input.TargetPositionModelCm));
    Telemetry.ReachedTarget = Telemetry.EndpointErrorCm <= Input.PositionToleranceCm;
    Telemetry.EndLocalOrientationPreserved =
        Input.OrientationPolicy == EndOrientationPolicy::PreserveInputLocal &&
        Result.OutputEndLocal.Rotation.X == Input.EndLocal.Rotation.X &&
        Result.OutputEndLocal.Rotation.Y == Input.EndLocal.Rotation.Y &&
        Result.OutputEndLocal.Rotation.Z == Input.EndLocal.Rotation.Z &&
        Result.OutputEndLocal.Rotation.W == Input.EndLocal.Rotation.W;
    Telemetry.LocalTranslationsPreserved =
        math::NearlyEqual(Result.OutputRootLocal.TranslationCm, Input.RootLocal.TranslationCm, 0.0) &&
        math::NearlyEqual(Result.OutputMidLocal.TranslationCm, Input.MidLocal.TranslationCm, 0.0) &&
        math::NearlyEqual(Result.OutputEndLocal.TranslationCm, Input.EndLocal.TranslationCm, 0.0);
    Telemetry.RootPelvisGlobalCompensationApplied = false;
    Telemetry.ContinuityReferenceUsed = Previous.HasRootLocal || Previous.HasMidLocal || Previous.HasEndLocal;
    Telemetry.MinimumQuaternionDotPrevious = std::min({Telemetry.RootQuaternionDotPrevious,
                                                       Telemetry.MidQuaternionDotPrevious,
                                                       Telemetry.EndQuaternionDotPrevious});

    if (!IsFinite(Telemetry.OutputFirstSegmentLengthCm) || !IsFinite(Telemetry.OutputSecondSegmentLengthCm) ||
        !IsFinite(Telemetry.EndpointErrorCm) ||
        Telemetry.FirstSegmentLengthErrorCm > Input.LengthToleranceCm ||
        Telemetry.SecondSegmentLengthErrorCm > Input.LengthToleranceCm)
    {
        return Fail(Input, TwoBoneFailureReason::NumericalFailure,
                    "output segment lengths are non-finite or exceeded the configured invariant tolerance");
    }

    const bool HasExplicitClampReason = Telemetry.InnerReachClamped || Telemetry.OuterReachClamped ||
                                        Telemetry.SoftReachApplied || Telemetry.JointLimitClamped;
    if (!Telemetry.ReachedTarget && !HasExplicitClampReason)
    {
        Result.Success = false;
        Result.Status = TwoBoneSolveStatus::Failed;
        Result.FailureReason = TwoBoneFailureReason::PositionPostconditionFailed;
        Result.Message = "unconstrained analytic solve missed the configured position tolerance";
        Result.Telemetry = Telemetry;
        return Result;
    }

    Result.Success = true;
    Result.Status = HasExplicitClampReason ? TwoBoneSolveStatus::SolvedClamped
                                           : TwoBoneSolveStatus::Solved;
    Result.FailureReason = TwoBoneFailureReason::None;
    Result.Message = Result.Status == TwoBoneSolveStatus::Solved
                         ? "analytic two-bone position target solved"
                         : "analytic two-bone solve completed with an explicit reach/soft/limit clamp";
    Result.Telemetry = Telemetry;
    return Result;
}

const char* ToString(PoleFallbackPolicy Policy)
{
    switch (Policy)
    {
    case PoleFallbackPolicy::FailClosed: return "fail_closed";
    case PoleFallbackPolicy::AllowConfiguredAxis: return "allow_configured_axis";
    default: return "unknown";
    }
}

const char* ToString(EndOrientationPolicy Policy)
{
    switch (Policy)
    {
    case EndOrientationPolicy::PreserveInputLocal: return "preserve_input_local";
    case EndOrientationPolicy::ApplyExplicitModel: return "apply_explicit_model";
    default: return "unknown";
    }
}

const char* ToString(TwoBoneSolveStatus Status)
{
    switch (Status)
    {
    case TwoBoneSolveStatus::Failed: return "failed";
    case TwoBoneSolveStatus::Solved: return "solved";
    case TwoBoneSolveStatus::SolvedClamped: return "solved_clamped";
    default: return "unknown";
    }
}

const char* ToString(TwoBoneFailureReason Reason)
{
    switch (Reason)
    {
    case TwoBoneFailureReason::None: return "none";
    case TwoBoneFailureReason::InvalidTopology: return "invalid_topology";
    case TwoBoneFailureReason::NonFiniteInput: return "nonfinite_input";
    case TwoBoneFailureReason::InvalidQuaternion: return "invalid_quaternion";
    case TwoBoneFailureReason::UnsupportedScale: return "unsupported_scale";
    case TwoBoneFailureReason::DegenerateSegment: return "degenerate_segment";
    case TwoBoneFailureReason::DegenerateTargetDirection: return "degenerate_target_direction";
    case TwoBoneFailureReason::CollinearPole: return "collinear_pole";
    case TwoBoneFailureReason::InvalidConfiguredFallback: return "invalid_configured_fallback";
    case TwoBoneFailureReason::InvalidJointLimits: return "invalid_joint_limits";
    case TwoBoneFailureReason::InvalidSoftReach: return "invalid_soft_reach";
    case TwoBoneFailureReason::InvalidTolerance: return "invalid_tolerance";
    case TwoBoneFailureReason::RotationRebuildFailed: return "rotation_rebuild_failed";
    case TwoBoneFailureReason::PositionPostconditionFailed: return "position_postcondition_failed";
    case TwoBoneFailureReason::NumericalFailure: return "numerical_failure";
    default: return "unknown";
    }
}
} // namespace skrtg::core::ik
