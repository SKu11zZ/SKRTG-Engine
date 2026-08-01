#include "mesh_renderer.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace skrtg::viewer
{
namespace
{
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
#define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif

using CreateShaderProc = GLuint(APIENTRY*)(GLenum);
using ShaderSourceProc = void(APIENTRY*)(
    GLuint, GLsizei, const char* const*, const GLint*);
using CompileShaderProc = void(APIENTRY*)(GLuint);
using GetShaderivProc = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GetShaderInfoLogProc = void(APIENTRY*)(
    GLuint, GLsizei, GLsizei*, char*);
using DeleteShaderProc = void(APIENTRY*)(GLuint);
using CreateProgramProc = GLuint(APIENTRY*)();
using AttachShaderProc = void(APIENTRY*)(GLuint, GLuint);
using LinkProgramProc = void(APIENTRY*)(GLuint);
using GetProgramivProc = void(APIENTRY*)(GLuint, GLenum, GLint*);
using GetProgramInfoLogProc = void(APIENTRY*)(
    GLuint, GLsizei, GLsizei*, char*);
using DeleteProgramProc = void(APIENTRY*)(GLuint);
using UseProgramProc = void(APIENTRY*)(GLuint);
using GetUniformLocationProc = GLint(APIENTRY*)(GLuint, const char*);
using Uniform1fProc = void(APIENTRY*)(GLint, GLfloat);
using Uniform1iProc = void(APIENTRY*)(GLint, GLint);
using Uniform3fProc = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
using Uniform4fProc = void(APIENTRY*)(
    GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using GenBuffersProc = void(APIENTRY*)(GLsizei, GLuint*);
using DeleteBuffersProc = void(APIENTRY*)(GLsizei, const GLuint*);
using BindBufferProc = void(APIENTRY*)(GLenum, GLuint);
using BufferDataProc = void(APIENTRY*)(
    GLenum, std::ptrdiff_t, const void*, GLenum);
using GenVertexArraysProc = void(APIENTRY*)(GLsizei, GLuint*);
using DeleteVertexArraysProc = void(APIENTRY*)(GLsizei, const GLuint*);
using BindVertexArrayProc = void(APIENTRY*)(GLuint);
using EnableVertexAttribArrayProc = void(APIENTRY*)(GLuint);
using VertexAttribPointerProc = void(APIENTRY*)(
    GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using DrawElementsProc = void(APIENTRY*)(
    GLenum, GLsizei, GLenum, const void*);

struct GlFunctions
{
    CreateShaderProc CreateShader = nullptr;
    ShaderSourceProc ShaderSource = nullptr;
    CompileShaderProc CompileShader = nullptr;
    GetShaderivProc GetShaderiv = nullptr;
    GetShaderInfoLogProc GetShaderInfoLog = nullptr;
    DeleteShaderProc DeleteShader = nullptr;
    CreateProgramProc CreateProgram = nullptr;
    AttachShaderProc AttachShader = nullptr;
    LinkProgramProc LinkProgram = nullptr;
    GetProgramivProc GetProgramiv = nullptr;
    GetProgramInfoLogProc GetProgramInfoLog = nullptr;
    DeleteProgramProc DeleteProgram = nullptr;
    UseProgramProc UseProgram = nullptr;
    GetUniformLocationProc GetUniformLocation = nullptr;
    Uniform1fProc Uniform1f = nullptr;
    Uniform1iProc Uniform1i = nullptr;
    Uniform3fProc Uniform3f = nullptr;
    Uniform4fProc Uniform4f = nullptr;
    GenBuffersProc GenBuffers = nullptr;
    DeleteBuffersProc DeleteBuffers = nullptr;
    BindBufferProc BindBuffer = nullptr;
    BufferDataProc BufferData = nullptr;
    GenVertexArraysProc GenVertexArrays = nullptr;
    DeleteVertexArraysProc DeleteVertexArrays = nullptr;
    BindVertexArrayProc BindVertexArray = nullptr;
    EnableVertexAttribArrayProc EnableVertexAttribArray = nullptr;
    VertexAttribPointerProc VertexAttribPointer = nullptr;
    DrawElementsProc DrawElements = nullptr;
};

GlFunctions Gl;

template <typename Function>
bool LoadFunction(Function& OutFunction, const char* Name)
{
    OutFunction = reinterpret_cast<Function>(glfwGetProcAddress(Name));
    return OutFunction != nullptr;
}

bool LoadFunctions(std::string& OutError)
{
    bool Success = true;
#define SKRTG_LOAD_GL(Name) \
    Success = LoadFunction(Gl.Name, "gl" #Name) && Success
    SKRTG_LOAD_GL(CreateShader);
    SKRTG_LOAD_GL(ShaderSource);
    SKRTG_LOAD_GL(CompileShader);
    SKRTG_LOAD_GL(GetShaderiv);
    SKRTG_LOAD_GL(GetShaderInfoLog);
    SKRTG_LOAD_GL(DeleteShader);
    SKRTG_LOAD_GL(CreateProgram);
    SKRTG_LOAD_GL(AttachShader);
    SKRTG_LOAD_GL(LinkProgram);
    SKRTG_LOAD_GL(GetProgramiv);
    SKRTG_LOAD_GL(GetProgramInfoLog);
    SKRTG_LOAD_GL(DeleteProgram);
    SKRTG_LOAD_GL(UseProgram);
    SKRTG_LOAD_GL(GetUniformLocation);
    SKRTG_LOAD_GL(Uniform1f);
    SKRTG_LOAD_GL(Uniform1i);
    SKRTG_LOAD_GL(Uniform3f);
    SKRTG_LOAD_GL(Uniform4f);
    SKRTG_LOAD_GL(GenBuffers);
    SKRTG_LOAD_GL(DeleteBuffers);
    SKRTG_LOAD_GL(BindBuffer);
    SKRTG_LOAD_GL(BufferData);
    SKRTG_LOAD_GL(GenVertexArrays);
    SKRTG_LOAD_GL(DeleteVertexArrays);
    SKRTG_LOAD_GL(BindVertexArray);
    SKRTG_LOAD_GL(EnableVertexAttribArray);
    SKRTG_LOAD_GL(VertexAttribPointer);
    SKRTG_LOAD_GL(DrawElements);
#undef SKRTG_LOAD_GL
    if (!Success)
        OutError = "OpenGL 3.3 mesh renderer entry points are unavailable";
    return Success;
}

std::string ShaderLog(const GLuint Shader)
{
    GLint Length = 0;
    Gl.GetShaderiv(Shader, GL_INFO_LOG_LENGTH, &Length);
    if (Length <= 1) return {};
    std::string Result(static_cast<std::size_t>(Length), '\0');
    Gl.GetShaderInfoLog(Shader, Length, nullptr, Result.data());
    return Result;
}

std::string ProgramLog(const GLuint Program)
{
    GLint Length = 0;
    Gl.GetProgramiv(Program, GL_INFO_LOG_LENGTH, &Length);
    if (Length <= 1) return {};
    std::string Result(static_cast<std::size_t>(Length), '\0');
    Gl.GetProgramInfoLog(Program, Length, nullptr, Result.data());
    return Result;
}

GLuint CompileShader(
    const GLenum Type,
    const char* Source,
    std::string& OutError)
{
    const GLuint Shader = Gl.CreateShader(Type);
    Gl.ShaderSource(Shader, 1, &Source, nullptr);
    Gl.CompileShader(Shader);
    GLint Compiled = GL_FALSE;
    Gl.GetShaderiv(Shader, GL_COMPILE_STATUS, &Compiled);
    if (Compiled == GL_TRUE) return Shader;
    OutError = "mesh shader compile failed: " + ShaderLog(Shader);
    Gl.DeleteShader(Shader);
    return 0;
}

Vec3 Add(const Vec3& Left, const Vec3& Right)
{
    return {Left.X + Right.X, Left.Y + Right.Y, Left.Z + Right.Z};
}

Vec3 Subtract(const Vec3& Left, const Vec3& Right)
{
    return {Left.X - Right.X, Left.Y - Right.Y, Left.Z - Right.Z};
}

Vec3 Multiply(const Vec3& Value, const float Scalar)
{
    return {Value.X * Scalar, Value.Y * Scalar, Value.Z * Scalar};
}

float Dot(const Vec3& Left, const Vec3& Right)
{
    return Left.X * Right.X + Left.Y * Right.Y + Left.Z * Right.Z;
}

Vec3 Cross(const Vec3& Left, const Vec3& Right)
{
    return {
        Left.Y * Right.Z - Left.Z * Right.Y,
        Left.Z * Right.X - Left.X * Right.Z,
        Left.X * Right.Y - Left.Y * Right.X};
}

Vec3 Normalize(const Vec3& Value)
{
    const float Length = std::sqrt(Dot(Value, Value));
    if (Length <= std::numeric_limits<float>::epsilon())
        return {};
    return Multiply(Value, 1.0F / Length);
}

struct CameraBasis
{
    Vec3 Eye;
    Vec3 Forward;
    Vec3 Right;
    Vec3 Up;
};

CameraBasis MakeBasis(const OrbitCamera& Camera)
{
    const float CosPitch = std::cos(Camera.PitchRadians);
    const Vec3 EyeDirection = {
        std::sin(Camera.YawRadians) * CosPitch,
        std::sin(Camera.PitchRadians),
        std::cos(Camera.YawRadians) * CosPitch};
    const Vec3 Eye = Add(
        Camera.Focus, Multiply(EyeDirection, Camera.Distance));
    const Vec3 Forward = Normalize(Subtract(Camera.Focus, Eye));
    const Vec3 Right = Normalize(Cross(Forward, {0.0F, 1.0F, 0.0F}));
    const Vec3 Up = Normalize(Cross(Right, Forward));
    return {Eye, Forward, Right, Up};
}

void SetUniform3(const GLuint Program, const char* Name, const Vec3& Value)
{
    Gl.Uniform3f(
        Gl.GetUniformLocation(Program, Name),
        Value.X, Value.Y, Value.Z);
}
} // namespace

bool MeshRenderer::Initialize(std::string& OutError)
{
    OutError.clear();
    if (!LoadFunctions(OutError)) return false;
    static constexpr const char* VertexSource = R"GLSL(
#version 330 core
layout(location = 0) in vec3 aPosition;
uniform vec3 uEye;
uniform vec3 uForward;
uniform vec3 uRight;
uniform vec3 uUp;
uniform float uAspect;
uniform float uHalfHeight;
uniform float uTanHalfFov;
uniform float uNear;
uniform float uFar;
uniform int uOrthographic;
out vec3 vWorld;
void main()
{
    vec3 relative = aPosition - uEye;
    float x = dot(relative, uRight);
    float y = dot(relative, uUp);
    float depth = dot(relative, uForward);
    if (uOrthographic != 0)
    {
        float z = 2.0 * (depth - uNear) / (uFar - uNear) - 1.0;
        gl_Position = vec4(
            x / (uHalfHeight * uAspect), y / uHalfHeight, z, 1.0);
    }
    else
    {
        float a = (uFar + uNear) / (uFar - uNear);
        float b = -2.0 * uFar * uNear / (uFar - uNear);
        gl_Position = vec4(
            x / (uTanHalfFov * uAspect),
            y / uTanHalfFov,
            a * depth + b,
            depth);
    }
    vWorld = aPosition;
}
)GLSL";
    static constexpr const char* FragmentSource = R"GLSL(
#version 330 core
in vec3 vWorld;
uniform vec4 uColor;
out vec4 outColor;
void main()
{
    vec3 derivativeNormal = cross(dFdx(vWorld), dFdy(vWorld));
    float normalLength = length(derivativeNormal);
    vec3 normal = normalLength > 1.0e-8
        ? derivativeNormal / normalLength : vec3(0.0, 1.0, 0.0);
    float light = 0.34 + 0.66 *
        abs(dot(normal, normalize(vec3(0.38, 0.72, 0.58))));
    outColor = vec4(uColor.rgb * light, uColor.a);
}
)GLSL";

    const GLuint VertexShader = CompileShader(
        GL_VERTEX_SHADER, VertexSource, OutError);
    if (VertexShader == 0) return false;
    const GLuint FragmentShader = CompileShader(
        GL_FRAGMENT_SHADER, FragmentSource, OutError);
    if (FragmentShader == 0)
    {
        Gl.DeleteShader(VertexShader);
        return false;
    }
    Program = Gl.CreateProgram();
    Gl.AttachShader(Program, VertexShader);
    Gl.AttachShader(Program, FragmentShader);
    Gl.LinkProgram(Program);
    Gl.DeleteShader(VertexShader);
    Gl.DeleteShader(FragmentShader);
    GLint Linked = GL_FALSE;
    Gl.GetProgramiv(Program, GL_LINK_STATUS, &Linked);
    if (Linked != GL_TRUE)
    {
        OutError = "mesh shader link failed: " + ProgramLog(Program);
        Shutdown();
        return false;
    }

    Gl.GenVertexArrays(1, &VertexArray);
    Gl.GenBuffers(1, &VertexBuffer);
    Gl.GenBuffers(1, &IndexBuffer);
    return true;
}

void MeshRenderer::Shutdown()
{
    if (IndexBuffer != 0)
        Gl.DeleteBuffers(1, &IndexBuffer);
    if (VertexBuffer != 0)
        Gl.DeleteBuffers(1, &VertexBuffer);
    if (VertexArray != 0)
        Gl.DeleteVertexArrays(1, &VertexArray);
    if (Program != 0)
        Gl.DeleteProgram(Program);
    Program = 0;
    VertexArray = 0;
    VertexBuffer = 0;
    IndexBuffer = 0;
}

void MeshRenderer::BeginFrame()
{
    FrameDrawCalls = 0;
}

bool MeshRenderer::Draw(
    const MeshRenderViewport& Viewport,
    const OrbitCamera& Camera,
    const MeshRenderLayer* Layers,
    const std::size_t LayerCount,
    std::string& OutError)
{
    OutError.clear();
    if (Program == 0 || Layers == nullptr)
    {
        OutError = "mesh renderer is not initialized";
        return false;
    }
    const int X = std::max(0, static_cast<int>(std::lround(
        Viewport.MinimumX * Viewport.FramebufferScaleX)));
    const int Width = std::max(1, static_cast<int>(std::lround(
        (Viewport.MaximumX - Viewport.MinimumX) *
        Viewport.FramebufferScaleX)));
    const int Height = std::max(1, static_cast<int>(std::lround(
        (Viewport.MaximumY - Viewport.MinimumY) *
        Viewport.FramebufferScaleY)));
    const int Y = std::max(0, Viewport.FramebufferHeight -
        static_cast<int>(std::lround(
            Viewport.MaximumY * Viewport.FramebufferScaleY)));

    glEnable(GL_SCISSOR_TEST);
    glScissor(X, Y, Width, Height);
    glViewport(X, Y, Width, Height);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    Gl.UseProgram(Program);
    const CameraBasis Basis = MakeBasis(Camera);
    SetUniform3(Program, "uEye", Basis.Eye);
    SetUniform3(Program, "uForward", Basis.Forward);
    SetUniform3(Program, "uRight", Basis.Right);
    SetUniform3(Program, "uUp", Basis.Up);
    Gl.Uniform1f(
        Gl.GetUniformLocation(Program, "uAspect"),
        static_cast<float>(Width) / static_cast<float>(Height));
    Gl.Uniform1f(
        Gl.GetUniformLocation(Program, "uHalfHeight"),
        std::max(Camera.OrthographicHalfHeight, 0.001F));
    Gl.Uniform1f(
        Gl.GetUniformLocation(Program, "uTanHalfFov"),
        std::max(std::tan(
            Camera.VerticalFieldOfViewRadians * 0.5F), 0.001F));
    const float Near = 0.01F;
    const float Far = std::max(
        100000.0F,
        Camera.Distance * 100.0F +
            Camera.OrthographicHalfHeight * 100.0F);
    Gl.Uniform1f(Gl.GetUniformLocation(Program, "uNear"), Near);
    Gl.Uniform1f(Gl.GetUniformLocation(Program, "uFar"), Far);
    Gl.Uniform1i(
        Gl.GetUniformLocation(Program, "uOrthographic"),
        Camera.Orthographic ? 1 : 0);

    Gl.BindVertexArray(VertexArray);
    Gl.BindBuffer(GL_ARRAY_BUFFER, VertexBuffer);
    Gl.EnableVertexAttribArray(0);
    Gl.VertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE,
        static_cast<GLsizei>(sizeof(Vec3)), nullptr);
    Gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBuffer);

    static_assert(sizeof(Vec3) == sizeof(float) * 3U);
    for (std::size_t LayerIndex = 0;
         LayerIndex < LayerCount; ++LayerIndex)
    {
        const MeshRenderLayer& Layer = Layers[LayerIndex];
        if (Layer.Package == nullptr || Layer.Positions == nullptr ||
            Layer.Package->Meshes.size() != Layer.Positions->size())
        {
            OutError = "mesh render layer shape is inconsistent";
            return false;
        }
        if (Layer.Xray)
        {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
        }
        else
        {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
        }
        Gl.Uniform4f(
            Gl.GetUniformLocation(Program, "uColor"),
            Layer.Color[0], Layer.Color[1],
            Layer.Color[2], Layer.Color[3]);
        for (std::size_t MeshIndex = 0;
             MeshIndex < Layer.Package->Meshes.size(); ++MeshIndex)
        {
            const Mesh& MeshValue = Layer.Package->Meshes[MeshIndex];
            const std::vector<Vec3>& Positions =
                Layer.Positions->at(MeshIndex);
            if (Positions.size() != MeshValue.BindPositions.size())
            {
                OutError = "skinned mesh vertex count is inconsistent";
                return false;
            }
            if (Positions.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<GLsizei>::max()) ||
                MeshValue.TriangleIndices.size() >
                    static_cast<std::size_t>(
                        std::numeric_limits<GLsizei>::max()))
            {
                OutError = "mesh exceeds OpenGL draw limits";
                return false;
            }
            Gl.BufferData(
                GL_ARRAY_BUFFER,
                static_cast<std::ptrdiff_t>(
                    Positions.size() * sizeof(Vec3)),
                Positions.data(), GL_DYNAMIC_DRAW);
            Gl.BufferData(
                GL_ELEMENT_ARRAY_BUFFER,
                static_cast<std::ptrdiff_t>(
                    MeshValue.TriangleIndices.size() *
                    sizeof(std::uint32_t)),
                MeshValue.TriangleIndices.data(), GL_DYNAMIC_DRAW);
            Gl.DrawElements(
                GL_TRIANGLES,
                static_cast<GLsizei>(MeshValue.TriangleIndices.size()),
                GL_UNSIGNED_INT, nullptr);
            ++FrameDrawCalls;
        }
    }
    glDepthMask(GL_TRUE);
    return true;
}

std::size_t MeshRenderer::DrawCalls() const
{
    return FrameDrawCalls;
}

} // namespace skrtg::viewer
