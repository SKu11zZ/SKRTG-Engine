#include "skrtg/viewer/playback.h"

#include <cmath>
#include <limits>

namespace skrtg::viewer
{
PlaybackAdvanceResult AdvancePlayback(
    PlaybackController& Controller,
    const std::uint64_t CurrentFrame,
    const std::uint64_t FrameCount,
    const double FramesPerSecond,
    const double DeltaSeconds)
{
    PlaybackAdvanceResult Result;
    Result.FrameIndex = CurrentFrame;
    if (!Controller.Playing || FrameCount == 0 ||
        !std::isfinite(FramesPerSecond) || FramesPerSecond <= 0.0 ||
        !std::isfinite(DeltaSeconds) || DeltaSeconds <= 0.0 ||
        !std::isfinite(Controller.Speed) || Controller.Speed <= 0.0)
    {
        return Result;
    }

    Controller.FractionalFrames +=
        DeltaSeconds * FramesPerSecond * Controller.Speed;
    if (!std::isfinite(Controller.FractionalFrames))
    {
        Controller.Playing = false;
        Controller.FractionalFrames = 0.0;
        return Result;
    }
    const double WholeFrames = std::floor(Controller.FractionalFrames);
    if (WholeFrames < 1.0) return Result;
    Controller.FractionalFrames -= WholeFrames;
    const std::uint64_t Steps = WholeFrames >= static_cast<double>(
        std::numeric_limits<std::uint64_t>::max())
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(WholeFrames);

    if (Controller.Loop)
    {
        const std::uint64_t Offset = Steps % FrameCount;
        const std::uint64_t UntilWrap = FrameCount - CurrentFrame;
        Result.FrameIndex = Offset >= UntilWrap
            ? Offset - UntilWrap
            : CurrentFrame + Offset;
        Result.FrameChanged = Result.FrameIndex != CurrentFrame;
        Result.ReachedEnd = Steps >= FrameCount - CurrentFrame;
        return Result;
    }

    const std::uint64_t LastFrame = FrameCount - 1;
    const std::uint64_t Remaining =
        CurrentFrame < LastFrame ? LastFrame - CurrentFrame : 0;
    if (Steps >= Remaining)
    {
        Result.FrameIndex = LastFrame;
        Result.FrameChanged = Result.FrameIndex != CurrentFrame;
        Result.ReachedEnd = true;
        Controller.Playing = false;
        Controller.FractionalFrames = 0.0;
        return Result;
    }
    Result.FrameIndex = CurrentFrame + Steps;
    Result.FrameChanged = true;
    return Result;
}

} // namespace skrtg::viewer
