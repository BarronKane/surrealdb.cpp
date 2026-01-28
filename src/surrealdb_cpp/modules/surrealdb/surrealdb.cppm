module;
#define SURREALDB_COMPILING_LIBRARY
#include <surrealdb/surrealdb.hpp>
export module surrealdb;

export import :internal;

export using ::hello_surreal;
