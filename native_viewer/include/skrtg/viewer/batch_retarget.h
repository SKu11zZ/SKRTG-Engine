#pragma once

#include "skrtg/viewer/retarget_bridge.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace skrtg::viewer
{
// Definition files are exported UE JSON inputs. The runtime never reads
// uasset files.
struct BatchCharacterInput
{
    std::filesystem::path RestFbx;
    std::string DefinitionKind = "ue_ik_json_v1";
    std::filesystem::path DefinitionFile;
    std::filesystem::path AlignmentRetargeterFile;
};

struct BatchRetargetRequest
{
    BatchCharacterInput SourceCharacter;
    BatchCharacterInput TargetCharacter;
    std::filesystem::path AnimationDirectory;
    std::filesystem::path OutputDirectory;
    RetargetBridgeTools Tools;
    std::string AnimationStack;
    bool Recursive = true;
    bool EnableSpinePelvisFollow = true;
    bool EnableSourceMotionFootLock = true;
};

enum class BatchRetargetJobState
{
    Pending,
    Running,
    Succeeded,
    Failed
};

struct BatchRetargetJob
{
    std::size_t Index = 0;
    std::filesystem::path SourceAnimationFbx;
    std::filesystem::path RelativeAnimationPath;
    std::filesystem::path JobDirectory;
    std::filesystem::path ReviewPackage;
    std::filesystem::path FinalFbx;
    std::string ClipId;
    std::string ClipLabel;
    std::string SourceAnimationSha256;
    std::string FinalFbxSha256;
    BatchRetargetJobState State = BatchRetargetJobState::Pending;
    double DurationSeconds = 0.0;
    std::vector<std::string> Errors;
};

struct BatchRetargetPlan
{
    bool Success = false;
    std::size_t MaximumConcurrentJobs = 1;
    std::vector<BatchRetargetJob> Jobs;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;
};

struct BatchRetargetStatus
{
    bool Running = false;
    bool Complete = false;
    bool Success = false;
    bool Cancelled = false;
    std::size_t MaximumConcurrentJobs = 1;
    std::size_t TotalJobs = 0;
    std::size_t CompletedJobs = 0;
    std::size_t SucceededJobs = 0;
    std::size_t FailedJobs = 0;
    std::size_t ActiveJobIndex = 0;
    bool HasActiveJob = false;
    double DurationSeconds = 0.0;
    BatchCharacterInput SourceCharacter;
    BatchCharacterInput TargetCharacter;
    std::filesystem::path AnimationDirectory;
    std::filesystem::path OutputDirectory;
    bool Recursive = true;
    std::string AnimationStack;
    bool EnableSpinePelvisFollow = true;
    bool EnableSourceMotionFootLock = true;
    std::vector<BatchRetargetJob> Jobs;
    std::vector<std::string> Errors;
};

struct BatchRetargetRunResult
{
    bool Success = false;
    std::filesystem::path StatusJson;
    BatchRetargetStatus Status;
    std::vector<std::string> Errors;
};

std::filesystem::path DiscoverBatchRetargetExecutable(
    const std::filesystem::path& ViewerExecutable);

BatchRetargetPlan BuildBatchRetargetPlan(
    const BatchRetargetRequest& Request);

bool WriteBatchRetargetRequest(
    const BatchRetargetRequest& Request,
    const std::filesystem::path& OutputJson,
    std::string& OutError);

bool ReadBatchRetargetRequest(
    const std::filesystem::path& InputJson,
    BatchRetargetRequest& OutRequest,
    std::string& OutError);

bool WriteBatchRetargetStatus(
    const BatchRetargetStatus& Status,
    const std::filesystem::path& OutputJson,
    std::string& OutError);

bool ReadBatchRetargetStatus(
    const std::filesystem::path& InputJson,
    BatchRetargetStatus& OutStatus,
    std::string& OutError);

BatchRetargetRunResult RunBatchRetarget(
    const BatchRetargetRequest& Request);

const char* BatchRetargetJobStateName(BatchRetargetJobState State);

} // namespace skrtg::viewer
