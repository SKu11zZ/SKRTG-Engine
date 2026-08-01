#pragma once

#include "skrtg/viewer/review_scene.h"

#include <vector>

namespace skrtg::viewer
{
struct Bounds3
{
    Vec3 Minimum;
    Vec3 Maximum;
    bool Valid = false;
};

struct OrbitCamera
{
    Vec3 Focus;
    float YawRadians = 0.0F;
    float PitchRadians = 0.0F;
    float Distance = 420.0F;
    float OrthographicHalfHeight = 135.0F;
    float VerticalFieldOfViewRadians = 0.78539816339F;
    bool Orthographic = false;
};

struct ProjectionViewport
{
    float Width = 1.0F;
    float Height = 1.0F;
};

struct ProjectedPoint
{
    float X = 0.0F;
    float Y = 0.0F;
    float ForwardDepth = 0.0F;
    bool Visible = false;
};

Bounds3 ComputeBounds(const std::vector<Vec3>& Points);

OrbitCamera FitCameraToBounds(
    const Bounds3& Bounds,
    float ViewportAspectRatio);

ProjectedPoint ProjectPoint(
    const OrbitCamera& Camera,
    const ProjectionViewport& Viewport,
    const Vec3& Point);

void RotateCamera(OrbitCamera& Camera, float DeltaX, float DeltaY);

void PanCamera(
    OrbitCamera& Camera,
    float DeltaX,
    float DeltaY,
    float ViewportHeight);

void ZoomCamera(OrbitCamera& Camera, float WheelDelta);

bool FocusCameraAtPoint(
    OrbitCamera& Camera,
    const Vec3& FocusPoint);

void FollowCameraTargetDelta(
    OrbitCamera& Camera,
    const Vec3& PreviousTarget,
    const Vec3& CurrentTarget);

} // namespace skrtg::viewer
