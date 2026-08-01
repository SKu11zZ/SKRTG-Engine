#include "skrtg/viewer/startup_paths.h"

#include <array>
#include <system_error>

namespace skrtg::viewer
{
namespace
{
constexpr const char* FormalPackageName =
    "SKRTG_UEIK_Review.skrv";

bool IsReviewPackageDirectory(const std::filesystem::path& Path)
{
    std::error_code Error;
    if (!std::filesystem::is_directory(Path, Error) || Error)
        return false;
    return std::filesystem::is_regular_file(Path / "manifest.json", Error) &&
        !Error &&
        std::filesystem::is_regular_file(Path / "integrity.tsv", Error) &&
        !Error;
}
} // namespace

DefaultPackageSearchResult FindDefaultReviewPackage(
    const std::filesystem::path& ExecutablePath)
{
    DefaultPackageSearchResult Result;
    std::error_code Error;
    std::filesystem::path AbsoluteExecutable =
        std::filesystem::absolute(ExecutablePath, Error);
    if (Error)
        AbsoluteExecutable = ExecutablePath;
    const std::filesystem::path ExecutableDirectory =
        AbsoluteExecutable.parent_path();

    Result.Candidates = {
        ExecutableDirectory / "review.skrv",
        ExecutableDirectory / "data" / "review.skrv",
        ExecutableDirectory / "data" / FormalPackageName,
        ExecutableDirectory / FormalPackageName,
        ExecutableDirectory / ".." / ".." / "Artifacts" /
            "NativeViewer" / "N0_N1_2026-07-19" / FormalPackageName};

    for (const std::filesystem::path& Candidate : Result.Candidates)
    {
        if (!IsReviewPackageDirectory(Candidate))
            continue;
        Result.Found = true;
        Result.PackageDirectory =
            std::filesystem::weakly_canonical(Candidate, Error);
        if (Error)
            Result.PackageDirectory = Candidate.lexically_normal();
        return Result;
    }
    return Result;
}

} // namespace skrtg::viewer
