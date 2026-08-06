#pragma once

#include "skrtg/core/math/transform.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::fbx
{
inline constexpr const char* UEFbxImportExactVersion =
    "ue_fbx_import_exact_v1";

struct UEFbxImportExactBone
{
    int Index = -1;
    int ParentIndex = -1;
    std::string Name;
    core::math::TransformRT ReferenceLocal;
    core::math::TransformRT ReferenceModel;
};

struct UEFbxImportExactFrame
{
    int FrameIndex = -1;
    double TimeSeconds = 0.0;
    std::vector<core::math::TransformRT> LocalPose;
    std::vector<core::math::TransformRT> ModelPose;
};

struct UEFbxImportExactMaximum
{
    double Value = 0.0;
    int KeyIndex = -1;
    std::string BoneName;
};

struct UEFbxImportExactValidationMetrics
{
    UEFbxImportExactMaximum LocalTranslationCm;
    UEFbxImportExactMaximum LocalRotationDegrees;
    UEFbxImportExactMaximum LocalScale;
    UEFbxImportExactMaximum ModelTranslationCm;
    UEFbxImportExactMaximum ModelRotationDegrees;
    UEFbxImportExactMaximum ModelScale;
};

struct UEFbxImportExactEvidence
{
    std::string OriginalAxis;
    std::string ConvertedAxis;
    double OriginalUnitScaleFactorCm = 0.0;
    double ConvertedUnitScaleFactorCm = 0.0;
    bool RemoveAllFbxRootsReturned = false;
    bool AxisConversionApplied = false;
    bool UnitConversionApplied = false;
    bool BakeMeshes = false;
    bool UseT0AsReferencePose = false;
    std::string ImportSettingsSource;
    std::string HandednessConversion =
        "FFbxConvert position(x,-y,z), quaternion(x,-y,z,-w), scale unchanged";
};

struct UEFbxImportExactOptions
{
    std::filesystem::path FbxPath;
    std::string FbxExpectedSha256;
    std::filesystem::path AnimationGoldenJsonPath;
    std::string AnimationGoldenJsonExpectedSha256;
    std::string AnimationStackName;
    double OutputSampleRate = 30.0;
    double MaximumDurationSeconds = 60.0 * 60.0;
    double TranslationToleranceCm = 1.0e-3;
    double RotationToleranceDegrees = 1.0e-3;
    double ScaleTolerance = 1.0e-5;
};

struct UEFbxImportExactResult
{
    bool Success = false;
    bool Selected = false;
    bool Adopted = false;
    std::string Version = UEFbxImportExactVersion;
    std::string FbxSha256;
    std::string AnimationGoldenJsonSha256;
    std::string AnimationStackName;
    double FbxRangeStartSeconds = 0.0;
    double DurationSeconds = 0.0;
    UEFbxImportExactEvidence Evidence;
    UEFbxImportExactValidationMetrics GoldenValidation;
    std::vector<UEFbxImportExactBone> Bones;
    std::vector<core::math::TransformRT> BindModelPose;
    std::vector<std::string> BindModelSources;
    int BindPoseBoneCount = 0;
    int SkinClusterBoneCount = 0;
    int EvaluatorFallbackBoneCount = 0;
    // Captured from the already imported scene so downstream callers do not
    // need to import the same animation a second time merely to choose the
    // strict direct-bind audit or the hash-bound Golden fallback.
    bool HasMeshPayload = false;
    bool HasBindPosePayload = false;
    double MaximumBindCandidateTranslationCm = 0.0;
    double MaximumBindCandidateRotationDegrees = 0.0;
    double MaximumBindCandidateScale = 0.0;
    std::vector<UEFbxImportExactFrame> Frames;
    std::vector<std::string> Warnings;
    std::vector<std::string> Errors;
};

// Replays the UE 5.8 Interchange FBX coordinate conversion and validates the
// result against every exported UE animation key before committing any
// sampled frame to the returned result. The v1 contract deliberately fails
// closed on non-identity import offsets, Force Front X Axis, T0-as-reference,
// non-unit animation scale, ambiguous skeleton binding, or incomplete hashes.
UEFbxImportExactResult LoadUEFbxImportExactClip(
    const UEFbxImportExactOptions& Options);
} // namespace skrtg::fbx
