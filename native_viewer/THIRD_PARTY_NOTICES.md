# Third-party notices

## JSON for Modern C++

- Project: nlohmann/json
- Version: 3.12.0
- Upstream: https://github.com/nlohmann/json/releases/tag/v3.12.0
- Vendored file: `third_party/nlohmann/json.hpp`
- Vendored header SHA-256: `AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63`
- License: MIT; preserved in `third_party/nlohmann/LICENSE.MIT`

This dependency is used only to parse and semantically validate the SKRV
manifest. It does not link the Retargeter, Autodesk FBX SDK, or any frozen
animation algorithm into the Native Viewer data layer.

## GLFW

- Project: GLFW
- Version: 3.4
- Upstream: https://github.com/glfw/glfw/releases/tag/3.4
- Official tag archive SHA-256: `A133DDC3D3C66143EBA9035621DB8E0BCF34DBA1EE9514A9E23E96AFD39FD57A`
- Vendored subset: root CMake project, `CMake/`, `include/`, `src/`, required
  Wayland/MinGW support files, and `LICENSE.md`
- License: zlib/libpng; preserved in `third_party/glfw/LICENSE.md`

GLFW owns only the native window, OpenGL context, and keyboard/mouse input
surface. It does not read FBX or execute Retargeter algorithms.

## Dear ImGui

- Project: Dear ImGui
- Version: 1.92.7
- Upstream: https://github.com/ocornut/imgui/releases/tag/v1.92.7
- Official tag archive SHA-256: `4C1C3164A6036DD675240E7E17F21CEA9E82FBC1052B142379C5CBB6D50FA85E`
- Vendored subset: core sources plus official GLFW and OpenGL 3 backends
- License: MIT; preserved in `third_party/imgui/LICENSE.txt`

Dear ImGui supplies N2 controls and CPU-authored line draw lists. Its official
OpenGL 3 backend renders those draw lists into the GLFW OpenGL 3.3 Core
context. N2 does not add a scene engine, WebView, browser, or FBX runtime.
