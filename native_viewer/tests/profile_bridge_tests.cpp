#include "skrtg/viewer/batch_retarget.h"
#include "skrtg/viewer/profile/character_profile.h"
#include "skrtg/viewer/retarget_asset_catalog.h"
#include "skrtg/viewer/retarget_bridge.h"
#include "skrtg/viewer/skrv/sha256.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
using Json = nlohmann::json;
namespace Profile = skrtg::viewer::profile;
using namespace skrtg::viewer;

int Failures = 0;

void Check(const bool Condition, const std::string& Message)
{
    if (Condition) return;
    ++Failures;
    std::cerr << "FAIL: " << Message << '\n';
}

void Write(
    const std::filesystem::path& Path,
    const std::string& Bytes)
{
    std::filesystem::create_directories(Path.parent_path());
    std::ofstream Output(
        Path, std::ios::binary | std::ios::trunc);
    Output.write(
        Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
}

std::string Read(const std::filesystem::path& Path)
{
    std::ifstream Input(Path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(Input),
        std::istreambuf_iterator<char>());
}

std::string Hash(const std::filesystem::path& Path)
{
    std::string Result;
    std::string Error;
    Check(skrtg::viewer::skrv::Sha256File(
              Path, Result, Error),
          "test fixture should be hashable: " + Error);
    return Result;
}

struct TemporaryRoot
{
    std::filesystem::path Path;

    TemporaryRoot()
    {
        const auto Stamp =
            std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count();
        Path = std::filesystem::temp_directory_path() /
            ("skrtg_profile_bridge_" + std::to_string(Stamp));
        std::filesystem::create_directories(Path);
    }

    ~TemporaryRoot()
    {
        std::error_code Error;
        std::filesystem::remove_all(Path, Error);
    }
};

Json CoordinateContract()
{
    return {
        {"handedness", "left"},
        {"forwardAxis", "+X"},
        {"rightAxis", "+Y"},
        {"upAxis", "+Z"},
        {"distanceUnit", "centimeter"},
        {"quaternionComponentOrder", "x,y,z,w"}};
}

Profile::ProfilePackResult PackProfile(
    const std::filesystem::path& Root,
    const std::string& Id,
    const std::string& RigName,
    const char FingerprintCharacter)
{
    const std::filesystem::path Inputs = Root / Id;
    const std::filesystem::path Rest = Inputs / "rest.fbx";
    const std::filesystem::path Rig = Inputs / "rig.json";
    const std::filesystem::path Alignment =
        Inputs / "alignment.json";
    Write(Rest, "profile-rest-" + Id);
    Write(
        Rig,
        Json{
            {"schema", "skrtg.ue_ik_asset_export.v2"},
            {"schemaVersion", 2},
            {"kind", "ikRigDefinition"},
            {"valid", true},
            {"unrealEngineVersion", "5.8.0"},
            {"coordinateContract", CoordinateContract()},
            {"asset", {{"assetName", RigName}}},
            {"retargetRootBone", "pelvis"},
            {"retargetPelvisBone", "pelvis"},
            {"referenceSkeleton",
             {
                 {"fingerprintSha256",
                  std::string(64, FingerprintCharacter)},
                 {"bones",
                  Json::array(
                      {{{"name", "root"},
                        {"parentIndex", -1}},
                       {{"name", "pelvis"},
                        {"parentIndex", 0}}})},
             }}}
            .dump(2) +
            "\n");
    Write(
        Alignment,
        Json{
            {"schema", "skrtg.ue_ik_asset_export.v2"},
            {"schemaVersion", 2},
            {"kind", "ikRetargeter"},
            {"valid", true},
            {"unrealEngineVersion", "5.8.0"},
            {"coordinateContract", CoordinateContract()},
            {"source",
             {{"ikRig", {{"assetName", "IK_Canonical"}}}}},
            {"target",
             {{"ikRig", {{"assetName", RigName}}}}}}
            .dump(2) +
            "\n");

    Profile::ProfilePackRequest Request;
    Request.OutputPackage =
        Root / (Id + ".skrtgprofile");
    Request.ProfileId = Id;
    Request.ProfileVersion = "1.0.0";
    Request.DisplayName = Id;
    Request.RestFbx = Rest;
    Request.IkRigJson = Rig;
    Request.AlignmentRetargeterJson = Alignment;
    return Profile::WriteCharacterProfilePackage(Request);
}

Json FileBinding(
    const std::filesystem::path& Relative,
    const std::filesystem::path& Absolute)
{
    return {
        {"path", PathToUtf8(Relative)},
        {"sha256", Hash(Absolute)}};
}

void TestProfileBoundBridge()
{
    TemporaryRoot Root;
    const Profile::ProfilePackResult SourcePackage =
        PackProfile(
            Root.Path / "packages", "test-source.v2",
            "IK_Source", 'A');
    const Profile::ProfilePackResult TargetPackage =
        PackProfile(
            Root.Path / "packages", "test-target.v2",
            "IK_Target", 'B');
    Check(SourcePackage.Success && TargetPackage.Success,
          "source and target profile fixtures should pack");

    const std::filesystem::path Store = Root.Path / "store";
    const Profile::ProfileInstallResult SourceInstall =
        Profile::InstallCharacterProfilePackage(
            SourcePackage.PackagePath, Store);
    const Profile::ProfileInstallResult TargetInstall =
        Profile::InstallCharacterProfilePackage(
            TargetPackage.PackagePath, Store);
    Check(SourceInstall.Success && TargetInstall.Success,
          "source and target profile fixtures should install");

    const std::filesystem::path CatalogRoot =
        Root.Path / "catalog";
    const std::filesystem::path Animation =
        CatalogRoot / "assets" / "source_animation.fbx";
    const std::filesystem::path Golden =
        CatalogRoot / "assets" / "source_animation.json";
    const std::filesystem::path AnimationTwo =
        CatalogRoot / "assets" / "source_animation_two.fbx";
    const std::filesystem::path GoldenTwo =
        CatalogRoot / "assets" / "source_animation_two.json";
    Write(Animation, "source-animation");
    Write(Golden, "{}");
    Write(AnimationTwo, "source-animation-two");
    Write(GoldenTwo, "{\"clip\":2}");

    const std::filesystem::path CatalogFile =
        CatalogRoot / "retarget_asset_catalog.json";
    Write(
        CatalogFile,
        Json{
            {"schema",
             "skrtg.native_viewer.retarget_asset_catalog.v1"},
            {"catalogId", "test_catalog"},
            {"assetRoot", "."},
            {"externalSkeletonIds",
             Json::array(
                 {"test-source.v2", "test-target.v2"})},
            {"skeletons", Json::array()},
            {"animations",
             Json::array({
                 {{"id", "source_clip"},
                   {"label", "Source Clip"},
                   {"sourceSkeletonId", "test-source.v2"},
                   {"sourceSkeletonSignatureSha256",
                    std::string(64, 'A')},
                   {"sourceFbxImportMode",
                    RetargetBridgeSourceFbxImportModeName(
                        RetargetBridgeSourceFbxImportMode::
                            UE58ExactGoldenV1)},
                   {"restFbxImportMode",
                    RetargetBridgeRestFbxImportModeName(
                        RetargetBridgeRestFbxImportMode::
                            UE58ExportedYReflectionV1)},
                   {"animationStack", ""},
                   {"enabled", true},
                   {"fbx",
                    FileBinding(
                        "assets/source_animation.fbx",
                        Animation)},
                   {"goldenJson",
                    FileBinding(
                        "assets/source_animation.json",
                        Golden)}},
                 {{"id", "source_clip_two"},
                   {"label", "Source Clip Two"},
                   {"sourceSkeletonId", "test-source.v2"},
                   {"sourceSkeletonSignatureSha256",
                    std::string(64, 'A')},
                   {"sourceFbxImportMode",
                    RetargetBridgeSourceFbxImportModeName(
                        RetargetBridgeSourceFbxImportMode::
                            UE58ExactGoldenV1)},
                   {"restFbxImportMode",
                    RetargetBridgeRestFbxImportModeName(
                        RetargetBridgeRestFbxImportMode::
                            UE58ExportedYReflectionV1)},
                   {"animationStack", "Take 001"},
                   {"enabled", true},
                   {"fbx",
                    FileBinding(
                        "assets/source_animation_two.fbx",
                        AnimationTwo)},
                   {"goldenJson",
                    FileBinding(
                        "assets/source_animation_two.json",
                        GoldenTwo)}}})}}
            .dump(2) +
            "\n");
    const RetargetAssetCatalogLoadResult DeclaredCatalog =
        LoadRetargetAssetCatalog(CatalogFile);
    Check(DeclaredCatalog.Success,
          "declared profile-backed animation catalog should validate "
          "without loose skeleton assets");
    const RetargetAssetCatalogLoadResult Catalog =
        LoadRetargetAssetCatalog(
            CatalogFile, true,
            {"test-source.v2", "test-target.v2"});
    Check(Catalog.Success,
          "profile-backed animation catalog should validate without "
          "loose skeleton assets");

    const auto InstalledPath =
        [](const Profile::ProfileInstallResult& Install,
           const Profile::ProfileResource& Resource)
        {
            return Profile::InstalledProfileResourcePath(
                Install.Installed, Resource);
        };
    RetargetBridgeRequest Request;
    Request.RouteKind = RetargetBridgeRouteKind::UEIKJsonV1;
    Request.SourceAnimationFbx = Animation;
    Request.SourceAnimationGoldenJson = Golden;
    Request.SourceRestFbx = InstalledPath(
        SourceInstall, SourceInstall.Installed.Profile.RestFbx);
    Request.TargetSkeletonFbx = InstalledPath(
        TargetInstall, TargetInstall.Installed.Profile.RestFbx);
    Request.SourceRigJson = InstalledPath(
        SourceInstall, SourceInstall.Installed.Profile.IkRigJson);
    Request.TargetRigJson = InstalledPath(
        TargetInstall, TargetInstall.Installed.Profile.IkRigJson);
    Request.SourceAlignmentRetargeterJson = InstalledPath(
        SourceInstall,
        SourceInstall.Installed.Profile.AlignmentRetargeterJson);
    Request.TargetAlignmentRetargeterJson = InstalledPath(
        TargetInstall,
        TargetInstall.Installed.Profile.AlignmentRetargeterJson);
    Request.OutputDirectory = Root.Path / "output";
    Request.ClipId = "source_clip";
    Request.ClipLabel = "Source Clip";
    Request.SourceFbxImportMode =
        RetargetBridgeSourceFbxImportMode::UE58ExactGoldenV1;
    Request.RestFbxImportMode =
        RetargetBridgeRestFbxImportMode::
            UE58ExportedYReflectionV1;
    Request.EnableSpinePelvisFollow = false;
    Request.EnableSourceMotionFootLock = false;

    const std::filesystem::path Tools = Root.Path / "tools";
    Request.Tools.BridgeExecutable = Tools / "bridge.exe";
    Request.Tools.UEIKRetargeterExecutable =
        Tools / "worker.exe";
    Request.Tools.NodeExecutable = Tools / "node.exe";
    Request.Tools.AdapterScript = Tools / "adapter.js";
    Request.Tools.SkrvPackExecutable = Tools / "pack.exe";
    for (const std::filesystem::path& Tool : {
             Request.Tools.BridgeExecutable,
             Request.Tools.UEIKRetargeterExecutable,
             Request.Tools.NodeExecutable,
             Request.Tools.AdapterScript,
             Request.Tools.SkrvPackExecutable})
    {
        Write(Tool, "test-tool");
    }

    RetargetBridgeAssetBinding& Binding =
        Request.AssetBinding;
    Binding.Required = true;
    Binding.CatalogFile = CatalogFile;
    Binding.CatalogSha256 =
        Catalog.Catalog.CatalogSha256;
    Binding.CatalogId = Catalog.Catalog.CatalogId;
    Binding.SourceSkeletonId = "test-source.v2";
    Binding.TargetSkeletonId = "test-target.v2";
    Binding.SourceAnimationId = "source_clip";
    Binding.SourceAnimationSkeletonId = "test-source.v2";
    Binding.SourceProfilePackage =
        SourceInstall.Installed.PackagePath;
    Binding.SourceProfilePackageSha256 =
        SourceInstall.Installed.PackageSha256;
    Binding.SourceProfileVersion =
        SourceInstall.Installed.Profile.ProfileVersion;
    Binding.TargetProfilePackage =
        TargetInstall.Installed.PackagePath;
    Binding.TargetProfilePackageSha256 =
        TargetInstall.Installed.PackageSha256;
    Binding.TargetProfileVersion =
        TargetInstall.Installed.Profile.ProfileVersion;
    Binding.SourceAnimationSha256 = Hash(Animation);
    Binding.SourceAnimationGoldenJsonSha256 = Hash(Golden);
    Binding.SourceRestSha256 =
        SourceInstall.Installed.Profile.RestFbx.Sha256;
    Binding.TargetRestSha256 =
        TargetInstall.Installed.Profile.RestFbx.Sha256;
    Binding.SourceRigJsonSha256 =
        SourceInstall.Installed.Profile.IkRigJson.Sha256;
    Binding.TargetRigJsonSha256 =
        TargetInstall.Installed.Profile.IkRigJson.Sha256;
    Binding.SourceAlignmentRetargeterJsonSha256 =
        SourceInstall.Installed.Profile
            .AlignmentRetargeterJson.Sha256;
    Binding.TargetAlignmentRetargeterJsonSha256 =
        TargetInstall.Installed.Profile
            .AlignmentRetargeterJson.Sha256;

    const RetargetBridgePreflight Preflight =
        PreflightRetargetBridge(Request);
    if (!Preflight.Success)
    {
        for (const std::string& Error : Preflight.Errors)
            std::cerr << "preflight: " << Error << '\n';
    }
    Check(Preflight.Success,
          "profile-bound Bridge request should pass preflight");
    Check(Preflight.DurationSeconds > 0.0,
          "Bridge preflight should report measured elapsed time");
    Check(Preflight.SourceProfilePackageSha256 ==
              Binding.SourceProfilePackageSha256,
          "source profile package hash should be audited");
    Check(Preflight.TargetProfilePackageSha256 ==
              Binding.TargetProfilePackageSha256,
          "target profile package hash should be audited");

    const std::filesystem::path RequestFile =
        Root.Path / "request.json";
    std::string Error;
    Check(WriteRetargetBridgeRequest(
              Request, RequestFile, Error),
          "profile-bound Bridge request should serialize");
    RetargetBridgeRequest RoundTrip;
    Check(ReadRetargetBridgeRequest(
              RequestFile, RoundTrip, Error),
          "profile-bound Bridge request should deserialize");
    Check(RoundTrip.AssetBinding.SourceProfilePackage ==
              Binding.SourceProfilePackage,
          "source profile path should round-trip");
    Check(RoundTrip.AssetBinding.TargetProfileVersion ==
              Binding.TargetProfileVersion,
          "target profile version should round-trip");
    Check(PreflightRetargetBridge(RoundTrip).Success,
          "round-tripped profile-bound request should pass preflight");

    BatchRetargetRequest Batch;
    Batch.SourceCharacter.RestFbx = Request.SourceRestFbx;
    Batch.SourceCharacter.DefinitionKind = "ue_ik_json_v1";
    Batch.SourceCharacter.DefinitionFile = Request.SourceRigJson;
    Batch.SourceCharacter.AlignmentRetargeterFile =
        Request.SourceAlignmentRetargeterJson;
    Batch.TargetCharacter.RestFbx = Request.TargetSkeletonFbx;
    Batch.TargetCharacter.DefinitionKind = "ue_ik_json_v1";
    Batch.TargetCharacter.DefinitionFile = Request.TargetRigJson;
    Batch.TargetCharacter.AlignmentRetargeterFile =
        Request.TargetAlignmentRetargeterJson;
    Batch.OutputDirectory = Root.Path / "batch_output";
    Batch.Tools = Request.Tools;
    Batch.Recursive = false;
    Batch.EnableSpinePelvisFollow = false;
    Batch.EnableSourceMotionFootLock = false;
    Batch.AssetBinding = Binding;
    const auto AddBatchAnimation =
        [&](const std::string& Id,
            const std::string& Label,
            const std::filesystem::path& Fbx,
            const std::filesystem::path& GoldenJson,
            const std::string& Stack)
        {
            BatchCatalogAnimationInput AnimationInput;
            AnimationInput.AnimationId = Id;
            AnimationInput.Label = Label;
            AnimationInput.SourceSkeletonId = "test-source.v2";
            AnimationInput.SourceAnimationFbx = Fbx;
            AnimationInput.SourceAnimationSha256 = Hash(Fbx);
            AnimationInput.SourceAnimationGoldenJson = GoldenJson;
            AnimationInput.SourceAnimationGoldenJsonSha256 =
                Hash(GoldenJson);
            AnimationInput.AnimationStack = Stack;
            Batch.CatalogAnimations.push_back(
                std::move(AnimationInput));
        };
    AddBatchAnimation(
        "source_clip", "Source Clip", Animation, Golden, "");
    AddBatchAnimation(
        "source_clip_two", "Source Clip Two",
        AnimationTwo, GoldenTwo, "Take 001");

    const BatchRetargetPlan BatchPlan =
        BuildBatchRetargetPlan(Batch);
    if (!BatchPlan.Success)
    {
        for (const std::string& BatchError : BatchPlan.Errors)
            std::cerr << "batch preflight: " << BatchError << '\n';
    }
    Check(BatchPlan.Success,
          "profile-backed batch v3 should preflight every selected "
          "catalog animation");
    Check(BatchPlan.MaximumConcurrentJobs == 1 &&
              BatchPlan.Jobs.size() == 2 &&
              BatchPlan.Preflights.size() == 2,
          "profile-backed batch must preserve fixed serial execution");
    Check(BatchPlan.Preflights[1].CacheHits > 0,
          "complete-selection preflight should reuse shared profile, "
          "catalog, rest, and rig evidence across animations");
    RetargetBridgeRequest MismatchedExecution = Request;
    MismatchedExecution.OutputDirectory =
        Root.Path / "mismatched_preflight_output";
    const RetargetBridgeRunResult MismatchedResult =
        RunRetargetBridgePreflighted(
            MismatchedExecution, BatchPlan.Preflights[1]);
    Check(!MismatchedResult.Success &&
              MismatchedResult.RetargeterExitCode == -1 &&
              MismatchedResult.Timings.TotalSeconds > 0.0 &&
              std::any_of(
                  MismatchedResult.Errors.begin(),
                  MismatchedResult.Errors.end(),
                  [](const std::string& ErrorText)
                  {
                      return ErrorText.find(
                          "preflight does not match") !=
                          std::string::npos;
                  }),
          "preflight reuse must fail closed when supplied to a different "
          "request or output directory");
    Check(BatchPlan.Jobs.size() == 2 &&
              BatchPlan.Jobs[1].SourceAnimationId ==
                  "source_clip_two" &&
              BatchPlan.Jobs[1].SourceAnimationGoldenJson ==
                  GoldenTwo &&
              BatchPlan.Jobs[1].AnimationStack == "Take 001",
          "each batch job must retain its own catalog ID, Golden JSON, "
          "and animation Stack");

    const std::filesystem::path BatchRequestFile =
        Root.Path / "batch_v3.json";
    Check(WriteBatchRetargetRequest(
              Batch, BatchRequestFile, Error),
          "profile-backed batch v3 should serialize");
    const Json BatchJson = Json::parse(Read(BatchRequestFile));
    Check(BatchJson.at("schema") ==
              "skrtg.native_viewer.batch_retarget_request.v3" &&
              BatchJson.at("animations").size() == 2 &&
              BatchJson.at("animationDirectory") == "" &&
              !BatchJson.at("recursive").get<bool>(),
          "profile-backed batch v3 JSON must contain explicit catalog "
          "animations and no loose folder scan");
    BatchRetargetRequest BatchRoundTrip;
    Check(ReadBatchRetargetRequest(
              BatchRequestFile, BatchRoundTrip, Error),
          "profile-backed batch v3 should deserialize");
    Check(BatchRoundTrip.AssetBinding.SourceProfilePackageSha256 ==
              Binding.SourceProfilePackageSha256 &&
              BatchRoundTrip.CatalogAnimations.size() == 2 &&
              BuildBatchRetargetPlan(BatchRoundTrip).Success,
          "round-tripped batch v3 must preserve profile bindings and "
          "pass full preflight");
    Json RecursiveBatchJson = BatchJson;
    RecursiveBatchJson["recursive"] = true;
    const std::filesystem::path RecursiveBatchFile =
        Root.Path / "batch_v3_recursive.json";
    Write(
        RecursiveBatchFile,
        RecursiveBatchJson.dump(2) + "\n");
    BatchRetargetRequest RejectedRecursive;
    Check(!ReadBatchRetargetRequest(
              RecursiveBatchFile, RejectedRecursive, Error),
          "profile-backed batch v3 reader must reject folder-scan "
          "semantics");

    BatchRetargetStatus BatchStatus;
    BatchStatus.MaximumConcurrentJobs = 1;
    BatchStatus.TotalJobs = BatchPlan.Jobs.size();
    BatchStatus.SourceCharacter = Batch.SourceCharacter;
    BatchStatus.TargetCharacter = Batch.TargetCharacter;
    BatchStatus.OutputDirectory = Batch.OutputDirectory;
    BatchStatus.Recursive = false;
    BatchStatus.EnableSpinePelvisFollow = false;
    BatchStatus.EnableSourceMotionFootLock = false;
    BatchStatus.AssetBinding = Batch.AssetBinding;
    BatchStatus.Jobs = BatchPlan.Jobs;
    const std::filesystem::path BatchStatusFile =
        Root.Path / "batch_status_v2.json";
    Check(WriteBatchRetargetStatus(
              BatchStatus, BatchStatusFile, Error),
          "profile-backed batch status v2 should serialize");
    const Json WrittenBatchStatus =
        Json::parse(Read(BatchStatusFile));
    Check(WrittenBatchStatus.at("schema") ==
              "skrtg.native_viewer.batch_retarget_status.v3" &&
              WrittenBatchStatus.contains("timings") &&
              WrittenBatchStatus.at("jobs").at(0).contains("timings"),
          "profile-backed status v3 must expose batch and per-job timings");
    BatchRetargetStatus StatusRoundTrip;
    Check(ReadBatchRetargetStatus(
              BatchStatusFile, StatusRoundTrip, Error) &&
              StatusRoundTrip.AssetBinding.Required &&
              StatusRoundTrip.Jobs.size() == 2 &&
              StatusRoundTrip.Jobs[0].SourceAnimationGoldenJson ==
                  Golden,
          "profile-backed status v2 must retain package and per-job "
          "Golden provenance");
    Json CompatibleV2Status = WrittenBatchStatus;
    CompatibleV2Status["schema"] =
        "skrtg.native_viewer.batch_retarget_status.v2";
    CompatibleV2Status.erase("timings");
    for (Json& Job : CompatibleV2Status["jobs"])
        Job.erase("timings");
    const std::filesystem::path CompatibleV2StatusFile =
        Root.Path / "batch_status_v2_compatible.json";
    Write(
        CompatibleV2StatusFile,
        CompatibleV2Status.dump(2) + "\n");
    BatchRetargetStatus CompatibleV2RoundTrip;
    Check(ReadBatchRetargetStatus(
              CompatibleV2StatusFile,
              CompatibleV2RoundTrip, Error) &&
              CompatibleV2RoundTrip.Jobs.size() == 2,
          "new reader must retain profile-backed status v2 compatibility");
    StatusRoundTrip.Jobs[0].State =
        BatchRetargetJobState::Succeeded;
    StatusRoundTrip.Jobs[1].State =
        BatchRetargetJobState::Failed;
    std::vector<BatchReviewAnimation> ReviewAnimations =
        BuildBatchReviewAnimationList(StatusRoundTrip);
    Check(ReviewAnimations.size() == 1 &&
              ReviewAnimations[0].Id == "source_clip" &&
              ReviewAnimations[0].Label == "Source Clip" &&
              ReviewAnimations[0].ReviewPackage ==
                  StatusRoundTrip.Jobs[0].ReviewPackage,
          "Viewer animation list must expose only successful batch "
          "packages with their catalog identity");
    StatusRoundTrip.Jobs[1].State =
        BatchRetargetJobState::Succeeded;
    ReviewAnimations =
        BuildBatchReviewAnimationList(StatusRoundTrip);
    Check(ReviewAnimations.size() == 2 &&
              ReviewAnimations[0].Id == "source_clip" &&
              ReviewAnimations[1].Id == "source_clip_two",
          "Viewer animation list must preserve deterministic batch "
          "job order");
    StatusRoundTrip.Jobs[0].ReviewPackage.clear();
    ReviewAnimations =
        BuildBatchReviewAnimationList(StatusRoundTrip);
    Check(ReviewAnimations.size() == 1 &&
              ReviewAnimations[0].Id == "source_clip_two",
          "Viewer animation list must omit a successful status entry "
          "that has no review package path");
    Json SelectedStatusJson =
        Json::parse(Read(BatchStatusFile));
    SelectedStatusJson["candidateRouteSelected"] = true;
    const std::filesystem::path SelectedStatusFile =
        Root.Path / "batch_status_v2_selected.json";
    Write(
        SelectedStatusFile,
        SelectedStatusJson.dump(2) + "\n");
    BatchRetargetStatus RejectedSelectedStatus;
    Check(!ReadBatchRetargetStatus(
              SelectedStatusFile, RejectedSelectedStatus, Error),
          "profile-backed status v2 must reject candidate-route "
          "selection or adoption claims");
    Json ConcurrentStatusJson =
        Json::parse(Read(BatchStatusFile));
    ConcurrentStatusJson["executionPolicy"]
        ["maximumConcurrentJobs"] = 2;
    const std::filesystem::path ConcurrentStatusFile =
        Root.Path / "batch_status_v2_concurrent.json";
    Write(
        ConcurrentStatusFile,
        ConcurrentStatusJson.dump(2) + "\n");
    BatchRetargetStatus RejectedConcurrentStatus;
    Check(!ReadBatchRetargetStatus(
              ConcurrentStatusFile, RejectedConcurrentStatus, Error),
          "profile-backed status v2 must reject a non-serial "
          "execution claim");
    Json ScanningStatusJson =
        Json::parse(Read(BatchStatusFile));
    ScanningStatusJson["recursive"] = true;
    const std::filesystem::path ScanningStatusFile =
        Root.Path / "batch_status_v2_scanning.json";
    Write(
        ScanningStatusFile,
        ScanningStatusJson.dump(2) + "\n");
    BatchRetargetStatus RejectedScanningStatus;
    Check(!ReadBatchRetargetStatus(
              ScanningStatusFile, RejectedScanningStatus, Error),
          "profile-backed status v2 must reject recursive folder-scan "
          "semantics");
    Json MissingProvenanceStatusJson =
        Json::parse(Read(BatchStatusFile));
    MissingProvenanceStatusJson["jobs"][0].erase(
        "sourceAnimationGoldenJsonSha256");
    const std::filesystem::path MissingProvenanceStatusFile =
        Root.Path / "batch_status_v2_missing_provenance.json";
    Write(
        MissingProvenanceStatusFile,
        MissingProvenanceStatusJson.dump(2) + "\n");
    BatchRetargetStatus RejectedMissingProvenanceStatus;
    Check(!ReadBatchRetargetStatus(
              MissingProvenanceStatusFile,
              RejectedMissingProvenanceStatus, Error),
          "profile-backed status v2 must reject a job with missing "
          "per-animation provenance");

    BatchRetargetRequest CrossSource = Batch;
    CrossSource.CatalogAnimations[1].SourceSkeletonId =
        "test-target.v2";
    Check(!BuildBatchRetargetPlan(CrossSource).Success,
          "batch preflight must reject a clip bound to another source "
          "profile");
    BatchRetargetRequest Duplicate = Batch;
    Duplicate.CatalogAnimations[1].AnimationId = "source_clip";
    Check(!BuildBatchRetargetPlan(Duplicate).Success,
          "batch preflight must reject duplicate animation IDs");
    BatchRetargetRequest LooseFolder = Batch;
    LooseFolder.AnimationDirectory = CatalogRoot;
    Check(!BuildBatchRetargetPlan(LooseFolder).Success,
          "profile-backed batch must reject arbitrary folder scanning");
    BatchRetargetRequest RecursiveFolderSemantics = Batch;
    RecursiveFolderSemantics.Recursive = true;
    Check(!BuildBatchRetargetPlan(RecursiveFolderSemantics).Success,
          "profile-backed batch must reject recursive folder semantics "
          "for direct C++ callers");
    const std::string OriginalGoldenTwo = Read(GoldenTwo);
    Write(GoldenTwo, "tampered-golden");
    Check(!BuildBatchRetargetPlan(Batch).Success,
          "changed per-animation Golden JSON must fail the whole batch "
          "before output commit");
    Check(!std::filesystem::exists(Batch.OutputDirectory),
          "failed full-batch preflight must not create the output "
          "directory");
    Write(GoldenTwo, OriginalGoldenTwo);

    const std::string OriginalCatalog = Read(CatalogFile);
    Json MismatchedCatalog = Json::parse(OriginalCatalog);
    MismatchedCatalog["animations"][0]
        ["sourceSkeletonSignatureSha256"] =
            std::string(64, 'C');
    Write(CatalogFile, MismatchedCatalog.dump(2) + "\n");
    const RetargetAssetCatalogLoadResult MismatchLoad =
        LoadRetargetAssetCatalog(
            CatalogFile, false,
            {"test-source.v2", "test-target.v2"});
    Check(MismatchLoad.Success,
          "catalog loader should accept a structurally valid "
          "external skeleton fingerprint");
    Request.AssetBinding.CatalogSha256 =
        MismatchLoad.Catalog.CatalogSha256;
    const RetargetBridgePreflight SignatureMismatch =
        PreflightRetargetBridge(Request);
    const bool ReportedSignatureMismatch = std::any_of(
        SignatureMismatch.Errors.begin(),
        SignatureMismatch.Errors.end(),
        [](const std::string& Message)
        {
            return Message.find("skeleton signature") !=
                std::string::npos;
        });
    Check(!SignatureMismatch.Success &&
              ReportedSignatureMismatch,
          "Bridge must reject an animation whose skeleton fingerprint "
          "differs from the source profile");
    Write(CatalogFile, OriginalCatalog);
    Request.AssetBinding.CatalogSha256 =
        Catalog.Catalog.CatalogSha256;

    Write(Request.SourceRestFbx, "tampered-rest");
    Check(!PreflightRetargetBridge(Request).Success,
          "tampered extracted profile resource must fail closed");
}

void TestLegacyV4JsonShape()
{
    TemporaryRoot Root;
    RetargetBridgeRequest Request;
    Request.RouteKind = RetargetBridgeRouteKind::UEIKJsonV1;
    Request.SourceFbxImportMode =
        RetargetBridgeSourceFbxImportMode::UE58ExactGoldenV1;
    Request.RestFbxImportMode =
        RetargetBridgeRestFbxImportMode::
            UE58ExportedYReflectionV1;
    Request.AssetBinding.Required = true;
    Request.OutputDirectory = Root.Path / "status";

    const std::filesystem::path RequestFile =
        Root.Path / "legacy_v4.json";
    std::string Error;
    Check(WriteRetargetBridgeRequest(
              Request, RequestFile, Error),
          "legacy v4 request fixture should serialize");
    const Json RequestJson = Json::parse(Read(RequestFile));
    Check(RequestJson.at("schema") ==
              "skrtg.native_viewer.retarget_bridge_request.v4",
          "profile-free asset request must retain v4 schema");
    Check(!RequestJson.at("assetSelection").contains(
              "characterProfiles"),
          "legacy v4 request shape must not gain v5 profile fields");

    const RetargetBridgeRunResult Run =
        RunRetargetBridge(Request);
    Check(!Run.Success &&
              std::filesystem::is_regular_file(Run.StatusJson),
          "failed legacy preflight should still write status fixture");
    const Json Status = Json::parse(Read(Run.StatusJson));
    Check(Status.at("schema") ==
              "skrtg.native_viewer.retarget_bridge_status.v4",
          "profile-free status must retain v4 schema");
    Check(!Status.at("assetSelection").contains(
              "sourceProfilePackage") &&
              !Status.contains(
                  "sourceProfilePackageSha256"),
          "legacy v4 status shape must not gain v5 profile fields");
}

void TestLegacyBatchV2JsonShape()
{
    TemporaryRoot Root;
    BatchRetargetRequest Request;
    Request.SourceCharacter.DefinitionKind = "ue_ik_json_v1";
    Request.TargetCharacter.DefinitionKind = "ue_ik_json_v1";
    Request.AnimationDirectory = Root.Path / "animations";
    Request.OutputDirectory = Root.Path / "output";
    Request.Recursive = true;

    const std::filesystem::path RequestFile =
        Root.Path / "legacy_batch_v2.json";
    std::string Error;
    Check(WriteBatchRetargetRequest(
              Request, RequestFile, Error),
          "legacy batch v2 request should serialize");
    const Json RequestJson = Json::parse(Read(RequestFile));
    Check(RequestJson.at("schema") ==
              "skrtg.native_viewer.batch_retarget_request.v2",
          "profile-free UE batch must retain the v2 schema");
    Check(!RequestJson.contains("assetSelection") &&
              !RequestJson.contains("animations"),
          "legacy batch v2 must not gain profile/catalog fields");
    BatchRetargetRequest RoundTrip;
    Check(ReadBatchRetargetRequest(
              RequestFile, RoundTrip, Error) &&
              !RoundTrip.AssetBinding.Required &&
              RoundTrip.CatalogAnimations.empty() &&
              RoundTrip.Recursive,
          "legacy batch v2 should remain readable without profile "
          "bindings");

    BatchRetargetStatus Status;
    Status.MaximumConcurrentJobs = 1;
    Status.Jobs.push_back({});
    const std::filesystem::path StatusFile =
        Root.Path / "legacy_batch_status_v1.json";
    Check(WriteBatchRetargetStatus(
              Status, StatusFile, Error),
          "legacy batch status v1 should serialize");
    const Json StatusJson = Json::parse(Read(StatusFile));
    Check(StatusJson.at("schema") ==
              "skrtg.native_viewer.batch_retarget_status.v1" &&
              !StatusJson.at("jobs").at(0).contains(
                  "sourceAnimationId") &&
              !StatusJson.contains("assetSelection"),
          "legacy batch status v1 shape must not gain profile/catalog "
          "fields");
}
} // namespace

int main()
{
    TestProfileBoundBridge();
    TestLegacyV4JsonShape();
    TestLegacyBatchV2JsonShape();
    if (Failures != 0)
    {
        std::cerr << Failures
                  << " profile Bridge test(s) failed\n";
        return 1;
    }
    std::cout << "All profile Bridge tests passed\n";
    return 0;
}
