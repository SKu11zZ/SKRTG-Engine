#pragma once

#include "skrtg/viewer/camera.h"
#include "skrtg/viewer/review_scene.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace skrtg::viewer
{
struct MeshRenderLayer
{
    const MeshPackage* Package = nullptr;
    const std::vector<std::vector<Vec3>>* Positions = nullptr;
    std::array<float, 4> Color = {0.7F, 0.7F, 0.7F, 1.0F};
    bool Xray = false;
};

struct MeshRenderViewport
{
    float MinimumX = 0.0F;
    float MinimumY = 0.0F;
    float MaximumX = 1.0F;
    float MaximumY = 1.0F;
    float FramebufferScaleX = 1.0F;
    float FramebufferScaleY = 1.0F;
    int FramebufferHeight = 1;
};

class MeshRenderer
{
public:
    bool Initialize(std::string& OutError);
    void Shutdown();

    void BeginFrame();
    bool Draw(
        const MeshRenderViewport& Viewport,
        const OrbitCamera& Camera,
        const MeshRenderLayer* Layers,
        std::size_t LayerCount,
        std::string& OutError);

    [[nodiscard]] std::size_t DrawCalls() const;

private:
    unsigned int Program = 0;
    unsigned int VertexArray = 0;
    unsigned int VertexBuffer = 0;
    unsigned int IndexBuffer = 0;
    std::size_t FrameDrawCalls = 0;
};

} // namespace skrtg::viewer
