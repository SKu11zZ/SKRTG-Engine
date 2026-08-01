#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::viewer
{
struct VerifiedExportCopyRequest
{
    std::filesystem::path SourceFbx;
    std::filesystem::path DestinationFbx;
    std::filesystem::path ProtectedPackageDirectory;
    std::string ExpectedSha256;
    bool AllowOverwrite = false;
};

struct VerifiedExportCopyResult
{
    bool Success = false;
    std::filesystem::path DestinationFbx;
    std::vector<std::string> Errors;
};

VerifiedExportCopyResult CopyVerifiedExport(
    const VerifiedExportCopyRequest& Request);

} // namespace skrtg::viewer
