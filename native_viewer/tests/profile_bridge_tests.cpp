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
    Write(Animation, "source-animation");
    Write(Golden, "{}");

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
             Json::array(
                 {{{"id", "source_clip"},
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
                        Golden)}}})}}
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
} // namespace

int main()
{
    TestProfileBoundBridge();
    TestLegacyV4JsonShape();
    if (Failures != 0)
    {
        std::cerr << Failures
                  << " profile Bridge test(s) failed\n";
        return 1;
    }
    std::cout << "All profile Bridge tests passed\n";
    return 0;
}
