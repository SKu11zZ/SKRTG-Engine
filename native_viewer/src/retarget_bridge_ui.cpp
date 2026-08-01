#include "retarget_bridge_ui.h"

#include "skrtg/viewer/batch_retarget.h"
#include "skrtg/viewer/profile/character_profile.h"
#include "skrtg/viewer/retarget_asset_catalog.h"
#include "skrtg/viewer/retarget_bridge.h"
#include "skrtg/viewer/verified_export.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace skrtg::viewer
{
namespace
{
constexpr std::size_t PathCapacity = 4096;

enum class ActiveRunKind
{
    None,
    Single,
    Batch
};

enum class BrowseTarget
{
    SourceAnimation,
    TargetSkeleton,
    SourceRest,
    SourceRigJson,
    TargetRigJson,
    SourceAlignmentRetargeterJson,
    TargetAlignmentRetargeterJson,
    OutputRoot,
    BatchAnimationDirectory,
    BatchOutputRoot,
    CharacterProfilePackage,
    OpenSkrv,
    ExportRoot
};

enum class BrowseSelection
{
    FbxFile,
    JsonFile,
    CharacterProfile,
    Directory,
    SkrvPackage
};

struct PathBuffers
{
    std::array<char, PathCapacity> SourceAnimation{};
    std::array<char, PathCapacity> TargetSkeleton{};
    std::array<char, PathCapacity> SourceRest{};
    std::array<char, PathCapacity> SourceRigJson{};
    std::array<char, PathCapacity> TargetRigJson{};
    std::array<char, PathCapacity> SourceAlignmentRetargeterJson{};
    std::array<char, PathCapacity> TargetAlignmentRetargeterJson{};
    std::array<char, PathCapacity> AnimationStack{};
    std::array<char, PathCapacity> OutputRoot{};
    std::array<char, PathCapacity> BatchAnimationDirectory{};
    std::array<char, PathCapacity> BatchOutputRoot{};
    std::array<char, PathCapacity> CharacterProfilePackage{};
    std::array<char, PathCapacity> OpenSkrv{};
    std::array<char, PathCapacity> ExportRoot{};
};

void SetBuffer(
    std::array<char, PathCapacity>& Buffer,
    const std::filesystem::path& Path)
{
    const std::string Text = PathToUtf8(Path);
    const std::size_t Count =
        std::min(Text.size(), Buffer.size() - 1);
    std::memcpy(Buffer.data(), Text.data(), Count);
    Buffer[Count] = '\0';
}

std::filesystem::path BufferPath(
    const std::array<char, PathCapacity>& Buffer)
{
    return PathFromUtf8(Buffer.data());
}

std::filesystem::path RepositoryFromTools(
    const RetargetBridgeTools& Tools)
{
    std::filesystem::path Path = Tools.CanonicalJson;
    for (int Index = 0; Index < 4 && !Path.empty(); ++Index)
        Path = Path.parent_path();
    return Path;
}

std::filesystem::path DefaultRunRoot(
    const std::filesystem::path& Repository)
{
#if defined(_WIN32)
    char* RawLocalAppData = nullptr;
    std::size_t RawSize = 0;
    if (_dupenv_s(
            &RawLocalAppData, &RawSize, "LOCALAPPDATA") == 0 &&
        RawLocalAppData != nullptr)
    {
        const std::filesystem::path Root =
            PathFromUtf8(RawLocalAppData) /
            "SKRTG" / "RetargetRuns";
        std::free(RawLocalAppData);
        return Root;
    }
    std::free(RawLocalAppData);
#endif
    std::error_code Error;
    const std::filesystem::path Temporary =
        std::filesystem::temp_directory_path(Error);
    if (!Error && !Temporary.empty())
        return Temporary / "SKRTG" / "RetargetRuns";
    return Repository / "Artifacts" / "NativeViewer" / "Runs";
}

std::string Timestamp()
{
    const std::time_t Time = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm Local{};
#if defined(_WIN32)
    localtime_s(&Local, &Time);
#else
    localtime_r(&Time, &Local);
#endif
    std::array<char, 32> Text{};
    std::strftime(
        Text.data(), Text.size(), "%Y%m%d_%H%M%S", &Local);
    return Text.data();
}

bool IsFbx(const std::filesystem::path& Path)
{
    std::string Extension = Path.extension().string();
    std::transform(
        Extension.begin(), Extension.end(), Extension.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::tolower(Character));
        });
    return Extension == ".fbx";
}

bool IsJson(const std::filesystem::path& Path)
{
    std::string Extension = Path.extension().string();
    std::transform(
        Extension.begin(), Extension.end(), Extension.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::tolower(Character));
        });
    return Extension == ".json";
}

bool IsSkrvPackage(const std::filesystem::path& Path)
{
    std::error_code Error;
    return std::filesystem::is_directory(Path, Error) && !Error &&
        std::filesystem::is_regular_file(Path / "manifest.json", Error) &&
        !Error &&
        std::filesystem::is_regular_file(Path / "integrity.tsv", Error) &&
        !Error;
}

bool IsCharacterProfilePackage(
    const std::filesystem::path& Path)
{
    std::string Extension = Path.extension().string();
    std::transform(
        Extension.begin(), Extension.end(), Extension.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::tolower(Character));
        });
    std::error_code Error;
    return Extension == ".skrtgprofile" &&
        std::filesystem::is_regular_file(Path, Error) && !Error;
}

bool IsDirectory(const std::filesystem::path& Path)
{
    std::error_code Error;
    return std::filesystem::is_directory(Path, Error) && !Error;
}

std::array<char, PathCapacity>& BufferFor(
    PathBuffers& Buffers,
    const BrowseTarget Target)
{
    switch (Target)
    {
    case BrowseTarget::SourceAnimation: return Buffers.SourceAnimation;
    case BrowseTarget::TargetSkeleton: return Buffers.TargetSkeleton;
    case BrowseTarget::SourceRest: return Buffers.SourceRest;
    case BrowseTarget::SourceRigJson: return Buffers.SourceRigJson;
    case BrowseTarget::TargetRigJson: return Buffers.TargetRigJson;
    case BrowseTarget::SourceAlignmentRetargeterJson:
        return Buffers.SourceAlignmentRetargeterJson;
    case BrowseTarget::TargetAlignmentRetargeterJson:
        return Buffers.TargetAlignmentRetargeterJson;
    case BrowseTarget::OutputRoot: return Buffers.OutputRoot;
    case BrowseTarget::BatchAnimationDirectory:
        return Buffers.BatchAnimationDirectory;
    case BrowseTarget::BatchOutputRoot: return Buffers.BatchOutputRoot;
    case BrowseTarget::CharacterProfilePackage:
        return Buffers.CharacterProfilePackage;
    case BrowseTarget::OpenSkrv: return Buffers.OpenSkrv;
    case BrowseTarget::ExportRoot: return Buffers.ExportRoot;
    }
    return Buffers.OutputRoot;
}

struct BrowserState
{
    BrowseTarget Target = BrowseTarget::SourceAnimation;
    BrowseSelection Selection = BrowseSelection::FbxFile;
    bool RequestOpen = false;
    std::filesystem::path CurrentDirectory;
    std::filesystem::path Selected;
    std::string Error;
};

void BeginBrowse(
    BrowserState& Browser,
    const BrowseTarget Target,
    const BrowseSelection Selection,
    const std::filesystem::path& Existing)
{
    Browser.Target = Target;
    Browser.Selection = Selection;
    Browser.Selected.clear();
    Browser.Error.clear();
    std::error_code Error;
    if (std::filesystem::is_directory(Existing, Error) && !Error)
        Browser.CurrentDirectory = Existing;
    else if (Existing.has_parent_path() &&
             std::filesystem::is_directory(Existing.parent_path(), Error) &&
             !Error)
        Browser.CurrentDirectory = Existing.parent_path();
    else
        Browser.CurrentDirectory = std::filesystem::current_path(Error);
    if (Browser.CurrentDirectory.empty())
        Browser.CurrentDirectory = std::filesystem::path(".");
    Browser.RequestOpen = true;
}

void DrawBrowser(BrowserState& Browser, PathBuffers& Buffers)
{
    if (Browser.RequestOpen)
    {
        ImGui::OpenPopup("选择 SKRTG 路径");
        Browser.RequestOpen = false;
    }
    ImGui::SetNextWindowSize({760.0F, 560.0F}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(
            "选择 SKRTG 路径", nullptr,
            ImGuiWindowFlags_NoSavedSettings))
        return;

    ImGui::TextWrapped(
        "%s", PathToUtf8(Browser.CurrentDirectory).c_str());
#if defined(_WIN32)
    for (const char Drive : {'C', 'D', 'E', 'F', 'G'})
    {
        const std::filesystem::path Root(
            std::string(1, Drive) + ":\\");
        std::error_code Error;
        if (std::filesystem::exists(Root, Error) && !Error)
        {
            if (ImGui::Button((std::string(1, Drive) + ":/").c_str()))
            {
                Browser.CurrentDirectory = Root;
                Browser.Selected.clear();
            }
            ImGui::SameLine();
        }
    }
    ImGui::NewLine();
#endif
    if (ImGui::Button("上一级") &&
        Browser.CurrentDirectory.has_parent_path())
    {
        Browser.CurrentDirectory =
            Browser.CurrentDirectory.parent_path();
        Browser.Selected.clear();
    }
    ImGui::SameLine();
    if (Browser.Selection == BrowseSelection::Directory &&
        ImGui::Button("使用当前文件夹"))
    {
        SetBuffer(
            BufferFor(Buffers, Browser.Target),
            Browser.CurrentDirectory);
        ImGui::CloseCurrentPopup();
    }
    if (Browser.Selection == BrowseSelection::SkrvPackage &&
        IsSkrvPackage(Browser.CurrentDirectory))
    {
        ImGui::SameLine();
        if (ImGui::Button("打开当前 SKRV"))
        {
            SetBuffer(
                BufferFor(Buffers, Browser.Target),
                Browser.CurrentDirectory);
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::Separator();
    std::vector<std::filesystem::directory_entry> Entries;
    std::error_code Error;
    std::filesystem::directory_iterator Iterator(
        Browser.CurrentDirectory,
        std::filesystem::directory_options::skip_permission_denied,
        Error);
    const std::filesystem::directory_iterator End;
    while (!Error && Iterator != End && Entries.size() < 4096)
    {
        Entries.push_back(*Iterator);
        Iterator.increment(Error);
    }
    if (Error)
        Browser.Error = "无法读取此文件夹。";
    std::sort(
        Entries.begin(), Entries.end(),
        [](const auto& Left, const auto& Right)
        {
            std::error_code LeftError;
            std::error_code RightError;
            const bool LeftDirectory = Left.is_directory(LeftError);
            const bool RightDirectory = Right.is_directory(RightError);
            if (LeftDirectory != RightDirectory) return LeftDirectory;
            return PathToUtf8(Left.path().filename()) <
                PathToUtf8(Right.path().filename());
        });

    ImGui::BeginChild(
        "PathEntries", {0.0F, -84.0F}, ImGuiChildFlags_Borders);
    for (const std::filesystem::directory_entry& Entry : Entries)
    {
        std::error_code EntryError;
        const bool Directory = Entry.is_directory(EntryError);
        const bool File = Entry.is_regular_file(EntryError);
        const bool Skrv = Directory && IsSkrvPackage(Entry.path());
        const bool Fbx = File && IsFbx(Entry.path());
        const bool Json = File && IsJson(Entry.path());
        const bool CharacterProfile =
            File && IsCharacterProfilePackage(Entry.path());
        if (EntryError || (!Directory &&
            ((Browser.Selection == BrowseSelection::FbxFile && !Fbx) ||
             (Browser.Selection == BrowseSelection::JsonFile && !Json) ||
             (Browser.Selection ==
                  BrowseSelection::CharacterProfile &&
              !CharacterProfile) ||
             (Browser.Selection != BrowseSelection::FbxFile &&
              Browser.Selection != BrowseSelection::JsonFile &&
              Browser.Selection !=
                  BrowseSelection::CharacterProfile))))
            continue;
        const std::string Name =
            (Skrv && Browser.Selection == BrowseSelection::SkrvPackage
                ? "[SKRV] "
                : (Directory
                    ? "[目录] "
                    : (CharacterProfile
                        ? "[PROFILE] "
                        : (Json ? "[JSON] " : "[FBX] ")))) +
            PathToUtf8(Entry.path().filename());
        const bool Selected = Browser.Selected == Entry.path();
        if (ImGui::Selectable(
                Name.c_str(), Selected,
                ImGuiSelectableFlags_AllowDoubleClick))
        {
            if (Directory)
            {
                Browser.Selected = Entry.path();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (Skrv &&
                        Browser.Selection == BrowseSelection::SkrvPackage)
                    {
                        SetBuffer(
                            BufferFor(Buffers, Browser.Target), Entry.path());
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        Browser.CurrentDirectory = Entry.path();
                        Browser.Selected.clear();
                    }
                }
            }
            else
            {
                Browser.Selected = Entry.path();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    SetBuffer(
                        BufferFor(Buffers, Browser.Target), Entry.path());
                    ImGui::CloseCurrentPopup();
                }
            }
        }
    }
    ImGui::EndChild();
    if (!Browser.Error.empty())
        ImGui::TextColored({1.0F, 0.45F, 0.45F, 1.0F},
                           "%s", Browser.Error.c_str());
    const bool AcceptedSelection = !Browser.Selected.empty() &&
        ((Browser.Selection == BrowseSelection::FbxFile &&
          IsFbx(Browser.Selected)) ||
         (Browser.Selection == BrowseSelection::JsonFile &&
          IsJson(Browser.Selected)) ||
         (Browser.Selection == BrowseSelection::CharacterProfile &&
          IsCharacterProfilePackage(Browser.Selected)) ||
         (Browser.Selection == BrowseSelection::SkrvPackage &&
          IsSkrvPackage(Browser.Selected)) ||
         (Browser.Selection == BrowseSelection::Directory &&
          IsDirectory(Browser.Selected)));
    ImGui::BeginDisabled(!AcceptedSelection);
    const char* SelectLabel =
        Browser.Selection == BrowseSelection::FbxFile ? "选择 FBX" :
        Browser.Selection == BrowseSelection::JsonFile ? "选择 JSON" :
        Browser.Selection == BrowseSelection::CharacterProfile
            ? "选择角色包" :
        Browser.Selection == BrowseSelection::SkrvPackage ? "打开 SKRV" :
        "选择文件夹";
    if (ImGui::Button(SelectLabel))
    {
        SetBuffer(
            BufferFor(Buffers, Browser.Target), Browser.Selected);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("取消")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawPathRow(
    const char* Label,
    const char* Id,
    std::array<char, PathCapacity>& Buffer,
    BrowserState& Browser,
    const BrowseTarget Target,
    const BrowseSelection Selection)
{
    ImGui::TextUnformatted(Label);
    ImGui::SetNextItemWidth(-86.0F);
    ImGui::InputText(Id, Buffer.data(), Buffer.size());
    ImGui::SameLine();
    const std::string ButtonId = std::string("浏览##") + Id;
    if (ImGui::Button(ButtonId.c_str()))
        BeginBrowse(Browser, Target, Selection, BufferPath(Buffer));
}
} // namespace

struct RetargetBridgeUi::Impl
{
    explicit Impl(const std::filesystem::path& ViewerExecutable)
        : Tools(DiscoverRetargetBridgeTools(ViewerExecutable)),
          BatchExecutable(
              DiscoverBatchRetargetExecutable(ViewerExecutable))
    {
        const std::filesystem::path ViewerDirectory =
            std::filesystem::absolute(ViewerExecutable).parent_path();
        std::filesystem::path Repository = RepositoryFromTools(Tools);
        if (Repository.empty())
        {
            Repository =
                (ViewerDirectory / ".." / "..").lexically_normal();
        }
        const std::filesystem::path RunRoot =
            DefaultRunRoot(Repository);
        SetBuffer(Buffers.SourceRest, {});
        SetBuffer(Buffers.TargetSkeleton, {});
        SetBuffer(Buffers.SourceAnimation, {});
        SetBuffer(
            Buffers.OutputRoot,
            RunRoot / "Single");
        SetBuffer(Buffers.BatchAnimationDirectory, {});
        SetBuffer(
            Buffers.BatchOutputRoot,
            RunRoot / "Batch");
        SetBuffer(Buffers.ExportRoot, Repository / "Exports");
        SetBuffer(Buffers.SourceRigJson, {});
        SetBuffer(Buffers.TargetRigJson, {});
        SetBuffer(Buffers.SourceAlignmentRetargeterJson, {});
        SetBuffer(Buffers.TargetAlignmentRetargeterJson, {});
        SetBuffer(Buffers.AnimationStack, {});
        SetBuffer(Buffers.CharacterProfilePackage, {});

        CatalogFile = DiscoverRetargetAssetCatalog(ViewerExecutable);
        ProfileStoreRoot = profile::DefaultCharacterProfileStore();
        ReloadCatalogAndProfiles(true);
    }

    const RetargetSkeletonAsset* SelectedSourceSkeleton() const
    {
        if (SourceSkeletonIndex < 0 ||
            static_cast<std::size_t>(SourceSkeletonIndex) >=
                Catalog.Skeletons.size())
        {
            return nullptr;
        }
        return &Catalog.Skeletons[
            static_cast<std::size_t>(SourceSkeletonIndex)];
    }

    const RetargetSkeletonAsset* SelectedTargetSkeleton() const
    {
        if (TargetSkeletonIndex < 0 ||
            static_cast<std::size_t>(TargetSkeletonIndex) >=
                Catalog.Skeletons.size())
        {
            return nullptr;
        }
        return &Catalog.Skeletons[
            static_cast<std::size_t>(TargetSkeletonIndex)];
    }

    const RetargetAnimationAsset* SelectedAnimation() const
    {
        if (AnimationIndex < 0 ||
            static_cast<std::size_t>(AnimationIndex) >=
                Catalog.Animations.size())
        {
            return nullptr;
        }
        return &Catalog.Animations[
            static_cast<std::size_t>(AnimationIndex)];
    }

    const profile::InstalledCharacterProfile* ActiveProfileFor(
        const std::string& ProfileId) const
    {
        const auto Found = std::find_if(
            ActiveProfiles.begin(), ActiveProfiles.end(),
            [&](const profile::InstalledCharacterProfile& Candidate)
            {
                return Candidate.Profile.ProfileId == ProfileId;
            });
        return Found == ActiveProfiles.end() ? nullptr : &*Found;
    }

    void ReloadCatalogAndProfiles(const bool SelectFirst)
    {
        const RetargetSkeletonAsset* PreviousSource =
            SelectedSourceSkeleton();
        const RetargetSkeletonAsset* PreviousTarget =
            SelectedTargetSkeleton();
        const RetargetAnimationAsset* PreviousAnimation =
            SelectedAnimation();
        const std::string PreviousSourceId =
            PreviousSource != nullptr ? PreviousSource->Id : "";
        const std::string PreviousTargetId =
            PreviousTarget != nullptr ? PreviousTarget->Id : "";
        const std::string PreviousAnimationId =
            PreviousAnimation != nullptr ? PreviousAnimation->Id : "";

        Catalog = {};
        CatalogErrors.clear();
        CatalogWarnings.clear();
        ProfileErrors.clear();
        ProfileWarnings.clear();
        InstalledProfiles.clear();
        ActiveProfiles.clear();

        profile::ProfileDiscoveryResult Discovery =
            profile::DiscoverInstalledCharacterProfiles(
                ProfileStoreRoot);
        ProfileErrors = std::move(Discovery.Errors);
        ProfileWarnings = std::move(Discovery.Warnings);
        InstalledProfiles = std::move(Discovery.Profiles);
        for (const profile::InstalledCharacterProfile& Installed :
             InstalledProfiles)
        {
            auto Existing = std::find_if(
                ActiveProfiles.begin(), ActiveProfiles.end(),
                [&](const profile::InstalledCharacterProfile& Candidate)
                {
                    return Candidate.Profile.ProfileId ==
                        Installed.Profile.ProfileId;
                });
            if (Existing == ActiveProfiles.end())
            {
                ActiveProfiles.push_back(Installed);
            }
            else if (profile::CompareCharacterProfileVersions(
                         Existing->Profile.ProfileVersion,
                         Installed.Profile.ProfileVersion) < 0)
            {
                *Existing = Installed;
            }
        }

        std::vector<std::string> ExternalSkeletonIds;
        ExternalSkeletonIds.reserve(ActiveProfiles.size());
        for (const profile::InstalledCharacterProfile& Installed :
             ActiveProfiles)
        {
            ExternalSkeletonIds.push_back(
                Installed.Profile.ProfileId);
        }
        RetargetAssetCatalogLoadResult CatalogResult =
            LoadRetargetAssetCatalog(
                CatalogFile, true, ExternalSkeletonIds);
        CatalogErrors = std::move(CatalogResult.Errors);
        CatalogWarnings = std::move(CatalogResult.Warnings);
        if (CatalogResult.Success)
        {
            Catalog = std::move(CatalogResult.Catalog);
        }
        else if (!UseUEIKJsonRoute)
        {
            Catalog.CatalogId = "profile_store";
            Catalog.CatalogFile =
                std::filesystem::absolute(CatalogFile)
                    .lexically_normal();
            Catalog.AssetRoot = Catalog.CatalogFile.parent_path();
        }

        for (const profile::InstalledCharacterProfile& Installed :
             ActiveProfiles)
        {
            RetargetSkeletonAsset Skeleton;
            Skeleton.Id = Installed.Profile.ProfileId;
            Skeleton.Label = Installed.Profile.DisplayName +
                "  [profile " +
                Installed.Profile.ProfileVersion + "]";
            Skeleton.SkeletonSignatureSha256 =
                Installed.Profile.SkeletonSignatureSha256;
            Skeleton.RestFbx =
                profile::InstalledProfileResourcePath(
                    Installed, Installed.Profile.RestFbx);
            Skeleton.RestFbxSha256 =
                Installed.Profile.RestFbx.Sha256;
            Skeleton.IkRigJson =
                profile::InstalledProfileResourcePath(
                    Installed, Installed.Profile.IkRigJson);
            Skeleton.IkRigJsonSha256 =
                Installed.Profile.IkRigJson.Sha256;
            Skeleton.AlignmentRetargeterJson =
                profile::InstalledProfileResourcePath(
                    Installed,
                    Installed.Profile.AlignmentRetargeterJson);
            Skeleton.AlignmentRetargeterJsonSha256 =
                Installed.Profile.AlignmentRetargeterJson.Sha256;
            Skeleton.SourceEnabled =
                Installed.Profile.SourceEnabled;
            Skeleton.TargetEnabled =
                Installed.Profile.TargetEnabled;
            auto Existing = std::find_if(
                Catalog.Skeletons.begin(), Catalog.Skeletons.end(),
                [&](const RetargetSkeletonAsset& Candidate)
                {
                    return Candidate.Id == Skeleton.Id;
                });
            if (Existing == Catalog.Skeletons.end())
                Catalog.Skeletons.push_back(std::move(Skeleton));
            else
                *Existing = std::move(Skeleton);
        }

        SourceSkeletonIndex = -1;
        TargetSkeletonIndex = -1;
        AnimationIndex = -1;
        for (std::size_t Index = 0;
             Index < Catalog.Skeletons.size(); ++Index)
        {
            const RetargetSkeletonAsset& Skeleton =
                Catalog.Skeletons[Index];
            if (Skeleton.SourceEnabled &&
                ((SourceSkeletonIndex < 0 && PreviousSourceId.empty()) ||
                 Skeleton.Id == PreviousSourceId))
            {
                SourceSkeletonIndex = static_cast<int>(Index);
                if (Skeleton.Id == PreviousSourceId) break;
            }
        }
        for (std::size_t Index = 0;
             Index < Catalog.Skeletons.size(); ++Index)
        {
            const RetargetSkeletonAsset& Skeleton =
                Catalog.Skeletons[Index];
            if (Skeleton.TargetEnabled &&
                ((TargetSkeletonIndex < 0 && PreviousTargetId.empty()) ||
                 Skeleton.Id == PreviousTargetId))
            {
                TargetSkeletonIndex = static_cast<int>(Index);
                if (Skeleton.Id == PreviousTargetId) break;
            }
        }
        if (SourceSkeletonIndex < 0 || TargetSkeletonIndex < 0)
        {
            for (std::size_t Index = 0;
                 Index < Catalog.Skeletons.size(); ++Index)
            {
                const RetargetSkeletonAsset& Skeleton =
                    Catalog.Skeletons[Index];
                if (SourceSkeletonIndex < 0 &&
                    Skeleton.SourceEnabled)
                    SourceSkeletonIndex = static_cast<int>(Index);
                if (TargetSkeletonIndex < 0 &&
                    Skeleton.TargetEnabled)
                    TargetSkeletonIndex = static_cast<int>(Index);
            }
        }

        CatalogLoaded = !Catalog.Skeletons.empty();
        if (CatalogLoaded)
        {
            UseUEIKJsonRoute = true;
            RefreshCompatibleAnimations(false);
            for (const std::size_t Index :
                 CompatibleAnimationIndices)
            {
                if (Catalog.Animations[Index].Id ==
                    PreviousAnimationId)
                {
                    AnimationIndex = static_cast<int>(Index);
                    break;
                }
            }
            if (AnimationIndex < 0 && SelectFirst &&
                !CompatibleAnimationIndices.empty())
            {
                AnimationIndex = static_cast<int>(
                    CompatibleAnimationIndices.front());
            }
            Message =
                "角色包与动画目录已刷新；动画只会显示当前源骨骼的绑定项。";
        }
        else
        {
            CompatibleAnimationIndices.clear();
            Message =
                "尚无可用角色；请安装 .skrtgprofile 或配置资产目录。";
        }
    }

    void InstallSelectedProfile()
    {
        const std::filesystem::path Package =
            BufferPath(Buffers.CharacterProfilePackage)
                .lexically_normal();
        const profile::ProfileInstallResult Result =
            profile::InstallCharacterProfilePackage(
                Package, ProfileStoreRoot);
        if (!Result.Success)
        {
            ProfileMessage = Result.Errors.empty()
                ? "角色包安装失败。"
                : "角色包安装失败：" + Result.Errors.front();
            return;
        }
        const std::string InstalledId =
            Result.Installed.Profile.ProfileId;
        const std::string InstalledVersion =
            Result.Installed.Profile.ProfileVersion;
        ReloadCatalogAndProfiles(true);
        ProfileMessage = Result.AlreadyInstalled
            ? "角色包已经安装且哈希一致：" +
                InstalledId + " " + InstalledVersion
            : "角色包安装完成：" +
                InstalledId + " " + InstalledVersion;
    }

    void RefreshCompatibleAnimations(const bool SelectFirst)
    {
        CompatibleAnimationIndices.clear();
        BatchSelectedAnimationIds.clear();
        AnimationIndex = -1;
        const RetargetSkeletonAsset* Source =
            SelectedSourceSkeleton();
        if (Source == nullptr) return;
        CompatibleAnimationIndices =
            CompatibleRetargetAnimationIndices(
                Catalog, Source->Id);
        for (const std::size_t Index :
             CompatibleAnimationIndices)
        {
            BatchSelectedAnimationIds.insert(
                Catalog.Animations[Index].Id);
        }
        if (SelectFirst && !CompatibleAnimationIndices.empty())
        {
            AnimationIndex = static_cast<int>(
                CompatibleAnimationIndices.front());
        }
    }

    RetargetAssetSelectionValidation ValidateCatalogSelection() const
    {
        if (!CatalogLoaded)
        {
            RetargetAssetSelectionValidation Result;
            Result.Errors.push_back(
                "retarget asset catalog is unavailable");
            return Result;
        }
        const RetargetSkeletonAsset* Source =
            SelectedSourceSkeleton();
        const RetargetSkeletonAsset* Target =
            SelectedTargetSkeleton();
        const RetargetAnimationAsset* Animation =
            SelectedAnimation();
        return ValidateRetargetAssetSelection(
            Catalog,
            Source != nullptr ? Source->Id : std::string(),
            Animation != nullptr ? Animation->Id : std::string(),
            Target != nullptr ? Target->Id : std::string());
    }

    RetargetBridgeRequest BuildRequest()
    {
        RetargetBridgeRequest Request;
        Request.RouteKind = UseUEIKJsonRoute
            ? RetargetBridgeRouteKind::UEIKJsonV1
            : RetargetBridgeRouteKind::ExternalFoundationV1;
        CatalogSelectionErrors.clear();
        if (UseUEIKJsonRoute && CatalogLoaded)
        {
            const RetargetSkeletonAsset* Source =
                SelectedSourceSkeleton();
            const RetargetSkeletonAsset* Target =
                SelectedTargetSkeleton();
            const RetargetAnimationAsset* Animation =
                SelectedAnimation();
            ApplyRetargetAssetSelection(
                Catalog,
                Source != nullptr ? Source->Id : std::string(),
                Animation != nullptr ? Animation->Id : std::string(),
                Target != nullptr ? Target->Id : std::string(),
                Request, CatalogSelectionErrors);
            if (Source != nullptr)
            {
                if (const profile::InstalledCharacterProfile*
                        Installed = ActiveProfileFor(Source->Id))
                {
                    Request.AssetBinding.SourceProfilePackage =
                        Installed->PackagePath;
                    Request.AssetBinding.SourceProfilePackageSha256 =
                        Installed->PackageSha256;
                    Request.AssetBinding.SourceProfileVersion =
                        Installed->Profile.ProfileVersion;
                }
            }
            if (Target != nullptr)
            {
                if (const profile::InstalledCharacterProfile*
                        Installed = ActiveProfileFor(Target->Id))
                {
                    Request.AssetBinding.TargetProfilePackage =
                        Installed->PackagePath;
                    Request.AssetBinding.TargetProfilePackageSha256 =
                        Installed->PackageSha256;
                    Request.AssetBinding.TargetProfileVersion =
                        Installed->Profile.ProfileVersion;
                }
            }
        }
        else
        {
            Request.SourceAnimationFbx =
                BufferPath(Buffers.SourceAnimation).lexically_normal();
            Request.TargetSkeletonFbx =
                BufferPath(Buffers.TargetSkeleton).lexically_normal();
            Request.SourceRestFbx = BufferPath(Buffers.SourceRest);
            if (Request.SourceRestFbx.empty())
                Request.SourceRestFbx = Tools.DefaultSourceRestFbx;
            Request.SourceRestFbx =
                Request.SourceRestFbx.lexically_normal();
            Request.SourceRigJson =
                BufferPath(Buffers.SourceRigJson).lexically_normal();
            Request.TargetRigJson =
                BufferPath(Buffers.TargetRigJson).lexically_normal();
            Request.SourceAlignmentRetargeterJson =
                BufferPath(Buffers.SourceAlignmentRetargeterJson)
                    .lexically_normal();
            Request.TargetAlignmentRetargeterJson =
                BufferPath(Buffers.TargetAlignmentRetargeterJson)
                    .lexically_normal();
            Request.AnimationStack = Buffers.AnimationStack.data();
        }
        const std::filesystem::path OutputRoot =
            BufferPath(Buffers.OutputRoot).lexically_normal();
        Request.ClipId = MakeBridgeClipId(Request.SourceAnimationFbx);
        const RetargetAnimationAsset* Animation =
            SelectedAnimation();
        const RetargetSkeletonAsset* Target =
            SelectedTargetSkeleton();
        Request.ClipLabel =
            UseUEIKJsonRoute && CatalogLoaded &&
                Animation != nullptr && Target != nullptr
            ? Animation->Label + " → " + Target->Label
            : PathToUtf8(Request.SourceAnimationFbx.stem());
        Request.OutputDirectory = OutputRoot /
            (Timestamp() + "_" + Request.ClipId);
        int Suffix = 1;
        std::error_code Error;
        while (std::filesystem::exists(Request.OutputDirectory, Error) &&
               !Error)
        {
            Request.OutputDirectory = OutputRoot /
                (Timestamp() + "_" + Request.ClipId + "_" +
                 std::to_string(Suffix++));
        }
        Request.Tools = Tools;
        Request.EnableSpinePelvisFollow =
            !UseUEIKJsonRoute && SpinePelvisFollow;
        Request.EnableSourceMotionFootLock =
            !UseUEIKJsonRoute && SourceMotionFootLock;
        return Request;
    }

    void Validate()
    {
        LastRequest = BuildRequest();
        LastPreflight = PreflightRetargetBridge(LastRequest);
        if (!CatalogSelectionErrors.empty())
        {
            LastPreflight.Success = false;
            LastPreflight.Errors.insert(
                LastPreflight.Errors.begin(),
                CatalogSelectionErrors.begin(),
                CatalogSelectionErrors.end());
        }
        Message = LastPreflight.Success
            ? "预检通过：骨骼—动画对应、资产哈希、工具与输出策略均已验证。"
            : "预检失败：未启动进程，也未生成输出。";
    }

    void Start()
    {
        Validate();
        if (!LastPreflight.Success || Process.IsActive()) return;
        const std::filesystem::path OutputRoot =
            LastRequest.OutputDirectory.parent_path();
        const std::string Base =
            LastRequest.OutputDirectory.filename().string();
        ActiveRequestJson = OutputRoot / (Base + ".bridge_request.json");
        ActiveLauncherLog = OutputRoot / (Base + ".bridge_launcher.log");
        std::string Error;
        if (!WriteRetargetBridgeRequest(
                LastRequest, ActiveRequestJson, Error))
        {
            Message = "写入 Bridge 请求失败：" + Error;
            return;
        }
        if (!Process.Start({
                Tools.BridgeExecutable,
                {"--request", PathToUtf8(ActiveRequestJson)},
                OutputRoot,
                ActiveLauncherLog}, Error))
        {
            Message = "启动 Bridge 子进程失败：" + Error;
            return;
        }
        ActiveRun = ActiveRunKind::Single;
        Message = "重定向正在独立进程中运行……";
    }

    BatchRetargetRequest BuildBatchRequest()
    {
        BatchRetargetRequest Request;
        Request.SourceCharacter.DefinitionKind = UseUEIKJsonRoute
            ? "ue_ik_json_v1"
            : "external_foundation_v1";
        Request.TargetCharacter.DefinitionKind =
            Request.SourceCharacter.DefinitionKind;
        if (UseUEIKJsonRoute && CatalogLoaded)
        {
            const RetargetSkeletonAsset* Source =
                SelectedSourceSkeleton();
            const RetargetSkeletonAsset* Target =
                SelectedTargetSkeleton();
            if (Source != nullptr)
            {
                Request.SourceCharacter.RestFbx =
                    Source->RestFbx;
                Request.SourceCharacter.DefinitionFile =
                    Source->IkRigJson;
                Request.SourceCharacter
                    .AlignmentRetargeterFile =
                        Source->AlignmentRetargeterJson;
            }
            if (Target != nullptr)
            {
                Request.TargetCharacter.RestFbx =
                    Target->RestFbx;
                Request.TargetCharacter.DefinitionFile =
                    Target->IkRigJson;
                Request.TargetCharacter
                    .AlignmentRetargeterFile =
                        Target->AlignmentRetargeterJson;
            }

            RetargetBridgeAssetBinding& Binding =
                Request.AssetBinding;
            Binding.Required = true;
            Binding.CatalogFile = Catalog.CatalogFile;
            Binding.CatalogSha256 = Catalog.CatalogSha256;
            Binding.CatalogId = Catalog.CatalogId;
            if (Source != nullptr)
            {
                Binding.SourceSkeletonId = Source->Id;
                Binding.SourceRestSha256 =
                    Source->RestFbxSha256;
                Binding.SourceRigJsonSha256 =
                    Source->IkRigJsonSha256;
                Binding.SourceAlignmentRetargeterJsonSha256 =
                    Source->AlignmentRetargeterJsonSha256;
                if (const profile::InstalledCharacterProfile*
                        Installed = ActiveProfileFor(Source->Id))
                {
                    Binding.SourceProfilePackage =
                        Installed->PackagePath;
                    Binding.SourceProfilePackageSha256 =
                        Installed->PackageSha256;
                    Binding.SourceProfileVersion =
                        Installed->Profile.ProfileVersion;
                }
            }
            if (Target != nullptr)
            {
                Binding.TargetSkeletonId = Target->Id;
                Binding.TargetRestSha256 =
                    Target->RestFbxSha256;
                Binding.TargetRigJsonSha256 =
                    Target->IkRigJsonSha256;
                Binding.TargetAlignmentRetargeterJsonSha256 =
                    Target->AlignmentRetargeterJsonSha256;
                if (const profile::InstalledCharacterProfile*
                        Installed = ActiveProfileFor(Target->Id))
                {
                    Binding.TargetProfilePackage =
                        Installed->PackagePath;
                    Binding.TargetProfilePackageSha256 =
                        Installed->PackageSha256;
                    Binding.TargetProfileVersion =
                        Installed->Profile.ProfileVersion;
                }
            }
            for (const std::size_t Index :
                 CompatibleAnimationIndices)
            {
                const RetargetAnimationAsset& Animation =
                    Catalog.Animations[Index];
                if (!BatchSelectedAnimationIds.contains(
                        Animation.Id))
                {
                    continue;
                }
                BatchCatalogAnimationInput Selected;
                Selected.AnimationId = Animation.Id;
                Selected.Label = Animation.Label;
                Selected.SourceSkeletonId =
                    Animation.SourceSkeletonId;
                Selected.SourceAnimationFbx = Animation.Fbx;
                Selected.SourceAnimationSha256 =
                    Animation.FbxSha256;
                Selected.SourceAnimationGoldenJson =
                    Animation.GoldenJson;
                Selected.SourceAnimationGoldenJsonSha256 =
                    Animation.GoldenJsonSha256;
                Selected.AnimationStack =
                    Animation.AnimationStack;
                Selected.SourceFbxImportMode =
                    Animation.SourceFbxImportMode;
                Selected.RestFbxImportMode =
                    Animation.RestFbxImportMode;
                Request.CatalogAnimations.push_back(
                    std::move(Selected));
            }
            Request.Recursive = false;
        }
        else if (!UseUEIKJsonRoute)
        {
            Request.SourceCharacter.RestFbx =
                BufferPath(Buffers.SourceRest)
                    .lexically_normal();
            Request.TargetCharacter.RestFbx =
                BufferPath(Buffers.TargetSkeleton)
                    .lexically_normal();
            Request.AnimationDirectory =
                BufferPath(Buffers.BatchAnimationDirectory)
                    .lexically_normal();
            Request.Recursive = BatchRecursive;
        }
        else
        {
            // Keep UE batch fail-closed when the profile/catalog layer is
            // unavailable. BuildBatchRetargetPlan will report the missing
            // bindings without falling back to loose paths.
            Request.AssetBinding.Required = true;
            Request.Recursive = false;
        }
        const std::filesystem::path OutputRoot =
            BufferPath(Buffers.BatchOutputRoot).lexically_normal();
        Request.OutputDirectory =
            OutputRoot / ("SKRTG_Batch_" + Timestamp());
        int Suffix = 1;
        std::error_code Error;
        while (std::filesystem::exists(Request.OutputDirectory, Error) &&
               !Error)
        {
            Request.OutputDirectory = OutputRoot /
                ("SKRTG_Batch_" + Timestamp() + "_" +
                 std::to_string(Suffix++));
        }
        Request.Tools = Tools;
        Request.EnableSpinePelvisFollow =
            !UseUEIKJsonRoute && SpinePelvisFollow;
        Request.EnableSourceMotionFootLock =
            !UseUEIKJsonRoute && SourceMotionFootLock;
        return Request;
    }

    void ValidateBatch()
    {
        LastBatchRequest = BuildBatchRequest();
        LastBatchPlan = BuildBatchRetargetPlan(LastBatchRequest);
        BatchMessage = LastBatchPlan.Success
            ? "批量预检通过：发现 " +
                std::to_string(LastBatchPlan.Jobs.size()) +
                " 个动画；执行并发固定为 1。"
            : "批量预检失败：未启动进程，也未写入目标文件夹。";
    }

    void StartBatch()
    {
        ValidateBatch();
        if (!LastBatchPlan.Success || Process.IsActive()) return;
        if (BatchExecutable.empty())
        {
            BatchMessage =
                "批量协调器不可用：当前 EXE 旁未找到 skrtg_batch_retarget。";
            return;
        }
        const std::filesystem::path OutputRoot =
            LastBatchRequest.OutputDirectory.parent_path();
        const std::string Base =
            LastBatchRequest.OutputDirectory.filename().string();
        ActiveBatchRequestJson =
            OutputRoot / (Base + ".batch_request.json");
        ActiveBatchLauncherLog =
            OutputRoot / (Base + ".batch_launcher.log");
        ActiveBatchStatusJson =
            LastBatchRequest.OutputDirectory / "batch_status.json";
        std::string Error;
        if (!WriteBatchRetargetRequest(
                LastBatchRequest, ActiveBatchRequestJson, Error))
        {
            BatchMessage = "写入批量请求失败：" + Error;
            return;
        }
        if (!Process.Start({
                BatchExecutable,
                {"--request", PathToUtf8(ActiveBatchRequestJson)},
                OutputRoot,
                ActiveBatchLauncherLog}, Error))
        {
            BatchMessage = "启动批量协调器失败：" + Error;
            return;
        }
        LastBatchStatus = {};
        HasBatchStatus = false;
        BatchStatusReadError.clear();
        ActiveRun = ActiveRunKind::Batch;
        NextBatchStatusPoll = std::chrono::steady_clock::now();
        BatchMessage =
            "批量重定向已启动：低内存流式模式，并发 1；一条完成并释放后才处理下一条。";
    }

    bool RefreshBatchStatus()
    {
        std::string Error;
        BatchRetargetStatus Status;
        if (ReadBatchRetargetStatus(
                ActiveBatchStatusJson, Status, Error))
        {
            LastBatchStatus = std::move(Status);
            HasBatchStatus = true;
            BatchStatusReadError.clear();
            return true;
        }
        std::error_code FileError;
        if (std::filesystem::exists(ActiveBatchStatusJson, FileError))
        {
            BatchStatusReadError = FileError
                ? "无法检查批次状态文件：" + FileError.message()
                : "批次状态读取失败，界面保留上一次有效快照：" + Error;
        }
        return false;
    }

    void CancelBatch()
    {
        if (ActiveRun != ActiveRunKind::Batch || !Process.IsActive())
            return;
        std::string Error;
        const bool Terminated = Process.Terminate(Error);
        const bool Refreshed = RefreshBatchStatus();
        if (!HasBatchStatus)
        {
            LastBatchStatus.MaximumConcurrentJobs = 1;
            LastBatchStatus.TotalJobs = LastBatchPlan.Jobs.size();
            LastBatchStatus.SourceCharacter =
                LastBatchRequest.SourceCharacter;
            LastBatchStatus.TargetCharacter =
                LastBatchRequest.TargetCharacter;
            LastBatchStatus.AnimationDirectory =
                LastBatchRequest.AnimationDirectory;
            LastBatchStatus.OutputDirectory =
                LastBatchRequest.OutputDirectory;
            LastBatchStatus.Recursive = LastBatchRequest.Recursive;
            LastBatchStatus.EnableSpinePelvisFollow =
                LastBatchRequest.EnableSpinePelvisFollow;
            LastBatchStatus.EnableSourceMotionFootLock =
                LastBatchRequest.EnableSourceMotionFootLock;
            LastBatchStatus.AssetBinding =
                LastBatchRequest.AssetBinding;
            LastBatchStatus.Jobs = LastBatchPlan.Jobs;
        }
        LastBatchStatus.Running = !Terminated;
        LastBatchStatus.Complete = false;
        LastBatchStatus.Success = false;
        LastBatchStatus.Cancelled = Terminated;
        LastBatchStatus.Errors.push_back(Terminated
            ? "cancelled by user from Native Viewer"
            : "Viewer requested cancellation but process-tree termination was not confirmed: " +
                Error);
        std::string StatusError;
        std::error_code FileError;
        const bool StatusExists =
            std::filesystem::exists(ActiveBatchStatusJson, FileError) &&
            !FileError;
        if ((Refreshed || !StatusExists) &&
            !WriteBatchRetargetStatus(
                LastBatchStatus, ActiveBatchStatusJson, StatusError))
        {
            BatchStatusReadError =
                "无法写入取消状态；原状态已保留：" + StatusError;
        }
        ActiveRun = ActiveRunKind::None;
        BatchMessage = Terminated
            ? "批处理已停止；当前 Worker 及其子进程已一并终止，已完成输出保留。"
            : "停止批处理时发生错误：" + Error;
    }

    RetargetBridgeTools Tools;
    std::filesystem::path BatchExecutable;
    std::filesystem::path CatalogFile;
    std::filesystem::path ProfileStoreRoot;
    RetargetAssetCatalog Catalog;
    std::vector<profile::InstalledCharacterProfile>
        InstalledProfiles;
    std::vector<profile::InstalledCharacterProfile>
        ActiveProfiles;
    bool CatalogLoaded = false;
    int SourceSkeletonIndex = -1;
    int TargetSkeletonIndex = -1;
    int AnimationIndex = -1;
    std::vector<std::size_t> CompatibleAnimationIndices;
    std::set<std::string> BatchSelectedAnimationIds;
    std::vector<std::string> CatalogErrors;
    std::vector<std::string> CatalogWarnings;
    std::vector<std::string> ProfileErrors;
    std::vector<std::string> ProfileWarnings;
    std::vector<std::string> CatalogSelectionErrors;
    PathBuffers Buffers;
    BrowserState Browser;
    bool Open = false;
    bool BatchOpen = false;
    ChildProcess Process;
    ActiveRunKind ActiveRun = ActiveRunKind::None;
    RetargetBridgeRequest LastRequest;
    RetargetBridgePreflight LastPreflight;
    std::filesystem::path ActiveRequestJson;
    std::filesystem::path ActiveLauncherLog;
    BatchRetargetRequest LastBatchRequest;
    BatchRetargetPlan LastBatchPlan;
    BatchRetargetStatus LastBatchStatus;
    bool HasBatchStatus = false;
    std::string BatchStatusReadError;
    std::filesystem::path ActiveBatchRequestJson;
    std::filesystem::path ActiveBatchLauncherLog;
    std::filesystem::path ActiveBatchStatusJson;
    std::chrono::steady_clock::time_point NextBatchStatusPoll{};
    std::optional<std::filesystem::path> CompletedPackage;
    std::optional<std::filesystem::path> RequestedPackage;
    bool OpenSkrvDialog = false;
    bool ExportDialog = false;
    bool ExportOverwrite = false;
    bool UseUEIKJsonRoute = false;
    bool SpinePelvisFollow = true;
    bool SourceMotionFootLock = true;
    bool BatchRecursive = true;
    std::filesystem::path ExportSourceFbx;
    std::filesystem::path ExportProtectedPackageDirectory;
    std::string ExportExpectedSha256;
    std::array<char, PathCapacity> ExportFileName{};
    std::string ExportMessage;
    std::string BatchMessage;
    std::string Message;
    std::string ProfileMessage;
    int PendingProfileDeleteIndex = -1;
    int CompletedRuns = 0;
};

RetargetBridgeUi::RetargetBridgeUi(
    const std::filesystem::path& ViewerExecutable)
    : State(std::make_unique<Impl>(ViewerExecutable)) {}

RetargetBridgeUi::~RetargetBridgeUi() = default;
RetargetBridgeUi::RetargetBridgeUi(RetargetBridgeUi&&) noexcept = default;
RetargetBridgeUi& RetargetBridgeUi::operator=(
    RetargetBridgeUi&&) noexcept = default;

void RetargetBridgeUi::Open() { State->Open = true; }

void RetargetBridgeUi::OpenBatch() { State->BatchOpen = true; }

void RetargetBridgeUi::OpenSkrvPicker()
{
    State->OpenSkrvDialog = true;
}

void RetargetBridgeUi::OpenExportDialog(
    const std::filesystem::path& SourceFbx,
    const std::filesystem::path& ProtectedPackageDirectory,
    const std::string& SuggestedFileName,
    const std::string& ExpectedSha256)
{
    State->ExportSourceFbx = SourceFbx;
    State->ExportProtectedPackageDirectory = ProtectedPackageDirectory;
    State->ExportExpectedSha256 = ExpectedSha256;
    SetBuffer(
        State->ExportFileName, PathFromUtf8(SuggestedFileName));
    State->ExportMessage.clear();
    State->ExportDialog = true;
}

void RetargetBridgeUi::Draw()
{
    if (State->Open)
    {
        ImGui::SetNextWindowSize({900.0F, 680.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(
                "导入与重定向 FBX##RetargetBridge",
                &State->Open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped(
                "选择重定向路线和输入。UE IK JSON 路线只读取从 UE 导出的 JSON，不读取 uasset，也不推断骨名；两条路线均在独立进程中运行并经过 SKRV v1 严格验证。");
            ImGui::SeparatorText(
                "角色包（.skrtgprofile v1）");
            ImGui::TextWrapped(
                "安装目录：%s",
                PathToUtf8(State->ProfileStoreRoot).c_str());
            DrawPathRow(
                "待安装角色包", "##CharacterProfilePackage",
                State->Buffers.CharacterProfilePackage,
                State->Browser,
                BrowseTarget::CharacterProfilePackage,
                BrowseSelection::CharacterProfile);
            const bool ProfilePackageSelected =
                IsCharacterProfilePackage(
                    BufferPath(
                        State->Buffers.CharacterProfilePackage));
            ImGui::BeginDisabled(!ProfilePackageSelected);
            if (ImGui::Button("校验并安装角色包"))
                State->InstallSelectedProfile();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("刷新角色包与动画目录"))
            {
                State->ReloadCatalogAndProfiles(false);
                State->ProfileMessage =
                    "角色包与动画目录已刷新。";
            }
            if (!State->ProfileMessage.empty())
                ImGui::TextWrapped(
                    "%s", State->ProfileMessage.c_str());
            bool OpenDeleteProfilePopup = false;
            if (ImGui::TreeNode(
                    "已安装角色包（同一角色仅启用最高版本）"))
            {
                if (State->InstalledProfiles.empty())
                    ImGui::TextDisabled("尚未安装角色包。");
                for (std::size_t Index = 0;
                     Index < State->InstalledProfiles.size();
                     ++Index)
                {
                    const profile::InstalledCharacterProfile&
                        Installed =
                            State->InstalledProfiles[Index];
                    const profile::InstalledCharacterProfile* Active =
                        State->ActiveProfileFor(
                            Installed.Profile.ProfileId);
                    const bool IsActive =
                        Active != nullptr &&
                        Active->PackageSha256 ==
                            Installed.PackageSha256;
                    ImGui::PushID(static_cast<int>(Index));
                    ImGui::Text(
                        "%s  %s%s",
                        Installed.Profile.DisplayName.c_str(),
                        Installed.Profile.ProfileVersion.c_str(),
                        IsActive ? "  [启用]" : "");
                    ImGui::TextDisabled(
                        "%s",
                        Installed.Profile.ProfileId.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("删除"))
                    {
                        State->PendingProfileDeleteIndex =
                            static_cast<int>(Index);
                        OpenDeleteProfilePopup = true;
                    }
                    ImGui::PopID();
                }
                for (const std::string& Warning :
                     State->ProfileWarnings)
                {
                    ImGui::TextColored(
                        {0.95F, 0.76F, 0.35F, 1.0F},
                        "- %s", Warning.c_str());
                }
                for (const std::string& Error :
                     State->ProfileErrors)
                {
                    ImGui::TextColored(
                        {1.0F, 0.45F, 0.45F, 1.0F},
                        "- %s", Error.c_str());
                }
                ImGui::TreePop();
            }
            if (OpenDeleteProfilePopup)
            {
                ImGui::OpenPopup(
                    "确认删除角色包##DeleteProfile");
            }
            if (ImGui::BeginPopupModal(
                    "确认删除角色包##DeleteProfile", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize))
            {
                const int Index =
                    State->PendingProfileDeleteIndex;
                if (Index >= 0 &&
                    static_cast<std::size_t>(Index) <
                        State->InstalledProfiles.size())
                {
                    const profile::InstalledCharacterProfile&
                        Installed =
                            State->InstalledProfiles[
                                static_cast<std::size_t>(Index)];
                    ImGui::TextWrapped(
                        "删除 %s %s？动画文件不会被删除。",
                        Installed.Profile.DisplayName.c_str(),
                        Installed.Profile.ProfileVersion.c_str());
                    if (ImGui::Button("确认删除"))
                    {
                        const profile::ProfileDeleteResult Deleted =
                            profile::DeleteInstalledCharacterProfile(
                                Installed,
                                State->ProfileStoreRoot);
                        State->ProfileMessage = Deleted.Success
                            ? "角色包已删除。"
                            : (Deleted.Errors.empty()
                                ? "角色包删除失败。"
                                : "角色包删除失败：" +
                                    Deleted.Errors.front());
                        State->PendingProfileDeleteIndex = -1;
                        if (Deleted.Success)
                            State->ReloadCatalogAndProfiles(false);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                }
                if (ImGui::Button("取消"))
                {
                    State->PendingProfileDeleteIndex = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::SeparatorText("重定向路线（必须由用户显式选择）");
            if (ImGui::RadioButton(
                    "外部 Foundation 兼容路线##ExternalFoundationRoute",
                    !State->UseUEIKJsonRoute))
            {
                State->UseUEIKJsonRoute = false;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(
                    "UE IK Rig JSON 候选路线##UEIKJsonRoute",
                    State->UseUEIKJsonRoute))
            {
                State->UseUEIKJsonRoute = true;
            }
            if (State->UseUEIKJsonRoute)
            {
                ImGui::TextColored(
                    {0.95F, 0.76F, 0.35F, 1.0F},
                    "候选路线：candidateRouteSelected=false / "
                    "candidateRouteAdopted=false；Foundation v1：frozen=true");
            }
            ImGui::Separator();
            if (State->UseUEIKJsonRoute)
            {
                ImGui::SeparatorText(
                    "源骨骼 → 对应动画 → 目标骨骼");
                if (State->CatalogLoaded)
                {
                    const RetargetSkeletonAsset* Source =
                        State->SelectedSourceSkeleton();
                    const char* SourcePreview = Source != nullptr
                        ? Source->Label.c_str()
                        : "请选择源骨骼";
                    ImGui::TextUnformatted("源骨骼");
                    ImGui::SetNextItemWidth(-1.0F);
                    if (ImGui::BeginCombo(
                            "##CatalogSourceSkeleton",
                            SourcePreview))
                    {
                        for (std::size_t Index = 0;
                             Index < State->Catalog.Skeletons.size();
                             ++Index)
                        {
                            const RetargetSkeletonAsset& Skeleton =
                                State->Catalog.Skeletons[Index];
                            if (!Skeleton.SourceEnabled) continue;
                            const bool Selected =
                                State->SourceSkeletonIndex ==
                                static_cast<int>(Index);
                            ImGui::PushID(Skeleton.Id.c_str());
                            if (ImGui::Selectable(
                                    Skeleton.Label.c_str(), Selected))
                            {
                                State->SourceSkeletonIndex =
                                    static_cast<int>(Index);
                                State->RefreshCompatibleAnimations(false);
                                State->Message =
                                    "源骨骼已切换；不兼容的旧动画已清除，请重新选择动画。";
                            }
                            if (Selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }

                    const RetargetAnimationAsset* Animation =
                        State->SelectedAnimation();
                    const char* AnimationPreview =
                        Animation != nullptr
                        ? Animation->Label.c_str()
                        : (State->CompatibleAnimationIndices.empty()
                            ? "该源骨骼暂无已绑定动画"
                            : "请选择动画");
                    ImGui::TextUnformatted("源动画（仅显示当前源骨骼）");
                    ImGui::BeginDisabled(
                        State->CompatibleAnimationIndices.empty());
                    ImGui::SetNextItemWidth(-1.0F);
                    if (ImGui::BeginCombo(
                            "##CatalogSourceAnimation",
                            AnimationPreview))
                    {
                        for (const std::size_t Index :
                             State->CompatibleAnimationIndices)
                        {
                            const RetargetAnimationAsset& Candidate =
                                State->Catalog.Animations[Index];
                            const bool Selected =
                                State->AnimationIndex ==
                                static_cast<int>(Index);
                            ImGui::PushID(Candidate.Id.c_str());
                            if (ImGui::Selectable(
                                    Candidate.Label.c_str(), Selected))
                            {
                                State->AnimationIndex =
                                    static_cast<int>(Index);
                                State->Message =
                                    "动画已绑定到当前源骨骼。";
                            }
                            if (Selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();

                    const RetargetSkeletonAsset* Target =
                        State->SelectedTargetSkeleton();
                    const char* TargetPreview = Target != nullptr
                        ? Target->Label.c_str()
                        : "请选择目标骨骼";
                    ImGui::TextUnformatted("目标骨骼");
                    ImGui::SetNextItemWidth(-1.0F);
                    if (ImGui::BeginCombo(
                            "##CatalogTargetSkeleton",
                            TargetPreview))
                    {
                        for (std::size_t Index = 0;
                             Index < State->Catalog.Skeletons.size();
                             ++Index)
                        {
                            const RetargetSkeletonAsset& Skeleton =
                                State->Catalog.Skeletons[Index];
                            if (!Skeleton.TargetEnabled) continue;
                            const bool Selected =
                                State->TargetSkeletonIndex ==
                                static_cast<int>(Index);
                            ImGui::PushID(Skeleton.Id.c_str());
                            if (ImGui::Selectable(
                                    Skeleton.Label.c_str(), Selected))
                            {
                                State->TargetSkeletonIndex =
                                    static_cast<int>(Index);
                            }
                            if (Selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::TextDisabled(
                        "当前源骨骼可选动画：%llu 个；列表不会显示其他骨骼的动画。",
                        static_cast<unsigned long long>(
                            State->CompatibleAnimationIndices.size()));
                    if (ImGui::TreeNode(
                            "实际绑定文件与 UE JSON（只读）"))
                    {
                        ImGui::TextWrapped(
                            "目录：%s",
                            PathToUtf8(State->CatalogFile).c_str());
                        if (Source != nullptr)
                        {
                            if (const profile::InstalledCharacterProfile*
                                    Installed =
                                        State->ActiveProfileFor(
                                            Source->Id))
                            {
                                ImGui::TextWrapped(
                                    "源角色包：%s",
                                    PathToUtf8(
                                        Installed->PackagePath)
                                        .c_str());
                            }
                            ImGui::TextWrapped(
                                "源 Rest：%s",
                                PathToUtf8(Source->RestFbx).c_str());
                            ImGui::TextWrapped(
                                "源 IK Rig：%s",
                                PathToUtf8(Source->IkRigJson).c_str());
                            ImGui::TextWrapped(
                                "源对齐 RTG：%s",
                                PathToUtf8(
                                    Source->AlignmentRetargeterJson)
                                    .c_str());
                        }
                        if (Animation != nullptr)
                        {
                            ImGui::TextWrapped(
                                "动画 FBX：%s",
                                PathToUtf8(Animation->Fbx).c_str());
                            ImGui::TextWrapped(
                                "动画 Golden：%s",
                                PathToUtf8(Animation->GoldenJson).c_str());
                        }
                        if (Target != nullptr)
                        {
                            if (const profile::InstalledCharacterProfile*
                                    Installed =
                                        State->ActiveProfileFor(
                                            Target->Id))
                            {
                                ImGui::TextWrapped(
                                    "目标角色包：%s",
                                    PathToUtf8(
                                        Installed->PackagePath)
                                        .c_str());
                            }
                            ImGui::TextWrapped(
                                "目标 Rest：%s",
                                PathToUtf8(Target->RestFbx).c_str());
                            ImGui::TextWrapped(
                                "目标 IK Rig：%s",
                                PathToUtf8(Target->IkRigJson).c_str());
                            ImGui::TextWrapped(
                                "目标对齐 RTG：%s",
                                PathToUtf8(
                                    Target->AlignmentRetargeterJson)
                                    .c_str());
                        }
                        ImGui::TreePop();
                    }
                    for (const std::string& Warning :
                         State->CatalogWarnings)
                    {
                        ImGui::TextColored(
                            {0.95F, 0.76F, 0.35F, 1.0F},
                            "- %s", Warning.c_str());
                    }
                    for (const std::string& Error :
                         State->CatalogErrors)
                    {
                        ImGui::TextColored(
                            {0.95F, 0.76F, 0.35F, 1.0F},
                            "动画目录：%s", Error.c_str());
                    }
                }
                else
                {
                    ImGui::TextColored(
                        {1.0F, 0.45F, 0.45F, 1.0F},
                        "资产目录加载失败；UE IK JSON 路线保持禁用。");
                    ImGui::TextWrapped(
                        "预期目录：%s",
                        PathToUtf8(State->CatalogFile).c_str());
                    for (const std::string& Error :
                         State->CatalogErrors)
                    {
                        ImGui::TextColored(
                            {1.0F, 0.45F, 0.45F, 1.0F},
                            "- %s", Error.c_str());
                    }
                }
            }
            else
            {
                DrawPathRow(
                    "源动画 FBX", "##SourceAnimation",
                    State->Buffers.SourceAnimation, State->Browser,
                    BrowseTarget::SourceAnimation,
                    BrowseSelection::FbxFile);
                DrawPathRow(
                    "目标角色 / 骨架 FBX", "##TargetSkeleton",
                    State->Buffers.TargetSkeleton, State->Browser,
                    BrowseTarget::TargetSkeleton,
                    BrowseSelection::FbxFile);
                DrawPathRow(
                    "源 Rest 骨架 FBX（可选；留空使用冻结默认）",
                    "##SourceRest", State->Buffers.SourceRest,
                    State->Browser, BrowseTarget::SourceRest,
                    BrowseSelection::FbxFile);
                if (ImGui::SmallButton(
                        "清空可选 Source Rest 路径"))
                {
                    State->Buffers.SourceRest.fill('\0');
                }
            }
            DrawPathRow(
                "输出根目录", "##OutputRoot",
                State->Buffers.OutputRoot, State->Browser,
                BrowseTarget::OutputRoot, BrowseSelection::Directory);

            ImGui::SeparatorText("本次运行的 Op Stack");
            ImGui::BeginDisabled(true);
            bool FoundationCore = true;
            ImGui::Checkbox(
                State->UseUEIKJsonRoute
                    ? "UE IK JSON 候选核心：FK + Pelvis + 解析式 Limb IK"
                    : "冻结核心：FK + Limb IK + Finger IK",
                &FoundationCore);
            ImGui::EndDisabled();
            if (State->UseUEIKJsonRoute)
            {
                ImGui::TextColored(
                    {0.95F, 0.76F, 0.35F, 1.0F},
                    "当前 IK Rig 导出声明 FullBodyIK；本候选仅为独立解析式双骨骼近似，不声明 UE FullBodyIK 等价。");
                bool DisabledOp = false;
                ImGui::BeginDisabled(true);
                ImGui::Checkbox(
                    "Spine / Pelvis 跟随（此候选路线关闭）",
                    &DisabledOp);
                ImGui::Checkbox(
                    "脚锁定（此候选路线关闭）", &DisabledOp);
                ImGui::EndDisabled();
            }
            else
            {
                ImGui::Checkbox(
                    "Spine / Pelvis 跟随",
                    &State->SpinePelvisFollow);
                ImGui::Checkbox(
                    "脚锁定（Source Motion Foot Lock）",
                    &State->SourceMotionFootLock);
            }
            if (!State->UseUEIKJsonRoute &&
                State->SourceMotionFootLock &&
                !State->SpinePelvisFollow)
            {
                ImGui::TextColored(
                    {1.0F, 0.66F, 0.25F, 1.0F},
                    "Foot Lock 依赖 Spine/Pelvis；当前组合会在预检时被拒绝，不会自动替你修改。");
            }

            ImGui::Separator();
            const RetargetAssetSelectionValidation CatalogSelection =
                State->UseUEIKJsonRoute
                ? State->ValidateCatalogSelection()
                : RetargetAssetSelectionValidation{true, {}};
            ImGui::BeginDisabled(State->Process.IsActive());
            if (ImGui::Button("验证输入")) State->Validate();
            ImGui::SameLine();
            ImGui::BeginDisabled(!CatalogSelection.Success);
            if (ImGui::Button("重定向并打开 SKRV")) State->Start();
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled(
                State->UseUEIKJsonRoute
                    ? "候选路线未采纳 | UE 导出 JSON | SKRV v1"
                    : "数值核心冻结 | SKRV v1 | N3 Mesh 未启动");

            if (State->UseUEIKJsonRoute &&
                !CatalogSelection.Success)
            {
                for (const std::string& Error :
                     CatalogSelection.Errors)
                {
                    ImGui::TextColored(
                        {1.0F, 0.66F, 0.25F, 1.0F},
                        "- %s", Error.c_str());
                }
            }
            if (!State->Message.empty())
                ImGui::TextWrapped("%s", State->Message.c_str());
            for (const std::string& Error : State->LastPreflight.Errors)
                ImGui::TextColored(
                    {1.0F, 0.45F, 0.45F, 1.0F}, "- %s", Error.c_str());
            for (const std::string& Warning : State->LastPreflight.Warnings)
                ImGui::TextColored(
                    {0.95F, 0.76F, 0.35F, 1.0F}, "- %s", Warning.c_str());
            if (State->Process.IsActive())
            {
                ImGui::Separator();
                ImGui::Text("运行目录：%s",
                    PathToUtf8(State->LastRequest.OutputDirectory).c_str());
                ImGui::Text("启动日志：%s",
                    PathToUtf8(State->ActiveLauncherLog).c_str());
            }
        }
        ImGui::End();
    }

    if (State->BatchOpen)
    {
        ImGui::SetNextWindowSize({960.0F, 760.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(
                "批量重定向文件夹##BatchRetarget",
                &State->BatchOpen, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped(
                "Profile 批处理会逐项绑定角色包、动画目录记录与 UE Golden；启动前验证完整批次。执行并发固定为 1，每个 Worker 完成验证并退出后才开始下一条。");
            ImGui::SeparatorText("重定向路线");
            if (ImGui::RadioButton(
                    "外部 Foundation 兼容路线##BatchExternalFoundationRoute",
                    !State->UseUEIKJsonRoute))
            {
                State->UseUEIKJsonRoute = false;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(
                    "UE IK Rig JSON 候选路线##BatchUEIKJsonRoute",
                    State->UseUEIKJsonRoute))
            {
                State->UseUEIKJsonRoute = true;
            }
            ImGui::SeparatorText("角色与目录");
            if (State->UseUEIKJsonRoute)
            {
                if (State->CatalogLoaded)
                {
                    const RetargetSkeletonAsset* Source =
                        State->SelectedSourceSkeleton();
                    const bool SourceIsProfile =
                        Source != nullptr &&
                        State->ActiveProfileFor(Source->Id) != nullptr;
                    const char* SourcePreview = SourceIsProfile
                        ? Source->Label.c_str()
                        : "请选择已安装的源 Profile";
                    ImGui::TextUnformatted("源角色 Profile");
                    ImGui::SetNextItemWidth(-1.0F);
                    if (ImGui::BeginCombo(
                            "##BatchSourceProfile",
                            SourcePreview))
                    {
                        for (std::size_t Index = 0;
                             Index < State->Catalog.Skeletons.size();
                             ++Index)
                        {
                            const RetargetSkeletonAsset& Skeleton =
                                State->Catalog.Skeletons[Index];
                            if (!Skeleton.SourceEnabled ||
                                State->ActiveProfileFor(
                                    Skeleton.Id) == nullptr)
                            {
                                continue;
                            }
                            const bool Selected =
                                State->SourceSkeletonIndex ==
                                static_cast<int>(Index);
                            ImGui::PushID(
                                ("batch_source_" + Skeleton.Id)
                                    .c_str());
                            if (ImGui::Selectable(
                                    Skeleton.Label.c_str(),
                                    Selected))
                            {
                                State->SourceSkeletonIndex =
                                    static_cast<int>(Index);
                                State->RefreshCompatibleAnimations(
                                    false);
                            }
                            if (Selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }

                    const RetargetSkeletonAsset* Target =
                        State->SelectedTargetSkeleton();
                    const bool TargetIsProfile =
                        Target != nullptr &&
                        State->ActiveProfileFor(Target->Id) != nullptr;
                    const char* TargetPreview = TargetIsProfile
                        ? Target->Label.c_str()
                        : "请选择已安装的目标 Profile";
                    ImGui::TextUnformatted("目标角色 Profile");
                    ImGui::SetNextItemWidth(-1.0F);
                    if (ImGui::BeginCombo(
                            "##BatchTargetProfile",
                            TargetPreview))
                    {
                        for (std::size_t Index = 0;
                             Index < State->Catalog.Skeletons.size();
                             ++Index)
                        {
                            const RetargetSkeletonAsset& Skeleton =
                                State->Catalog.Skeletons[Index];
                            if (!Skeleton.TargetEnabled ||
                                State->ActiveProfileFor(
                                    Skeleton.Id) == nullptr)
                            {
                                continue;
                            }
                            const bool Selected =
                                State->TargetSkeletonIndex ==
                                static_cast<int>(Index);
                            ImGui::PushID(
                                ("batch_target_" + Skeleton.Id)
                                    .c_str());
                            if (ImGui::Selectable(
                                    Skeleton.Label.c_str(),
                                    Selected))
                            {
                                State->TargetSkeletonIndex =
                                    static_cast<int>(Index);
                            }
                            if (Selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::SeparatorText(
                        "本批次动画（仅当前源 Profile）");
                    if (ImGui::SmallButton("全选"))
                    {
                        for (const std::size_t Index :
                             State->CompatibleAnimationIndices)
                        {
                            State->BatchSelectedAnimationIds.insert(
                                State->Catalog.Animations[Index].Id);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("清空"))
                    {
                        State->BatchSelectedAnimationIds.clear();
                    }
                    for (const std::size_t Index :
                         State->CompatibleAnimationIndices)
                    {
                        const RetargetAnimationAsset& Animation =
                            State->Catalog.Animations[Index];
                        bool Selected =
                            State->BatchSelectedAnimationIds.contains(
                                Animation.Id);
                        ImGui::PushID(
                            ("batch_animation_" + Animation.Id)
                                .c_str());
                        if (ImGui::Checkbox(
                                Animation.Label.c_str(),
                                &Selected))
                        {
                            if (Selected)
                            {
                                State->BatchSelectedAnimationIds
                                    .insert(Animation.Id);
                            }
                            else
                            {
                                State->BatchSelectedAnimationIds
                                    .erase(Animation.Id);
                            }
                        }
                        ImGui::PopID();
                    }
                    ImGui::TextDisabled(
                        "已选择 %llu / %llu；每段动画保留自己的 FBX、Golden、Stack 与 SHA-256。",
                        static_cast<unsigned long long>(
                            State->BatchSelectedAnimationIds.size()),
                        static_cast<unsigned long long>(
                            State->CompatibleAnimationIndices.size()));
                    if (!SourceIsProfile || !TargetIsProfile)
                    {
                        ImGui::TextColored(
                            {1.0F, 0.66F, 0.25F, 1.0F},
                            "批量 v3 要求源和目标都来自已安装的 .skrtgprofile。");
                    }
                }
                else
                {
                    ImGui::TextColored(
                        {1.0F, 0.45F, 0.45F, 1.0F},
                        "Profile/Catalog 不可用；UE IK JSON 批处理保持 fail-closed。");
                }
            }
            else
            {
                DrawPathRow(
                    "源角色 T-pose FBX", "##BatchSourceRest",
                    State->Buffers.SourceRest, State->Browser,
                    BrowseTarget::SourceRest,
                    BrowseSelection::FbxFile);
                DrawPathRow(
                    "目标角色 T-pose FBX",
                    "##BatchTargetRest",
                    State->Buffers.TargetSkeleton, State->Browser,
                    BrowseTarget::TargetSkeleton,
                    BrowseSelection::FbxFile);
                DrawPathRow(
                    "源动画文件夹",
                    "##BatchAnimationDirectory",
                    State->Buffers.BatchAnimationDirectory,
                    State->Browser,
                    BrowseTarget::BatchAnimationDirectory,
                    BrowseSelection::Directory);
            }
            DrawPathRow(
                "批量输出文件夹", "##BatchOutputRoot",
                State->Buffers.BatchOutputRoot, State->Browser,
                BrowseTarget::BatchOutputRoot,
                BrowseSelection::Directory);
            if (!State->UseUEIKJsonRoute)
            {
                ImGui::Checkbox(
                    "递归扫描子文件夹，并在 FinalFBX 中保留相对目录",
                    &State->BatchRecursive);
            }

            ImGui::SeparatorText("本批次的 Op Stack");
            ImGui::BeginDisabled(true);
            bool FoundationCore = true;
            ImGui::Checkbox(
                State->UseUEIKJsonRoute
                    ? "UE IK JSON 候选核心：FK + Pelvis + Limb IK"
                    : "冻结核心：FK + Limb IK + Finger IK",
                &FoundationCore);
            ImGui::EndDisabled();
            if (State->UseUEIKJsonRoute)
            {
                bool DisabledOp = false;
                ImGui::BeginDisabled(true);
                ImGui::Checkbox(
                    "Spine / Pelvis 跟随（此候选路线关闭）"
                    "##BatchSpinePelvisDisabled",
                    &DisabledOp);
                ImGui::Checkbox(
                    "脚锁定（此候选路线关闭）##BatchFootLockDisabled",
                    &DisabledOp);
                ImGui::EndDisabled();
            }
            else
            {
                ImGui::Checkbox(
                    "Spine / Pelvis 跟随##BatchSpinePelvis",
                    &State->SpinePelvisFollow);
                ImGui::Checkbox(
                    "脚锁定（Source Motion Foot Lock）##BatchFootLock",
                    &State->SourceMotionFootLock);
            }
            if (!State->UseUEIKJsonRoute &&
                State->SourceMotionFootLock &&
                !State->SpinePelvisFollow)
            {
                ImGui::TextColored(
                    {1.0F, 0.66F, 0.25F, 1.0F},
                    "Foot Lock 依赖 Spine/Pelvis；预检会 fail-closed，不会自动改路线。 ");
            }

            ImGui::Separator();
            ImGui::BeginDisabled(State->Process.IsActive());
            if (ImGui::Button("扫描并验证批次"))
                State->ValidateBatch();
            ImGui::SameLine();
            if (ImGui::Button("开始批量重定向"))
                State->StartBatch();
            ImGui::EndDisabled();
            if (State->ActiveRun == ActiveRunKind::Batch &&
                State->Process.IsActive())
            {
                ImGui::SameLine();
                if (ImGui::Button("停止批处理"))
                    State->CancelBatch();
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "低内存流式模式 | 并发固定 1 | 不读取 XML/uasset");

            if (!State->BatchMessage.empty())
                ImGui::TextWrapped("%s", State->BatchMessage.c_str());
            if (!State->BatchStatusReadError.empty())
            {
                ImGui::TextColored(
                    {1.0F, 0.66F, 0.25F, 1.0F}, "%s",
                    State->BatchStatusReadError.c_str());
            }
            for (const std::string& Error : State->LastBatchPlan.Errors)
            {
                ImGui::TextColored(
                    {1.0F, 0.45F, 0.45F, 1.0F}, "- %s", Error.c_str());
            }
            for (const std::string& Warning : State->LastBatchPlan.Warnings)
            {
                ImGui::TextColored(
                    {0.95F, 0.76F, 0.35F, 1.0F}, "- %s", Warning.c_str());
            }

            const BatchRetargetStatus& Status = State->LastBatchStatus;
            if (Status.TotalJobs > 0)
            {
                ImGui::SeparatorText("批次进度");
                const float Progress = static_cast<float>(
                    static_cast<double>(Status.CompletedJobs) /
                    static_cast<double>(Status.TotalJobs));
                const std::string Overlay =
                    std::to_string(Status.CompletedJobs) + " / " +
                    std::to_string(Status.TotalJobs) +
                    "（成功 " + std::to_string(Status.SucceededJobs) +
                    "，失败 " + std::to_string(Status.FailedJobs) + "）";
                ImGui::ProgressBar(Progress, {-1.0F, 0.0F}, Overlay.c_str());
                if (Status.HasActiveJob &&
                    Status.ActiveJobIndex < Status.Jobs.size())
                {
                    const BatchRetargetJob& Active =
                        Status.Jobs[Status.ActiveJobIndex];
                    ImGui::TextWrapped(
                        "正在处理：%s",
                        PathToUtf8(Active.RelativeAnimationPath).c_str());
                }
                ImGui::TextWrapped(
                    "批次目录：%s",
                    PathToUtf8(Status.OutputDirectory).c_str());
                ImGui::Text(
                    "累计用时：%.2f 秒 | 最大并发：%zu",
                    Status.DurationSeconds,
                    Status.MaximumConcurrentJobs);

                const BatchRetargetJob* LastSucceeded = nullptr;
                for (auto Iterator = Status.Jobs.rbegin();
                     Iterator != Status.Jobs.rend(); ++Iterator)
                {
                    if (Iterator->State ==
                        BatchRetargetJobState::Succeeded)
                    {
                        LastSucceeded = &*Iterator;
                        break;
                    }
                }
                ImGui::BeginDisabled(LastSucceeded == nullptr);
                if (ImGui::Button("加载最后一个成功任务的 SKRV") &&
                    LastSucceeded != nullptr)
                {
                    State->RequestedPackage =
                        LastSucceeded->ReviewPackage;
                }
                ImGui::EndDisabled();

                int ShownFailures = 0;
                for (const BatchRetargetJob& Job : Status.Jobs)
                {
                    if (Job.State != BatchRetargetJobState::Failed)
                        continue;
                    if (ShownFailures++ == 0)
                        ImGui::SeparatorText("失败任务");
                    if (ShownFailures > 8)
                    {
                        ImGui::TextDisabled("其余失败项请查看 batch_status.json");
                        break;
                    }
                    ImGui::TextColored(
                        {1.0F, 0.45F, 0.45F, 1.0F},
                        "%s：%s",
                        PathToUtf8(Job.RelativeAnimationPath).c_str(),
                        Job.Errors.empty() ? "未知错误" :
                            Job.Errors.front().c_str());
                }
            }

            if (!State->ActiveBatchLauncherLog.empty())
            {
                ImGui::TextDisabled(
                    "协调器日志：%s",
                    PathToUtf8(
                        State->ActiveBatchLauncherLog).c_str());
            }
        }
        ImGui::End();
    }

    if (State->OpenSkrvDialog)
    {
        ImGui::SetNextWindowSize({760.0F, 230.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(
                "打开 SKRV 审查包##OpenSkrv",
                &State->OpenSkrvDialog, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped(
                "选择一个 .skrv 文件夹。打开前会重新执行完整的 manifest、路径、尺寸与 SHA-256 严格校验。");
            DrawPathRow(
                "SKRV 审查包", "##OpenSkrvPath",
                State->Buffers.OpenSkrv, State->Browser,
                BrowseTarget::OpenSkrv, BrowseSelection::SkrvPackage);
            const std::filesystem::path Package =
                BufferPath(State->Buffers.OpenSkrv);
            ImGui::BeginDisabled(!IsSkrvPackage(Package));
            if (ImGui::Button("严格验证并打开"))
            {
                State->RequestedPackage = Package;
                State->OpenSkrvDialog = false;
            }
            ImGui::EndDisabled();
        }
        ImGui::End();
    }

    if (State->ExportDialog)
    {
        ImGui::SetNextWindowSize({780.0F, 330.0F}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(
                "导出已验证的 Final FBX##ExportFinal",
                &State->ExportDialog, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextWrapped(
                "源文件：%s",
                PathToUtf8(State->ExportSourceFbx).c_str());
            ImGui::TextDisabled(
                "保护中的 SKRV：%s（禁止导出到其内部）",
                PathToUtf8(
                    State->ExportProtectedPackageDirectory).c_str());
            DrawPathRow(
                "导出目录", "##ExportRoot",
                State->Buffers.ExportRoot, State->Browser,
                BrowseTarget::ExportRoot, BrowseSelection::Directory);
            ImGui::TextUnformatted("文件名");
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputText(
                "##ExportFileName", State->ExportFileName.data(),
                State->ExportFileName.size());
            ImGui::Checkbox("允许覆盖同名文件", &State->ExportOverwrite);
            if (ImGui::Button("导出 Final FBX"))
            {
                State->ExportMessage.clear();
                const std::filesystem::path Root =
                    BufferPath(State->Buffers.ExportRoot);
                const std::filesystem::path FileName =
                    BufferPath(State->ExportFileName);
                if (Root.empty() || FileName.empty() ||
                    FileName != FileName.filename() || !IsFbx(FileName))
                {
                    State->ExportMessage =
                        "导出失败：请选择目录并输入不含路径的 .fbx 文件名。";
                }
                else
                {
                    const VerifiedExportCopyResult Export =
                        CopyVerifiedExport({
                            State->ExportSourceFbx,
                            Root / FileName,
                            State->ExportProtectedPackageDirectory,
                            State->ExportExpectedSha256,
                            State->ExportOverwrite});
                    if (Export.Success)
                    {
                        State->ExportMessage =
                            "导出成功：" +
                            PathToUtf8(Export.DestinationFbx);
                    }
                    else
                    {
                        State->ExportMessage = "导出失败";
                        if (!Export.Errors.empty())
                            State->ExportMessage += "：" + Export.Errors.front();
                    }
                }
            }
            if (!State->ExportMessage.empty())
                ImGui::TextWrapped("%s", State->ExportMessage.c_str());
        }
        ImGui::End();
    }

    DrawBrowser(State->Browser, State->Buffers);
}

void RetargetBridgeUi::DrawOperationStackWindow(bool* Open)
{
    if (Open == nullptr || !*Open) return;
    ImGui::SetNextWindowSize({390.0F, 330.0F}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("操作栈##OperationStack", Open))
    {
        ImGui::TextDisabled("从上到下执行；设置应用于下一次重定向");
        ImGui::Separator();
        ImGui::TextUnformatted("01  基础重定向");
        ImGui::Indent();
        ImGui::BeginDisabled(true);
        bool Fk = true;
        bool LimbIk = true;
        bool FingerIk = true;
        ImGui::Checkbox("FK 映射（路线定义，只读）", &Fk);
        ImGui::Checkbox("Limb IK（路线定义，只读）", &LimbIk);
        ImGui::Checkbox("Finger IK（依路线可能关闭）", &FingerIk);
        ImGui::EndDisabled();
        ImGui::Unindent();
        ImGui::Separator();
        ImGui::TextUnformatted("02  身体跟随");
        ImGui::Indent();
        ImGui::Checkbox(
            "Spine / Pelvis Follow", &State->SpinePelvisFollow);
        ImGui::Unindent();
        ImGui::Separator();
        ImGui::TextUnformatted("03  后处理");
        ImGui::Indent();
        ImGui::Checkbox(
            "Source Motion Foot Lock", &State->SourceMotionFootLock);
        ImGui::Unindent();
        ImGui::TextWrapped(
            "当前 SKRV 含已验证的 Foundation / Final 时，此开关会同时实时切换 Viewer A/B；它也会作为下一次重定向的 Foot Lock 参数。 ");
        if (State->SourceMotionFootLock && !State->SpinePelvisFollow)
        {
            ImGui::TextColored(
                {1.0F, 0.66F, 0.25F, 1.0F},
                "当前依赖无效：Foot Lock 需要 Spine/Pelvis。预检会 fail-closed。 ");
        }
        ImGui::Separator();
        ImGui::TextDisabled(
            "基础核心由导入面板中显式选择的路线定义；此面板不提供会改写路线合同的虚假开关。 ");
    }
    ImGui::End();
}

void RetargetBridgeUi::Poll()
{
    if (!State->Process.IsActive()) return;
    if (State->ActiveRun == ActiveRunKind::Batch &&
        std::chrono::steady_clock::now() >=
            State->NextBatchStatusPoll)
    {
        State->RefreshBatchStatus();
        State->NextBatchStatusPoll =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(250);
    }
    bool Finished = false;
    int ExitCode = -1;
    std::string Error;
    if (!State->Process.Poll(Finished, ExitCode, Error))
    {
        if (State->ActiveRun == ActiveRunKind::Batch)
            State->BatchMessage = "批量协调器轮询失败：" + Error;
        else
            State->Message = "Bridge 进程轮询失败：" + Error;
        return;
    }
    if (!Finished) return;
    if (State->ActiveRun == ActiveRunKind::Batch)
    {
        State->RefreshBatchStatus();
        if (ExitCode == 0 && State->LastBatchStatus.Complete &&
            State->LastBatchStatus.Success)
        {
            State->BatchMessage =
                "批量重定向完成：" +
                std::to_string(State->LastBatchStatus.SucceededJobs) +
                " 个动画全部成功。Final FBX 位于：" +
                PathToUtf8(
                    State->LastBatchStatus.OutputDirectory / "FinalFBX");
        }
        else if (State->LastBatchStatus.TotalJobs > 0)
        {
            State->BatchMessage =
                "批处理结束（退出码 " + std::to_string(ExitCode) +
                "）：成功 " +
                std::to_string(State->LastBatchStatus.SucceededJobs) +
                "，失败 " +
                std::to_string(State->LastBatchStatus.FailedJobs) +
                "。请查看 batch_status.json 与对应任务日志。";
        }
        else
        {
            State->BatchMessage =
                "批量协调器未生成有效状态（退出码 " +
                std::to_string(ExitCode) + "）。请检查日志：" +
                PathToUtf8(State->ActiveBatchLauncherLog);
        }
        State->ActiveRun = ActiveRunKind::None;
        return;
    }
    if (ExitCode != 0)
    {
        State->Message =
            "重定向 Bridge 失败（退出码 " + std::to_string(ExitCode) +
            "）。请检查 bridge_status.json 与日志：" +
            PathToUtf8(State->LastRequest.OutputDirectory);
        State->ActiveRun = ActiveRunKind::None;
        return;
    }
    const std::filesystem::path Package =
        State->LastRequest.OutputDirectory / "review.skrv";
    std::error_code FileError;
    if (!std::filesystem::is_directory(Package, FileError) || FileError)
    {
        State->Message =
            "Bridge 已正常退出，但没有生成 review.skrv。";
        State->ActiveRun = ActiveRunKind::None;
        return;
    }
    State->CompletedPackage = Package;
    ++State->CompletedRuns;
    State->Message =
        "Bridge 已完成；本帧将执行严格 SKRV 校验并载入 Viewer。";
    State->ActiveRun = ActiveRunKind::None;
}

bool RetargetBridgeUi::IsOpen() const
{
    return State->Open || State->BatchOpen;
}
bool RetargetBridgeUi::IsRunning() const { return State->Process.IsActive(); }
int RetargetBridgeUi::CompletedRunCount() const
{
    return State->CompletedRuns;
}

std::optional<std::filesystem::path>
RetargetBridgeUi::ConsumeCompletedPackage()
{
    std::optional<std::filesystem::path> Result =
        std::move(State->CompletedPackage);
    State->CompletedPackage.reset();
    return Result;
}

std::optional<std::filesystem::path>
RetargetBridgeUi::ConsumeRequestedPackage()
{
    std::optional<std::filesystem::path> Result =
        std::move(State->RequestedPackage);
    State->RequestedPackage.reset();
    return Result;
}

bool RetargetBridgeUi::SpinePelvisFollowEnabled() const
{
    return State->SpinePelvisFollow;
}

bool RetargetBridgeUi::SourceMotionFootLockEnabled() const
{
    return State->SourceMotionFootLock;
}

void RetargetBridgeUi::SetSpinePelvisFollowEnabled(const bool Enabled)
{
    State->SpinePelvisFollow = Enabled;
}

void RetargetBridgeUi::SetSourceMotionFootLockEnabled(const bool Enabled)
{
    State->SourceMotionFootLock = Enabled;
}

} // namespace skrtg::viewer
