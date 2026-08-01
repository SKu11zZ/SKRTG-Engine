#pragma once

#include <cstdint>

namespace skrtg::viewer
{
struct PlaybackController
{
    bool Playing = false;
    bool Loop = true;
    double Speed = 1.0;
    double FractionalFrames = 0.0;
};

struct PlaybackAdvanceResult
{
    std::uint64_t FrameIndex = 0;
    bool FrameChanged = false;
    bool ReachedEnd = false;
};

PlaybackAdvanceResult AdvancePlayback(
    PlaybackController& Controller,
    std::uint64_t CurrentFrame,
    std::uint64_t FrameCount,
    double FramesPerSecond,
    double DeltaSeconds);

} // namespace skrtg::viewer
