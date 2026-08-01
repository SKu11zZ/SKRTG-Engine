#pragma once

#include "skrtg/core/animation/pose.h"
#include "skrtg/core/math/transform.h"
#include "skrtg/core/skeleton/runtime_skeleton.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::retarget
{
struct UEIKJsonBone
{
    int Index = -1;
    int ParentIndex = -1;
    std::string Name;
    core::math::TransformRT ReferenceLocal;
    core::math::TransformRT ReferenceModel;
    core::math::TransformRT RetargetLocal;
    core::math::TransformRT RetargetModel;
};

struct UEIKJsonChain
{
    std::string Name;
    std::string StartBone;
    std::string EndBone;
    std::string GoalName;
    std::vector<int> BoneIndices;
};

struct UEIKJsonRig
{
    std::string AssetObjectPath;
    std::string AssetName;
    std::string RetargetRootBone;
    std::string RetargetPelvisBone;
    std::vector<UEIKJsonBone> Bones;
    std::vector<UEIKJsonChain> Chains;
};

struct UEIKJsonChainPair
{
    std::string CanonicalChainName;
    std::string SourceChainName;
    std::string TargetChainName;
    std::vector<int> SourceBoneIndices;
    std::vector<int> TargetBoneIndices;
    std::string SourceGoalName;
    std::string TargetGoalName;
    bool EnableFk = true;
    bool EnableIk = false;
    double LengthScale = 1.0;
};

struct UEIKJsonCanonicalBridgeLoadOptions
{
    std::filesystem::path SourceRigJson;
    std::filesystem::path TargetRigJson;
    std::filesystem::path SourceAlignmentRetargeterJson;
    std::filesystem::path TargetAlignmentRetargeterJson;
};

struct UEIKJsonRoute
{
    std::string Schema = "skrtg.ue_ik_json_route.v1";
    std::string RouteId = "ue_ik_json_canonical_bridge_v1";
    std::string FoundationRouteId =
        "ue_ik_json_fk_pelvis_limb_ik_candidate_v1";
    std::string CanonicalRigObjectPath;
    std::string SourcePoseName;
    std::string TargetPoseName;
    UEIKJsonRig SourceRig;
    UEIKJsonRig TargetRig;
    std::vector<UEIKJsonChainPair> ChainPairs;
    int SourceRootIndex = -1;
    int SourcePelvisIndex = -1;
    int TargetRootIndex = -1;
    int TargetPelvisIndex = -1;
    double GlobalTranslationScale = 1.0;
};

struct UEIKJsonRouteLoadResult
{
    bool Success = false;
    UEIKJsonRoute Route;
    std::vector<std::string> Warnings;
    std::vector<std::string> Errors;
};

UEIKJsonRouteLoadResult LoadUEIKJsonCanonicalBridgeRoute(
    const UEIKJsonCanonicalBridgeLoadOptions& Options);

struct UEIKJsonSolveResult
{
    bool Success = false;
    core::animation::PoseBuffer TargetFkLocalPose;
    core::animation::PoseBuffer TargetFkModelPose;
    core::animation::PoseBuffer TargetFoundationLocalPose;
    core::animation::PoseBuffer TargetFoundationModelPose;
    int AppliedIkChainCount = 0;
    int FailedIkChainCount = 0;
    double MaximumIkEndpointErrorCm = 0.0;
    std::vector<std::string> Errors;
};

bool BuildModelPose(
    const core::skeleton::NormalizedRuntimeSkeleton& Skeleton,
    const core::animation::PoseBuffer& LocalPose,
    core::animation::PoseBuffer& OutModelPose);

core::skeleton::NormalizedRuntimeSkeleton BuildTargetRuntimeSkeleton(
    const UEIKJsonRoute& Route);

UEIKJsonSolveResult SolveUEIKJsonRouteFrame(
    const UEIKJsonRoute& Route,
    const std::vector<core::math::TransformRT>& SourceCurrentLocalPose);
} // namespace skrtg::retarget
