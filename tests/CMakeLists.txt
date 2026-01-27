add_executable(surrealdb_cpp_tests
    surrealdb_cpp_tests.c
)

target_link_libraries(surrealdb_cpp_tests PRIVATE
    Unity::unity
    surrealdb::surrealdb_c
)

set_target_properties(surrealdb_cpp_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

if(MSVC)
    # Rust library uses release runtime (/MD); avoid debug runtime conflicts.
    target_link_options(surrealdb_cpp_tests PRIVATE /NODEFAULTLIB:MSVCRT)
endif()

add_test(NAME surrealdb_cpp_tests COMMAND surrealdb_cpp_tests)
