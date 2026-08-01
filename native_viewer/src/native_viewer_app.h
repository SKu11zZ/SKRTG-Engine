#pragma once

#include "skrtg/viewer/review_scene.h"

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace skrtg::viewer
{
struct NativeViewerOptions
{
    bool HiddenWindow = false;
    int ExitAfterRenderedFrames = 0;
    std::filesystem::path CapturePpmPath;
    std::filesystem::path ExecutablePath;
    bool OpenInputPickerOnStart = false;
    bool OpenBatchPickerOnStart = false;
    bool StartPlaybackOnLaunch = false;
    bool StartCameraFollowOnLaunch = false;
    std::optional<bool> InitialFootLockDisplayEnabled;
    double FixedDeltaSeconds = 0.0;
    std::size_t InitialClipIndex = 0;
    std::uint64_t InitialFrameIndex = 0;
};

int RunNativeViewer(
    std::optional<ReviewScene> Scene,
    const NativeViewerOptions& Options);

} // namespace skrtg::viewer
