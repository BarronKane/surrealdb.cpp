set(EXAMPLE_TARGET surrealdb_cpp_modules_example)

add_executable(${EXAMPLE_TARGET} main.cpp)

# `import surrealdb;` needs the module target, which only exists when
# SURREALDB_USE_MODULES is on -- the parent guards this directory on that.
target_link_libraries(${EXAMPLE_TARGET} PRIVATE surrealdb::surrealdb_cpp_module)
target_compile_features(${EXAMPLE_TARGET} PRIVATE cxx_std_20)
