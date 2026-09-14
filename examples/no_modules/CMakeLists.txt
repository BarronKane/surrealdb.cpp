set(EXAMPLE_TARGET surrealdb_cpp_no_modules_example)

add_executable(${EXAMPLE_TARGET} main.cpp)

# The library is header-only and carries cxx_std_17 as an INTERFACE
# requirement, so nothing is forced here -- the example compiles at whatever
# standard the toolchain defaults to, which is part of what it demonstrates.
target_link_libraries(${EXAMPLE_TARGET} PRIVATE surrealdb::surrealdb_cpp)
