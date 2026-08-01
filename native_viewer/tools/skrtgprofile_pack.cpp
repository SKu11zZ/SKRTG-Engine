#include "skrtg/viewer/profile/character_profile.h"

#include "cli_platform.h"

#include <iostream>
#include <optional>
#include <string>

namespace
{
void Usage()
{
    std::cerr
        << "usage: skrtgprofile_pack"
        << " --id <profile_id>"
        << " --version <semver>"
        << " --label <display_name>"
        << " [--canonical <profile_id>]"
        << " --rest <character_rest.fbx>"
        << " --rig <ik_rig.json>"
        << " --alignment <canonical_to_character.ikretargeter.json>"
        << " --out <character.skrtgprofile>"
        << " [--source-disabled] [--target-disabled]\n";
}
} // namespace

int main(int argc, char** argv)
{
    ConfigureNonInteractiveCli();
    skrtg::viewer::profile::ProfilePackRequest Request;
    auto Value = [&](int& Index) -> std::optional<std::string>
    {
        if (Index + 1 >= argc) return std::nullopt;
        return std::string(argv[++Index]);
    };
    for (int Index = 1; Index < argc; ++Index)
    {
        const std::string Argument = argv[Index];
        if (Argument == "--source-disabled")
        {
            Request.SourceEnabled = false;
            continue;
        }
        if (Argument == "--target-disabled")
        {
            Request.TargetEnabled = false;
            continue;
        }
        const std::optional<std::string> Next = Value(Index);
        if (!Next.has_value())
        {
            Usage();
            return 2;
        }
        if (Argument == "--id")
            Request.ProfileId = *Next;
        else if (Argument == "--version")
            Request.ProfileVersion = *Next;
        else if (Argument == "--label")
            Request.DisplayName = *Next;
        else if (Argument == "--canonical")
            Request.CanonicalProfileId = *Next;
        else if (Argument == "--rest")
            Request.RestFbx = *Next;
        else if (Argument == "--rig")
            Request.IkRigJson = *Next;
        else if (Argument == "--alignment")
            Request.AlignmentRetargeterJson = *Next;
        else if (Argument == "--out")
            Request.OutputPackage = *Next;
        else
        {
            Usage();
            return 2;
        }
    }
    const auto Result =
        skrtg::viewer::profile::WriteCharacterProfilePackage(Request);
    if (!Result.Success)
    {
        std::cerr << "SKRTG character profile packaging failed\n";
        for (const std::string& Error : Result.Errors)
            std::cerr << Error << '\n';
        return 1;
    }
    std::cout
        << "SKRTG character profile committed\n"
        << "path=" << Result.PackagePath.string() << '\n'
        << "profile_id=" << Result.Profile.ProfileId << '\n'
        << "profile_version=" << Result.Profile.ProfileVersion << '\n'
        << "skeleton_signature_sha256="
        << Result.Profile.SkeletonSignatureSha256 << '\n'
        << "package_sha256=" << Result.PackageSha256 << '\n'
        << "entry_count=" << Result.Entries.size() << '\n';
    return 0;
}
