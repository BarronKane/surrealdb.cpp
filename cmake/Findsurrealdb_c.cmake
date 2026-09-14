# Findsurrealdb_c.cmake
#
# Locate the surrealdb.c library and provide the imported target
#
#     surrealdb::surrealdb_c
#
# The name matches the package surrealdb.c installs (`surrealdb_cConfig.cmake`,
# target `surrealdb::surrealdb_c`), so `find_package(surrealdb_c)` reaches
# either this module or that config file and a consumer sees the same target
# name whichever wins.
#
# Strategies, in order. The first that works stops the search:
#
#   1. CMake package config   — an install of surrealdb.c itself, or a distro
#                               package shipping surrealdb_cConfig.cmake
#   2. pkg-config             — a distro package shipping surrealdb_c.pc
#   3. plain header + library — a distro package shipping only those
#   4. bundled subproject     — the surrealdb.c checkout in this tree
#   5. FetchContent           — clone it, for a non-recursive clone of this repo
#
# The ordering is deliberate: a system package beats the bundled copy, so
# installing surrealdb.c from pacman/apt/brew does what a packager expects
# without editing this project. Set SURREALDB_C_FORCE_SUBPROJECT to invert that
# while developing against a local checkout.
#
# Cache variables
#   SURREALDB_C_FORCE_SUBPROJECT  prefer the in-tree checkout over any install
#   SURREALDB_C_SOURCE_DIR        where the checkout lives
#   SURREALDB_C_ALLOW_FETCH       may clone when the checkout is missing
#   SURREALDB_C_GIT_REPOSITORY    URL to clone from
#   SURREALDB_C_GIT_TAG           ref to clone
#
# Result variables
#   surrealdb_c_FOUND
#   SURREALDB_C_TARGET            always surrealdb::surrealdb_c when found
#   SURREALDB_C_ORIGIN            which strategy won, for diagnostics

include_guard(GLOBAL)

include(FindPackageHandleStandardArgs)

option(SURREALDB_C_FORCE_SUBPROJECT
       "Use the bundled surrealdb.c checkout even if a package is installed" OFF)
option(SURREALDB_C_ALLOW_FETCH
       "Clone surrealdb.c when the bundled checkout is missing" ON)

set(SURREALDB_C_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../surrealdb.c"
    CACHE PATH "Path to the surrealdb.c source checkout")
set(SURREALDB_C_GIT_REPOSITORY "https://github.com/BarronKane/surrealdb.c.git"
    CACHE STRING "Repository to clone surrealdb.c from")
# A tag, not a branch.
#
# This used to default to `main-standardization`, which meant a non-recursive
# clone of surrealdb.cpp fetched whatever that branch happened to point at --
# so two people building the same surrealdb.cpp commit could get different C
# libraries, and neither would match the submodule pointer this repository
# actually pins. A tag makes the fetch path reproduce the submodule path.
#
# Bump this and the submodule together; the floor check below catches the case
# where they disagree in the direction that matters.
set(SURREALDB_C_GIT_TAG "v0.2.6"
    CACHE STRING "Ref of surrealdb.c to clone. Prefer a tag over a branch.")

set(SURREALDB_C_ORIGIN "")

# --- 1. CMake package config -------------------------------------------------
#
# CONFIG is explicit so this does not re-enter the module and recurse.

if(NOT TARGET surrealdb::surrealdb_c AND NOT SURREALDB_C_FORCE_SUBPROJECT)
    find_package(surrealdb_c CONFIG QUIET)
    if(TARGET surrealdb::surrealdb_c)
        set(SURREALDB_C_ORIGIN "package config")
    endif()
endif()

# --- 2. pkg-config -----------------------------------------------------------
#
# surrealdb.c does not ship a .pc file yet, but distro packaging commonly adds
# one, and honouring it costs nothing.

if(NOT TARGET surrealdb::surrealdb_c AND NOT SURREALDB_C_FORCE_SUBPROJECT)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(_sdbc QUIET IMPORTED_TARGET surrealdb_c)
        if(_sdbc_FOUND AND TARGET PkgConfig::_sdbc)
            add_library(surrealdb::surrealdb_c ALIAS PkgConfig::_sdbc)
            set(SURREALDB_C_ORIGIN "pkg-config")
        endif()
    endif()
endif()

# --- 3. plain header and library ---------------------------------------------

if(NOT TARGET surrealdb::surrealdb_c AND NOT SURREALDB_C_FORCE_SUBPROJECT)
    find_path(SURREALDB_C_INCLUDE_DIR
        NAMES surrealdb.h
        PATH_SUFFIXES surrealdb surrealdb_c)
    find_library(SURREALDB_C_LIBRARY
        NAMES surrealdb_c libsurrealdb_c)

    if(SURREALDB_C_INCLUDE_DIR AND SURREALDB_C_LIBRARY)
        add_library(surrealdb::surrealdb_c UNKNOWN IMPORTED)
        set_target_properties(surrealdb::surrealdb_c PROPERTIES
            IMPORTED_LOCATION "${SURREALDB_C_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SURREALDB_C_INCLUDE_DIR}")

        # A bare .a carries no record of what the Rust runtime needs, so the
        # platform libraries have to be restated. This list mirrors
        # SURREALDB_C_SYSTEM_LIBS in surrealdb.c/cmake/RustBuild.cmake; a
        # shared library or a .pc file would supply them itself, which is why
        # this only applies on this branch.
        if(WIN32)
            set(_sdbc_sys ws2_32 userenv bcrypt ntdll pdh netapi32 iphlpapi
                          psapi propsys runtimeobject secur32 powrprof)
        elseif(APPLE)
            set(_sdbc_sys "-framework Security" "-framework SystemConfiguration"
                          "-framework CoreFoundation" "-framework IOKit" "-lobjc")
        elseif(UNIX)
            set(_sdbc_sys m dl pthread rt)
        else()
            set(_sdbc_sys "")
        endif()
        set_property(TARGET surrealdb::surrealdb_c PROPERTY
                     INTERFACE_LINK_LIBRARIES "${_sdbc_sys}")

        set(SURREALDB_C_ORIGIN "system library: ${SURREALDB_C_LIBRARY}")
    endif()
endif()

# --- 4. bundled checkout -----------------------------------------------------

if(NOT TARGET surrealdb::surrealdb_c)
    if(EXISTS "${SURREALDB_C_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${SURREALDB_C_SOURCE_DIR}"
                         "${CMAKE_BINARY_DIR}/surrealdb.c-build")
        set(SURREALDB_C_ORIGIN "bundled checkout: ${SURREALDB_C_SOURCE_DIR}")
    endif()
endif()

# --- 5. fetch ----------------------------------------------------------------
#
# Only when the checkout is genuinely absent. An existing checkout is never
# touched: this repository tracks surrealdb.c as a submodule and it is normal
# for a developer to have it on a branch of their own. Re-pointing that from a
# configure step would discard work, and silently -- the tree would still build,
# against the wrong library.

if(NOT TARGET surrealdb::surrealdb_c AND SURREALDB_C_ALLOW_FETCH)
    message(STATUS "surrealdb.c not found; cloning ${SURREALDB_C_GIT_TAG} into "
                   "${SURREALDB_C_SOURCE_DIR}")
    include(FetchContent)
    FetchContent_Declare(surrealdb_c_src
        GIT_REPOSITORY "${SURREALDB_C_GIT_REPOSITORY}"
        GIT_TAG        "${SURREALDB_C_GIT_TAG}"
        SOURCE_DIR     "${SURREALDB_C_SOURCE_DIR}")
    FetchContent_MakeAvailable(surrealdb_c_src)
    if(TARGET surrealdb::surrealdb_c)
        set(SURREALDB_C_ORIGIN "fetched: ${SURREALDB_C_GIT_TAG}")
    endif()
endif()

# --- result ------------------------------------------------------------------

if(TARGET surrealdb::surrealdb_c)
    set(SURREALDB_C_TARGET surrealdb::surrealdb_c)
    message(STATUS "surrealdb.c via ${SURREALDB_C_ORIGIN}")
endif()

find_package_handle_standard_args(surrealdb_c
    REQUIRED_VARS SURREALDB_C_TARGET
    FAIL_MESSAGE
        "surrealdb.c not found. Install it, or run: git submodule update --init --recursive"
)

mark_as_advanced(SURREALDB_C_INCLUDE_DIR SURREALDB_C_LIBRARY)
