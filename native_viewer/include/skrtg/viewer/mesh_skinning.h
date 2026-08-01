#pragma once

#include "skrtg/viewer/review_scene.h"

#include <string>
#include <vector>

namespace skrtg::viewer
{
struct MeshSkinningResult
{
    bool Success = false;
    std::vector<std::vector<Vec3>> MeshPositions;
    std::vector<std::string> Errors;
};

MeshSkinningResult SkinMeshPackage(
    const MeshPackage& Package,
    const PoseLane& Pose);

std::vector<std::vector<Vec3>> BuildAnchorAlignedSourceMesh(
    const ReviewScene& Scene,
    const PoseLane& TargetLane,
    const std::vector<std::vector<Vec3>>& SourceMeshPositions);

} // namespace skrtg::viewer
