set(EXAMPLE_TARGET surrealdb_cpp_no_modules_example)

add_executable(${EXAMPLE_TARGET} main.cpp)
target_link_libraries(${EXAMPLE_TARGET} PRIVATE surrealdb_cpp)
target_compile_features(${EXAMPLE_TARGET} PRIVATE cxx_std_23)
