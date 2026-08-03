#include "skrtg/viewer/skrv/package.h"
#include "skrtg/viewer/skrv/sha256.h"

#include "nlohmann/json.hpp"
#include "package_inventory.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
using Json = nlohmann::json;
using skrtg::viewer::skrv::EntryRole;
using skrtg::viewer::skrv::PackageSourceItem;
using skrtg::viewer::skrv::PackageWriteRequest;

int Failures = 0;

void Check(const bool Condition, const std::string& Message)
{
    if (Condition) return;
    ++Failures;
    std::cerr << "FAIL: " << Message << '\n';
}

void Write(const std::filesystem::path& Path, const std::string& Bytes)
{
    std::filesystem::create_directories(Path.parent_path());
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
}

bool SupportsCaseDistinctNames(const std::filesystem::path& Root)
{
    const std::filesystem::path Probe = Root / "case_sensitive_probe";
    std::error_code ErrorCode;
    std::filesystem::remove_all(Probe, ErrorCode);
    ErrorCode.clear();
    std::filesystem::create_directories(Probe, ErrorCode);
    if (ErrorCode) return false;

    Write(Probe / "name", "lower");
    Write(Probe / "NAME", "upper");
    std::size_t RegularFileCount = 0;
    for (const std::filesystem::directory_entry& Entry :
         std::filesystem::directory_iterator(Probe, ErrorCode))
    {
        if (ErrorCode) break;
        if (Entry.is_regular_file()) ++RegularFileCount;
    }
    std::filesystem::remove_all(Probe, ErrorCode);
    return RegularFileCount == 2;
}

std::string Read(const std::filesystem::path& Path)
{
    std::ifstream Input(Path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(Input),
        std::istreambuf_iterator<char>());
}

std::string Digest(const std::string& Bytes)
{
    return skrtg::viewer::skrv::Sha256(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()));
}

Json BlobDescriptor(
    const std::string& Bytes,
    const char* ScalarType,
    const std::uint64_t ElementCount)
{
    const std::string Hash = Digest(Bytes);
    return {
        {"path", "blobs/" + Hash + ".bin"},
        {"scalarType", ScalarType},
        {"byteOrder", "little_endian"},
        {"elementCount", ElementCount},
        {"byteCount", Bytes.size()},
        {"sha256", Hash}};
}

Json PoseDescriptor(const std::string& Bytes)
{
    Json Result = BlobDescriptor(Bytes, "float32", 10);
    Result["layout"] =
        "frame_major_bone_major_tx_ty_tz_qx_qy_qz_qw_sx_sy_sz";
    Result["transformStrideFloat32"] = 10;
    return Result;
}

Json MeshPackage(
    const std::string& Empty,
    const std::string& OneUint32)
{
    const Json EmptyFloat = BlobDescriptor(Empty, "float32", 0);
    const Json EmptyUint = BlobDescriptor(Empty, "uint32", 0);
    const Json Offset = BlobDescriptor(OneUint32, "uint32", 1);
    const Json Mesh = {
        {"name", "mesh"},
        {"path", "mesh"},
        {"skinMode", "normalize"},
        {"controlPointCount", 0},
        {"triangleCount", 0},
        {"clusterCount", 0},
        {"skinDeformerCount", 0},
        {"blendShapeDeformerCount", 0},
        {"materialSlotCount", 0},
        {"maximumInfluencesPerControlPoint", 0},
        {"fallback", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
        {"arrays",
         {{"positions", EmptyFloat},
          {"triangleIndices", EmptyUint},
          {"influenceOffsets", Offset},
          {"influenceClusterIndices", EmptyUint},
          {"influenceWeights", EmptyFloat},
          {"clusterBoneIndices", EmptyUint},
          {"clusterBindOffsets3x4", EmptyFloat}}}};
    return {
        {"label", "mesh_package"},
        {"meshCount", 1},
        {"controlPointCount", 0},
        {"triangleCount", 0},
        {"skinDeformerCount", 0},
        {"skinClusterCount", 0},
        {"influenceCount", 0},
        {"blendShapeDeformerCount", 0},
        {"materialSlotCount", 0},
        {"maximumInfluencesPerControlPoint", 0},
        {"maximumBindReconstructionErrorCm", 0.0},
        {"meshes", Json::array({Mesh})}};
}

Json ValidManifest(
    const std::string& Empty,
    const std::string& OneUint32,
    const std::string& Pose,
    const std::string& ExportBytes)
{
    const std::string SourceHash = Digest("source");
    const std::string VerificationHash = Digest("verification");
    const std::string ExportHash = Digest(ExportBytes);
    const Json PoseValue = PoseDescriptor(Pose);
    const Json Package = MeshPackage(Empty, OneUint32);
    const Json Bone = Json::array({-1, "root", "root", false, 0, 0, 0});
    const Json Clip = {
        {"id", "clip"},
        {"label", "clip"},
        {"sourceMeshIndex", 0},
        {"sourceMeshFallbackUsed", false},
        {"limbIkStatus", "committed"},
        {"sourceMotionFootLockEnabled", false},
        {"sourceMotionFootLockSuccess", true},
        {"sourceMotionFootLockDeterministic", true},
        {"sourceMotionFootLockNoGroundOrContactSemanticsUsed", true},
        {"fps", 30.0},
        {"startFrame", 0},
        {"stopFrame", 0},
        {"frameCount", 1},
        {"sourceAnimationSha256", SourceHash},
        {"foundationExportFbx", "foundation.fbx"},
        {"exportFbx", "final.fbx"},
        {"sourceTrs", PoseValue},
        {"fkTrs", PoseValue},
        {"foundationTrs", PoseValue},
        {"finalTrs", PoseValue}};
    const Json Chain = {
        {"label", "root"},
        {"source", {0}},
        {"target", {0}},
        {"ikMode", "fk_only"},
        {"sourceGoalName", ""},
        {"targetGoalName", ""},
        {"sourceGoalBone", -1},
        {"targetGoalBone", -1},
        {"sourcePoleBone", -1},
        {"targetPoleBone", -1}};
    const Json Anchor = {
        {"label", "root"},
        {"sourceBone", 0},
        {"targetBone", 0},
        {"sourcePath", "root"},
        {"targetPath", "root"},
        {"basis", {0, 0, 0, 1}}};
    const auto Export = [&](const char* Lane, const char* FileName)
    {
        return Json{
            {"clip_id", "clip"},
            {"lane", Lane},
            {"path", std::string("exports/") + FileName},
            {"sha256", ExportHash},
            {"byteCount", ExportBytes.size()},
            {"samples_compared", 1},
            {"local_mismatch_count", 0},
            {"model_mismatch_count", 0},
            {"mesh_fingerprint_before", "same"},
            {"mesh_fingerprint_after", "same"}};
    };
    return {
        {"schema", "skrtg.skrv.manifest.v1"},
        {"contractVersion", 1},
        {"sourceReviewSchema", "skrtg.d1_17b_retarget_review_viewer.v3"},
        {"provenance",
         {{"sourceReviewDataSha256", SourceHash},
          {"sourceVerificationSha256", VerificationHash},
          {"adapter", "skrtg.review_html_to_skrv_payload.v1"},
          {"algorithmRecomputed", false},
          {"fbxReserialized", false}}},
        {"storageContract",
         {{"scalarEncoding", "IEEE_754_or_uint32"},
          {"byteOrder", "little_endian"},
          {"translationUnit", "centimeter"},
          {"quaternionOrder", "x_y_z_w"},
          {"transformFloat32Layout", "tx_ty_tz_qx_qy_qz_qw_sx_sy_sz"},
          {"meshClusterBindOffsetLayout", "row_major_3x4_affine"},
          {"packageForm", "auditable_directory"}}},
        {"displayContract",
         {{"columns",
           {"original_source_model_space",
            "fk_plus_anchor_aligned_original_10_percent",
            "foundation_fk_ik_plus_anchor_aligned_original_10_percent",
            "final_result_only"}},
          {"sourceGhostOpacity", 0.1},
          {"resultOpacity", 1.0},
          {"synchronized", {"frame", "camera", "projection", "display_scale"}},
          {"originalIndependentPanelKeepsOriginalSpace", true},
          {"overlaysUseExplicitAnchorAlignedSourceGhost", true},
          {"nonIkBonesReducedOpacity", true},
          {"virtualGroundIsDisplayOnly", true},
          {"goalHistoryIsDisplayOnly", true},
          {"noGroundOrContactSemanticsClaimed", true}}},
        {"counts",
         {{"clipCount", 1},
          {"frameCount", 1},
          {"sourceBoneCount", 1},
          {"targetBoneCount", 1},
          {"mappedChainCount", 1},
          {"goalChainCount", 0}}},
        {"snapshot",
         {{"route", "frozen"},
          {"selected", false},
          {"adopted", false},
          {"stageComplete", false},
          {"route_selected", false},
          {"route_adopted", false},
          {"stage_complete", false},
          {"foundationRoute", "frozen"},
          {"foundationFrozen", true},
          {"sourceMotionFootLockRoute", "disabled"},
          {"sourceMotionFootLockCandidateEnabled", false},
          {"sourceMotionFootLockCandidateSelected", false},
          {"sourceMotionFootLockCandidateAdopted", false},
          {"upstreamLimbIkRouteSelected", true},
          {"upstreamLimbIkRouteAdopted", true},
          {"spinePelvisFollowCandidateEnabled", true},
          {"spinePelvisFollowCandidateSelected", true},
          {"spinePelvisFollowCandidateAdopted", true},
          {"sourceBones", Json::array({Bone})},
          {"targetBones", Json::array({Bone})},
          {"rootPelvis",
           {{"sourceRoot", 0},
            {"sourcePelvis", 0},
            {"targetHips", 0},
            {"rootOwnership", "root"},
            {"pelvisOwnership", "pelvis"},
            {"scaleOwnership", "scale"}}},
          {"retargetChains", Json::array({Chain})},
          {"anchors", Json::array({Anchor})},
          {"sourceMeshes", Json::array({Package})},
          {"targetMesh", Package},
          {"clips", Json::array({Clip})}}},
        {"verifiedExports",
         Json::array(
             {Export("foundation", "foundation.fbx"),
              Export("final", "final.fbx")})},
        {"verificationContract",
         {{"schema", "skrtg.d1_17b_mesh_and_fbx_export_verification.v1"},
          {"roundtripTolerances",
           {{"local_translation_cm", 0.00005},
            {"model_translation_cm", 0.00015},
            {"rotation_degrees", 0.00005},
            {"scale", 0.00000125}}},
          {"viewerContract",
           {{"mesh_toggle", true},
            {"skeleton_toggle", true},
            {"animation_switch_control", true},
            {"export_button", true},
            {"foot_lock_op_toggle", true},
            {"third_column_fixed_to_foundation", true},
            {"fourth_column_and_export_follow_toggle", true},
            {"foundation_and_final_fbx_both_roundtrip_verified", true},
            {"source_motion_only_no_ground_or_contact_semantics", true},
            {"four_synchronized_columns", true},
            {"meshless_motion_shared_source_mesh_fallback", true},
            {"missing_display_bones_rest_local_passthrough", true},
            {"display_passthrough_used_as_solver_evidence", false},
            {"source_ghost_opacity", 0.1},
            {"result_opacity", 1.0},
            {"limb_ik_execution_status_visible", true},
            {"limb_ik_full_commit_required_for_generation", true},
            {"viewer_data_schema", "skrtg.d1_17b_retarget_review_viewer.v3"},
            {"static_tpose_contract_below_four_columns", true},
            {"source_target_tpose_diagram_count", 2},
            {"mapped_chain_count", 1},
            {"limb_ik_goal_count_per_side", 4},
            {"finger_ik_goal_count_per_side", 10},
            {"goal_marker_count_per_side", 14},
            {"pole_marker_count_per_side", 14},
            {"root_pelvis_hips_roles_visible", true},
            {"static_contract_ignores_animation_frame_camera", true}}},
          {"sourceAnimations",
           Json::array(
               {{{"clipId", "clip"},
                 {"fileName", "source.fbx"},
                 {"sha256", SourceHash},
                 {"unchanged", true}}})},
          {"targetTpose",
           {{"fileName", "target.fbx"},
            {"sha256", Digest("target")},
            {"unchanged", true}}}}}};
}

std::vector<PackageSourceItem> ValidItems(
    const std::filesystem::path& Payload,
    const std::string& EmptyHash,
    const std::string& UintHash,
    const std::string& PoseHash)
{
    return {
        {EntryRole::Manifest, "manifest.json", Payload / "manifest.json"},
        {EntryRole::Blob, std::filesystem::path("blobs") / (EmptyHash + ".bin"),
         Payload / "blobs" / (EmptyHash + ".bin")},
        {EntryRole::Blob, std::filesystem::path("blobs") / (UintHash + ".bin"),
         Payload / "blobs" / (UintHash + ".bin")},
        {EntryRole::Blob, std::filesystem::path("blobs") / (PoseHash + ".bin"),
         Payload / "blobs" / (PoseHash + ".bin")},
        {EntryRole::Export, "exports/foundation.fbx",
         Payload / "exports" / "foundation.fbx"},
        {EntryRole::Export, "exports/final.fbx",
         Payload / "exports" / "final.fbx"}};
}
} // namespace

int main()
{
    Check(
        Digest("") ==
            "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855",
        "SHA-256 empty-vector contract");
    Check(
        Digest("abc") ==
            "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
        "SHA-256 abc-vector contract");

    skrtg::viewer::skrv::detail::PackageInventory CollisionProbe;
    Check(CollisionProbe.Register("manifest.json", true),
          "inventory accepts the first portable path key");
    Check(!CollisionProbe.Register("manifest.json", true),
          "inventory rejects a case-folded duplicate path key");
    Check(CollisionProbe.GetRegularFiles().size() == 1,
          "inventory collision does not duplicate the regular-file set");

    const auto Nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    std::uint64_t InvalidCase = 0;
    const std::filesystem::path Root =
        std::filesystem::temp_directory_path() /
        ("skrv_package_tests_" + std::to_string(Nonce));
    const std::filesystem::path Payload = Root / "payload";
    const std::filesystem::path Package = Root / "review.skrv";
    const std::string Empty;
    const std::string OneUint32(4, '\0');
    const std::string Pose(40, '\0');
    const std::string ExportBytes = "fbx";
    const std::string EmptyHash = Digest(Empty);
    const std::string UintHash = Digest(OneUint32);
    const std::string PoseHash = Digest(Pose);
    Json Manifest = ValidManifest(Empty, OneUint32, Pose, ExportBytes);

    Write(Payload / "manifest.json", Manifest.dump(2) + "\n");
    Write(Payload / "blobs" / (EmptyHash + ".bin"), Empty);
    Write(Payload / "blobs" / (UintHash + ".bin"), OneUint32);
    Write(Payload / "blobs" / (PoseHash + ".bin"), Pose);
    Write(Payload / "exports" / "foundation.fbx", ExportBytes);
    Write(Payload / "exports" / "final.fbx", ExportBytes);

    PackageWriteRequest Request;
    Request.OutputDirectory = Package;
    Request.Items = ValidItems(Payload, EmptyHash, UintHash, PoseHash);
    const auto Written = skrtg::viewer::skrv::WriteDirectoryPackage(Request);
    Check(Written.Success, "writer commits a semantically valid package");
    Check(Written.Entries.size() == 6, "writer indexes every payload file");
    Check(!std::filesystem::exists(Package.string() + ".partial"),
          "writer exposes no fixed partial output");

    const auto Inspected = skrtg::viewer::skrv::InspectDirectoryPackage(Package);
    Check(Inspected.Success, "reader accepts a semantically valid package");
    Check(Inspected.Entries.size() == 6, "reader returns the inventory");
    Check(Inspected.Manifest.ClipCount == 1 &&
              Inspected.Manifest.FrameCount == 1 &&
              Inspected.Manifest.ReferencedBlobCount == 3 &&
              Inspected.Manifest.VerifiedExportCount == 2,
          "reader returns the semantic manifest summary");

    const std::filesystem::path SealPayload = Root / "seal_payload";
    const std::filesystem::path SealedPackage =
        Root / "sealed_review.skrv";
    Write(SealPayload / "manifest.json", Manifest.dump(2) + "\n");
    Write(SealPayload / "blobs" / (EmptyHash + ".bin"), Empty);
    Write(SealPayload / "blobs" / (UintHash + ".bin"), OneUint32);
    Write(SealPayload / "blobs" / (PoseHash + ".bin"), Pose);
    Write(SealPayload / "exports" / "foundation.fbx", ExportBytes);
    Write(SealPayload / "exports" / "final.fbx", ExportBytes);
    const auto Sealed = skrtg::viewer::skrv::SealDirectoryPackage(
        SealPayload, SealedPackage);
    Check(Sealed.Success && Sealed.Entries.size() == 6,
          "sealed writer commits the adapter payload without a copy stage");
    Check(!std::filesystem::exists(SealPayload) &&
              skrtg::viewer::skrv::InspectDirectoryPackage(
                  SealedPackage).Success,
          "sealed writer consumes staging input and commits a valid SKRV");

    const std::filesystem::path CorruptSealPayload =
        Root / "corrupt_seal_payload";
    const std::filesystem::path CorruptSealedPackage =
        Root / "corrupt_sealed_review.skrv";
    Write(
        CorruptSealPayload / "manifest.json",
        Manifest.dump(2) + "\n");
    Write(
        CorruptSealPayload / "blobs" / (EmptyHash + ".bin"),
        "not-empty");
    Write(
        CorruptSealPayload / "blobs" / (UintHash + ".bin"),
        OneUint32);
    Write(
        CorruptSealPayload / "blobs" / (PoseHash + ".bin"), Pose);
    Write(
        CorruptSealPayload / "exports" / "foundation.fbx",
        ExportBytes);
    Write(
        CorruptSealPayload / "exports" / "final.fbx", ExportBytes);
    Check(
        !skrtg::viewer::skrv::SealDirectoryPackage(
             CorruptSealPayload, CorruptSealedPackage).Success &&
            !std::filesystem::exists(CorruptSealedPackage),
        "sealed writer fails closed when a digest-named blob is corrupt");

    Json UEIKJsonCandidate = Manifest;
    UEIKJsonCandidate["snapshot"]["route"] =
        "ue_ik_json_canonical_bridge_v1";
    UEIKJsonCandidate["snapshot"]["foundationRoute"] =
        "ue_ik_json_fk_pelvis_limb_ik_candidate_v1";
    UEIKJsonCandidate["snapshot"]["foundationFrozen"] = false;
    for (const char* Key : {
             "selected", "adopted", "stageComplete",
             "route_selected", "route_adopted", "stage_complete",
             "sourceMotionFootLockCandidateEnabled",
             "sourceMotionFootLockCandidateSelected",
             "sourceMotionFootLockCandidateAdopted",
             "upstreamLimbIkRouteSelected",
             "upstreamLimbIkRouteAdopted",
             "spinePelvisFollowCandidateEnabled",
             "spinePelvisFollowCandidateSelected",
             "spinePelvisFollowCandidateAdopted"})
    {
        UEIKJsonCandidate["snapshot"][Key] = false;
    }
    Json& UEIKViewer =
        UEIKJsonCandidate["verificationContract"]["viewerContract"];
    UEIKViewer["limb_ik_goal_count_per_side"] = 0;
    UEIKViewer["finger_ik_goal_count_per_side"] = 0;
    UEIKViewer["goal_marker_count_per_side"] = 0;
    UEIKViewer["pole_marker_count_per_side"] = 0;
    Write(
        Payload / "manifest.json",
        UEIKJsonCandidate.dump(2) + "\n");
    PackageWriteRequest UEIKCandidateRequest = Request;
    UEIKCandidateRequest.OutputDirectory =
        Root / "ue_ik_json_candidate.skrv";
    const auto UEIKCandidateWritten =
        skrtg::viewer::skrv::WriteDirectoryPackage(
            UEIKCandidateRequest);
    Check(
        UEIKCandidateWritten.Success,
        "reader accepts the explicit non-frozen UE IK JSON candidate");
    Check(
        skrtg::viewer::skrv::InspectDirectoryPackage(
            UEIKCandidateRequest.OutputDirectory).Success,
        "reader reopens the UE IK JSON candidate package");

    Json UnknownNonFrozen = UEIKJsonCandidate;
    UnknownNonFrozen["snapshot"]["route"] = "unknown_non_frozen_route";
    Write(
        Payload / "manifest.json",
        UnknownNonFrozen.dump(2) + "\n");
    PackageWriteRequest UnknownNonFrozenRequest = Request;
    UnknownNonFrozenRequest.OutputDirectory =
        Root / "unknown_non_frozen.skrv";
    Check(
        !skrtg::viewer::skrv::WriteDirectoryPackage(
            UnknownNonFrozenRequest).Success,
        "reader rejects every other non-frozen route");
    Write(Payload / "manifest.json", Manifest.dump(2) + "\n");

    skrtg::viewer::skrv::PackageInspectOptions TinyInventoryBudget;
    TinyInventoryBudget.InventoryEntryLimit = 2;
    Check(
        !skrtg::viewer::skrv::InspectDirectoryPackage(
             Package, TinyInventoryBudget).Success,
        "reader enforces the package inventory-entry limit directly");

    const std::filesystem::path IntegrityPath = Package / "integrity.tsv";
    const std::string ValidIntegrity = Read(IntegrityPath);
    std::string RoleMismatch = ValidIntegrity;
    const std::size_t BlobRole = RoleMismatch.find("blob\t");
    Check(BlobRole != std::string::npos,
          "valid fixture contains a blob integrity record");
    if (BlobRole != std::string::npos)
        RoleMismatch.replace(BlobRole, 4, "auxiliary");
    Write(IntegrityPath, RoleMismatch);
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects an auxiliary role inside blobs/");

    const std::string Header =
        "SKRV_INTEGRITY_V1\nrole\tpath\tbyte_count\tsha256\n";
    Write(
        IntegrityPath,
        Header + "blob\tnotes.bin\t0\t" + Digest("") + "\n");
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects a blob role outside blobs/");

    Write(
        IntegrityPath,
        Header + "auxiliary\t" +
            std::string(
                skrtg::viewer::skrv::MaximumPortablePathBytes + 1, 'a') +
            "\t0\t" + Digest("") + "\n");
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects an integrity path over the portable length limit");

    Write(
        IntegrityPath,
        Header + "auxiliary\t" +
            std::string(
                skrtg::viewer::skrv::MaximumPortablePathComponentBytes + 1,
                'a') +
            "\t0\t" + Digest("") + "\n");
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects an oversized portable path component");

    Write(
        IntegrityPath,
        Header +
            std::string(
                skrtg::viewer::skrv::MaximumIntegrityLineBytes + 1, 'x') +
            "\n");
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects an oversized integrity line");

    std::string TooManyRecords = Header;
    for (std::size_t Index = 0;
         Index <= skrtg::viewer::skrv::MaximumIndexedEntries;
         ++Index)
        TooManyRecords += "x\n";
    Write(IntegrityPath, TooManyRecords);
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects too many integrity records before resolving paths");

    Write(
        IntegrityPath,
        std::string(
            static_cast<std::size_t>(
                skrtg::viewer::skrv::MaximumIntegrityIndexBytes + 1),
            'x'));
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects an oversized integrity index before hashing it");
    Write(IntegrityPath, ValidIntegrity);

    std::string CorruptPose = Pose;
    CorruptPose[0] = '\1';
    Write(Package / "blobs" / (PoseHash + ".bin"), CorruptPose);
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects a same-size hash mismatch");
    Write(Package / "blobs" / (PoseHash + ".bin"), Pose);

    Write(Package / "unindexed.bin", "x");
    Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
          "reader rejects unindexed files");
    std::filesystem::remove(Package / "unindexed.bin");

    if (SupportsCaseDistinctNames(Root))
    {
        Write(Package / "MANIFEST.JSON", "case collision");
        Check(!skrtg::viewer::skrv::InspectDirectoryPackage(Package).Success,
              "reader rejects case-insensitive filesystem path collisions");
        std::filesystem::remove(Package / "MANIFEST.JSON");
    }

    Json InvalidManifest = Manifest;
    InvalidManifest.erase("counts");
    Write(Payload / "manifest.json", InvalidManifest.dump(2) + "\n");
    PackageWriteRequest InvalidSemantic = Request;
    InvalidSemantic.OutputDirectory = Root / "invalid_semantic.skrv";
    Check(!skrtg::viewer::skrv::WriteDirectoryPackage(InvalidSemantic).Success,
          "writer's staged reader rejects a semantically invalid manifest");
    Check(!std::filesystem::exists(InvalidSemantic.OutputDirectory),
          "semantic failure is atomic and leaves no output package");

    std::string DuplicateKeyManifest = Manifest.dump(2);
    DuplicateKeyManifest.insert(
        1, "\n  \"schema\": \"skrtg.skrv.manifest.v1\",");
    Write(Payload / "manifest.json", DuplicateKeyManifest + "\n");
    PackageWriteRequest DuplicateKey = Request;
    DuplicateKey.OutputDirectory = Root / "duplicate_key.skrv";
    Check(!skrtg::viewer::skrv::WriteDirectoryPackage(DuplicateKey).Success,
          "reader rejects duplicate JSON object keys");

    const std::vector<std::pair<std::int64_t, std::int64_t>> ExtremeFrames = {
        {std::numeric_limits<std::int64_t>::min(),
         std::numeric_limits<std::int64_t>::max()},
        {std::numeric_limits<std::int64_t>::min(), 0},
        {0, std::numeric_limits<std::int64_t>::max()}};
    for (std::size_t Index = 0; Index < ExtremeFrames.size(); ++Index)
    {
        InvalidManifest = Manifest;
        InvalidManifest["snapshot"]["clips"][0]["startFrame"] =
            ExtremeFrames[Index].first;
        InvalidManifest["snapshot"]["clips"][0]["stopFrame"] =
            ExtremeFrames[Index].second;
        Write(Payload / "manifest.json", InvalidManifest.dump(2) + "\n");
        PackageWriteRequest ExtremeTiming = Request;
        ExtremeTiming.OutputDirectory =
            Root / ("extreme_timing_" + std::to_string(Index) + ".skrv");
        Check(!skrtg::viewer::skrv::WriteDirectoryPackage(ExtremeTiming).Success,
              "reader rejects an unrepresentable extreme frame span");
    }

    InvalidManifest = Manifest;
    InvalidManifest["snapshot"]["clips"][0]["sourceTrs"]["path"] =
        "blobs/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.bin";
    Write(Payload / "manifest.json", InvalidManifest.dump(2) + "\n");
    PackageWriteRequest InvalidAddress = Request;
    InvalidAddress.OutputDirectory = Root / "invalid_address.skrv";
    Check(!skrtg::viewer::skrv::WriteDirectoryPackage(InvalidAddress).Success,
          "reader rejects a blob path that is not named by its digest");

    InvalidManifest = Manifest;
    InvalidManifest["snapshot"]["retargetChains"][0]["sourceGoalBone"] = -2;
    Write(Payload / "manifest.json", InvalidManifest.dump(2) + "\n");
    PackageWriteRequest InvalidFkSentinel = Request;
    InvalidFkSentinel.OutputDirectory = Root / "invalid_fk_sentinel.skrv";
    Check(!skrtg::viewer::skrv::WriteDirectoryPackage(InvalidFkSentinel).Success,
          "reader rejects FK-only Goal or pole indices below -1");
    Write(Payload / "manifest.json", Manifest.dump(2) + "\n");

    const auto ExpectWriterFailure = [&](const PackageSourceItem& Item,
                                         const char* Message)
    {
        PackageWriteRequest Invalid;
        Invalid.OutputDirectory =
            Root / ("invalid_case_" + std::to_string(InvalidCase++) + ".skrv");
        Invalid.Items = {
            {EntryRole::Manifest, "manifest.json", Payload / "manifest.json"},
            Item};
        Check(!skrtg::viewer::skrv::WriteDirectoryPackage(Invalid).Success,
              Message);
        Check(!std::filesystem::exists(Invalid.OutputDirectory),
              std::string(Message) + " leaves no output");
    };
    ExpectWriterFailure(
        {EntryRole::Manifest, "../manifest.json", Payload / "manifest.json"},
        "writer rejects path traversal");
    ExpectWriterFailure(
        {EntryRole::Blob, "notes.bin", Payload / "exports" / "final.fbx"},
        "writer rejects a blob role outside blobs/");
    ExpectWriterFailure(
        {EntryRole::Auxiliary,
         std::filesystem::path("blobs") / (EmptyHash + ".bin"),
         Payload / "blobs" / (EmptyHash + ".bin")},
        "writer rejects an auxiliary role inside blobs/");
    ExpectWriterFailure(
        {EntryRole::Auxiliary, "con.txt", Payload / "exports" / "final.fbx"},
        "writer rejects a reserved Windows device name");
    ExpectWriterFailure(
        {EntryRole::Auxiliary,
         std::string(
             skrtg::viewer::skrv::MaximumPortablePathComponentBytes + 1,
             'a'),
         Payload / "exports" / "final.fbx"},
        "writer rejects an oversized portable path component");
    ExpectWriterFailure(
        {EntryRole::Auxiliary,
         std::filesystem::path(std::u8string(u8"notes/\u00E9.txt")),
         Payload / "exports" / "final.fbx"},
        "writer rejects paths outside the portable ASCII subset");

    PackageWriteRequest TooManyItems;
    TooManyItems.OutputDirectory = Root / "too_many_items.skrv";
    TooManyItems.Items.resize(
        skrtg::viewer::skrv::MaximumIndexedEntries + 1);
    Check(!skrtg::viewer::skrv::WriteDirectoryPackage(TooManyItems).Success,
          "writer rejects an oversized indexed-entry request");
    Check(!std::filesystem::exists(TooManyItems.OutputDirectory),
          "oversized writer request leaves no output package");

    std::error_code Ignored;
    std::filesystem::remove_all(Root, Ignored);
    if (Failures != 0)
    {
        std::cerr << Failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "SKRV package tests passed\n";
    return 0;
}
