#pragma once

#include <array>

namespace skrtg::core::math
{
struct Vec3
{
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
};

struct Quat
{
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    double W = 1.0;
};

struct TransformRT
{
    Vec3 TranslationCm;
    Quat Rotation;
    Vec3 Scale{1.0, 1.0, 1.0};
};

Vec3 Add(Vec3 Left, Vec3 Right);
Vec3 Subtract(Vec3 Left, Vec3 Right);
Vec3 MultiplyComponents(Vec3 Left, Vec3 Right);
Vec3 Scale(Vec3 Value, double Scalar);

double Dot(Vec3 Left, Vec3 Right);
double Length(Vec3 Value);

Quat IdentityQuat();
Quat Normalize(Quat Value);
Quat Multiply(Quat Parent, Quat Child);
Quat Conjugate(Quat Value);
Quat FromAxisAngleDegrees(Vec3 Axis, double Degrees);
Vec3 RotateVector(Quat Rotation, Vec3 Value);

TransformRT IdentityTransform();
TransformRT Compose(TransformRT Parent, TransformRT Local);
bool InverseUnitScaleTransform(TransformRT Value,
                               TransformRT& OutInverse,
                               double ScaleTolerance = 1.0e-9);
bool RelativeUnitScaleTransform(TransformRT ParentModel,
                                TransformRT ChildModel,
                                TransformRT& OutLocal,
                                double ScaleTolerance = 1.0e-9);

bool NearlyEqual(double Left, double Right, double Tolerance);
bool NearlyEqual(Vec3 Left, Vec3 Right, double Tolerance);
bool NearlyEqual(Quat Left, Quat Right, double Tolerance);
bool NearlyEqual(TransformRT Left, TransformRT Right, double TranslationTolerance, double RotationTolerance, double ScaleTolerance);
} // namespace skrtg::core::math
