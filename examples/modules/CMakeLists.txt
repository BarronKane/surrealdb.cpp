set(EXAMPLE_TARGET surrealdb_cpp_modules_example)

add_executable(${EXAMPLE_TARGET} main.cpp)
target_link_libraries(${EXAMPLE_TARGET} PRIVATE surrealdb_cpp)
target_compile_features(${EXAMPLE_TARGET} PRIVATE cxx_std_23)
target_compile_definitions(${EXAMPLE_TARGET} PRIVATE SURREALDB_USE_MODULES=1)
set_target_properties(${EXAMPLE_TARGET} PROPERTIES CXX_SCAN_FOR_MODULES ON)
