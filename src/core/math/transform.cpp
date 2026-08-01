#include "skrtg/core/math/transform.h"

#include <cmath>

namespace skrtg::core::math
{
namespace
{
constexpr double Pi = 3.141592653589793238462643383279502884;

double DegreesToRadians(double Degrees)
{
    return Degrees * Pi / 180.0;
}

Vec3 Cross(Vec3 Left, Vec3 Right)
{
    return {
        Left.Y * Right.Z - Left.Z * Right.Y,
        Left.Z * Right.X - Left.X * Right.Z,
        Left.X * Right.Y - Left.Y * Right.X,
    };
}
} // namespace

Vec3 Add(Vec3 Left, Vec3 Right)
{
    return {Left.X + Right.X, Left.Y + Right.Y, Left.Z + Right.Z};
}

Vec3 Subtract(Vec3 Left, Vec3 Right)
{
    return {Left.X - Right.X, Left.Y - Right.Y, Left.Z - Right.Z};
}

Vec3 MultiplyComponents(Vec3 Left, Vec3 Right)
{
    return {Left.X * Right.X, Left.Y * Right.Y, Left.Z * Right.Z};
}

Vec3 Scale(Vec3 Value, double Scalar)
{
    return {Value.X * Scalar, Value.Y * Scalar, Value.Z * Scalar};
}

double Dot(Vec3 Left, Vec3 Right)
{
    return Left.X * Right.X + Left.Y * Right.Y + Left.Z * Right.Z;
}

double Length(Vec3 Value)
{
    return std::sqrt(Dot(Value, Value));
}

Quat IdentityQuat()
{
    return {};
}

Quat Normalize(Quat Value)
{
    const double LengthSq = Value.X * Value.X + Value.Y * Value.Y + Value.Z * Value.Z + Value.W * Value.W;
    if (LengthSq <= 0.0)
    {
        return IdentityQuat();
    }

    const double InvLength = 1.0 / std::sqrt(LengthSq);
    return {Value.X * InvLength, Value.Y * InvLength, Value.Z * InvLength, Value.W * InvLength};
}

Quat Multiply(Quat Parent, Quat Child)
{
    const Quat A = Normalize(Parent);
    const Quat B = Normalize(Child);
    return Normalize({
        A.W * B.X + A.X * B.W + A.Y * B.Z - A.Z * B.Y,
        A.W * B.Y - A.X * B.Z + A.Y * B.W + A.Z * B.X,
        A.W * B.Z + A.X * B.Y - A.Y * B.X + A.Z * B.W,
        A.W * B.W - A.X * B.X - A.Y * B.Y - A.Z * B.Z,
    });
}

Quat Conjugate(Quat Value)
{
    const Quat Normalized = Normalize(Value);
    return {-Normalized.X, -Normalized.Y, -Normalized.Z, Normalized.W};
}

Quat FromAxisAngleDegrees(Vec3 Axis, double Degrees)
{
    const double AxisLength = Length(Axis);
    if (AxisLength <= 0.0)
    {
        return IdentityQuat();
    }

    const Vec3 UnitAxis = Scale(Axis, 1.0 / AxisLength);
    const double HalfAngle = DegreesToRadians(Degrees) * 0.5;
    const double SinHalf = std::sin(HalfAngle);
    return Normalize({UnitAxis.X * SinHalf, UnitAxis.Y * SinHalf, UnitAxis.Z * SinHalf, std::cos(HalfAngle)});
}

Vec3 RotateVector(Quat Rotation, Vec3 Value)
{
    const Quat Q = Normalize(Rotation);
    const Vec3 Qv{Q.X, Q.Y, Q.Z};
    const Vec3 T = Scale(Cross(Qv, Value), 2.0);
    return Add(Value, Add(Scale(T, Q.W), Cross(Qv, T)));
}

TransformRT IdentityTransform()
{
    return {};
}

TransformRT Compose(TransformRT Parent, TransformRT Local)
{
    TransformRT Result;
    Result.Scale = MultiplyComponents(Parent.Scale, Local.Scale);
    Result.Rotation = Multiply(Parent.Rotation, Local.Rotation);
    Result.TranslationCm = Add(Parent.TranslationCm, RotateVector(Parent.Rotation, MultiplyComponents(Parent.Scale, Local.TranslationCm)));
    return Result;
}

bool InverseUnitScaleTransform(TransformRT Value,
                               TransformRT& OutInverse,
                               double ScaleTolerance)
{
    const auto Finite = [](double Component)
    {
        return std::isfinite(Component);
    };
    const bool FiniteTransform =
        Finite(Value.TranslationCm.X) && Finite(Value.TranslationCm.Y) &&
        Finite(Value.TranslationCm.Z) && Finite(Value.Rotation.X) &&
        Finite(Value.Rotation.Y) && Finite(Value.Rotation.Z) &&
        Finite(Value.Rotation.W) && Finite(Value.Scale.X) &&
        Finite(Value.Scale.Y) && Finite(Value.Scale.Z);
    const double RotationNormSquared =
        Value.Rotation.X * Value.Rotation.X +
        Value.Rotation.Y * Value.Rotation.Y +
        Value.Rotation.Z * Value.Rotation.Z +
        Value.Rotation.W * Value.Rotation.W;
    if (!FiniteTransform || !Finite(ScaleTolerance) || ScaleTolerance < 0.0 ||
        RotationNormSquared <= 1.0e-18 ||
        std::abs(Value.Scale.X - 1.0) > ScaleTolerance ||
        std::abs(Value.Scale.Y - 1.0) > ScaleTolerance ||
        std::abs(Value.Scale.Z - 1.0) > ScaleTolerance)
    {
        return false;
    }

    OutInverse = IdentityTransform();
    OutInverse.Rotation = Conjugate(Value.Rotation);
    OutInverse.TranslationCm = RotateVector(
        OutInverse.Rotation, Scale(Value.TranslationCm, -1.0));
    return true;
}

bool RelativeUnitScaleTransform(TransformRT ParentModel,
                                TransformRT ChildModel,
                                TransformRT& OutLocal,
                                double ScaleTolerance)
{
    TransformRT ParentInverse;
    if (!InverseUnitScaleTransform(ParentModel, ParentInverse, ScaleTolerance))
    {
        return false;
    }
    const double ChildRotationNormSquared =
        ChildModel.Rotation.X * ChildModel.Rotation.X +
        ChildModel.Rotation.Y * ChildModel.Rotation.Y +
        ChildModel.Rotation.Z * ChildModel.Rotation.Z +
        ChildModel.Rotation.W * ChildModel.Rotation.W;
    if (!std::isfinite(ChildModel.TranslationCm.X) ||
        !std::isfinite(ChildModel.TranslationCm.Y) ||
        !std::isfinite(ChildModel.TranslationCm.Z) ||
        !std::isfinite(ChildModel.Rotation.X) ||
        !std::isfinite(ChildModel.Rotation.Y) ||
        !std::isfinite(ChildModel.Rotation.Z) ||
        !std::isfinite(ChildModel.Rotation.W) ||
        !std::isfinite(ChildModel.Scale.X) ||
        !std::isfinite(ChildModel.Scale.Y) ||
        !std::isfinite(ChildModel.Scale.Z) ||
        ChildRotationNormSquared <= 1.0e-18 ||
        std::abs(ChildModel.Scale.X - 1.0) > ScaleTolerance ||
        std::abs(ChildModel.Scale.Y - 1.0) > ScaleTolerance ||
        std::abs(ChildModel.Scale.Z - 1.0) > ScaleTolerance)
    {
        return false;
    }
    OutLocal = Compose(ParentInverse, ChildModel);
    return std::isfinite(OutLocal.TranslationCm.X) &&
           std::isfinite(OutLocal.TranslationCm.Y) &&
           std::isfinite(OutLocal.TranslationCm.Z) &&
           std::isfinite(OutLocal.Rotation.X) &&
           std::isfinite(OutLocal.Rotation.Y) &&
           std::isfinite(OutLocal.Rotation.Z) &&
           std::isfinite(OutLocal.Rotation.W) &&
           std::isfinite(OutLocal.Scale.X) &&
           std::isfinite(OutLocal.Scale.Y) &&
           std::isfinite(OutLocal.Scale.Z);
}

bool NearlyEqual(double Left, double Right, double Tolerance)
{
    return std::abs(Left - Right) <= Tolerance;
}

bool NearlyEqual(Vec3 Left, Vec3 Right, double Tolerance)
{
    return NearlyEqual(Left.X, Right.X, Tolerance) && NearlyEqual(Left.Y, Right.Y, Tolerance) &&
           NearlyEqual(Left.Z, Right.Z, Tolerance);
}

bool NearlyEqual(Quat Left, Quat Right, double Tolerance)
{
    const Quat A = Normalize(Left);
    const Quat B = Normalize(Right);
    const bool Direct = NearlyEqual(A.X, B.X, Tolerance) && NearlyEqual(A.Y, B.Y, Tolerance) &&
                        NearlyEqual(A.Z, B.Z, Tolerance) && NearlyEqual(A.W, B.W, Tolerance);
    const bool Negated = NearlyEqual(A.X, -B.X, Tolerance) && NearlyEqual(A.Y, -B.Y, Tolerance) &&
                         NearlyEqual(A.Z, -B.Z, Tolerance) && NearlyEqual(A.W, -B.W, Tolerance);
    return Direct || Negated;
}

bool NearlyEqual(TransformRT Left, TransformRT Right, double TranslationTolerance, double RotationTolerance, double ScaleTolerance)
{
    return NearlyEqual(Left.TranslationCm, Right.TranslationCm, TranslationTolerance) &&
           NearlyEqual(Left.Rotation, Right.Rotation, RotationTolerance) && NearlyEqual(Left.Scale, Right.Scale, ScaleTolerance);
}
} // namespace skrtg::core::math
