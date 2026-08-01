#include "skrtg/viewer/profile/character_profile.h"

#include "nlohmann/json.hpp"

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
            ("skrtgprofile_tests_" + std::to_string(Stamp));
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

Json RigDocument()
{
    return {
        {"schema", "skrtg.ue_ik_asset_export.v2"},
        {"schemaVersion", 2},
        {"kind", "ikRigDefinition"},
        {"valid", true},
        {"unrealEngineVersion", "5.8.0"},
        {"coordinateContract", CoordinateContract()},
        {"asset", {{"assetName", "IK_Test"}}},
        {"retargetRootBone", "pelvis"},
        {"retargetPelvisBone", "pelvis"},
        {"referenceSkeleton",
         {
             {"fingerprintSha256",
              std::string(64, 'A')},
             {"bones",
              Json::array(
                  {{{"name", "root"}, {"parentIndex", -1}},
                   {{"name", "pelvis"}, {"parentIndex", 0}}})},
         }}};
}

Json AlignmentDocument(const char* TargetRig = "IK_Test")
{
    return {
        {"schema", "skrtg.ue_ik_asset_export.v2"},
        {"schemaVersion", 2},
        {"kind", "ikRetargeter"},
        {"valid", true},
        {"unrealEngineVersion", "5.8.0"},
        {"coordinateContract", CoordinateContract()},
        {"source",
         {
             {"ikRig", {{"assetName", "IK_Canonical"}}},
         }},
        {"target",
         {
             {"ikRig", {{"assetName", TargetRig}}},
         }}};
}

void TestIdentityValidation()
{
    Check(Profile::IsCharacterProfileId("mixamo_ybot"),
          "lowercase profile id should be valid");
    Check(Profile::IsCharacterProfileId("ue5-manny.v2"),
          "portable punctuation should be valid");
    Check(!Profile::IsCharacterProfileId("MetaHuman"),
          "uppercase profile id must be rejected");
    Check(!Profile::IsCharacterProfileId("../escape"),
          "path-like profile id must be rejected");
    Check(!Profile::IsCharacterProfileId("con"),
          "Windows device profile id must be rejected");

    Check(Profile::IsCharacterProfileVersion("1.0.0"),
          "plain semantic version should be valid");
    Check(Profile::IsCharacterProfileVersion("1.2.3-preview.1"),
          "semantic prerelease version should be valid");
    Check(!Profile::IsCharacterProfileVersion("1.0"),
          "incomplete semantic version must be rejected");
    Check(!Profile::IsCharacterProfileVersion("v1.0.0"),
          "prefixed semantic version must be rejected");
    Check(!Profile::IsCharacterProfileVersion("01.0.0"),
          "semantic version leading zero must be rejected");
    Check(!Profile::IsCharacterProfileVersion("1.0.0-preview..1"),
          "empty prerelease identifier must be rejected");
    Check(Profile::CompareCharacterProfileVersions(
              "1.10.0", "1.2.0") > 0,
          "semantic numeric components must not sort lexically");
    Check(Profile::CompareCharacterProfileVersions(
              "1.0.0", "1.0.0-preview.1") > 0,
          "release version must sort after prerelease");
}

void TestPackageLifecycle()
{
    TemporaryRoot Root;
    const std::filesystem::path Inputs = Root.Path / "inputs";
    const std::filesystem::path Rest = Inputs / "rest.fbx";
    const std::filesystem::path Rig = Inputs / "rig.json";
    const std::filesystem::path Alignment =
        Inputs / "alignment.json";
    Write(Rest, "Kaydara FBX Binary  test-rest-bytes");
    Write(Rig, RigDocument().dump(2) + "\n");
    Write(Alignment, AlignmentDocument().dump(2) + "\n");

    Profile::ProfilePackRequest Request;
    Request.OutputPackage =
        Root.Path / "test_character.skrtgprofile";
    Request.ProfileId = "test_character";
    Request.ProfileVersion = "1.2.3-preview.1";
    Request.DisplayName = "Test Character";
    Request.CanonicalProfileId = "ue5_manny";
    Request.RestFbx = Rest;
    Request.IkRigJson = Rig;
    Request.AlignmentRetargeterJson = Alignment;

    const Profile::ProfilePackResult Packed =
        Profile::WriteCharacterProfilePackage(Request);
    Check(Packed.Success, "valid profile package should pack");
    Check(Packed.Errors.empty(),
          "valid profile package should not report errors");
    Check(Packed.Entries.size() == 6,
          "v1 package should contain exactly six entries");
    Check(Packed.Profile.ProfileId == Request.ProfileId,
          "packed profile id should round-trip");
    Check(Packed.Profile.SkeletonSignatureSha256 ==
              std::string(64, 'A'),
          "skeleton fingerprint should round-trip");

    const Profile::ProfileInspectResult Inspected =
        Profile::InspectCharacterProfilePackage(
            Request.OutputPackage);
    Check(Inspected.Success,
          "committed package should inspect successfully");
    Check(Inspected.PackageSha256 == Packed.PackageSha256,
          "pack and inspect package digests should match");

    const Profile::ProfilePackResult Overwrite =
        Profile::WriteCharacterProfilePackage(Request);
    Check(!Overwrite.Success,
          "packer must refuse to overwrite an existing package");

    std::string PackageBytes = Read(Request.OutputPackage);
    Check(PackageBytes.size() > 128,
          "test profile package should have content");
    if (PackageBytes.size() > 128)
    {
        PackageBytes.back() =
            PackageBytes.back() == '\0' ? '\1' : '\0';
        const std::filesystem::path Corrupted =
            Root.Path / "corrupted.skrtgprofile";
        Write(Corrupted, PackageBytes);
        Check(!Profile::InspectCharacterProfilePackage(Corrupted)
                   .Success,
              "payload corruption must fail verification");

        PackageBytes.pop_back();
        const std::filesystem::path Truncated =
            Root.Path / "truncated.skrtgprofile";
        Write(Truncated, PackageBytes);
        Check(!Profile::InspectCharacterProfilePackage(Truncated)
                   .Success,
              "truncated package must fail verification");
    }

    const std::filesystem::path Store = Root.Path / "store";
    const std::filesystem::path StalePartial =
        Store / Request.ProfileId /
        (Request.ProfileVersion + ".partial." +
         Packed.PackageSha256.substr(0, 12));
    Write(StalePartial / "stale.txt", "stale");
    const Profile::ProfileInstallResult Installed =
        Profile::InstallCharacterProfilePackage(
            Request.OutputPackage, Store);
    Check(Installed.Success,
          "verified profile should install atomically");
    Check(!Installed.AlreadyInstalled,
          "first profile install should not be idempotent");
    Check(std::filesystem::is_regular_file(
              Profile::InstalledProfileResourcePath(
                  Installed.Installed,
                  Installed.Installed.Profile.RestFbx)),
          "installed rest FBX should exist");

    const Profile::ProfileInstallResult Reinstalled =
        Profile::InstallCharacterProfilePackage(
            Request.OutputPackage, Store);
    Check(Reinstalled.Success && Reinstalled.AlreadyInstalled,
          "installing the identical package should be idempotent");

    const Profile::ProfileDiscoveryResult Discovered =
        Profile::DiscoverInstalledCharacterProfiles(Store);
    Check(Discovered.Success,
          "profile store discovery should succeed");
    Check(Discovered.Profiles.size() == 1,
          "profile store should expose one installed profile");

    const std::filesystem::path NestedInstall =
        Store / "unmanaged" / "deep" / Request.ProfileId /
        Request.ProfileVersion;
    std::filesystem::create_directories(
        NestedInstall.parent_path());
    std::filesystem::copy(
        Installed.Installed.InstallDirectory,
        NestedInstall,
        std::filesystem::copy_options::recursive);
    const Profile::ProfileDiscoveryResult ExactDepth =
        Profile::DiscoverInstalledCharacterProfiles(Store);
    Check(ExactDepth.Success &&
              ExactDepth.Profiles.size() == 1,
          "profile discovery must ignore nested unmanaged installs");

    const std::filesystem::path InstalledRest =
        Profile::InstalledProfileResourcePath(
            Installed.Installed,
            Installed.Installed.Profile.RestFbx);
    Write(InstalledRest, "tampered");
    const Profile::ProfileDiscoveryResult Damaged =
        Profile::DiscoverInstalledCharacterProfiles(Store);
    Check(Damaged.Success,
          "damaged profile should not make the store unreadable");
    Check(Damaged.Profiles.empty(),
          "damaged installed resources must fail closed");
    Check(!Damaged.Warnings.empty(),
          "damaged installed resources should emit a warning");
    const Profile::ProfileInstallResult Repaired =
        Profile::InstallCharacterProfilePackage(
            Request.OutputPackage, Store);
    Check(Repaired.Success && !Repaired.AlreadyInstalled,
          "reinstalling the same package must repair damaged "
          "extracted resources");
    Check(Read(InstalledRest) ==
              "Kaydara FBX Binary  test-rest-bytes",
          "profile repair must restore verified resource bytes");

    Write(
        Repaired.Installed.InstallDirectory / "content" /
            "undeclared.txt",
        "undeclared");
    const Profile::ProfileDiscoveryResult ExtraContent =
        Profile::DiscoverInstalledCharacterProfiles(Store);
    Check(ExtraContent.Success &&
              ExtraContent.Profiles.empty(),
          "installed content with undeclared files must fail closed");
    const Profile::ProfileInstallResult ExtraRepaired =
        Profile::InstallCharacterProfilePackage(
            Request.OutputPackage, Store);
    Check(ExtraRepaired.Success &&
              !std::filesystem::exists(
                  ExtraRepaired.Installed.InstallDirectory /
                      "content" / "undeclared.txt"),
          "same-package repair must remove undeclared installed "
          "content");

    Profile::InstalledCharacterProfile Outside =
        Installed.Installed;
    Outside.InstallDirectory = Root.Path / "outside";
    Check(!Profile::DeleteInstalledCharacterProfile(
               Outside, Store).Success,
          "delete must reject paths outside the managed store");

    const Profile::ProfileDeleteResult Deleted =
        Profile::DeleteInstalledCharacterProfile(
            Installed.Installed, Store);
    Check(Deleted.Success,
          "profile with a matching receipt should delete");
    Check(!std::filesystem::exists(
              Installed.Installed.InstallDirectory),
          "deleted profile directory should be absent");

    const Profile::ProfileInstallResult ReceiptFixture =
        Profile::InstallCharacterProfilePackage(
            Request.OutputPackage, Store);
    Check(ReceiptFixture.Success,
          "receipt validation fixture should install");
    Write(
        ReceiptFixture.Installed.InstallDirectory /
            "install.json",
        Json{
            {"schema", "skrtg.character_profile_install.v1"},
            {"schemaVersion", 1},
            {"packageSha256", Json::array()}}
            .dump(2) +
            "\n");
    const Profile::ProfileDiscoveryResult BadReceipt =
        Profile::DiscoverInstalledCharacterProfiles(Store);
    Check(BadReceipt.Success && BadReceipt.Profiles.empty() &&
              !BadReceipt.Warnings.empty(),
          "wrong-type install receipt must fail closed without "
          "crashing discovery");

    Profile::ProfilePackRequest WrongRig = Request;
    WrongRig.OutputPackage =
        Root.Path / "wrong_rig.skrtgprofile";
    Write(Alignment, AlignmentDocument("IK_Other").dump(2) + "\n");
    Check(!Profile::WriteCharacterProfilePackage(WrongRig).Success,
          "mismatched Retargeter target IK Rig must fail");

    Write(Alignment, AlignmentDocument().dump(2) + "\n");
    Json WrongTypeRig = RigDocument();
    WrongTypeRig["schemaVersion"] = "2";
    Write(Rig, WrongTypeRig.dump(2) + "\n");
    Profile::ProfilePackRequest WrongType = Request;
    WrongType.OutputPackage =
        Root.Path / "wrong_type.skrtgprofile";
    Check(!Profile::WriteCharacterProfilePackage(WrongType).Success,
          "wrong-type UE JSON fields must fail without crashing the "
          "profile packer");

    std::string DeepJson = "{\"deep\":";
    for (int Index = 0; Index < 140; ++Index)
        DeepJson.push_back('[');
    DeepJson.push_back('0');
    for (int Index = 0; Index < 140; ++Index)
        DeepJson.push_back(']');
    DeepJson += "}\n";
    Write(Rig, DeepJson);
    Profile::ProfilePackRequest TooDeep = Request;
    TooDeep.OutputPackage =
        Root.Path / "too_deep.skrtgprofile";
    Check(!Profile::WriteCharacterProfilePackage(TooDeep).Success,
          "deeply nested definition JSON must hit the parser depth "
          "limit without crashing");
}
} // namespace

int main()
{
    TestIdentityValidation();
    TestPackageLifecycle();
    if (Failures != 0)
    {
        std::cerr << Failures
                  << " skrtgprofile test(s) failed\n";
        return 1;
    }
    std::cout << "All skrtgprofile tests passed\n";
    return 0;
}
