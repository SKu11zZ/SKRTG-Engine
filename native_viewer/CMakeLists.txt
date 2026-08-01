cmake_minimum_required(VERSION 3.23)

project(SKRTGNativeViewer
    VERSION 0.3.0
    DESCRIPTION "Native review, bridge, batch, and SKRV tools for SKRTG"
    LANGUAGES C CXX)

function(skrtg_repair_msvc_ninja_showincludes_prefix)
    if(NOT (MSVC AND CMAKE_GENERATOR MATCHES "^Ninja"))
        return()
    endif()

    set(probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/SKRTGShowIncludes")
    file(MAKE_DIRECTORY "${probe_dir}")
    file(WRITE "${probe_dir}/probe_header.h" "#pragma once\n")
    file(WRITE "${probe_dir}/probe.cpp"
        "#include \"probe_header.h\"\nint main() { return 0; }\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}"
            /nologo /TP /utf-8 /showIncludes /c probe.cpp
            "/Fo${probe_dir}/probe.obj"
        WORKING_DIRECTORY "${probe_dir}"
        OUTPUT_VARIABLE showincludes_output
        ERROR_VARIABLE showincludes_error
        RESULT_VARIABLE showincludes_result
        ENCODING NONE)
    if(NOT showincludes_result EQUAL 0 OR NOT
       "${showincludes_output}" MATCHES
       "(^|\n)([^:\n][^:\n]+:[^:\n]*[^: \n][^: \n]:?[ \t]+)([A-Za-z]:\\\\|\\./|\\.\\\\|/)")
        message(FATAL_ERROR
            "Unable to establish reliable MSVC/Ninja /showIncludes parsing.\n"
            "stdout:\n${showincludes_output}\n"
            "stderr:\n${showincludes_error}")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

skrtg_repair_msvc_ninja_showincludes_prefix()

option(SKRTG_NATIVE_VIEWER_BUILD_TESTS "Build native Viewer data-contract tests" ON)
set(SKRTG_RETARGET_ASSET_CATALOG_DIR "" CACHE PATH
    "Optional external directory containing retarget_asset_catalog.json and its hash-bound assets")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)

find_package(OpenGL REQUIRED)

add_library(skrtg_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(skrtg_imgui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends")
target_link_libraries(skrtg_imgui PUBLIC glfw OpenGL::GL)
target_compile_features(skrtg_imgui PUBLIC cxx_std_20)

add_library(skrtg_skrv STATIC
    src/manifest_validation.cpp
    src/package.cpp
    src/sha256.cpp)
target_include_directories(skrtg_skrv
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_features(skrtg_skrv PUBLIC cxx_std_20)

function(skrtg_configure_viewer_target target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /EHsc /W4 /permissive- /utf-8)
        set_property(TARGET ${target_name} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Wpedantic)
    endif()
endfunction()

skrtg_configure_viewer_target(skrtg_skrv)
skrtg_configure_viewer_target(skrtg_imgui)

add_library(skrtg_viewer_core STATIC
    src/batch_retarget.cpp
    src/camera.cpp
    src/mesh_skinning.cpp
    src/playback.cpp
    src/process.cpp
    src/retarget_asset_catalog.cpp
    src/retarget_bridge.cpp
    src/review_scene.cpp
    src/startup_paths.cpp
    src/verified_export.cpp)
target_include_directories(skrtg_viewer_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_link_libraries(skrtg_viewer_core PUBLIC skrtg_skrv)
target_compile_features(skrtg_viewer_core PUBLIC cxx_std_20)
skrtg_configure_viewer_target(skrtg_viewer_core)

add_executable(skrv_pack tools/skrv_pack.cpp)
target_link_libraries(skrv_pack PRIVATE skrtg_skrv)
skrtg_configure_viewer_target(skrv_pack)

add_executable(skrv_inspect tools/skrv_inspect.cpp)
target_link_libraries(skrv_inspect PRIVATE skrtg_skrv)
skrtg_configure_viewer_target(skrv_inspect)

add_executable(skrtg_retarget_bridge tools/skrtg_retarget_bridge_main.cpp)
target_include_directories(skrtg_retarget_bridge PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_link_libraries(skrtg_retarget_bridge PRIVATE skrtg_viewer_core)
skrtg_configure_viewer_target(skrtg_retarget_bridge)

add_executable(skrtg_batch_retarget tools/skrtg_batch_retarget_main.cpp)
target_include_directories(skrtg_batch_retarget PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_link_libraries(skrtg_batch_retarget PRIVATE skrtg_viewer_core)
skrtg_configure_viewer_target(skrtg_batch_retarget)

add_executable(skrtg_viewer
    tools/skrtg_viewer_main.cpp
    src/mesh_renderer.cpp
    src/native_viewer_app.cpp
    src/retarget_bridge_ui.cpp)
target_include_directories(skrtg_viewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(skrtg_viewer PRIVATE
    skrtg_viewer_core
    skrtg_imgui)
skrtg_configure_viewer_target(skrtg_viewer)

add_custom_command(TARGET skrtg_viewer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:skrtg_viewer>/scripts"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/review_html_to_skrv_payload.js"
        "$<TARGET_FILE_DIR:skrtg_viewer>/scripts/review_html_to_skrv_payload.js"
    COMMENT "Copying the HTML-to-SKRV adapter")

if(SKRTG_RETARGET_ASSET_CATALOG_DIR)
    if(NOT EXISTS
       "${SKRTG_RETARGET_ASSET_CATALOG_DIR}/retarget_asset_catalog.json")
        message(FATAL_ERROR
            "SKRTG_RETARGET_ASSET_CATALOG_DIR must contain retarget_asset_catalog.json")
    endif()
    add_custom_command(TARGET skrtg_viewer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${SKRTG_RETARGET_ASSET_CATALOG_DIR}"
            "$<TARGET_FILE_DIR:skrtg_viewer>/data/retarget_catalog"
        COMMENT "Copying the external SHA-256-bound asset catalog")
else()
    message(STATUS
        "No asset catalog configured; the Viewer will start without character or animation entries")
endif()

if(SKRTG_NATIVE_VIEWER_BUILD_TESTS)
    enable_testing()
    add_executable(skrv_package_tests tests/skrv_package_tests.cpp)
    target_include_directories(skrv_package_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(skrv_package_tests PRIVATE skrtg_skrv)
    skrtg_configure_viewer_target(skrv_package_tests)
    add_test(NAME skrv_package_tests COMMAND skrv_package_tests)
endif()

install(TARGETS
    skrv_pack
    skrv_inspect
    skrtg_retarget_bridge
    skrtg_batch_retarget
    skrtg_viewer
    RUNTIME DESTINATION .)
install(FILES scripts/review_html_to_skrv_payload.js
    DESTINATION scripts)
install(FILES THIRD_PARTY_NOTICES.md
    DESTINATION .)
