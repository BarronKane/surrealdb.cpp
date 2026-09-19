cmake_minimum_required(VERSION 3.31.11) # I'm using 4+, but C23/CXX23 fixes here for ninja codegen.

# Must precede project(): CMake configures the Apple toolchain there and ignores
# a deployment target set afterwards.
#
# surrealdb.c carries this module and includes it before its own project(), but
# that is too late when this project is the top-level one -- the toolchain is
# already configured by the time the submodule is added, and every macOS build
# gets the wall of "built for newer macOS version" linker warnings the module
# exists to prevent. Reused from the submodule rather than copied, so the logic
# has one home; skipped silently when the submodule has not been fetched yet,
# which is the same position a fresh clone is in.
set(_sdb_apple_module
    "${CMAKE_CURRENT_LIST_DIR}/surrealdb.c/cmake/AppleDeploymentTarget.cmake")
if(APPLE AND EXISTS "${_sdb_apple_module}")
    include("${_sdb_apple_module}")
endif()

project(surrealdb_cpp VERSION 0.1.3 LANGUAGES C CXX)

# ============================================================================
# Git Submodules
# ============================================================================

option(GIT_SUBMODULE "Check submodules during build" ON)
if(GIT_SUBMODULE)
    message(STATUS "GIT_SUBMODULE=ON")
else()
    message(STATUS "GIT_SUBMODULE=OFF")
endif()

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git" AND GIT_SUBMODULE)
    # Only *populate* submodules that are missing. Never update one that is
    # already checked out.
    #
    # `git submodule update --init --recursive` unconditionally resets every
    # submodule to the commit recorded in this repository's index. Running that
    # on every configure means merely building surrealdb.cpp silently rewinds a
    # surrealdb.c checkout to whatever pointer happens to be committed here,
    # discarding the branch someone is working on. That is not the build
    # system's call to make, and the failure is quiet: the tree still compiles,
    # against the wrong library.
    #
    # An empty directory is unambiguous -- nothing to lose -- so that case is
    # initialised and everything else is left alone.
    foreach(_sub IN ITEMS "surrealdb.c")
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${_sub}/.git")
            message(STATUS "Initialising submodule: ${_sub}")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive -- "${_sub}"
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                RESULT_VARIABLE _sub_result)
            if(NOT _sub_result EQUAL "0")
                message(WARNING "Could not initialise ${_sub} (${_sub_result}); "
                                "run: git submodule update --init --recursive")
            endif()
        endif()
    endforeach()

    # Say so when the checkout differs from the recorded pointer, rather than
    # "fixing" it. During development that difference is usually deliberate.
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/surrealdb.c"
                    OUTPUT_VARIABLE _sub_head OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} ls-files -s surrealdb.c
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE _sub_recorded OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(_sub_head AND _sub_recorded)
        # `git ls-files -s` prints "<mode> <sha> <stage>\t<path>"; the mode is
        # also all digits, so match the second field rather than the first.
        string(REGEX MATCH "^[0-9]+ ([0-9a-f]+)" _ "${_sub_recorded}")
        set(_sub_recorded_sha "${CMAKE_MATCH_1}")
        if(NOT _sub_head STREQUAL _sub_recorded_sha)
            message(STATUS "surrealdb.c is at ${_sub_head}, "
                           "this repo records ${_sub_recorded_sha} — building against the checkout.")
        endif()
    endif()
endif()

# Verify submodules are present
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/surrealdb.c/CMakeLists.txt")
    message(FATAL_ERROR "The surrealdb.c submodule was not downloaded! GIT_SUBMODULE was turned off or failed. Please update submodules and try again.")
endif()

# ============================================================================
# User Options
# ============================================================================

# Build options
option(BUILD_TESTING "Build tests." ON)
option(SURREALDB_RELEASE "Build Rust library in release mode" OFF)
option(SURREALDB_USE_MODULES "Enable C++ modules for surrealdb_cpp" OFF)

# Static/Shared - On for default for now for testing.
# surrealdb.cpp is header-only: there is no C++ library to build shared or
# static, so BUILD_SHARED would have nothing to act on. The Rust library's own
# artifact kind is decided in its Cargo.toml.

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

# Make custom find modules available. This is additive and safe either way.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ----------------------------------------------------------------------------
# Everything below is a *default*, not a policy.
# ----------------------------------------------------------------------------
#
# A library must not set global CMake state when it is not the project being
# built. Language standard, extensions, output layout and warning flags all
# belong to whoever is driving the build -- a toolchain file, a superbuild, or
# a host with a build system of its own. Setting them here would silently
# override a
# host that had already chosen, and the override is invisible: the tree still
# configures, still builds, and produces objects compiled differently from
# everything they are about to be linked against.
#
# So these apply only when this project is the top-level one, and even then
# only where the value has not already been supplied. Building surrealdb.cpp
# directly gets sensible defaults; embedding it changes nothing.
#
# See cmake/toolchains/ for driving this from a host toolchain, and
# docs/embedding.md for what a host is expected to provide.

if(PROJECT_IS_TOP_LEVEL)
    if(NOT DEFINED CMAKE_C_STANDARD)
        set(CMAKE_C_STANDARD 17)
        set(CMAKE_C_STANDARD_REQUIRED ON)
    endif()

    # C++17 is the floor; newer standards light up more (see config.hpp). Not
    # pinned: the library target carries cxx_std_17 as an INTERFACE requirement,
    # which raises a consumer that asked for less and never lowers one that
    # asked for more.
    if(NOT DEFINED CMAKE_CXX_STANDARD)
        set(CMAKE_CXX_STANDARD 17)
    endif()
    if(NOT DEFINED CMAKE_CXX_STANDARD_REQUIRED)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
    endif()
    # -std=c++xx rather than -std=gnu++xx. A host that wants the GNU dialect
    # sets this itself and is left alone.
    if(NOT DEFINED CMAKE_CXX_EXTENSIONS)
        set(CMAKE_CXX_EXTENSIONS OFF)
    endif()

    # Collect build output in one place, for convenience when working here.
    if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    endif()
    if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    endif()
    if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    endif()
endif()

# To homogenize folders in IDEs.
include(GenerateExportHeader)
include(${CMAKE_SOURCE_DIR}/cmake/helpers.cmake)

if(PROJECT_IS_TOP_LEVEL)
    # Single-config generators leave this empty, which means no optimisation
    # flags at all and a blank line in the summary below.
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                     Debug Release RelWithDebInfo MinSizeRel)
    endif()

    # clangd needs this to resolve surrealdb headers while editing tests;
    # without it the whole tree reads as errors while compiling fine.
    if(NOT DEFINED CMAKE_EXPORT_COMPILE_COMMANDS)
        set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    endif()

    # IDE organisation, matching surrealdb.c's convention.
    if(NOT DEFINED CMAKE_FOLDER)
        set(CMAKE_FOLDER "surrealdb.cpp")
    endif()
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT DEFINED CMAKE_INCLUDE_CURRENT_DIR)
    set(CMAKE_INCLUDE_CURRENT_DIR ON)
endif()

# ============================================================================
# Linking
# ============================================================================
#
# Every test binary statically links the whole Rust library, and in a Debug
# build that archive is well over a gigabyte of mostly DWARF. Measured on one
# link of one test:
#
#     bfd, debug info kept      1.70 GB    9.8 s
#     lld, debug info kept      2.44 GB    1.0 s
#     lld, --strip-debug        1.28 GB    0.6 s
#
# Two things follow, and the second is the one that bites.
#
# The default linker is slow here: ten seconds a link, twenty-five links. lld
# is ten times faster and is almost always installed alongside clang.
#
# But lld also wants *more* memory per link, so switching linker without
# capping how many run at once makes the spike worse. `make -j32` with
# twenty-five link targets will happily start twenty of them together and ask
# for forty gigabytes. That is the "why is my machine on fire" case, and the
# fix is a job pool, not a faster linker.
#
# Both are top-level only. An embedding host picks its own linker and its own
# parallelism, and neither is this library's business to set.

if(PROJECT_IS_TOP_LEVEL)
    # --- linker ------------------------------------------------------------
    set(SURREALDB_CPP_LINKER "auto"
        CACHE STRING "Linker to use: auto, lld, mold, gold, bfd, or empty to leave alone")

    if(SURREALDB_CPP_LINKER STREQUAL "auto")
        set(_sdb_linker "")
        foreach(_cand mold lld)
            find_program(SURREALDB_CPP_LINKER_${_cand} ${_cand})
            if(SURREALDB_CPP_LINKER_${_cand})
                set(_sdb_linker ${_cand})
                break()
            endif()
        endforeach()
    else()
        set(_sdb_linker "${SURREALDB_CPP_LINKER}")
    endif()

    if(_sdb_linker AND NOT MSVC)
        # Checked rather than assumed: a linker can be on PATH and still be
        # rejected by the compiler driver, and finding that out at the first
        # link of a long build is a poor trade for one try here.
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_LINK_OPTIONS "-fuse-ld=${_sdb_linker}")
        check_cxx_source_compiles("int main(){return 0;}" SURREALDB_CPP_LINKER_WORKS)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)

        if(SURREALDB_CPP_LINKER_WORKS)
            add_link_options("-fuse-ld=${_sdb_linker}")
            set(SURREALDB_CPP_LINKER_IN_USE "${_sdb_linker}")
        else()
            set(SURREALDB_CPP_LINKER_IN_USE "system default (-fuse-ld=${_sdb_linker} rejected)")
        endif()
    else()
        set(SURREALDB_CPP_LINKER_IN_USE "system default")
    endif()

    # --- how many links at once -------------------------------------------
    #
    # Sized from memory rather than cores, because memory is what runs out.
    # Roughly 2.5 GB a link with lld on a debug archive, and a build has
    # compiles in flight too, so budget about an eighth of RAM.
    #
    # Ninja only: the Makefiles generator has no equivalent, and `make -j` is
    # one number for compiling and linking both. That is worth knowing rather
    # than working around -- on Makefiles this silently does nothing, which is
    # why the summary prints what actually happened.
    set(SURREALDB_CPP_LINK_JOBS "auto"
        CACHE STRING "Concurrent link jobs (Ninja only). auto, a number, or empty to leave alone")

    if(SURREALDB_CPP_LINK_JOBS STREQUAL "auto")
        cmake_host_system_information(RESULT _sdb_ram QUERY TOTAL_PHYSICAL_MEMORY)
        cmake_host_system_information(RESULT _sdb_cpus QUERY NUMBER_OF_LOGICAL_CORES)
        math(EXPR _sdb_link_jobs "${_sdb_ram} / 8192")          # MB -> ~8 GB each
        if(_sdb_link_jobs LESS 1)
            set(_sdb_link_jobs 1)
        endif()
        if(_sdb_link_jobs GREATER ${_sdb_cpus})
            set(_sdb_link_jobs ${_sdb_cpus})
        endif()
    else()
        set(_sdb_link_jobs "${SURREALDB_CPP_LINK_JOBS}")
    endif()

    if(_sdb_link_jobs AND CMAKE_GENERATOR MATCHES "Ninja")
        set_property(GLOBAL APPEND PROPERTY JOB_POOLS surrealdb_link=${_sdb_link_jobs})
        set(CMAKE_JOB_POOL_LINK surrealdb_link)
        set(SURREALDB_CPP_LINK_JOBS_IN_USE "${_sdb_link_jobs}")
    elseif(_sdb_link_jobs)
        set(SURREALDB_CPP_LINK_JOBS_IN_USE
            "unlimited (job pools need Ninja; -G Ninja to cap at ${_sdb_link_jobs})")
    else()
        set(SURREALDB_CPP_LINK_JOBS_IN_USE "unlimited")
    endif()
endif()

# Platform defaults, again only when this is the project being built.
#
# `add_compile_definitions` and `add_definitions` are directory-scoped, so they
# reach every target added below them -- including an embedding host's, if this
# project was pulled in with add_subdirectory. Choosing a Windows API level for
# someone else's build is not this library's business.
if(PROJECT_IS_TOP_LEVEL AND WIN32)
    if(NOT DEFINED CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS)
        set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endif()
    add_compile_definitions(_WIN32_WINNT=0x0601)   # Windows 7 feature set
endif()

# Note on MSVC and /Zc:__cplusplus: without it MSVC reports __cplusplus as
# 199711L regardless of /std. This library does not need the flag and does not
# impose it -- config.hpp reads _MSVC_LANG, which is correct either way. The
# previous `add_definitions(" /Zc:__cplusplus ")` was doubly wrong: a compiler
# option passed as a definition, with surrounding whitespace, applied to every
# target in the directory.

# Platform agnostic installation trees.
include(GNUInstallDirs)

# ============================================================================
# Dependencies
# ============================================================================

find_package(surrealdb_c REQUIRED)

# --- minimum surrealdb.c -----------------------------------------------------
#
# 0.1.3 calls `sr_session_fork`, `sr_runtime_init` and `sr_rpc_kill_on`.
# `detail/c_api.hpp` static_asserts this and is the real backstop -- it fires
# wherever the headers come from, including an installed package this module
# never looked inside. Checking here as well only buys an earlier and more
# obvious failure: a configure error naming both versions beats a compile error
# in a header the user did not write.
#
# The floor is read out of c_api.hpp rather than written twice, for the same
# reason the project version is read out of config.hpp.
file(READ "${PROJECT_SOURCE_DIR}/src/surrealdb_cpp/include/surrealdb/detail/c_api.hpp" _sdb_capi)
string(REGEX MATCH "SURREALDB_CPP_REQUIRES_C_MAJOR ([0-9]+)" _ "${_sdb_capi}")
set(_sdb_need "${CMAKE_MATCH_1}")
string(REGEX MATCH "SURREALDB_CPP_REQUIRES_C_MINOR ([0-9]+)" _ "${_sdb_capi}")
string(APPEND _sdb_need ".${CMAKE_MATCH_1}")
string(REGEX MATCH "SURREALDB_CPP_REQUIRES_C_PATCH ([0-9]+)" _ "${_sdb_capi}")
string(APPEND _sdb_need ".${CMAKE_MATCH_1}")
unset(_sdb_capi)

# Best effort: the header is not always locatable from here (an installed
# package carries its own include path through the target, which is not
# readable at configure time without resolving generator expressions). When it
# cannot be found the static_assert still covers it, so this reports and moves
# on rather than failing.
find_file(SURREALDB_C_HEADER surrealdb.h
    HINTS "${SURREALDB_C_INCLUDE_DIR}" "${SURREALDB_C_SOURCE_DIR}/include"
    NO_DEFAULT_PATH)
if(SURREALDB_C_HEADER)
    file(READ "${SURREALDB_C_HEADER}" _sdb_h)
    string(REGEX MATCH "SR_VERSION_STRING \"([0-9.]+)\"" _ "${_sdb_h}")
    set(SURREALDB_C_VERSION "${CMAKE_MATCH_1}")
    unset(_sdb_h)
    if(SURREALDB_C_VERSION VERSION_LESS _sdb_need)
        message(FATAL_ERROR
            "surrealdb.cpp ${PROJECT_VERSION} needs surrealdb.c ${_sdb_need} or "
            "newer; found ${SURREALDB_C_VERSION} at ${SURREALDB_C_HEADER}. "
            "Run: git submodule update --init --recursive")
    endif()
    message(STATUS "surrealdb.c ${SURREALDB_C_VERSION} (need >= ${_sdb_need})")
else()
    message(STATUS "surrealdb.c version not readable here; "
                   "detail/c_api.hpp asserts the >= ${_sdb_need} floor at compile time")
endif()
mark_as_advanced(SURREALDB_C_HEADER)

# ============================================================================
# Project structure.
# ============================================================================

add_subdirectory_with_folder("surrealdb.cpp" src/surrealdb_cpp)

# ============================================================================
# Licence files
# ============================================================================
#
# Apache-2.0 section 4 requires the NOTICE to travel with any redistribution,
# so an installed prefix has to carry it rather than leaving it in the repo.
# Only when top-level: a superproject installing us as a subdirectory has its
# own docdir and its own notices to assemble, and NOTICE names surrealdb.cpp
# specifically -- dropping it into someone else's share/doc would misattribute
# the tree it landed in.
if(PROJECT_IS_TOP_LEVEL)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    )
endif()

# ============================================================================
# Tests
# ============================================================================
#
# No framework. Every suite is a standalone binary returning non-zero on
# failure, which is what makes it cheap to sweep them across every standard and
# standard library with nothing but a compiler.
#
# Unity used to be pulled in here for a C test that no longer exists. It was a
# required submodule even with BUILD_TESTING off, and its install rules put
# `include/unity` and `lib/cmake/unity` into the consumer's prefix -- a test
# framework leaking out of a header-only library. surrealdb.c vendors its own
# Unity for its own C suite, so nothing here needs one.

# Sanitizers for the test targets.
#
# surrealdb.c defines `SURREALDB_SANITIZE` and `surrealdb_apply_sanitizers()`;
# reusing them means one flag covers both halves of the stack rather than each
# growing its own spelling. Already defined when surrealdb.c was added as a
# subproject, hence the guard.
if(NOT COMMAND surrealdb_apply_sanitizers)
    set(_sdb_sanitizers_module
        "${CMAKE_CURRENT_LIST_DIR}/surrealdb.c/cmake/Sanitizers.cmake")
    if(EXISTS "${_sdb_sanitizers_module}")
        include("${_sdb_sanitizers_module}")
    else()
        # Fetch/system paths may not have the submodule on disk; keep the call
        # sites uniform rather than making every one of them conditional.
        function(surrealdb_apply_sanitizers target)
        endfunction()
    endif()
endif()

if(BUILD_TESTING)
    enable_testing()
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

# ============================================================================
# Uninstall
# ============================================================================
#
# CMake ships no uninstall target. surrealdb.c has one, but gates it on being
# top-level -- so when this project is the top one, the whole tree had none.
# Reuses the submodule's template for the same reason the other modules are
# reused.

if(PROJECT_IS_TOP_LEVEL AND NOT TARGET uninstall)
    set(_sdb_uninstall_in
        "${CMAKE_CURRENT_LIST_DIR}/surrealdb.c/cmake/cmake_uninstall.cmake.in")
    if(EXISTS "${_sdb_uninstall_in}")
        configure_file("${_sdb_uninstall_in}"
                       "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
                       IMMEDIATE @ONLY)
        add_custom_target(uninstall
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
            COMMENT "Removing files listed in install_manifest.txt")
    endif()
endif()

# ============================================================================
# Summary
# ============================================================================

if(PROJECT_IS_TOP_LEVEL)
    message(STATUS "")
    message(STATUS "=== surrealdb.cpp ${PROJECT_VERSION} ===")
    message(STATUS "Build type:   ${CMAKE_BUILD_TYPE}")
    message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD} (floor is 17)")
    message(STATUS "Modules:      ${SURREALDB_USE_MODULES}")
    message(STATUS "Testing:      ${BUILD_TESTING}")
    message(STATUS "Linker:       ${SURREALDB_CPP_LINKER_IN_USE}")
    message(STATUS "Link jobs:    ${SURREALDB_CPP_LINK_JOBS_IN_USE}")
    message(STATUS "Examples:     ${BUILD_EXAMPLES}")
    if(SURREALDB_SANITIZE)
        message(STATUS "Sanitizers:   ${SURREALDB_SANITIZE}")
    endif()
    message(STATUS "surrealdb.c:  ${SURREALDB_C_ORIGIN}")
    message(STATUS "")
endif()
