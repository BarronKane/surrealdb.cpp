# FindUnity.cmake
#
# This module prefers a bundled Unity source checkout. It can optionally use a
# system install if UNITY_USE_SYSTEM is enabled.
#
# Output:
#   Unity::unity - target for linking
#   UNITY_FOUND  - TRUE if Unity was found or built successfully

include_guard(GLOBAL)

include(FetchContent)
include(FindPackageHandleStandardArgs)

option(UNITY_USE_SYSTEM "Use a system-installed Unity if available" OFF)

if(UNITY_USE_SYSTEM)
    find_path(UNITY_INCLUDE_DIR
        NAMES unity.h
        PATHS
            /usr/include
            /usr/local/include
            /opt/local/include
            ${UNITY_ROOT}/include
            $ENV{UNITY_ROOT}/include
        PATH_SUFFIXES
            unity
    )

    find_library(UNITY_LIBRARY
        NAMES unity libunity
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/local/lib
            ${UNITY_ROOT}/lib
            $ENV{UNITY_ROOT}/lib
    )
endif()

if(UNITY_INCLUDE_DIR AND UNITY_LIBRARY)
    message(STATUS "Found system Unity: ${UNITY_LIBRARY}")
    message(STATUS "Unity include dir: ${UNITY_INCLUDE_DIR}")

    if(NOT TARGET Unity::unity)
        add_library(Unity::unity UNKNOWN IMPORTED)
        set_target_properties(Unity::unity PROPERTIES
            IMPORTED_LOCATION "${UNITY_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${UNITY_INCLUDE_DIR}"
        )
    endif()

    set(UNITY_FOUND TRUE)
else()
    message(STATUS "Fetching Unity from source...")

    set(UNITY_EXTENSION_FIXTURE ON CACHE BOOL "Enable Unity Fixture extension" FORCE)
    set(UNITY_SOURCE_DIR "${PROJECT_SOURCE_DIR}/ThirdParty/Unity")

    if(EXISTS "${UNITY_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "Unity source found at: ${UNITY_SOURCE_DIR}")
    else()
        message(STATUS "Cloning Unity v2.6.1 into ThirdParty/Unity...")

        FetchContent_Declare(
            unity
            GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
            GIT_TAG        v2.6.1
            SOURCE_DIR     "${UNITY_SOURCE_DIR}"
        )
        FetchContent_MakeAvailable(unity)
    endif()

    if(NOT TARGET unity)
        add_subdirectory("${UNITY_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/ThirdParty/Unity-build")
    endif()

    if(TARGET unity AND MSVC)
        target_compile_options(unity PRIVATE
            /wd4820  # 'struct': 'n' bytes padding added
            /wd5045  # Compiler will insert Spectre mitigation
        )
    endif()

    if(TARGET unity AND NOT TARGET Unity::unity)
        add_library(Unity::unity ALIAS unity)
    endif()

    set(UNITY_INCLUDE_DIR "${UNITY_SOURCE_DIR}/src")
    set(UNITY_LIBRARY "unity")
    set(UNITY_FOUND TRUE)

    message(STATUS "Unity will be built from source at: ${UNITY_SOURCE_DIR}")
endif()

find_package_handle_standard_args(Unity
    REQUIRED_VARS UNITY_LIBRARY UNITY_INCLUDE_DIR
)

mark_as_advanced(UNITY_INCLUDE_DIR UNITY_LIBRARY)
