#include "skrtg/viewer/camera.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace skrtg::viewer
{
namespace
{
constexpr float Pi = 3.14159265358979323846F;

Vec3 Add(const Vec3& Left, const Vec3& Right)
{
    return {Left.X + Right.X, Left.Y + Right.Y, Left.Z + Right.Z};
}

Vec3 Subtract(const Vec3& Left, const Vec3& Right)
{
    return {Left.X - Right.X, Left.Y - Right.Y, Left.Z - Right.Z};
}

Vec3 Multiply(const Vec3& Value, const float Scale)
{
    return {Value.X * Scale, Value.Y * Scale, Value.Z * Scale};
}

float Dot(const Vec3& Left, const Vec3& Right)
{
    return Left.X * Right.X + Left.Y * Right.Y + Left.Z * Right.Z;
}

Vec3 Cross(const Vec3& Left, const Vec3& Right)
{
    return {
        Left.Y * Right.Z - Left.Z * Right.Y,
        Left.Z * Right.X - Left.X * Right.Z,
        Left.X * Right.Y - Left.Y * Right.X};
}

Vec3 Normalize(const Vec3& Value)
{
    const float Length = std::sqrt(Dot(Value, Value));
    if (Length <= std::numeric_limits<float>::epsilon())
        return {0.0F, 0.0F, 0.0F};
    return Multiply(Value, 1.0F / Length);
}

struct CameraBasis
{
    Vec3 Eye;
    Vec3 Forward;
    Vec3 Right;
    Vec3 Up;
};

CameraBasis MakeBasis(const OrbitCamera& Camera)
{
    const float CosPitch = std::cos(Camera.PitchRadians);
    const Vec3 EyeDirection = {
        std::sin(Camera.YawRadians) * CosPitch,
        std::sin(Camera.PitchRadians),
        std::cos(Camera.YawRadians) * CosPitch};
    const Vec3 Eye = Add(
        Camera.Focus, Multiply(EyeDirection, Camera.Distance));
    const Vec3 Forward = Normalize(Subtract(Camera.Focus, Eye));
    const Vec3 Right = Normalize(Cross(Forward, {0.0F, 1.0F, 0.0F}));
    const Vec3 Up = Normalize(Cross(Right, Forward));
    return {Eye, Forward, Right, Up};
}
} // namespace

Bounds3 ComputeBounds(const std::vector<Vec3>& Points)
{
    Bounds3 Bounds;
    if (Points.empty())
        return Bounds;
    Bounds.Minimum = Points.front();
    Bounds.Maximum = Points.front();
    Bounds.Valid = true;
    for (const Vec3& Point : Points)
    {
        Bounds.Minimum.X = std::min(Bounds.Minimum.X, Point.X);
        Bounds.Minimum.Y = std::min(Bounds.Minimum.Y, Point.Y);
        Bounds.Minimum.Z = std::min(Bounds.Minimum.Z, Point.Z);
        Bounds.Maximum.X = std::max(Bounds.Maximum.X, Point.X);
        Bounds.Maximum.Y = std::max(Bounds.Maximum.Y, Point.Y);
        Bounds.Maximum.Z = std::max(Bounds.Maximum.Z, Point.Z);
    }
    return Bounds;
}

OrbitCamera FitCameraToBounds(
    const Bounds3& Bounds,
    const float ViewportAspectRatio)
{
    OrbitCamera Camera;
    if (!Bounds.Valid)
        return Camera;
    Camera.Focus = {
        (Bounds.Minimum.X + Bounds.Maximum.X) * 0.5F,
        (Bounds.Minimum.Y + Bounds.Maximum.Y) * 0.5F,
        (Bounds.Minimum.Z + Bounds.Maximum.Z) * 0.5F};
    const float Width = std::max(Bounds.Maximum.X - Bounds.Minimum.X, 1.0F);
    const float Height = std::max(Bounds.Maximum.Y - Bounds.Minimum.Y, 1.0F);
    const float Depth = std::max(Bounds.Maximum.Z - Bounds.Minimum.Z, 1.0F);
    const float Aspect = std::max(ViewportAspectRatio, 0.1F);
    Camera.OrthographicHalfHeight =
        std::max(Height * 0.60F, Width * 0.60F / Aspect);
    Camera.OrthographicHalfHeight =
        std::max(Camera.OrthographicHalfHeight, Depth * 0.60F / Aspect);
    Camera.Distance =
        Camera.OrthographicHalfHeight /
        std::tan(Camera.VerticalFieldOfViewRadians * 0.5F) * 1.08F;
    return Camera;
}

ProjectedPoint ProjectPoint(
    const OrbitCamera& Camera,
    const ProjectionViewport& Viewport,
    const Vec3& Point)
{
    const CameraBasis Basis = MakeBasis(Camera);
    const Vec3 Relative = Subtract(Point, Basis.Eye);
    const float ForwardDepth = Dot(Relative, Basis.Forward);
    if (ForwardDepth <= 0.01F || Viewport.Width <= 0.0F ||
        Viewport.Height <= 0.0F)
    {
        return {0.0F, 0.0F, ForwardDepth, false};
    }

    const float ViewX = Dot(Relative, Basis.Right);
    const float ViewY = Dot(Relative, Basis.Up);
    const float Aspect = Viewport.Width / Viewport.Height;
    float NdcX = 0.0F;
    float NdcY = 0.0F;
    if (Camera.Orthographic)
    {
        const float HalfHeight =
            std::max(Camera.OrthographicHalfHeight, 0.001F);
        NdcX = ViewX / (HalfHeight * Aspect);
        NdcY = ViewY / HalfHeight;
    }
    else
    {
        const float Tangent =
            std::tan(Camera.VerticalFieldOfViewRadians * 0.5F);
        NdcX = ViewX / (ForwardDepth * Tangent * Aspect);
        NdcY = ViewY / (ForwardDepth * Tangent);
    }
    return {
        (NdcX * 0.5F + 0.5F) * Viewport.Width,
        (0.5F - NdcY * 0.5F) * Viewport.Height,
        ForwardDepth,
        std::isfinite(NdcX) && std::isfinite(NdcY)};
}

void RotateCamera(
    OrbitCamera& Camera,
    const float DeltaX,
    const float DeltaY)
{
    Camera.YawRadians -= DeltaX * 0.006F;
    Camera.PitchRadians = std::clamp(
        Camera.PitchRadians + DeltaY * 0.006F,
        -Pi * 0.48F,
        Pi * 0.48F);
}

void PanCamera(
    OrbitCamera& Camera,
    const float DeltaX,
    const float DeltaY,
    const float ViewportHeight)
{
    if (ViewportHeight <= 1.0F)
        return;
    const CameraBasis Basis = MakeBasis(Camera);
    const float VisibleHeight = Camera.Orthographic
        ? 2.0F * Camera.OrthographicHalfHeight
        : 2.0F * Camera.Distance *
            std::tan(Camera.VerticalFieldOfViewRadians * 0.5F);
    const float UnitsPerPixel = VisibleHeight / ViewportHeight;
    Camera.Focus = Add(
        Camera.Focus,
        Add(
            Multiply(Basis.Right, -DeltaX * UnitsPerPixel),
            Multiply(Basis.Up, DeltaY * UnitsPerPixel)));
}

void ZoomCamera(OrbitCamera& Camera, const float WheelDelta)
{
    const float Scale = std::exp(-WheelDelta * 0.12F);
    Camera.Distance = std::clamp(
        Camera.Distance * Scale, 1.0F, 100000.0F);
    Camera.OrthographicHalfHeight = std::clamp(
        Camera.OrthographicHalfHeight * Scale, 0.1F, 100000.0F);
}

bool FocusCameraAtPoint(
    OrbitCamera& Camera,
    const Vec3& FocusPoint)
{
    if (!std::isfinite(FocusPoint.X) ||
        !std::isfinite(FocusPoint.Y) ||
        !std::isfinite(FocusPoint.Z))
    {
        return false;
    }
    Camera.Focus = FocusPoint;
    return true;
}

void FollowCameraTargetDelta(
    OrbitCamera& Camera,
    const Vec3& PreviousTarget,
    const Vec3& CurrentTarget)
{
    Camera.Focus = Add(
        Camera.Focus, Subtract(CurrentTarget, PreviousTarget));
}

} // namespace skrtg::viewer
