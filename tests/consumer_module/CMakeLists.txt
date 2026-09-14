cmake_minimum_required(VERSION 3.28)   # module consumption
project(sdb_module_consumer LANGUAGES CXX)
find_package(surrealdb.cpp REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE surrealdb::surrealdb_cpp_module)
target_compile_features(app PRIVATE cxx_std_20)
