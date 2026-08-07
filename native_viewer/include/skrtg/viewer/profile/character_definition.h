#pragma once

#include "skrtg/viewer/profile/character_profile.h"

#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::viewer::profile
{
inline constexpr const char* CharacterDefinitionSchema =
    "skrtg.character_definition.v1";
inline constexpr const char* ProfileCreateRequestSchema =
    "skrtg.profile_create_request.v1";

enum class CharacterDefinitionFormat
{
    Auto,
    UEIKRigJson,
    SKRTGCharacterJson,
    SKRTGCharacterXml,
    RestFbx
};

struct CharacterDefinitionInspectOptions
{
    CharacterDefinitionFormat Format = CharacterDefinitionFormat::Auto;
    std::string RestPoseKind = "unknown";
};

struct CharacterDefinitionSummary
{
    std::string SourceFormat;
    std::string SourceSchema;
    std::string AdapterId;
    std::string AdapterVersion;
    std::string InputSha256;
    std::string CharacterId;
    std::string DisplayName;
    std::string RigAssetName;
    std::string SkeletonSignatureSha256;
    std::string SkeletonSignatureKind;
    std::string RetargetRootBone;
    std::string RetargetPelvisBone;
    std::string RestPoseKind = "unknown";
    std::size_t BoneCount = 0;
    std::size_t ChainCount = 0;
    bool RuntimeDefinitionComplete = false;
    std::vector<std::string> MissingRequirements;
};

struct CharacterDefinitionInspectResult
{
    bool Success = false;
    std::filesystem::path SourcePath;
    CharacterDefinitionSummary Definition;
    std::string NormalizedJson;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

struct CharacterProfileCreateRequest
{
    std::filesystem::path RequestFile;
    std::filesystem::path OutputPackage;
    std::string ProfileId;
    std::string ProfileVersion;
    std::string DisplayName;
    std::string CanonicalProfileId = "ue5_manny";
    std::filesystem::path RestFbx;
    std::filesystem::path DefinitionFile;
    std::filesystem::path AlignmentRetargeterJson;
    CharacterDefinitionFormat Format = CharacterDefinitionFormat::Auto;
    std::string RestPoseKind = "unknown";
    bool SourceEnabled = true;
    bool TargetEnabled = true;
    CharacterMeshSelectionDescriptor MeshSelection;
};

struct CharacterProfileCreateResult
{
    bool Success = false;
    std::string Stage;
    CharacterDefinitionSummary Definition;
    ProfilePackResult Package;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

const char* CharacterDefinitionFormatName(CharacterDefinitionFormat Format);

bool ParseCharacterDefinitionFormat(
    const std::string& Text,
    CharacterDefinitionFormat& OutFormat);

std::vector<std::string> SupportedCharacterDefinitionAdapters();

CharacterDefinitionInspectResult InspectCharacterDefinition(
    const std::filesystem::path& SourcePath,
    const CharacterDefinitionInspectOptions& Options = {});

bool WriteNormalizedCharacterDefinition(
    const CharacterDefinitionInspectResult& Definition,
    const std::filesystem::path& OutputJson,
    std::string& OutError);

bool ReadCharacterProfileCreateRequest(
    const std::filesystem::path& RequestJson,
    CharacterProfileCreateRequest& OutRequest,
    std::string& OutError);

CharacterProfileCreateResult CreateCharacterProfile(
    const CharacterProfileCreateRequest& Request);
} // namespace skrtg::viewer::profile
