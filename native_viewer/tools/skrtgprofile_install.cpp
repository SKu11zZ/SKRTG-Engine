#include "skrtg/viewer/profile/character_profile.h"

#include "cli_platform.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    ConfigureNonInteractiveCli();
    if (argc != 2 && argc != 4)
    {
        std::cerr
            << "usage: skrtgprofile_install"
            << " <character.skrtgprofile>"
            << " [--store <profile_store_directory>]\n";
        return 2;
    }
    std::filesystem::path Store =
        skrtg::viewer::profile::DefaultCharacterProfileStore();
    if (argc == 4)
    {
        if (std::string(argv[2]) != "--store")
        {
            std::cerr
                << "usage: skrtgprofile_install"
                << " <character.skrtgprofile>"
                << " [--store <profile_store_directory>]\n";
            return 2;
        }
        Store = argv[3];
    }
    const auto Result =
        skrtg::viewer::profile::InstallCharacterProfilePackage(
            argv[1], Store);
    if (!Result.Success)
    {
        std::cerr << "SKRTG character profile installation failed\n";
        for (const std::string& Error : Result.Errors)
            std::cerr << Error << '\n';
        return 1;
    }
    std::cout
        << (Result.AlreadyInstalled
            ? "SKRTG character profile already installed\n"
            : "SKRTG character profile installed\n")
        << "profile_id="
        << Result.Installed.Profile.ProfileId << '\n'
        << "profile_version="
        << Result.Installed.Profile.ProfileVersion << '\n'
        << "package_sha256="
        << Result.Installed.PackageSha256 << '\n'
        << "install_directory="
        << Result.Installed.InstallDirectory.string() << '\n';
    return 0;
}
