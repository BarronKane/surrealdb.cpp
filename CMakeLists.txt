cmake_minimum_required(VERSION 3.31.11) # I'm using 4+, but C23/CXX23 fixes here for ninja codegen.
project(surrealdb_cpp VERSION 0.1.0 LANGUAGES C CXX)

# ============================================================================
# Project Configuration
# ============================================================================

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # -std=c++xx instead of -std=gnu++xx

# Build options
option(BUILD_TESTING "Build tests." ON)
option(SURREALDB_RELEASE "Build Rust library in release mode" OFF)

# Make custom find modules available
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Put binary files on the top level of the build tree for convenience.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
# DLLs are runtime for windows.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# To homogenize folders in IDEs.
include(GenerateExportHeader)
include(${CMAKE_SOURCE_DIR}/cmake/helpers.cmake)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# For compiler options, we can also enable things like static analysis
# later, as well as many other options. C++ is a large standard
# so we will tweak only as necessary.
if(WIN32 OR MSVC)
    # For DLL library definitions.
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

    add_compile_definitions(
            # This sets the minimum feature set to Windows 7.
            _WIN32_WINNT=0x0601
    )
    add_definitions(
            # These are needed in windows to activate the __cplusplus macro
            # which is off by default, and enforce macro reporting for version.
            # Classically, these are off for backwards compatability.
            " /Zc:__cplusplus "
            " /std:c++23 "
    )
endif(WIN32 OR MSVC)

# Platform agnostic installation trees.
include(GNUInstallDirs)

# ============================================================================
# Dependencies
# ============================================================================

find_package(Surrealdb.c REQUIRED)

# ============================================================================
# Project structure.
# ============================================================================



# ============================================================================
# Tests (Unity from surrealdb.c)
# ============================================================================

if(BUILD_TESTING)
    enable_testing()

    set(_unity_dir "${SURREALDB_C_SOURCE_DIR}/ThirdParty/Unity")
    if(EXISTS "${_unity_dir}/CMakeLists.txt")
        set(UNITY_EXTENSION_FIXTURE ON CACHE BOOL "Enable Unity Fixture extension" FORCE)
        if(NOT TARGET unity)
            add_subdirectory("${_unity_dir}" "${CMAKE_BINARY_DIR}/ThirdParty/Unity-build")
        endif()
        if(TARGET unity AND NOT TARGET Unity::unity)
            add_library(Unity::unity ALIAS unity)
        endif()
    else()
        message(FATAL_ERROR "Unity not found at: ${_unity_dir}")
    endif()

    add_subdirectory(tests)
endif()
