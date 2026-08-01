#include "skrtg/viewer/profile/character_profile.h"

#include "cli_platform.h"

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    ConfigureNonInteractiveCli();
    if (argc != 2)
    {
        std::cerr
            << "usage: skrtgprofile_inspect <character.skrtgprofile>\n";
        return 2;
    }
    const auto Result =
        skrtg::viewer::profile::InspectCharacterProfilePackage(argv[1]);
    if (!Result.Success)
    {
        std::cerr << "SKRTG character profile verification failed\n";
        for (const std::string& Error : Result.Errors)
            std::cerr << Error << '\n';
        return 1;
    }
    std::uint64_t Bytes = 0;
    for (const auto& Entry : Result.Entries)
        Bytes += Entry.ByteCount;
    std::cout
        << "SKRTG character profile verification passed\n"
        << "status=pass\n"
        << "path=" << Result.PackagePath.string() << '\n'
        << "profile_id=" << Result.Profile.ProfileId << '\n'
        << "profile_version=" << Result.Profile.ProfileVersion << '\n'
        << "display_name=" << Result.Profile.DisplayName << '\n'
        << "canonical_profile_id="
        << Result.Profile.CanonicalProfileId << '\n'
        << "definition_kind=" << Result.Profile.DefinitionKind << '\n'
        << "source_enabled="
        << (Result.Profile.SourceEnabled ? "true" : "false") << '\n'
        << "target_enabled="
        << (Result.Profile.TargetEnabled ? "true" : "false") << '\n'
        << "skeleton_signature_sha256="
        << Result.Profile.SkeletonSignatureSha256 << '\n'
        << "package_sha256=" << Result.PackageSha256 << '\n'
        << "entry_count=" << Result.Entries.size() << '\n'
        << "payload_bytes=" << Bytes << '\n';
    for (const auto& Entry : Result.Entries)
    {
        std::cout
            << "entry=" << Entry.RelativePath.generic_string()
            << '\t' << Entry.ByteCount
            << '\t' << Entry.Sha256 << '\n';
    }
    return 0;
}
