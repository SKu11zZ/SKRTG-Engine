#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::viewer::skrv
{
inline constexpr std::uintmax_t MaximumManifestBytes =
    16U * 1024U * 1024U;
inline constexpr std::uintmax_t MaximumIntegrityIndexBytes =
    8U * 1024U * 1024U;
inline constexpr std::size_t MaximumIntegrityLineBytes = 4096;
inline constexpr std::size_t MaximumIndexedEntries = 16384;
inline constexpr std::size_t MaximumPackageInventoryEntries = 32768;
inline constexpr std::size_t MaximumPortablePathBytes = 1024;
inline constexpr std::size_t MaximumPortablePathComponentBytes = 255;

enum class EntryRole
{
    Manifest,
    Blob,
    Export,
    Auxiliary
};

struct PackageSourceItem
{
    EntryRole Role = EntryRole::Auxiliary;
    std::filesystem::path RelativePath;
    std::filesystem::path SourcePath;
};

struct PackageWriteRequest
{
    std::filesystem::path OutputDirectory;
    std::vector<PackageSourceItem> Items;
};

struct IntegrityEntry
{
    EntryRole Role = EntryRole::Auxiliary;
    std::filesystem::path RelativePath;
    std::uintmax_t ByteCount = 0;
    std::string Sha256;
};

struct PackageWriteResult
{
    bool Success = false;
    std::filesystem::path OutputDirectory;
    std::vector<IntegrityEntry> Entries;
    std::vector<std::string> Errors;
};

struct PackageInspectOptions
{
    bool VerifyHashes = true;
    bool RejectUnindexedFiles = true;
    // Callers may choose a stricter inventory budget. Values above the v1
    // contract maximum are clamped and can never weaken the reader limit.
    std::size_t InventoryEntryLimit = MaximumPackageInventoryEntries;
};

struct ManifestSummary
{
    int ContractVersion = 0;
    std::uint64_t ClipCount = 0;
    std::uint64_t FrameCount = 0;
    std::uint64_t SourceBoneCount = 0;
    std::uint64_t TargetBoneCount = 0;
    std::uint64_t MappedChainCount = 0;
    std::uint64_t GoalChainCount = 0;
    std::uint64_t ReferencedBlobCount = 0;
    std::uint64_t VerifiedExportCount = 0;
};

struct PackageInspectResult
{
    bool Success = false;
    std::filesystem::path PackageDirectory;
    std::vector<IntegrityEntry> Entries;
    ManifestSummary Manifest;
    std::string ManifestSha256;
    std::string IntegrityIndexSha256;
    std::vector<std::string> Errors;
};

const char* EntryRoleName(EntryRole Role);

bool ParseEntryRole(const std::string& Value, EntryRole& OutRole);

PackageWriteResult WriteDirectoryPackage(
    const PackageWriteRequest& Request);

PackageInspectResult InspectDirectoryPackage(
    const std::filesystem::path& PackageDirectory,
    const PackageInspectOptions& Options = {});
} // namespace skrtg::viewer::skrv
