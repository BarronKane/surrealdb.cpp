cmake_minimum_required(VERSION 3.31.11) # I'm using 4+, but C23/CXX23 fixes here for ninja codegen.
project(surrealdb_cpp VERSION 0.1.0 LANGUAGES C CXX)

# ============================================================================
# Git Submodules
# ============================================================================

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    option(GIT_SUBMODULE "Check submodules during build" ON)
    if(GIT_SUBMODULE)
        message(STATUS "Updating git submodules...")
        execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE GIT_SUBMOD_RESULT)
        if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update --init --recursive failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
        endif()
    endif()
endif()

# Verify submodules are present
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/surrealdb.c/CMakeLists.txt")
    message(FATAL_ERROR "The surrealdb.c submodule was not downloaded! GIT_SUBMODULE was turned off or failed. Please update submodules and try again.")
endif()

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/ThirdParty/Unity/CMakeLists.txt")
    message(FATAL_ERROR "The Unity submodule was not downloaded! GIT_SUBMODULE was turned off or failed. Please update submodules and try again.")
endif()

# ============================================================================
# User Options
# ============================================================================

# Build options
option(BUILD_TESTING "Build tests." ON)
option(SURREALDB_RELEASE "Build Rust library in release mode" OFF)
option(SURREALDB_USE_MODULES "Enable C++ modules for surrealdb_cpp" OFF)

# Static/Shared - On for default for now for testing.
option(BUILD_SHARED "Build shared library .dll/.so/.dylib)" ON)

# Toolchain notes
if(SURREALDB_USE_MODULES)
    if(MSVC AND NOT CMAKE_GENERATOR MATCHES "Visual Studio 18 2026")
        message(WARNING "SURREALDB_USE_MODULES is ON. Full feature support requires the Visual Studio 18 2026 generator (current: ${CMAKE_GENERATOR}).")
    elseif(NOT MSVC)
        message(STATUS "SURREALDB_USE_MODULES is ON with a non-MSVC toolchain; module support may be limited.")
    endif()
endif()

if(MINGW)
    message(STATUS "MinGW builds produce .a static libraries instead of .lib.")
endif()

# ============================================================================
# Project Configuration
# ============================================================================

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # -std=c++xx instead of -std=gnu++xx

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
if(WIN32)
    # For DLL library definitions.
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

    add_compile_definitions(
            # This sets the minimum feature set to Windows 7.
            _WIN32_WINNT=0x0601
    )
endif()

if(MSVC)
    add_definitions(
            # These are needed in windows to activate the __cplusplus macro
            # which is off by default, and enforce macro reporting for version.
            # Classically, these are off for backwards compatability.
            " /Zc:__cplusplus "
            #" /std:c++23 " # This actually doesn't exist in MSVC atm, needs exploring.
    )
endif()

# Platform agnostic installation trees.
include(GNUInstallDirs)

# ============================================================================
# Dependencies
# ============================================================================

find_package(Surrealdb.c REQUIRED)

# ============================================================================
# Project structure.
# ============================================================================

add_subdirectory_with_folder("surrealdb.cpp" src/surrealdb_cpp)

# ============================================================================
# Tests (Unity)
# ============================================================================

if(BUILD_TESTING)
    enable_testing()

    set(UNITY_USE_SYSTEM OFF CACHE BOOL "Use a system-installed Unity if available" FORCE)
    find_package(Unity REQUIRED MODULE)

    add_subdirectory(tests)
endif()

# ============================================================================
# Examples
# ============================================================================

option(BUILD_EXAMPLES "Build examples" ON)

if(BUILD_EXAMPLES)
    add_subdirectory(examples/no_modules)
    if(SURREALDB_USE_MODULES)
        add_subdirectory(examples/modules)
    endif()
endif()
