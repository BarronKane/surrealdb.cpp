#ifndef SURREALDB_SURREALDB_HPP
#define SURREALDB_SURREALDB_HPP

#if defined(SURREALDB_USE_MODULES) && !defined(SURREALDB_COMPILING_LIBRARY)
    import surrealdb;
#else
    #include <surrealdb/api/api.hpp>
#endif

#endif // SURREALDB_SURREALDB_HPP
