#include "native_viewer_app.h"
#include "mesh_renderer.h"
#include "retarget_bridge_ui.h"

#include "skrtg/viewer/camera.h"
#include "skrtg/viewer/mesh_skinning.h"
#include "skrtg/viewer/playback.h"
#include "skrtg/viewer/retarget_bridge.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skrtg::viewer
{
namespace
{
constexpr float PanelHeaderHeight = 48.0F;
constexpr float PanelGap = 6.0F;

struct PanelViewport
{
    ImVec2 Minimum;
    ImVec2 Maximum;
    bool Hovered = false;
};

struct PanelDefinition
{
    const char* Id = "";
    const char* Title = "";
    const char* Subtitle = "";
    const std::vector<Bone>* Bones = nullptr;
    const std::vector<Vec3>* MainPositions = nullptr;
    const std::vector<Bone>* GhostBones = nullptr;
    const std::vector<Vec3>* GhostPositions = nullptr;
    const MeshPackage* MainMesh = nullptr;
    const std::vector<std::vector<Vec3>>* MainMeshPositions = nullptr;
    const MeshPackage* GhostMesh = nullptr;
    const std::vector<std::vector<Vec3>>* GhostMeshPositions = nullptr;
    ReviewLane Lane = ReviewLane::Original;
    ImVec4 MainColor;
    float MainOpacity = 1.0F;
    float GhostOpacity = 0.1F;
};

struct DisplayVisibility
{
    bool Grid = true;
    bool Mesh = true;
    bool Skeleton = true;
    bool IkGoals = true;
    bool GoalHistory = true;
    bool IncludeFingerGoals = false;
};

struct SceneDisplayData
{
    std::vector<Vec3> FkGhostBones;
    std::vector<Vec3> FoundationGhostBones;
    std::vector<std::vector<Vec3>> SourceMesh;
    std::vector<std::vector<Vec3>> FkMesh;
    std::vector<std::vector<Vec3>> FoundationMesh;
    std::vector<std::vector<Vec3>> FinalMesh;
    std::vector<std::vector<Vec3>> FkGhostMesh;
    std::vector<std::vector<Vec3>> FoundationGhostMesh;
    GoalHistory GoalTrails;
};

struct MeshCallbackData
{
    MeshRenderer* Renderer = nullptr;
    MeshRenderViewport Viewport;
    OrbitCamera Camera;
    std::array<MeshRenderLayer, 2> Layers{};
    std::size_t LayerCount = 0;
    std::string* RenderError = nullptr;
};

std::string ComparableReviewPackagePath(
    const std::filesystem::path& Package)
{
    std::error_code Error;
    const std::filesystem::path Absolute =
        std::filesystem::absolute(Package, Error);
    std::string Result = PathToUtf8(
        (Error ? Package : Absolute).lexically_normal());
#if defined(_WIN32)
    std::transform(
        Result.begin(), Result.end(), Result.begin(),
        [](const unsigned char Character)
        {
            return static_cast<char>(std::tolower(Character));
        });
#endif
    return Result;
}

ImU32 WithOpacity(const ImVec4& Color, const float Opacity)
{
    ImVec4 Value = Color;
    Value.w = std::clamp(Color.w * Opacity, 0.0F, 1.0F);
    return ImGui::ColorConvertFloat4ToU32(Value);
}

std::optional<float> MaximumMeshPositionDelta(
    const std::vector<std::vector<Vec3>>& Foundation,
    const std::vector<std::vector<Vec3>>& Final)
{
    if (Foundation.size() != Final.size()) return std::nullopt;
    float Maximum = 0.0F;
    for (std::size_t MeshIndex = 0;
         MeshIndex < Foundation.size(); ++MeshIndex)
    {
        if (Foundation[MeshIndex].size() != Final[MeshIndex].size())
            return std::nullopt;
        for (std::size_t VertexIndex = 0;
             VertexIndex < Foundation[MeshIndex].size(); ++VertexIndex)
        {
            const Vec3& Left = Foundation[MeshIndex][VertexIndex];
            const Vec3& Right = Final[MeshIndex][VertexIndex];
            const float X = Left.X - Right.X;
            const float Y = Left.Y - Right.Y;
            const float Z = Left.Z - Right.Z;
            Maximum = std::max(Maximum, std::sqrt(X * X + Y * Y + Z * Z));
        }
    }
    return Maximum;
}

ImVec2 ToScreen(
    const PanelViewport& Viewport,
    const ProjectedPoint& Point)
{
    return {Viewport.Minimum.x + Point.X, Viewport.Minimum.y + Point.Y};
}

void DrawGrid(
    ImDrawList* DrawList,
    const PanelViewport& Viewport,
    const OrbitCamera& Camera)
{
    const ProjectionViewport Projection = {
        Viewport.Maximum.x - Viewport.Minimum.x,
        Viewport.Maximum.y - Viewport.Minimum.y};
    const float Extent = 260.0F;
    const float Step = 20.0F;
    DrawList->PushClipRect(Viewport.Minimum, Viewport.Maximum, true);
    for (float Coordinate = -Extent; Coordinate <= Extent; Coordinate += Step)
    {
        const ProjectedPoint AlongXStart =
            ProjectPoint(Camera, Projection, {-Extent, 0.0F, Coordinate});
        const ProjectedPoint AlongXEnd =
            ProjectPoint(Camera, Projection, {Extent, 0.0F, Coordinate});
        const ProjectedPoint AlongZStart =
            ProjectPoint(Camera, Projection, {Coordinate, 0.0F, -Extent});
        const ProjectedPoint AlongZEnd =
            ProjectPoint(Camera, Projection, {Coordinate, 0.0F, Extent});
        const bool Major = std::fmod(std::fabs(Coordinate), 100.0F) < 0.1F;
        const ImU32 Color = Major
            ? IM_COL32(78, 83, 98, 115)
            : IM_COL32(59, 63, 75, 78);
        if (AlongXStart.Visible && AlongXEnd.Visible)
        {
            DrawList->AddLine(
                ToScreen(Viewport, AlongXStart),
                ToScreen(Viewport, AlongXEnd), Color, Major ? 1.2F : 1.0F);
        }
        if (AlongZStart.Visible && AlongZEnd.Visible)
        {
            DrawList->AddLine(
                ToScreen(Viewport, AlongZStart),
                ToScreen(Viewport, AlongZEnd), Color, Major ? 1.2F : 1.0F);
        }
    }
    DrawList->PopClipRect();
}

void DrawSkeleton(
    ImDrawList* DrawList,
    const PanelViewport& Viewport,
    const OrbitCamera& Camera,
    const std::vector<Bone>& Bones,
    const std::vector<Vec3>& Positions,
    const ImVec4& Color,
    const float Opacity,
    const float Thickness)
{
    if (Bones.size() != Positions.size())
        return;
    const ProjectionViewport Projection = {
        Viewport.Maximum.x - Viewport.Minimum.x,
        Viewport.Maximum.y - Viewport.Minimum.y};
    DrawList->PushClipRect(Viewport.Minimum, Viewport.Maximum, true);
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        const int Parent = Bones[Index].ParentIndex;
        if (Parent < 0 || static_cast<std::size_t>(Parent) >= Positions.size())
            continue;
        const ProjectedPoint Child =
            ProjectPoint(Camera, Projection, Positions[Index]);
        const ProjectedPoint ParentPoint =
            ProjectPoint(Camera, Projection, Positions[Parent]);
        if (!Child.Visible || !ParentPoint.Visible)
            continue;
        const float ParticipationOpacity =
            Bones[Index].ParticipatesInIk ? 1.0F : 0.28F;
        DrawList->AddLine(
            ToScreen(Viewport, ParentPoint),
            ToScreen(Viewport, Child),
            WithOpacity(Color, Opacity * ParticipationOpacity),
            Bones[Index].ParticipatesInIk ? Thickness : 1.0F);
    }
    for (std::size_t Index = 0; Index < Bones.size(); ++Index)
    {
        if (!Bones[Index].ParticipatesInIk)
            continue;
        const ProjectedPoint Point =
            ProjectPoint(Camera, Projection, Positions[Index]);
        if (!Point.Visible)
            continue;
        DrawList->AddCircleFilled(
            ToScreen(Viewport, Point),
            std::max(1.5F, Thickness * 1.25F),
            WithOpacity(Color, Opacity));
    }
    DrawList->PopClipRect();
}

void RenderMeshCallback(
    const ImDrawList*,
    const ImDrawCmd* Command)
{
    auto* Data = static_cast<MeshCallbackData*>(
        Command->UserCallbackData);
    if (Data == nullptr || Data->Renderer == nullptr)
        return;
    std::string Error;
    if (!Data->Renderer->Draw(
            Data->Viewport, Data->Camera, Data->Layers.data(),
            Data->LayerCount, Error) &&
        Data->RenderError != nullptr && Data->RenderError->empty())
    {
        *Data->RenderError = std::move(Error);
    }
}

ImVec4 GoalColor(
    const RetargetChain& Chain,
    const std::size_t Index)
{
    if (Chain.Label == "LeftLeg")
        return {0.15F, 0.82F, 1.00F, 1.0F};
    if (Chain.Label == "RightLeg")
        return {1.00F, 0.72F, 0.20F, 1.0F};
    if (Chain.Label == "LeftArm")
        return {1.00F, 0.44F, 0.70F, 1.0F};
    if (Chain.Label == "RightArm")
        return {0.47F, 0.91F, 0.44F, 1.0F};
    static constexpr std::array<ImVec4, 14> Palette = {{
        {0.15F, 0.82F, 1.00F, 1.0F},
        {1.00F, 0.72F, 0.20F, 1.0F},
        {1.00F, 0.44F, 0.70F, 1.0F},
        {0.47F, 0.91F, 0.44F, 1.0F},
        {0.62F, 0.73F, 1.00F, 1.0F},
        {0.95F, 0.53F, 0.30F, 1.0F},
        {0.45F, 0.88F, 0.76F, 1.0F},
        {0.91F, 0.52F, 0.96F, 1.0F},
        {0.96F, 0.86F, 0.37F, 1.0F},
        {0.40F, 0.64F, 0.97F, 1.0F},
        {0.98F, 0.39F, 0.44F, 1.0F},
        {0.54F, 0.95F, 0.56F, 1.0F},
        {0.35F, 0.88F, 0.96F, 1.0F},
        {0.78F, 0.58F, 1.00F, 1.0F}
    }};
    return Palette[Index % Palette.size()];
}

void DrawDashedLine(
    ImDrawList* DrawList,
    const ImVec2& Start,
    const ImVec2& End,
    const ImU32 Color,
    const float Thickness)
{
    const float DeltaX = End.x - Start.x;
    const float DeltaY = End.y - Start.y;
    const float Length = std::sqrt(DeltaX * DeltaX + DeltaY * DeltaY);
    if (Length <= 0.01F) return;
    const float DirectionX = DeltaX / Length;
    const float DirectionY = DeltaY / Length;
    constexpr float Dash = 5.0F;
    constexpr float Gap = 4.0F;
    for (float Offset = 0.0F; Offset < Length; Offset += Dash + Gap)
    {
        const float EndOffset = std::min(Length, Offset + Dash);
        DrawList->AddLine(
            {Start.x + DirectionX * Offset,
             Start.y + DirectionY * Offset},
            {Start.x + DirectionX * EndOffset,
             Start.y + DirectionY * EndOffset},
            Color, Thickness);
    }
}

void DrawGoalPath(
    ImDrawList* DrawList,
    const PanelViewport& Viewport,
    const OrbitCamera& Camera,
    const std::vector<Vec3>& Points,
    const ImVec4& Color,
    const float Opacity,
    const bool Dashed)
{
    if (Points.size() < 2) return;
    const ProjectionViewport Projection = {
        Viewport.Maximum.x - Viewport.Minimum.x,
        Viewport.Maximum.y - Viewport.Minimum.y};
    const ImU32 Packed = WithOpacity(Color, Opacity);
    for (std::size_t Index = 1; Index < Points.size(); ++Index)
    {
        const ProjectedPoint Start =
            ProjectPoint(Camera, Projection, Points[Index - 1]);
        const ProjectedPoint End =
            ProjectPoint(Camera, Projection, Points[Index]);
        if (!Start.Visible || !End.Visible) continue;
        if (Dashed)
        {
            DrawDashedLine(
                DrawList, ToScreen(Viewport, Start),
                ToScreen(Viewport, End), Packed, 1.4F);
        }
        else
        {
            DrawList->AddLine(
                ToScreen(Viewport, Start),
                ToScreen(Viewport, End), Packed, 2.0F);
        }
    }
}

void DrawGoalHistory(
    ImDrawList* DrawList,
    const PanelViewport& Viewport,
    const OrbitCamera& Camera,
    const ReviewScene& Scene,
    const GoalHistory& History,
    const ReviewLane Lane,
    const bool IncludeSourceGhost,
    const bool IncludeFingerGoals)
{
    DrawList->PushClipRect(Viewport.Minimum, Viewport.Maximum, true);
    std::size_t VisibleIndex = 0;
    for (const GoalTrail& Trail : History.Trails)
    {
        if (Trail.ChainIndex >= Scene.RetargetChains.size()) continue;
        const RetargetChain& Chain =
            Scene.RetargetChains[Trail.ChainIndex];
        if (!IncludeFingerGoals && Chain.IkMode != "two_bone")
            continue;
        const ImVec4 Color = GoalColor(Chain, VisibleIndex++);
        switch (Lane)
        {
        case ReviewLane::Original:
            DrawGoalPath(
                DrawList, Viewport, Camera,
                Trail.SourceOriginal, Color, 0.92F, false);
            break;
        case ReviewLane::Fk:
            if (IncludeSourceGhost)
            {
                DrawGoalPath(
                    DrawList, Viewport, Camera,
                    Trail.SourceAlignedFk, Color, 0.28F, true);
            }
            DrawGoalPath(
                DrawList, Viewport, Camera,
                Trail.TargetFk, Color, 0.92F, false);
            break;
        case ReviewLane::Foundation:
            if (IncludeSourceGhost)
            {
                DrawGoalPath(
                    DrawList, Viewport, Camera,
                    Trail.SourceAlignedFoundation, Color, 0.28F, true);
            }
            DrawGoalPath(
                DrawList, Viewport, Camera,
                Trail.TargetFoundation, Color, 0.92F, false);
            break;
        case ReviewLane::Final:
            DrawGoalPath(
                DrawList, Viewport, Camera,
                Trail.TargetFinal, Color, 0.92F, false);
            break;
        }
    }
    DrawList->PopClipRect();
}

void DrawGoalMarkerSet(
    ImDrawList* DrawList,
    const PanelViewport& Viewport,
    const OrbitCamera& Camera,
    const ReviewScene& Scene,
    const std::vector<Vec3>& Positions,
    const bool SourceSide,
    const bool IncludeFingerGoals,
    const float Opacity,
    const bool Labels)
{
    const ProjectionViewport Projection = {
        Viewport.Maximum.x - Viewport.Minimum.x,
        Viewport.Maximum.y - Viewport.Minimum.y};
    std::size_t VisibleIndex = 0;
    for (const RetargetChain& Chain : Scene.RetargetChains)
    {
        if (Chain.IkMode == "fk_only") continue;
        if (!IncludeFingerGoals && Chain.IkMode != "two_bone")
            continue;
        const int GoalIndex = SourceSide
            ? Chain.SourceGoalBone : Chain.TargetGoalBone;
        const int PoleIndex = SourceSide
            ? Chain.SourcePoleBone : Chain.TargetPoleBone;
        if (GoalIndex < 0 || PoleIndex < 0 ||
            static_cast<std::size_t>(GoalIndex) >= Positions.size() ||
            static_cast<std::size_t>(PoleIndex) >= Positions.size())
        {
            ++VisibleIndex;
            continue;
        }
        const ImVec4 Color = GoalColor(Chain, VisibleIndex++);
        const ProjectedPoint Goal = ProjectPoint(
            Camera, Projection,
            Positions[static_cast<std::size_t>(GoalIndex)]);
        const ProjectedPoint Pole = ProjectPoint(
            Camera, Projection,
            Positions[static_cast<std::size_t>(PoleIndex)]);
        if (!Goal.Visible || !Pole.Visible) continue;
        const ImVec2 GoalScreen = ToScreen(Viewport, Goal);
        const ImVec2 PoleScreen = ToScreen(Viewport, Pole);
        const ImU32 Packed = WithOpacity(Color, Opacity);
        DrawList->AddLine(
            PoleScreen, GoalScreen,
            WithOpacity(Color, Opacity * 0.45F), 1.0F);
        const float Radius = Chain.IkMode == "two_bone" ? 5.0F : 3.4F;
        DrawList->AddCircleFilled(GoalScreen, Radius, Packed);
        DrawList->AddCircle(
            GoalScreen, Radius, IM_COL32(20, 23, 31, 220),
            0, 1.2F);
        const float PoleRadius = std::max(2.5F, Radius * 0.72F);
        const ImVec2 Diamond[4] = {
            {PoleScreen.x, PoleScreen.y - PoleRadius},
            {PoleScreen.x + PoleRadius, PoleScreen.y},
            {PoleScreen.x, PoleScreen.y + PoleRadius},
            {PoleScreen.x - PoleRadius, PoleScreen.y}};
        DrawList->AddConvexPolyFilled(Diamond, 4, Packed);
        if (Labels && Chain.IkMode == "two_bone")
        {
            const ImVec2 LabelSize =
                ImGui::CalcTextSize(Chain.Label.c_str());
            const bool OnLeftHalf = GoalScreen.x <
                (Viewport.Minimum.x + Viewport.Maximum.x) * 0.5F;
            DrawList->AddText(
                {OnLeftHalf
                    ? GoalScreen.x - Radius - 3.0F - LabelSize.x
                    : GoalScreen.x + Radius + 3.0F,
                 GoalScreen.y - Radius - 1.0F},
                WithOpacity(Color, Opacity), Chain.Label.c_str());
        }
    }
}

void DrawCurrentGoals(
    ImDrawList* DrawList,
    const PanelViewport& Viewport,
    const OrbitCamera& Camera,
    const ReviewScene& Scene,
    const PanelDefinition& Definition,
    const bool IncludeFingerGoals)
{
    DrawList->PushClipRect(Viewport.Minimum, Viewport.Maximum, true);
    if (Definition.GhostPositions != nullptr)
    {
        DrawGoalMarkerSet(
            DrawList, Viewport, Camera, Scene,
            *Definition.GhostPositions, true,
            IncludeFingerGoals, 0.32F, false);
    }
    DrawGoalMarkerSet(
        DrawList, Viewport, Camera, Scene,
        *Definition.MainPositions,
        Definition.Lane == ReviewLane::Original,
        IncludeFingerGoals, 1.0F,
        Definition.Lane != ReviewLane::Original &&
            Definition.GhostPositions == nullptr &&
            !IncludeFingerGoals);
    DrawList->PopClipRect();
}

void ApplyCameraInput(
    OrbitCamera& Camera,
    const PanelViewport& Viewport)
{
    if (!Viewport.Hovered)
        return;
    ImGuiIO& Io = ImGui::GetIO();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
        RotateCamera(Camera, Io.MouseDelta.x, Io.MouseDelta.y);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        PanCamera(
            Camera,
            Io.MouseDelta.x,
            Io.MouseDelta.y,
            Viewport.Maximum.y - Viewport.Minimum.y);
    }
    if (Io.MouseWheel != 0.0F)
        ZoomCamera(Camera, Io.MouseWheel);
}

PanelViewport DrawPanel(
    const PanelDefinition& Definition,
    OrbitCamera& Camera,
    const DisplayVisibility& Visibility,
    const ImVec2& Size,
    const ReviewScene& Scene,
    const GoalHistory& GoalTrails,
    MeshRenderer& Renderer,
    const ImVec2& MainViewportPosition,
    const float FramebufferScaleX,
    const float FramebufferScaleY,
    const int FramebufferHeight,
    std::string& RenderError)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::BeginChild(
        Definition.Id,
        Size,
        ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* DrawList = ImGui::GetWindowDrawList();
    const ImVec2 WindowMinimum = ImGui::GetWindowPos();
    const ImVec2 WindowMaximum = {
        WindowMinimum.x + ImGui::GetWindowSize().x,
        WindowMinimum.y + ImGui::GetWindowSize().y};
    DrawList->AddRectFilled(
        WindowMinimum, WindowMaximum, IM_COL32(31, 32, 39, 255));
    DrawList->AddLine(
        {WindowMinimum.x, WindowMinimum.y + PanelHeaderHeight},
        {WindowMaximum.x, WindowMinimum.y + PanelHeaderHeight},
        IM_COL32(74, 77, 89, 255));
    DrawList->AddText(
        {WindowMinimum.x + 10.0F, WindowMinimum.y + 7.0F},
        IM_COL32(242, 244, 250, 255), Definition.Title);
    DrawList->AddText(
        {WindowMinimum.x + 10.0F, WindowMinimum.y + 26.0F},
        IM_COL32(154, 159, 174, 255), Definition.Subtitle);

    PanelViewport Viewport;
    Viewport.Minimum = {
        WindowMinimum.x + 1.0F,
        WindowMinimum.y + PanelHeaderHeight + 1.0F};
    Viewport.Maximum = {
        WindowMaximum.x - 1.0F,
        WindowMaximum.y - 1.0F};
    ImGui::SetCursorScreenPos(Viewport.Minimum);
    ImGui::InvisibleButton(
        "canvas",
        {Viewport.Maximum.x - Viewport.Minimum.x,
         Viewport.Maximum.y - Viewport.Minimum.y},
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight);
    // Keep the captured panel active while a drag leaves its rectangle, so a
    // single orbit/pan gesture remains continuous instead of stuttering at
    // panel borders.
    Viewport.Hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();

    if (Visibility.Grid)
        DrawGrid(DrawList, Viewport, Camera);
    if (Visibility.Mesh && Definition.MainMesh != nullptr &&
        Definition.MainMeshPositions != nullptr)
    {
        MeshCallbackData Callback;
        Callback.Renderer = &Renderer;
        Callback.Viewport = {
            Viewport.Minimum.x - MainViewportPosition.x,
            Viewport.Minimum.y - MainViewportPosition.y,
            Viewport.Maximum.x - MainViewportPosition.x,
            Viewport.Maximum.y - MainViewportPosition.y,
            FramebufferScaleX,
            FramebufferScaleY,
            FramebufferHeight};
        Callback.Camera = Camera;
        Callback.RenderError = &RenderError;
        Callback.Layers[Callback.LayerCount++] = {
            Definition.MainMesh,
            Definition.MainMeshPositions,
            {Definition.MainColor.x, Definition.MainColor.y,
             Definition.MainColor.z,
             Definition.MainColor.w * Definition.MainOpacity},
            false};
        if (Definition.GhostMesh != nullptr &&
            Definition.GhostMeshPositions != nullptr)
        {
            Callback.Layers[Callback.LayerCount++] = {
                Definition.GhostMesh,
                Definition.GhostMeshPositions,
                {0.38F, 0.68F, 0.94F, Definition.GhostOpacity},
                true};
        }
        DrawList->AddCallback(
            RenderMeshCallback, &Callback, sizeof(Callback));
        DrawList->AddCallback(
            ImDrawCallback_ResetRenderState, nullptr);
    }
    if (Visibility.GoalHistory)
    {
        DrawGoalHistory(
            DrawList, Viewport, Camera, Scene, GoalTrails,
            Definition.Lane, Definition.GhostPositions != nullptr,
            Visibility.IncludeFingerGoals);
    }
    if (Visibility.Skeleton && Definition.GhostPositions != nullptr)
    {
        DrawSkeleton(
            DrawList,
            Viewport,
            Camera,
            *Definition.GhostBones,
            *Definition.GhostPositions,
            {0.38F, 0.68F, 0.94F, 1.0F},
            Definition.GhostOpacity,
            1.5F);
    }
    if (Visibility.Skeleton)
    {
        DrawSkeleton(
            DrawList,
            Viewport,
            Camera,
            *Definition.Bones,
            *Definition.MainPositions,
            Definition.MainColor,
            Definition.MainOpacity,
            2.0F);
    }
    if (Visibility.IkGoals)
    {
        DrawCurrentGoals(
            DrawList, Viewport, Camera, Scene,
            Definition, Visibility.IncludeFingerGoals);
    }

    if (Viewport.Hovered)
    {
        DrawList->AddRect(
            Viewport.Minimum, Viewport.Maximum,
            IM_COL32(113, 149, 213, 170), 0.0F, 0, 1.5F);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    return Viewport;
}

void ConfigureStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& Style = ImGui::GetStyle();
    Style.WindowRounding = 0.0F;
    Style.ChildRounding = 0.0F;
    Style.FrameRounding = 3.0F;
    Style.WindowBorderSize = 0.0F;
    Style.ChildBorderSize = 1.0F;
    Style.ItemSpacing = {8.0F, 6.0F};
    Style.Colors[ImGuiCol_WindowBg] = {0.075F, 0.078F, 0.095F, 1.0F};
    Style.Colors[ImGuiCol_ChildBg] = {0.12F, 0.125F, 0.15F, 1.0F};
    Style.Colors[ImGuiCol_Border] = {0.30F, 0.31F, 0.36F, 1.0F};
    Style.Colors[ImGuiCol_Button] = {0.20F, 0.23F, 0.30F, 1.0F};
    Style.Colors[ImGuiCol_ButtonHovered] = {0.28F, 0.34F, 0.46F, 1.0F};
    Style.Colors[ImGuiCol_ButtonActive] = {0.34F, 0.43F, 0.59F, 1.0F};
}

std::filesystem::path ConfigureChineseFont(
    const std::filesystem::path& ExecutablePath)
{
    ImGuiIO& Io = ImGui::GetIO();
    std::vector<std::filesystem::path> Candidates = {
        ExecutablePath.parent_path() / "fonts" /
            "NotoSansCJKsc-Regular.otf",
        ExecutablePath.parent_path() / "fonts" /
            "NotoSansSC-Regular.ttf"};
#if defined(_WIN32)
    Candidates.emplace_back("C:/Windows/Fonts/msyh.ttc");
    Candidates.emplace_back("C:/Windows/Fonts/msyhbd.ttc");
    Candidates.emplace_back("C:/Windows/Fonts/simhei.ttf");
#elif defined(__APPLE__)
    Candidates.emplace_back("/System/Library/Fonts/PingFang.ttc");
#else
    Candidates.emplace_back(
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    Candidates.emplace_back(
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc");
    Candidates.emplace_back(
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf");
#endif
    for (const std::filesystem::path& Candidate : Candidates)
    {
        std::error_code Error;
        if (!std::filesystem::is_regular_file(Candidate, Error) || Error)
            continue;
        const std::string FontPath = PathToUtf8(Candidate);
        if (Io.Fonts->AddFontFromFileTTF(FontPath.c_str(), 17.0F) != nullptr)
            return Candidate;
    }
    Io.Fonts->AddFontDefault();
    return {};
}

bool CaptureFramebufferPpm(
    GLFWwindow* Window,
    const std::filesystem::path& OutputPath)
{
    int Width = 0;
    int Height = 0;
    glfwGetFramebufferSize(Window, &Width, &Height);
    if (Width <= 0 || Height <= 0)
        return false;
    std::vector<unsigned char> Pixels(
        static_cast<std::size_t>(Width) *
        static_cast<std::size_t>(Height) * 3U);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, Width, Height, GL_RGB, GL_UNSIGNED_BYTE, Pixels.data());

    std::ofstream Stream(OutputPath, std::ios::binary);
    if (!Stream)
        return false;
    Stream << "P6\n" << Width << ' ' << Height << "\n255\n";
    const std::size_t RowBytes = static_cast<std::size_t>(Width) * 3U;
    for (int Row = Height - 1; Row >= 0; --Row)
    {
        Stream.write(
            reinterpret_cast<const char*>(
                Pixels.data() + static_cast<std::size_t>(Row) * RowBytes),
            static_cast<std::streamsize>(RowBytes));
    }
    return static_cast<bool>(Stream);
}

void GlfwErrorCallback(const int Code, const char* Description)
{
    std::cerr << "GLFW error " << Code << ": "
              << (Description != nullptr ? Description : "unknown") << '\n';
}
} // namespace

int RunNativeViewer(
    std::optional<ReviewScene> Scene,
    const NativeViewerOptions& Options)
{
    glfwSetErrorCallback(GlfwErrorCallback);
    if (glfwInit() != GLFW_TRUE)
    {
        std::cerr << "native_viewer_error=glfw_init_failed\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    if (Options.HiddenWindow)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* Window = glfwCreateWindow(
        1800, 820, "SKRTG 重定向审查器", nullptr, nullptr);
    if (Window == nullptr)
    {
        std::cerr << "native_viewer_error=window_creation_failed\n";
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(Window);
    glfwSwapInterval(Options.HiddenWindow ? 0 : 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& Io = ImGui::GetIO();
    Io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ConfigureStyle();
    const std::filesystem::path ChineseFont =
        ConfigureChineseFont(Options.ExecutablePath);
    if (!ImGui_ImplGlfw_InitForOpenGL(Window, true) ||
        !ImGui_ImplOpenGL3_Init("#version 330 core"))
    {
        std::cerr << "native_viewer_error=imgui_backend_init_failed\n";
        ImGui::DestroyContext();
        glfwDestroyWindow(Window);
        glfwTerminate();
        return 1;
    }

    MeshRenderer Renderer;
    std::string MeshRendererError;
    if (!Renderer.Initialize(MeshRendererError))
    {
        std::cerr << "native_viewer_error=mesh_renderer_init_failed "
                  << MeshRendererError << '\n';
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(Window);
        glfwTerminate();
        return 1;
    }

    RetargetBridgeUi BridgeUi(Options.ExecutablePath);
    if (Options.InitialFootLockDisplayEnabled.has_value())
    {
        BridgeUi.SetSourceMotionFootLockEnabled(
            *Options.InitialFootLockDisplayEnabled);
    }
    if (Options.OpenBatchPickerOnStart)
        BridgeUi.OpenBatch();
    else if (Options.OpenInputPickerOnStart || !Scene.has_value())
        BridgeUi.Open();
    SceneDisplayData Display;
    DisplayVisibility Visibility;
    OrbitCamera DefaultCamera;
    OrbitCamera Camera = DefaultCamera;
    bool CameraFollowEnabled = Options.StartCameraFollowOnLaunch;
    ReviewLane CameraFollowLane = ReviewLane::Final;
    std::vector<CameraFollowTarget> CameraFollowTargets;
    std::size_t CameraFollowTargetIndex = 1;
    std::optional<Vec3> LastCameraFollowPoint;
    PlaybackController Playback;
    bool ShowOperationStack = false;
    bool ShowAbout = false;
    int RenderedFrames = 0;
    bool CaptureSucceeded = Options.CapturePpmPath.empty();
    std::string SceneLoadMessage;
    std::string MeshRenderError;
    bool LastFootLockDisplayEnabled =
        BridgeUi.SourceMotionFootLockEnabled();

    auto DisplayedResultLane = [&]()
    {
        return Scene.has_value()
            ? ResolveFootLockDisplayLane(
                *Scene, BridgeUi.SourceMotionFootLockEnabled())
            : ReviewLane::Final;
    };
    auto DisplayedResultPose = [&]() -> const PoseLane&
    {
        return DisplayedResultLane() == ReviewLane::Foundation
            ? Scene->Foundation : Scene->Final;
    };

    auto BuildSceneDisplay = [](
        const ReviewScene& Current,
        SceneDisplayData& OutDisplay,
        std::string& OutError)
    {
        SceneDisplayData Candidate;
        Candidate.FkGhostBones =
            BuildAnchorAlignedSourceGhost(Current, Current.Fk);
        Candidate.FoundationGhostBones =
            BuildAnchorAlignedSourceGhost(Current, Current.Foundation);
        MeshSkinningResult SourceSkin =
            SkinMeshPackage(Current.SourceMesh, Current.Source);
        MeshSkinningResult FkSkin =
            SkinMeshPackage(Current.TargetMesh, Current.Fk);
        MeshSkinningResult FoundationSkin =
            SkinMeshPackage(Current.TargetMesh, Current.Foundation);
        MeshSkinningResult FinalSkin =
            SkinMeshPackage(Current.TargetMesh, Current.Final);
        if (Candidate.FkGhostBones.size() != Current.SourceBones.size() ||
            Candidate.FoundationGhostBones.size() !=
                Current.SourceBones.size() ||
            !SourceSkin.Success || !FkSkin.Success ||
            !FoundationSkin.Success || !FinalSkin.Success)
        {
            OutError = "蒙皮显示数据生成失败。";
            const std::array<const MeshSkinningResult*, 4> Results = {
                &SourceSkin, &FkSkin, &FoundationSkin, &FinalSkin};
            for (const MeshSkinningResult* Result : Results)
            {
                if (!Result->Errors.empty())
                {
                    OutError += " " + Result->Errors.front();
                    break;
                }
            }
            return false;
        }
        Candidate.SourceMesh = std::move(SourceSkin.MeshPositions);
        Candidate.FkMesh = std::move(FkSkin.MeshPositions);
        Candidate.FoundationMesh =
            std::move(FoundationSkin.MeshPositions);
        Candidate.FinalMesh = std::move(FinalSkin.MeshPositions);
        Candidate.FkGhostMesh = BuildAnchorAlignedSourceMesh(
            Current, Current.Fk, Candidate.SourceMesh);
        Candidate.FoundationGhostMesh = BuildAnchorAlignedSourceMesh(
            Current, Current.Foundation, Candidate.SourceMesh);
        if (Candidate.FkGhostMesh.size() !=
                Candidate.SourceMesh.size() ||
            Candidate.FoundationGhostMesh.size() !=
                Candidate.SourceMesh.size())
        {
            OutError = "源 Mesh 的 anchor-aligned Ghost 生成失败。";
            return false;
        }
        const GoalHistoryLoadResult GoalHistoryResult =
            LoadReviewGoalHistory(Current, 50);
        if (!GoalHistoryResult.Success)
        {
            OutError = "IK Goal 最近 50 帧轨迹读取失败。";
            if (!GoalHistoryResult.Errors.empty())
                OutError += " " + GoalHistoryResult.Errors.front();
            return false;
        }
        Candidate.GoalTrails = GoalHistoryResult.History;
        OutDisplay = std::move(Candidate);
        OutError.clear();
        return true;
    };
    auto ResetCameraForScene = [&]()
    {
        if (!Scene.has_value()) return;
        DefaultCamera = FitCameraToBounds(
            ComputeBounds(DisplayedResultPose().ModelPositions), 0.64F);
        Camera = DefaultCamera;
    };
    auto SynchronizeFollowCamera = [&](const bool ResetBaseline)
    {
        if (!Scene.has_value() || !CameraFollowEnabled ||
            CameraFollowTargetIndex >= CameraFollowTargets.size())
        {
            LastCameraFollowPoint.reset();
            return;
        }
        Vec3 CurrentPoint;
        const ReviewLane EffectiveLane =
            CameraFollowLane == ReviewLane::Final
                ? DisplayedResultLane() : CameraFollowLane;
        if (!ResolveCameraFollowPoint(
                *Scene, EffectiveLane,
                CameraFollowTargets[CameraFollowTargetIndex],
                CurrentPoint))
        {
            LastCameraFollowPoint.reset();
            return;
        }
        if (!ResetBaseline && LastCameraFollowPoint.has_value())
        {
            FollowCameraTargetDelta(
                Camera, *LastCameraFollowPoint, CurrentPoint);
        }
        LastCameraFollowPoint = CurrentPoint;
    };
    auto FocusCameraOnCurrentPelvis = [&]()
    {
        if (!Scene.has_value()) return;
        const CameraFollowTarget PelvisTarget = {
            "Pelvis / Hips",
            Scene->SourcePelvisBone,
            Scene->TargetHipsBone};
        Vec3 PelvisPoint;
        if (!ResolveCameraFollowPoint(
                *Scene, DisplayedResultLane(),
                PelvisTarget, PelvisPoint) ||
            !FocusCameraAtPoint(Camera, PelvisPoint))
        {
            SceneLoadMessage =
                "无法聚焦当前 Pelvis / Hips：当前帧没有有效坐标。";
            return;
        }
        // A one-shot focus changes only the orbit pivot. Keep the selected
        // follow mode/target, but reset its baseline so the next frame cannot
        // apply a stale delta after this explicit camera move.
        SynchronizeFollowCamera(true);
        SceneLoadMessage =
            "相机已聚焦到当前帧结果角色的 Pelvis / Hips；视角、距离和投影保持不变。";
    };
    auto RefreshFootLockDisplayState = [&](const bool Announce)
    {
        const bool Enabled = BridgeUi.SourceMotionFootLockEnabled();
        if (Enabled == LastFootLockDisplayEnabled) return;
        LastFootLockDisplayEnabled = Enabled;
        SynchronizeFollowCamera(true);
        if (!Announce || !Scene.has_value()) return;
        const ReviewClipInfo* Clip = CurrentReviewClipInfo(*Scene);
        if (!FootLockComparisonAvailable(*Scene))
        {
            SceneLoadMessage =
                "当前 SKRV 没有可验证的 Foot Lock A/B；此开关仅作为下一次重定向参数。";
        }
        else if (Clip != nullptr && !Clip->FootLockHasStoredPoseDelta)
        {
            SceneLoadMessage = Enabled
                ? "Foot Lock 已切到 ON / Final；但该动画的 Foundation 与 Final 完全相同。"
                : "Foot Lock 已切到 OFF / Foundation；该动画的 Foundation 与 Final 完全相同。";
        }
        else
        {
            SceneLoadMessage = Enabled
                ? "Foot Lock 已实时切到 ON / Final：模型、骨架、Goal 与轨迹已同步。"
                : "Foot Lock 已实时切到 OFF / Foundation：模型、骨架、Goal 与轨迹已同步。";
        }
    };
    auto SetFootLockDisplayEnabled = [&](const bool Enabled)
    {
        BridgeUi.SetSourceMotionFootLockEnabled(Enabled);
        RefreshFootLockDisplayState(true);
    };
    auto OpenVerifiedPackage = [&]
        (const std::filesystem::path& Package, const std::size_t ClipIndex)
    {
        ReviewSceneLoadResult Load =
            LoadReviewScene(Package, ClipIndex, 0);
        if (!Load.Success)
        {
            SceneLoadMessage = "已拒绝打开 SKRV：严格校验失败。";
            if (!Load.Errors.empty())
                SceneLoadMessage += " " + Load.Errors.front();
            return false;
        }
        ReviewScene CandidateScene = std::move(Load.Scene);
        SceneDisplayData CandidateDisplay;
        std::string DisplayError;
        if (!BuildSceneDisplay(
                CandidateScene, CandidateDisplay, DisplayError))
        {
            SceneLoadMessage =
                "已拒绝显示 SKRV：Viewer 投影失败。 " + DisplayError;
            return false;
        }
        Scene = std::move(CandidateScene);
        Display = std::move(CandidateDisplay);
        if (IsUEIKJsonCandidateRoute(*Scene))
        {
            BridgeUi.SetSourceMotionFootLockEnabled(false);
            LastFootLockDisplayEnabled = false;
        }
        MeshRenderError.clear();
        CameraFollowTargets = BuildCameraFollowTargets(*Scene);
        CameraFollowTargetIndex = std::min(
            CameraFollowTargetIndex,
            CameraFollowTargets.empty()
                ? std::size_t{0}
                : CameraFollowTargets.size() - 1U);
        Playback.Playing = false;
        Playback.FractionalFrames = 0.0;
        ResetCameraForScene();
        SynchronizeFollowCamera(true);
        SceneLoadMessage = "SKRV 已通过严格校验并打开。";
        return true;
    };
    auto SeekFrame = [&](const std::uint64_t FrameIndex)
    {
        if (!Scene.has_value() ||
            FrameIndex >= Scene->ClipFrameCount)
            return false;
        if (FrameIndex == Scene->FrameIndex)
            return true;
        const std::uint64_t PreviousFrame = Scene->FrameIndex;
        PoseLane PreviousSource = Scene->Source;
        PoseLane PreviousFk = Scene->Fk;
        PoseLane PreviousFoundation = Scene->Foundation;
        PoseLane PreviousFinal = Scene->Final;
        std::vector<std::string> Errors;
        if (!LoadVerifiedReviewSceneFrame(*Scene, FrameIndex, Errors))
        {
            Playback.Playing = false;
            SceneLoadMessage = "帧读取失败；保留上一有效帧。";
            if (!Errors.empty())
                SceneLoadMessage += " " + Errors.front();
            return false;
        }
        SceneDisplayData CandidateDisplay;
        std::string DisplayError;
        if (!BuildSceneDisplay(*Scene, CandidateDisplay, DisplayError))
        {
            Scene->Source = std::move(PreviousSource);
            Scene->Fk = std::move(PreviousFk);
            Scene->Foundation = std::move(PreviousFoundation);
            Scene->Final = std::move(PreviousFinal);
            Scene->FrameIndex = PreviousFrame;
            Playback.Playing = false;
            SceneLoadMessage =
                "帧显示数据生成失败；已恢复上一有效帧。 " +
                DisplayError;
            return false;
        }
        Display = std::move(CandidateDisplay);
        SynchronizeFollowCamera(false);
        return true;
    };
    auto PauseAndSeek = [&](const std::uint64_t FrameIndex)
    {
        Playback.Playing = false;
        Playback.FractionalFrames = 0.0;
        return SeekFrame(FrameIndex);
    };
    auto StepFrame = [&](const int Direction)
    {
        if (!Scene.has_value() || Scene->ClipFrameCount == 0)
            return;
        std::uint64_t Target = Scene->FrameIndex;
        if (Direction < 0 && Target > 0)
            --Target;
        else if (Direction > 0 && Target + 1 < Scene->ClipFrameCount)
            ++Target;
        PauseAndSeek(Target);
    };
    auto TogglePlayback = [&]()
    {
        if (!Scene.has_value() || Scene->ClipFrameCount == 0)
            return;
        if (!Playback.Playing && !Playback.Loop &&
            Scene->FrameIndex + 1 >= Scene->ClipFrameCount)
        {
            PauseAndSeek(0);
        }
        Playback.Playing = !Playback.Playing;
        Playback.FractionalFrames = 0.0;
    };
    auto RequestDisplayedResultExport = [&]()
    {
        if (!Scene.has_value()) return;
        const ReviewLane ResultLane = DisplayedResultLane();
        const std::string Lane = ResultLane == ReviewLane::Foundation
            ? "foundation" : "final";
        const ReviewExportResult Export =
            FindVerifiedReviewExport(*Scene, Lane);
        if (!Export.Success)
        {
            SceneLoadMessage =
                "无法导出当前显示结果 FBX：SKRV 的已验证导出记录校验失败。";
            if (!Export.Errors.empty())
                SceneLoadMessage += " " + Export.Errors.front();
            return;
        }
        BridgeUi.OpenExportDialog(
            Export.SourceFbx,
            Scene->PackageDirectory,
            Export.SuggestedFileName,
            Export.ExpectedSha256);
    };

    if (Scene.has_value())
    {
        std::string DisplayError;
        if (!BuildSceneDisplay(*Scene, Display, DisplayError))
        {
            SceneLoadMessage =
                "初始 Viewer 投影失败。 " + DisplayError;
            Scene.reset();
        }
        else
        {
            if (IsUEIKJsonCandidateRoute(*Scene))
            {
                BridgeUi.SetSourceMotionFootLockEnabled(false);
                LastFootLockDisplayEnabled = false;
            }
            CameraFollowTargets = BuildCameraFollowTargets(*Scene);
            CameraFollowTargetIndex = std::min(
                CameraFollowTargetIndex,
                CameraFollowTargets.empty()
                    ? std::size_t{0}
                    : CameraFollowTargets.size() - 1U);
            ResetCameraForScene();
            SynchronizeFollowCamera(true);
        }
    }
    Playback.Playing =
        Options.StartPlaybackOnLaunch && Scene.has_value();
    double LastFrameTime = glfwGetTime();

    while (!glfwWindowShouldClose(Window))
    {
        glfwPollEvents();

        BridgeUi.Poll();
        if (const std::optional<std::filesystem::path> Package =
                BridgeUi.ConsumeCompletedPackage();
            Package.has_value())
        {
            OpenVerifiedPackage(*Package, 0);
        }
        RefreshFootLockDisplayState(true);
        if (const std::optional<std::filesystem::path> Package =
                BridgeUi.ConsumeRequestedPackage();
            Package.has_value())
        {
            OpenVerifiedPackage(*Package, 0);
        }

        const double Now = glfwGetTime();
        const double DeltaSeconds = Options.FixedDeltaSeconds > 0.0
            ? Options.FixedDeltaSeconds
            : std::clamp(Now - LastFrameTime, 0.0, 0.25);
        LastFrameTime = Now;
        if (Scene.has_value() && Playback.Playing)
        {
            const PlaybackAdvanceResult Advance = AdvancePlayback(
                Playback,
                Scene->FrameIndex,
                Scene->ClipFrameCount,
                Scene->FramesPerSecond,
                DeltaSeconds);
            if (Advance.FrameChanged)
                SeekFrame(Advance.FrameIndex);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        Renderer.BeginFrame();
        const bool UEIKCandidateRoute =
            Scene.has_value() &&
            IsUEIKJsonCandidateRoute(*Scene);

        if (Scene.has_value() && !ImGui::IsAnyItemActive() &&
            !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
                FocusCameraOnCurrentPelvis();
            if (ImGui::IsKeyPressed(ImGuiKey_P, false))
                TogglePlayback();
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
                StepFrame(-1);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
                StepFrame(1);
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
                PauseAndSeek(0);
            if (ImGui::IsKeyPressed(ImGuiKey_End, false) &&
                Scene->ClipFrameCount > 0)
            {
                PauseAndSeek(Scene->ClipFrameCount - 1);
            }
        }
        if (!ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput &&
            ImGui::GetIO().KeyCtrl)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_I, false))
                BridgeUi.Open();
            if (ImGui::GetIO().KeyShift &&
                ImGui::IsKeyPressed(ImGuiKey_B, false))
            {
                BridgeUi.OpenBatch();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_O, false))
                BridgeUi.OpenSkrvPicker();
            if (Scene.has_value() &&
                ImGui::IsKeyPressed(ImGuiKey_E, false))
            {
                RequestDisplayedResultExport();
            }
        }

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("文件"))
            {
                if (ImGui::MenuItem("导入并重定向 FBX...", "Ctrl+I"))
                    BridgeUi.Open();
                if (ImGui::MenuItem(
                        "批量重定向文件夹...", "Ctrl+Shift+B"))
                {
                    BridgeUi.OpenBatch();
                }
                if (ImGui::MenuItem("打开 SKRV 审查包...", "Ctrl+O"))
                    BridgeUi.OpenSkrvPicker();
                ImGui::BeginDisabled(!Scene.has_value());
                const char* ExportLabel =
                    "导出当前 Final FBX...";
                if (Scene.has_value() && FootLockComparisonAvailable(*Scene))
                {
                    ExportLabel =
                        DisplayedResultLane() == ReviewLane::Foundation
                            ? "导出当前 Foot Lock OFF / Foundation FBX..."
                            : "导出当前 Foot Lock ON / Final FBX...";
                }
                if (ImGui::MenuItem(ExportLabel, "Ctrl+E"))
                    RequestDisplayedResultExport();
                ImGui::EndDisabled();
                ImGui::Separator();
                if (ImGui::MenuItem("退出"))
                    glfwSetWindowShouldClose(Window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("动画"))
            {
                ImGui::BeginDisabled(!Scene.has_value());
                if (ImGui::MenuItem(
                        "播放 / 暂停", "P", Playback.Playing))
                    TogglePlayback();
                if (ImGui::MenuItem("跳到首帧", "Home"))
                    PauseAndSeek(0);
                if (ImGui::MenuItem("上一帧", "Left"))
                    StepFrame(-1);
                if (ImGui::MenuItem("下一帧", "Right"))
                    StepFrame(1);
                if (ImGui::MenuItem("跳到末帧", "End") &&
                    Scene.has_value() && Scene->ClipFrameCount > 0)
                {
                    PauseAndSeek(Scene->ClipFrameCount - 1);
                }
                ImGui::Separator();
                ImGui::MenuItem("循环播放", nullptr, &Playback.Loop);
                if (Scene.has_value() && Scene->Clips.size() > 1 &&
                    ImGui::BeginMenu("切换动画片段"))
                {
                    const std::filesystem::path Package =
                        Scene->PackageDirectory;
                    const std::size_t CurrentClip = Scene->ClipIndex;
                    const std::vector<ReviewClipInfo> Clips = Scene->Clips;
                    for (std::size_t Index = 0; Index < Clips.size(); ++Index)
                    {
                        ImGui::PushID(Clips[Index].Id.c_str());
                        if (ImGui::MenuItem(
                                Clips[Index].Label.c_str(), nullptr,
                                Index == CurrentClip))
                        {
                            OpenVerifiedPackage(Package, Index);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("操作栈"))
            {
                if (ImGui::MenuItem(
                        "显示操作栈面板", nullptr, ShowOperationStack))
                    ShowOperationStack = !ShowOperationStack;
                ImGui::Separator();
                ImGui::MenuItem(
                    UEIKCandidateRoute
                        ? "UE IK JSON 候选：FK + Pelvis + 解析式 Limb IK"
                        : "FK / Limb IK / Finger IK（冻结）",
                    nullptr, true, false);
                if (UEIKCandidateRoute)
                {
                    ImGui::MenuItem(
                        "候选路线未选择 / 未采纳；Foundation v1 frozen=true",
                        nullptr, false, false);
                    ImGui::MenuItem(
                        "解析式 Limb IK；不声明 UE FullBodyIK 等价",
                        nullptr, false, false);
                }
                ImGui::BeginDisabled(UEIKCandidateRoute);
                const bool Spine =
                    BridgeUi.SpinePelvisFollowEnabled();
                if (ImGui::MenuItem(
                        "Spine / Pelvis 跟随", nullptr, Spine))
                {
                    BridgeUi.SetSpinePelvisFollowEnabled(!Spine);
                }
                const bool FootLock =
                    BridgeUi.SourceMotionFootLockEnabled();
                if (ImGui::MenuItem(
                        "脚锁定（实时 A/B + 下次运行参数）",
                        nullptr, FootLock))
                {
                    SetFootLockDisplayEnabled(!FootLock);
                }
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("视图"))
            {
                ImGui::BeginDisabled(!Scene.has_value());
                if (ImGui::MenuItem(
                        "正交投影", nullptr, Camera.Orthographic))
                    Camera.Orthographic = !Camera.Orthographic;
                ImGui::SeparatorText("显示层");
                ImGui::MenuItem("Mesh", nullptr, &Visibility.Mesh);
                ImGui::MenuItem("骨骼", nullptr, &Visibility.Skeleton);
                ImGui::MenuItem(
                    "IK Goal / Pole", nullptr, &Visibility.IkGoals);
                ImGui::MenuItem(
                    "Goal 最近 50 帧轨迹", nullptr,
                    &Visibility.GoalHistory);
                ImGui::MenuItem(
                    "包含手指 Goal", nullptr,
                    &Visibility.IncludeFingerGoals);
                ImGui::MenuItem(
                    "显示辅助网格", nullptr, &Visibility.Grid);
                ImGui::SeparatorText("相机");
                if (ImGui::MenuItem(
                        "聚焦当前结果 Pelvis / Hips", "Space"))
                {
                    FocusCameraOnCurrentPelvis();
                }
                if (ImGui::MenuItem(
                        "自由相机", nullptr, !CameraFollowEnabled))
                {
                    CameraFollowEnabled = false;
                    LastCameraFollowPoint.reset();
                }
                if (ImGui::MenuItem(
                        "跟随相机（仅跟随位置）", nullptr,
                        CameraFollowEnabled))
                {
                    CameraFollowEnabled = true;
                    SynchronizeFollowCamera(true);
                }
                if (ImGui::MenuItem("重置相机"))
                {
                    ResetCameraForScene();
                    SynchronizeFollowCamera(true);
                }
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("帮助"))
            {
                if (ImGui::MenuItem("关于 SKRTG"))
                    ShowAbout = true;
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(MainViewport->WorkPos);
        ImGui::SetNextWindowSize(MainViewport->WorkSize);
        ImGui::Begin(
            "SKRTG N4 Root",
            nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextUnformatted("SKRTG 重定向审查器  /  Native Viewer N4.4");
        ImGui::SameLine();
        ImGui::TextDisabled(
            UEIKCandidateRoute
                ? "UE IK JSON 候选 | 路线未选择/未采纳 | Foundation v1 frozen=true | 解析式 Limb IK（非 FullBodyIK 等价）"
                : "冻结 FK + IK 基石 | SKRV v1 未改 | Foot Lock 实时 A/B；其他 Op 设置用于下一次重定向");

        if (!SceneLoadMessage.empty())
            ImGui::TextWrapped("%s", SceneLoadMessage.c_str());
        if (!MeshRenderError.empty())
        {
            ImGui::TextColored(
                {1.0F, 0.42F, 0.38F, 1.0F},
                "Mesh 渲染失败：%s", MeshRenderError.c_str());
        }

        const std::vector<BatchReviewAnimation> BatchAnimations =
            BridgeUi.ReviewableBatchAnimations();
        if (Scene.has_value())
        {
            ImGui::PushID("Transport");
            if (ImGui::Button("|<")) PauseAndSeek(0);
            ImGui::SameLine();
            if (ImGui::Button("<")) StepFrame(-1);
            ImGui::SameLine();
            if (ImGui::Button(Playback.Playing ? "暂停" : "播放"))
                TogglePlayback();
            ImGui::SameLine();
            if (ImGui::Button(">")) StepFrame(1);
            ImGui::SameLine();
            if (ImGui::Button(">|"))
                PauseAndSeek(Scene->ClipFrameCount - 1);
            ImGui::SameLine();
            ImGui::Checkbox("循环", &Playback.Loop);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0F);
            float PlaybackSpeed = static_cast<float>(Playback.Speed);
            if (ImGui::SliderFloat(
                    "速度", &PlaybackSpeed, 0.25F, 2.0F, "%.2fx"))
            {
                Playback.Speed = PlaybackSpeed;
            }
            ImGui::SameLine();
            std::uint64_t RequestedFrame = Scene->FrameIndex;
            const std::uint64_t MinimumFrame = 0;
            const std::uint64_t MaximumFrame = Scene->ClipFrameCount - 1;
            ImGui::SetNextItemWidth(-250.0F);
            if (ImGui::SliderScalar(
                    "##Timeline", ImGuiDataType_U64,
                    &RequestedFrame, &MinimumFrame, &MaximumFrame,
                    "%llu"))
            {
                PauseAndSeek(RequestedFrame);
            }
            ImGui::SameLine();
            ImGui::Text(
                "帧 %llu / %llu   %.2f 秒",
                static_cast<unsigned long long>(Scene->FrameIndex),
                static_cast<unsigned long long>(MaximumFrame),
                Scene->FramesPerSecond > 0.0
                    ? static_cast<double>(Scene->FrameIndex) /
                        Scene->FramesPerSecond
                    : 0.0);
            ImGui::PopID();

            bool HasAnimationSelector = false;
            if (BatchAnimations.size() > 1)
            {
                const std::string CurrentPackage =
                    ComparableReviewPackagePath(
                        Scene->PackageDirectory);
                const BatchReviewAnimation* CurrentAnimation = nullptr;
                for (const BatchReviewAnimation& Animation :
                     BatchAnimations)
                {
                    if (ComparableReviewPackagePath(
                            Animation.ReviewPackage) ==
                        CurrentPackage)
                    {
                        CurrentAnimation = &Animation;
                        break;
                    }
                }
                const std::string Preview = CurrentAnimation != nullptr
                    ? CurrentAnimation->Label
                    : Scene->ClipLabel + "（当前 SKRV）";
                ImGui::SetNextItemWidth(420.0F);
                if (ImGui::BeginCombo("动画", Preview.c_str()))
                {
                    for (const BatchReviewAnimation& Animation :
                         BatchAnimations)
                    {
                        const bool Selected =
                            ComparableReviewPackagePath(
                                Animation.ReviewPackage) ==
                            CurrentPackage;
                        ImGui::PushID(
                            static_cast<int>(Animation.JobIndex));
                        if (ImGui::Selectable(
                                Animation.Label.c_str(), Selected) &&
                            !Selected &&
                            OpenVerifiedPackage(
                                Animation.ReviewPackage, 0))
                        {
                            BridgeUi.SetSelectedBatchReviewJobIndex(
                                Animation.JobIndex);
                            SceneLoadMessage =
                                "已切换动画：" + Animation.Label +
                                "。SKRV 已重新执行严格校验。";
                        }
                        if (Selected) ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                HasAnimationSelector = true;
            }
            if (Scene->Clips.size() > 1)
            {
                if (HasAnimationSelector) ImGui::SameLine();
                ImGui::SetNextItemWidth(420.0F);
                if (ImGui::BeginCombo(
                        "包内片段", Scene->ClipLabel.c_str()))
                {
                    const std::filesystem::path Package =
                        Scene->PackageDirectory;
                    const std::size_t CurrentClip = Scene->ClipIndex;
                    const std::vector<ReviewClipInfo> Clips = Scene->Clips;
                    for (std::size_t Index = 0; Index < Clips.size(); ++Index)
                    {
                        const bool Selected = Index == CurrentClip;
                        ImGui::PushID(Clips[Index].Id.c_str());
                        if (ImGui::Selectable(
                                Clips[Index].Label.c_str(), Selected))
                            OpenVerifiedPackage(Package, Index);
                        if (Selected) ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::Separator();
            bool FootLockDisplayEnabled =
                BridgeUi.SourceMotionFootLockEnabled();
            ImGui::BeginDisabled(UEIKCandidateRoute);
            if (ImGui::Checkbox(
                    UEIKCandidateRoute
                        ? "Foot Lock（此候选路线关闭）"
                        : "Foot Lock 实时 A/B",
                    &FootLockDisplayEnabled))
            {
                SetFootLockDisplayEnabled(FootLockDisplayEnabled);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Checkbox("Mesh##Display", &Visibility.Mesh);
            ImGui::SameLine();
            ImGui::Checkbox("骨骼##Display", &Visibility.Skeleton);
            ImGui::SameLine();
            ImGui::Checkbox("IK Goal / Pole", &Visibility.IkGoals);
            ImGui::SameLine();
            ImGui::Checkbox(
                "Goal 轨迹：最近 50 帧", &Visibility.GoalHistory);
            ImGui::SameLine();
            ImGui::Checkbox("网格", &Visibility.Grid);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(128.0F);
            const char* GoalScope = Visibility.IncludeFingerGoals
                ? "全部 IK（含手指）" : "仅四肢";
            if (ImGui::BeginCombo("##GoalScope", GoalScope))
            {
                if (ImGui::Selectable(
                        "全部 IK（含手指）",
                        Visibility.IncludeFingerGoals))
                    Visibility.IncludeFingerGoals = true;
                if (ImGui::Selectable(
                        "仅四肢", !Visibility.IncludeFingerGoals))
                    Visibility.IncludeFingerGoals = false;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(118.0F);
            if (ImGui::BeginCombo(
                    "##CameraMode",
                    CameraFollowEnabled ? "相机：跟随" : "相机：自由"))
            {
                if (ImGui::Selectable(
                        "自由相机", !CameraFollowEnabled))
                {
                    CameraFollowEnabled = false;
                    LastCameraFollowPoint.reset();
                }
                if (ImGui::Selectable(
                        "跟随相机", CameraFollowEnabled))
                {
                    CameraFollowEnabled = true;
                    SynchronizeFollowCamera(true);
                }
                ImGui::EndCombo();
            }
            if (CameraFollowEnabled)
            {
                ImGui::SameLine();
                static constexpr std::array<const char*, 4> LaneLabels = {
                    "Original", "FK", "FK + IK", "显示结果"};
                const int FollowLaneIndex =
                    static_cast<int>(CameraFollowLane);
                ImGui::SetNextItemWidth(112.0F);
                if (ImGui::BeginCombo(
                        "##FollowLane", LaneLabels[FollowLaneIndex]))
                {
                    for (int Index = 0; Index < 4; ++Index)
                    {
                        if (ImGui::Selectable(
                                LaneLabels[Index],
                                Index == FollowLaneIndex))
                        {
                            CameraFollowLane =
                                static_cast<ReviewLane>(Index);
                            SynchronizeFollowCamera(true);
                        }
                    }
                    ImGui::EndCombo();
                }
                if (!CameraFollowTargets.empty())
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(210.0F);
                    if (ImGui::BeginCombo(
                            "##FollowTarget",
                            CameraFollowTargets[
                                CameraFollowTargetIndex].Label.c_str()))
                    {
                        for (std::size_t Index = 0;
                             Index < CameraFollowTargets.size(); ++Index)
                        {
                            if (ImGui::Selectable(
                                    CameraFollowTargets[Index].Label.c_str(),
                                    Index == CameraFollowTargetIndex))
                            {
                                CameraFollowTargetIndex = Index;
                                SynchronizeFollowCamera(true);
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
            }
            const ReviewScene& Current = *Scene;
            const ReviewLane ResultLane = DisplayedResultLane();
            const bool FootLockAvailable =
                FootLockComparisonAvailable(Current);
            const ReviewClipInfo* Clip = CurrentReviewClipInfo(Current);
            const FootLockDeltaSummary FootLockDelta =
                MeasureFootLockDelta(Current, Display.GoalTrails);
            const std::optional<float> MeshDelta =
                MaximumMeshPositionDelta(
                    Display.FoundationMesh, Display.FinalMesh);
            ImGui::Text(
                "%.3f fps  |  四视图同步帧 / 相机 / 投影 / 显示尺度  |  Goal 历史 %zu 帧",
                Current.FramesPerSecond,
                Display.GoalTrails.SampleCount);
            if (!FootLockAvailable)
            {
                ImGui::TextColored(
                    {1.0F, 0.66F, 0.25F, 1.0F},
                    UEIKCandidateRoute
                        ? "UE IK JSON 候选：Foot Lock 关闭；当前显示候选 Final。路线未选择、未采纳，且不声明 FullBodyIK 等价。"
                        : "Foot Lock：当前 SKRV 无可验证 A/B；开关只影响下一次重定向。当前仍显示 Final。");
            }
            else if (Clip != nullptr && !Clip->FootLockHasStoredPoseDelta)
            {
                ImGui::TextColored(
                    {1.0F, 0.76F, 0.30F, 1.0F},
                    "Foot Lock %s · 显示 %s | 该片段 Foundation = Final（整段 pose 哈希相同，无可见修正）",
                    BridgeUi.SourceMotionFootLockEnabled() ? "ON" : "OFF",
                    ResultLane == ReviewLane::Final ? "Final" : "Foundation");
            }
            else
            {
                ImGui::TextColored(
                    ResultLane == ReviewLane::Final
                        ? ImVec4{0.43F, 0.88F, 0.54F, 1.0F}
                        : ImVec4{0.42F, 0.72F, 1.0F, 1.0F},
                    "Foot Lock %s · 实时显示 %s | 本帧 Δ：左脚 Goal %.4f cm · 右脚 Goal %.4f cm · 最大骨点 %.4f cm · 最大骨旋转 %.4f° · Mesh %.4f cm | 近 50 帧脚 Goal 轨迹 Δmax %.4f cm",
                    BridgeUi.SourceMotionFootLockEnabled() ? "ON" : "OFF",
                    ResultLane == ReviewLane::Final ? "Final" : "Foundation",
                    FootLockDelta.LeftFootGoalPositionCm,
                    FootLockDelta.RightFootGoalPositionCm,
                    FootLockDelta.MaximumBonePositionCm,
                    FootLockDelta.MaximumBoneRotationDegrees,
                    MeshDelta.value_or(0.0F),
                    FootLockDelta.MaximumRecentFootGoalPositionCm);
            }
            ImGui::TextDisabled(
                "右键旋转  |  左键平移  |  滚轮缩放  |  跟随模式只平移焦点、不旋转视角  |  源 Ghost：%s anchor-aligned（10%%）",
                Current.GhostAnchor.Label.c_str());

            const ImVec2 Available = ImGui::GetContentRegionAvail();
            const float PanelWidth =
                std::max(1.0F, (Available.x - PanelGap * 3.0F) * 0.25F);
            const ImVec2 PanelSize = {PanelWidth, Available.y};
            const bool ShowFoundationResult =
                ResultLane == ReviewLane::Foundation;
            const char* ResultTitle = ShowFoundationResult
                ? "对照结果 · Foot Lock OFF"
                : UEIKCandidateRoute
                    ? "最终结果 · UE IK JSON 候选"
                    : FootLockAvailable
                        ? "最终结果 · Foot Lock ON"
                        : "最终结果 · Final";
            const char* ResultSubtitle = ShowFoundationResult
                ? "锁脚前 Foundation（独立显示）"
                : UEIKCandidateRoute
                    ? "候选 Final（无 Foot Lock A/B）"
                    : FootLockAvailable
                        ? "锁脚后 Final（独立显示）"
                        : "Final（独立显示）";
            const std::vector<Vec3>* ResultPositions = ShowFoundationResult
                ? &Current.Foundation.ModelPositions
                : &Current.Final.ModelPositions;
            const std::vector<std::vector<Vec3>>* ResultMesh =
                ShowFoundationResult
                    ? &Display.FoundationMesh : &Display.FinalMesh;
            const std::array<PanelDefinition, 4> Panels = {{
                {"OriginalPanel", "原动画 Original", "源角色原始空间",
                 &Current.SourceBones, &Current.Source.ModelPositions,
                 nullptr, nullptr,
                 &Current.SourceMesh, &Display.SourceMesh,
                 nullptr, nullptr, ReviewLane::Original,
                 {0.34F, 0.67F, 0.93F, 1.0F}, 1.0F,
                 Current.SourceGhostOpacity},
                {"FkPanel", "FK + 原动画 10%",
                 "目标 FK + anchor-aligned 源 Ghost",
                 &Current.TargetBones, &Current.Fk.ModelPositions,
                 &Current.SourceBones, &Display.FkGhostBones,
                 &Current.TargetMesh, &Display.FkMesh,
                 &Current.SourceMesh, &Display.FkGhostMesh,
                 ReviewLane::Fk,
                 {0.93F, 0.66F, 0.29F, 1.0F}, 1.0F,
                Current.SourceGhostOpacity},
                {"FoundationPanel", "FK + IK + 原动画 10%",
                 UEIKCandidateRoute
                    ? "候选 FK + 解析式 Limb IK + anchor-aligned 源 Ghost"
                    : "冻结基石 + anchor-aligned 源 Ghost",
                 &Current.TargetBones, &Current.Foundation.ModelPositions,
                 &Current.SourceBones, &Display.FoundationGhostBones,
                 &Current.TargetMesh, &Display.FoundationMesh,
                 &Current.SourceMesh, &Display.FoundationGhostMesh,
                 ReviewLane::Foundation,
                 {0.73F, 0.53F, 0.94F, 1.0F}, 1.0F,
                 Current.SourceGhostOpacity},
                {"FinalPanel", ResultTitle, ResultSubtitle,
                 &Current.TargetBones, ResultPositions,
                 nullptr, nullptr,
                 &Current.TargetMesh, ResultMesh,
                 nullptr, nullptr, ResultLane,
                 {0.75F, 0.55F, 0.96F, 1.0F}, 1.0F,
                 Current.SourceGhostOpacity}
            }};
            int WindowWidth = 1;
            int WindowHeight = 1;
            int FramebufferWidth = 1;
            int FramebufferHeight = 1;
            glfwGetWindowSize(Window, &WindowWidth, &WindowHeight);
            glfwGetFramebufferSize(
                Window, &FramebufferWidth, &FramebufferHeight);
            const float FramebufferScaleX =
                static_cast<float>(FramebufferWidth) /
                static_cast<float>(std::max(WindowWidth, 1));
            const float FramebufferScaleY =
                static_cast<float>(FramebufferHeight) /
                static_cast<float>(std::max(WindowHeight, 1));
            std::array<PanelViewport, 4> PanelViewports{};
            for (std::size_t Index = 0; Index < Panels.size(); ++Index)
            {
                if (Index != 0)
                    ImGui::SameLine(0.0F, PanelGap);
                PanelViewports[Index] = DrawPanel(
                    Panels[Index], Camera, Visibility, PanelSize,
                    Current, Display.GoalTrails, Renderer,
                    MainViewport->Pos, FramebufferScaleX,
                    FramebufferScaleY, FramebufferHeight,
                    MeshRenderError);
            }
            // Apply input only after every panel has emitted this frame's draw
            // commands. The next frame then renders all four panels with the
            // same updated camera; no earlier panel can retain an old camera
            // while a later panel uses a new one.
            for (const PanelViewport& Viewport : PanelViewports)
            {
                if (Viewport.Hovered)
                {
                    ApplyCameraInput(Camera, Viewport);
                    break;
                }
            }
        }
        else
        {
            ImGui::BeginChild(
                "NoReviewLoaded", {0.0F, 0.0F}, ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("当前没有打开 SKRV 审查包。 ");
            if (!BatchAnimations.empty())
            {
                ImGui::TextWrapped(
                    "当前批次已有 %llu 条成功动画；请选择一条进入四视图审查。",
                    static_cast<unsigned long long>(
                        BatchAnimations.size()));
                const std::string Preview =
                    "选择批次动画（" +
                    std::to_string(BatchAnimations.size()) + " 条）";
                ImGui::SetNextItemWidth(420.0F);
                if (ImGui::BeginCombo(
                        "动画", Preview.c_str()))
                {
                    for (const BatchReviewAnimation& Animation :
                         BatchAnimations)
                    {
                        ImGui::PushID(
                            static_cast<int>(Animation.JobIndex));
                        if (ImGui::Selectable(
                                Animation.Label.c_str()) &&
                            OpenVerifiedPackage(
                                Animation.ReviewPackage, 0))
                        {
                            BridgeUi.SetSelectedBatchReviewJobIndex(
                                Animation.JobIndex);
                            SceneLoadMessage =
                                "已打开动画：" + Animation.Label +
                                "。SKRV 已执行严格校验。";
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::Separator();
            }
            ImGui::TextWrapped(
                "请从“文件”菜单导入并重定向 FBX，或打开已有 SKRV。Bridge 会在独立进程中运行冻结 Retargeter；只有通过严格校验的 SKRV 才会显示。 ");
            ImGui::EndChild();
        }
        ImGui::End();
        BridgeUi.Draw();
        BridgeUi.DrawOperationStackWindow(&ShowOperationStack);

        if (ShowAbout)
        {
            ImGui::SetNextWindowSize({520.0F, 250.0F}, ImGuiCond_FirstUseEver);
            if (ImGui::Begin("关于 SKRTG", &ShowAbout))
            {
                ImGui::TextUnformatted("SKRTG Native Viewer N4.4");
                ImGui::Separator();
                ImGui::TextWrapped(
                    "原生跨平台审查器：GLFW + OpenGL 3.3 Core + Dear ImGui。");
                ImGui::TextWrapped(
                    "本阶段只改 Viewer 工作流与可视化；Retargeter 数值算法和 SKRV v1 数据合同保持冻结。 ");
                ImGui::TextWrapped(
                    "Mesh、骨骼、IK Goal / Pole 可独立显示；Goal 轨迹严格读取当前帧及此前最多 49 帧。相机可在自由与仅位置跟随之间切换。 ");
                ImGui::TextWrapped(
                    "Foot Lock 开关会在含可验证 A/B 的 SKRV 中实时切换 Foundation / Final；模型、骨架、Goal、50 帧轨迹、结果相机跟随与导出使用同一 lane。 ");
                ImGui::TextWrapped(
                    "核心 FK / Limb IK / Finger IK 没有可独立关闭的 CLI，因此界面明确显示为冻结项。 ");
            }
            ImGui::End();
        }

        ImGui::Render();
        int FramebufferWidth = 0;
        int FramebufferHeight = 0;
        glfwGetFramebufferSize(Window, &FramebufferWidth, &FramebufferHeight);
        glViewport(0, 0, FramebufferWidth, FramebufferHeight);
        glClearColor(0.075F, 0.078F, 0.095F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ++RenderedFrames;
        const bool LastRequestedFrame =
            Options.ExitAfterRenderedFrames > 0 &&
            RenderedFrames >= Options.ExitAfterRenderedFrames;
        if (LastRequestedFrame && !Options.CapturePpmPath.empty())
        {
            CaptureSucceeded = CaptureFramebufferPpm(
                Window, Options.CapturePpmPath);
        }
        glfwSwapBuffers(Window);
        if (LastRequestedFrame)
            break;
    }

    const char* OpenGlVersion = reinterpret_cast<const char*>(
        glGetString(GL_VERSION));
    FootLockDeltaSummary FinalFootLockDelta;
    std::optional<float> FinalMeshDelta;
    const ReviewClipInfo* FinalClip = nullptr;
    ReviewLane FinalResultLane = ReviewLane::Final;
    if (Scene.has_value())
    {
        FinalFootLockDelta = MeasureFootLockDelta(*Scene, Display.GoalTrails);
        FinalMeshDelta = MaximumMeshPositionDelta(
            Display.FoundationMesh, Display.FinalMesh);
        FinalClip = CurrentReviewClipInfo(*Scene);
        FinalResultLane = DisplayedResultLane();
    }
    std::cout << "native_viewer_n2=true\n"
              << "native_viewer_n2_1=true\n"
              << "native_viewer_n4_ui=true\n"
              << "native_viewer_n4_2_visualization=true\n"
              << "native_viewer_n4_3_foot_lock_live_ab=true\n"
              << "native_viewer_n4_4_batch=true\n"
              << "playback_available=true\n"
              << "chinese_ui=true\n"
              << "dcc_menu_bar=true\n"
              << "operation_stack_ui=true\n"
              << "chinese_font_loaded="
              << (!ChineseFont.empty() ? "true" : "false") << '\n'
              << "chinese_font="
              << (!ChineseFont.empty()
                    ? PathToUtf8(ChineseFont) : "default") << '\n'
              << "skrv_v1_verified="
              << (Scene.has_value() ? "true" : "false") << '\n'
              << "retargeter_algorithms_executed=false\n"
              << "retarget_bridge_completed_runs="
              << BridgeUi.CompletedRunCount() << '\n'
              << "foot_lock_display_enabled="
              << (BridgeUi.SourceMotionFootLockEnabled()
                    ? "true" : "false") << '\n'
              << "foot_lock_comparison_available="
              << (Scene.has_value() && FootLockComparisonAvailable(*Scene)
                    ? "true" : "false") << '\n'
              << "foot_lock_stored_pose_delta="
              << (FinalClip != nullptr && FinalClip->FootLockHasStoredPoseDelta
                    ? "true" : "false") << '\n'
              << "display_result_lane="
              << (FinalResultLane == ReviewLane::Foundation
                    ? "foundation" : "final") << '\n'
              << "foot_lock_left_goal_delta_cm="
              << FinalFootLockDelta.LeftFootGoalPositionCm << '\n'
              << "foot_lock_right_goal_delta_cm="
              << FinalFootLockDelta.RightFootGoalPositionCm << '\n'
              << "foot_lock_max_bone_position_delta_cm="
              << FinalFootLockDelta.MaximumBonePositionCm << '\n'
              << "foot_lock_max_bone_rotation_delta_degrees="
              << FinalFootLockDelta.MaximumBoneRotationDegrees << '\n'
              << "foot_lock_max_mesh_position_delta_cm="
              << FinalMeshDelta.value_or(0.0F) << '\n'
              << "foot_lock_recent_goal_history_delta_cm="
              << FinalFootLockDelta.MaximumRecentFootGoalPositionCm << '\n'
              << "rendered_frames=" << RenderedFrames << '\n'
              << "four_synchronized_panels="
              << (Scene.has_value() ? "true" : "false") << '\n'
              << "camera_follow_available=true\n"
              << "camera_mode="
              << (CameraFollowEnabled ? "follow" : "free") << '\n'
              << "camera_follow_lane="
              << static_cast<int>(CameraFollowLane) << '\n'
              << "camera_follow_target="
              << (!CameraFollowTargets.empty() &&
                        CameraFollowTargetIndex <
                            CameraFollowTargets.size()
                    ? CameraFollowTargets[
                        CameraFollowTargetIndex].Label
                    : "none") << '\n'
              << "mesh_visible="
              << (Visibility.Mesh ? "true" : "false") << '\n'
              << "skeleton_visible="
              << (Visibility.Skeleton ? "true" : "false") << '\n'
              << "ik_goals_visible="
              << (Visibility.IkGoals ? "true" : "false") << '\n'
              << "goal_history_visible="
              << (Visibility.GoalHistory ? "true" : "false") << '\n'
              << "goal_history_limit=50\n"
              << "goal_history_samples="
              << (Scene.has_value()
                    ? std::to_string(Display.GoalTrails.SampleCount)
                    : "0") << '\n'
              << "mesh_draw_calls=" << Renderer.DrawCalls() << '\n'
              << "mesh_render_error="
              << (MeshRenderError.empty() ? "none" : MeshRenderError)
              << '\n'
              << "displayed_frame="
              << (Scene.has_value()
                    ? std::to_string(Scene->FrameIndex) : "none") << '\n'
              << "displayed_clip="
              << (Scene.has_value()
                    ? std::to_string(Scene->ClipIndex) : "none") << '\n'
              << "ghost_anchor="
              << (Scene.has_value() ? Scene->GhostAnchor.Label : "none")
              << '\n'
              << "opengl_version="
              << (OpenGlVersion != nullptr ? OpenGlVersion : "unknown")
              << '\n';
    if (!Options.CapturePpmPath.empty())
    {
        std::cout << "capture_ppm="
                  << Options.CapturePpmPath.generic_string() << '\n'
                  << "capture_success="
                  << (CaptureSucceeded ? "true" : "false") << '\n';
    }

    Renderer.Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(Window);
    glfwTerminate();
    return CaptureSucceeded ? 0 : 1;
}

} // namespace skrtg::viewer
