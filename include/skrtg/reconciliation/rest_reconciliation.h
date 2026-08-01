#pragma once

#include "skrtg/core/math/transform.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::reconciliation
{
enum class RetargetPoseState
{
    NotRequiredRestPoseMatch,
    TopologyOnlyReuse,
    RestReconciliationRequired,
    RetargetPoseDefinedUnvalidated,
    RestReconciliationValidated,
    BlockedTopologyMismatch,
    BlockedArtifactMismatch,
};

struct ValidationIssue
{
    std::string Severity;
    std::string Label;
    std::string Message;
    std::string Action;
};

struct ReadinessGates
{
    std::string MathExportReadiness = "not_ready";
    bool ExportSmokeReady = false;
    bool RetargetWriteAllowed = false;
    bool SampledLocalPoseCanAffectReadiness = false;
};

struct RestReconciliationInput
{
    bool ArtifactProvenanceMatches = true;
    bool AxisBasisConsistent = true;
    bool TopologyMatch = false;
    bool RestPoseMatch = false;
    bool HasNumericRestDeltaEvidence = false;
    bool RetargetPoseDefined = false;
    bool RetargetPoseValidated = false;
    bool HasRequiredChainBlocker = false;
    std::vector<std::string> RequiredChainBlockers;
};

struct RestReconciliationResult
{
    RetargetPoseState State = RetargetPoseState::BlockedArtifactMismatch;
    ReadinessGates Readiness;
    bool RuntimeSkeletonReuseAllowed = false;
    bool BlindReuseAllowed = false;
    bool RequiresRetargetPoseOrRestReconciliation = true;
    bool FailClosed = true;
    std::vector<ValidationIssue> Issues;
};

struct BasisConversionProvenance
{
    std::string ConversionMethod;
    std::string SourceAxisConvention;
    std::string InternalAxisConvention;
    int ConversionApplicationCount = 1;
    double BasisDeterminantSign = -1.0;
};

struct RetargetLocalDelta
{
    skrtg::core::math::TransformRT Delta;
    skrtg::core::math::Vec3 IsolatedRootPlacementDeltaCm;
};

struct ArtifactGenerationOptions
{
    std::filesystem::path IdentityReportPath;
    std::filesystem::path RestPoseDeltaReportPath;
    std::filesystem::path OutputDirectory;
    std::string PairName;
};

struct RetargetPoseAuthoringOptions
{
    std::filesystem::path IdentityReportPath;
    std::filesystem::path RestPoseDeltaReportPath;
    std::filesystem::path SourceRestPosePath;
    std::filesystem::path CandidateRestPosePath;
    std::filesystem::path PriorReconciliationPath;
    std::filesystem::path OutputDirectory;
    std::string PairName;
};

struct RetargetPoseValidationOptions
{
    std::filesystem::path IdentityReportPath;
    std::filesystem::path RestPoseDeltaReportPath;
    std::filesystem::path SourceRestPosePath;
    std::filesystem::path CandidateRestPosePath;
    std::filesystem::path AuthoredRetargetPosePath;
    std::filesystem::path AuthoredReconciliationPath;
    std::filesystem::path AuthoredValidationPath;
    std::filesystem::path OutputDirectory;
    std::string PairName;
};

struct TextArtifact
{
    std::filesystem::path Path;
    std::string Text;
};

struct ArtifactGenerationResult
{
    bool Success = false;
    RestReconciliationResult Reconciliation;
    std::string ConsoleSummary;
    std::vector<std::string> Warnings;
    std::vector<std::string> Errors;
    std::vector<TextArtifact> Artifacts;
};

const char* ToString(RetargetPoseState State);

skrtg::core::math::TransformRT Inverse(skrtg::core::math::TransformRT Transform);
skrtg::core::math::TransformRT ToLocalTransform(skrtg::core::math::TransformRT ParentModel,
                                                skrtg::core::math::TransformRT ChildModel);
RetargetLocalDelta ComputeRestPoseLocalDelta(skrtg::core::math::TransformRT SourceReferenceRestLocal,
                                             skrtg::core::math::TransformRT CandidateRestLocal,
                                             bool bIsRoot,
                                             bool bIsolateRootPlacement);
skrtg::core::math::TransformRT ApplyRestPoseLocalDelta(skrtg::core::math::TransformRT CandidateRestLocal,
                                                       skrtg::core::math::TransformRT RestPoseDeltaLocal);

bool ValidateBasisConversion(const BasisConversionProvenance& Provenance, std::vector<ValidationIssue>& Issues);
RestReconciliationResult EvaluateRestReconciliation(const RestReconciliationInput& Input);
ArtifactGenerationResult GenerateRestReconciliationDraftArtifacts(const ArtifactGenerationOptions& Options);
ArtifactGenerationResult GenerateRetargetPoseReconciliationArtifacts(const RetargetPoseAuthoringOptions& Options);
ArtifactGenerationResult GenerateRestReconciliationValidationArtifacts(const RetargetPoseValidationOptions& Options);
} // namespace skrtg::reconciliation
