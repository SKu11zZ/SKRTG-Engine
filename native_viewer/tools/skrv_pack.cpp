#include "skrtg/viewer/skrv/package.h"

#include "cli_platform.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using skrtg::viewer::skrv::EntryRole;

EntryRole RoleFor(const std::filesystem::path& Relative)
{
    const std::filesystem::path Generic = Relative.lexically_normal();
    if (Generic == "manifest.json") return EntryRole::Manifest;
    const auto First = Generic.begin();
    if (First != Generic.end() && *First == "blobs") return EntryRole::Blob;
    if (First != Generic.end() && *First == "exports") return EntryRole::Export;
    return EntryRole::Auxiliary;
}
} // namespace

int main(int argc, char** argv)
{
    ConfigureNonInteractiveCli();
    if (argc != 3)
    {
        std::cerr << "usage: skrv_pack <payload-directory> <output.skrv>\n";
        return 2;
    }

    std::error_code Error;
    const std::filesystem::path Payload =
        std::filesystem::absolute(argv[1], Error).lexically_normal();
    if (Error || !std::filesystem::is_directory(Payload, Error) || Error)
    {
        std::cerr << "payload directory does not exist\n";
        return 1;
    }

    skrtg::viewer::skrv::PackageWriteRequest Request;
    Request.OutputDirectory = argv[2];
    std::filesystem::recursive_directory_iterator Iterator(
        Payload, std::filesystem::directory_options::none, Error);
    const std::filesystem::recursive_directory_iterator End;
    if (Error)
    {
        std::cerr << "failed to begin payload inventory\n";
        return 1;
    }
    while (Iterator != End)
    {
        const std::filesystem::directory_entry Entry = *Iterator;
        const std::filesystem::file_status Status =
            Entry.symlink_status(Error);
        if (Error || std::filesystem::is_symlink(Status))
        {
            std::cerr << "payload contains a symlink or unreadable entry\n";
            return 1;
        }
        if (std::filesystem::is_regular_file(Status))
        {
            const std::filesystem::path Relative =
                std::filesystem::relative(Entry.path(), Payload, Error);
            if (Error)
            {
                std::cerr << "failed to inventory payload\n";
                return 1;
            }
            Request.Items.push_back({RoleFor(Relative), Relative, Entry.path()});
        }
        Iterator.increment(Error);
        if (Error)
        {
            std::cerr << "failed while inventorying payload\n";
            return 1;
        }
    }

    const skrtg::viewer::skrv::PackageWriteResult Result =
        skrtg::viewer::skrv::WriteDirectoryPackage(Request);
    if (!Result.Success)
    {
        for (const std::string& Message : Result.Errors)
            std::cerr << Message << '\n';
        return 1;
    }
    std::uintmax_t Bytes = 0;
    for (const auto& Entry : Result.Entries) Bytes += Entry.ByteCount;
    std::cout << "SKRV package committed\n"
              << "path=" << Result.OutputDirectory.string() << '\n'
              << "indexed_files=" << Result.Entries.size() << '\n'
              << "indexed_bytes=" << Bytes << '\n';
    return 0;
}
