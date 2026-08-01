#include "skrtg/viewer/skrv/package.h"

#include "cli_platform.h"

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    ConfigureNonInteractiveCli();
    if (argc < 2 || argc > 3 ||
        (argc == 3 && std::string(argv[2]) != "--no-hash"))
    {
        std::cerr << "usage: skrv_inspect <package.skrv> [--no-hash]\n";
        return 2;
    }
    skrtg::viewer::skrv::PackageInspectOptions Options;
    Options.VerifyHashes = argc != 3;
    const auto Result = skrtg::viewer::skrv::InspectDirectoryPackage(
        argv[1], Options);
    if (!Result.Success)
    {
        std::cerr << "SKRV verification failed\n";
        for (const std::string& Error : Result.Errors)
            std::cerr << Error << '\n';
        return 1;
    }
    std::uintmax_t Bytes = 0;
    for (const auto& Entry : Result.Entries) Bytes += Entry.ByteCount;
    std::cout << "SKRV verification passed\n"
              << "status=pass\n"
              << "package_verified=true\n"
              << "manifest_semantics_verified=true\n"
              << "path=" << Result.PackageDirectory.string() << '\n'
              << "indexed_files=" << Result.Entries.size() << '\n'
              << "indexed_bytes=" << Bytes << '\n'
              << "hashes_verified=" << (Options.VerifyHashes ? "true" : "false") << '\n'
              << "manifest_sha256=" << Result.ManifestSha256 << '\n'
              << "integrity_index_sha256=" << Result.IntegrityIndexSha256 << '\n'
              << "contract_version=" << Result.Manifest.ContractVersion << '\n'
              << "clip_count=" << Result.Manifest.ClipCount << '\n'
              << "frame_count=" << Result.Manifest.FrameCount << '\n'
              << "source_bone_count=" << Result.Manifest.SourceBoneCount << '\n'
              << "target_bone_count=" << Result.Manifest.TargetBoneCount << '\n'
              << "mapped_chain_count=" << Result.Manifest.MappedChainCount << '\n'
              << "goal_chain_count=" << Result.Manifest.GoalChainCount << '\n'
              << "referenced_blob_count=" << Result.Manifest.ReferencedBlobCount << '\n'
              << "verified_export_count=" << Result.Manifest.VerifiedExportCount << '\n';
    return 0;
}
