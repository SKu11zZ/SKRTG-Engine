#pragma once

#include "skrtg/retarget/op_stack.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace skrtg::retarget
{
struct RetargetOpProgramEntry
{
    std::string InstanceId;
    std::string TypeId;
    bool Enabled = false;
};

struct RetargetOpProgram
{
    std::string Schema = "skrtg.op_stack.v2";
    bool Candidate = true;
    RetargetOpStackRunOptions RunOptions;
    std::vector<RetargetOpGoalSeed> GoalSeeds;
    std::vector<RetargetOpProgramEntry> Entries;
    RetargetOpStack Stack;
};

struct RetargetOpProgramLoadResult
{
    bool Success = false;
    std::unique_ptr<RetargetOpProgram> Program;
    std::vector<std::string> Warnings;
    std::vector<std::string> Errors;
};

// Loads only explicit names declared by the configuration. Bone names must
// resolve exactly once in the supplied skeleton; no aliases or heuristics are
// used. The configuration is a candidate execution request, not route
// selection or adoption evidence.
RetargetOpProgramLoadResult LoadRetargetOpProgram(
    const std::filesystem::path& ConfigJson,
    const core::skeleton::NormalizedRuntimeSkeleton& SourceSkeleton,
    const core::skeleton::NormalizedRuntimeSkeleton& TargetSkeleton);
} // namespace skrtg::retarget
