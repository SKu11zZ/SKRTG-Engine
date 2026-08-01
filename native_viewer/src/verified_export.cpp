#include "skrtg/viewer/verified_export.h"

#include "skrtg/viewer/skrv/sha256.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <system_error>

namespace skrtg::viewer
{
namespace
{
std::atomic<unsigned long long> TemporarySequence{0};

std::string PathText(const std::filesystem::path& Path)
{
    const std::u8string Value = Path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(Value.data()), Value.size());
}

std::filesystem::path AbsoluteNormalized(
    const std::filesystem::path& Path)
{
    std::error_code Error;
    std::filesystem::path Result = std::filesystem::absolute(Path, Error);
    if (Error) Result = Path;
    return Result.lexically_normal();
}

std::filesystem::path WeakCanonicalNormalized(
    const std::filesystem::path& Path)
{
    std::error_code Error;
    const std::filesystem::path Result =
        std::filesystem::weakly_canonical(Path, Error);
    return Error ? AbsoluteNormalized(Path) : Result.lexically_normal();
}

bool ComponentEqual(
    const std::filesystem::path& Left,
    const std::filesystem::path& Right)
{
#if defined(_WIN32)
    std::wstring LeftText = Left.native();
    std::wstring RightText = Right.native();
    std::transform(
        LeftText.begin(), LeftText.end(), LeftText.begin(),
        [](const wchar_t Value)
        {
            return static_cast<wchar_t>(std::towlower(Value));
        });
    std::transform(
        RightText.begin(), RightText.end(), RightText.begin(),
        [](const wchar_t Value)
        {
            return static_cast<wchar_t>(std::towlower(Value));
        });
    return LeftText == RightText;
#else
    return Left == Right;
#endif
}

bool SameOrDescendantNormalized(
    const std::filesystem::path& Candidate,
    const std::filesystem::path& Directory)
{
    auto CandidatePart = Candidate.begin();
    for (auto DirectoryPart = Directory.begin();
         DirectoryPart != Directory.end();
         ++DirectoryPart, ++CandidatePart)
    {
        if (CandidatePart == Candidate.end() ||
            !ComponentEqual(*CandidatePart, *DirectoryPart))
        {
            return false;
        }
    }
    return true;
}

bool IsSameOrDescendant(
    const std::filesystem::path& Candidate,
    const std::filesystem::path& Directory)
{
    if (Directory.empty()) return false;
    return SameOrDescendantNormalized(
               AbsoluteNormalized(Candidate),
               AbsoluteNormalized(Directory)) ||
        SameOrDescendantNormalized(
               WeakCanonicalNormalized(Candidate),
               WeakCanonicalNormalized(Directory));
}

bool IsFbx(const std::filesystem::path& Path)
{
    std::string Extension = Path.extension().string();
    std::transform(
        Extension.begin(), Extension.end(), Extension.begin(),
        [](const unsigned char Value)
        {
            return static_cast<char>(std::tolower(Value));
        });
    return Extension == ".fbx";
}

std::filesystem::path UniqueSibling(
    const std::filesystem::path& Destination,
    const char* Role)
{
    for (int Attempt = 0; Attempt < 1024; ++Attempt)
    {
        const auto Sequence = TemporarySequence.fetch_add(1);
        std::filesystem::path TemporaryName = Destination.filename();
        TemporaryName += std::filesystem::path(
            ".skrtg_" + std::string(Role) + "_" +
            std::to_string(Sequence));
        const std::filesystem::path Candidate =
            Destination.parent_path() / TemporaryName;
        std::error_code Error;
        if (!std::filesystem::exists(Candidate, Error) && !Error)
            return Candidate;
    }
    return {};
}

void RemoveIfPresent(const std::filesystem::path& Path)
{
    if (Path.empty()) return;
    std::error_code Ignored;
    std::filesystem::remove(Path, Ignored);
}
} // namespace

VerifiedExportCopyResult CopyVerifiedExport(
    const VerifiedExportCopyRequest& Request)
{
    VerifiedExportCopyResult Result;
    Result.DestinationFbx = Request.DestinationFbx;
    std::error_code Error;
    if (!std::filesystem::is_regular_file(Request.SourceFbx, Error) || Error)
    {
        Result.Errors.emplace_back("verified export source is not readable");
        return Result;
    }
    if (Request.DestinationFbx.empty() ||
        Request.DestinationFbx.filename().empty() ||
        !IsFbx(Request.DestinationFbx))
    {
        Result.Errors.emplace_back("export destination must be an FBX file");
        return Result;
    }
    if (IsSameOrDescendant(
            Request.DestinationFbx,
            Request.ProtectedPackageDirectory))
    {
        Result.Errors.emplace_back(
            "export destination must be outside the active SKRV package");
        return Result;
    }

    std::string SourceHash;
    std::string HashError;
    if (!skrv::Sha256File(Request.SourceFbx, SourceHash, HashError) ||
        SourceHash != Request.ExpectedSha256)
    {
        Result.Errors.emplace_back(
            "verified export source SHA-256 no longer matches the manifest");
        return Result;
    }

    const std::filesystem::path Parent =
        Request.DestinationFbx.parent_path();
    if (Parent.empty())
    {
        Result.Errors.emplace_back("export destination directory is empty");
        return Result;
    }
    std::filesystem::create_directories(Parent, Error);
    if (Error)
    {
        Result.Errors.emplace_back(
            "unable to create export directory: " + Error.message());
        return Result;
    }

    bool DestinationExists =
        std::filesystem::exists(Request.DestinationFbx, Error);
    if (Error)
    {
        Result.Errors.emplace_back(
            "unable to inspect export destination: " + Error.message());
        return Result;
    }
    if (DestinationExists && !Request.AllowOverwrite)
    {
        Result.Errors.emplace_back(
            "export destination exists and overwrite is disabled");
        return Result;
    }
    if (DestinationExists &&
        !std::filesystem::is_regular_file(Request.DestinationFbx, Error))
    {
        Result.Errors.emplace_back(
            "export destination exists but is not a regular file");
        return Result;
    }

    const std::filesystem::path Temporary =
        UniqueSibling(Request.DestinationFbx, "export_tmp");
    if (Temporary.empty())
    {
        Result.Errors.emplace_back(
            "unable to reserve a temporary export path");
        return Result;
    }
    std::filesystem::copy_file(
        Request.SourceFbx, Temporary,
        std::filesystem::copy_options::none, Error);
    if (Error)
    {
        RemoveIfPresent(Temporary);
        Result.Errors.emplace_back(
            "unable to stage verified export: " + Error.message());
        return Result;
    }
    std::string StagedHash;
    if (!skrv::Sha256File(Temporary, StagedHash, HashError) ||
        StagedHash != Request.ExpectedSha256)
    {
        RemoveIfPresent(Temporary);
        Result.Errors.emplace_back(
            "staged export SHA-256 verification failed");
        return Result;
    }

    Error.clear();
    DestinationExists =
        std::filesystem::exists(Request.DestinationFbx, Error);
    if (Error || (DestinationExists && !Request.AllowOverwrite))
    {
        RemoveIfPresent(Temporary);
        Result.Errors.emplace_back(Error
            ? "unable to recheck export destination: " + Error.message()
            : "export destination appeared while overwrite was disabled");
        return Result;
    }

    std::filesystem::path Backup;
    if (DestinationExists)
    {
        Backup = UniqueSibling(Request.DestinationFbx, "export_backup");
        if (Backup.empty())
        {
            RemoveIfPresent(Temporary);
            Result.Errors.emplace_back(
                "unable to reserve an export rollback path");
            return Result;
        }
        std::filesystem::rename(Request.DestinationFbx, Backup, Error);
        if (Error)
        {
            RemoveIfPresent(Temporary);
            Result.Errors.emplace_back(
                "unable to stage existing export for rollback: " +
                Error.message());
            return Result;
        }
    }

    Error.clear();
    std::filesystem::rename(Temporary, Request.DestinationFbx, Error);
    if (Error)
    {
        RemoveIfPresent(Temporary);
        if (!Backup.empty())
        {
            std::error_code RestoreError;
            std::filesystem::rename(
                Backup, Request.DestinationFbx, RestoreError);
            if (RestoreError)
            {
                Result.Errors.emplace_back(
                    "export commit failed and rollback is at: " +
                    PathText(Backup));
                return Result;
            }
        }
        Result.Errors.emplace_back(
            "unable to commit verified export: " + Error.message());
        return Result;
    }

    std::string DestinationHash;
    if (!skrv::Sha256File(
            Request.DestinationFbx, DestinationHash, HashError) ||
        DestinationHash != Request.ExpectedSha256)
    {
        RemoveIfPresent(Request.DestinationFbx);
        if (!Backup.empty())
        {
            std::error_code RestoreError;
            std::filesystem::rename(
                Backup, Request.DestinationFbx, RestoreError);
            if (RestoreError)
            {
                Result.Errors.emplace_back(
                    "destination verification failed and rollback is at: " +
                    PathText(Backup));
                return Result;
            }
        }
        Result.Errors.emplace_back(
            "committed export SHA-256 verification failed; destination was rolled back");
        return Result;
    }

    if (!Backup.empty())
    {
        std::filesystem::remove(Backup, Error);
        if (Error)
        {
            Result.Errors.emplace_back(
                "export is valid but rollback cleanup failed at: " +
                PathText(Backup));
            return Result;
        }
    }
    Result.Success = true;
    return Result;
}

} // namespace skrtg::viewer
