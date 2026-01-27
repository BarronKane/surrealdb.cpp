/**
 *  How consumers should use this:
 *
 *  .cpp:
 *  ```cpp
 *  #include <surrealdb/surreal.hpp
 *  ```
 *
 *  or
 *
 *  .cppm:
 *  ```cpp
 *  import surrealdb;
 *  ```
 *
 *  CMake:
 *  ```cmake
 *  find_package(surrealdb.cpp CONFIG REQUIRED)
 *  set(SURREALDB_USE_MODULES ON) # Or as a cmake build flag.
 *  target_link_libraries(app PRIVATE surrealdb::surrealdb_cpp)
 *  ```
*/

module;

#include <surrealdb/api/api.hpp>

export module surrealdb;

export using ::hello_surreal;
