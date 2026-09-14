# ============================================================================
# surrealdb.cpp — the library
# ============================================================================
#
# Header-only, so this is an INTERFACE target: there is nothing to compile.
# That is not a shortcut, it is what the design produces. Every type here is a
# compile-time construct -- result<T>, the RAII handles, the value views -- and
# once they evaporate there is no object code left to build. It also keeps the
# call sites able to see through to the C functions, which a compiled boundary
# would hide from the optimiser without LTO.
#
# The one thing that does get compiled is the C++20 module interface, and only
# when modules are asked for.

set(SURREALDB_CPP_TARGET surrealdb_cpp)

add_library(${SURREALDB_CPP_TARGET} INTERFACE)
add_library(surrealdb::surrealdb_cpp ALIAS ${SURREALDB_CPP_TARGET})

target_include_directories(${SURREALDB_CPP_TARGET}
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# C++17 is the floor. Consumers may compile at 20 or 23 and the headers light
# up accordingly -- see config.hpp, which is the only file permitted to look at
# __cplusplus.
target_compile_features(${SURREALDB_CPP_TARGET} INTERFACE cxx_std_17)

set(_sdb_c_target surrealdb::surrealdb_c)
if(DEFINED SURREALDB_C_TARGET)
    set(_sdb_c_target ${SURREALDB_C_TARGET})
endif()

# The C dependency is attached to the *build* interface only, and re-attached
# for consumers by surrealdb.cppConfig.cmake.
#
# It cannot simply be linked, because in the bundled path
# `surrealdb::surrealdb_c` is an ALIAS for an in-tree target named
# `surrealdb_c` (surrealdb.c's RustBuild.cmake), and CMake writes the *aliased*
# name into the export -- inside `$<INSTALL_INTERFACE:>` as well, since aliases
# are resolved when the export is generated. The installed config then carried
# `INTERFACE_LINK_LIBRARIES "surrealdb_c"`, which a consumer resolved as the
# bare flag `-lsurrealdb_c`: `find_package` configured cleanly and then failed
# at the link with "cannot find -lsurrealdb_c", with the library and its own
# config sitting correctly in the same prefix.
#
# Leaving the install interface empty here and appending the namespaced target
# in the Config file sidesteps alias resolution entirely.
target_link_libraries(${SURREALDB_CPP_TARGET} INTERFACE
    $<BUILD_INTERFACE:${_sdb_c_target}>)

# Headers, listed so IDEs and the install step both see them.
set(SDB_PUBLIC_HEADERS
    include/surrealdb/surrealdb.hpp
    include/surrealdb/config.hpp
    include/surrealdb/error.hpp
    include/surrealdb/value.hpp
    include/surrealdb/array.hpp
    include/surrealdb/object.hpp
    include/surrealdb/format.hpp
    include/surrealdb/geometry.hpp
    include/surrealdb/make.hpp
    include/surrealdb/options.hpp
    include/surrealdb/poll.hpp
    include/surrealdb/results.hpp
    include/surrealdb/rpc.hpp
    include/surrealdb/stream.hpp
    include/surrealdb/dsl/arena.hpp
    include/surrealdb/dsl/guards.hpp
    include/surrealdb/dsl/expr.hpp
    include/surrealdb/dsl/query.hpp
    include/surrealdb/connection.hpp
    include/surrealdb/detail/c_api.hpp
    include/surrealdb/detail/owned.hpp
    include/surrealdb/detail/invoke.hpp
)

target_sources(${SURREALDB_CPP_TARGET}
    INTERFACE
        FILE_SET public_headers TYPE HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
        FILES ${SDB_PUBLIC_HEADERS}
)

# ----------------------------------------------------------------------------
# C++20 module
# ----------------------------------------------------------------------------
#
# A re-export of the same headers, nothing more, so `import surrealdb;` and
# `#include <surrealdb/surrealdb.hpp>` are interchangeable.

if(SURREALDB_USE_MODULES)
    add_library(surrealdb_cpp_module STATIC)
    add_library(surrealdb::surrealdb_cpp_module ALIAS surrealdb_cpp_module)

    target_sources(surrealdb_cpp_module
        PUBLIC
            FILE_SET cxx_modules TYPE CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules
            FILES ${CMAKE_CURRENT_SOURCE_DIR}/modules/surrealdb.cppm
    )
    target_link_libraries(surrealdb_cpp_module PUBLIC ${SURREALDB_CPP_TARGET})
    target_compile_features(surrealdb_cpp_module PUBLIC cxx_std_20)
    set_target_properties(surrealdb_cpp_module PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

# The version lives in two places -- `project()` above and config.hpp, which is
# what a consumer can actually read at compile time. Two places drift, so this
# fails the configure rather than shipping a header that lies about itself.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/surrealdb/config.hpp" _sdb_cfg)
string(REGEX MATCH "SURREALDB_CPP_VERSION_MAJOR ([0-9]+)" _ "${_sdb_cfg}")
set(_sdb_h_major "${CMAKE_MATCH_1}")
string(REGEX MATCH "SURREALDB_CPP_VERSION_MINOR ([0-9]+)" _ "${_sdb_cfg}")
set(_sdb_h_minor "${CMAKE_MATCH_1}")
string(REGEX MATCH "SURREALDB_CPP_VERSION_PATCH ([0-9]+)" _ "${_sdb_cfg}")
set(_sdb_h_patch "${CMAKE_MATCH_1}")
set(_sdb_h_version "${_sdb_h_major}.${_sdb_h_minor}.${_sdb_h_patch}")
if(NOT _sdb_h_version VERSION_EQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "Version mismatch: project() says ${PROJECT_VERSION}, "
        "config.hpp says ${_sdb_h_version}. Update both.")
endif()
unset(_sdb_cfg)

# ----------------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------------

include(CMakePackageConfigHelpers)
set(_sdb_cpp_config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/surrealdb.cpp")

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../cmake/surrealdb.cppConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/surrealdb.cppConfig.cmake"
    INSTALL_DESTINATION "${_sdb_cpp_config_dir}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/surrealdb.cppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

set(_sdb_install_targets ${SURREALDB_CPP_TARGET})

# The module target is installed alongside the headers when it exists.
#
# Without this an installed package built with SURREALDB_USE_MODULES=ON still
# had no `surrealdb::surrealdb_cpp_module` in its config, so a consumer could
# `find_package` it and then find `import surrealdb;` unavailable -- the module
# existed only inside this build tree.
#
# CXX_MODULES_BMI installs the compiled interface; `FILE_SET cxx_modules`
# installs the .cppm itself, which is what lets a consumer on a different
# compiler rebuild it. Both are needed: a BMI is not portable between
# toolchains.
set(_sdb_export_module_args "")
if(TARGET surrealdb_cpp_module)
    list(APPEND _sdb_install_targets surrealdb_cpp_module)
    list(APPEND _sdb_install_targets_fileset
         FILE_SET cxx_modules
         DESTINATION ${CMAKE_INSTALL_DATADIR}/surrealdb.cpp/modules)
    set(_sdb_export_module_args CXX_MODULES_DIRECTORY cxx-modules)
endif()

# One call, not two: a target cannot be exported by one `install(TARGETS)` and
# have its file sets installed by another -- CMake rejects that with "is
# exported but not all of its interface file sets are installed". The module's
# `cxx_modules` set has to travel with the same command that exports it.
#
# `FILE_SET cxx_modules` installs the .cppm source and `CXX_MODULES_BMI` the
# compiled interface. Both, because a BMI is not portable across toolchains: a
# consumer on a different compiler rebuilds from the source, and one on a
# matching compiler can use what is already built.
install(TARGETS ${_sdb_install_targets}
    EXPORT surrealdb_cppTargets
    FILE_SET public_headers DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    ${_sdb_install_targets_fileset}
    CXX_MODULES_BMI DESTINATION ${CMAKE_INSTALL_LIBDIR}/surrealdb.cpp/bmi
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT surrealdb_cppTargets
    NAMESPACE surrealdb::
    DESTINATION ${_sdb_cpp_config_dir}
    ${_sdb_export_module_args}
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/surrealdb.cppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/surrealdb.cppConfigVersion.cmake"
    DESTINATION ${_sdb_cpp_config_dir}
)
# A build-tree package: point CMAKE_PREFIX_PATH at the build directory and
# `find_package(surrealdb.cpp)` works without installing anything, which is what
# a superproject developing against both halves at once wants.
export(EXPORT surrealdb_cppTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/surrealdb_cppTargets.cmake"
    NAMESPACE surrealdb::
)
