#include "cli.h"

#include "skrtg/viewer/batch_retarget.h"
#include "skrtg/viewer/profile/character_definition.h"
#include "skrtg/viewer/profile/character_profile.h"
#include "skrtg/viewer/retarget_bridge.h"
#include "skrtg/viewer/skrv/package.h"

#include "cli_platform.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef SKRTG_CLI_VERSION
#define SKRTG_CLI_VERSION "0.0.0-dev"
#endif

namespace
{
using Json = nlohmann::json;
namespace Profile = skrtg::viewer::profile;
namespace Skrv = skrtg::viewer::skrv;
using namespace skrtg::viewer;

constexpr int ExitSuccess = 0;
constexpr int ExitFailure = 1;
constexpr int ExitUsage = 2;
constexpr int ExitUnavailable = 3;

std::string PathUtf8(const std::filesystem::path& Path)
{
    return PathToUtf8(Path);
}

struct Outcome
{
    bool Ok = false;
    int ExitCode = ExitFailure;
    std::string Command;
    Json Data = Json::object();
    std::vector<std::string> Warnings;
    std::string ErrorCode;
    std::string Stage;
    std::string Message;
    std::vector<std::string> Details;
    std::vector<std::string> SuggestedActions;
    Json Context = Json::object();
};

Outcome Success(
    std::string Command,
    Json Data = Json::object(),
    std::vector<std::string> Warnings = {})
{
    Outcome Result;
    Result.Ok = true;
    Result.ExitCode = ExitSuccess;
    Result.Command = std::move(Command);
    Result.Data = std::move(Data);
    Result.Warnings = std::move(Warnings);
    return Result;
}

Outcome Failure(
    std::string Command,
    std::string Code,
    std::string Stage,
    std::string Message,
    std::vector<std::string> Details = {},
    std::vector<std::string> SuggestedActions = {},
    const int ExitCode = ExitFailure,
    Json Context = Json::object())
{
    Outcome Result;
    Result.Ok = false;
    Result.ExitCode = ExitCode;
    Result.Command = std::move(Command);
    Result.ErrorCode = std::move(Code);
    Result.Stage = std::move(Stage);
    Result.Message = std::move(Message);
    Result.Details = std::move(Details);
    Result.SuggestedActions = std::move(SuggestedActions);
    Result.Context = std::move(Context);
    return Result;
}

Json DefinitionJson(const Profile::CharacterDefinitionSummary& Value)
{
    return {
        {"sourceFormat", Value.SourceFormat},
        {"sourceSchema", Value.SourceSchema},
        {"adapter", Value.AdapterId},
        {"adapterVersion", Value.AdapterVersion},
        {"inputSha256", Value.InputSha256},
        {"characterId", Value.CharacterId},
        {"displayName", Value.DisplayName},
        {"rigAssetName", Value.RigAssetName},
        {"skeletonSignatureSha256", Value.SkeletonSignatureSha256},
        {"skeletonSignatureKind", Value.SkeletonSignatureKind},
        {"retargetRootBone", Value.RetargetRootBone},
        {"retargetPelvisBone", Value.RetargetPelvisBone},
        {"restPoseKind", Value.RestPoseKind},
        {"boneCount", Value.BoneCount},
        {"chainCount", Value.ChainCount},
        {"runtimeDefinitionComplete", Value.RuntimeDefinitionComplete},
        {"missingRequirements", Value.MissingRequirements}};
}

Json ProfileJson(const Profile::CharacterProfileDescriptor& Value)
{
    Json Result = {
        {"id", Value.ProfileId},
        {"version", Value.ProfileVersion},
        {"displayName", Value.DisplayName},
        {"canonicalProfileId", Value.CanonicalProfileId},
        {"definitionKind", Value.DefinitionKind},
        {"skeletonSignatureSha256", Value.SkeletonSignatureSha256},
        {"unrealEngineVersion", Value.UnrealEngineVersion},
        {"retargetRootBone", Value.RetargetRootBone},
        {"retargetPelvisBone", Value.RetargetPelvisBone},
        {"sourceEnabled", Value.SourceEnabled},
        {"targetEnabled", Value.TargetEnabled}};
    if (!Value.SourceDefinitionFormat.empty())
    {
        Result["authoring"] = {
            {"sourceFormat", Value.SourceDefinitionFormat},
            {"sourceSha256", Value.SourceDefinitionSha256},
            {"importer", Value.DefinitionImporter},
            {"importerVersion", Value.DefinitionImporterVersion},
            {"restPoseKind", Value.RestPoseKind}};
    }
    return Result;
}

Json ProfileEntriesJson(
    const std::vector<Profile::ProfilePackageEntry>& Entries)
{
    Json Result = Json::array();
    for (const auto& Entry : Entries)
    {
        Result.push_back({
            {"path", PathUtf8(Entry.RelativePath)},
            {"byteCount", Entry.ByteCount},
            {"sha256", Entry.Sha256}});
    }
    return Result;
}

Json BatchJobJson(const BatchRetargetJob& Job)
{
    return {
        {"index", Job.Index},
        {"state", BatchRetargetJobStateName(Job.State)},
        {"clipId", Job.ClipId},
        {"clipLabel", Job.ClipLabel},
        {"sourceAnimationId", Job.SourceAnimationId},
        {"sourceAnimationSkeletonId", Job.SourceAnimationSkeletonId},
        {"sourceAnimationFbx", PathUtf8(Job.SourceAnimationFbx)},
        {"sourceAnimationSha256", Job.SourceAnimationSha256},
        {"jobDirectory", PathUtf8(Job.JobDirectory)},
        {"reviewPackage", PathUtf8(Job.ReviewPackage)},
        {"finalFbx", PathUtf8(Job.FinalFbx)},
        {"finalFbxSha256", Job.FinalFbxSha256},
        {"durationSeconds", Job.DurationSeconds},
        {"errors", Job.Errors}};
}

Json BatchPlanJson(const BatchRetargetPlan& Plan)
{
    Json Jobs = Json::array();
    for (const BatchRetargetJob& Job : Plan.Jobs)
        Jobs.push_back(BatchJobJson(Job));
    return {
        {"valid", Plan.Success},
        {"maximumConcurrentJobs", Plan.MaximumConcurrentJobs},
        {"jobCount", Plan.Jobs.size()},
        {"jobs", Jobs}};
}

Json BridgePreflightJson(const RetargetBridgePreflight& Value)
{
    return {
        {"valid", Value.Success},
        {"assetCatalogSha256", Value.AssetCatalogSha256},
        {"sourceProfilePackageSha256", Value.SourceProfilePackageSha256},
        {"targetProfilePackageSha256", Value.TargetProfilePackageSha256},
        {"sourceAnimationSha256", Value.SourceAnimationSha256},
        {"sourceRestSha256", Value.SourceRestSha256},
        {"targetSkeletonSha256", Value.TargetSkeletonSha256},
        {"canonicalSha256", Value.CanonicalSha256},
        {"sourceRigJsonSha256", Value.SourceRigJsonSha256},
        {"targetRigJsonSha256", Value.TargetRigJsonSha256},
        {"sourceAlignmentRetargeterJsonSha256",
         Value.SourceAlignmentRetargeterJsonSha256},
        {"targetAlignmentRetargeterJsonSha256",
         Value.TargetAlignmentRetargeterJsonSha256},
        {"sourceAnimationGoldenJsonSha256",
         Value.SourceAnimationGoldenJsonSha256}};
}

std::optional<std::string> TakeOption(
    std::vector<std::string>& Arguments,
    const std::string& Name,
    std::string& OutError)
{
    for (std::size_t Index = 0; Index < Arguments.size(); ++Index)
    {
        if (Arguments[Index] != Name) continue;
        if (Index + 1 >= Arguments.size())
        {
            OutError = "missing value for " + Name;
            return std::nullopt;
        }
        const std::string Value = Arguments[Index + 1];
        Arguments.erase(
            Arguments.begin() + static_cast<std::ptrdiff_t>(Index),
            Arguments.begin() + static_cast<std::ptrdiff_t>(Index + 2));
        return Value;
    }
    return std::nullopt;
}

bool TakeFlag(
    std::vector<std::string>& Arguments,
    const std::string& Name)
{
    const auto It = std::find(Arguments.begin(), Arguments.end(), Name);
    if (It == Arguments.end()) return false;
    Arguments.erase(It);
    return true;
}

bool IsHelpRequest(const std::vector<std::string>& Arguments)
{
    return Arguments.size() == 1 &&
        (Arguments[0] == "help" || Arguments[0] == "--help" ||
         Arguments[0] == "-h");
}

Outcome UsageFailure(
    const std::string& Command,
    const std::string& Message)
{
    return Failure(
        Command, "USAGE_ERROR", "arguments", Message, {},
        {"Run `skrtg help` or `skrtg <group> help`."},
        ExitUsage);
}

Outcome CommandHelp(
    const std::string& Command,
    const std::string& Usage)
{
    return Success(Command + " help", {{"usage", Usage}});
}

Json Capabilities()
{
    return {
        {"cliVersion", SKRTG_CLI_VERSION},
        {"machineResultSchema", "skrtg.cli.result.v1"},
        {"machineErrorSchema", "skrtg.cli.error.v1"},
        {"traceSchema", "skrtg.cli.trace_event.v1"},
        {"profilePackage", ".skrtgprofile v1"},
        {"characterDefinitionSchema", Profile::CharacterDefinitionSchema},
        {"profileCreateRequestSchema", Profile::ProfileCreateRequestSchema},
        {"definitionAdapters",
         Profile::SupportedCharacterDefinitionAdapters()},
        {"commands",
         Json::array({
             "version", "capabilities", "doctor",
             "profile adapters", "profile probe", "profile normalize",
             "profile create", "profile inspect", "profile install",
             "profile list", "batch validate", "batch run",
             "bridge validate", "bridge run", "skrv inspect",
             "debug profile", "debug batch", "debug bridge"})},
        {"agentContract",
         {{"jsonStdoutOnly", true},
          {"diagnosticsOnStderr", true},
          {"stableExitCodes",
           {{"success", ExitSuccess},
            {"validationOrExecutionFailure", ExitFailure},
            {"usageOrSchemaError", ExitUsage},
            {"dependencyUnavailable", ExitUnavailable}}},
          {"supportsJsonlTrace", true},
          {"silentInference", false}}}};
}

Outcome Help()
{
    return Success("help", {
        {"usage", "skrtg [--json] [--trace <events.jsonl>] <command> ..."},
        {"examples", Json::array({
            "skrtg capabilities --json",
            "skrtg profile probe character.fbx --rest-pose t_pose --json",
            "skrtg profile normalize definition.xml --out definition.json --json",
            "skrtg profile create --request profile-create.json --json",
            "skrtg batch validate --request batch-request.json --json",
            "skrtg debug bridge --request bridge-request.json --json"})},
        {"groups", Json::array({"profile", "batch", "bridge", "skrv", "debug"})}});
}

Outcome RunProfile(std::vector<std::string> Arguments)
{
    if (Arguments.empty() || Arguments[0] == "help")
    {
        return Success("profile help", {
            {"commands", Json::array({
                "adapters",
                "probe <definition> [--format <auto|ue_ik_rig_json|skrtg_character_json|skrtg_character_xml|rest_fbx>] [--rest-pose <kind>] [--include-normalized]",
                "normalize <definition> --out <definition.json> [--format <kind>] [--rest-pose <kind>]",
                "create --request <profile-create.json>",
                "create --id <id> --version <semver> --label <name> --rest <fbx> --definition <file> --alignment <json> --out <profile.skrtgprofile> [--canonical <id>] [--format <kind>] [--rest-pose <kind>]",
                "inspect <profile.skrtgprofile>",
                "install <profile.skrtgprofile> [--store <directory>]",
                "list [--store <directory>]"})}});
    }
    const std::string Action = Arguments[0];
    Arguments.erase(Arguments.begin());
    if (IsHelpRequest(Arguments))
    {
        if (Action == "adapters")
            return CommandHelp("profile adapters", "skrtg profile adapters [--json]");
        if (Action == "probe")
            return CommandHelp("profile probe", "skrtg profile probe <definition> [--format <kind>] [--rest-pose <kind>] [--include-normalized] [--json]");
        if (Action == "normalize")
            return CommandHelp("profile normalize", "skrtg profile normalize <definition> --out <definition.json> [--format <kind>] [--rest-pose <kind>] [--json]");
        if (Action == "create")
            return CommandHelp("profile create", "skrtg profile create --request <profile-create.json> [--json] | skrtg profile create --id <id> --version <semver> --label <name> --rest <fbx> --definition <file> --alignment <json> --out <profile.skrtgprofile> [options]");
        if (Action == "inspect")
            return CommandHelp("profile inspect", "skrtg profile inspect <profile.skrtgprofile> [--no-hash] [--json]");
        if (Action == "install")
            return CommandHelp("profile install", "skrtg profile install <profile.skrtgprofile> [--store <directory>] [--json]");
        if (Action == "list")
            return CommandHelp("profile list", "skrtg profile list [--store <directory>] [--json]");
        return UsageFailure("profile", "unknown profile command: " + Action);
    }
    if (Action == "adapters")
    {
        if (!Arguments.empty()) return UsageFailure("profile adapters", "unexpected arguments");
        return Success("profile adapters", {
            {"adapters", Profile::SupportedCharacterDefinitionAdapters()},
            {"autoDetection", "content schema for JSON; extension for XML and FBX"},
            {"unknownFormatPolicy", "fail_closed"}});
    }
    if (Action == "probe" || Action == "normalize")
    {
        const std::string Command = "profile " + Action;
        if (Arguments.empty()) return UsageFailure(Command, "definition path is required");
        const std::filesystem::path Input = PathFromUtf8(Arguments.front());
        Arguments.erase(Arguments.begin());
        std::string OptionError;
        const auto FormatText = TakeOption(Arguments, "--format", OptionError);
        if (!OptionError.empty()) return UsageFailure(Command, OptionError);
        const auto RestPose = TakeOption(Arguments, "--rest-pose", OptionError);
        if (!OptionError.empty()) return UsageFailure(Command, OptionError);
        const auto Output = TakeOption(Arguments, "--out", OptionError);
        if (!OptionError.empty()) return UsageFailure(Command, OptionError);
        const bool IncludeNormalized = TakeFlag(Arguments, "--include-normalized");
        if (!Arguments.empty()) return UsageFailure(Command, "unknown or duplicate arguments");
        if (Action == "normalize" && !Output.has_value())
            return UsageFailure(Command, "--out is required");
        if (Action == "probe" && Output.has_value())
            return UsageFailure(Command, "--out belongs to profile normalize");
        Profile::CharacterDefinitionInspectOptions Options;
        if (FormatText.has_value() &&
            !Profile::ParseCharacterDefinitionFormat(*FormatText, Options.Format))
            return UsageFailure(Command, "unknown --format value");
        Options.RestPoseKind = RestPose.value_or("unknown");
        const auto Result = Profile::InspectCharacterDefinition(Input, Options);
        if (!Result.Success)
        {
            return Failure(
                Command, "UNSUPPORTED_OR_INVALID_CHARACTER_DEFINITION",
                "definition", "Character Definition inspection failed",
                Result.Errors,
                {"Run `skrtg profile adapters --json` to list accepted formats.",
                 "Use an explicit --format only when the file matches that adapter."});
        }
        Json Data = {
            {"sourcePath", PathUtf8(Result.SourcePath)},
            {"definition", DefinitionJson(Result.Definition)}};
        if (IncludeNormalized) Data["normalized"] = Json::parse(Result.NormalizedJson);
        if (Action == "normalize")
        {
            std::string Error;
            if (!Profile::WriteNormalizedCharacterDefinition(
                    Result, PathFromUtf8(*Output), Error))
            {
                return Failure(
                    Command, "WRITE_FAILED", "commit", Error);
            }
            Data["output"] = PathUtf8(
                std::filesystem::absolute(PathFromUtf8(*Output)).lexically_normal());
        }
        return Success(Command, std::move(Data), Result.Warnings);
    }
    if (Action == "create")
    {
        const std::string Command = "profile create";
        std::string OptionError;
        const auto RequestPath = TakeOption(Arguments, "--request", OptionError);
        if (!OptionError.empty()) return UsageFailure(Command, OptionError);
        Profile::CharacterProfileCreateRequest Request;
        if (RequestPath.has_value())
        {
            if (!Arguments.empty())
                return UsageFailure(Command, "--request cannot be combined with direct create options");
            if (!Profile::ReadCharacterProfileCreateRequest(
                    PathFromUtf8(*RequestPath), Request, OptionError))
            {
                return Failure(
                    Command, "REQUEST_SCHEMA_INVALID", "request",
                    "Profile create request could not be read",
                    {OptionError}, {}, ExitUsage);
            }
        }
        else
        {
            const auto Id = TakeOption(Arguments, "--id", OptionError);
            const auto Version = TakeOption(Arguments, "--version", OptionError);
            const auto Label = TakeOption(Arguments, "--label", OptionError);
            const auto Canonical = TakeOption(Arguments, "--canonical", OptionError);
            const auto Rest = TakeOption(Arguments, "--rest", OptionError);
            const auto Definition = TakeOption(Arguments, "--definition", OptionError);
            const auto Alignment = TakeOption(Arguments, "--alignment", OptionError);
            const auto Output = TakeOption(Arguments, "--out", OptionError);
            const auto Format = TakeOption(Arguments, "--format", OptionError);
            const auto RestPose = TakeOption(Arguments, "--rest-pose", OptionError);
            const bool SourceDisabled = TakeFlag(Arguments, "--source-disabled");
            const bool TargetDisabled = TakeFlag(Arguments, "--target-disabled");
            if (!OptionError.empty()) return UsageFailure(Command, OptionError);
            if (!Arguments.empty()) return UsageFailure(Command, "unknown or duplicate create arguments");
            if (!Id || !Version || !Label || !Rest || !Definition ||
                !Alignment || !Output)
                return UsageFailure(Command, "id, version, label, rest, definition, alignment, and out are required");
            Request.ProfileId = *Id;
            Request.ProfileVersion = *Version;
            Request.DisplayName = *Label;
            Request.CanonicalProfileId = Canonical.value_or("ue5_manny");
            Request.RestFbx = PathFromUtf8(*Rest);
            Request.DefinitionFile = PathFromUtf8(*Definition);
            Request.AlignmentRetargeterJson = PathFromUtf8(*Alignment);
            Request.OutputPackage = PathFromUtf8(*Output);
            Request.RestPoseKind = RestPose.value_or("unknown");
            Request.SourceEnabled = !SourceDisabled;
            Request.TargetEnabled = !TargetDisabled;
            if (Format.has_value() &&
                !Profile::ParseCharacterDefinitionFormat(*Format, Request.Format))
                return UsageFailure(Command, "unknown --format value");
        }
        const auto Result = Profile::CreateCharacterProfile(Request);
        if (!Result.Success)
        {
            return Failure(
                Command,
                Result.Stage == "definition"
                    ? "PROFILE_DEFINITION_NOT_READY"
                    : "PROFILE_CREATE_FAILED",
                Result.Stage,
                "Character Profile was not committed",
                Result.Errors,
                {"Use `skrtg profile probe <definition> --json` to inspect completeness.",
                 "No output package is committed on failure."},
                ExitFailure,
                {{"definition", DefinitionJson(Result.Definition)}});
        }
        return Success(
            Command,
            {{"package", PathUtf8(Result.Package.PackagePath)},
             {"packageSha256", Result.Package.PackageSha256},
             {"profile", ProfileJson(Result.Package.Profile)},
             {"entries", ProfileEntriesJson(Result.Package.Entries)}},
            Result.Warnings);
    }
    if (Action == "inspect")
    {
        if (Arguments.size() != 1)
            return UsageFailure("profile inspect", "exactly one package path is required");
        const auto Result = Profile::InspectCharacterProfilePackage(
            PathFromUtf8(Arguments[0]));
        if (!Result.Success)
            return Failure("profile inspect", "PROFILE_INVALID", "inspect",
                "Character Profile verification failed", Result.Errors);
        return Success("profile inspect", {
            {"path", PathUtf8(Result.PackagePath)},
            {"packageSha256", Result.PackageSha256},
            {"profile", ProfileJson(Result.Profile)},
            {"entries", ProfileEntriesJson(Result.Entries)}});
    }
    if (Action == "install")
    {
        if (Arguments.empty())
            return UsageFailure("profile install", "package path is required");
        const std::filesystem::path Package = PathFromUtf8(Arguments.front());
        Arguments.erase(Arguments.begin());
        std::string OptionError;
        const auto Store = TakeOption(Arguments, "--store", OptionError);
        if (!OptionError.empty() || !Arguments.empty())
            return UsageFailure("profile install", OptionError.empty() ? "unknown arguments" : OptionError);
        const auto Result = Profile::InstallCharacterProfilePackage(
            Package,
            Store ? PathFromUtf8(*Store) : Profile::DefaultCharacterProfileStore());
        if (!Result.Success)
            return Failure("profile install", "PROFILE_INSTALL_FAILED", "install",
                "Character Profile installation failed", Result.Errors);
        return Success("profile install", {
            {"alreadyInstalled", Result.AlreadyInstalled},
            {"installDirectory", PathUtf8(Result.Installed.InstallDirectory)},
            {"packageSha256", Result.Installed.PackageSha256},
            {"profile", ProfileJson(Result.Installed.Profile)}});
    }
    if (Action == "list")
    {
        std::string OptionError;
        const auto Store = TakeOption(Arguments, "--store", OptionError);
        if (!OptionError.empty() || !Arguments.empty())
            return UsageFailure("profile list", OptionError.empty() ? "unknown arguments" : OptionError);
        const auto Result = Profile::DiscoverInstalledCharacterProfiles(
            Store ? PathFromUtf8(*Store) : Profile::DefaultCharacterProfileStore());
        if (!Result.Success)
            return Failure("profile list", "PROFILE_STORE_INVALID", "discover",
                "Character Profile store could not be read", Result.Errors);
        Json Profiles = Json::array();
        for (const auto& Installed : Result.Profiles)
        {
            Profiles.push_back({
                {"profile", ProfileJson(Installed.Profile)},
                {"installDirectory", PathUtf8(Installed.InstallDirectory)},
                {"packagePath", PathUtf8(Installed.PackagePath)},
                {"packageSha256", Installed.PackageSha256}});
        }
        return Success("profile list", {
            {"store", PathUtf8(Result.StoreRoot)},
            {"count", Profiles.size()},
            {"profiles", Profiles}}, Result.Warnings);
    }
    return UsageFailure("profile", "unknown profile command: " + Action);
}

Outcome RunBatch(
    std::vector<std::string> Arguments,
    const bool DebugAlias)
{
    const std::string Prefix = DebugAlias ? "debug batch" : "batch";
    std::string Action = DebugAlias ? "validate" : "";
    if (!DebugAlias)
    {
        if (Arguments.empty() || Arguments[0] == "help")
            return Success("batch help", {{"usage", "skrtg batch <validate|run> --request <batch-request.json>"}});
        Action = Arguments[0];
        Arguments.erase(Arguments.begin());
    }
    if (IsHelpRequest(Arguments))
    {
        const std::string Usage = DebugAlias
            ? "skrtg debug batch --request <batch-request.json> [--json]"
            : "skrtg batch " + Action + " --request <batch-request.json> [--json]";
        return CommandHelp(Prefix + " " + Action, Usage);
    }
    if (Action != "validate" && Action != "run")
        return UsageFailure(Prefix, "expected validate or run");
    std::string OptionError;
    const auto RequestPath = TakeOption(Arguments, "--request", OptionError);
    if (!OptionError.empty() || !RequestPath || !Arguments.empty())
        return UsageFailure(Prefix + " " + Action,
            OptionError.empty() ? "--request is required and must be the only option" : OptionError);
    BatchRetargetRequest Request;
    if (!ReadBatchRetargetRequest(PathFromUtf8(*RequestPath), Request, OptionError))
        return Failure(Prefix + " " + Action, "REQUEST_SCHEMA_INVALID", "request",
            "Batch request could not be read", {OptionError}, {}, ExitUsage);
    if (Action == "validate")
    {
        const BatchRetargetPlan Plan = BuildBatchRetargetPlan(Request);
        if (!Plan.Success)
            return Failure(Prefix + " validate", "BATCH_PREFLIGHT_FAILED", "preflight",
                "Batch request did not pass preflight", Plan.Errors, {}, ExitFailure,
                BatchPlanJson(Plan));
        return Success(Prefix + " validate", BatchPlanJson(Plan), Plan.Warnings);
    }
    const BatchRetargetRunResult Result = RunBatchRetarget(Request);
    Json Jobs = Json::array();
    for (const BatchRetargetJob& Job : Result.Status.Jobs)
        Jobs.push_back(BatchJobJson(Job));
    Json Data = {
        {"statusJson", PathUtf8(Result.StatusJson)},
        {"success", Result.Status.Success},
        {"cancelled", Result.Status.Cancelled},
        {"totalJobs", Result.Status.TotalJobs},
        {"completedJobs", Result.Status.CompletedJobs},
        {"succeededJobs", Result.Status.SucceededJobs},
        {"failedJobs", Result.Status.FailedJobs},
        {"maximumConcurrentJobs", Result.Status.MaximumConcurrentJobs},
        {"durationSeconds", Result.Status.DurationSeconds},
        {"jobs", Jobs}};
    if (!Result.Success)
        return Failure(Prefix + " run", "BATCH_RUN_FAILED", "execute",
            "Batch execution failed", Result.Errors, {}, ExitFailure, Data);
    return Success(Prefix + " run", std::move(Data));
}

Outcome RunBridge(
    std::vector<std::string> Arguments,
    const bool DebugAlias)
{
    const std::string Prefix = DebugAlias ? "debug bridge" : "bridge";
    std::string Action = DebugAlias ? "validate" : "";
    if (!DebugAlias)
    {
        if (Arguments.empty() || Arguments[0] == "help")
            return Success("bridge help", {{"usage", "skrtg bridge <validate|run> --request <bridge-request.json>"}});
        Action = Arguments[0];
        Arguments.erase(Arguments.begin());
    }
    if (IsHelpRequest(Arguments))
    {
        const std::string Usage = DebugAlias
            ? "skrtg debug bridge --request <bridge-request.json> [--json]"
            : "skrtg bridge " + Action + " --request <bridge-request.json> [--json]";
        return CommandHelp(Prefix + " " + Action, Usage);
    }
    if (Action != "validate" && Action != "run")
        return UsageFailure(Prefix, "expected validate or run");
    std::string OptionError;
    const auto RequestPath = TakeOption(Arguments, "--request", OptionError);
    if (!OptionError.empty() || !RequestPath || !Arguments.empty())
        return UsageFailure(Prefix + " " + Action,
            OptionError.empty() ? "--request is required and must be the only option" : OptionError);
    RetargetBridgeRequest Request;
    if (!ReadRetargetBridgeRequest(PathFromUtf8(*RequestPath), Request, OptionError))
        return Failure(Prefix + " " + Action, "REQUEST_SCHEMA_INVALID", "request",
            "Bridge request could not be read", {OptionError}, {}, ExitUsage);
    if (Action == "validate")
    {
        const RetargetBridgePreflight Preflight = PreflightRetargetBridge(Request);
        if (!Preflight.Success)
            return Failure(Prefix + " validate", "BRIDGE_PREFLIGHT_FAILED", "preflight",
                "Bridge request did not pass preflight", Preflight.Errors, {}, ExitFailure,
                BridgePreflightJson(Preflight));
        return Success(Prefix + " validate", BridgePreflightJson(Preflight), Preflight.Warnings);
    }
    const RetargetBridgeRunResult Result = RunRetargetBridge(Request);
    Json Data = {
        {"reviewPackage", PathUtf8(Result.ReviewPackage)},
        {"statusJson", PathUtf8(Result.StatusJson)},
        {"retargeterLog", PathUtf8(Result.RetargeterLog)},
        {"adapterLog", PathUtf8(Result.AdapterLog)},
        {"packLog", PathUtf8(Result.PackLog)},
        {"sourceAnimationSha256", Result.SourceAnimationSha256},
        {"retargeterExitCode", Result.RetargeterExitCode},
        {"adapterExitCode", Result.AdapterExitCode},
        {"packExitCode", Result.PackExitCode}};
    if (!Result.Success)
        return Failure(Prefix + " run", "BRIDGE_RUN_FAILED", "execute",
            "Bridge execution failed", Result.Errors, {}, ExitFailure, Data);
    return Success(Prefix + " run", std::move(Data));
}

Outcome RunSkrv(std::vector<std::string> Arguments)
{
    if (Arguments.empty() || Arguments[0] == "help")
        return Success("skrv help", {{"usage", "skrtg skrv inspect <package-directory> [--no-hash]"}});
    const std::string Action = Arguments[0];
    Arguments.erase(Arguments.begin());
    if (IsHelpRequest(Arguments))
    {
        if (Action == "inspect")
            return CommandHelp("skrv inspect", "skrtg skrv inspect <package-directory> [--no-hash] [--json]");
        return UsageFailure("skrv", "unknown SKRV command: " + Action);
    }
    if (Action != "inspect" || Arguments.empty())
        return UsageFailure("skrv", "expected inspect and one package directory");
    const std::filesystem::path Directory = PathFromUtf8(Arguments.front());
    Arguments.erase(Arguments.begin());
    const bool NoHash = TakeFlag(Arguments, "--no-hash");
    if (!Arguments.empty()) return UsageFailure("skrv inspect", "unknown arguments");
    Skrv::PackageInspectOptions Options;
    Options.VerifyHashes = !NoHash;
    const auto Result = Skrv::InspectDirectoryPackage(Directory, Options);
    if (!Result.Success)
        return Failure("skrv inspect", "SKRV_INVALID", "inspect",
            "SKRV verification failed", Result.Errors);
    Json Entries = Json::array();
    std::uintmax_t Bytes = 0;
    for (const auto& Entry : Result.Entries)
    {
        Bytes += Entry.ByteCount;
        Entries.push_back({
            {"role", Skrv::EntryRoleName(Entry.Role)},
            {"path", PathUtf8(Entry.RelativePath)},
            {"byteCount", Entry.ByteCount},
            {"sha256", Entry.Sha256}});
    }
    return Success("skrv inspect", {
        {"path", PathUtf8(Result.PackageDirectory)},
        {"hashesVerified", Options.VerifyHashes},
        {"indexedBytes", Bytes},
        {"manifestSha256", Result.ManifestSha256},
        {"integrityIndexSha256", Result.IntegrityIndexSha256},
        {"manifest",
         {{"contractVersion", Result.Manifest.ContractVersion},
          {"clipCount", Result.Manifest.ClipCount},
          {"frameCount", Result.Manifest.FrameCount},
          {"sourceBoneCount", Result.Manifest.SourceBoneCount},
          {"targetBoneCount", Result.Manifest.TargetBoneCount},
          {"mappedChainCount", Result.Manifest.MappedChainCount},
          {"goalChainCount", Result.Manifest.GoalChainCount},
          {"referencedBlobCount", Result.Manifest.ReferencedBlobCount},
          {"verifiedExportCount", Result.Manifest.VerifiedExportCount}}},
        {"entries", Entries}});
}

Outcome RunDoctor(const std::filesystem::path& Executable)
{
    const std::filesystem::path Directory =
        std::filesystem::absolute(Executable).lexically_normal().parent_path();
#if defined(_WIN32)
    constexpr const char* Suffix = ".exe";
#else
    constexpr const char* Suffix = "";
#endif
    struct Tool { const char* Name; bool Required; };
    const std::vector<Tool> Tools = {
        {"skrtg_viewer", true},
        {"skrtg_ueik_retarget_worker", true},
        {"skrtg_retarget_bridge", false},
        {"skrtg_batch_retarget", false},
        {"skrv_pack", false},
        {"skrv_inspect", false},
        {"skrtgprofile_pack", false},
        {"skrtgprofile_inspect", false}};
    bool Healthy = true;
    Json Checks = Json::array();
    for (const Tool& ToolInfo : Tools)
    {
        const std::filesystem::path Path =
            Directory / (std::string(ToolInfo.Name) + Suffix);
        std::error_code Error;
        const bool Present = std::filesystem::is_regular_file(Path, Error) && !Error;
        if (ToolInfo.Required && !Present) Healthy = false;
        Checks.push_back({
            {"kind", "companion_executable"},
            {"name", ToolInfo.Name},
            {"required", ToolInfo.Required},
            {"present", Present},
            {"path", PathUtf8(Path)}});
    }
    const std::filesystem::path Store = Profile::DefaultCharacterProfileStore();
    Checks.push_back({
        {"kind", "profile_store"},
        {"required", false},
        {"path", PathUtf8(Store)},
        {"present", std::filesystem::is_directory(Store)}});
    Json Data = {
        {"healthy", Healthy},
        {"executableDirectory", PathUtf8(Directory)},
        {"checks", Checks},
        {"definitionAdapters", Profile::SupportedCharacterDefinitionAdapters()}};
    if (!Healthy)
        return Failure("doctor", "DEPENDENCY_UNAVAILABLE", "environment",
            "One or more required runtime companions are missing", {},
            {"Build or install the complete SKRTG runtime beside skrtg."},
            ExitUnavailable, Data);
    return Success("doctor", std::move(Data));
}

Outcome RunDebug(std::vector<std::string> Arguments)
{
    if (Arguments.empty() || Arguments[0] == "help")
        return Success("debug help", {{"commands", Json::array({
            "profile <package>",
            "batch --request <batch-request.json>",
            "bridge --request <bridge-request.json>"})}});
    const std::string Kind = Arguments[0];
    Arguments.erase(Arguments.begin());
    if (IsHelpRequest(Arguments))
    {
        if (Kind == "profile")
            return CommandHelp("debug profile", "skrtg debug profile <profile.skrtgprofile> [--no-hash] [--json]");
        if (Kind == "batch")
            return CommandHelp("debug batch", "skrtg debug batch --request <batch-request.json> [--json]");
        if (Kind == "bridge")
            return CommandHelp("debug bridge", "skrtg debug bridge --request <bridge-request.json> [--json]");
        return UsageFailure("debug", "unknown debug target: " + Kind);
    }
    if (Kind == "profile")
    {
        Arguments.insert(Arguments.begin(), "inspect");
        Outcome Result = RunProfile(std::move(Arguments));
        Result.Command = "debug profile";
        return Result;
    }
    if (Kind == "batch") return RunBatch(std::move(Arguments), true);
    if (Kind == "bridge") return RunBridge(std::move(Arguments), true);
    return UsageFailure("debug", "unknown debug target: " + Kind);
}

Outcome Dispatch(
    std::vector<std::string> Arguments,
    const std::filesystem::path& Executable)
{
    if (Arguments.empty() || Arguments[0] == "help" ||
        Arguments[0] == "--help" || Arguments[0] == "-h")
        return Help();
    const std::string Command = Arguments[0];
    Arguments.erase(Arguments.begin());
    if (Command == "version")
    {
        if (IsHelpRequest(Arguments))
            return CommandHelp("version", "skrtg version [--json]");
        if (!Arguments.empty()) return UsageFailure("version", "unexpected arguments");
        return Success("version", {
            {"name", "SKRTG Engine CLI"},
            {"version", SKRTG_CLI_VERSION},
            {"profilePackageVersion", 1},
            {"characterDefinitionVersion", 1}});
    }
    if (Command == "capabilities")
    {
        if (IsHelpRequest(Arguments))
            return CommandHelp("capabilities", "skrtg capabilities [--json]");
        if (!Arguments.empty()) return UsageFailure("capabilities", "unexpected arguments");
        return Success("capabilities", Capabilities());
    }
    if (Command == "doctor")
    {
        if (IsHelpRequest(Arguments))
            return CommandHelp("doctor", "skrtg doctor [--json] [--trace <events.jsonl>]");
        if (!Arguments.empty()) return UsageFailure("doctor", "unexpected arguments");
        return RunDoctor(Executable);
    }
    if (Command == "profile") return RunProfile(std::move(Arguments));
    if (Command == "batch") return RunBatch(std::move(Arguments), false);
    if (Command == "bridge") return RunBridge(std::move(Arguments), false);
    if (Command == "skrv") return RunSkrv(std::move(Arguments));
    if (Command == "debug") return RunDebug(std::move(Arguments));
    return UsageFailure(Command, "unknown command: " + Command);
}

Json Envelope(const Outcome& Result)
{
    if (Result.Ok)
    {
        return {
            {"schema", "skrtg.cli.result.v1"},
            {"schemaVersion", 1},
            {"ok", true},
            {"command", Result.Command},
            {"data", Result.Data},
            {"warnings", Result.Warnings},
            {"meta", {{"cliVersion", SKRTG_CLI_VERSION}}}};
    }
    Json Error = {
        {"code", Result.ErrorCode},
        {"stage", Result.Stage},
        {"retryable", false},
        {"message", Result.Message},
        {"details", Result.Details},
        {"suggestedActions", Result.SuggestedActions}};
    if (!Result.Context.empty()) Error["context"] = Result.Context;
    return {
        {"schema", "skrtg.cli.error.v1"},
        {"schemaVersion", 1},
        {"ok", false},
        {"command", Result.Command},
        {"error", Error},
        {"meta", {{"cliVersion", SKRTG_CLI_VERSION}}}};
}

void PrintHuman(const Outcome& Result)
{
    if (Result.Ok)
    {
        std::cout << "SKRTG: " << Result.Command << " succeeded\n";
        if (!Result.Data.empty()) std::cout << Result.Data.dump(2) << '\n';
        for (const std::string& Warning : Result.Warnings)
            std::cerr << "warning: " << Warning << '\n';
        return;
    }
    std::cerr << "SKRTG: " << Result.Command << " failed ["
              << Result.ErrorCode << "] at " << Result.Stage << "\n"
              << Result.Message << '\n';
    for (const std::string& Detail : Result.Details)
        std::cerr << "  - " << Detail << '\n';
    for (const std::string& Action : Result.SuggestedActions)
        std::cerr << "next: " << Action << '\n';
}

bool WriteTraceEvent(
    const std::filesystem::path& Path,
    const Json& Event,
    std::string& OutError)
{
    std::ofstream Output(Path, std::ios::binary | std::ios::app);
    if (!Output)
    {
        OutError = "failed to open trace JSONL: " + PathUtf8(Path);
        return false;
    }
    Output << Event.dump() << '\n';
    if (!Output)
    {
        OutError = "failed to append trace JSONL";
        return false;
    }
    return true;
}
} // namespace

int skrtg::cli::Run(const int argc, char** argv)
{
    ConfigureNonInteractiveCli();
    std::vector<std::string> Arguments;
    for (int Index = 1; Index < argc; ++Index)
        Arguments.emplace_back(argv[Index]);

    const bool JsonOutput = TakeFlag(Arguments, "--json");
    std::string GlobalError;
    const auto Trace = TakeOption(Arguments, "--trace", GlobalError);
    if (!GlobalError.empty())
    {
        const Outcome Result = UsageFailure("global", GlobalError);
        if (JsonOutput) std::cout << Envelope(Result).dump() << '\n';
        else PrintHuman(Result);
        return Result.ExitCode;
    }

    const auto Started = std::chrono::steady_clock::now();
    std::filesystem::path TracePath;
    if (Trace.has_value())
    {
        TracePath = std::filesystem::absolute(PathFromUtf8(*Trace)).lexically_normal();
        std::error_code Error;
        std::filesystem::create_directories(TracePath.parent_path(), Error);
        if (Error)
        {
            const Outcome Result = Failure(
                "global", "TRACE_UNAVAILABLE", "trace",
                "Trace directory could not be created", {Error.message()}, {},
                ExitUnavailable);
            if (JsonOutput) std::cout << Envelope(Result).dump() << '\n';
            else PrintHuman(Result);
            return Result.ExitCode;
        }
        std::string TraceError;
        if (!WriteTraceEvent(
                TracePath,
                {{"schema", "skrtg.cli.trace_event.v1"},
                 {"schemaVersion", 1},
                 {"event", "command_start"},
                 {"unixTimeMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count()},
                 {"arguments", Arguments}},
                TraceError))
        {
            const Outcome Result = Failure(
                "global", "TRACE_UNAVAILABLE", "trace", TraceError,
                {}, {}, ExitUnavailable);
            if (JsonOutput) std::cout << Envelope(Result).dump() << '\n';
            else PrintHuman(Result);
            return Result.ExitCode;
        }
    }

    const Outcome Result = Dispatch(
        std::move(Arguments), argc > 0 ? PathFromUtf8(argv[0]) : "skrtg");
    if (JsonOutput) std::cout << Envelope(Result).dump() << '\n';
    else PrintHuman(Result);

    if (!TracePath.empty())
    {
        const auto Duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - Started).count();
        std::string Ignored;
        WriteTraceEvent(
            TracePath,
            {{"schema", "skrtg.cli.trace_event.v1"},
             {"schemaVersion", 1},
             {"event", "command_finish"},
             {"unixTimeMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch()).count()},
             {"command", Result.Command},
             {"ok", Result.Ok},
             {"exitCode", Result.ExitCode},
             {"durationMs", Duration}},
            Ignored);
    }
    return Result.ExitCode;
}
