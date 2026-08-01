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

struct ProfileResource
{
    std::filesystem::path RelativePath;
    std::uint64_t ByteCount = 0;
    std::string Sha256;
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
    bool SourceEnabled = true;
    bool TargetEnabled = true;
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
    bool SourceEnabled = true;
    bool TargetEnabled = true;
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
