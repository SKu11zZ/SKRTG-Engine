#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::viewer::profile
{
inline constexpr std::size_t MaximumProfileEntries = 32;
inline constexpr std::uint64_t MaximumProfilePackageBytes =
    16ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t MaximumProfileJsonBytes =
    16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t MaximumDefinitionJsonBytes =
    16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t MaximumProfileIdBytes = 128;
inline constexpr std::size_t MaximumProfileVersionBytes = 64;
inline constexpr std::size_t MaximumProfileMeshNodePaths = 64;
inline constexpr std::size_t MaximumProfileMeshNodePathBytes = 4096;
inline constexpr const char* CharacterMeshSelectionSchema =
    "skrtg.character_mesh_selection.v1";

struct ProfileResource
{
    std::filesystem::path RelativePath;
    std::uint64_t ByteCount = 0;
    std::string Sha256;
};

// A hash-bound, exact FBX scene-node selection used only for runtime mesh
// presentation. It never changes the skeleton, rest pose, chain mapping, or
// solver. Declared=false preserves legacy single-mesh profiles; a runtime may
// require an explicit declaration when an FBX contains multiple Mesh nodes.
struct CharacterMeshSelectionDescriptor
{
    bool Declared = false;
    int ActiveLod = -1;
    std::vector<std::string> MeshNodePaths;
};

struct CharacterProfileDescriptor
{
    std::string ProfileId;
    std::string ProfileVersion;
    std::string DisplayName;
    std::string CanonicalProfileId;
    std::string DefinitionKind;
    std::string SkeletonSignatureSha256;
    std::string UnrealEngineVersion;
    std::string RetargetRootBone;
    std::string RetargetPelvisBone;
    std::string SourceDefinitionFormat;
    std::string SourceDefinitionSha256;
    std::string DefinitionImporter;
    std::string DefinitionImporterVersion;
    std::string RestPoseKind;
    bool SourceEnabled = true;
    bool TargetEnabled = true;
    CharacterMeshSelectionDescriptor MeshSelection;
    ProfileResource ProfileJson;
    ProfileResource RestFbx;
    ProfileResource IkRigJson;
    ProfileResource AlignmentRetargeterJson;
};

struct ProfilePackageEntry
{
    std::filesystem::path RelativePath;
    std::uint64_t ByteCount = 0;
    std::string Sha256;
};

struct ProfilePackRequest
{
    std::filesystem::path OutputPackage;
    std::string ProfileId;
    std::string ProfileVersion;
    std::string DisplayName;
    std::string CanonicalProfileId = "ue5_manny";
    std::filesystem::path RestFbx;
    std::filesystem::path IkRigJson;
    std::filesystem::path AlignmentRetargeterJson;
    // Optional authoring provenance. Legacy callers may leave every field
    // empty. New Character Definition importers set the complete group so a
    // packaged profile remains traceable to its source without weakening the
    // compiled UE IK JSON runtime contract.
    std::string SourceDefinitionFormat;
    std::string SourceDefinitionSha256;
    std::string DefinitionImporter;
    std::string DefinitionImporterVersion;
    std::string RestPoseKind;
    bool SourceEnabled = true;
    bool TargetEnabled = true;
    CharacterMeshSelectionDescriptor MeshSelection;
};

struct ProfilePackResult
{
    bool Success = false;
    std::filesystem::path PackagePath;
    std::string PackageSha256;
    CharacterProfileDescriptor Profile;
    std::vector<ProfilePackageEntry> Entries;
    std::vector<std::string> Errors;
};

struct ProfileInspectResult
{
    bool Success = false;
    std::filesystem::path PackagePath;
    std::string PackageSha256;
    CharacterProfileDescriptor Profile;
    std::vector<ProfilePackageEntry> Entries;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

struct InstalledCharacterProfile
{
    CharacterProfileDescriptor Profile;
    std::filesystem::path InstallDirectory;
    std::filesystem::path PackagePath;
    std::string PackageSha256;
};

struct ProfileInstallResult
{
    bool Success = false;
    bool AlreadyInstalled = false;
    InstalledCharacterProfile Installed;
    std::vector<std::string> Errors;
};

struct ProfileDiscoveryResult
{
    bool Success = false;
    std::filesystem::path StoreRoot;
    std::vector<InstalledCharacterProfile> Profiles;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

struct ProfileDeleteResult
{
    bool Success = false;
    std::vector<std::string> Errors;
};

bool IsCharacterProfileId(const std::string& Value);

bool IsCharacterProfileVersion(const std::string& Value);

bool ValidateCharacterMeshSelection(
    const CharacterMeshSelectionDescriptor& Value,
    std::string& OutError);

int CompareCharacterProfileVersions(
    const std::string& Left,
    const std::string& Right);

std::filesystem::path DefaultCharacterProfileStore();

ProfilePackResult WriteCharacterProfilePackage(
    const ProfilePackRequest& Request);

ProfileInspectResult InspectCharacterProfilePackage(
    const std::filesystem::path& PackagePath);

ProfileInstallResult InstallCharacterProfilePackage(
    const std::filesystem::path& PackagePath,
    const std::filesystem::path& StoreRoot =
        DefaultCharacterProfileStore());

ProfileDiscoveryResult DiscoverInstalledCharacterProfiles(
    const std::filesystem::path& StoreRoot =
        DefaultCharacterProfileStore());

ProfileDeleteResult DeleteInstalledCharacterProfile(
    const InstalledCharacterProfile& Profile,
    const std::filesystem::path& StoreRoot =
        DefaultCharacterProfileStore());

std::filesystem::path InstalledProfileResourcePath(
    const InstalledCharacterProfile& Profile,
    const ProfileResource& Resource);
} // namespace skrtg::viewer::profile
