set(_skrtg_fbx_roots)

if(SKRTG_FBX_SDK_ROOT)
    list(APPEND _skrtg_fbx_roots "${SKRTG_FBX_SDK_ROOT}")
endif()

if(DEFINED ENV{FBX_SDK_ROOT})
    list(APPEND _skrtg_fbx_roots "$ENV{FBX_SDK_ROOT}")
endif()

list(APPEND _skrtg_fbx_roots
    "C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9"
    "C:/Program Files (x86)/Autodesk/FBX/FBX SDK/2020.3.9")

foreach(_candidate IN LISTS _skrtg_fbx_roots)
    if(EXISTS "${_candidate}/include/fbxsdk.h")
        file(TO_CMAKE_PATH "${_candidate}" FBXSDK_ROOT)
        break()
    endif()
endforeach()

if(NOT FBXSDK_ROOT)
    set(FBXSDK_FOUND FALSE)
    if(FBXSDK_FIND_REQUIRED)
        message(FATAL_ERROR "Autodesk FBX SDK 2020.3.9 was not found. Set SKRTG_FBX_SDK_ROOT or FBX_SDK_ROOT.")
    endif()
    return()
endif()

set(FBXSDK_INCLUDE_DIR "${FBXSDK_ROOT}/include")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_skrtg_fbx_arch "x64")
else()
    set(_skrtg_fbx_arch "x86")
endif()

set(_skrtg_fbx_release_dir "${FBXSDK_ROOT}/lib/${_skrtg_fbx_arch}/release")
set(_skrtg_fbx_debug_dir "${FBXSDK_ROOT}/lib/${_skrtg_fbx_arch}/debug")

if(WIN32)
    set(FBXSDK_LIBRARY_RELEASE "${_skrtg_fbx_release_dir}/libfbxsdk.lib")
    set(FBXSDK_RUNTIME_RELEASE "${_skrtg_fbx_release_dir}/libfbxsdk.dll")
    set(FBXSDK_LIBRARY_DEBUG "${_skrtg_fbx_debug_dir}/libfbxsdk.lib")
    set(FBXSDK_RUNTIME_DEBUG "${_skrtg_fbx_debug_dir}/libfbxsdk.dll")
else()
    set(FBXSDK_LIBRARY_RELEASE "${_skrtg_fbx_release_dir}/libfbxsdk${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(FBXSDK_RUNTIME_RELEASE "${FBXSDK_LIBRARY_RELEASE}")
    set(FBXSDK_LIBRARY_DEBUG "${_skrtg_fbx_debug_dir}/libfbxsdk${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(FBXSDK_RUNTIME_DEBUG "${FBXSDK_LIBRARY_DEBUG}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FBXSDK
    REQUIRED_VARS
        FBXSDK_ROOT
        FBXSDK_INCLUDE_DIR
        FBXSDK_LIBRARY_RELEASE
        FBXSDK_RUNTIME_RELEASE
    VERSION_VAR FBXSDK_VERSION)

if(FBXSDK_FOUND AND NOT TARGET FBXSDK::FBXSDK)
    add_library(FBXSDK::FBXSDK SHARED IMPORTED GLOBAL)
    set_target_properties(FBXSDK::FBXSDK PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${FBXSDK_INCLUDE_DIR}"
        IMPORTED_LOCATION_RELEASE "${FBXSDK_RUNTIME_RELEASE}"
        IMPORTED_IMPLIB_RELEASE "${FBXSDK_LIBRARY_RELEASE}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${FBXSDK_RUNTIME_RELEASE}"
        IMPORTED_IMPLIB_RELWITHDEBINFO "${FBXSDK_LIBRARY_RELEASE}"
        IMPORTED_LOCATION_MINSIZEREL "${FBXSDK_RUNTIME_RELEASE}"
        IMPORTED_IMPLIB_MINSIZEREL "${FBXSDK_LIBRARY_RELEASE}")

    if(EXISTS "${FBXSDK_RUNTIME_DEBUG}" AND EXISTS "${FBXSDK_LIBRARY_DEBUG}")
        set_target_properties(FBXSDK::FBXSDK PROPERTIES
            IMPORTED_LOCATION_DEBUG "${FBXSDK_RUNTIME_DEBUG}"
            IMPORTED_IMPLIB_DEBUG "${FBXSDK_LIBRARY_DEBUG}")
    else()
        set_target_properties(FBXSDK::FBXSDK PROPERTIES
            IMPORTED_LOCATION_DEBUG "${FBXSDK_RUNTIME_RELEASE}"
            IMPORTED_IMPLIB_DEBUG "${FBXSDK_LIBRARY_RELEASE}")
    endif()

    if(WIN32)
        target_compile_definitions(FBXSDK::FBXSDK INTERFACE FBXSDK_SHARED)
    endif()
endif()

mark_as_advanced(FBXSDK_INCLUDE_DIR FBXSDK_LIBRARY_RELEASE FBXSDK_RUNTIME_RELEASE)
