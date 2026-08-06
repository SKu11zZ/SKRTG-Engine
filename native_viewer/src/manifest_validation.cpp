#include "manifest_validation.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace skrtg::viewer::skrv
{
namespace
{
using Json = nlohmann::json;

constexpr const char* ManifestSchema = "skrtg.skrv.manifest.v1";
constexpr const char* SourceReviewSchema =
    "skrtg.d1_17b_retarget_review_viewer.v3";
constexpr const char* VerificationSchema =
    "skrtg.d1_17b_mesh_and_fbx_export_verification.v1";
constexpr const char* TransformLayout =
    "frame_major_bone_major_tx_ty_tz_qx_qy_qz_qw_sx_sy_sz";

std::string LowerAscii(std::string Value)
{
    std::transform(
        Value.begin(), Value.end(), Value.begin(),
        [](const unsigned char Character)
        {
            if (Character >= 'A' && Character <= 'Z')
                return static_cast<char>(Character - 'A' + 'a');
            return static_cast<char>(Character);
        });
    return Value;
}

std::string UpperAscii(std::string Value)
{
    std::transform(
        Value.begin(), Value.end(), Value.begin(),
        [](const unsigned char Character)
        {
            if (Character >= 'a' && Character <= 'z')
                return static_cast<char>(Character - 'a' + 'A');
            return static_cast<char>(Character);
        });
    return Value;
}

bool IsSha256(const std::string& Value)
{
    return Value.size() == 64 &&
        std::all_of(
            Value.begin(), Value.end(),
            [](const unsigned char Character)
            {
                return (Character >= '0' && Character <= '9') ||
                    (Character >= 'A' && Character <= 'F') ||
                    (Character >= 'a' && Character <= 'f');
            });
}

bool SafeMultiply(
    const std::uint64_t Left,
    const std::uint64_t Right,
    std::uint64_t& Out)
{
    if (Right != 0 &&
        Left > std::numeric_limits<std::uint64_t>::max() / Right)
        return false;
    Out = Left * Right;
    return true;
}

bool SafeAdd(
    const std::uint64_t Left,
    const std::uint64_t Right,
    std::uint64_t& Out)
{
    if (Left > std::numeric_limits<std::uint64_t>::max() - Right)
        return false;
    Out = Left + Right;
    return true;
}

bool SafeInclusiveFrameSpan(
    const std::int64_t Start,
    const std::int64_t Stop,
    std::uint64_t& Out)
{
    if (Stop < Start) return false;
    if (Start >= 0 || Stop < 0)
    {
        return SafeAdd(
            static_cast<std::uint64_t>(Stop - Start), 1, Out);
    }
    const std::uint64_t NegativeFrames =
        static_cast<std::uint64_t>(-(Start + 1)) + 1;
    const std::uint64_t NonNegativeFrames =
        static_cast<std::uint64_t>(Stop) + 1;
    return SafeAdd(NegativeFrames, NonNegativeFrames, Out);
}

std::string FileNameOf(const std::string& Path)
{
    const std::size_t Slash = Path.find_last_of('/');
    return Slash == std::string::npos ? Path : Path.substr(Slash + 1);
}

struct MeshTotals
{
    std::uint64_t Meshes = 0;
    std::uint64_t ControlPoints = 0;
    std::uint64_t Triangles = 0;
    std::uint64_t SkinDeformers = 0;
    std::uint64_t Clusters = 0;
    std::uint64_t Influences = 0;
    std::uint64_t BlendShapeDeformers = 0;
    std::uint64_t MaterialSlots = 0;
    std::uint64_t MaximumInfluences = 0;
};

struct ClipInfo
{
    std::string Id;
    std::string SourceAnimationHash;
    std::string FoundationExport;
    std::string FinalExport;
    std::uint64_t FrameCount = 0;
};

class ManifestValidator
{
public:
    ManifestValidator(
        const Json& Root,
        const std::vector<IntegrityEntry>& Inventory,
        ManifestSummary& Summary,
        std::vector<std::string>& Errors)
        : Root_(Root), Summary_(Summary), Errors_(Errors)
    {
        for (const IntegrityEntry& Entry : Inventory)
        {
            const std::string Path = Entry.RelativePath.generic_string();
            Inventory_.emplace(LowerAscii(Path), &Entry);
        }
    }

    bool Run()
    {
        Summary_ = {};
        if (!Root_.is_object()) return Fail("$", "root must be an object");
        if (!ExactString(Root_, "schema", ManifestSchema, "$")) return false;
        std::uint64_t ContractVersion = 0;
        if (!UnsignedMember(Root_, "contractVersion", ContractVersion, "$"))
            return false;
        if (ContractVersion != 1)
            return Fail("$.contractVersion", "unsupported contract version");
        Summary_.ContractVersion = 1;
        if (!ExactString(
                Root_, "sourceReviewSchema", SourceReviewSchema, "$"))
            return false;
        if (!ValidateProvenance() || !ValidateStorage() ||
            !ValidateDisplay())
            return false;
        const Json* Counts = RequireObject(Root_, "counts", "$");
        const Json* Snapshot = RequireObject(Root_, "snapshot", "$");
        if (Counts == nullptr || Snapshot == nullptr) return false;
        if (!ReadDeclaredCounts(*Counts) ||
            !ValidateSnapshot(*Snapshot) ||
            !ValidateExports() ||
            !ValidateVerificationContract())
            return false;
        if (!ValidateInventoryCoverage()) return false;
        return true;
    }

private:
    bool Fail(const std::string& Path, const std::string& Message)
    {
        Errors_.push_back("manifest semantic error at " + Path + ": " + Message);
        return false;
    }

    const Json* Require(
        const Json& Object,
        const char* Key,
        const std::string& Path)
    {
        if (!Object.is_object())
        {
            Fail(Path, "expected object");
            return nullptr;
        }
        const auto Found = Object.find(Key);
        if (Found == Object.end())
        {
            Fail(Path + "." + Key, "required field is missing");
            return nullptr;
        }
        return &*Found;
    }

    const Json* RequireObject(
        const Json& Object,
        const char* Key,
        const std::string& Path)
    {
        const Json* Value = Require(Object, Key, Path);
        if (Value != nullptr && !Value->is_object())
        {
            Fail(Path + "." + Key, "must be an object");
            return nullptr;
        }
        return Value;
    }

    const Json* RequireArray(
        const Json& Object,
        const char* Key,
        const std::string& Path)
    {
        const Json* Value = Require(Object, Key, Path);
        if (Value != nullptr && !Value->is_array())
        {
            Fail(Path + "." + Key, "must be an array");
            return nullptr;
        }
        return Value;
    }

    bool StringMember(
        const Json& Object,
        const char* Key,
        std::string& Out,
        const std::string& Path,
        const bool AllowEmpty = false)
    {
        const Json* Value = Require(Object, Key, Path);
        if (Value == nullptr) return false;
        if (!Value->is_string())
            return Fail(Path + "." + Key, "must be a string");
        Out = Value->get<std::string>();
        if (!AllowEmpty && Out.empty())
            return Fail(Path + "." + Key, "must not be empty");
        return true;
    }

    bool ExactString(
        const Json& Object,
        const char* Key,
        const std::string& Expected,
        const std::string& Path)
    {
        std::string Value;
        if (!StringMember(Object, Key, Value, Path)) return false;
        if (Value != Expected)
            return Fail(Path + "." + Key, "unsupported or unexpected value");
        return true;
    }

    bool BoolMember(
        const Json& Object,
        const char* Key,
        bool& Out,
        const std::string& Path)
    {
        const Json* Value = Require(Object, Key, Path);
        if (Value == nullptr) return false;
        if (!Value->is_boolean())
            return Fail(Path + "." + Key, "must be boolean");
        Out = Value->get<bool>();
        return true;
    }

    bool UnsignedValue(
        const Json& Value,
        std::uint64_t& Out,
        const std::string& Path)
    {
        if (Value.is_number_unsigned())
        {
            Out = Value.get<std::uint64_t>();
            return true;
        }
        if (Value.is_number_integer())
        {
            const std::int64_t Signed = Value.get<std::int64_t>();
            if (Signed >= 0)
            {
                Out = static_cast<std::uint64_t>(Signed);
                return true;
            }
        }
        return Fail(Path, "must be a non-negative integer");
    }

    bool UnsignedMember(
        const Json& Object,
        const char* Key,
        std::uint64_t& Out,
        const std::string& Path)
    {
        const Json* Value = Require(Object, Key, Path);
        return Value != nullptr &&
            UnsignedValue(*Value, Out, Path + "." + Key);
    }

    bool SignedValue(
        const Json& Value,
        std::int64_t& Out,
        const std::string& Path)
    {
        if (!Value.is_number_integer())
            return Fail(Path, "must be an integer");
        if (Value.is_number_unsigned())
        {
            const std::uint64_t Unsigned = Value.get<std::uint64_t>();
            if (Unsigned >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
                return Fail(Path, "integer is out of range");
            Out = static_cast<std::int64_t>(Unsigned);
        }
        else Out = Value.get<std::int64_t>();
        return true;
    }

    bool SignedMember(
        const Json& Object,
        const char* Key,
        std::int64_t& Out,
        const std::string& Path)
    {
        const Json* Value = Require(Object, Key, Path);
        return Value != nullptr &&
            SignedValue(*Value, Out, Path + "." + Key);
    }

    bool NumberValue(
        const Json& Value,
        double& Out,
        const std::string& Path)
    {
        if (!Value.is_number()) return Fail(Path, "must be a number");
        Out = Value.get<double>();
        if (!std::isfinite(Out)) return Fail(Path, "must be finite");
        return true;
    }

    bool NumberMember(
        const Json& Object,
        const char* Key,
        double& Out,
        const std::string& Path)
    {
        const Json* Value = Require(Object, Key, Path);
        return Value != nullptr &&
            NumberValue(*Value, Out, Path + "." + Key);
    }

    bool ShaMember(
        const Json& Object,
        const char* Key,
        std::string& Out,
        const std::string& Path)
    {
        if (!StringMember(Object, Key, Out, Path)) return false;
        if (!IsSha256(Out))
            return Fail(Path + "." + Key, "must be a SHA-256 hex digest");
        Out = UpperAscii(Out);
        return true;
    }

    bool ValidateProvenance()
    {
        const Json* Value = RequireObject(Root_, "provenance", "$");
        if (Value == nullptr) return false;
        std::string Hash;
        bool Recomputed = true;
        bool Reserialized = true;
        if (!ShaMember(*Value, "sourceReviewDataSha256", Hash,
                       "$.provenance") ||
            !ShaMember(*Value, "sourceVerificationSha256", Hash,
                       "$.provenance") ||
            !ExactString(*Value, "adapter",
                         "skrtg.review_html_to_skrv_payload.v1",
                         "$.provenance") ||
            !BoolMember(*Value, "algorithmRecomputed", Recomputed,
                        "$.provenance"))
            return false;
        if (Recomputed)
            return Fail("$.provenance.algorithmRecomputed", "must be false");
        if (!BoolMember(*Value, "fbxReserialized", Reserialized,
                        "$.provenance"))
            return false;
        if (Reserialized)
            return Fail("$.provenance.fbxReserialized", "must be false");
        return true;
    }

    bool ValidateStorage()
    {
        const Json* Value = RequireObject(Root_, "storageContract", "$");
        if (Value == nullptr) return false;
        return ExactString(*Value, "scalarEncoding", "IEEE_754_or_uint32",
                           "$.storageContract") &&
            ExactString(*Value, "byteOrder", "little_endian",
                        "$.storageContract") &&
            ExactString(*Value, "translationUnit", "centimeter",
                        "$.storageContract") &&
            ExactString(*Value, "quaternionOrder", "x_y_z_w",
                        "$.storageContract") &&
            ExactString(*Value, "transformFloat32Layout",
                        "tx_ty_tz_qx_qy_qz_qw_sx_sy_sz",
                        "$.storageContract") &&
            ExactString(*Value, "meshClusterBindOffsetLayout",
                        "row_major_3x4_affine",
                        "$.storageContract") &&
            ExactString(*Value, "packageForm", "auditable_directory",
                        "$.storageContract");
    }

    bool ExactStringArray(
        const Json& Object,
        const char* Key,
        const std::vector<std::string>& Expected,
        const std::string& Path)
    {
        const Json* Values = RequireArray(Object, Key, Path);
        if (Values == nullptr) return false;
        if (Values->size() != Expected.size())
            return Fail(Path + "." + Key, "array length is incorrect");
        for (std::size_t Index = 0; Index < Expected.size(); ++Index)
        {
            if (!(*Values)[Index].is_string() ||
                (*Values)[Index].get<std::string>() != Expected[Index])
            {
                return Fail(
                    Path + "." + Key + "[" + std::to_string(Index) + "]",
                    "unexpected value");
            }
        }
        return true;
    }

    bool RequireTrue(
        const Json& Object,
        const char* Key,
        const std::string& Path)
    {
        bool Value = false;
        if (!BoolMember(Object, Key, Value, Path)) return false;
        return Value || Fail(Path + "." + Key, "must be true");
    }

    bool RequireFalse(
        const Json& Object,
        const char* Key,
        const std::string& Path)
    {
        bool Value = true;
        if (!BoolMember(Object, Key, Value, Path)) return false;
        return !Value || Fail(Path + "." + Key, "must be false");
    }

    bool ExactUnsigned(
        const Json& Object,
        const char* Key,
        const std::uint64_t Expected,
        const std::string& Path)
    {
        std::uint64_t Value = 0;
        if (!UnsignedMember(Object, Key, Value, Path)) return false;
        return Value == Expected ||
            Fail(Path + "." + Key, "has an unexpected value");
    }

    bool ValidateDisplay()
    {
        const Json* Value = RequireObject(Root_, "displayContract", "$");
        if (Value == nullptr) return false;
        double GhostOpacity = 0.0;
        double ResultOpacity = 0.0;
        if (!ExactStringArray(
                *Value, "columns",
                {"original_source_model_space",
                 "fk_plus_anchor_aligned_original_10_percent",
                 "foundation_fk_ik_plus_anchor_aligned_original_10_percent",
                 "final_result_only"},
                "$.displayContract") ||
            !NumberMember(*Value, "sourceGhostOpacity", GhostOpacity,
                          "$.displayContract") ||
            !NumberMember(*Value, "resultOpacity", ResultOpacity,
                          "$.displayContract"))
            return false;
        if (std::abs(GhostOpacity - 0.1) > 1.0e-12)
            return Fail(
                "$.displayContract.sourceGhostOpacity", "must equal 0.1");
        if (std::abs(ResultOpacity - 1.0) > 1.0e-12)
            return Fail("$.displayContract.resultOpacity", "must equal 1.0");
        return ExactStringArray(
                *Value, "synchronized",
                {"frame", "camera", "projection", "display_scale"},
                "$.displayContract") &&
            RequireTrue(*Value, "originalIndependentPanelKeepsOriginalSpace",
                        "$.displayContract") &&
            RequireTrue(*Value, "overlaysUseExplicitAnchorAlignedSourceGhost",
                        "$.displayContract") &&
            RequireTrue(*Value, "nonIkBonesReducedOpacity",
                        "$.displayContract") &&
            RequireTrue(*Value, "virtualGroundIsDisplayOnly",
                        "$.displayContract") &&
            RequireTrue(*Value, "goalHistoryIsDisplayOnly",
                        "$.displayContract") &&
            RequireTrue(*Value, "noGroundOrContactSemanticsClaimed",
                        "$.displayContract");
    }

    bool ReadDeclaredCounts(const Json& Counts)
    {
        if (!UnsignedMember(Counts, "clipCount", Summary_.ClipCount,
                            "$.counts") ||
            !UnsignedMember(Counts, "frameCount", Summary_.FrameCount,
                            "$.counts") ||
            !UnsignedMember(Counts, "sourceBoneCount",
                            Summary_.SourceBoneCount, "$.counts") ||
            !UnsignedMember(Counts, "targetBoneCount",
                            Summary_.TargetBoneCount, "$.counts") ||
            !UnsignedMember(Counts, "mappedChainCount",
                            Summary_.MappedChainCount, "$.counts") ||
            !UnsignedMember(Counts, "goalChainCount",
                            Summary_.GoalChainCount, "$.counts"))
            return false;
        if (Summary_.ClipCount == 0 || Summary_.FrameCount == 0 ||
            Summary_.SourceBoneCount == 0 || Summary_.TargetBoneCount == 0)
            return Fail(
                "$.counts",
                "clip, frame, source bone, and target bone counts must be positive");
        return true;
    }

    bool ValidateBoneArray(
        const Json& Bones,
        const std::string& Path)
    {
        if (!Bones.is_array() || Bones.empty())
            return Fail(Path, "must be a non-empty array");
        std::size_t RootCount = 0;
        for (std::size_t Index = 0; Index < Bones.size(); ++Index)
        {
            const Json& Bone = Bones[Index];
            const std::string BonePath =
                Path + "[" + std::to_string(Index) + "]";
            if (!Bone.is_array() || Bone.size() != 7)
                return Fail(BonePath, "bone record must contain seven fields");
            std::int64_t Parent = 0;
            if (!SignedValue(Bone[0], Parent, BonePath + "[0]") ||
                Parent < -1 || Parent >= static_cast<std::int64_t>(Index))
                return Fail(BonePath + "[0]", "parent index is invalid");
            if (Parent == -1) ++RootCount;
            if (!Bone[1].is_string() || Bone[1].get<std::string>().empty() ||
                !Bone[2].is_string() || Bone[2].get<std::string>().empty() ||
                !Bone[3].is_boolean())
                return Fail(BonePath, "bone name/path/IK flag is invalid");
            for (std::size_t Component = 4; Component < 7; ++Component)
            {
                double Number = 0.0;
                if (!NumberValue(Bone[Component], Number,
                                 BonePath + "[" +
                                     std::to_string(Component) + "]"))
                    return false;
            }
        }
        return RootCount == 1 ||
            Fail(Path, "skeleton must contain exactly one root bone");
    }

    bool ValidBoneIndex(
        const std::int64_t Index,
        const std::size_t Count) const
    {
        return Index >= 0 &&
            static_cast<std::uint64_t>(Index) < Count;
    }

    bool ValidateRootPelvis(
        const Json& Snapshot,
        const std::size_t SourceCount,
        const std::size_t TargetCount)
    {
        const Json* Value = RequireObject(Snapshot, "rootPelvis", "$.snapshot");
        if (Value == nullptr) return false;
        std::int64_t SourceRoot = -1;
        std::int64_t SourcePelvis = -1;
        std::int64_t TargetHips = -1;
        std::string Text;
        return SignedMember(*Value, "sourceRoot", SourceRoot,
                            "$.snapshot.rootPelvis") &&
            SignedMember(*Value, "sourcePelvis", SourcePelvis,
                         "$.snapshot.rootPelvis") &&
            SignedMember(*Value, "targetHips", TargetHips,
                         "$.snapshot.rootPelvis") &&
            ValidBoneIndex(SourceRoot, SourceCount) &&
            ValidBoneIndex(SourcePelvis, SourceCount) &&
            ValidBoneIndex(TargetHips, TargetCount) &&
            StringMember(*Value, "rootOwnership", Text,
                         "$.snapshot.rootPelvis") &&
            StringMember(*Value, "pelvisOwnership", Text,
                         "$.snapshot.rootPelvis") &&
            StringMember(*Value, "scaleOwnership", Text,
                         "$.snapshot.rootPelvis");
    }

    bool ValidateChainIndices(
        const Json& Indices,
        const Json& Bones,
        const std::string& Path)
    {
        if (!Indices.is_array() || Indices.empty())
            return Fail(Path, "chain must be a non-empty array");
        std::int64_t Previous = -1;
        for (std::size_t Position = 0; Position < Indices.size(); ++Position)
        {
            std::int64_t Index = -1;
            if (!SignedValue(Indices[Position], Index,
                             Path + "[" + std::to_string(Position) + "]") ||
                !ValidBoneIndex(Index, Bones.size()))
                return Fail(Path, "chain bone index is invalid");
            if (Position != 0)
            {
                std::int64_t Parent = -1;
                if (!SignedValue(Bones[static_cast<std::size_t>(Index)][0],
                                 Parent, Path) || Parent != Previous)
                    return Fail(Path, "chain is not parent-contiguous");
            }
            Previous = Index;
        }
        return true;
    }

    bool ValidateChains(
        const Json& Chains,
        const Json& SourceBones,
        const Json& TargetBones)
    {
        if (!Chains.is_array()) return Fail("$.snapshot.retargetChains", "must be array");
        std::set<std::string> Labels;
        std::uint64_t GoalCount = 0;
        std::uint64_t LimbIkGoalCount = 0;
        std::uint64_t FingerIkGoalCount = 0;
        for (std::size_t Index = 0; Index < Chains.size(); ++Index)
        {
            const Json& Chain = Chains[Index];
            const std::string Path =
                "$.snapshot.retargetChains[" + std::to_string(Index) + "]";
            if (!Chain.is_object()) return Fail(Path, "must be object");
            std::string Label;
            std::string Mode;
            if (!StringMember(Chain, "label", Label, Path) ||
                !Labels.insert(Label).second ||
                !StringMember(Chain, "ikMode", Mode, Path))
                return Fail(Path, "chain label or mode is invalid");
            const Json* Source = RequireArray(Chain, "source", Path);
            const Json* Target = RequireArray(Chain, "target", Path);
            if (Source == nullptr || Target == nullptr ||
                !ValidateChainIndices(*Source, SourceBones, Path + ".source") ||
                !ValidateChainIndices(*Target, TargetBones, Path + ".target"))
                return false;
            if (Mode != "fk_only" && Mode != "two_bone" && Mode != "finger")
                return Fail(Path + ".ikMode", "unsupported IK mode");
            std::int64_t SourceGoal = -1;
            std::int64_t TargetGoal = -1;
            std::int64_t SourcePole = -1;
            std::int64_t TargetPole = -1;
            std::string GoalName;
            if (!SignedMember(Chain, "sourceGoalBone", SourceGoal, Path) ||
                !SignedMember(Chain, "targetGoalBone", TargetGoal, Path) ||
                !SignedMember(Chain, "sourcePoleBone", SourcePole, Path) ||
                !SignedMember(Chain, "targetPoleBone", TargetPole, Path) ||
                !StringMember(Chain, "sourceGoalName", GoalName, Path, true) ||
                !StringMember(Chain, "targetGoalName", GoalName, Path, true))
                return false;
            if (SourceGoal < -1 || TargetGoal < -1 ||
                SourcePole < -1 || TargetPole < -1)
                return Fail(
                    Path,
                    "Goal and pole indices must be -1 or valid bone indices");
            if (Mode != "fk_only")
            {
                ++GoalCount;
                if (Mode == "two_bone") ++LimbIkGoalCount;
                else if (Mode == "finger") ++FingerIkGoalCount;
                if (!ValidBoneIndex(SourceGoal, SourceBones.size()) ||
                    !ValidBoneIndex(TargetGoal, TargetBones.size()) ||
                    !ValidBoneIndex(SourcePole, SourceBones.size()) ||
                    !ValidBoneIndex(TargetPole, TargetBones.size()))
                    return Fail(Path, "IK Goal or pole index is invalid");
            }
        }
        if (Chains.size() != Summary_.MappedChainCount ||
            GoalCount != Summary_.GoalChainCount)
            return Fail("$.counts", "chain counts do not match snapshot");
        LimbIkGoalCount_ = LimbIkGoalCount;
        FingerIkGoalCount_ = FingerIkGoalCount;
        return true;
    }

    bool ValidateAnchors(
        const Json& Anchors,
        const std::size_t SourceCount,
        const std::size_t TargetCount)
    {
        if (!Anchors.is_array() || Anchors.empty())
            return Fail("$.snapshot.anchors", "must be a non-empty array");
        for (std::size_t Index = 0; Index < Anchors.size(); ++Index)
        {
            const Json& Anchor = Anchors[Index];
            const std::string Path =
                "$.snapshot.anchors[" + std::to_string(Index) + "]";
            if (!Anchor.is_object()) return Fail(Path, "must be object");
            std::string Text;
            std::int64_t Source = -1;
            std::int64_t Target = -1;
            if (!StringMember(Anchor, "label", Text, Path) ||
                !StringMember(Anchor, "sourcePath", Text, Path) ||
                !StringMember(Anchor, "targetPath", Text, Path) ||
                !SignedMember(Anchor, "sourceBone", Source, Path) ||
                !SignedMember(Anchor, "targetBone", Target, Path) ||
                !ValidBoneIndex(Source, SourceCount) ||
                !ValidBoneIndex(Target, TargetCount))
                return Fail(Path, "anchor identity is invalid");
            const Json* Basis = RequireArray(Anchor, "basis", Path);
            if (Basis == nullptr || Basis->size() != 4)
                return Fail(Path + ".basis", "must contain four values");
            double NormSquared = 0.0;
            for (std::size_t Component = 0; Component < 4; ++Component)
            {
                double Number = 0.0;
                if (!NumberValue((*Basis)[Component], Number,
                                 Path + ".basis[" +
                                     std::to_string(Component) + "]"))
                    return false;
                NormSquared += Number * Number;
            }
            if (std::abs(NormSquared - 1.0) > 1.0e-3)
                return Fail(Path + ".basis", "quaternion is not normalized");
        }
        return true;
    }

    bool ValidateBlobDescriptor(
        const Json& Descriptor,
        const char* ScalarType,
        const std::optional<std::uint64_t> ExpectedElements,
        const std::string& Path,
        std::uint64_t& OutElements)
    {
        if (!Descriptor.is_object()) return Fail(Path, "must be an object");
        std::string BlobPath;
        std::string Hash;
        std::string Scalar;
        std::string ByteOrder;
        std::uint64_t ByteCount = 0;
        if (!StringMember(Descriptor, "path", BlobPath, Path) ||
            !StringMember(Descriptor, "scalarType", Scalar, Path) ||
            !StringMember(Descriptor, "byteOrder", ByteOrder, Path) ||
            !UnsignedMember(Descriptor, "elementCount", OutElements, Path) ||
            !UnsignedMember(Descriptor, "byteCount", ByteCount, Path) ||
            !ShaMember(Descriptor, "sha256", Hash, Path))
            return false;
        if (Scalar != ScalarType || ByteOrder != "little_endian")
            return Fail(Path, "scalar type or byte order is incorrect");
        if (ExpectedElements.has_value() &&
            OutElements != *ExpectedElements)
            return Fail(Path + ".elementCount", "does not match semantic dimensions");
        std::uint64_t ExpectedBytes = 0;
        if (!SafeMultiply(OutElements, 4, ExpectedBytes) ||
            ByteCount != ExpectedBytes)
            return Fail(Path + ".byteCount", "does not equal elementCount * 4");
        const std::string ExpectedBlobPath = "blobs/" + Hash + ".bin";
        if (BlobPath != ExpectedBlobPath)
            return Fail(
                Path + ".path",
                "must be the content-addressed path blobs/<SHA256>.bin");
        const auto Found = Inventory_.find(LowerAscii(BlobPath));
        if (Found == Inventory_.end() ||
            Found->second->RelativePath.generic_string() != BlobPath ||
            Found->second->Role != EntryRole::Blob ||
            Found->second->ByteCount != ByteCount ||
            UpperAscii(Found->second->Sha256) != Hash)
            return Fail(Path, "descriptor does not match indexed blob");
        ReferencedBlobs_.insert(LowerAscii(BlobPath));
        return true;
    }

    bool ValidateMesh(
        const Json& Mesh,
        const std::string& Path,
        MeshTotals& Totals)
    {
        if (!Mesh.is_object()) return Fail(Path, "must be an object");
        std::string Text;
        std::string SkinMode;
        std::uint64_t ControlPoints = 0;
        std::uint64_t Triangles = 0;
        std::uint64_t Clusters = 0;
        std::uint64_t SkinDeformers = 0;
        std::uint64_t BlendShapes = 0;
        std::uint64_t Materials = 0;
        std::uint64_t MaximumInfluences = 0;
        if (!StringMember(Mesh, "name", Text, Path) ||
            !StringMember(Mesh, "path", Text, Path) ||
            !StringMember(Mesh, "skinMode", SkinMode, Path) ||
            (SkinMode != "normalize" && SkinMode != "total_one") ||
            !UnsignedMember(Mesh, "controlPointCount", ControlPoints, Path) ||
            !UnsignedMember(Mesh, "triangleCount", Triangles, Path) ||
            !UnsignedMember(Mesh, "clusterCount", Clusters, Path) ||
            !UnsignedMember(Mesh, "skinDeformerCount", SkinDeformers, Path) ||
            !UnsignedMember(Mesh, "blendShapeDeformerCount", BlendShapes, Path) ||
            !UnsignedMember(Mesh, "materialSlotCount", Materials, Path) ||
            !UnsignedMember(Mesh, "maximumInfluencesPerControlPoint",
                            MaximumInfluences, Path))
            return Fail(Path, "mesh metadata is invalid");
        const Json* Fallback = RequireArray(Mesh, "fallback", Path);
        const Json* Arrays = RequireObject(Mesh, "arrays", Path);
        if (Fallback == nullptr || Arrays == nullptr || Fallback->size() != 12)
            return Fail(Path + ".fallback", "must contain 12 finite values");
        for (std::size_t Index = 0; Index < Fallback->size(); ++Index)
        {
            double Number = 0.0;
            if (!NumberValue((*Fallback)[Index], Number,
                             Path + ".fallback[" +
                                 std::to_string(Index) + "]"))
                return false;
        }
        const Json* Positions =
            Require(*Arrays, "positions", Path + ".arrays");
        const Json* TriangleIndices =
            Require(*Arrays, "triangleIndices", Path + ".arrays");
        const Json* InfluenceOffsets =
            Require(*Arrays, "influenceOffsets", Path + ".arrays");
        const Json* InfluenceClusterIndices =
            Require(*Arrays, "influenceClusterIndices", Path + ".arrays");
        const Json* InfluenceWeights =
            Require(*Arrays, "influenceWeights", Path + ".arrays");
        const Json* ClusterBoneIndices =
            Require(*Arrays, "clusterBoneIndices", Path + ".arrays");
        const Json* ClusterBindOffsets =
            Require(*Arrays, "clusterBindOffsets3x4", Path + ".arrays");
        if (Positions == nullptr || TriangleIndices == nullptr ||
            InfluenceOffsets == nullptr ||
            InfluenceClusterIndices == nullptr ||
            InfluenceWeights == nullptr || ClusterBoneIndices == nullptr ||
            ClusterBindOffsets == nullptr)
            return false;
        std::uint64_t Expected = 0;
        std::uint64_t Elements = 0;
        std::uint64_t InfluenceOffsetElements = 0;
        if (!SafeMultiply(ControlPoints, 3, Expected) ||
            !ValidateBlobDescriptor(
                *Positions,
                "float32", Expected, Path + ".arrays.positions", Elements) ||
            !SafeMultiply(Triangles, 3, Expected) ||
            !ValidateBlobDescriptor(
                *TriangleIndices,
                "uint32", Expected, Path + ".arrays.triangleIndices", Elements) ||
            !SafeAdd(ControlPoints, 1, InfluenceOffsetElements) ||
            !ValidateBlobDescriptor(
                *InfluenceOffsets,
                "uint32", InfluenceOffsetElements,
                Path + ".arrays.influenceOffsets", Elements))
            return false;
        std::uint64_t InfluenceCount = 0;
        if (!ValidateBlobDescriptor(
                *InfluenceClusterIndices,
                "uint32", std::nullopt,
                Path + ".arrays.influenceClusterIndices", InfluenceCount) ||
            !ValidateBlobDescriptor(
                *InfluenceWeights,
                "float32", InfluenceCount,
                Path + ".arrays.influenceWeights", Elements) ||
            !ValidateBlobDescriptor(
                *ClusterBoneIndices,
                "uint32", Clusters,
                Path + ".arrays.clusterBoneIndices", Elements) ||
            !SafeMultiply(Clusters, 12, Expected) ||
            !ValidateBlobDescriptor(
                *ClusterBindOffsets,
                "float32", Expected,
                Path + ".arrays.clusterBindOffsets3x4", Elements))
            return false;
        if (MaximumInfluences > InfluenceCount)
            return Fail(Path, "maximum influences exceeds total influences");
        if (!SafeAdd(Totals.Meshes, 1, Totals.Meshes) ||
            !SafeAdd(Totals.ControlPoints, ControlPoints,
                     Totals.ControlPoints) ||
            !SafeAdd(Totals.Triangles, Triangles, Totals.Triangles) ||
            !SafeAdd(Totals.SkinDeformers, SkinDeformers,
                     Totals.SkinDeformers) ||
            !SafeAdd(Totals.Clusters, Clusters, Totals.Clusters) ||
            !SafeAdd(Totals.Influences, InfluenceCount,
                     Totals.Influences) ||
            !SafeAdd(Totals.BlendShapeDeformers, BlendShapes,
                     Totals.BlendShapeDeformers) ||
            !SafeAdd(Totals.MaterialSlots, Materials,
                     Totals.MaterialSlots))
            return Fail(Path, "mesh package totals overflow");
        Totals.MaximumInfluences =
            std::max(Totals.MaximumInfluences, MaximumInfluences);
        return true;
    }

    bool ValidateMeshPackage(
        const Json& Package,
        const std::string& Path)
    {
        if (!Package.is_object()) return Fail(Path, "must be an object");
        std::string Label;
        if (!StringMember(Package, "label", Label, Path)) return false;
        const Json* Meshes = RequireArray(Package, "meshes", Path);
        if (Meshes == nullptr || Meshes->empty())
            return Fail(Path + ".meshes", "must not be empty");
        MeshTotals Totals;
        for (std::size_t Index = 0; Index < Meshes->size(); ++Index)
        {
            if (!ValidateMesh(
                    (*Meshes)[Index],
                    Path + ".meshes[" + std::to_string(Index) + "]",
                    Totals))
                return false;
        }
        struct CountField
        {
            const char* Name;
            std::uint64_t Expected;
        };
        for (const CountField Field : {
                 CountField{"meshCount", Totals.Meshes},
                 {"controlPointCount", Totals.ControlPoints},
                 {"triangleCount", Totals.Triangles},
                 {"skinDeformerCount", Totals.SkinDeformers},
                 {"skinClusterCount", Totals.Clusters},
                 {"influenceCount", Totals.Influences},
                 {"blendShapeDeformerCount", Totals.BlendShapeDeformers},
                 {"materialSlotCount", Totals.MaterialSlots},
                 {"maximumInfluencesPerControlPoint",
                  Totals.MaximumInfluences}})
        {
            std::uint64_t Actual = 0;
            if (!UnsignedMember(Package, Field.Name, Actual, Path) ||
                Actual != Field.Expected)
                return Fail(Path + "." + Field.Name,
                            "does not match mesh inventory");
        }
        double Error = 0.0;
        return NumberMember(
            Package, "maximumBindReconstructionErrorCm", Error, Path) &&
            Error >= 0.0;
    }

    bool SafeExportFileName(const std::string& Value) const
    {
        return !Value.empty() && Value.find('/') == std::string::npos &&
            Value.find('\\') == std::string::npos &&
            Value.find(':') == std::string::npos;
    }

    bool ValidatePoseDescriptor(
        const Json& Clip,
        const char* Key,
        const std::uint64_t ExpectedElements,
        const std::string& Path)
    {
        const Json* Descriptor = Require(Clip, Key, Path);
        if (Descriptor == nullptr) return false;
        std::uint64_t Elements = 0;
        if (!ValidateBlobDescriptor(
                *Descriptor, "float32", ExpectedElements,
                Path + "." + Key, Elements) ||
            !ExactString(*Descriptor, "layout", TransformLayout,
                         Path + "." + Key))
            return false;
        std::uint64_t Stride = 0;
        return UnsignedMember(*Descriptor, "transformStrideFloat32", Stride,
                              Path + "." + Key) && Stride == 10;
    }

    bool ValidateClips(
        const Json& Clips,
        const std::size_t SourceMeshCount,
        const std::size_t SourceBoneCount,
        const std::size_t TargetBoneCount)
    {
        if (!Clips.is_array() || Clips.size() != Summary_.ClipCount)
            return Fail("$.snapshot.clips", "clip count does not match counts");
        std::set<std::string> Ids;
        std::uint64_t TotalFrames = 0;
        for (std::size_t Index = 0; Index < Clips.size(); ++Index)
        {
            const Json& Clip = Clips[Index];
            const std::string Path =
                "$.snapshot.clips[" + std::to_string(Index) + "]";
            if (!Clip.is_object()) return Fail(Path, "must be object");
            ClipInfo Info;
            std::string Label;
            std::uint64_t SourceMeshIndex = 0;
            std::uint64_t FrameCount = 0;
            std::int64_t StartFrame = 0;
            std::int64_t StopFrame = 0;
            std::uint64_t TimingFrameCount = 0;
            double Fps = 0.0;
            if (!StringMember(Clip, "id", Info.Id, Path) ||
                !Ids.insert(Info.Id).second ||
                !StringMember(Clip, "label", Label, Path) ||
                !UnsignedMember(Clip, "sourceMeshIndex", SourceMeshIndex, Path) ||
                SourceMeshIndex >= SourceMeshCount ||
                SourceMeshIndex != Index ||
                !UnsignedMember(Clip, "frameCount", FrameCount, Path) ||
                FrameCount == 0 ||
                !SignedMember(Clip, "startFrame", StartFrame, Path) ||
                !SignedMember(Clip, "stopFrame", StopFrame, Path) ||
                !SafeInclusiveFrameSpan(
                    StartFrame, StopFrame, TimingFrameCount) ||
                TimingFrameCount != FrameCount ||
                !NumberMember(Clip, "fps", Fps, Path) || Fps <= 0.0 ||
                !ShaMember(Clip, "sourceAnimationSha256",
                           Info.SourceAnimationHash, Path) ||
                !StringMember(Clip, "foundationExportFbx",
                              Info.FoundationExport, Path) ||
                !StringMember(Clip, "exportFbx", Info.FinalExport, Path) ||
                !SafeExportFileName(Info.FoundationExport) ||
                !SafeExportFileName(Info.FinalExport))
                return Fail(Path, "clip identity, timing, or export metadata is invalid");
            std::uint64_t SourceElements = 0;
            std::uint64_t TargetElements = 0;
            if (!SafeMultiply(FrameCount, SourceBoneCount, SourceElements) ||
                !SafeMultiply(SourceElements, 10, SourceElements) ||
                !SafeMultiply(FrameCount, TargetBoneCount, TargetElements) ||
                !SafeMultiply(TargetElements, 10, TargetElements) ||
                !ValidatePoseDescriptor(Clip, "sourceTrs", SourceElements, Path) ||
                !ValidatePoseDescriptor(Clip, "fkTrs", TargetElements, Path) ||
                !ValidatePoseDescriptor(
                    Clip, "foundationTrs", TargetElements, Path) ||
                !ValidatePoseDescriptor(Clip, "finalTrs", TargetElements, Path))
                return false;
            bool Flag = false;
            bool OperationStackEnabled = false;
            std::string Status;
            if (!BoolMember(Clip, "sourceMeshFallbackUsed", Flag, Path) ||
                !StringMember(Clip, "limbIkStatus", Status, Path) ||
                (Status != "committed" && Status != "fail_closed") ||
                !BoolMember(Clip, "sourceMotionFootLockEnabled", Flag, Path) ||
                !BoolMember(Clip, "sourceMotionFootLockSuccess", Flag, Path) ||
                !BoolMember(Clip, "sourceMotionFootLockDeterministic", Flag, Path) ||
                !BoolMember(
                    Clip, "sourceMotionFootLockNoGroundOrContactSemanticsUsed",
                    Flag, Path))
                return false;
            if (HasOperationStackMetadata_)
            {
                if (!BoolMember(
                        Clip, "operationStackEnabled",
                        OperationStackEnabled, Path) ||
                    OperationStackEnabled !=
                        OperationStackCandidateEnabled_)
                {
                    return Fail(
                        Path + ".operationStackEnabled",
                        "must match the snapshot Operation System candidate state");
                }
            }
            else if (Clip.contains("operationStackEnabled"))
            {
                return Fail(
                    Path + ".operationStackEnabled",
                    "requires complete snapshot Operation System metadata");
            }
            if (IsUEIKJsonCandidate_ && Status != "committed")
            {
                return Fail(
                    Path + ".limbIkStatus",
                    "UE IK JSON candidate requires all four limb IK "
                    "chains to commit on every frame");
            }
            Info.FrameCount = FrameCount;
            if (!SafeAdd(TotalFrames, FrameCount, TotalFrames))
                return Fail("$.counts.frameCount", "clip frame total overflows");
            const std::string ClipId = Info.Id;
            ClipInfos_.emplace(ClipId, std::move(Info));
        }
        if (TotalFrames != Summary_.FrameCount)
            return Fail("$.counts.frameCount", "does not match clip inventory");
        return true;
    }

    bool ValidateSnapshot(const Json& Snapshot)
    {
        std::string Route;
        std::string FoundationRoute;
        std::string Text;
        bool Flag = false;
        if (!StringMember(Snapshot, "route", Route, "$.snapshot") ||
            !StringMember(
                Snapshot, "foundationRoute", FoundationRoute,
                "$.snapshot") ||
            !BoolMember(Snapshot, "foundationFrozen", Flag, "$.snapshot") ||
            !StringMember(Snapshot, "sourceMotionFootLockRoute", Text,
                          "$.snapshot"))
            return Fail("$.snapshot", "route/foundation metadata is invalid");
        const bool IsUEIKJsonCandidate =
            Route == "ue_ik_json_canonical_bridge_v1" &&
            FoundationRoute ==
                "ue_ik_json_fk_pelvis_limb_ik_candidate_v1" &&
            !Flag;
        if (!Flag && !IsUEIKJsonCandidate)
        {
            return Fail(
                "$.snapshot",
                "only the explicit UE IK JSON candidate route may be "
                "non-frozen");
        }
        IsUEIKJsonCandidate_ = IsUEIKJsonCandidate;
        const bool HasAnyOperationStackMetadata =
            Snapshot.contains("operationStackCandidateEnabled") ||
            Snapshot.contains("operationStackCandidateSelected") ||
            Snapshot.contains("operationStackCandidateAdopted");
        if (HasAnyOperationStackMetadata)
        {
            bool Selected = false;
            bool Adopted = false;
            if (!Snapshot.contains("operationStackCandidateEnabled") ||
                !Snapshot.contains("operationStackCandidateSelected") ||
                !Snapshot.contains("operationStackCandidateAdopted") ||
                !BoolMember(
                    Snapshot, "operationStackCandidateEnabled",
                    OperationStackCandidateEnabled_, "$.snapshot") ||
                !BoolMember(
                    Snapshot, "operationStackCandidateSelected",
                    Selected, "$.snapshot") ||
                !BoolMember(
                    Snapshot, "operationStackCandidateAdopted",
                    Adopted, "$.snapshot"))
            {
                return Fail(
                    "$.snapshot",
                    "Operation System candidate metadata must be complete booleans");
            }
            if (Selected || Adopted)
            {
                return Fail(
                    "$.snapshot",
                    "Operation System v2 is candidate-only and may not be selected or adopted");
            }
            HasOperationStackMetadata_ = true;
        }
        for (const char* Key : {
                 "selected", "adopted", "stageComplete", "route_selected",
                 "route_adopted", "stage_complete",
                 "sourceMotionFootLockCandidateEnabled",
                 "sourceMotionFootLockCandidateSelected",
                 "sourceMotionFootLockCandidateAdopted",
                 "upstreamLimbIkRouteSelected",
                 "upstreamLimbIkRouteAdopted",
                 "spinePelvisFollowCandidateEnabled",
                 "spinePelvisFollowCandidateSelected",
                 "spinePelvisFollowCandidateAdopted"})
        {
            if (!BoolMember(Snapshot, Key, Flag, "$.snapshot")) return false;
            if (IsUEIKJsonCandidate && Flag)
            {
                return Fail(
                    "$.snapshot." + std::string(Key),
                    "must remain false for an unselected UE IK JSON "
                    "candidate route");
            }
        }
        const Json* SourceBones = RequireArray(Snapshot, "sourceBones", "$.snapshot");
        const Json* TargetBones = RequireArray(Snapshot, "targetBones", "$.snapshot");
        const Json* Chains = RequireArray(Snapshot, "retargetChains", "$.snapshot");
        const Json* Anchors = RequireArray(Snapshot, "anchors", "$.snapshot");
        const Json* SourceMeshes = RequireArray(Snapshot, "sourceMeshes", "$.snapshot");
        const Json* TargetMesh = RequireObject(Snapshot, "targetMesh", "$.snapshot");
        const Json* Clips = RequireArray(Snapshot, "clips", "$.snapshot");
        if (SourceBones == nullptr || TargetBones == nullptr || Chains == nullptr ||
            Anchors == nullptr || SourceMeshes == nullptr || TargetMesh == nullptr ||
            Clips == nullptr ||
            !ValidateBoneArray(*SourceBones, "$.snapshot.sourceBones") ||
            !ValidateBoneArray(*TargetBones, "$.snapshot.targetBones"))
            return false;
        if (SourceBones->size() != Summary_.SourceBoneCount ||
            TargetBones->size() != Summary_.TargetBoneCount)
            return Fail("$.counts", "bone counts do not match snapshot");
        if (!ValidateRootPelvis(
                Snapshot, SourceBones->size(), TargetBones->size()) ||
            !ValidateChains(*Chains, *SourceBones, *TargetBones) ||
            !ValidateAnchors(
                *Anchors, SourceBones->size(), TargetBones->size()))
            return false;
        if (SourceMeshes->size() != Summary_.ClipCount)
            return Fail("$.snapshot.sourceMeshes",
                        "requires one package per clip");
        for (std::size_t Index = 0; Index < SourceMeshes->size(); ++Index)
        {
            if (!ValidateMeshPackage(
                    (*SourceMeshes)[Index],
                    "$.snapshot.sourceMeshes[" +
                        std::to_string(Index) + "]"))
                return false;
        }
        return ValidateMeshPackage(*TargetMesh, "$.snapshot.targetMesh") &&
            ValidateClips(
                *Clips, SourceMeshes->size(), SourceBones->size(),
                TargetBones->size());
    }

    bool ValidateExports()
    {
        const Json* Exports = RequireArray(Root_, "verifiedExports", "$");
        std::uint64_t ExpectedExportCount = 0;
        if (Exports == nullptr ||
            !SafeMultiply(Summary_.ClipCount, 2, ExpectedExportCount) ||
            Exports->size() != ExpectedExportCount)
            return Fail("$.verifiedExports",
                        "requires Foundation and Final for every clip");
        std::set<std::pair<std::string, std::string>> ClipLanes;
        for (std::size_t Index = 0; Index < Exports->size(); ++Index)
        {
            const Json& Export = (*Exports)[Index];
            const std::string Path =
                "$.verifiedExports[" + std::to_string(Index) + "]";
            if (!Export.is_object()) return Fail(Path, "must be object");
            std::string ClipId;
            std::string Lane;
            std::string ExportPath;
            std::string Hash;
            std::string Before;
            std::string After;
            std::uint64_t ByteCount = 0;
            std::uint64_t Samples = 0;
            std::uint64_t LocalMismatches = 0;
            std::uint64_t ModelMismatches = 0;
            if (!StringMember(Export, "clip_id", ClipId, Path) ||
                !StringMember(Export, "lane", Lane, Path) ||
                (Lane != "foundation" && Lane != "final") ||
                !ClipLanes.emplace(ClipId, Lane).second ||
                !StringMember(Export, "path", ExportPath, Path) ||
                ExportPath.rfind("exports/", 0) != 0 ||
                !ShaMember(Export, "sha256", Hash, Path) ||
                !UnsignedMember(Export, "byteCount", ByteCount, Path) ||
                ByteCount == 0 ||
                !UnsignedMember(Export, "samples_compared", Samples, Path) ||
                Samples == 0 ||
                !UnsignedMember(Export, "local_mismatch_count",
                                LocalMismatches, Path) ||
                !UnsignedMember(Export, "model_mismatch_count",
                                ModelMismatches, Path) ||
                LocalMismatches != 0 || ModelMismatches != 0 ||
                !StringMember(Export, "mesh_fingerprint_before", Before, Path) ||
                !StringMember(Export, "mesh_fingerprint_after", After, Path) ||
                Before != After)
                return Fail(Path, "export verification record is invalid");
            const auto Clip = ClipInfos_.find(ClipId);
            if (Clip == ClipInfos_.end())
                return Fail(Path + ".clip_id", "does not identify a clip");
            const std::string ExpectedFile = Lane == "foundation"
                ? Clip->second.FoundationExport : Clip->second.FinalExport;
            if (FileNameOf(ExportPath) != ExpectedFile)
                return Fail(Path + ".path", "does not match clip export name");
            std::uint64_t ExpectedSamples = 0;
            if (!SafeMultiply(
                    Clip->second.FrameCount,
                    Summary_.TargetBoneCount,
                    ExpectedSamples) || Samples != ExpectedSamples)
                return Fail(
                    Path + ".samples_compared",
                    "does not cover every target bone in every clip frame");
            const auto Entry = Inventory_.find(LowerAscii(ExportPath));
            if (Entry == Inventory_.end() ||
                Entry->second->RelativePath.generic_string() != ExportPath ||
                Entry->second->Role != EntryRole::Export ||
                Entry->second->ByteCount != ByteCount ||
                UpperAscii(Entry->second->Sha256) != Hash)
                return Fail(Path, "does not match indexed export");
            if (!ReferencedExports_.insert(LowerAscii(ExportPath)).second)
                return Fail(Path + ".path", "export path is duplicated");
        }
        for (const auto& [ClipId, Info] : ClipInfos_)
        {
            if (ClipLanes.find({ClipId, "foundation"}) == ClipLanes.end() ||
                ClipLanes.find({ClipId, "final"}) == ClipLanes.end())
                return Fail("$.verifiedExports", "clip export lane is missing");
        }
        Summary_.VerifiedExportCount = Exports->size();
        return true;
    }

    bool ValidateVerificationContract()
    {
        const Json* Contract =
            RequireObject(Root_, "verificationContract", "$");
        if (Contract == nullptr ||
            !ExactString(*Contract, "schema", VerificationSchema,
                         "$.verificationContract"))
            return false;
        const Json* Tolerances = RequireObject(
            *Contract, "roundtripTolerances", "$.verificationContract");
        const Json* Viewer = RequireObject(
            *Contract, "viewerContract", "$.verificationContract");
        const Json* Sources = RequireArray(
            *Contract, "sourceAnimations", "$.verificationContract");
        const Json* Target = RequireObject(
            *Contract, "targetTpose", "$.verificationContract");
        if (Tolerances == nullptr || Viewer == nullptr || Sources == nullptr ||
            Target == nullptr || Sources->size() != Summary_.ClipCount)
            return false;
        for (const char* Key : {
                 "local_translation_cm", "model_translation_cm",
                 "rotation_degrees", "scale"})
        {
            double Value = 0.0;
            if (!NumberMember(*Tolerances, Key, Value,
                              "$.verificationContract.roundtripTolerances") ||
                Value <= 0.0)
                return false;
        }
        double Ghost = 0.0;
        double Result = 0.0;
        if (!NumberMember(*Viewer, "source_ghost_opacity", Ghost,
                          "$.verificationContract.viewerContract") ||
            !NumberMember(*Viewer, "result_opacity", Result,
                          "$.verificationContract.viewerContract"))
            return false;
        if (std::abs(Ghost - 0.1) > 1.0e-12)
            return Fail(
                "$.verificationContract.viewerContract.source_ghost_opacity",
                "must equal 0.1");
        if (std::abs(Result - 1.0) > 1.0e-12)
            return Fail(
                "$.verificationContract.viewerContract.result_opacity",
                "must equal 1.0");
        for (const char* Key : {
                 "mesh_toggle",
                 "skeleton_toggle",
                 "animation_switch_control",
                 "export_button",
                 "foot_lock_op_toggle",
                 "third_column_fixed_to_foundation",
                 "fourth_column_and_export_follow_toggle",
                 "foundation_and_final_fbx_both_roundtrip_verified",
                 "source_motion_only_no_ground_or_contact_semantics",
                 "four_synchronized_columns",
                 "meshless_motion_shared_source_mesh_fallback",
                 "missing_display_bones_rest_local_passthrough",
                 "limb_ik_execution_status_visible",
                 "limb_ik_full_commit_required_for_generation",
                 "static_tpose_contract_below_four_columns",
                 "root_pelvis_hips_roles_visible",
                 "static_contract_ignores_animation_frame_camera"})
        {
            if (!RequireTrue(
                    *Viewer, Key,
                    "$.verificationContract.viewerContract"))
                return false;
        }
        const std::uint64_t ExpectedLimbIkGoalCount =
            IsUEIKJsonCandidate_ ? LimbIkGoalCount_ : 4;
        const std::uint64_t ExpectedFingerIkGoalCount =
            IsUEIKJsonCandidate_ ? FingerIkGoalCount_ : 10;
        if (!RequireFalse(
                *Viewer, "display_passthrough_used_as_solver_evidence",
                "$.verificationContract.viewerContract") ||
            !ExactString(
                *Viewer, "viewer_data_schema", SourceReviewSchema,
                "$.verificationContract.viewerContract") ||
            !ExactUnsigned(
                *Viewer, "source_target_tpose_diagram_count", 2,
                "$.verificationContract.viewerContract") ||
            !ExactUnsigned(
                *Viewer, "mapped_chain_count", Summary_.MappedChainCount,
                "$.verificationContract.viewerContract") ||
            !ExactUnsigned(
                *Viewer, "limb_ik_goal_count_per_side",
                ExpectedLimbIkGoalCount,
                "$.verificationContract.viewerContract") ||
            !ExactUnsigned(
                *Viewer, "finger_ik_goal_count_per_side",
                ExpectedFingerIkGoalCount,
                "$.verificationContract.viewerContract") ||
            !ExactUnsigned(
                *Viewer, "goal_marker_count_per_side",
                ExpectedLimbIkGoalCount + ExpectedFingerIkGoalCount,
                "$.verificationContract.viewerContract") ||
            !ExactUnsigned(
                *Viewer, "pole_marker_count_per_side",
                ExpectedLimbIkGoalCount + ExpectedFingerIkGoalCount,
                "$.verificationContract.viewerContract"))
            return false;
        std::set<std::string> SourceIds;
        for (std::size_t Index = 0; Index < Sources->size(); ++Index)
        {
            const Json& Source = (*Sources)[Index];
            const std::string Path =
                "$.verificationContract.sourceAnimations[" +
                std::to_string(Index) + "]";
            std::string ClipId;
            std::string FileName;
            std::string Hash;
            bool Unchanged = false;
            if (!StringMember(Source, "clipId", ClipId, Path) ||
                !SourceIds.insert(ClipId).second ||
                !StringMember(Source, "fileName", FileName, Path) ||
                !ShaMember(Source, "sha256", Hash, Path) ||
                !BoolMember(Source, "unchanged", Unchanged, Path) || !Unchanged)
                return Fail(Path, "source identity is invalid");
            const auto Clip = ClipInfos_.find(ClipId);
            if (Clip == ClipInfos_.end() ||
                Clip->second.SourceAnimationHash != Hash)
                return Fail(
                    Path + ".sha256",
                    "does not match the clip source animation identity");
        }
        std::string FileName;
        std::string Hash;
        bool Unchanged = false;
        return StringMember(*Target, "fileName", FileName,
                            "$.verificationContract.targetTpose") &&
            ShaMember(*Target, "sha256", Hash,
                      "$.verificationContract.targetTpose") &&
            BoolMember(*Target, "unchanged", Unchanged,
                       "$.verificationContract.targetTpose") &&
            Unchanged;
    }

    bool ValidateInventoryCoverage()
    {
        for (const auto& [Path, Entry] : Inventory_)
        {
            if (Entry->Role == EntryRole::Blob &&
                ReferencedBlobs_.find(Path) == ReferencedBlobs_.end())
                return Fail("$.snapshot", "indexed blob is unreferenced: " + Path);
            if (Entry->Role == EntryRole::Export &&
                ReferencedExports_.find(Path) == ReferencedExports_.end())
                return Fail("$.verifiedExports",
                            "indexed export is unreferenced: " + Path);
        }
        Summary_.ReferencedBlobCount = ReferencedBlobs_.size();
        return true;
    }

    const Json& Root_;
    ManifestSummary& Summary_;
    std::vector<std::string>& Errors_;
    std::map<std::string, const IntegrityEntry*> Inventory_;
    std::set<std::string> ReferencedBlobs_;
    std::set<std::string> ReferencedExports_;
    std::map<std::string, ClipInfo> ClipInfos_;
    bool IsUEIKJsonCandidate_ = false;
    bool HasOperationStackMetadata_ = false;
    bool OperationStackCandidateEnabled_ = false;
    std::uint64_t LimbIkGoalCount_ = 0;
    std::uint64_t FingerIkGoalCount_ = 0;
};
} // namespace

bool ValidateManifestJson(
    const std::string_view Text,
    const std::vector<IntegrityEntry>& Inventory,
    ManifestSummary& OutSummary,
    std::vector<std::string>& OutErrors)
{
    OutSummary = {};
    Json Root;
    bool DuplicateKey = false;
    std::vector<std::set<std::string>> ObjectKeyStack;
    const Json::parser_callback_t RejectDuplicateKeys =
        [&](const int, const Json::parse_event_t Event, Json& Parsed)
        {
            if (Event == Json::parse_event_t::object_start)
            {
                ObjectKeyStack.emplace_back();
            }
            else if (Event == Json::parse_event_t::key)
            {
                if (ObjectKeyStack.empty() ||
                    !ObjectKeyStack.back()
                         .insert(Parsed.get<std::string>())
                         .second)
                    DuplicateKey = true;
            }
            else if (Event == Json::parse_event_t::object_end &&
                     !ObjectKeyStack.empty())
            {
                ObjectKeyStack.pop_back();
            }
            return true;
        };
    try
    {
        Root = Json::parse(
            Text.begin(), Text.end(), RejectDuplicateKeys,
            true, false);
    }
    catch (const std::exception& Error)
    {
        OutErrors.push_back(
            "manifest JSON parse failed: " + std::string(Error.what()));
        return false;
    }
    if (DuplicateKey)
    {
        OutErrors.push_back("manifest JSON contains a duplicate object key");
        return false;
    }
    ManifestValidator Validator(Root, Inventory, OutSummary, OutErrors);
    const bool Valid = Validator.Run();
    if (!Valid && OutErrors.empty())
        OutErrors.push_back("manifest semantic validation failed");
    return Valid;
}
} // namespace skrtg::viewer::skrv
