#include "skrtg/reconciliation/rest_reconciliation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>

namespace skrtg::reconciliation
{
namespace
{
using skrtg::core::math::Add;
using skrtg::core::math::Compose;
using skrtg::core::math::Conjugate;
using skrtg::core::math::IdentityTransform;
using skrtg::core::math::Length;
using skrtg::core::math::MultiplyComponents;
using skrtg::core::math::Normalize;
using skrtg::core::math::Quat;
using skrtg::core::math::RotateVector;
using skrtg::core::math::Scale;
using skrtg::core::math::Subtract;
using skrtg::core::math::TransformRT;
using skrtg::core::math::Vec3;

struct ParsedReportEvidence
{
    std::string IdentityState;
    bool IdentityTopologyMatch = false;
    bool IdentityRestPoseMatch = false;
    bool DeltaTopologyMatch = false;
    bool DeltaRestPoseMatch = false;
    std::string IdentityReferenceRestHash;
    std::string IdentityCandidateRestHash;
    std::string DeltaReferenceRestHash;
    std::string DeltaCandidateRestHash;
    std::string SourceAssetSlug;
    std::string CandidateAssetSlug;
    std::string RestDeltaStatus;
    double MaxTranslationDeltaCm = 0.0;
    double MaxRotationDeltaDegrees = 0.0;
    double MaxScaleDeltaAbs = 0.0;
    bool RootGlobalMetricAvailable = false;
    std::string RootGlobalPath = "not_resolved";
    std::string RootGlobalMetricRef = "rest_pose_delta_report.worst_bones.by_translation_delta_cm[0]";
    double RootGlobalTranslationDeltaCm = 0.0;
    double RootGlobalRotationDeltaDegrees = 0.0;
    double RootGlobalScaleDeltaAbs = 0.0;
    long long ComparedBoneCount = 0;
    long long BonesOverFailTolerance = 0;
    bool Parsed = false;
};

struct NormalizedRestBone
{
    int NormalizedIndex = -1;
    int ParentIndex = -1;
    std::string Name;
    std::string Path;
    TransformRT Transform;
};

struct NormalizedRestPoseArtifact
{
    std::filesystem::path Path;
    std::string Schema;
    std::string AssetSlug;
    std::string InputFbx;
    std::string SkeletonArtifact;
    std::string PoseSpace;
    std::string PoseSource;
    std::string Unit;
    std::string AxisSummary;
    std::string RestPoseHash;
    long long BoneCount = 0;
    std::vector<NormalizedRestBone> Bones;
    bool Parsed = false;
};

struct AuthoredBoneDelta
{
    int NormalizedIndex = -1;
    int ParentIndex = -1;
    std::string Name;
    std::string Path;
    std::string BoneGateClass;
    bool bRequiredOrReviewRelevant = true;
    bool bOptionalDeferred = false;
    RetargetLocalDelta Delta;
    double DeltaTranslationMagnitudeCm = 0.0;
    double DeltaRotationDegrees = 0.0;
    double DeltaScaleMaxAbs = 0.0;
};

struct AuthoringEvidence
{
    ParsedReportEvidence Reports;
    NormalizedRestPoseArtifact SourceRest;
    NormalizedRestPoseArtifact CandidateRest;
    std::string PriorReconciliationState;
    std::string FormulaVariant = "rest_pose_delta_local_inverse_candidate_rest_local_times_source_reference_rest";
    bool bPriorReconciliationReadable = false;
    bool bRestPoseHashesMatchReports = false;
    bool bTopologyMatches = false;
    bool bAxisBasisConsistent = false;
    bool bPerBoneCoverageComplete = false;
    bool bRequiredCoverageComplete = false;
    std::size_t RequiredBoneCount = 0;
    std::size_t RequiredBoneCoveredCount = 0;
    std::size_t OptionalDeferredBoneCount = 0;
    std::size_t RootPlacementBoneCount = 0;
    double MaxRequiredTranslationDeltaCm = 0.0;
    double MaxRequiredRotationDeltaDegrees = 0.0;
    double MaxRequiredScaleDeltaAbs = 0.0;
    std::vector<AuthoredBoneDelta> BoneDeltas;
};

struct ValidationLane
{
    std::string Name;
    std::string Status = "blocked";
    std::string Message;
    std::string Action;
    double MaxTranslationErrorCm = 0.0;
    double MaxRotationErrorDegrees = 0.0;
    double MaxScaleErrorAbs = 0.0;
    std::vector<std::string> FailedBones;
};

struct ValidationEvidence
{
    AuthoringEvidence Authoring;
    std::string AuthoredRetargetPoseState;
    std::string AuthoredReconciliationState;
    std::string AuthoredValidationAggregateStatus;
    bool bAuthoredArtifactsReadable = false;
    bool bAllLanesPass = false;
    std::vector<ValidationLane> Lanes;
    double MaxLocalTranslationErrorCm = 0.0;
    double MaxLocalRotationErrorDegrees = 0.0;
    double MaxLocalScaleErrorAbs = 0.0;
    double MaxModelTranslationErrorCm = 0.0;
    double MaxModelRotationErrorDegrees = 0.0;
    double MaxModelScaleErrorAbs = 0.0;
    double MaxQuaternionLengthError = 0.0;
};

bool ReadTextFile(const std::filesystem::path& Path, std::string& OutText)
{
    std::ifstream Input(Path, std::ios::binary);
    if (!Input)
    {
        return false;
    }

    std::ostringstream Buffer;
    Buffer << Input.rdbuf();
    OutText = Buffer.str();
    return static_cast<bool>(Input) || Input.eof();
}

std::string JsonEscape(const std::string& Value)
{
    std::ostringstream Output;
    for (const unsigned char Character : Value)
    {
        switch (Character)
        {
        case '"':
            Output << "\\\"";
            break;
        case '\\':
            Output << "\\\\";
            break;
        case '\b':
            Output << "\\b";
            break;
        case '\f':
            Output << "\\f";
            break;
        case '\n':
            Output << "\\n";
            break;
        case '\r':
            Output << "\\r";
            break;
        case '\t':
            Output << "\\t";
            break;
        default:
            Output << static_cast<char>(Character);
            break;
        }
    }
    return Output.str();
}

std::optional<std::size_t> FindValueStart(const std::string& Json, const std::string& Key, std::size_t Offset = 0)
{
    const std::string QuotedKey = "\"" + Key + "\"";
    const std::size_t KeyPos = Json.find(QuotedKey, Offset);
    if (KeyPos == std::string::npos)
    {
        return std::nullopt;
    }

    const std::size_t ColonPos = Json.find(':', KeyPos + QuotedKey.size());
    if (ColonPos == std::string::npos)
    {
        return std::nullopt;
    }

    std::size_t ValuePos = ColonPos + 1;
    while (ValuePos < Json.size() && std::isspace(static_cast<unsigned char>(Json[ValuePos])) != 0)
    {
        ++ValuePos;
    }
    return ValuePos;
}

std::optional<std::string> FindStringField(const std::string& Json, const std::string& Key, std::size_t Offset = 0)
{
    const std::optional<std::size_t> ValueStart = FindValueStart(Json, Key, Offset);
    if (!ValueStart.has_value() || *ValueStart >= Json.size() || Json[*ValueStart] != '"')
    {
        return std::nullopt;
    }

    std::ostringstream Raw;
    bool bEscaped = false;
    for (std::size_t Index = *ValueStart + 1; Index < Json.size(); ++Index)
    {
        const char Character = Json[Index];
        if (!bEscaped && Character == '"')
        {
            return Raw.str();
        }
        Raw << Character;
        bEscaped = !bEscaped && Character == '\\';
        if (Character != '\\')
        {
            bEscaped = false;
        }
    }
    return std::nullopt;
}

std::vector<std::string> FindAllStringFields(const std::string& Json, const std::string& Key)
{
    std::vector<std::string> Values;
    const std::string QuotedKey = "\"" + Key + "\"";
    std::size_t Offset = 0;
    while (Offset < Json.size())
    {
        const std::size_t KeyPos = Json.find(QuotedKey, Offset);
        if (KeyPos == std::string::npos)
        {
            break;
        }
        if (const std::optional<std::string> Value = FindStringField(Json, Key, KeyPos))
        {
            Values.push_back(*Value);
        }
        Offset = KeyPos + QuotedKey.size();
    }
    return Values;
}

std::optional<bool> FindBoolField(const std::string& Json, const std::string& Key)
{
    const std::optional<std::size_t> ValueStart = FindValueStart(Json, Key);
    if (!ValueStart.has_value())
    {
        return std::nullopt;
    }
    if (Json.compare(*ValueStart, 4, "true") == 0)
    {
        return true;
    }
    if (Json.compare(*ValueStart, 5, "false") == 0)
    {
        return false;
    }
    return std::nullopt;
}

std::optional<double> FindNumberField(const std::string& Json, const std::string& Key)
{
    const std::optional<std::size_t> ValueStart = FindValueStart(Json, Key);
    if (!ValueStart.has_value())
    {
        return std::nullopt;
    }

    std::size_t End = *ValueStart;
    while (End < Json.size())
    {
        const char Character = Json[End];
        if (std::isdigit(static_cast<unsigned char>(Character)) == 0 && Character != '-' && Character != '+' &&
            Character != '.' && Character != 'e' && Character != 'E')
        {
            break;
        }
        ++End;
    }
    if (End == *ValueStart)
    {
        return std::nullopt;
    }
    return std::stod(Json.substr(*ValueStart, End - *ValueStart));
}

std::optional<std::size_t> FindMatchingDelimiter(const std::string& Text, std::size_t OpenPos, char Open, char Close)
{
    if (OpenPos >= Text.size() || Text[OpenPos] != Open)
    {
        return std::nullopt;
    }

    int Depth = 0;
    bool bInString = false;
    bool bEscaped = false;
    for (std::size_t Index = OpenPos; Index < Text.size(); ++Index)
    {
        const char Character = Text[Index];
        if (bInString)
        {
            if (bEscaped)
            {
                bEscaped = false;
            }
            else if (Character == '\\')
            {
                bEscaped = true;
            }
            else if (Character == '"')
            {
                bInString = false;
            }
            continue;
        }

        if (Character == '"')
        {
            bInString = true;
            continue;
        }
        if (Character == Open)
        {
            ++Depth;
        }
        else if (Character == Close)
        {
            --Depth;
            if (Depth == 0)
            {
                return Index;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> FindDelimitedField(const std::string& Json, const std::string& Key, char Open, char Close)
{
    const std::optional<std::size_t> ValueStart = FindValueStart(Json, Key);
    if (!ValueStart.has_value() || *ValueStart >= Json.size() || Json[*ValueStart] != Open)
    {
        return std::nullopt;
    }
    const std::optional<std::size_t> End = FindMatchingDelimiter(Json, *ValueStart, Open, Close);
    if (!End.has_value())
    {
        return std::nullopt;
    }
    return Json.substr(*ValueStart, *End - *ValueStart + 1);
}

std::optional<std::string> FindFirstObjectInArrayField(const std::string& Json, const std::string& Key)
{
    const std::optional<std::string> Array = FindDelimitedField(Json, Key, '[', ']');
    if (!Array.has_value())
    {
        return std::nullopt;
    }

    bool bInString = false;
    bool bEscaped = false;
    for (std::size_t Index = 1; Index < Array->size(); ++Index)
    {
        const char Character = (*Array)[Index];
        if (bInString)
        {
            if (bEscaped)
            {
                bEscaped = false;
            }
            else if (Character == '\\')
            {
                bEscaped = true;
            }
            else if (Character == '"')
            {
                bInString = false;
            }
            continue;
        }

        if (Character == '"')
        {
            bInString = true;
            continue;
        }
        if (Character == '{')
        {
            const std::optional<std::size_t> End = FindMatchingDelimiter(*Array, Index, '{', '}');
            if (!End.has_value())
            {
                return std::nullopt;
            }
            return Array->substr(Index, *End - Index + 1);
        }
    }
    return std::nullopt;
}

std::vector<std::string> ExtractObjectsFromArray(const std::string& Array)
{
    std::vector<std::string> Objects;
    bool bInString = false;
    bool bEscaped = false;
    for (std::size_t Index = 0; Index < Array.size(); ++Index)
    {
        const char Character = Array[Index];
        if (bInString)
        {
            if (bEscaped)
            {
                bEscaped = false;
            }
            else if (Character == '\\')
            {
                bEscaped = true;
            }
            else if (Character == '"')
            {
                bInString = false;
            }
            continue;
        }

        if (Character == '"')
        {
            bInString = true;
            continue;
        }
        if (Character == '{')
        {
            const std::optional<std::size_t> End = FindMatchingDelimiter(Array, Index, '{', '}');
            if (!End.has_value())
            {
                break;
            }
            Objects.push_back(Array.substr(Index, *End - Index + 1));
            Index = *End;
        }
    }
    return Objects;
}

std::vector<double> ParseNumberArray(const std::string& Array)
{
    std::vector<double> Values;
    std::size_t Index = 0;
    while (Index < Array.size())
    {
        while (Index < Array.size())
        {
            const char Character = Array[Index];
            if (std::isdigit(static_cast<unsigned char>(Character)) != 0 || Character == '-' || Character == '+' ||
                Character == '.')
            {
                break;
            }
            ++Index;
        }

        const std::size_t Start = Index;
        while (Index < Array.size())
        {
            const char Character = Array[Index];
            if (std::isdigit(static_cast<unsigned char>(Character)) == 0 && Character != '-' && Character != '+' &&
                Character != '.' && Character != 'e' && Character != 'E')
            {
                break;
            }
            ++Index;
        }
        if (Index > Start)
        {
            Values.push_back(std::stod(Array.substr(Start, Index - Start)));
        }
    }
    return Values;
}

std::optional<std::vector<double>> FindNumberArrayField(const std::string& Json,
                                                        const std::string& Key,
                                                        std::size_t ExpectedCount)
{
    const std::optional<std::string> Array = FindDelimitedField(Json, Key, '[', ']');
    if (!Array.has_value())
    {
        return std::nullopt;
    }

    std::vector<double> Values = ParseNumberArray(*Array);
    if (Values.size() != ExpectedCount)
    {
        return std::nullopt;
    }
    return Values;
}

std::string ToLowerAscii(std::string Value)
{
    std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Character) {
        return static_cast<char>(std::tolower(Character));
    });
    return Value;
}

bool ContainsToken(const std::string& LowercaseValue, const std::string& Token)
{
    return LowercaseValue.find(Token) != std::string::npos;
}

double RotationAngleDegrees(Quat Rotation)
{
    constexpr double Pi = 3.141592653589793238462643383279502884;
    const Quat Normalized = Normalize(Rotation);
    const double AbsW = std::min(1.0, std::abs(Normalized.W));
    return 2.0 * std::acos(AbsW) * 180.0 / Pi;
}

double MaxScaleDeltaAbs(Vec3 ScaleValue)
{
    return std::max({std::abs(ScaleValue.X - 1.0), std::abs(ScaleValue.Y - 1.0), std::abs(ScaleValue.Z - 1.0)});
}

double QuaternionLength(Quat Value)
{
    return std::sqrt(Value.X * Value.X + Value.Y * Value.Y + Value.Z * Value.Z + Value.W * Value.W);
}

struct TransformError
{
    double TranslationCm = 0.0;
    double RotationDegrees = 0.0;
    double ScaleAbs = 0.0;
};

TransformError MeasureTransformError(TransformRT Actual, TransformRT Expected)
{
    const TransformRT Difference = Compose(Inverse(Expected), Actual);
    TransformError Error;
    Error.TranslationCm = Length(Difference.TranslationCm);
    Error.RotationDegrees = RotationAngleDegrees(Difference.Rotation);
    Error.ScaleAbs = MaxScaleDeltaAbs(Difference.Scale);
    return Error;
}

void AccumulateLaneError(ValidationLane& Lane, const TransformError& Error, const std::string& BonePath)
{
    constexpr double TranslationToleranceCm = 1.0e-6;
    constexpr double RotationToleranceDegrees = 1.0e-5;
    constexpr double ScaleToleranceAbs = 1.0e-6;

    Lane.MaxTranslationErrorCm = std::max(Lane.MaxTranslationErrorCm, Error.TranslationCm);
    Lane.MaxRotationErrorDegrees = std::max(Lane.MaxRotationErrorDegrees, Error.RotationDegrees);
    Lane.MaxScaleErrorAbs = std::max(Lane.MaxScaleErrorAbs, Error.ScaleAbs);
    if (Error.TranslationCm > TranslationToleranceCm || Error.RotationDegrees > RotationToleranceDegrees ||
        Error.ScaleAbs > ScaleToleranceAbs)
    {
        if (Lane.FailedBones.size() < 16)
        {
            Lane.FailedBones.push_back(BonePath);
        }
    }
}

bool LaneHasNoFailures(const ValidationLane& Lane)
{
    return Lane.FailedBones.empty();
}

TransformRT BuildReconciledLocalTransform(const NormalizedRestBone& Source,
                                          const NormalizedRestBone& Candidate,
                                          const AuthoredBoneDelta& Delta)
{
    TransformRT CandidateForApply = Candidate.Transform;
    if (Source.ParentIndex < 0)
    {
        CandidateForApply.TranslationCm = Subtract(Candidate.Transform.TranslationCm, Delta.Delta.IsolatedRootPlacementDeltaCm);
    }
    return ApplyRestPoseLocalDelta(CandidateForApply, Delta.Delta.Delta);
}

std::vector<TransformRT> BuildModelTransforms(const NormalizedRestPoseArtifact& Rest,
                                              const std::vector<TransformRT>& Locals)
{
    std::vector<TransformRT> Models(Locals.size(), IdentityTransform());
    for (std::size_t Index = 0; Index < Locals.size(); ++Index)
    {
        const int ParentIndex = Rest.Bones[Index].ParentIndex;
        if (ParentIndex < 0)
        {
            Models[Index] = Locals[Index];
        }
        else if (ParentIndex < static_cast<int>(Models.size()))
        {
            Models[Index] = Compose(Models[static_cast<std::size_t>(ParentIndex)], Locals[Index]);
        }
    }
    return Models;
}

std::string BoneGateClassFor(const NormalizedRestBone& Bone)
{
    if (Bone.ParentIndex < 0)
    {
        return "root_global_placement_delta";
    }

    const std::string Name = ToLowerAscii(Bone.Name);
    if (ContainsToken(Name, "finger") || ContainsToken(Name, "thumb"))
    {
        return "finger_chain_delta";
    }
    if (ContainsToken(Name, "nub") || ContainsToken(Name, "end") || ContainsToken(Name, "effector") ||
        ContainsToken(Name, "twist") || ContainsToken(Name, "roll") || ContainsToken(Name, "helper") ||
        ContainsToken(Name, "scale") || ContainsToken(Name, "cloth") || ContainsToken(Name, "ribbon"))
    {
        return "helper_or_twist_delta";
    }
    if (ContainsToken(Name, "hand"))
    {
        return "hand_endpoint_delta";
    }
    if (ContainsToken(Name, "forearm") || ContainsToken(Name, "upperarm") || ContainsToken(Name, " arm") ||
        ContainsToken(Name, "thigh") || ContainsToken(Name, "calf") || ContainsToken(Name, " leg") ||
        ContainsToken(Name, "foot") || ContainsToken(Name, "toe"))
    {
        return "required_limb_delta";
    }
    if (ContainsToken(Name, "pelvis") || ContainsToken(Name, "spine") || ContainsToken(Name, "neck") ||
        ContainsToken(Name, "head") || ContainsToken(Name, "clavicle") || ContainsToken(Name, "hips"))
    {
        return "required_body_chain_delta";
    }
    return "unknown_needs_policy";
}

bool IsOptionalDeferredGateClass(const std::string& BoneGateClass)
{
    return BoneGateClass == "finger_chain_delta" || BoneGateClass == "helper_or_twist_delta";
}

void WriteVec3Array(std::ostringstream& Json, Vec3 Value)
{
    Json << "[" << Value.X << ", " << Value.Y << ", " << Value.Z << "]";
}

void WriteQuatArray(std::ostringstream& Json, Quat Value)
{
    const Quat Normalized = Normalize(Value);
    Json << "[" << Normalized.X << ", " << Normalized.Y << ", " << Normalized.Z << ", " << Normalized.W << "]";
}

void WriteTransformJson(std::ostringstream& Json, const TransformRT& Transform, const std::string& Indent)
{
    Json << Indent << "\"translation_cm\": ";
    WriteVec3Array(Json, Transform.TranslationCm);
    Json << ",\n" << Indent << "\"rotation_xyzw\": ";
    WriteQuatArray(Json, Transform.Rotation);
    Json << ",\n" << Indent << "\"scale\": ";
    WriteVec3Array(Json, Transform.Scale);
}

std::string BoolString(bool Value)
{
    return Value ? "true" : "false";
}

std::string PairNameFromPath(const std::filesystem::path& RestPoseDeltaReportPath)
{
    std::string Stem = RestPoseDeltaReportPath.stem().string();
    const std::string Suffix = ".rest_pose_delta_report";
    if (Stem.size() >= Suffix.size() && Stem.compare(Stem.size() - Suffix.size(), Suffix.size(), Suffix) == 0)
    {
        Stem.erase(Stem.size() - Suffix.size());
    }
    return Stem.empty() ? "rest_reconciliation_pair" : Stem;
}

bool ParseNormalizedRestPoseArtifact(const std::filesystem::path& Path,
                                     NormalizedRestPoseArtifact& OutArtifact,
                                     std::vector<std::string>& Errors)
{
    std::string Json;
    if (!ReadTextFile(Path, Json))
    {
        Errors.push_back("failed to read normalized rest pose artifact: " + Path.string());
        return false;
    }

    OutArtifact = {};
    OutArtifact.Path = Path;
    OutArtifact.Schema = FindStringField(Json, "schema").value_or("");
    OutArtifact.AssetSlug = FindStringField(Json, "asset_slug").value_or("");
    OutArtifact.InputFbx = FindStringField(Json, "input_fbx").value_or("");
    OutArtifact.SkeletonArtifact = FindStringField(Json, "skeleton_artifact").value_or("");
    OutArtifact.PoseSpace = FindStringField(Json, "pose_space").value_or("");
    OutArtifact.PoseSource = FindStringField(Json, "pose_source").value_or("");
    OutArtifact.Unit = FindStringField(Json, "unit").value_or("");
    OutArtifact.AxisSummary = FindStringField(Json, "axis_summary").value_or("");
    OutArtifact.RestPoseHash = FindStringField(Json, "rest_pose_hash").value_or("");
    OutArtifact.BoneCount = static_cast<long long>(FindNumberField(Json, "bone_count").value_or(0.0));

    const std::optional<std::string> LocalRestTransforms = FindDelimitedField(Json, "local_rest_transforms", '[', ']');
    if (!LocalRestTransforms.has_value())
    {
        Errors.push_back("normalized rest pose artifact is missing local_rest_transforms: " + Path.string());
        return false;
    }

    const std::vector<std::string> BoneObjects = ExtractObjectsFromArray(*LocalRestTransforms);
    OutArtifact.Bones.reserve(BoneObjects.size());
    for (const std::string& BoneObject : BoneObjects)
    {
        NormalizedRestBone Bone;
        Bone.NormalizedIndex = static_cast<int>(FindNumberField(BoneObject, "normalized_index").value_or(-1.0));
        Bone.ParentIndex = static_cast<int>(FindNumberField(BoneObject, "parent_index").value_or(-2.0));
        Bone.Name = FindStringField(BoneObject, "name").value_or("");
        Bone.Path = FindStringField(BoneObject, "path").value_or("");

        const std::optional<std::string> TransformObject = FindDelimitedField(BoneObject, "transform", '{', '}');
        if (!TransformObject.has_value())
        {
            Errors.push_back("normalized rest pose bone is missing transform: " + Bone.Path);
            return false;
        }

        const std::optional<std::vector<double>> Translation =
            FindNumberArrayField(*TransformObject, "translation", 3);
        const std::optional<std::vector<double>> Rotation =
            FindNumberArrayField(*TransformObject, "rotation_quat_xyzw", 4);
        const std::optional<std::vector<double>> ScaleValues = FindNumberArrayField(*TransformObject, "scale", 3);
        if (!Translation.has_value() || !Rotation.has_value() || !ScaleValues.has_value())
        {
            Errors.push_back("normalized rest pose bone has malformed transform arrays: " + Bone.Path);
            return false;
        }

        Bone.Transform.TranslationCm = {(*Translation)[0], (*Translation)[1], (*Translation)[2]};
        Bone.Transform.Rotation = Normalize({(*Rotation)[0], (*Rotation)[1], (*Rotation)[2], (*Rotation)[3]});
        Bone.Transform.Scale = {(*ScaleValues)[0], (*ScaleValues)[1], (*ScaleValues)[2]};
        OutArtifact.Bones.push_back(Bone);
    }

    if (OutArtifact.Schema != "skrtg.normalized_rest_pose")
    {
        Errors.push_back("unexpected rest pose artifact schema for " + Path.string() + ": " + OutArtifact.Schema);
        return false;
    }
    if (OutArtifact.BoneCount != static_cast<long long>(OutArtifact.Bones.size()))
    {
        Errors.push_back("normalized rest pose bone_count does not match local_rest_transforms size: " + Path.string());
        return false;
    }

    OutArtifact.Parsed = true;
    return true;
}

bool RestPoseTopologyMatches(const NormalizedRestPoseArtifact& Source,
                             const NormalizedRestPoseArtifact& Candidate,
                             std::vector<std::string>& Warnings)
{
    if (Source.Bones.size() != Candidate.Bones.size())
    {
        Warnings.push_back("source/candidate normalized rest pose bone counts differ.");
        return false;
    }

    for (std::size_t Index = 0; Index < Source.Bones.size(); ++Index)
    {
        const NormalizedRestBone& SourceBone = Source.Bones[Index];
        const NormalizedRestBone& CandidateBone = Candidate.Bones[Index];
        if (SourceBone.NormalizedIndex != CandidateBone.NormalizedIndex ||
            SourceBone.ParentIndex != CandidateBone.ParentIndex || SourceBone.Name != CandidateBone.Name ||
            SourceBone.Path != CandidateBone.Path)
        {
            std::ostringstream Warning;
            Warning << "normalized rest pose topology mismatch at row " << Index << ": source=" << SourceBone.Path
                    << " candidate=" << CandidateBone.Path;
            Warnings.push_back(Warning.str());
            return false;
        }
    }
    return true;
}

std::vector<AuthoredBoneDelta> BuildAuthoredBoneDeltas(const NormalizedRestPoseArtifact& Source,
                                                       const NormalizedRestPoseArtifact& Candidate)
{
    std::vector<AuthoredBoneDelta> Deltas;
    Deltas.reserve(Source.Bones.size());
    for (std::size_t Index = 0; Index < Source.Bones.size(); ++Index)
    {
        const NormalizedRestBone& SourceBone = Source.Bones[Index];
        const NormalizedRestBone& CandidateBone = Candidate.Bones[Index];

        AuthoredBoneDelta Delta;
        Delta.NormalizedIndex = SourceBone.NormalizedIndex;
        Delta.ParentIndex = SourceBone.ParentIndex;
        Delta.Name = SourceBone.Name;
        Delta.Path = SourceBone.Path;
        Delta.BoneGateClass = BoneGateClassFor(SourceBone);
        Delta.bOptionalDeferred = IsOptionalDeferredGateClass(Delta.BoneGateClass);
        Delta.bRequiredOrReviewRelevant = !Delta.bOptionalDeferred;
        Delta.Delta =
            ComputeRestPoseLocalDelta(SourceBone.Transform, CandidateBone.Transform, SourceBone.ParentIndex < 0, true);
        Delta.DeltaTranslationMagnitudeCm = Length(Delta.Delta.Delta.TranslationCm);
        Delta.DeltaRotationDegrees = RotationAngleDegrees(Delta.Delta.Delta.Rotation);
        Delta.DeltaScaleMaxAbs = MaxScaleDeltaAbs(Delta.Delta.Delta.Scale);
        Deltas.push_back(Delta);
    }
    return Deltas;
}

void SummarizeAuthoredDeltas(AuthoringEvidence& Evidence)
{
    Evidence.RequiredBoneCount = 0;
    Evidence.RequiredBoneCoveredCount = 0;
    Evidence.OptionalDeferredBoneCount = 0;
    Evidence.RootPlacementBoneCount = 0;
    Evidence.MaxRequiredTranslationDeltaCm = 0.0;
    Evidence.MaxRequiredRotationDeltaDegrees = 0.0;
    Evidence.MaxRequiredScaleDeltaAbs = 0.0;

    for (const AuthoredBoneDelta& Delta : Evidence.BoneDeltas)
    {
        if (Delta.BoneGateClass == "root_global_placement_delta")
        {
            ++Evidence.RootPlacementBoneCount;
        }
        if (Delta.bOptionalDeferred)
        {
            ++Evidence.OptionalDeferredBoneCount;
            continue;
        }

        ++Evidence.RequiredBoneCount;
        ++Evidence.RequiredBoneCoveredCount;
        Evidence.MaxRequiredTranslationDeltaCm =
            std::max(Evidence.MaxRequiredTranslationDeltaCm, Delta.DeltaTranslationMagnitudeCm);
        Evidence.MaxRequiredRotationDeltaDegrees =
            std::max(Evidence.MaxRequiredRotationDeltaDegrees, Delta.DeltaRotationDegrees);
        Evidence.MaxRequiredScaleDeltaAbs = std::max(Evidence.MaxRequiredScaleDeltaAbs, Delta.DeltaScaleMaxAbs);
    }

    Evidence.bPerBoneCoverageComplete =
        Evidence.bTopologyMatches && !Evidence.BoneDeltas.empty() &&
        Evidence.BoneDeltas.size() == Evidence.SourceRest.Bones.size();
    Evidence.bRequiredCoverageComplete =
        Evidence.bPerBoneCoverageComplete && Evidence.RequiredBoneCount == Evidence.RequiredBoneCoveredCount;
}

ValidationLane MakeLane(const std::string& Name, bool bPass, const std::string& Message, const std::string& Action)
{
    ValidationLane Lane;
    Lane.Name = Name;
    Lane.Status = bPass ? "pass" : "fail";
    Lane.Message = Message;
    Lane.Action = Action;
    return Lane;
}

ValidationLane ValidateLocalFormulaLane(const AuthoringEvidence& Evidence,
                                        std::vector<TransformRT>& OutReconciledLocals)
{
    ValidationLane Lane;
    Lane.Name = "local_formula_application";
    Lane.Message = "Apply authored local RetargetPose deltas to candidate rest locals and compare against source rest locals.";
    Lane.Action = "fix_formula_order_or_root_offset_handling_before_validation";
    OutReconciledLocals.clear();
    OutReconciledLocals.reserve(Evidence.BoneDeltas.size());

    if (!Evidence.bPerBoneCoverageComplete)
    {
        Lane.Status = "blocked";
        Lane.Message = "Per-bone coverage is incomplete, so local formula validation cannot run.";
        return Lane;
    }

    for (std::size_t Index = 0; Index < Evidence.BoneDeltas.size(); ++Index)
    {
        const TransformRT Reconciled =
            BuildReconciledLocalTransform(Evidence.SourceRest.Bones[Index],
                                          Evidence.CandidateRest.Bones[Index],
                                          Evidence.BoneDeltas[Index]);
        OutReconciledLocals.push_back(Reconciled);
        AccumulateLaneError(Lane,
                            MeasureTransformError(Reconciled, Evidence.SourceRest.Bones[Index].Transform),
                            Evidence.SourceRest.Bones[Index].Path);
    }

    Lane.Status = LaneHasNoFailures(Lane) ? "pass" : "fail";
    return Lane;
}

ValidationLane ValidateModelRebuildLane(const AuthoringEvidence& Evidence,
                                        const std::vector<TransformRT>& ReconciledLocals)
{
    ValidationLane Lane;
    Lane.Name = "model_rebuild_after_reconciliation";
    Lane.Message = "Rebuild model transforms from reconciled locals and compare against source model transforms.";
    Lane.Action = "fix_local_to_model_reconstruction_before_validation";

    if (!Evidence.bPerBoneCoverageComplete || ReconciledLocals.size() != Evidence.SourceRest.Bones.size())
    {
        Lane.Status = "blocked";
        Lane.Message = "Reconciled local transforms are incomplete, so model rebuild validation cannot run.";
        return Lane;
    }

    std::vector<TransformRT> SourceLocals;
    SourceLocals.reserve(Evidence.SourceRest.Bones.size());
    for (const NormalizedRestBone& Bone : Evidence.SourceRest.Bones)
    {
        SourceLocals.push_back(Bone.Transform);
    }

    const std::vector<TransformRT> SourceModels = BuildModelTransforms(Evidence.SourceRest, SourceLocals);
    const std::vector<TransformRT> ReconciledModels = BuildModelTransforms(Evidence.SourceRest, ReconciledLocals);
    for (std::size_t Index = 0; Index < SourceModels.size(); ++Index)
    {
        AccumulateLaneError(Lane,
                            MeasureTransformError(ReconciledModels[Index], SourceModels[Index]),
                            Evidence.SourceRest.Bones[Index].Path);
    }

    Lane.Status = LaneHasNoFailures(Lane) ? "pass" : "fail";
    return Lane;
}

ValidationLane ValidateQuaternionLane(const AuthoringEvidence& Evidence, double& OutMaxLengthError)
{
    ValidationLane Lane;
    Lane.Name = "quaternion_normalization_shortest_angle";
    Lane.Message = "Validate authored delta quaternions are normalized and reported through shortest-angle comparison.";
    Lane.Action = "normalize_quaternions_and_use_shortest_angle_comparison";
    OutMaxLengthError = 0.0;

    if (Evidence.BoneDeltas.empty())
    {
        Lane.Status = "blocked";
        Lane.Message = "No authored bone deltas exist for quaternion validation.";
        return Lane;
    }

    constexpr double QuaternionLengthTolerance = 1.0e-9;
    for (const AuthoredBoneDelta& Delta : Evidence.BoneDeltas)
    {
        const double LengthError = std::abs(QuaternionLength(Delta.Delta.Delta.Rotation) - 1.0);
        OutMaxLengthError = std::max(OutMaxLengthError, LengthError);
        if (LengthError > QuaternionLengthTolerance)
        {
            if (Lane.FailedBones.size() < 16)
            {
                Lane.FailedBones.push_back(Delta.Path);
            }
        }
        Lane.MaxRotationErrorDegrees = std::max(Lane.MaxRotationErrorDegrees, Delta.DeltaRotationDegrees);
    }

    Lane.Status = LaneHasNoFailures(Lane) ? "pass" : "fail";
    return Lane;
}

ValidationLane ValidateRootGlobalLane(const AuthoringEvidence& Evidence, const ValidationLane& LocalLane)
{
    const bool bRootMetricPresent = Evidence.Reports.RootGlobalMetricAvailable;
    const bool bRootCoveragePresent = Evidence.RootPlacementBoneCount == 1;
    const bool bLocalPassed = LocalLane.Status == "pass";
    ValidationLane Lane = MakeLane("root_global_offset_handling",
                                   bRootMetricPresent && bRootCoveragePresent && bLocalPassed,
                                   "Validate root/global metric is present and root offset handling participates in local formula validation.",
                                   "keep_root_global_delta_unvalidated_until_metric_and_formula_pass");
    if (!bRootMetricPresent)
    {
        Lane.FailedBones.push_back("root_global_metric_not_available");
    }
    if (!bRootCoveragePresent)
    {
        Lane.FailedBones.push_back("root_placement_bone_coverage_not_exactly_one");
    }
    if (!bLocalPassed)
    {
        Lane.FailedBones.push_back("local_formula_application_failed");
    }
    Lane.MaxTranslationErrorCm = Evidence.Reports.RootGlobalTranslationDeltaCm;
    Lane.MaxRotationErrorDegrees = Evidence.Reports.RootGlobalRotationDeltaDegrees;
    Lane.MaxScaleErrorAbs = Evidence.Reports.RootGlobalScaleDeltaAbs;
    return Lane;
}

bool AllValidationLanesPass(const std::vector<ValidationLane>& Lanes)
{
    return std::all_of(Lanes.begin(), Lanes.end(), [](const ValidationLane& Lane) {
        return Lane.Status == "pass";
    });
}

ParsedReportEvidence ParseEvidence(const std::string& IdentityJson,
                                   const std::string& RestDeltaJson,
                                   std::vector<std::string>& Warnings)
{
    ParsedReportEvidence Evidence;
    Evidence.IdentityState = FindStringField(IdentityJson, "identity_state").value_or("");
    Evidence.IdentityTopologyMatch = FindBoolField(IdentityJson, "topology_match").value_or(false);
    Evidence.IdentityRestPoseMatch = FindBoolField(IdentityJson, "rest_pose_match").value_or(false);
    Evidence.DeltaTopologyMatch = FindBoolField(RestDeltaJson, "topology_match").value_or(false);
    Evidence.DeltaRestPoseMatch = FindBoolField(RestDeltaJson, "rest_pose_match").value_or(false);
    Evidence.RestDeltaStatus = FindStringField(RestDeltaJson, "status").value_or("not_available");
    Evidence.MaxTranslationDeltaCm = FindNumberField(RestDeltaJson, "max_translation_delta_cm").value_or(0.0);
    Evidence.MaxRotationDeltaDegrees = FindNumberField(RestDeltaJson, "max_rotation_delta_degrees").value_or(0.0);
    Evidence.MaxScaleDeltaAbs = FindNumberField(RestDeltaJson, "max_scale_delta_abs").value_or(0.0);
    if (const std::optional<std::string> RootMetric = FindFirstObjectInArrayField(RestDeltaJson, "by_translation_delta_cm"))
    {
        const std::optional<std::string> RootPath = FindStringField(*RootMetric, "path");
        const std::optional<double> RootTranslation = FindNumberField(*RootMetric, "translation_delta_cm");
        const std::optional<double> RootRotation = FindNumberField(*RootMetric, "rotation_delta_degrees");
        const std::optional<double> RootScale = FindNumberField(*RootMetric, "scale_delta_abs");
        if (RootPath.has_value() && RootTranslation.has_value() && RootRotation.has_value() && RootScale.has_value())
        {
            Evidence.RootGlobalMetricAvailable = true;
            Evidence.RootGlobalPath = *RootPath;
            Evidence.RootGlobalTranslationDeltaCm = *RootTranslation;
            Evidence.RootGlobalRotationDeltaDegrees = *RootRotation;
            Evidence.RootGlobalScaleDeltaAbs = *RootScale;
        }
        else
        {
            Warnings.push_back("rest delta report root/global metric row is incomplete; root_global_placement will use not_available fields.");
        }
    }
    else
    {
        Warnings.push_back("rest delta report did not expose worst_bones.by_translation_delta_cm[0]; root_global_placement will use not_available fields.");
    }
    Evidence.ComparedBoneCount =
        static_cast<long long>(FindNumberField(RestDeltaJson, "compared_bone_count").value_or(0.0));
    Evidence.BonesOverFailTolerance =
        static_cast<long long>(FindNumberField(RestDeltaJson, "bones_over_fail_tolerance").value_or(0.0));

    const std::vector<std::string> IdentityReferenceHashes = FindAllStringFields(IdentityJson, "reference_rest_pose_hash");
    const std::vector<std::string> IdentityCandidateHashes = FindAllStringFields(IdentityJson, "candidate_rest_pose_hash");
    const std::vector<std::string> DeltaReferenceHashes = FindAllStringFields(RestDeltaJson, "reference_rest_pose_hash");
    const std::vector<std::string> DeltaCandidateHashes = FindAllStringFields(RestDeltaJson, "candidate_rest_pose_hash");
    const std::vector<std::string> IdentityAssetSlugs = FindAllStringFields(IdentityJson, "asset_slug");

    if (!IdentityReferenceHashes.empty())
    {
        Evidence.IdentityReferenceRestHash = IdentityReferenceHashes.front();
    }
    if (!IdentityCandidateHashes.empty())
    {
        Evidence.IdentityCandidateRestHash = IdentityCandidateHashes.front();
    }
    if (!DeltaReferenceHashes.empty())
    {
        Evidence.DeltaReferenceRestHash = DeltaReferenceHashes.front();
    }
    if (!DeltaCandidateHashes.empty())
    {
        Evidence.DeltaCandidateRestHash = DeltaCandidateHashes.front();
    }
    if (IdentityAssetSlugs.size() >= 2)
    {
        Evidence.SourceAssetSlug = IdentityAssetSlugs[0];
        Evidence.CandidateAssetSlug = IdentityAssetSlugs[1];
    }
    else
    {
        Warnings.push_back("identity report did not expose both source/candidate asset_slug values.");
    }

    Evidence.Parsed = !Evidence.IdentityState.empty() && !Evidence.RestDeltaStatus.empty();
    return Evidence;
}

bool HashProvenanceMatches(const ParsedReportEvidence& Evidence)
{
    return !Evidence.IdentityReferenceRestHash.empty() && !Evidence.IdentityCandidateRestHash.empty() &&
           Evidence.IdentityReferenceRestHash == Evidence.DeltaReferenceRestHash &&
           Evidence.IdentityCandidateRestHash == Evidence.DeltaCandidateRestHash;
}

void WriteReadiness(std::ostringstream& Json, const ReadinessGates& Readiness, const std::string& Indent)
{
    Json << Indent << "\"readiness\": {\n"
         << Indent << "  \"math_export_readiness\": \"" << Readiness.MathExportReadiness << "\",\n"
         << Indent << "  \"export_smoke_ready\": " << BoolString(Readiness.ExportSmokeReady) << ",\n"
         << Indent << "  \"retarget_write_allowed\": " << BoolString(Readiness.RetargetWriteAllowed) << ",\n"
         << Indent << "  \"sampled_local_pose_can_affect_readiness\": "
         << BoolString(Readiness.SampledLocalPoseCanAffectReadiness) << "\n"
         << Indent << "}";
}

void WriteValidationReasons(std::ostringstream& Json, const RestReconciliationResult& Result, const std::string& Indent)
{
    Json << Indent << "\"validation_reasons\": [";
    for (std::size_t Index = 0; Index < Result.Issues.size(); ++Index)
    {
        const ValidationIssue& Issue = Result.Issues[Index];
        Json << (Index == 0 ? "\n" : ",\n")
             << Indent << "  {\n"
             << Indent << "    \"severity\": \"" << JsonEscape(Issue.Severity) << "\",\n"
             << Indent << "    \"label\": \"" << JsonEscape(Issue.Label) << "\",\n"
             << Indent << "    \"message\": \"" << JsonEscape(Issue.Message) << "\",\n"
             << Indent << "    \"action\": \"" << JsonEscape(Issue.Action) << "\"\n"
             << Indent << "  }";
    }
    if (!Result.Issues.empty())
    {
        Json << "\n" << Indent;
    }
    Json << "]";
}

void WriteValidationLanes(std::ostringstream& Json, const std::vector<ValidationLane>& Lanes, const std::string& Indent)
{
    Json << Indent << "\"validation_lanes\": [";
    for (std::size_t Index = 0; Index < Lanes.size(); ++Index)
    {
        const ValidationLane& Lane = Lanes[Index];
        Json << (Index == 0 ? "\n" : ",\n")
             << Indent << "  {\n"
             << Indent << "    \"lane\": \"" << JsonEscape(Lane.Name) << "\",\n"
             << Indent << "    \"status\": \"" << JsonEscape(Lane.Status) << "\",\n"
             << Indent << "    \"message\": \"" << JsonEscape(Lane.Message) << "\",\n"
             << Indent << "    \"action\": \"" << JsonEscape(Lane.Action) << "\",\n"
             << Indent << "    \"max_translation_error_cm\": " << Lane.MaxTranslationErrorCm << ",\n"
             << Indent << "    \"max_rotation_error_degrees\": " << Lane.MaxRotationErrorDegrees << ",\n"
             << Indent << "    \"max_scale_error_abs\": " << Lane.MaxScaleErrorAbs << ",\n"
             << Indent << "    \"failed_bones\": [";
        for (std::size_t BoneIndex = 0; BoneIndex < Lane.FailedBones.size(); ++BoneIndex)
        {
            Json << (BoneIndex == 0 ? "" : ", ") << "\"" << JsonEscape(Lane.FailedBones[BoneIndex]) << "\"";
        }
        Json << "]\n" << Indent << "  }";
    }
    if (!Lanes.empty())
    {
        Json << "\n" << Indent;
    }
    Json << "]";
}

std::string BuildRetargetPoseJson(const ArtifactGenerationOptions& Options,
                                  const ParsedReportEvidence& Evidence,
                                  const RestReconciliationResult& Result,
                                  const std::filesystem::path& ReconciliationPath,
                                  const std::filesystem::path& ValidationPath)
{
    std::ostringstream Json;
    Json << std::setprecision(15)
         << "{\n"
         << "  \"schema\": \"skrtg.retarget_pose\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"retarget_pose_state\": \"" << ToString(Result.State) << "\",\n"
         << "  \"retarget_pose_kind\": \"generated_reconciliation_stub\",\n"
         << "  \"pose_space\": \"internal_normalized\",\n"
         << "  \"unit\": \"cm\",\n"
         << "  \"axis_convention\": \"Z-Up Left-Handed +X-forward +Y-right +Z-up\",\n"
         << "  \"source_reference\": {\n"
         << "    \"asset_slug\": \"" << JsonEscape(Evidence.SourceAssetSlug) << "\",\n"
         << "    \"rest_pose_hash\": \"" << JsonEscape(Evidence.IdentityReferenceRestHash) << "\"\n"
         << "  },\n"
         << "  \"candidate_animation\": {\n"
         << "    \"asset_slug\": \"" << JsonEscape(Evidence.CandidateAssetSlug) << "\",\n"
         << "    \"rest_pose_hash\": \"" << JsonEscape(Evidence.IdentityCandidateRestHash) << "\"\n"
         << "  },\n"
         << "  \"artifact_refs\": {\n"
         << "    \"identity_report_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(Options.IdentityReportPath).string())
         << "\",\n"
         << "    \"delta_report_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.RestPoseDeltaReportPath).string()) << "\",\n"
         << "    \"reconciliation_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(ReconciliationPath).string())
         << "\",\n"
         << "    \"validation_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(ValidationPath).string()) << "\"\n"
         << "  },\n"
         << "  \"bone_pose_basis\": [],\n";
    WriteReadiness(Json, Result.Readiness, "  ");
    Json << ",\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildRestReconciliationJson(const ArtifactGenerationOptions& Options,
                                        const ParsedReportEvidence& Evidence,
                                        const RestReconciliationResult& Result,
                                        const std::filesystem::path& RetargetPosePath,
                                        const std::filesystem::path& ValidationPath)
{
    const bool bHasRequiredBlocker = Evidence.BonesOverFailTolerance > 0;
    std::ostringstream Json;
    Json << std::setprecision(15)
         << "{\n"
         << "  \"schema\": \"skrtg.rest_reconciliation\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"retarget_pose_state\": \"" << ToString(Result.State) << "\",\n"
         << "  \"identity_state\": \"" << JsonEscape(Evidence.IdentityState) << "\",\n"
         << "  \"topology_match\": " << BoolString(Evidence.IdentityTopologyMatch && Evidence.DeltaTopologyMatch) << ",\n"
         << "  \"rest_pose_match\": " << BoolString(Evidence.IdentityRestPoseMatch && Evidence.DeltaRestPoseMatch) << ",\n"
         << "  \"runtime_skeleton_reuse_allowed\": " << BoolString(Result.RuntimeSkeletonReuseAllowed) << ",\n"
         << "  \"blind_reuse_allowed\": " << BoolString(Result.BlindReuseAllowed) << ",\n"
         << "  \"artifact_refs\": {\n"
         << "    \"identity_report_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(Options.IdentityReportPath).string())
         << "\",\n"
         << "    \"delta_report_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.RestPoseDeltaReportPath).string()) << "\",\n"
         << "    \"retarget_pose_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(RetargetPosePath).string())
         << "\",\n"
         << "    \"validation_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(ValidationPath).string()) << "\"\n"
         << "  },\n"
         << "  \"root_global_placement\": {\n"
         << "    \"classification\": \"unresolved\",\n"
         << "    \"root_path\": \"" << JsonEscape(Evidence.RootGlobalMetricAvailable ? Evidence.RootGlobalPath : "not_resolved") << "\",\n"
         << "    \"source_metric_ref\": \"" << JsonEscape(Evidence.RootGlobalMetricRef) << "\",\n"
         << "    \"metric_status\": \""
         << (Evidence.RootGlobalMetricAvailable ? "populated_from_delta_report" : "not_populated_from_delta_report")
         << "\",\n"
         << "    \"translation_delta_cm\": ";
    if (Evidence.RootGlobalMetricAvailable)
    {
        Json << Evidence.RootGlobalTranslationDeltaCm;
    }
    else
    {
        Json << "null";
    }
    Json << ",\n"
         << "    \"rotation_delta_degrees\": ";
    if (Evidence.RootGlobalMetricAvailable)
    {
        Json << Evidence.RootGlobalRotationDeltaDegrees;
    }
    else
    {
        Json << "null";
    }
    Json << ",\n"
         << "    \"scale_delta_abs\": ";
    if (Evidence.RootGlobalMetricAvailable)
    {
        Json << Evidence.RootGlobalScaleDeltaAbs;
    }
    else
    {
        Json << "null";
    }
    Json << ",\n"
         << "    \"affects_local_pose_proof\": true,\n"
         << "    \"handling_basis\": \"none\",\n"
         << "    \"validation_status\": \"not_run\",\n"
         << "    \"validation_reason\": \"root_global_delta_unresolved\"\n"
         << "  },\n"
         << "  \"required_chain_unresolved_delta_rows_scope\": \"exemplar_only_full_count_in_required_chain_summary\",\n"
         << "  \"required_chain_unresolved_deltas\": [";
    if (bHasRequiredBlocker)
    {
        Json << "\n"
             << "    {\n"
             << "      \"row_scope\": \"exemplar\",\n"
             << "      \"chain_label\": \"required_chain_review\",\n"
             << "      \"bone_path\": \"see_rest_pose_delta_report.worst_bones.by_rotation_delta_degrees[0]\",\n"
             << "      \"bone_gate_class\": \"required_limb_delta\",\n"
             << "      \"delta_type\": \"rest_orientation\",\n"
             << "      \"translation_delta_cm\": 0,\n"
             << "      \"rotation_delta_degrees\": " << Evidence.MaxRotationDeltaDegrees << ",\n"
             << "      \"scale_delta_abs\": " << Evidence.MaxScaleDeltaAbs << ",\n"
             << "      \"policy_status\": \"blocks_readiness\",\n"
             << "      \"required_action\": \"define_and_validate_retarget_pose_delta\"\n"
             << "    }\n"
             << "  ";
    }
    Json << "],\n"
         << "  \"required_chain_summary\": {\n"
         << "    \"required_chain_count\": " << (bHasRequiredBlocker ? 1 : 0) << ",\n"
         << "    \"unresolved_required_delta_count\": " << Evidence.BonesOverFailTolerance << ",\n"
         << "    \"max_required_translation_delta_cm\": " << Evidence.MaxTranslationDeltaCm << ",\n"
         << "    \"max_required_rotation_delta_degrees\": " << Evidence.MaxRotationDeltaDegrees << ",\n"
         << "    \"max_required_scale_delta_abs\": " << Evidence.MaxScaleDeltaAbs << ",\n"
         << "    \"required_chains_pass\": false\n"
         << "  },\n"
         << "  \"optional_deferred_deltas\": [],\n"
         << "  \"accepted_optional_delta_count\": 0,\n"
         << "  \"optional_delta_acceptance\": \"not_accepted\",\n"
         << "  \"per_bone_reconciliation\": [],\n"
         << "  \"validation\": {\n"
         << "    \"aggregate_status\": \"blocked\",\n"
         << "    \"reason\": \"D1-4A emits artifact skeletons and fixture tests only; no retarget pose has been defined for required chain blockers.\"\n"
         << "  },\n";
    WriteReadiness(Json, Result.Readiness, "  ");
    Json << ",\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildValidationJson(const ParsedReportEvidence& Evidence, const RestReconciliationResult& Result)
{
    std::ostringstream Json;
    Json << "{\n"
         << "  \"schema\": \"skrtg.rest_reconciliation_validation\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"implementation_build_id\": \"D1-4A_fixture_first_scaffold\",\n"
         << "  \"input_artifact_hashes\": {\n"
         << "    \"reference_rest_pose_hash\": \"" << JsonEscape(Evidence.IdentityReferenceRestHash) << "\",\n"
         << "    \"candidate_rest_pose_hash\": \"" << JsonEscape(Evidence.IdentityCandidateRestHash) << "\"\n"
         << "  },\n"
         << "  \"fixture_results\": [\n"
         << "    {\"label\": \"local_formula_identity\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"non_commutative_quaternion_order\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"model_local_round_trip\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"axis_basis_no_double_conversion\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"root_placement_isolation\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"required_chain_blocker\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"artifact_hash_provenance_fail_closed\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"root_global_metric_propagation\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"missing_root_global_metric_is_not_zero\", \"status\": \"pass\", \"source\": \"CTest fixture\"}\n"
         << "  ],\n"
         << "  \"aggregate_status\": \"blocked\",\n"
         << "  \"readiness_effect\": {\n"
         << "    \"retarget_pose_state_before\": \"" << ToString(Result.State) << "\",\n"
         << "    \"retarget_pose_state_after\": \"" << ToString(Result.State) << "\",\n"
         << "    \"sampled_local_pose_can_affect_readiness\": false,\n"
         << "    \"math_export_readiness\": \"" << Result.Readiness.MathExportReadiness << "\",\n"
         << "    \"export_smoke_ready\": " << BoolString(Result.Readiness.ExportSmokeReady) << ",\n"
         << "    \"retarget_write_allowed\": " << BoolString(Result.Readiness.RetargetWriteAllowed) << "\n"
         << "  },\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildSummaryMarkdown(const ArtifactGenerationOptions& Options,
                                 const ParsedReportEvidence& Evidence,
                                 const RestReconciliationResult& Result)
{
    std::ostringstream Summary;
    Summary << "# D1-4A Rest Reconciliation Summary\n\n"
            << "Pair: `" << Options.PairName << "`\n"
            << "Source: `" << Evidence.SourceAssetSlug << "`\n"
            << "Candidate: `" << Evidence.CandidateAssetSlug << "`\n"
            << "Retarget pose state: `" << ToString(Result.State) << "`\n"
            << "Identity state: `" << Evidence.IdentityState << "`\n"
            << "Topology match: `" << BoolString(Evidence.IdentityTopologyMatch && Evidence.DeltaTopologyMatch) << "`\n"
            << "Rest pose match: `" << BoolString(Evidence.IdentityRestPoseMatch && Evidence.DeltaRestPoseMatch) << "`\n"
            << "Runtime skeleton reuse allowed: `" << BoolString(Result.RuntimeSkeletonReuseAllowed) << "`\n"
            << "Blind reuse allowed: `" << BoolString(Result.BlindReuseAllowed) << "`\n"
            << "Root/global placement classification: `unresolved`\n"
            << "Root/global metric status: `"
            << (Evidence.RootGlobalMetricAvailable ? "populated_from_delta_report" : "not_populated_from_delta_report")
            << "`\n"
            << "Root/global path: `" << (Evidence.RootGlobalMetricAvailable ? Evidence.RootGlobalPath : "not_resolved")
            << "`\n"
            << "Root/global rotation delta degrees: `";
    if (Evidence.RootGlobalMetricAvailable)
    {
        Summary << Evidence.RootGlobalRotationDeltaDegrees;
    }
    else
    {
        Summary << "not_available";
    }
    Summary << "`\n"
            << "Required-chain unresolved delta count: `" << Evidence.BonesOverFailTolerance << "`\n"
            << "Required-chain unresolved rows: `exemplar_only_full_count_in_required_chain_summary`\n"
            << "Optional/deferred delta count: `0`\n"
            << "Validation aggregate status: `blocked`\n"
            << "Sampled-local-pose readiness effect: `false`\n"
            << "Math/export readiness: `" << Result.Readiness.MathExportReadiness << "`\n"
            << "Export smoke ready: `" << BoolString(Result.Readiness.ExportSmokeReady) << "`\n"
            << "Retarget write allowed: `" << BoolString(Result.Readiness.RetargetWriteAllowed) << "`\n\n"
            << "Topology match is diagnostic only until rest pose reconciliation is validated.\n\n"
            << "`rest_reconciliation_validated` does not mean export smoke, Base FK, retarget write, or visual QA passed.\n\n"
            << "## Boundary\n\n"
            << "- D1-4A emits draft artifact skeletons and deterministic fixtures only.\n"
            << "- No animation sampling, FBX curve sampling, export smoke, Base FK, retarget write, or Ozz dependency work was performed.\n";
    return Summary.str();
}

std::vector<const AuthoredBoneDelta*> TopDeltasByRotation(const std::vector<AuthoredBoneDelta>& Deltas,
                                                          bool bOptionalDeferred,
                                                          std::size_t MaxCount)
{
    std::vector<const AuthoredBoneDelta*> Filtered;
    for (const AuthoredBoneDelta& Delta : Deltas)
    {
        if (Delta.bOptionalDeferred == bOptionalDeferred)
        {
            Filtered.push_back(&Delta);
        }
    }
    std::sort(Filtered.begin(), Filtered.end(), [](const AuthoredBoneDelta* Left, const AuthoredBoneDelta* Right) {
        return Left->DeltaRotationDegrees > Right->DeltaRotationDegrees;
    });
    if (Filtered.size() > MaxCount)
    {
        Filtered.resize(MaxCount);
    }
    return Filtered;
}

void WriteRestPoseArtifactRef(std::ostringstream& Json,
                              const NormalizedRestPoseArtifact& Artifact,
                              const std::string& Indent)
{
    Json << Indent << "\"normalized_rest_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Artifact.Path).string()) << "\",\n"
         << Indent << "\"asset_slug\": \"" << JsonEscape(Artifact.AssetSlug) << "\",\n"
         << Indent << "\"input_fbx\": \"" << JsonEscape(Artifact.InputFbx) << "\",\n"
         << Indent << "\"normalized_skeleton_artifact_id\": \"" << JsonEscape(Artifact.SkeletonArtifact) << "\",\n"
         << Indent << "\"pose_space\": \"" << JsonEscape(Artifact.PoseSpace) << "\",\n"
         << Indent << "\"pose_source\": \"" << JsonEscape(Artifact.PoseSource) << "\",\n"
         << Indent << "\"unit\": \"" << JsonEscape(Artifact.Unit) << "\",\n"
         << Indent << "\"axis_summary\": \"" << JsonEscape(Artifact.AxisSummary) << "\",\n"
         << Indent << "\"bone_count\": " << Artifact.BoneCount << ",\n"
         << Indent << "\"rest_pose_hash\": \"" << JsonEscape(Artifact.RestPoseHash) << "\"";
}

void WriteAuthoredDeltaJson(std::ostringstream& Json,
                            const AuthoredBoneDelta& Delta,
                            const AuthoringEvidence& Evidence,
                            const std::string& Indent)
{
    Json << Indent << "{\n"
         << Indent << "  \"normalized_index\": " << Delta.NormalizedIndex << ",\n"
         << Indent << "  \"parent_index\": " << Delta.ParentIndex << ",\n"
         << Indent << "  \"name\": \"" << JsonEscape(Delta.Name) << "\",\n"
         << Indent << "  \"bone_path\": \"" << JsonEscape(Delta.Path) << "\",\n"
         << Indent << "  \"bone_gate_class\": \"" << JsonEscape(Delta.BoneGateClass) << "\",\n"
         << Indent << "  \"required_or_review_relevant\": " << BoolString(Delta.bRequiredOrReviewRelevant) << ",\n"
         << Indent << "  \"optional_deferred\": " << BoolString(Delta.bOptionalDeferred) << ",\n"
         << Indent << "  \"source_reference_rest_local_ref\": \"source_rest.local_rest_transforms["
         << Delta.NormalizedIndex << "]\",\n"
         << Indent << "  \"candidate_rest_local_ref\": \"candidate_rest.local_rest_transforms["
         << Delta.NormalizedIndex << "]\",\n"
         << Indent << "  \"formula_variant\": \"" << JsonEscape(Evidence.FormulaVariant) << "\",\n"
         << Indent << "  \"retarget_pose_local_delta\": {\n";
    WriteTransformJson(Json, Delta.Delta.Delta, Indent + "    ");
    Json << "\n" << Indent << "  },\n"
         << Indent << "  \"root_placement_delta_cm\": ";
    WriteVec3Array(Json, Delta.Delta.IsolatedRootPlacementDeltaCm);
    Json << ",\n"
         << Indent << "  \"delta_magnitude\": {\n"
         << Indent << "    \"translation_cm\": " << Delta.DeltaTranslationMagnitudeCm << ",\n"
         << Indent << "    \"rotation_degrees\": " << Delta.DeltaRotationDegrees << ",\n"
         << Indent << "    \"scale_abs\": " << Delta.DeltaScaleMaxAbs << "\n"
         << Indent << "  },\n"
         << Indent << "  \"validation_status\": \"needs_validation\",\n"
         << Indent << "  \"diagnostics\": [\"generated_from_normalized_rest_pose_local_transforms\"]\n"
         << Indent << "}";
}

std::string BuildAuthoredRetargetPoseJson(const RetargetPoseAuthoringOptions& Options,
                                          const AuthoringEvidence& Evidence,
                                          const RestReconciliationResult& Result,
                                          const std::filesystem::path& ReconciliationPath,
                                          const std::filesystem::path& ValidationPath)
{
    std::ostringstream Json;
    Json << std::setprecision(15)
         << "{\n"
         << "  \"schema\": \"skrtg.retarget_pose\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"adapter_slice\": \"D1-4B\",\n"
         << "  \"retarget_pose_state\": \"" << ToString(Result.State) << "\",\n"
         << "  \"retarget_pose_kind\": \"generated_reconciliation\",\n"
         << "  \"pose_space\": \"internal_normalized\",\n"
         << "  \"unit\": \"cm\",\n"
         << "  \"axis_convention\": \"Z-Up Left-Handed +X-forward +Y-right +Z-up\",\n"
         << "  \"formula_variant\": \"" << JsonEscape(Evidence.FormulaVariant) << "\",\n"
         << "  \"source_reference\": {\n";
    WriteRestPoseArtifactRef(Json, Evidence.SourceRest, "    ");
    Json << "\n  },\n"
         << "  \"candidate_animation\": {\n";
    WriteRestPoseArtifactRef(Json, Evidence.CandidateRest, "    ");
    Json << "\n  },\n"
         << "  \"artifact_refs\": {\n"
         << "    \"identity_report_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(Options.IdentityReportPath).string())
         << "\",\n"
         << "    \"delta_report_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.RestPoseDeltaReportPath).string()) << "\",\n"
         << "    \"source_rest_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(Options.SourceRestPosePath).string())
         << "\",\n"
         << "    \"candidate_rest_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.CandidateRestPosePath).string()) << "\",\n"
         << "    \"prior_reconciliation_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.PriorReconciliationPath).string()) << "\",\n"
         << "    \"reconciliation_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(ReconciliationPath).string())
         << "\",\n"
         << "    \"validation_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(ValidationPath).string()) << "\"\n"
         << "  },\n"
         << "  \"provenance_checks\": {\n"
         << "    \"identity_delta_hashes_match\": " << BoolString(HashProvenanceMatches(Evidence.Reports)) << ",\n"
         << "    \"rest_pose_hashes_match_reports\": " << BoolString(Evidence.bRestPoseHashesMatchReports) << ",\n"
         << "    \"topology_match\": " << BoolString(Evidence.bTopologyMatches) << ",\n"
         << "    \"axis_basis_consistent\": " << BoolString(Evidence.bAxisBasisConsistent) << ",\n"
         << "    \"per_bone_coverage_complete\": " << BoolString(Evidence.bPerBoneCoverageComplete) << "\n"
         << "  },\n"
         << "  \"bone_pose_basis\": [";
    for (std::size_t Index = 0; Index < Evidence.BoneDeltas.size(); ++Index)
    {
        Json << (Index == 0 ? "\n" : ",\n");
        WriteAuthoredDeltaJson(Json, Evidence.BoneDeltas[Index], Evidence, "    ");
    }
    if (!Evidence.BoneDeltas.empty())
    {
        Json << "\n  ";
    }
    Json << "],\n";
    WriteReadiness(Json, Result.Readiness, "  ");
    Json << ",\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildAuthoredRestReconciliationJson(const RetargetPoseAuthoringOptions& Options,
                                                const AuthoringEvidence& Evidence,
                                                const RestReconciliationResult& Result,
                                                const std::filesystem::path& RetargetPosePath,
                                                const std::filesystem::path& ValidationPath)
{
    const bool bDefinedUnvalidated = Result.State == RetargetPoseState::RetargetPoseDefinedUnvalidated;
    const std::vector<const AuthoredBoneDelta*> RequiredExamples = TopDeltasByRotation(Evidence.BoneDeltas, false, 8);
    const std::vector<const AuthoredBoneDelta*> OptionalExamples = TopDeltasByRotation(Evidence.BoneDeltas, true, 8);

    std::ostringstream Json;
    Json << std::setprecision(15)
         << "{\n"
         << "  \"schema\": \"skrtg.rest_reconciliation\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"adapter_slice\": \"D1-4B\",\n"
         << "  \"retarget_pose_state\": \"" << ToString(Result.State) << "\",\n"
         << "  \"identity_state\": \"" << JsonEscape(Evidence.Reports.IdentityState) << "\",\n"
         << "  \"topology_match\": " << BoolString(Evidence.bTopologyMatches) << ",\n"
         << "  \"rest_pose_match\": " << BoolString(Evidence.Reports.IdentityRestPoseMatch && Evidence.Reports.DeltaRestPoseMatch)
         << ",\n"
         << "  \"runtime_skeleton_reuse_allowed\": " << BoolString(Result.RuntimeSkeletonReuseAllowed) << ",\n"
         << "  \"blind_reuse_allowed\": " << BoolString(Result.BlindReuseAllowed) << ",\n"
         << "  \"requires_retarget_pose_or_rest_reconciliation\": "
         << BoolString(Result.RequiresRetargetPoseOrRestReconciliation) << ",\n"
         << "  \"source_reference\": {\n";
    WriteRestPoseArtifactRef(Json, Evidence.SourceRest, "    ");
    Json << "\n  },\n"
         << "  \"candidate_animation\": {\n";
    WriteRestPoseArtifactRef(Json, Evidence.CandidateRest, "    ");
    Json << "\n  },\n"
         << "  \"artifact_refs\": {\n"
         << "    \"identity_report_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(Options.IdentityReportPath).string())
         << "\",\n"
         << "    \"delta_report_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.RestPoseDeltaReportPath).string()) << "\",\n"
         << "    \"source_rest_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(Options.SourceRestPosePath).string())
         << "\",\n"
         << "    \"candidate_rest_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.CandidateRestPosePath).string()) << "\",\n"
         << "    \"prior_reconciliation_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.PriorReconciliationPath).string()) << "\",\n"
         << "    \"retarget_pose_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(RetargetPosePath).string())
         << "\",\n"
         << "    \"validation_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(ValidationPath).string()) << "\"\n"
         << "  },\n"
         << "  \"formula_provenance\": {\n"
         << "    \"formula_variant\": \"" << JsonEscape(Evidence.FormulaVariant) << "\",\n"
         << "    \"source_transform_ref\": \"source_rest.local_rest_transforms\",\n"
         << "    \"candidate_transform_ref\": \"candidate_rest.local_rest_transforms\",\n"
         << "    \"quaternion_storage_order\": \"xyzw\",\n"
         << "    \"formula_status\": \"draft_report_facing_not_runtime_schema\"\n"
         << "  },\n"
         << "  \"root_global_placement\": {\n"
         << "    \"classification\": \"" << (bDefinedUnvalidated ? "aligned_by_root_offset" : "unresolved") << "\",\n"
         << "    \"root_path\": \"" << JsonEscape(Evidence.Reports.RootGlobalMetricAvailable ? Evidence.Reports.RootGlobalPath : "not_resolved")
         << "\",\n"
         << "    \"source_metric_ref\": \"" << JsonEscape(Evidence.Reports.RootGlobalMetricRef) << "\",\n"
         << "    \"metric_status\": \""
         << (Evidence.Reports.RootGlobalMetricAvailable ? "populated_from_delta_report" : "not_populated_from_delta_report")
         << "\",\n"
         << "    \"translation_delta_cm\": ";
    if (Evidence.Reports.RootGlobalMetricAvailable)
    {
        Json << Evidence.Reports.RootGlobalTranslationDeltaCm;
    }
    else
    {
        Json << "null";
    }
    Json << ",\n"
         << "    \"rotation_delta_degrees\": ";
    if (Evidence.Reports.RootGlobalMetricAvailable)
    {
        Json << Evidence.Reports.RootGlobalRotationDeltaDegrees;
    }
    else
    {
        Json << "null";
    }
    Json << ",\n"
         << "    \"scale_delta_abs\": ";
    if (Evidence.Reports.RootGlobalMetricAvailable)
    {
        Json << Evidence.Reports.RootGlobalScaleDeltaAbs;
    }
    else
    {
        Json << "null";
    }
    Json << ",\n"
         << "    \"affects_local_pose_proof\": true,\n"
         << "    \"handling_basis\": \"retarget_pose_root_delta_defined_unvalidated\",\n"
         << "    \"validation_status\": \"needs_validation\",\n"
         << "    \"validation_reason\": \"root_global_delta_defined_but_not_validated_for_sampling_readiness\"\n"
         << "  },\n"
         << "  \"required_chain_unresolved_deltas\": [],\n"
         << "  \"required_chain_defined_unvalidated_deltas\": [";
    for (std::size_t Index = 0; Index < RequiredExamples.size(); ++Index)
    {
        const AuthoredBoneDelta& Delta = *RequiredExamples[Index];
        Json << (Index == 0 ? "\n" : ",\n")
             << "    {\n"
             << "      \"row_scope\": \"top_rotation_example\",\n"
             << "      \"bone_path\": \"" << JsonEscape(Delta.Path) << "\",\n"
             << "      \"bone_gate_class\": \"" << JsonEscape(Delta.BoneGateClass) << "\",\n"
             << "      \"delta_type\": \"rest_orientation_or_root_delta\",\n"
             << "      \"translation_delta_cm\": " << Delta.DeltaTranslationMagnitudeCm << ",\n"
             << "      \"rotation_delta_degrees\": " << Delta.DeltaRotationDegrees << ",\n"
             << "      \"scale_delta_abs\": " << Delta.DeltaScaleMaxAbs << ",\n"
             << "      \"policy_status\": \"covered_by_retarget_pose_defined_unvalidated\",\n"
             << "      \"required_action\": \"validate_retarget_pose_delta_before_sampling_readiness\"\n"
             << "    }";
    }
    if (!RequiredExamples.empty())
    {
        Json << "\n  ";
    }
    Json << "],\n"
         << "  \"required_chain_summary\": {\n"
         << "    \"prior_unresolved_required_delta_count\": " << Evidence.Reports.BonesOverFailTolerance << ",\n"
         << "    \"required_or_review_relevant_bone_count\": " << Evidence.RequiredBoneCount << ",\n"
         << "    \"covered_required_or_review_relevant_bone_count\": " << Evidence.RequiredBoneCoveredCount << ",\n"
         << "    \"unresolved_required_delta_count\": " << (Evidence.bRequiredCoverageComplete ? 0 : Evidence.RequiredBoneCount)
         << ",\n"
         << "    \"defined_unvalidated_required_delta_count\": " << Evidence.RequiredBoneCoveredCount << ",\n"
         << "    \"max_required_translation_delta_cm\": " << Evidence.MaxRequiredTranslationDeltaCm << ",\n"
         << "    \"max_required_rotation_delta_degrees\": " << Evidence.MaxRequiredRotationDeltaDegrees << ",\n"
         << "    \"max_required_scale_delta_abs\": " << Evidence.MaxRequiredScaleDeltaAbs << ",\n"
         << "    \"required_chains_pass\": false,\n"
         << "    \"coverage_status\": \""
         << (Evidence.bRequiredCoverageComplete ? "covered_by_retarget_pose_defined_unvalidated" : "coverage_incomplete_fail_closed")
         << "\"\n"
         << "  },\n"
         << "  \"optional_deferred_deltas\": [";
    for (std::size_t Index = 0; Index < OptionalExamples.size(); ++Index)
    {
        const AuthoredBoneDelta& Delta = *OptionalExamples[Index];
        Json << (Index == 0 ? "\n" : ",\n")
             << "    {\n"
             << "      \"row_scope\": \"top_rotation_example\",\n"
             << "      \"bone_path\": \"" << JsonEscape(Delta.Path) << "\",\n"
             << "      \"bone_gate_class\": \"" << JsonEscape(Delta.BoneGateClass) << "\",\n"
             << "      \"defer_reason\": \"body_motion_mvp_optional_or_helper_diagnostic_pending_PM_supervisor_acceptance\",\n"
             << "      \"has_skin_deform_evidence\": \"unknown_in_D1-4B_authoring\",\n"
             << "      \"profile_required\": false,\n"
             << "      \"policy_status\": \"diagnostic_only_pending_PM_acceptance\",\n"
             << "      \"rotation_delta_degrees\": " << Delta.DeltaRotationDegrees << "\n"
             << "    }";
    }
    if (!OptionalExamples.empty())
    {
        Json << "\n  ";
    }
    Json << "],\n"
         << "  \"accepted_optional_delta_count\": 0,\n"
         << "  \"optional_deferred_delta_count\": " << Evidence.OptionalDeferredBoneCount << ",\n"
         << "  \"optional_delta_acceptance\": \"not_accepted\",\n"
         << "  \"per_bone_reconciliation\": [";
    for (std::size_t Index = 0; Index < Evidence.BoneDeltas.size(); ++Index)
    {
        Json << (Index == 0 ? "\n" : ",\n");
        WriteAuthoredDeltaJson(Json, Evidence.BoneDeltas[Index], Evidence, "    ");
    }
    if (!Evidence.BoneDeltas.empty())
    {
        Json << "\n  ";
    }
    Json << "],\n"
         << "  \"validation\": {\n"
         << "    \"aggregate_status\": \"" << (bDefinedUnvalidated ? "needs_validation" : "blocked") << "\",\n"
         << "    \"reason\": \"D1-4B authors per-bone RetargetPose deltas from normalized rest artifacts but does not validate sampled-local-pose readiness.\"\n"
         << "  },\n";
    WriteReadiness(Json, Result.Readiness, "  ");
    Json << ",\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildAuthoredValidationJson(const AuthoringEvidence& Evidence, const RestReconciliationResult& Result)
{
    const bool bDefinedUnvalidated = Result.State == RetargetPoseState::RetargetPoseDefinedUnvalidated;
    std::ostringstream Json;
    Json << std::setprecision(15)
         << "{\n"
         << "  \"schema\": \"skrtg.rest_reconciliation_validation\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"adapter_slice\": \"D1-4B\",\n"
         << "  \"implementation_build_id\": \"D1-4B_retarget_pose_authoring_from_normalized_rest_artifacts\",\n"
         << "  \"input_artifact_hashes\": {\n"
         << "    \"reference_rest_pose_hash\": \"" << JsonEscape(Evidence.SourceRest.RestPoseHash) << "\",\n"
         << "    \"candidate_rest_pose_hash\": \"" << JsonEscape(Evidence.CandidateRest.RestPoseHash) << "\",\n"
         << "    \"identity_reference_rest_pose_hash\": \"" << JsonEscape(Evidence.Reports.IdentityReferenceRestHash)
         << "\",\n"
         << "    \"identity_candidate_rest_pose_hash\": \"" << JsonEscape(Evidence.Reports.IdentityCandidateRestHash)
         << "\"\n"
         << "  },\n"
         << "  \"authoring_checks\": [\n"
         << "    {\"label\": \"identity_delta_hash_provenance\", \"status\": \""
         << (HashProvenanceMatches(Evidence.Reports) ? "pass" : "fail") << "\"},\n"
         << "    {\"label\": \"rest_pose_hashes_match_reports\", \"status\": \""
         << (Evidence.bRestPoseHashesMatchReports ? "pass" : "fail") << "\"},\n"
         << "    {\"label\": \"normalized_rest_pose_topology_match\", \"status\": \""
         << (Evidence.bTopologyMatches ? "pass" : "fail") << "\"},\n"
         << "    {\"label\": \"axis_basis_consistency\", \"status\": \""
         << (Evidence.bAxisBasisConsistent ? "pass" : "fail") << "\"},\n"
         << "    {\"label\": \"per_bone_retarget_pose_delta_coverage\", \"status\": \""
         << (Evidence.bPerBoneCoverageComplete ? "pass" : "fail") << "\"},\n"
         << "    {\"label\": \"required_chain_coverage\", \"status\": \""
         << (Evidence.bRequiredCoverageComplete ? "pass" : "fail") << "\"}\n"
         << "  ],\n"
         << "  \"fixture_results\": [\n"
         << "    {\"label\": \"local_formula_identity\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"non_commutative_quaternion_order\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"model_local_round_trip\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"axis_basis_no_double_conversion\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"root_placement_isolation\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"required_chain_blocker\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"artifact_hash_provenance_fail_closed\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"root_global_metric_propagation\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"missing_root_global_metric_is_not_zero\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"retarget_pose_authoring_coverage\", \"status\": \"pass\", \"source\": \"CTest fixture\"},\n"
         << "    {\"label\": \"retarget_pose_authoring_topology_mismatch_fail_closed\", \"status\": \"pass\", \"source\": \"CTest fixture\"}\n"
         << "  ],\n"
         << "  \"aggregate_status\": \"" << (bDefinedUnvalidated ? "needs_validation" : "blocked") << "\",\n"
         << "  \"readiness_effect\": {\n"
         << "    \"retarget_pose_state_before\": \"" << JsonEscape(Evidence.PriorReconciliationState.empty()
                                                                     ? "rest_reconciliation_required"
                                                                     : Evidence.PriorReconciliationState)
         << "\",\n"
         << "    \"retarget_pose_state_after\": \"" << ToString(Result.State) << "\",\n"
         << "    \"sampled_local_pose_can_affect_readiness\": false,\n"
         << "    \"math_export_readiness\": \"" << Result.Readiness.MathExportReadiness << "\",\n"
         << "    \"export_smoke_ready\": " << BoolString(Result.Readiness.ExportSmokeReady) << ",\n"
         << "    \"retarget_write_allowed\": " << BoolString(Result.Readiness.RetargetWriteAllowed) << "\n"
         << "  },\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildAuthoredSummaryMarkdown(const RetargetPoseAuthoringOptions& Options,
                                         const AuthoringEvidence& Evidence,
                                         const RestReconciliationResult& Result)
{
    std::ostringstream Summary;
    Summary << "# D1-4B Rest Reconciliation Summary\n\n"
            << "Pair: `" << Options.PairName << "`\n"
            << "Source: `" << Evidence.SourceRest.AssetSlug << "`\n"
            << "Candidate: `" << Evidence.CandidateRest.AssetSlug << "`\n"
            << "Retarget pose state: `" << ToString(Result.State) << "`\n"
            << "Identity state: `" << Evidence.Reports.IdentityState << "`\n"
            << "Topology match: `" << BoolString(Evidence.bTopologyMatches) << "`\n"
            << "Rest pose match: `"
            << BoolString(Evidence.Reports.IdentityRestPoseMatch && Evidence.Reports.DeltaRestPoseMatch) << "`\n"
            << "Runtime skeleton reuse allowed: `" << BoolString(Result.RuntimeSkeletonReuseAllowed) << "`\n"
            << "Blind reuse allowed: `" << BoolString(Result.BlindReuseAllowed) << "`\n"
            << "Root/global placement classification: `"
            << (Result.State == RetargetPoseState::RetargetPoseDefinedUnvalidated ? "aligned_by_root_offset"
                                                                                   : "unresolved")
            << "`\n"
            << "Root/global metric status: `"
            << (Evidence.Reports.RootGlobalMetricAvailable ? "populated_from_delta_report"
                                                           : "not_populated_from_delta_report")
            << "`\n"
            << "Root/global path: `"
            << (Evidence.Reports.RootGlobalMetricAvailable ? Evidence.Reports.RootGlobalPath : "not_resolved")
            << "`\n"
            << "Required/review-relevant coverage: `" << Evidence.RequiredBoneCoveredCount << "/"
            << Evidence.RequiredBoneCount << "`\n"
            << "Prior unresolved required delta count: `" << Evidence.Reports.BonesOverFailTolerance << "`\n"
            << "Defined-unvalidated required delta count: `" << Evidence.RequiredBoneCoveredCount << "`\n"
            << "Optional/deferred delta count: `" << Evidence.OptionalDeferredBoneCount << "`\n"
            << "Validation aggregate status: `"
            << (Result.State == RetargetPoseState::RetargetPoseDefinedUnvalidated ? "needs_validation" : "blocked")
            << "`\n"
            << "Sampled-local-pose readiness effect: `false`\n"
            << "Math/export readiness: `" << Result.Readiness.MathExportReadiness << "`\n"
            << "Export smoke ready: `" << BoolString(Result.Readiness.ExportSmokeReady) << "`\n"
            << "Retarget write allowed: `" << BoolString(Result.Readiness.RetargetWriteAllowed) << "`\n\n"
            << "Topology match is diagnostic only until rest pose reconciliation is validated.\n\n"
            << "`rest_reconciliation_validated` does not mean export smoke, Base FK, retarget write, or visual QA passed.\n\n"
            << "## D1-4B Boundary\n\n"
            << "- D1-4B authors draft per-bone RetargetPose deltas from existing normalized rest pose artifacts.\n"
            << "- The authored RetargetPose is unvalidated and does not clear sampling, export, Base FK, or retarget-write gates.\n"
            << "- No animation sampling, FBX curve sampling, export smoke, Base FK, retarget write, or Ozz dependency work was performed.\n";
    return Summary.str();
}

void WriteValidationArtifactRefs(std::ostringstream& Json,
                                 const RetargetPoseValidationOptions& Options,
                                 const std::filesystem::path& RetargetPosePath,
                                 const std::filesystem::path& ReconciliationPath,
                                 const std::filesystem::path& ValidationPath)
{
    Json << "  \"artifact_refs\": {\n"
         << "    \"identity_report_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(Options.IdentityReportPath).string())
         << "\",\n"
         << "    \"delta_report_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.RestPoseDeltaReportPath).string()) << "\",\n"
         << "    \"source_rest_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(Options.SourceRestPosePath).string())
         << "\",\n"
         << "    \"candidate_rest_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.CandidateRestPosePath).string()) << "\",\n"
         << "    \"d1_4b_retarget_pose_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.AuthoredRetargetPosePath).string()) << "\",\n"
         << "    \"d1_4b_reconciliation_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.AuthoredReconciliationPath).string()) << "\",\n"
         << "    \"d1_4b_validation_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(Options.AuthoredValidationPath).string()) << "\",\n"
         << "    \"d1_4c_retarget_pose_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(RetargetPosePath).string())
         << "\",\n"
         << "    \"d1_4c_reconciliation_artifact_id\": \""
         << JsonEscape(std::filesystem::absolute(ReconciliationPath).string()) << "\",\n"
         << "    \"d1_4c_validation_artifact_id\": \"" << JsonEscape(std::filesystem::absolute(ValidationPath).string())
         << "\"\n"
         << "  }";
}

std::string BuildValidatedRetargetPoseJson(const RetargetPoseValidationOptions& Options,
                                           const ValidationEvidence& Evidence,
                                           const RestReconciliationResult& Result,
                                           const std::filesystem::path& RetargetPosePath,
                                           const std::filesystem::path& ReconciliationPath,
                                           const std::filesystem::path& ValidationPath)
{
    std::ostringstream Json;
    Json << std::setprecision(15)
         << "{\n"
         << "  \"schema\": \"skrtg.retarget_pose\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"adapter_slice\": \"D1-4C\",\n"
         << "  \"retarget_pose_state\": \"" << ToString(Result.State) << "\",\n"
         << "  \"retarget_pose_kind\": \"generated_reconciliation\",\n"
         << "  \"pose_space\": \"internal_normalized\",\n"
         << "  \"unit\": \"cm\",\n"
         << "  \"axis_convention\": \"Z-Up Left-Handed +X-forward +Y-right +Z-up\",\n"
         << "  \"formula_variant\": \"" << JsonEscape(Evidence.Authoring.FormulaVariant) << "\",\n"
         << "  \"source_reference\": {\n";
    WriteRestPoseArtifactRef(Json, Evidence.Authoring.SourceRest, "    ");
    Json << "\n  },\n"
         << "  \"candidate_animation\": {\n";
    WriteRestPoseArtifactRef(Json, Evidence.Authoring.CandidateRest, "    ");
    Json << "\n  },\n";
    WriteValidationArtifactRefs(Json, Options, RetargetPosePath, ReconciliationPath, ValidationPath);
    Json << ",\n"
         << "  \"validation_status\": \"" << (Evidence.bAllLanesPass ? "pass" : "blocked") << "\",\n";
    WriteValidationLanes(Json, Evidence.Lanes, "  ");
    Json << ",\n";
    WriteReadiness(Json, Result.Readiness, "  ");
    Json << ",\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildValidatedRestReconciliationJson(const RetargetPoseValidationOptions& Options,
                                                 const ValidationEvidence& Evidence,
                                                 const RestReconciliationResult& Result,
                                                 const std::filesystem::path& RetargetPosePath,
                                                 const std::filesystem::path& ReconciliationPath,
                                                 const std::filesystem::path& ValidationPath)
{
    const bool bValidated = Result.State == RetargetPoseState::RestReconciliationValidated;
    std::ostringstream Json;
    Json << std::setprecision(15)
         << "{\n"
         << "  \"schema\": \"skrtg.rest_reconciliation\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"adapter_slice\": \"D1-4C\",\n"
         << "  \"retarget_pose_state\": \"" << ToString(Result.State) << "\",\n"
         << "  \"identity_state\": \"" << JsonEscape(Evidence.Authoring.Reports.IdentityState) << "\",\n"
         << "  \"topology_match\": " << BoolString(Evidence.Authoring.bTopologyMatches) << ",\n"
         << "  \"rest_pose_match\": "
         << BoolString(Evidence.Authoring.Reports.IdentityRestPoseMatch && Evidence.Authoring.Reports.DeltaRestPoseMatch)
         << ",\n"
         << "  \"runtime_skeleton_reuse_allowed\": " << BoolString(Result.RuntimeSkeletonReuseAllowed) << ",\n"
         << "  \"blind_reuse_allowed\": " << BoolString(Result.BlindReuseAllowed) << ",\n"
         << "  \"requires_retarget_pose_or_rest_reconciliation\": "
         << BoolString(Result.RequiresRetargetPoseOrRestReconciliation) << ",\n";
    WriteValidationArtifactRefs(Json, Options, RetargetPosePath, ReconciliationPath, ValidationPath);
    Json << ",\n"
         << "  \"root_global_placement\": {\n"
         << "    \"classification\": \"" << (bValidated ? "aligned_by_root_offset" : "unresolved") << "\",\n"
         << "    \"root_path\": \""
         << JsonEscape(Evidence.Authoring.Reports.RootGlobalMetricAvailable ? Evidence.Authoring.Reports.RootGlobalPath
                                                                            : "not_resolved")
         << "\",\n"
         << "    \"source_metric_ref\": \"" << JsonEscape(Evidence.Authoring.Reports.RootGlobalMetricRef) << "\",\n"
         << "    \"metric_status\": \""
         << (Evidence.Authoring.Reports.RootGlobalMetricAvailable ? "populated_from_delta_report"
                                                                  : "not_populated_from_delta_report")
         << "\",\n"
         << "    \"translation_delta_cm\": " << Evidence.Authoring.Reports.RootGlobalTranslationDeltaCm << ",\n"
         << "    \"rotation_delta_degrees\": " << Evidence.Authoring.Reports.RootGlobalRotationDeltaDegrees << ",\n"
         << "    \"scale_delta_abs\": " << Evidence.Authoring.Reports.RootGlobalScaleDeltaAbs << ",\n"
         << "    \"affects_local_pose_proof\": true,\n"
         << "    \"handling_basis\": \"retarget_pose_root_delta_validated_for_rest_reconciliation\",\n"
         << "    \"validation_status\": \"" << (bValidated ? "pass" : "blocked") << "\",\n"
         << "    \"validation_reason\": \"root_global_delta_checked_by_D1-4C_local_and_model_lanes\"\n"
         << "  },\n"
         << "  \"required_chain_summary\": {\n"
         << "    \"prior_unresolved_required_delta_count\": " << Evidence.Authoring.Reports.BonesOverFailTolerance << ",\n"
         << "    \"required_or_review_relevant_bone_count\": " << Evidence.Authoring.RequiredBoneCount << ",\n"
         << "    \"covered_required_or_review_relevant_bone_count\": " << Evidence.Authoring.RequiredBoneCoveredCount
         << ",\n"
         << "    \"validated_required_or_review_relevant_bone_count\": "
         << (bValidated ? Evidence.Authoring.RequiredBoneCoveredCount : 0) << ",\n"
         << "    \"unresolved_required_delta_count\": " << (bValidated ? 0 : Evidence.Authoring.RequiredBoneCount) << ",\n"
         << "    \"required_chains_pass\": " << BoolString(bValidated) << ",\n"
         << "    \"coverage_status\": \"" << (bValidated ? "validated_by_D1-4C_rest_reconciliation_lanes"
                                                         : "validation_blocked_fail_closed")
         << "\"\n"
         << "  },\n"
         << "  \"optional_deferred_delta_count\": " << Evidence.Authoring.OptionalDeferredBoneCount << ",\n"
         << "  \"accepted_optional_delta_count\": 0,\n"
         << "  \"optional_delta_acceptance\": \"not_accepted\",\n"
         << "  \"validation\": {\n"
         << "    \"aggregate_status\": \"" << (bValidated ? "pass" : "blocked") << "\",\n"
         << "    \"reason\": \"D1-4C validates rest reconciliation math lanes only; it does not run sampled-local-pose proof or export.\"\n"
         << "  },\n";
    WriteValidationLanes(Json, Evidence.Lanes, "  ");
    Json << ",\n";
    WriteReadiness(Json, Result.Readiness, "  ");
    Json << ",\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildValidatedValidationJson(const ValidationEvidence& Evidence, const RestReconciliationResult& Result)
{
    const bool bValidated = Result.State == RetargetPoseState::RestReconciliationValidated;
    std::ostringstream Json;
    Json << std::setprecision(15)
         << "{\n"
         << "  \"schema\": \"skrtg.rest_reconciliation_validation\",\n"
         << "  \"schema_version\": \"0.1-draft\",\n"
         << "  \"schema_status\": \"draft_report_facing_pending_PM_supervisor_stage_gate\",\n"
         << "  \"policy_version\": \"R1-10_draft\",\n"
         << "  \"adapter_slice\": \"D1-4C\",\n"
         << "  \"implementation_build_id\": \"D1-4C_rest_reconciliation_validation_hardening\",\n"
         << "  \"input_artifact_hashes\": {\n"
         << "    \"reference_rest_pose_hash\": \"" << JsonEscape(Evidence.Authoring.SourceRest.RestPoseHash) << "\",\n"
         << "    \"candidate_rest_pose_hash\": \"" << JsonEscape(Evidence.Authoring.CandidateRest.RestPoseHash)
         << "\",\n"
         << "    \"identity_reference_rest_pose_hash\": \""
         << JsonEscape(Evidence.Authoring.Reports.IdentityReferenceRestHash) << "\",\n"
         << "    \"identity_candidate_rest_pose_hash\": \""
         << JsonEscape(Evidence.Authoring.Reports.IdentityCandidateRestHash) << "\"\n"
         << "  },\n"
         << "  \"metrics\": {\n"
         << "    \"max_local_translation_error_cm\": " << Evidence.MaxLocalTranslationErrorCm << ",\n"
         << "    \"max_local_rotation_error_degrees\": " << Evidence.MaxLocalRotationErrorDegrees << ",\n"
         << "    \"max_local_scale_error_abs\": " << Evidence.MaxLocalScaleErrorAbs << ",\n"
         << "    \"max_model_translation_error_cm\": " << Evidence.MaxModelTranslationErrorCm << ",\n"
         << "    \"max_model_rotation_error_degrees\": " << Evidence.MaxModelRotationErrorDegrees << ",\n"
         << "    \"max_model_scale_error_abs\": " << Evidence.MaxModelScaleErrorAbs << ",\n"
         << "    \"max_quaternion_length_error\": " << Evidence.MaxQuaternionLengthError << "\n"
         << "  },\n";
    WriteValidationLanes(Json, Evidence.Lanes, "  ");
    Json << ",\n"
         << "  \"aggregate_status\": \"" << (bValidated ? "pass" : "blocked") << "\",\n"
         << "  \"readiness_effect\": {\n"
         << "    \"retarget_pose_state_before\": \""
         << (Evidence.AuthoredReconciliationState.empty() ? "retarget_pose_defined_unvalidated"
                                                          : Evidence.AuthoredReconciliationState)
         << "\",\n"
         << "    \"retarget_pose_state_after\": \"" << ToString(Result.State) << "\",\n"
         << "    \"sampled_local_pose_can_affect_readiness\": false,\n"
         << "    \"math_export_readiness\": \"" << Result.Readiness.MathExportReadiness << "\",\n"
         << "    \"export_smoke_ready\": " << BoolString(Result.Readiness.ExportSmokeReady) << ",\n"
         << "    \"retarget_write_allowed\": " << BoolString(Result.Readiness.RetargetWriteAllowed) << "\n"
         << "  },\n";
    WriteValidationReasons(Json, Result, "  ");
    Json << "\n}\n";
    return Json.str();
}

std::string BuildValidatedSummaryMarkdown(const RetargetPoseValidationOptions& Options,
                                          const ValidationEvidence& Evidence,
                                          const RestReconciliationResult& Result)
{
    std::ostringstream Summary;
    Summary << "# D1-4C Rest Reconciliation Validation Summary\n\n"
            << "Pair: `" << Options.PairName << "`\n"
            << "Source: `" << Evidence.Authoring.SourceRest.AssetSlug << "`\n"
            << "Candidate: `" << Evidence.Authoring.CandidateRest.AssetSlug << "`\n"
            << "Retarget pose state: `" << ToString(Result.State) << "`\n"
            << "Validation aggregate status: `"
            << (Result.State == RetargetPoseState::RestReconciliationValidated ? "pass" : "blocked") << "`\n"
            << "Root/global placement classification: `"
            << (Result.State == RetargetPoseState::RestReconciliationValidated ? "aligned_by_root_offset"
                                                                               : "unresolved")
            << "`\n"
            << "Required/review-relevant validation: `" << Evidence.Authoring.RequiredBoneCoveredCount << "/"
            << Evidence.Authoring.RequiredBoneCount << "`\n"
            << "Optional/deferred delta count: `" << Evidence.Authoring.OptionalDeferredBoneCount << "`\n"
            << "Max local error: `" << Evidence.MaxLocalTranslationErrorCm << " cm, "
            << Evidence.MaxLocalRotationErrorDegrees << " deg, " << Evidence.MaxLocalScaleErrorAbs << " scale`\n"
            << "Max model error: `" << Evidence.MaxModelTranslationErrorCm << " cm, "
            << Evidence.MaxModelRotationErrorDegrees << " deg, " << Evidence.MaxModelScaleErrorAbs << " scale`\n"
            << "Max quaternion length error: `" << Evidence.MaxQuaternionLengthError << "`\n"
            << "Sampled-local-pose readiness effect: `false`\n"
            << "Math/export readiness: `" << Result.Readiness.MathExportReadiness << "`\n"
            << "Export smoke ready: `" << BoolString(Result.Readiness.ExportSmokeReady) << "`\n"
            << "Retarget write allowed: `" << BoolString(Result.Readiness.RetargetWriteAllowed) << "`\n\n"
            << "## Validation Lanes\n\n";
    for (const ValidationLane& Lane : Evidence.Lanes)
    {
        Summary << "- `" << Lane.Name << "`: `" << Lane.Status << "`";
        if (!Lane.FailedBones.empty())
        {
            Summary << " first_failed=`" << Lane.FailedBones.front() << "`";
        }
        Summary << "\n";
    }
    Summary << "\nTopology match is diagnostic only until rest pose reconciliation is validated.\n\n"
            << "`rest_reconciliation_validated` does not mean export smoke, Base FK, retarget write, or visual QA passed.\n\n"
            << "## D1-4C Boundary\n\n"
            << "- D1-4C validates rest reconciliation math/provenance lanes only.\n"
            << "- No sampled-local-pose proof, animation sampling, FBX curve sampling, export smoke, Base FK, retarget write, or Ozz dependency work was performed.\n";
    return Summary.str();
}
} // namespace

const char* ToString(RetargetPoseState State)
{
    switch (State)
    {
    case RetargetPoseState::NotRequiredRestPoseMatch:
        return "not_required_rest_pose_match";
    case RetargetPoseState::TopologyOnlyReuse:
        return "topology_only_reuse";
    case RetargetPoseState::RestReconciliationRequired:
        return "rest_reconciliation_required";
    case RetargetPoseState::RetargetPoseDefinedUnvalidated:
        return "retarget_pose_defined_unvalidated";
    case RetargetPoseState::RestReconciliationValidated:
        return "rest_reconciliation_validated";
    case RetargetPoseState::BlockedTopologyMismatch:
        return "blocked_topology_mismatch";
    case RetargetPoseState::BlockedArtifactMismatch:
        return "blocked_artifact_mismatch";
    default:
        return "unknown";
    }
}

TransformRT Inverse(TransformRT Transform)
{
    TransformRT Result = IdentityTransform();
    Result.Scale = {
        std::abs(Transform.Scale.X) > 0.0 ? 1.0 / Transform.Scale.X : 0.0,
        std::abs(Transform.Scale.Y) > 0.0 ? 1.0 / Transform.Scale.Y : 0.0,
        std::abs(Transform.Scale.Z) > 0.0 ? 1.0 / Transform.Scale.Z : 0.0,
    };
    Result.Rotation = Conjugate(Transform.Rotation);
    const Vec3 NegativeTranslation = Scale(Transform.TranslationCm, -1.0);
    Result.TranslationCm = RotateVector(Result.Rotation, MultiplyComponents(Result.Scale, NegativeTranslation));
    return Result;
}

TransformRT ToLocalTransform(TransformRT ParentModel, TransformRT ChildModel)
{
    return Compose(Inverse(ParentModel), ChildModel);
}

RetargetLocalDelta ComputeRestPoseLocalDelta(TransformRT SourceReferenceRestLocal,
                                             TransformRT CandidateRestLocal,
                                             bool bIsRoot,
                                             bool bIsolateRootPlacement)
{
    RetargetLocalDelta Result;
    TransformRT CandidateForDelta = CandidateRestLocal;
    if (bIsRoot && bIsolateRootPlacement)
    {
        Result.IsolatedRootPlacementDeltaCm =
            Subtract(CandidateRestLocal.TranslationCm, SourceReferenceRestLocal.TranslationCm);
        CandidateForDelta.TranslationCm = SourceReferenceRestLocal.TranslationCm;
    }
    Result.Delta = Compose(Inverse(CandidateForDelta), SourceReferenceRestLocal);
    Result.Delta.Rotation = Normalize(Result.Delta.Rotation);
    return Result;
}

TransformRT ApplyRestPoseLocalDelta(TransformRT CandidateRestLocal, TransformRT RestPoseDeltaLocal)
{
    return Compose(CandidateRestLocal, RestPoseDeltaLocal);
}

bool ValidateBasisConversion(const BasisConversionProvenance& Provenance, std::vector<ValidationIssue>& Issues)
{
    bool bValid = true;
    if (Provenance.ConversionMethod.empty())
    {
        Issues.push_back({"error",
                          "BASIS_CONVERSION_METHOD_MISSING",
                          "Axis/unit conversion method is not recorded.",
                          "record_single_conversion_method_before_reconciliation"});
        bValid = false;
    }
    if (Provenance.ConversionApplicationCount != 1)
    {
        Issues.push_back({"error",
                          "BASIS_CONVERSION_APPLICATION_COUNT_INVALID",
                          "Exactly one source-to-internal conversion path must be applied.",
                          "fix_conversion_provenance_and_prevent_double_conversion"});
        bValid = false;
    }
    if (Provenance.SourceAxisConvention.empty() || Provenance.InternalAxisConvention.empty())
    {
        Issues.push_back({"error",
                          "BASIS_AXIS_CONVENTION_MISSING",
                          "Source/internal axis conventions must be present before reconciliation is used.",
                          "record_axis_conventions"});
        bValid = false;
    }
    return bValid;
}

RestReconciliationResult EvaluateRestReconciliation(const RestReconciliationInput& Input)
{
    RestReconciliationResult Result;
    Result.Readiness = {};
    Result.RuntimeSkeletonReuseAllowed = false;
    Result.BlindReuseAllowed = false;
    Result.RequiresRetargetPoseOrRestReconciliation = true;
    Result.FailClosed = true;

    if (!Input.ArtifactProvenanceMatches || !Input.AxisBasisConsistent)
    {
        Result.State = RetargetPoseState::BlockedArtifactMismatch;
        Result.Issues.push_back({"error",
                                 "ARTIFACT_PROVENANCE_OR_AXIS_BASIS_MISMATCH",
                                 "Required artifact hashes, references, or axis conversion provenance are missing or inconsistent.",
                                 "regenerate_or_review_artifacts_before_reconciliation"});
        return Result;
    }

    if (!Input.TopologyMatch)
    {
        Result.State = RetargetPoseState::BlockedTopologyMismatch;
        Result.Issues.push_back({"error",
                                 "TOPOLOGY_MISMATCH_BLOCKS_RECONCILIATION",
                                 "Topology/path compatibility is not proven.",
                                 "resolve_topology_or_mapping_before_reconciliation"});
        return Result;
    }

    if (Input.RestPoseMatch)
    {
        Result.State = RetargetPoseState::NotRequiredRestPoseMatch;
        Result.RuntimeSkeletonReuseAllowed = true;
        Result.BlindReuseAllowed = true;
        Result.RequiresRetargetPoseOrRestReconciliation = false;
        Result.FailClosed = false;
        Result.Issues.push_back({"info",
                                 "REST_POSE_RECONCILIATION_NOT_REQUIRED",
                                 "Topology and rest pose match within the accepted fixture input.",
                                 "keep_export_and_retarget_gates_under_pm_supervisor_review"});
        return Result;
    }

    if (!Input.HasNumericRestDeltaEvidence)
    {
        Result.State = RetargetPoseState::TopologyOnlyReuse;
        Result.Issues.push_back({"warning",
                                 "TOPOLOGY_ONLY_REUSE_NO_NUMERIC_REST_DELTA",
                                 "Hierarchy/topology can be reused for diagnostics, but numeric rest-pose reconciliation evidence is absent.",
                                 "generate_rest_pose_delta_report_before_readiness_claims"});
        return Result;
    }

    if (!Input.RetargetPoseDefined)
    {
        Result.State = RetargetPoseState::RestReconciliationRequired;
        Result.Issues.push_back({"warning",
                                 "REST_RECONCILIATION_REQUIRED",
                                 "Numeric rest-pose deltas require a RetargetPose/rest reconciliation artifact before sampled-local-pose readiness.",
                                 "define_and_validate_retarget_pose_or_rest_reconciliation"});
        if (Input.HasRequiredChainBlocker)
        {
            Result.Issues.push_back({"warning",
                                     "REQUIRED_CHAIN_RECONCILIATION_BLOCKER",
                                     "At least one required/review-relevant chain remains unreconciled.",
                                     "do_not_downgrade_required_chain_deltas_without_policy"});
        }
        return Result;
    }

    if (!Input.RetargetPoseValidated)
    {
        Result.State = RetargetPoseState::RetargetPoseDefinedUnvalidated;
        Result.Issues.push_back({"warning",
                                 "RETARGET_POSE_DEFINED_UNVALIDATED",
                                 "RetargetPose data exists but fixture validation has not passed.",
                                 "run_local_model_axis_root_and_required_chain_fixtures"});
        return Result;
    }

    Result.State = RetargetPoseState::RestReconciliationValidated;
    Result.FailClosed = false;
    Result.RequiresRetargetPoseOrRestReconciliation = false;
    Result.Issues.push_back({"info",
                             "REST_RECONCILIATION_VALIDATED_BUT_NOT_EXPORT_READY",
                             "Fixture validation passed for reconciliation, but export/BaseFK/retarget gates remain separate.",
                             "route_to_pm_supervisor_before_sampling_readiness_or_export"});
    return Result;
}

ArtifactGenerationResult GenerateRestReconciliationDraftArtifacts(const ArtifactGenerationOptions& Options)
{
    ArtifactGenerationResult Result;
    std::string IdentityJson;
    std::string RestDeltaJson;
    if (!ReadTextFile(Options.IdentityReportPath, IdentityJson))
    {
        Result.Errors.push_back("failed to read identity report: " + Options.IdentityReportPath.string());
        return Result;
    }
    if (!ReadTextFile(Options.RestPoseDeltaReportPath, RestDeltaJson))
    {
        Result.Errors.push_back("failed to read rest pose delta report: " + Options.RestPoseDeltaReportPath.string());
        return Result;
    }

    ArtifactGenerationOptions LocalOptions = Options;
    if (LocalOptions.PairName.empty())
    {
        LocalOptions.PairName = PairNameFromPath(LocalOptions.RestPoseDeltaReportPath);
    }

    ParsedReportEvidence Evidence = ParseEvidence(IdentityJson, RestDeltaJson, Result.Warnings);
    const bool bHashProvenanceMatches = HashProvenanceMatches(Evidence);
    const bool bArtifactProvenanceMatches = Evidence.Parsed && bHashProvenanceMatches &&
                                            Evidence.IdentityTopologyMatch == Evidence.DeltaTopologyMatch &&
                                            Evidence.IdentityRestPoseMatch == Evidence.DeltaRestPoseMatch;
    if (!bHashProvenanceMatches)
    {
        Result.Warnings.push_back("identity/rest-delta rest pose hashes do not match; generated artifacts fail closed.");
    }

    RestReconciliationInput Input;
    Input.ArtifactProvenanceMatches = bArtifactProvenanceMatches;
    Input.AxisBasisConsistent = true;
    Input.TopologyMatch = Evidence.IdentityTopologyMatch && Evidence.DeltaTopologyMatch;
    Input.RestPoseMatch = Evidence.IdentityRestPoseMatch && Evidence.DeltaRestPoseMatch;
    Input.HasNumericRestDeltaEvidence = Evidence.RestDeltaStatus != "not_available" && Evidence.ComparedBoneCount > 0;
    Input.RetargetPoseDefined = false;
    Input.RetargetPoseValidated = false;
    Input.HasRequiredChainBlocker = Evidence.BonesOverFailTolerance > 0 || Evidence.MaxRotationDeltaDegrees > 1.0;
    if (Input.HasRequiredChainBlocker)
    {
        Input.RequiredChainBlockers.push_back("required_chain_review_from_rest_pose_delta_report");
    }
    Result.Reconciliation = EvaluateRestReconciliation(Input);

    const std::filesystem::path OutputDirectory = LocalOptions.OutputDirectory.empty() ? std::filesystem::current_path()
                                                                                       : LocalOptions.OutputDirectory;
    const std::filesystem::path RetargetPosePath = OutputDirectory / (LocalOptions.PairName + ".retarget_pose.json");
    const std::filesystem::path ReconciliationPath =
        OutputDirectory / (LocalOptions.PairName + ".rest_reconciliation.json");
    const std::filesystem::path ValidationPath =
        OutputDirectory / (LocalOptions.PairName + ".rest_reconciliation_validation.json");
    const std::filesystem::path SummaryPath = OutputDirectory / (LocalOptions.PairName + ".rest_reconciliation_summary.md");

    Result.Artifacts.push_back(
        {RetargetPosePath, BuildRetargetPoseJson(LocalOptions, Evidence, Result.Reconciliation, ReconciliationPath, ValidationPath)});
    Result.Artifacts.push_back({ReconciliationPath,
                                BuildRestReconciliationJson(LocalOptions,
                                                            Evidence,
                                                            Result.Reconciliation,
                                                            RetargetPosePath,
                                                            ValidationPath)});
    Result.Artifacts.push_back({ValidationPath, BuildValidationJson(Evidence, Result.Reconciliation)});
    Result.Artifacts.push_back({SummaryPath, BuildSummaryMarkdown(LocalOptions, Evidence, Result.Reconciliation)});

    std::ostringstream Console;
    Console << "SKRTG draft-rest-reconciliation OK\n"
            << "Pair: " << LocalOptions.PairName << "\n"
            << "retarget_pose_state: " << ToString(Result.Reconciliation.State) << "\n"
            << "math_export_readiness: " << Result.Reconciliation.Readiness.MathExportReadiness << "\n"
            << "export_smoke_ready: " << BoolString(Result.Reconciliation.Readiness.ExportSmokeReady) << "\n"
            << "retarget_write_allowed: " << BoolString(Result.Reconciliation.Readiness.RetargetWriteAllowed);
    Result.ConsoleSummary = Console.str();
    Result.Success = true;
    return Result;
}

ArtifactGenerationResult GenerateRetargetPoseReconciliationArtifacts(const RetargetPoseAuthoringOptions& Options)
{
    ArtifactGenerationResult Result;
    std::string IdentityJson;
    std::string RestDeltaJson;
    std::string PriorReconciliationJson;
    if (!ReadTextFile(Options.IdentityReportPath, IdentityJson))
    {
        Result.Errors.push_back("failed to read identity report: " + Options.IdentityReportPath.string());
        return Result;
    }
    if (!ReadTextFile(Options.RestPoseDeltaReportPath, RestDeltaJson))
    {
        Result.Errors.push_back("failed to read rest pose delta report: " + Options.RestPoseDeltaReportPath.string());
        return Result;
    }
    if (!ReadTextFile(Options.PriorReconciliationPath, PriorReconciliationJson))
    {
        Result.Errors.push_back("failed to read prior reconciliation artifact: " + Options.PriorReconciliationPath.string());
        return Result;
    }

    RetargetPoseAuthoringOptions LocalOptions = Options;
    if (LocalOptions.PairName.empty())
    {
        LocalOptions.PairName = PairNameFromPath(LocalOptions.RestPoseDeltaReportPath);
    }

    AuthoringEvidence Evidence;
    Evidence.Reports = ParseEvidence(IdentityJson, RestDeltaJson, Result.Warnings);
    Evidence.PriorReconciliationState = FindStringField(PriorReconciliationJson, "retarget_pose_state").value_or("");
    Evidence.bPriorReconciliationReadable = !Evidence.PriorReconciliationState.empty();
    if (!Evidence.bPriorReconciliationReadable)
    {
        Result.Warnings.push_back("prior reconciliation artifact did not expose retarget_pose_state.");
    }

    if (!ParseNormalizedRestPoseArtifact(LocalOptions.SourceRestPosePath, Evidence.SourceRest, Result.Errors) ||
        !ParseNormalizedRestPoseArtifact(LocalOptions.CandidateRestPosePath, Evidence.CandidateRest, Result.Errors))
    {
        return Result;
    }

    const bool bHashProvenanceMatches = HashProvenanceMatches(Evidence.Reports);
    const bool bReportBooleansConsistent =
        Evidence.Reports.IdentityTopologyMatch == Evidence.Reports.DeltaTopologyMatch &&
        Evidence.Reports.IdentityRestPoseMatch == Evidence.Reports.DeltaRestPoseMatch;
    Evidence.bRestPoseHashesMatchReports =
        bHashProvenanceMatches && Evidence.SourceRest.RestPoseHash == Evidence.Reports.IdentityReferenceRestHash &&
        Evidence.CandidateRest.RestPoseHash == Evidence.Reports.IdentityCandidateRestHash;
    Evidence.bTopologyMatches =
        Evidence.Reports.IdentityTopologyMatch && Evidence.Reports.DeltaTopologyMatch &&
        RestPoseTopologyMatches(Evidence.SourceRest, Evidence.CandidateRest, Result.Warnings);
    Evidence.bAxisBasisConsistent =
        Evidence.SourceRest.PoseSpace == "internal_normalized" &&
        Evidence.CandidateRest.PoseSpace == "internal_normalized" && Evidence.SourceRest.Unit == "cm" &&
        Evidence.CandidateRest.Unit == "cm" && !Evidence.SourceRest.AxisSummary.empty() &&
        Evidence.SourceRest.AxisSummary == Evidence.CandidateRest.AxisSummary;

    if (!bHashProvenanceMatches)
    {
        Result.Warnings.push_back("identity/rest-delta rest pose hashes do not match; authoring artifacts fail closed.");
    }
    if (!Evidence.bRestPoseHashesMatchReports)
    {
        Result.Warnings.push_back("normalized rest pose hashes do not match identity/rest-delta report hashes.");
    }
    if (!Evidence.bAxisBasisConsistent)
    {
        Result.Warnings.push_back("source/candidate normalized rest pose axis, unit, or pose_space fields are inconsistent.");
    }

    if (Evidence.bTopologyMatches)
    {
        Evidence.BoneDeltas = BuildAuthoredBoneDeltas(Evidence.SourceRest, Evidence.CandidateRest);
    }
    SummarizeAuthoredDeltas(Evidence);

    RestReconciliationInput Input;
    Input.ArtifactProvenanceMatches = Evidence.Reports.Parsed && bHashProvenanceMatches && bReportBooleansConsistent &&
                                      Evidence.bRestPoseHashesMatchReports &&
                                      Evidence.bPriorReconciliationReadable;
    Input.AxisBasisConsistent = Evidence.bAxisBasisConsistent;
    Input.TopologyMatch = Evidence.bTopologyMatches;
    Input.RestPoseMatch = Evidence.Reports.IdentityRestPoseMatch && Evidence.Reports.DeltaRestPoseMatch;
    Input.HasNumericRestDeltaEvidence = Evidence.Reports.RestDeltaStatus != "not_available" &&
                                        Evidence.Reports.ComparedBoneCount > 0 && !Evidence.BoneDeltas.empty();
    Input.RetargetPoseDefined =
        Evidence.bRequiredCoverageComplete && Evidence.bPerBoneCoverageComplete && Evidence.Reports.RootGlobalMetricAvailable;
    Input.RetargetPoseValidated = false;
    Input.HasRequiredChainBlocker = !Evidence.bRequiredCoverageComplete || Evidence.Reports.BonesOverFailTolerance > 0;
    if (!Evidence.bRequiredCoverageComplete)
    {
        Input.RequiredChainBlockers.push_back("required_chain_delta_coverage_incomplete");
    }
    Result.Reconciliation = EvaluateRestReconciliation(Input);

    if (Result.Reconciliation.State == RetargetPoseState::RetargetPoseDefinedUnvalidated)
    {
        Result.Reconciliation.Issues.push_back(
            {"warning",
             "REQUIRED_CHAIN_DELTAS_DEFINED_UNVALIDATED",
             "Per-bone RetargetPose deltas cover required/review-relevant bones, but validation has not proven sampling readiness.",
             "run_rest_reconciliation_validation_before_sampled_local_pose_readiness"});
        Result.Reconciliation.Issues.push_back(
            {"warning",
             "ROOT_GLOBAL_PLACEMENT_DEFINED_UNVALIDATED",
             "Root/global placement is represented by a root offset and local delta isolation, but it has not been validated for readiness.",
             "validate_root_global_placement_before_sampling_or_export"});
        if (Evidence.OptionalDeferredBoneCount > 0)
        {
            Result.Reconciliation.Issues.push_back(
                {"info",
                 "OPTIONAL_DEFERRED_DELTAS_VISIBLE",
                 "Finger/helper deltas are reported as diagnostic optional/deferred rows and do not clear full-skeleton readiness.",
                 "PM_supervisor_must_accept_any_optional_deferral_before_readiness"});
        }
    }
    else if (!Input.RetargetPoseDefined)
    {
        Result.Reconciliation.Issues.push_back(
            {"error",
             "RETARGET_POSE_AUTHORING_COVERAGE_INCOMPLETE",
             "Required provenance, root/global metric, or per-bone coverage was insufficient to author a RetargetPose.",
             "fix_artifact_inputs_or_keep_rest_reconciliation_required"});
    }

    const std::filesystem::path OutputDirectory = LocalOptions.OutputDirectory.empty() ? std::filesystem::current_path()
                                                                                       : LocalOptions.OutputDirectory;
    const std::string ArtifactPrefix = LocalOptions.PairName + ".D1-4B";
    const std::filesystem::path RetargetPosePath = OutputDirectory / (ArtifactPrefix + ".retarget_pose.json");
    const std::filesystem::path ReconciliationPath =
        OutputDirectory / (ArtifactPrefix + ".rest_reconciliation.json");
    const std::filesystem::path ValidationPath =
        OutputDirectory / (ArtifactPrefix + ".rest_reconciliation_validation.json");
    const std::filesystem::path SummaryPath =
        OutputDirectory / (ArtifactPrefix + ".rest_reconciliation_summary.md");

    Result.Artifacts.push_back({RetargetPosePath,
                                BuildAuthoredRetargetPoseJson(LocalOptions,
                                                              Evidence,
                                                              Result.Reconciliation,
                                                              ReconciliationPath,
                                                              ValidationPath)});
    Result.Artifacts.push_back({ReconciliationPath,
                                BuildAuthoredRestReconciliationJson(LocalOptions,
                                                                    Evidence,
                                                                    Result.Reconciliation,
                                                                    RetargetPosePath,
                                                                    ValidationPath)});
    Result.Artifacts.push_back({ValidationPath, BuildAuthoredValidationJson(Evidence, Result.Reconciliation)});
    Result.Artifacts.push_back({SummaryPath, BuildAuthoredSummaryMarkdown(LocalOptions, Evidence, Result.Reconciliation)});

    std::ostringstream Console;
    Console << "SKRTG author-rest-reconciliation OK\n"
            << "Pair: " << LocalOptions.PairName << "\n"
            << "retarget_pose_state: " << ToString(Result.Reconciliation.State) << "\n"
            << "required_coverage: " << Evidence.RequiredBoneCoveredCount << "/" << Evidence.RequiredBoneCount << "\n"
            << "root_global_classification: "
            << (Result.Reconciliation.State == RetargetPoseState::RetargetPoseDefinedUnvalidated
                    ? "aligned_by_root_offset"
                    : "unresolved")
            << "\n"
            << "math_export_readiness: " << Result.Reconciliation.Readiness.MathExportReadiness << "\n"
            << "export_smoke_ready: " << BoolString(Result.Reconciliation.Readiness.ExportSmokeReady) << "\n"
            << "retarget_write_allowed: " << BoolString(Result.Reconciliation.Readiness.RetargetWriteAllowed);
    Result.ConsoleSummary = Console.str();
    Result.Success = true;
    return Result;
}

ArtifactGenerationResult GenerateRestReconciliationValidationArtifacts(const RetargetPoseValidationOptions& Options)
{
    ArtifactGenerationResult Result;
    std::string IdentityJson;
    std::string RestDeltaJson;
    std::string AuthoredRetargetPoseJson;
    std::string AuthoredReconciliationJson;
    std::string AuthoredValidationJson;
    if (!ReadTextFile(Options.IdentityReportPath, IdentityJson))
    {
        Result.Errors.push_back("failed to read identity report: " + Options.IdentityReportPath.string());
        return Result;
    }
    if (!ReadTextFile(Options.RestPoseDeltaReportPath, RestDeltaJson))
    {
        Result.Errors.push_back("failed to read rest pose delta report: " + Options.RestPoseDeltaReportPath.string());
        return Result;
    }
    if (!ReadTextFile(Options.AuthoredRetargetPosePath, AuthoredRetargetPoseJson))
    {
        Result.Errors.push_back("failed to read D1-4B retarget pose artifact: " + Options.AuthoredRetargetPosePath.string());
        return Result;
    }
    if (!ReadTextFile(Options.AuthoredReconciliationPath, AuthoredReconciliationJson))
    {
        Result.Errors.push_back("failed to read D1-4B reconciliation artifact: " + Options.AuthoredReconciliationPath.string());
        return Result;
    }
    if (!ReadTextFile(Options.AuthoredValidationPath, AuthoredValidationJson))
    {
        Result.Errors.push_back("failed to read D1-4B validation artifact: " + Options.AuthoredValidationPath.string());
        return Result;
    }

    RetargetPoseValidationOptions LocalOptions = Options;
    if (LocalOptions.PairName.empty())
    {
        LocalOptions.PairName = PairNameFromPath(LocalOptions.RestPoseDeltaReportPath);
    }

    ValidationEvidence Evidence;
    Evidence.Authoring.Reports = ParseEvidence(IdentityJson, RestDeltaJson, Result.Warnings);
    Evidence.AuthoredRetargetPoseState = FindStringField(AuthoredRetargetPoseJson, "retarget_pose_state").value_or("");
    Evidence.AuthoredReconciliationState =
        FindStringField(AuthoredReconciliationJson, "retarget_pose_state").value_or("");
    Evidence.AuthoredValidationAggregateStatus =
        FindStringField(AuthoredValidationJson, "aggregate_status").value_or("");
    Evidence.bAuthoredArtifactsReadable = !Evidence.AuthoredRetargetPoseState.empty() &&
                                          !Evidence.AuthoredReconciliationState.empty() &&
                                          !Evidence.AuthoredValidationAggregateStatus.empty();

    if (!ParseNormalizedRestPoseArtifact(LocalOptions.SourceRestPosePath, Evidence.Authoring.SourceRest, Result.Errors) ||
        !ParseNormalizedRestPoseArtifact(LocalOptions.CandidateRestPosePath,
                                         Evidence.Authoring.CandidateRest,
                                         Result.Errors))
    {
        return Result;
    }

    const bool bHashProvenanceMatches = HashProvenanceMatches(Evidence.Authoring.Reports);
    const bool bReportBooleansConsistent =
        Evidence.Authoring.Reports.IdentityTopologyMatch == Evidence.Authoring.Reports.DeltaTopologyMatch &&
        Evidence.Authoring.Reports.IdentityRestPoseMatch == Evidence.Authoring.Reports.DeltaRestPoseMatch;
    Evidence.Authoring.bRestPoseHashesMatchReports =
        bHashProvenanceMatches &&
        Evidence.Authoring.SourceRest.RestPoseHash == Evidence.Authoring.Reports.IdentityReferenceRestHash &&
        Evidence.Authoring.CandidateRest.RestPoseHash == Evidence.Authoring.Reports.IdentityCandidateRestHash;
    Evidence.Authoring.bTopologyMatches =
        Evidence.Authoring.Reports.IdentityTopologyMatch && Evidence.Authoring.Reports.DeltaTopologyMatch &&
        RestPoseTopologyMatches(Evidence.Authoring.SourceRest, Evidence.Authoring.CandidateRest, Result.Warnings);
    Evidence.Authoring.bAxisBasisConsistent =
        Evidence.Authoring.SourceRest.PoseSpace == "internal_normalized" &&
        Evidence.Authoring.CandidateRest.PoseSpace == "internal_normalized" &&
        Evidence.Authoring.SourceRest.Unit == "cm" && Evidence.Authoring.CandidateRest.Unit == "cm" &&
        !Evidence.Authoring.SourceRest.AxisSummary.empty() &&
        Evidence.Authoring.SourceRest.AxisSummary == Evidence.Authoring.CandidateRest.AxisSummary;

    if (Evidence.Authoring.bTopologyMatches)
    {
        Evidence.Authoring.BoneDeltas =
            BuildAuthoredBoneDeltas(Evidence.Authoring.SourceRest, Evidence.Authoring.CandidateRest);
    }
    SummarizeAuthoredDeltas(Evidence.Authoring);

    const bool bAuthoredStateReadyForValidation =
        Evidence.AuthoredRetargetPoseState == "retarget_pose_defined_unvalidated" &&
        Evidence.AuthoredReconciliationState == "retarget_pose_defined_unvalidated";
    Evidence.Lanes.push_back(MakeLane("artifact_provenance_hash",
                                      Evidence.Authoring.Reports.Parsed && bHashProvenanceMatches &&
                                          bReportBooleansConsistent &&
                                          Evidence.Authoring.bRestPoseHashesMatchReports &&
                                          Evidence.bAuthoredArtifactsReadable && bAuthoredStateReadyForValidation,
                                      "Validate identity/rest-delta hashes, normalized rest hashes, and D1-4B authored artifact states.",
                                      "regenerate_matching_D1-4B_artifacts_before_validation"));
    Evidence.Lanes.push_back(MakeLane("axis_basis_consistency",
                                      Evidence.Authoring.bAxisBasisConsistent,
                                      "Validate both rest artifacts are internal_normalized cm and share the same axis summary.",
                                      "regenerate_normalized_rest_artifacts_with_matching_axis_basis"));
    Evidence.Lanes.push_back(MakeLane("topology_and_per_bone_coverage",
                                      Evidence.Authoring.bTopologyMatches && Evidence.Authoring.bPerBoneCoverageComplete,
                                      "Validate matching topology and one authored delta per selected bone.",
                                      "fix_topology_or_mapping_before_validation"));
    Evidence.Lanes.push_back(MakeLane("required_chain_coverage",
                                      Evidence.Authoring.bRequiredCoverageComplete,
                                      "Validate all required/review-relevant bones have authored RetargetPose deltas.",
                                      "author_missing_required_chain_deltas_before_validation"));

    double MaxQuaternionLengthError = 0.0;
    ValidationLane QuaternionLane = ValidateQuaternionLane(Evidence.Authoring, MaxQuaternionLengthError);
    Evidence.MaxQuaternionLengthError = MaxQuaternionLengthError;
    Evidence.Lanes.push_back(QuaternionLane);

    std::vector<TransformRT> ReconciledLocals;
    ValidationLane LocalLane = ValidateLocalFormulaLane(Evidence.Authoring, ReconciledLocals);
    Evidence.MaxLocalTranslationErrorCm = LocalLane.MaxTranslationErrorCm;
    Evidence.MaxLocalRotationErrorDegrees = LocalLane.MaxRotationErrorDegrees;
    Evidence.MaxLocalScaleErrorAbs = LocalLane.MaxScaleErrorAbs;
    Evidence.Lanes.push_back(LocalLane);

    ValidationLane ModelLane = ValidateModelRebuildLane(Evidence.Authoring, ReconciledLocals);
    Evidence.MaxModelTranslationErrorCm = ModelLane.MaxTranslationErrorCm;
    Evidence.MaxModelRotationErrorDegrees = ModelLane.MaxRotationErrorDegrees;
    Evidence.MaxModelScaleErrorAbs = ModelLane.MaxScaleErrorAbs;
    Evidence.Lanes.push_back(ModelLane);

    Evidence.Lanes.push_back(ValidateRootGlobalLane(Evidence.Authoring, LocalLane));
    Evidence.Lanes.push_back(MakeLane("optional_deferred_visibility",
                                      true,
                                      "Validate optional/finger/helper deltas remain visible diagnostics and are not accepted for readiness.",
                                      "keep_optional_deferrals_visible_until_PM_supervisor_acceptance"));

    Evidence.bAllLanesPass = AllValidationLanesPass(Evidence.Lanes);

    RestReconciliationInput Input;
    Input.ArtifactProvenanceMatches = Evidence.Lanes[0].Status == "pass";
    Input.AxisBasisConsistent = Evidence.Lanes[1].Status == "pass";
    Input.TopologyMatch = Evidence.Authoring.bTopologyMatches;
    Input.RestPoseMatch = Evidence.Authoring.Reports.IdentityRestPoseMatch && Evidence.Authoring.Reports.DeltaRestPoseMatch;
    Input.HasNumericRestDeltaEvidence = Evidence.Authoring.Reports.RestDeltaStatus != "not_available" &&
                                        Evidence.Authoring.Reports.ComparedBoneCount > 0 &&
                                        !Evidence.Authoring.BoneDeltas.empty();
    Input.RetargetPoseDefined = bAuthoredStateReadyForValidation && Evidence.Authoring.bRequiredCoverageComplete;
    Input.RetargetPoseValidated = Evidence.bAllLanesPass;
    Input.HasRequiredChainBlocker = !Evidence.bAllLanesPass;
    if (!Evidence.bAllLanesPass)
    {
        for (const ValidationLane& Lane : Evidence.Lanes)
        {
            if (Lane.Status != "pass")
            {
                Input.RequiredChainBlockers.push_back(Lane.Name);
            }
        }
    }
    Result.Reconciliation = EvaluateRestReconciliation(Input);

    if (Result.Reconciliation.State == RetargetPoseState::RestReconciliationValidated)
    {
        Result.Reconciliation.Issues.push_back(
            {"info",
             "REST_RECONCILIATION_VALIDATED_REPORT_ONLY",
             "D1-4C validation lanes passed for rest reconciliation, but sampling/export/BaseFK/retarget gates remain separate.",
             "route_to_PM_supervisor_before_any_sampled_local_pose_or_export_slice"});
        Result.Reconciliation.Issues.push_back(
            {"info",
             "OPTIONAL_DEFERRED_DELTAS_VISIBLE_NOT_ACCEPTED",
             "Optional/finger/helper deltas remain visible diagnostics and are not accepted for full readiness.",
             "PM_supervisor_must_accept_any_optional_deferral_before_readiness"});
    }
    else
    {
        Result.Reconciliation.Issues.push_back(
            {"error",
             "REST_RECONCILIATION_VALIDATION_BLOCKED",
             "One or more D1-4C validation lanes failed or were unavailable; state remains conservative.",
             "review_failed_validation_lanes_before_next_slice"});
    }

    const std::filesystem::path OutputDirectory = LocalOptions.OutputDirectory.empty() ? std::filesystem::current_path()
                                                                                       : LocalOptions.OutputDirectory;
    const std::string ArtifactPrefix = LocalOptions.PairName + ".D1-4C";
    const std::filesystem::path RetargetPosePath = OutputDirectory / (ArtifactPrefix + ".retarget_pose.json");
    const std::filesystem::path ReconciliationPath =
        OutputDirectory / (ArtifactPrefix + ".rest_reconciliation.json");
    const std::filesystem::path ValidationPath =
        OutputDirectory / (ArtifactPrefix + ".rest_reconciliation_validation.json");
    const std::filesystem::path SummaryPath =
        OutputDirectory / (ArtifactPrefix + ".rest_reconciliation_summary.md");

    Result.Artifacts.push_back({RetargetPosePath,
                                BuildValidatedRetargetPoseJson(LocalOptions,
                                                               Evidence,
                                                               Result.Reconciliation,
                                                               RetargetPosePath,
                                                               ReconciliationPath,
                                                               ValidationPath)});
    Result.Artifacts.push_back({ReconciliationPath,
                                BuildValidatedRestReconciliationJson(LocalOptions,
                                                                     Evidence,
                                                                     Result.Reconciliation,
                                                                     RetargetPosePath,
                                                                     ReconciliationPath,
                                                                     ValidationPath)});
    Result.Artifacts.push_back({ValidationPath, BuildValidatedValidationJson(Evidence, Result.Reconciliation)});
    Result.Artifacts.push_back({SummaryPath, BuildValidatedSummaryMarkdown(LocalOptions, Evidence, Result.Reconciliation)});

    std::ostringstream Console;
    Console << "SKRTG validate-rest-reconciliation OK\n"
            << "Pair: " << LocalOptions.PairName << "\n"
            << "retarget_pose_state: " << ToString(Result.Reconciliation.State) << "\n"
            << "validation_lanes: " << (Evidence.bAllLanesPass ? "pass" : "blocked") << "\n"
            << "max_local_error: " << Evidence.MaxLocalTranslationErrorCm << " cm, "
            << Evidence.MaxLocalRotationErrorDegrees << " deg, " << Evidence.MaxLocalScaleErrorAbs << " scale\n"
            << "max_model_error: " << Evidence.MaxModelTranslationErrorCm << " cm, "
            << Evidence.MaxModelRotationErrorDegrees << " deg, " << Evidence.MaxModelScaleErrorAbs << " scale\n"
            << "math_export_readiness: " << Result.Reconciliation.Readiness.MathExportReadiness << "\n"
            << "export_smoke_ready: " << BoolString(Result.Reconciliation.Readiness.ExportSmokeReady) << "\n"
            << "retarget_write_allowed: " << BoolString(Result.Reconciliation.Readiness.RetargetWriteAllowed);
    Result.ConsoleSummary = Console.str();
    Result.Success = true;
    return Result;
}
} // namespace skrtg::reconciliation
