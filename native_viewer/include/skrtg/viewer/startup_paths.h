#pragma once

#include <filesystem>
#include <vector>

namespace skrtg::viewer
{
struct DefaultPackageSearchResult
{
    bool Found = false;
    std::filesystem::path PackageDirectory;
    std::vector<std::filesystem::path> Candidates;
};

DefaultPackageSearchResult FindDefaultReviewPackage(
    const std::filesystem::path& ExecutablePath);

} // namespace skrtg::viewer
