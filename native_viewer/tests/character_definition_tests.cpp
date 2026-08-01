#include "skrtg/viewer/profile/character_definition.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

void Write(const std::filesystem::path& Path, const std::string& Bytes)
{
    std::filesystem::create_directories(Path.parent_path());
    std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
    Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
}

struct TemporaryRoot
{
    std::filesystem::path Path;
    TemporaryRoot()
    {
        const auto Stamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch().count();
        Path = std::filesystem::temp_directory_path() /
            ("skrtg_character_definition_tests_" + std::to_string(Stamp));
        std::filesystem::create_directories(Path);
    }
    ~TemporaryRoot()
    {
        std::error_code Error;
        std::filesystem::remove_all(Path, Error);
    }
};

Json Coordinate()
{
    return {
        {"handedness", "left"},
        {"forwardAxis", "+X"},
        {"rightAxis", "+Y"},
        {"upAxis", "+Z"},
        {"distanceUnit", "centimeter"},
        {"quaternionComponentOrder", "x,y,z,w"}};
}

Json Transform(const double X, const double Y, const double Z)
{
    return {
        {"translationCm", {X, Y, Z}},
        {"rotation", {0.0, 0.0, 0.0, 1.0}},
        {"scale", {1.0, 1.0, 1.0}}};
}

Json Definition(const bool Complete = true)
{
    return {
        {"schema", Profile::CharacterDefinitionSchema},
        {"schemaVersion", 1},
        {"character",
         {{"id", "test_character"},
          {"displayName", "Test Character"},
          {"rigAssetName", Complete ? "IK_Test" : ""},
          {"restPoseKind", "t_pose"}}},
        {"coordinateContract", Coordinate()},
        {"skeleton",
         {{"bones", Json::array({
             {{"index", 0}, {"parentIndex", -1}, {"name", "root"},
              {"local", Transform(0.0, 0.0, 0.0)}},
             {{"index", 1}, {"parentIndex", 0}, {"name", "pelvis"},
              {"local", Transform(0.0, 0.0, 100.0)}}})}}},
        {"retarget",
         {{"rootBone", Complete ? "root" : ""},
          {"pelvisBone", Complete ? "pelvis" : ""},
          {"chains", Complete
              ? Json::array({
                    {{"name", "Root"},
                     {"startBone", "root"},
                     {"endBone", "pelvis"},
                     {"ikGoal", ""}}})
              : Json::array()}}}};
}

Json Alignment()
{
    return {
        {"schema", "skrtg.ue_ik_asset_export.v2"},
        {"schemaVersion", 2},
        {"kind", "ikRetargeter"},
        {"valid", true},
        {"unrealEngineVersion", "5.8.0"},
        {"coordinateContract", Coordinate()},
        {"source", {{"ikRig", {{"assetName", "IK_Canonical"}}}}},
        {"target", {{"ikRig", {{"assetName", "IK_Test"}}}}}};
}

std::string XmlDefinition()
{
    return
        "<?xml version=\"1.0\"?>\n"
        "<CharacterDefinition schema=\"skrtg.character_definition.v1\" schemaVersion=\"1\">\n"
        "  <Character id=\"test_character\" displayName=\"Test Character\" rigAssetName=\"IK_Test\" restPoseKind=\"t_pose\"/>\n"
        "  <Coordinate handedness=\"left\" forwardAxis=\"+X\" rightAxis=\"+Y\" upAxis=\"+Z\" distanceUnit=\"centimeter\" quaternionComponentOrder=\"x,y,z,w\"/>\n"
        "  <Skeleton>\n"
        "    <Bone index=\"0\" parentIndex=\"-1\" name=\"root\" localTranslationCm=\"0,0,0\" localRotation=\"0,0,0,1\" localScale=\"1,1,1\"/>\n"
        "    <Bone index=\"1\" parentIndex=\"0\" name=\"pelvis\" localTranslationCm=\"0,0,100\" localRotation=\"0,0,0,1\" localScale=\"1,1,1\"/>\n"
        "  </Skeleton>\n"
        "  <Retarget rootBone=\"root\" pelvisBone=\"pelvis\">\n"
        "    <Chain name=\"Root\" startBone=\"root\" endBone=\"pelvis\" ikGoal=\"\"/>\n"
        "  </Retarget>\n"
        "</CharacterDefinition>\n";
}

void TestAdaptersAndNormalization()
{
    TemporaryRoot Root;
    const auto JsonPath = Root.Path / "character.json";
    const auto XmlPath = Root.Path / "character.xml";
    const auto DraftPath = Root.Path / "draft.json";
    Write(JsonPath, Definition().dump(2) + "\n");
    Write(XmlPath, XmlDefinition());
    Write(DraftPath, Definition(false).dump(2) + "\n");

    const auto JsonResult = Profile::InspectCharacterDefinition(JsonPath);
    Check(JsonResult.Success, "normalized JSON should inspect");
    Check(JsonResult.Definition.RuntimeDefinitionComplete,
          "complete JSON should be runtime-complete");
    Check(JsonResult.Definition.BoneCount == 2,
          "JSON bone count should be preserved");
    Check(JsonResult.Definition.ChainCount == 1,
          "JSON chain count should be preserved");
    Check(JsonResult.Definition.SkeletonSignatureSha256.size() == 64,
          "missing fingerprint should be generated deterministically");

    const auto XmlResult = Profile::InspectCharacterDefinition(XmlPath);
    Check(XmlResult.Success, "strict XML should normalize");
    Check(XmlResult.Definition.SourceFormat == "skrtg_character_xml",
          "XML adapter should identify itself");
    Check(XmlResult.Definition.SkeletonSignatureSha256 ==
              JsonResult.Definition.SkeletonSignatureSha256,
          "equivalent JSON and XML skeletons should share a fingerprint");

    const auto Draft = Profile::InspectCharacterDefinition(DraftPath);
    Check(Draft.Success, "incomplete definition should still be inspectable");
    Check(!Draft.Definition.RuntimeDefinitionComplete,
          "incomplete definition must not be runtime-ready");
    Check(Draft.Definition.MissingRequirements.size() == 4,
          "draft should report root, pelvis, chains, and rig asset");

    const auto NormalizedPath = Root.Path / "normalized.json";
    std::string Error;
    Check(Profile::WriteNormalizedCharacterDefinition(
              XmlResult, NormalizedPath, Error),
          "normalized definition should write atomically: " + Error);
    Check(!Profile::WriteNormalizedCharacterDefinition(
               XmlResult, NormalizedPath, Error),
          "normalization must refuse overwrite");

    Json WrongCoordinate = Definition();
    WrongCoordinate["coordinateContract"]["upAxis"] = "+Y";
    const auto WrongPath = Root.Path / "wrong-axis.json";
    Write(WrongPath, WrongCoordinate.dump(2));
    Check(!Profile::InspectCharacterDefinition(WrongPath).Success,
          "unsupported coordinate contracts must fail closed");

    Json UnknownField = Definition();
    UnknownField["guessBoneNames"] = true;
    const auto UnknownPath = Root.Path / "unknown-field.json";
    Write(UnknownPath, UnknownField.dump(2));
    Check(!Profile::InspectCharacterDefinition(UnknownPath).Success,
          "unknown normalized fields must fail closed");

    Json ModelMismatch = Definition();
    ModelMismatch["skeleton"]["bones"][1]["model"] =
        Transform(0.0, 0.0, 999.0);
    const auto MismatchPath = Root.Path / "model-mismatch.json";
    Write(MismatchPath, ModelMismatch.dump(2));
    Check(!Profile::InspectCharacterDefinition(MismatchPath).Success,
          "provided model transforms must match the rebuilt hierarchy");

    std::string UnknownXml = XmlDefinition();
    const std::string Marker = " restPoseKind=\"t_pose\"";
    UnknownXml.replace(
        UnknownXml.find(Marker), Marker.size(),
        Marker + " guessBoneNames=\"true\"");
    const auto UnknownXmlPath = Root.Path / "unknown-attribute.xml";
    Write(UnknownXmlPath, UnknownXml);
    Check(!Profile::InspectCharacterDefinition(UnknownXmlPath).Success,
          "unknown XML attributes must fail closed");
}

void TestCreateRequestAndPackage()
{
    TemporaryRoot Root;
    const auto Inputs = Root.Path / "inputs";
    Write(Inputs / "rest.fbx", "Kaydara FBX Binary  profile-rest");
    Write(Inputs / "definition.json", Definition().dump(2) + "\n");
    Write(Inputs / "alignment.json", Alignment().dump(2) + "\n");
    const Json RequestJson = {
        {"schema", Profile::ProfileCreateRequestSchema},
        {"schemaVersion", 1},
        {"profile",
         {{"id", "test_character"},
          {"version", "2.0.0-preview.1"},
          {"displayName", "Test Character"},
          {"canonicalProfileId", "ue5_manny"},
          {"restPoseKind", "t_pose"},
          {"sourceEnabled", true},
          {"targetEnabled", true}}},
        {"inputs",
         {{"restFbx", "inputs/rest.fbx"},
          {"definition", "inputs/definition.json"},
          {"alignmentRetargeterJson", "inputs/alignment.json"},
          {"format", "auto"}}},
        {"output", {{"package", "out/test_character.skrtgprofile"}}}};
    const auto RequestPath = Root.Path / "profile-create.json";
    Write(RequestPath, RequestJson.dump(2) + "\n");

    Profile::CharacterProfileCreateRequest Request;
    std::string Error;
    Check(Profile::ReadCharacterProfileCreateRequest(
              RequestPath, Request, Error),
          "relative profile create request should parse: " + Error);
    Check(Request.RestFbx ==
              std::filesystem::absolute(Inputs / "rest.fbx").lexically_normal(),
          "request paths should resolve relative to request file");

    Json UnknownRequest = RequestJson;
    UnknownRequest["profile"]["guessBoneNames"] = true;
    const auto UnknownRequestPath = Root.Path / "unknown-request.json";
    Write(UnknownRequestPath, UnknownRequest.dump(2));
    Profile::CharacterProfileCreateRequest RejectedRequest;
    Check(!Profile::ReadCharacterProfileCreateRequest(
               UnknownRequestPath, RejectedRequest, Error),
          "unknown request fields must fail schema validation");

    const auto Created = Profile::CreateCharacterProfile(Request);
    Check(Created.Success, "complete normalized definition should package");
    Check(Created.Package.Success, "package result should be successful");
    Check(Created.Package.Profile.SourceDefinitionFormat ==
              "skrtg_character_json",
          "package should retain source format provenance");
    Check(Created.Package.Profile.DefinitionImporter ==
              "skrtg_character_json",
          "package should retain importer provenance");
    Check(Created.Package.Profile.RestPoseKind == "t_pose",
          "package should retain declared rest pose kind");

    const auto Inspected = Profile::InspectCharacterProfilePackage(
        Request.OutputPackage);
    Check(Inspected.Success, "created package should verify");
    Check(Inspected.Profile.SourceDefinitionSha256.size() == 64,
          "inspected package should retain source definition SHA-256");

    Profile::CharacterProfileCreateRequest DraftRequest = Request;
    DraftRequest.OutputPackage = Root.Path / "out/draft.skrtgprofile";
    Write(Inputs / "draft.json", Definition(false).dump(2));
    DraftRequest.DefinitionFile = Inputs / "draft.json";
    const auto Draft = Profile::CreateCharacterProfile(DraftRequest);
    Check(!Draft.Success && !std::filesystem::exists(DraftRequest.OutputPackage),
          "draft definitions must never commit a runtime package");
}
} // namespace

int main()
{
    TestAdaptersAndNormalization();
    TestCreateRequestAndPackage();
    if (Failures != 0)
    {
        std::cerr << Failures << " character definition test(s) failed\n";
        return 1;
    }
    std::cout << "character definition tests passed\n";
    return 0;
}
