set(SURREALDB_CPP_TARGET surrealdb_cpp)

set(_sdb_lib_type STATIC)
if(DEFINED BUILD_SHARED)
    if(BUILD_SHARED)
        set(_sdb_lib_type SHARED)
    endif()
elseif(BUILD_SHARED_LIBS)
    set(_sdb_lib_type SHARED)
endif()

add_library(${SURREALDB_CPP_TARGET} ${_sdb_lib_type})
add_library(surrealdb::surrealdb_cpp ALIAS ${SURREALDB_CPP_TARGET})

generate_export_header(${SURREALDB_CPP_TARGET}
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/surrealdb_cpp_export.h"
)

set(SDB_PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include/surrealdb/surrealdb.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/surrealdb/api/api.hpp

    # DLL export header.
    ${CMAKE_CURRENT_BINARY_DIR}/surrealdb_cpp_export.h
)


target_sources(${SURREALDB_CPP_TARGET}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/private/hello_surreal.cpp
    PUBLIC
        FILE_SET public_headers TYPE HEADERS
        BASE_DIRS
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}
        FILES
            ${SDB_PUBLIC}
)

if(SURREALDB_USE_MODULES)
    target_sources(${SURREALDB_CPP_TARGET}
        PUBLIC
            FILE_SET cxx_modules TYPE CXX_MODULES
            BASE_DIRS
                ${CMAKE_CURRENT_SOURCE_DIR}/modules
            FILES
                ${CMAKE_CURRENT_SOURCE_DIR}/modules/surrealdb/surrealdb.cppm
                ${CMAKE_CURRENT_SOURCE_DIR}/modules/surrealdb/internal.cppm
    )
    target_compile_definitions(${SURREALDB_CPP_TARGET} PRIVATE SURREALDB_USE_MODULES=1)
    set_target_properties(${SURREALDB_CPP_TARGET} PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

target_include_directories(${SURREALDB_CPP_TARGET}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/private
)

target_compile_features(${SURREALDB_CPP_TARGET} PUBLIC cxx_std_23)

set(_sdb_c_target surrealdb::surrealdb_c)
if(DEFINED SURREALDB_C_TARGET)
    set(_sdb_c_target ${SURREALDB_C_TARGET})
endif()

target_link_libraries(${SURREALDB_CPP_TARGET}
    PUBLIC
        ${_sdb_c_target}
)

if(_sdb_lib_type STREQUAL "STATIC")
    target_compile_definitions(${SURREALDB_CPP_TARGET} PUBLIC SURREALDB_CPP_STATIC_DEFINE)
endif()

set_target_properties(${SURREALDB_CPP_TARGET} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    LINKER_LANGUAGE CXX
)

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

set(_sdb_install_filesets
    FILE_SET public_headers DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
if(SURREALDB_USE_MODULES)
    list(APPEND _sdb_install_filesets
        FILE_SET cxx_modules DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

install(TARGETS ${SURREALDB_CPP_TARGET}
    EXPORT surrealdb_cppTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ${_sdb_install_filesets}
)

install(EXPORT surrealdb_cppTargets
    NAMESPACE surrealdb::
    DESTINATION ${_sdb_cpp_config_dir}
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/surrealdb.cppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/surrealdb.cppConfigVersion.cmake"
    DESTINATION ${_sdb_cpp_config_dir}
)

export(EXPORT surrealdb_cppTargets
    FILE "${CMAKE_CURRENT_BINARY_DIR}/surrealdb_cppTargets.cmake"
    NAMESPACE surrealdb::
)
