cmake_minimum_required(VERSION 3.23)

project(SKRTG
    VERSION 0.1.0
    DESCRIPTION "Offline UE IK JSON driven skeletal retargeting tools"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SKRTG_BUILD_TESTS "Build engine contract tests" ON)
option(SKRTG_BUILD_NATIVE_VIEWER "Build the native Viewer, Bridge, Batch, and SKRV tools" ON)
set(SKRTG_FBX_SDK_ROOT "" CACHE PATH "Autodesk FBX SDK 2020.3.9 root")

find_package(FBXSDK REQUIRED)

function(skrtg_configure_target target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
        set_property(TARGET ${target_name} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(skrtg_core STATIC
    src/core/animation/jobs.cpp
    src/core/animation/pose.cpp
    src/core/ik/two_bone_ik.cpp
    src/core/ik/two_bone_pose_buffer_consumer.cpp
    src/core/math/transform.cpp
    src/core/skeleton/runtime_skeleton.cpp
    src/core/validation/report.cpp
    src/reconciliation/rest_reconciliation.cpp
    src/retarget/op_stack.cpp
    src/retarget/ops/source_motion_foot_lock_op.cpp)

target_include_directories(skrtg_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(skrtg_core PUBLIC cxx_std_17)
skrtg_configure_target(skrtg_core)

add_library(skrtg_ueik_runtime STATIC
    src/retarget/ue_ik_json_route.cpp)
target_include_directories(skrtg_ueik_runtime
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native_viewer/third_party")
target_link_libraries(skrtg_ueik_runtime PUBLIC skrtg_core)
target_compile_features(skrtg_ueik_runtime PUBLIC cxx_std_17)
skrtg_configure_target(skrtg_ueik_runtime)

add_library(skrtg_ue_fbx_import_exact STATIC
    src/fbx/ue_fbx_import_exact.cpp)
target_include_directories(skrtg_ue_fbx_import_exact
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/native_viewer/third_party")
target_link_libraries(skrtg_ue_fbx_import_exact
    PUBLIC skrtg_core
    PRIVATE FBXSDK::FBXSDK)
target_compile_features(skrtg_ue_fbx_import_exact PUBLIC cxx_std_17)
skrtg_configure_target(skrtg_ue_fbx_import_exact)

add_library(skrtg_retarget_review STATIC
    src/fbx/retarget_review_package.cpp)
target_include_directories(skrtg_retarget_review
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(skrtg_retarget_review
    PUBLIC skrtg_core
    PRIVATE FBXSDK::FBXSDK)
target_compile_features(skrtg_retarget_review PUBLIC cxx_std_17)
skrtg_configure_target(skrtg_retarget_review)

if(WIN32)
    target_link_libraries(skrtg_ue_fbx_import_exact PRIVATE bcrypt)
    target_link_libraries(skrtg_retarget_review PRIVATE bcrypt)
endif()

add_executable(skrtg_ueik_route_probe
    src/cli/ue_ik_json_route_probe_main.cpp)
target_link_libraries(skrtg_ueik_route_probe PRIVATE skrtg_ueik_runtime)
skrtg_configure_target(skrtg_ueik_route_probe)

add_executable(skrtg_ueik_retarget_worker
    src/cli/ue_ik_json_retarget_worker_main.cpp
    src/fbx/ue_ik_json_retarget.cpp)
target_include_directories(skrtg_ueik_retarget_worker
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/native_viewer/third_party")
target_link_libraries(skrtg_ueik_retarget_worker
    PRIVATE
        skrtg_ueik_runtime
        skrtg_ue_fbx_import_exact
        skrtg_retarget_review
        skrtg_core
        FBXSDK::FBXSDK)
if(WIN32)
    target_link_libraries(skrtg_ueik_retarget_worker PRIVATE bcrypt)
endif()
skrtg_configure_target(skrtg_ueik_retarget_worker)

if(WIN32)
    add_custom_command(TARGET skrtg_ueik_retarget_worker POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:FBXSDK::FBXSDK>"
            "$<TARGET_FILE_DIR:skrtg_ueik_retarget_worker>"
        COMMENT "Copying the Autodesk FBX SDK runtime")
endif()

if(SKRTG_BUILD_TESTS)
    enable_testing()

    add_executable(skrtg_core_contract_tests tests/core_contract_tests.cpp)
    target_link_libraries(skrtg_core_contract_tests PRIVATE skrtg_core)
    skrtg_configure_target(skrtg_core_contract_tests)
    add_test(NAME skrtg_core_contracts COMMAND skrtg_core_contract_tests)

    add_executable(skrtg_two_bone_ik_tests tests/two_bone_ik_tests.cpp)
    target_link_libraries(skrtg_two_bone_ik_tests PRIVATE skrtg_core)
    skrtg_configure_target(skrtg_two_bone_ik_tests)
    add_test(NAME skrtg_two_bone_ik_contracts COMMAND skrtg_two_bone_ik_tests)

    add_executable(skrtg_retarget_op_stack_tests tests/retarget_op_stack_tests.cpp)
    target_link_libraries(skrtg_retarget_op_stack_tests PRIVATE skrtg_core)
    skrtg_configure_target(skrtg_retarget_op_stack_tests)
    add_test(NAME skrtg_retarget_op_stack_contracts COMMAND skrtg_retarget_op_stack_tests)

    add_executable(skrtg_source_motion_foot_lock_tests
        tests/source_motion_foot_lock_op_tests.cpp)
    target_link_libraries(skrtg_source_motion_foot_lock_tests PRIVATE skrtg_core)
    skrtg_configure_target(skrtg_source_motion_foot_lock_tests)
    add_test(NAME skrtg_source_motion_foot_lock_contracts
        COMMAND skrtg_source_motion_foot_lock_tests)
endif()

if(SKRTG_BUILD_NATIVE_VIEWER)
    add_subdirectory(native_viewer)
endif()

install(TARGETS
    skrtg_ueik_route_probe
    skrtg_ueik_retarget_worker
    RUNTIME DESTINATION .)
