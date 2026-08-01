#include "skrtg/viewer/retarget_asset_catalog.h"

#include "skrtg/viewer/skrv/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

namespace skrtg::viewer
{
namespace
{
constexpr const char* CatalogSchema =
    "skrtg.native_viewer.retarget_asset_catalog.v1";

bool IsRegularFile(const std::filesystem::path& Path)
{
    std::error_code Error;
    return !Path.empty() &&
        std::filesystem::is_regular_file(Path, Error) && !Error;
}

std::string UpperAscii(std::string Value)
{
    std::transform(
        Value.begin(), Value.end(), Value.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::toupper(Character));
        });
    return Value;
}

std::string LowerAscii(std::string Value)
{
    std::transform(
        Value.begin(), Value.end(), Value.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::tolower(Character));
        });
    return Value;
}

bool IsContractId(const std::string& Value)
{
    if (Value.empty()) return false;
    return std::all_of(
        Value.begin(), Value.end(),
        [](const unsigned char Character)
        {
            return (Character >= 'a' && Character <= 'z') ||
                (Character >= '0' && Character <= '9') ||
                Character == '_';
        });
}

bool IsSha256(const std::string& Value)
{
    return Value.size() == 64 &&
        std::all_of(
            Value.begin(), Value.end(),
            [](const unsigned char Character)
            {
                return std::isxdigit(Character) != 0;
            });
}

bool IsSafeRelativePath(const std::filesystem::path& Path)
{
    if (Path.empty() || Path.is_absolute() || Path.has_root_path())
        return false;
    for (const std::filesystem::path& Component : Path)
    {
        if (Component == "..") return false;
    }
    return true;
}

std::filesystem::path ResolveAssetPath(
    const std::filesystem::path& AssetRoot,
    const std::string& Text,
    const char* Label,
    std::vector<std::string>& Errors)
{
    const std::filesystem::path Relative = PathFromUtf8(Text);
    if (!IsSafeRelativePath(Relative))
    {
        Errors.push_back(
            std::string(Label) +
            " must be a non-empty relative path without '..': " + Text);
        return {};
    }
    return (AssetRoot / Relative).lexically_normal();
}

struct FileBinding
{
    std::filesystem::path Path;
    std::string Sha256;
};

FileBinding ParseFileBinding(
    const nlohmann::json& Json,
    const char* Field,
    const std::filesystem::path& AssetRoot,
    std::vector<std::string>& Errors)
{
    FileBinding Result;
    const nlohmann::json& Binding = Json.at(Field);
    const std::string PathText = Binding.at("path").get<std::string>();
    Result.Path = ResolveAssetPath(
        AssetRoot, PathText, Field, Errors);
    Result.Sha256 = UpperAscii(
        Binding.at("sha256").get<std::string>());
    if (!IsSha256(Result.Sha256))
    {
        Errors.push_back(
            std::string(Field) +
            " must declare a 64-character SHA-256 value");
    }
    return Result;
}

bool VerifyBinding(
    const FileBinding& Binding,
    const char* Label,
    std::map<std::filesystem::path, std::string>& HashCache,
    std::vector<std::string>& Errors)
{
    if (!IsRegularFile(Binding.Path))
    {
        Errors.push_back(
            std::string(Label) + " is not a readable file: " +
            PathToUtf8(Binding.Path));
        return false;
    }
    auto Hash = HashCache.find(Binding.Path);
    if (Hash == HashCache.end())
    {
        std::string Computed;
        std::string Error;
        if (!skrv::Sha256File(Binding.Path, Computed, Error))
        {
            Errors.push_back(
                std::string("failed to hash ") + Label + ": " + Error);
            return false;
        }
        Hash = HashCache.emplace(
            Binding.Path, UpperAscii(Computed)).first;
    }
    if (Hash->second != Binding.Sha256)
    {
        Errors.push_back(
            std::string(Label) + " SHA-256 mismatch: expected " +
            Binding.Sha256 + ", got " + Hash->second);
        return false;
    }
    return true;
}

FileBinding Binding(
    const std::filesystem::path& Path,
    const std::string& Sha256)
{
    return {Path, Sha256};
}

bool HasExtension(
    const std::filesystem::path& Path,
    const std::string& Expected)
{
    return LowerAscii(Path.extension().string()) == Expected;
}

void ValidateSkeletonFileTypes(
    const RetargetSkeletonAsset& Skeleton,
    std::vector<std::string>& Errors)
{
    if (!HasExtension(Skeleton.RestFbx, ".fbx"))
    {
        Errors.push_back(
            "skeleton " + Skeleton.Id + " restFbx must use .fbx");
    }
    if (!HasExtension(Skeleton.IkRigJson, ".json") ||
        !HasExtension(Skeleton.AlignmentRetargeterJson, ".json"))
    {
        Errors.push_back(
            "skeleton " + Skeleton.Id +
            " definitions must be exported UE JSON files");
    }
}

void ValidateAnimationFileTypes(
    const RetargetAnimationAsset& Animation,
    std::vector<std::string>& Errors)
{
    if (!HasExtension(Animation.Fbx, ".fbx"))
    {
        Errors.push_back(
            "animation " + Animation.Id + " fbx must use .fbx");
    }
    if (!HasExtension(Animation.GoldenJson, ".json"))
    {
        Errors.push_back(
            "animation " + Animation.Id +
            " goldenJson must be exported JSON");
    }
}
} // namespace

std::filesystem::path DiscoverRetargetAssetCatalog(
    const std::filesystem::path& ViewerExecutable)
{
    const std::filesystem::path Directory =
        std::filesystem::absolute(ViewerExecutable).parent_path();
    const std::vector<std::filesystem::path> Candidates = {
        Directory / "data" / "retarget_catalog" /
            "retarget_asset_catalog.json",
        Directory / "retarget_asset_catalog.json",
        Directory / ".." / "data" / "retarget_catalog" /
            "retarget_asset_catalog.json",
        Directory / ".." / ".." / "AssetCatalog" /
            "retarget_asset_catalog.json",
        std::filesystem::current_path() / "data" /
            "retarget_catalog" / "retarget_asset_catalog.json",
        std::filesystem::current_path() / "native_viewer" /
            "runtime_data" / "retarget_catalog" /
            "retarget_asset_catalog.json"};
    for (const std::filesystem::path& Candidate : Candidates)
    {
        if (IsRegularFile(Candidate))
            return Candidate.lexically_normal();
    }
    return Candidates.front().lexically_normal();
}

RetargetAssetCatalogLoadResult LoadRetargetAssetCatalog(
    const std::filesystem::path& CatalogFile,
    const bool VerifyFilesAndHashes)
{
    RetargetAssetCatalogLoadResult Result;
    Result.Catalog.CatalogFile =
        std::filesystem::absolute(CatalogFile).lexically_normal();
    try
    {
        std::ifstream Stream(Result.Catalog.CatalogFile, std::ios::binary);
        if (!Stream)
        {
            Result.Errors.push_back(
                "retarget asset catalog is not readable: " +
                PathToUtf8(Result.Catalog.CatalogFile));
            return Result;
        }
        std::string CatalogHashError;
        if (!skrv::Sha256File(
                Result.Catalog.CatalogFile,
                Result.Catalog.CatalogSha256,
                CatalogHashError))
        {
            Result.Errors.push_back(
                "failed to hash retarget asset catalog: " +
                CatalogHashError);
            return Result;
        }
        Result.Catalog.CatalogSha256 =
            UpperAscii(Result.Catalog.CatalogSha256);
        const nlohmann::json Json = nlohmann::json::parse(Stream);
        if (Json.at("schema").get<std::string>() != CatalogSchema)
        {
            Result.Errors.push_back(
                "unsupported retarget asset catalog schema");
            return Result;
        }
        Result.Catalog.CatalogId =
            Json.at("catalogId").get<std::string>();
        if (!IsContractId(Result.Catalog.CatalogId))
        {
            Result.Errors.push_back(
                "catalogId must contain only lowercase ASCII letters, "
                "digits, and underscore");
        }

        const std::filesystem::path RelativeRoot = PathFromUtf8(
            Json.value("assetRoot", std::string(".")));
        if (!IsSafeRelativePath(RelativeRoot) && RelativeRoot != ".")
        {
            Result.Errors.push_back(
                "assetRoot must be a relative path without '..'");
            return Result;
        }
        Result.Catalog.AssetRoot =
            (Result.Catalog.CatalogFile.parent_path() / RelativeRoot)
                .lexically_normal();

        std::set<std::string> SkeletonIds;
        for (const nlohmann::json& Item : Json.at("skeletons"))
        {
            RetargetSkeletonAsset Skeleton;
            Skeleton.Id = Item.at("id").get<std::string>();
            Skeleton.Label = Item.at("label").get<std::string>();
            Skeleton.SourceEnabled =
                Item.value("sourceEnabled", true);
            Skeleton.TargetEnabled =
                Item.value("targetEnabled", true);
            if (!IsContractId(Skeleton.Id))
            {
                Result.Errors.push_back(
                    "invalid skeleton id: " + Skeleton.Id);
            }
            if (!SkeletonIds.insert(Skeleton.Id).second)
            {
                Result.Errors.push_back(
                    "duplicate skeleton id: " + Skeleton.Id);
            }
            if (Skeleton.Label.empty())
            {
                Result.Errors.push_back(
                    "skeleton label is empty: " + Skeleton.Id);
            }
            const FileBinding Rest = ParseFileBinding(
                Item, "restFbx", Result.Catalog.AssetRoot,
                Result.Errors);
            const FileBinding Rig = ParseFileBinding(
                Item, "ikRigJson", Result.Catalog.AssetRoot,
                Result.Errors);
            const FileBinding Alignment = ParseFileBinding(
                Item, "alignmentRetargeterJson",
                Result.Catalog.AssetRoot, Result.Errors);
            Skeleton.RestFbx = Rest.Path;
            Skeleton.RestFbxSha256 = Rest.Sha256;
            Skeleton.IkRigJson = Rig.Path;
            Skeleton.IkRigJsonSha256 = Rig.Sha256;
            Skeleton.AlignmentRetargeterJson = Alignment.Path;
            Skeleton.AlignmentRetargeterJsonSha256 = Alignment.Sha256;
            ValidateSkeletonFileTypes(Skeleton, Result.Errors);
            Result.Catalog.Skeletons.push_back(std::move(Skeleton));
        }

        std::set<std::string> AnimationIds;
        for (const nlohmann::json& Item : Json.at("animations"))
        {
            RetargetAnimationAsset Animation;
            Animation.Id = Item.at("id").get<std::string>();
            Animation.Label = Item.at("label").get<std::string>();
            Animation.SourceSkeletonId =
                Item.at("sourceSkeletonId").get<std::string>();
            Animation.AnimationStack =
                Item.value("animationStack", std::string());
            Animation.Enabled = Item.value("enabled", true);
            if (!IsContractId(Animation.Id))
            {
                Result.Errors.push_back(
                    "invalid animation id: " + Animation.Id);
            }
            if (!AnimationIds.insert(Animation.Id).second)
            {
                Result.Errors.push_back(
                    "duplicate animation id: " + Animation.Id);
            }
            if (Animation.Label.empty())
            {
                Result.Errors.push_back(
                    "animation label is empty: " + Animation.Id);
            }
            if (!ParseRetargetBridgeSourceFbxImportMode(
                    Item.at("sourceFbxImportMode")
                        .get<std::string>(),
                    Animation.SourceFbxImportMode))
            {
                Result.Errors.push_back(
                    "unsupported source FBX import mode for animation " +
                    Animation.Id);
            }
            if (!ParseRetargetBridgeRestFbxImportMode(
                    Item.at("restFbxImportMode")
                        .get<std::string>(),
                    Animation.RestFbxImportMode))
            {
                Result.Errors.push_back(
                    "unsupported rest FBX import mode for animation " +
                    Animation.Id);
            }
            const FileBinding Fbx = ParseFileBinding(
                Item, "fbx", Result.Catalog.AssetRoot,
                Result.Errors);
            const FileBinding Golden = ParseFileBinding(
                Item, "goldenJson", Result.Catalog.AssetRoot,
                Result.Errors);
            Animation.Fbx = Fbx.Path;
            Animation.FbxSha256 = Fbx.Sha256;
            Animation.GoldenJson = Golden.Path;
            Animation.GoldenJsonSha256 = Golden.Sha256;
            ValidateAnimationFileTypes(Animation, Result.Errors);
            Result.Catalog.Animations.push_back(std::move(Animation));
        }

        for (const RetargetAnimationAsset& Animation :
             Result.Catalog.Animations)
        {
            const RetargetSkeletonAsset* Source =
                FindRetargetSkeletonAsset(
                    Result.Catalog, Animation.SourceSkeletonId);
            if (Source == nullptr)
            {
                Result.Errors.push_back(
                    "animation " + Animation.Id +
                    " references unknown source skeleton " +
                    Animation.SourceSkeletonId);
            }
            else if (!Source->SourceEnabled)
            {
                Result.Errors.push_back(
                    "animation " + Animation.Id +
                    " references a source-disabled skeleton");
            }
        }

        if (Result.Catalog.Skeletons.empty())
            Result.Errors.push_back("asset catalog has no skeletons");
        if (Result.Catalog.Animations.empty())
            Result.Warnings.push_back("asset catalog has no animations");

        if (VerifyFilesAndHashes)
        {
            std::map<std::filesystem::path, std::string> HashCache;
            for (const RetargetSkeletonAsset& Skeleton :
                 Result.Catalog.Skeletons)
            {
                VerifyBinding(
                    Binding(Skeleton.RestFbx, Skeleton.RestFbxSha256),
                    (Skeleton.Id + " rest FBX").c_str(),
                    HashCache, Result.Errors);
                VerifyBinding(
                    Binding(
                        Skeleton.IkRigJson,
                        Skeleton.IkRigJsonSha256),
                    (Skeleton.Id + " IK Rig JSON").c_str(),
                    HashCache, Result.Errors);
                VerifyBinding(
                    Binding(
                        Skeleton.AlignmentRetargeterJson,
                        Skeleton.AlignmentRetargeterJsonSha256),
                    (Skeleton.Id + " alignment Retargeter JSON").c_str(),
                    HashCache, Result.Errors);
            }
            for (const RetargetAnimationAsset& Animation :
                 Result.Catalog.Animations)
            {
                VerifyBinding(
                    Binding(Animation.Fbx, Animation.FbxSha256),
                    (Animation.Id + " animation FBX").c_str(),
                    HashCache, Result.Errors);
                VerifyBinding(
                    Binding(
                        Animation.GoldenJson,
                        Animation.GoldenJsonSha256),
                    (Animation.Id + " animation golden JSON").c_str(),
                    HashCache, Result.Errors);
            }
        }
    }
    catch (const std::exception& Error)
    {
        Result.Errors.push_back(
            std::string("invalid retarget asset catalog: ") +
            Error.what());
    }
    Result.Success = Result.Errors.empty();
    return Result;
}

const RetargetSkeletonAsset* FindRetargetSkeletonAsset(
    const RetargetAssetCatalog& Catalog,
    const std::string& SkeletonId)
{
    const auto Found = std::find_if(
        Catalog.Skeletons.begin(), Catalog.Skeletons.end(),
        [&](const RetargetSkeletonAsset& Skeleton)
        {
            return Skeleton.Id == SkeletonId;
        });
    return Found == Catalog.Skeletons.end() ? nullptr : &*Found;
}

const RetargetAnimationAsset* FindRetargetAnimationAsset(
    const RetargetAssetCatalog& Catalog,
    const std::string& AnimationId)
{
    const auto Found = std::find_if(
        Catalog.Animations.begin(), Catalog.Animations.end(),
        [&](const RetargetAnimationAsset& Animation)
        {
            return Animation.Id == AnimationId;
        });
    return Found == Catalog.Animations.end() ? nullptr : &*Found;
}

std::vector<std::size_t> CompatibleRetargetAnimationIndices(
    const RetargetAssetCatalog& Catalog,
    const std::string& SourceSkeletonId)
{
    std::vector<std::size_t> Result;
    for (std::size_t Index = 0;
         Index < Catalog.Animations.size(); ++Index)
    {
        const RetargetAnimationAsset& Animation =
            Catalog.Animations[Index];
        if (Animation.Enabled &&
            Animation.SourceSkeletonId == SourceSkeletonId)
        {
            Result.push_back(Index);
        }
    }
    return Result;
}

RetargetAssetSelectionValidation ValidateRetargetAssetSelection(
    const RetargetAssetCatalog& Catalog,
    const std::string& SourceSkeletonId,
    const std::string& AnimationId,
    const std::string& TargetSkeletonId)
{
    RetargetAssetSelectionValidation Result;
    const RetargetSkeletonAsset* Source =
        FindRetargetSkeletonAsset(Catalog, SourceSkeletonId);
    const RetargetSkeletonAsset* Target =
        FindRetargetSkeletonAsset(Catalog, TargetSkeletonId);
    const RetargetAnimationAsset* Animation =
        FindRetargetAnimationAsset(Catalog, AnimationId);
    if (Source == nullptr)
        Result.Errors.push_back("source skeleton is not in the catalog");
    else if (!Source->SourceEnabled)
        Result.Errors.push_back("source skeleton is disabled");
    if (Target == nullptr)
        Result.Errors.push_back("target skeleton is not in the catalog");
    else if (!Target->TargetEnabled)
        Result.Errors.push_back("target skeleton is disabled");
    if (Animation == nullptr)
        Result.Errors.push_back("source animation is not in the catalog");
    else if (!Animation->Enabled)
        Result.Errors.push_back("source animation is disabled");
    else if (Animation->SourceSkeletonId != SourceSkeletonId)
    {
        Result.Errors.push_back(
            "source animation belongs to " +
            Animation->SourceSkeletonId + ", not " +
            SourceSkeletonId);
    }
    Result.Success = Result.Errors.empty();
    return Result;
}

bool ApplyRetargetAssetSelection(
    const RetargetAssetCatalog& Catalog,
    const std::string& SourceSkeletonId,
    const std::string& AnimationId,
    const std::string& TargetSkeletonId,
    RetargetBridgeRequest& InOutRequest,
    std::vector<std::string>& OutErrors)
{
    OutErrors.clear();
    const RetargetAssetSelectionValidation Validation =
        ValidateRetargetAssetSelection(
            Catalog, SourceSkeletonId, AnimationId,
            TargetSkeletonId);
    if (!Validation.Success)
    {
        OutErrors = Validation.Errors;
        return false;
    }
    const RetargetSkeletonAsset& Source =
        *FindRetargetSkeletonAsset(Catalog, SourceSkeletonId);
    const RetargetSkeletonAsset& Target =
        *FindRetargetSkeletonAsset(Catalog, TargetSkeletonId);
    const RetargetAnimationAsset& Animation =
        *FindRetargetAnimationAsset(Catalog, AnimationId);

    InOutRequest.RouteKind = RetargetBridgeRouteKind::UEIKJsonV1;
    InOutRequest.SourceAnimationFbx = Animation.Fbx;
    InOutRequest.TargetSkeletonFbx = Target.RestFbx;
    InOutRequest.SourceRestFbx = Source.RestFbx;
    InOutRequest.SourceRigJson = Source.IkRigJson;
    InOutRequest.TargetRigJson = Target.IkRigJson;
    InOutRequest.SourceAlignmentRetargeterJson =
        Source.AlignmentRetargeterJson;
    InOutRequest.TargetAlignmentRetargeterJson =
        Target.AlignmentRetargeterJson;
    InOutRequest.SourceAnimationGoldenJson =
        Animation.GoldenJson;
    InOutRequest.AnimationStack = Animation.AnimationStack;
    InOutRequest.SourceFbxImportMode =
        Animation.SourceFbxImportMode;
    InOutRequest.RestFbxImportMode =
        Animation.RestFbxImportMode;
    InOutRequest.EnableSpinePelvisFollow = false;
    InOutRequest.EnableSourceMotionFootLock = false;

    RetargetBridgeAssetBinding& Binding =
        InOutRequest.AssetBinding;
    Binding.Required = true;
    Binding.CatalogFile = Catalog.CatalogFile;
    Binding.CatalogSha256 = Catalog.CatalogSha256;
    Binding.CatalogId = Catalog.CatalogId;
    Binding.SourceSkeletonId = Source.Id;
    Binding.TargetSkeletonId = Target.Id;
    Binding.SourceAnimationId = Animation.Id;
    Binding.SourceAnimationSkeletonId =
        Animation.SourceSkeletonId;
    Binding.SourceAnimationSha256 = Animation.FbxSha256;
    Binding.SourceRestSha256 = Source.RestFbxSha256;
    Binding.TargetRestSha256 = Target.RestFbxSha256;
    Binding.SourceRigJsonSha256 = Source.IkRigJsonSha256;
    Binding.TargetRigJsonSha256 = Target.IkRigJsonSha256;
    Binding.SourceAlignmentRetargeterJsonSha256 =
        Source.AlignmentRetargeterJsonSha256;
    Binding.TargetAlignmentRetargeterJsonSha256 =
        Target.AlignmentRetargeterJsonSha256;
    Binding.SourceAnimationGoldenJsonSha256 =
        Animation.GoldenJsonSha256;
    return true;
}

} // namespace skrtg::viewer
