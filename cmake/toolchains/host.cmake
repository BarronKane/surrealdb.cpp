# host.cmake — drive this build from another build system's toolchain.
#
#   cmake -S . -B build \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/host.cmake \
#         -DHOST_CXX_COMPILER=/path/to/clang++ \
#         -DHOST_CXX_STANDARD=20 \
#         -DHOST_CXX_FLAGS="-fno-exceptions -fno-rtti"
#
# Everything is optional: whatever is not given is left to CMake's own
# detection. The point is that a host with an opinion can state it in one place
# rather than by patching this project.
#
# This is deliberately generic and is the only toolchain entry point this
# project ships. A host with specific requirements -- a cross SDK, a vendored
# NDK, a superbuild, a host that bundles its own compiler and standard library,
# a distro packager pinning a toolchain -- supplies the answers through
# these variables. None of that knowledge belongs here; this file's job is to
# have somewhere to put it.

# Toolchain files are included again inside every try_compile sub-project, and
# variables passed with -D on the command line are NOT inherited there. Without
# this the compiler check fails complaining that a variable is unset, from a
# temporary directory -- which reads like a user error and is not one.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
     HOST_C_COMPILER HOST_CXX_COMPILER HOST_AR HOST_RANLIB
     HOST_TARGET_TRIPLE HOST_SYSROOT
     HOST_C_STANDARD HOST_CXX_STANDARD HOST_CXX_EXTENSIONS
     HOST_C_FLAGS HOST_CXX_FLAGS HOST_EXE_LINKER_FLAGS)

# Compiler and linker ---------------------------------------------------------

if(DEFINED HOST_C_COMPILER)
    set(CMAKE_C_COMPILER "${HOST_C_COMPILER}" CACHE FILEPATH "C compiler" FORCE)
endif()
if(DEFINED HOST_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "${HOST_CXX_COMPILER}" CACHE FILEPATH "C++ compiler" FORCE)
endif()
if(DEFINED HOST_AR)
    set(CMAKE_AR "${HOST_AR}" CACHE FILEPATH "archiver" FORCE)
endif()
if(DEFINED HOST_RANLIB)
    set(CMAKE_RANLIB "${HOST_RANLIB}" CACHE FILEPATH "ranlib" FORCE)
endif()

# Target and sysroot ----------------------------------------------------------

if(DEFINED HOST_TARGET_TRIPLE)
    set(CMAKE_C_COMPILER_TARGET "${HOST_TARGET_TRIPLE}" CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILER_TARGET "${HOST_TARGET_TRIPLE}" CACHE STRING "" FORCE)
endif()
if(DEFINED HOST_SYSROOT)
    set(CMAKE_SYSROOT "${HOST_SYSROOT}" CACHE PATH "" FORCE)
endif()

# Language standard -----------------------------------------------------------
#
# Set here rather than left to the project, so the host's choice is what the
# project sees as "already defined" and declines to override.

if(DEFINED HOST_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD "${HOST_CXX_STANDARD}" CACHE STRING "" FORCE)
    set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
endif()
if(DEFINED HOST_C_STANDARD)
    set(CMAKE_C_STANDARD "${HOST_C_STANDARD}" CACHE STRING "" FORCE)
    set(CMAKE_C_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
endif()
if(DEFINED HOST_CXX_EXTENSIONS)
    set(CMAKE_CXX_EXTENSIONS "${HOST_CXX_EXTENSIONS}" CACHE BOOL "" FORCE)
endif()

# Flags -----------------------------------------------------------------------
#
# Appended, not replaced: CMake's own configuration flags (-g, -O2) still apply.

if(DEFINED HOST_C_FLAGS)
    set(CMAKE_C_FLAGS_INIT "${HOST_C_FLAGS}")
endif()
if(DEFINED HOST_CXX_FLAGS)
    set(CMAKE_CXX_FLAGS_INIT "${HOST_CXX_FLAGS}")
endif()
if(DEFINED HOST_EXE_LINKER_FLAGS)
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${HOST_EXE_LINKER_FLAGS}")
endif()

# A host driving this build almost never wants the samples or the suite.
if(NOT DEFINED BUILD_EXAMPLES)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
endif()
if(NOT DEFINED BUILD_TESTING)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
endif()
