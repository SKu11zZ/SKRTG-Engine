#include "skrtg/viewer/review_scene.h"
#include "skrtg/viewer/startup_paths.h"

#include "native_viewer_app.h"

#include "cli_platform.h"

#include <filesystem>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace
{
std::filesystem::path CurrentExecutablePath(const char* Argv0)
{
#if defined(_WIN32)
    std::wstring Buffer(32768, L'\0');
    const DWORD Length = GetModuleFileNameW(
        nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
    if (Length > 0 && Length < Buffer.size())
    {
        Buffer.resize(Length);
        return std::filesystem::path(Buffer);
    }
#endif
    return std::filesystem::absolute(
        Argv0 != nullptr ? std::filesystem::path(Argv0)
                         : std::filesystem::path("skrtg_viewer"));
}

bool ResolveDefaultPackage(
    const std::filesystem::path& ExecutablePath,
    std::filesystem::path& OutPackage,
    const bool ReportFailure = true)
{
    const skrtg::viewer::DefaultPackageSearchResult Search =
        skrtg::viewer::FindDefaultReviewPackage(ExecutablePath);
    if (Search.Found)
    {
        OutPackage = Search.PackageDirectory;
        std::cout << "default_package="
                  << OutPackage.generic_string() << '\n';
        return true;
    }
    if (ReportFailure)
    {
        std::cerr
            << "SKRTG Viewer could not find a default review package.\n"
            << "Place review.skrv beside the executable or in its data folder.\n"
            << "Searched:\n";
        for (const std::filesystem::path& Candidate : Search.Candidates)
            std::cerr << "  " << Candidate.generic_string() << '\n';
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    ConfigureNonInteractiveCli();
    skrtg::viewer::NativeViewerOptions Options;
    Options.ExecutablePath = CurrentExecutablePath(
        argc > 0 ? argv[0] : nullptr);
    std::filesystem::path PackagePath;
    const std::filesystem::path& ExecutablePath = Options.ExecutablePath;
    if (argc == 1)
    {
        if (!ResolveDefaultPackage(ExecutablePath, PackagePath, false))
            Options.OpenInputPickerOnStart = true;
    }
    else if (argc == 2 && std::string(argv[1]) == "--input-picker")
    {
        Options.OpenInputPickerOnStart = true;
    }
    else if (argc == 2 && std::string(argv[1]) == "--batch-picker")
    {
        Options.OpenBatchPickerOnStart = true;
    }
    else if ((argc == 2 || argc == 3) &&
             std::string(argv[1]) == "--headless-batch-picker-smoke")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 3;
        Options.OpenBatchPickerOnStart = true;
        if (argc == 3)
            Options.CapturePpmPath = argv[2];
    }
    else if ((argc == 2 || argc == 3) &&
             std::string(argv[1]) == "--headless-input-picker-smoke")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 3;
        Options.OpenInputPickerOnStart = true;
        if (argc == 3)
            Options.CapturePpmPath = argv[2];
    }
    else if ((argc == 2 || argc == 3) &&
             std::string(argv[1]) == "--headless-smoke-default")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 3;
        if (!ResolveDefaultPackage(ExecutablePath, PackagePath))
            return 2;
        if (argc == 3)
            Options.CapturePpmPath = argv[2];
    }
    else if ((argc == 3 || argc == 4) &&
             std::string(argv[1]) == "--headless-playback-smoke")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 4;
        Options.StartPlaybackOnLaunch = true;
        Options.FixedDeltaSeconds = 1.0 / 30.0;
        PackagePath = argv[2];
        if (argc == 4)
            Options.CapturePpmPath = argv[3];
    }
    else if ((argc == 4 || argc == 5) &&
             std::string(argv[1]) ==
                "--headless-visualization-smoke")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 3;
        PackagePath = argv[2];
        try
        {
            Options.InitialFrameIndex = std::stoull(argv[3]);
        }
        catch (const std::exception&)
        {
            std::cerr << "visualization smoke frame must be uint64\n";
            return 2;
        }
        if (argc == 5)
            Options.CapturePpmPath = argv[4];
    }
    else if ((argc == 3 || argc == 4) &&
             std::string(argv[1]) ==
                "--headless-camera-follow-smoke")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 4;
        Options.StartPlaybackOnLaunch = true;
        Options.StartCameraFollowOnLaunch = true;
        Options.FixedDeltaSeconds = 1.0 / 30.0;
        PackagePath = argv[2];
        if (argc == 4)
            Options.CapturePpmPath = argv[3];
    }
    else if ((argc == 5 || argc == 6) &&
             std::string(argv[1]) ==
                "--headless-clip-visualization-smoke")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 3;
        PackagePath = argv[2];
        try
        {
            Options.InitialClipIndex = static_cast<std::size_t>(
                std::stoull(argv[3]));
            Options.InitialFrameIndex = std::stoull(argv[4]);
        }
        catch (const std::exception&)
        {
            std::cerr
                << "clip visualization indices must be uint64\n";
            return 2;
        }
        if (argc == 6)
            Options.CapturePpmPath = argv[5];
    }
    else if ((argc == 6 || argc == 7) &&
             std::string(argv[1]) ==
                "--headless-foot-lock-ab-smoke")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 3;
        PackagePath = argv[2];
        try
        {
            Options.InitialClipIndex = static_cast<std::size_t>(
                std::stoull(argv[3]));
            Options.InitialFrameIndex = std::stoull(argv[4]);
        }
        catch (const std::exception&)
        {
            std::cerr << "Foot Lock A/B indices must be uint64\n";
            return 2;
        }
        const std::string State = argv[5];
        if (State == "on")
            Options.InitialFootLockDisplayEnabled = true;
        else if (State == "off")
            Options.InitialFootLockDisplayEnabled = false;
        else
        {
            std::cerr << "Foot Lock A/B state must be on or off\n";
            return 2;
        }
        if (argc == 7)
            Options.CapturePpmPath = argv[6];
    }
    else if (argc == 2)
    {
        PackagePath = argv[1];
    }
    else if ((argc == 3 || argc == 4) &&
             std::string(argv[1]) == "--headless-smoke")
    {
        Options.HiddenWindow = true;
        Options.ExitAfterRenderedFrames = 3;
        PackagePath = argv[2];
        if (argc == 4)
            Options.CapturePpmPath = argv[3];
    }
    else
    {
        std::cerr
            << "usage: skrtg_viewer <review.skrv>\n"
            << "       skrtg_viewer --input-picker\n"
            << "       skrtg_viewer --batch-picker\n"
            << "       skrtg_viewer --headless-smoke <review.skrv> [capture.ppm]\n"
            << "       skrtg_viewer --headless-playback-smoke <review.skrv> [capture.ppm]\n"
            << "       skrtg_viewer --headless-visualization-smoke <review.skrv> <frame> [capture.ppm]\n"
            << "       skrtg_viewer --headless-camera-follow-smoke <review.skrv> [capture.ppm]\n"
            << "       skrtg_viewer --headless-clip-visualization-smoke <review.skrv> <clip> <frame> [capture.ppm]\n"
            << "       skrtg_viewer --headless-foot-lock-ab-smoke <review.skrv> <clip> <frame> <on|off> [capture.ppm]\n"
            << "       skrtg_viewer --headless-smoke-default [capture.ppm]\n"
            << "       skrtg_viewer --headless-input-picker-smoke [capture.ppm]\n"
            << "       skrtg_viewer --headless-batch-picker-smoke [capture.ppm]\n"
            << "       skrtg_viewer                 (auto-find review.skrv, otherwise open picker)\n";
        return 2;
    }

    std::optional<skrtg::viewer::ReviewScene> Scene;
    if (!PackagePath.empty())
    {
        const skrtg::viewer::ReviewSceneLoadResult Result =
            skrtg::viewer::LoadReviewScene(
                PackagePath, Options.InitialClipIndex,
                Options.InitialFrameIndex);
        if (!Result.Success)
        {
            std::cerr << "SKRTG Viewer refused an invalid SKRV v1 package\n";
            for (const std::string& Error : Result.Errors)
                std::cerr << Error << '\n';
            return 1;
        }
        Scene = Result.Scene;
    }
    return skrtg::viewer::RunNativeViewer(std::move(Scene), Options);
}
