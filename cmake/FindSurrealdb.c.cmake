include_guard(GLOBAL)

include(FindPackageHandleStandardArgs)

option(SURREALDB_C_FORCE_SUBPROJECT "Force use of bundled surrealdb.c subproject" OFF)
set(SURREALDB_C_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../surrealdb.c" CACHE PATH "Path to the surrealdb.c source directory")

set(_SURREALDB_C_TARGET "surrealdb::surrealdb_c")

if(NOT TARGET ${_SURREALDB_C_TARGET} AND NOT SURREALDB_C_FORCE_SUBPROJECT)
    find_package(surrealdb_c CONFIG QUIET)
endif()

if(NOT TARGET ${_SURREALDB_C_TARGET})
    if(EXISTS "${SURREALDB_C_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "Using bundled surrealdb.c from: ${SURREALDB_C_SOURCE_DIR}")
        add_subdirectory("${SURREALDB_C_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/surrealdb.c-build")
    else()
        message(STATUS "Bundled surrealdb.c not found at: ${SURREALDB_C_SOURCE_DIR}")
    endif()
else()
    message(STATUS "Using system-installed surrealdb_c package")
endif()

if(TARGET ${_SURREALDB_C_TARGET})
    set(SURREALDB_C_TARGET ${_SURREALDB_C_TARGET})
endif()

find_package_handle_standard_args("Surrealdb.c" REQUIRED_VARS SURREALDB_C_TARGET)
