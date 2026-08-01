#include "retarget_bridge_ui.h"

#include "skrtg/viewer/batch_retarget.h"
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
    OpenSkrv,
    ExportRoot
};

enum class BrowseSelection
{
    FbxFile,
    JsonFile,
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
        if (EntryError || (!Directory &&
            ((Browser.Selection == BrowseSelection::FbxFile && !Fbx) ||
             (Browser.Selection == BrowseSelection::JsonFile && !Json) ||
             (Browser.Selection != BrowseSelection::FbxFile &&
              Browser.Selection != BrowseSelection::JsonFile))))
            continue;
        const std::string Name =
            (Skrv && Browser.Selection == BrowseSelection::SkrvPackage
                ? "[SKRV] "
                : (Directory
                    ? "[目录] "
                    : (Json ? "[JSON] " : "[FBX] "))) +
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
         (Browser.Selection == BrowseSelection::SkrvPackage &&
          IsSkrvPackage(Browser.Selected)) ||
         (Browser.Selection == BrowseSelection::Directory &&
          IsDirectory(Browser.Selected)));
    ImGui::BeginDisabled(!AcceptedSelection);
    const char* SelectLabel =
        Browser.Selection == BrowseSelection::FbxFile ? "选择 FBX" :
        Browser.Selection == BrowseSelection::JsonFile ? "选择 JSON" :
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

        CatalogFile = DiscoverRetargetAssetCatalog(ViewerExecutable);
        RetargetAssetCatalogLoadResult CatalogResult =
            LoadRetargetAssetCatalog(CatalogFile);
        CatalogErrors = std::move(CatalogResult.Errors);
        CatalogWarnings = std::move(CatalogResult.Warnings);
        if (CatalogResult.Success)
        {
            Catalog = std::move(CatalogResult.Catalog);
            CatalogLoaded = true;
            UseUEIKJsonRoute = true;
            for (std::size_t Index = 0;
                 Index < Catalog.Skeletons.size(); ++Index)
            {
                const RetargetSkeletonAsset& Skeleton =
                    Catalog.Skeletons[Index];
                if (SourceSkeletonIndex < 0 &&
                    Skeleton.SourceEnabled)
                {
                    SourceSkeletonIndex =
                        static_cast<int>(Index);
                }
                if (TargetSkeletonIndex < 0 &&
                    Skeleton.TargetEnabled)
                {
                    TargetSkeletonIndex =
                        static_cast<int>(Index);
                }
            }
            RefreshCompatibleAnimations(true);
            Message =
                "资产目录已验证：请选择源骨骼、对应动画和目标骨骼。";
        }
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

    void RefreshCompatibleAnimations(const bool SelectFirst)
    {
        CompatibleAnimationIndices.clear();
        AnimationIndex = -1;
        const RetargetSkeletonAsset* Source =
            SelectedSourceSkeleton();
        if (Source == nullptr) return;
        CompatibleAnimationIndices =
            CompatibleRetargetAnimationIndices(
                Catalog, Source->Id);
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
        Request.SourceCharacter.RestFbx =
            BufferPath(Buffers.SourceRest).lexically_normal();
        Request.TargetCharacter.RestFbx =
            BufferPath(Buffers.TargetSkeleton).lexically_normal();
        Request.SourceCharacter.DefinitionKind = UseUEIKJsonRoute
            ? "ue_ik_json_v1"
            : "external_foundation_v1";
        Request.TargetCharacter.DefinitionKind =
            Request.SourceCharacter.DefinitionKind;
        if (UseUEIKJsonRoute)
        {
            Request.SourceCharacter.DefinitionFile =
                BufferPath(Buffers.SourceRigJson).lexically_normal();
            Request.TargetCharacter.DefinitionFile =
                BufferPath(Buffers.TargetRigJson).lexically_normal();
            Request.SourceCharacter.AlignmentRetargeterFile =
                BufferPath(Buffers.SourceAlignmentRetargeterJson)
                    .lexically_normal();
            Request.TargetCharacter.AlignmentRetargeterFile =
                BufferPath(Buffers.TargetAlignmentRetargeterJson)
                    .lexically_normal();
            Request.AnimationStack = Buffers.AnimationStack.data();
        }
        Request.AnimationDirectory =
            BufferPath(Buffers.BatchAnimationDirectory).lexically_normal();
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
        Request.Recursive = BatchRecursive;
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
    RetargetAssetCatalog Catalog;
    bool CatalogLoaded = false;
    int SourceSkeletonIndex = -1;
    int TargetSkeletonIndex = -1;
    int AnimationIndex = -1;
    std::vector<std::size_t> CompatibleAnimationIndices;
    std::vector<std::string> CatalogErrors;
    std::vector<std::string> CatalogWarnings;
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
                            if (ImGui::Selectable(
                                    Skeleton.Label.c_str(), Selected))
                            {
                                State->TargetSkeletonIndex =
                                    static_cast<int>(Index);
                            }
                            if (Selected)
                                ImGui::SetItemDefaultFocus();
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
                "先选择路线、源与目标 Rest FBX，再选择动画和输出文件夹。批处理固定并发 1；每个 Worker 完成验证并退出后才开始下一条，单条失败会记录并继续。");
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
            DrawPathRow(
                "源角色 T-pose FBX", "##BatchSourceRest",
                State->Buffers.SourceRest, State->Browser,
                BrowseTarget::SourceRest, BrowseSelection::FbxFile);
            DrawPathRow(
                "目标角色 T-pose FBX", "##BatchTargetRest",
                State->Buffers.TargetSkeleton, State->Browser,
                BrowseTarget::TargetSkeleton, BrowseSelection::FbxFile);
            if (State->UseUEIKJsonRoute)
            {
                DrawPathRow(
                    "源 IK Rig JSON", "##BatchSourceRigJson",
                    State->Buffers.SourceRigJson, State->Browser,
                    BrowseTarget::SourceRigJson,
                    BrowseSelection::JsonFile);
                DrawPathRow(
                    "目标 IK Rig JSON", "##BatchTargetRigJson",
                    State->Buffers.TargetRigJson, State->Browser,
                    BrowseTarget::TargetRigJson,
                    BrowseSelection::JsonFile);
                DrawPathRow(
                    "源对齐 IK Retargeter JSON",
                    "##BatchSourceAlignmentRetargeterJson",
                    State->Buffers.SourceAlignmentRetargeterJson,
                    State->Browser,
                    BrowseTarget::SourceAlignmentRetargeterJson,
                    BrowseSelection::JsonFile);
                DrawPathRow(
                    "目标对齐 IK Retargeter JSON",
                    "##BatchTargetAlignmentRetargeterJson",
                    State->Buffers.TargetAlignmentRetargeterJson,
                    State->Browser,
                    BrowseTarget::TargetAlignmentRetargeterJson,
                    BrowseSelection::JsonFile);
                ImGui::TextUnformatted(
                    "动画 Stack（留空时每个 FBX 确定性选择最长 Stack）");
                ImGui::SetNextItemWidth(-1.0F);
                ImGui::InputText(
                    "##BatchAnimationStack",
                    State->Buffers.AnimationStack.data(),
                    State->Buffers.AnimationStack.size());
            }
            DrawPathRow(
                "源动画文件夹", "##BatchAnimationDirectory",
                State->Buffers.BatchAnimationDirectory, State->Browser,
                BrowseTarget::BatchAnimationDirectory,
                BrowseSelection::Directory);
            DrawPathRow(
                "批量输出文件夹", "##BatchOutputRoot",
                State->Buffers.BatchOutputRoot, State->Browser,
                BrowseTarget::BatchOutputRoot,
                BrowseSelection::Directory);
            ImGui::Checkbox(
                "递归扫描子文件夹，并在 FinalFBX 中保留相对目录",
                &State->BatchRecursive);

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
