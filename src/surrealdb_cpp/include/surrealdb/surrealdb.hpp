#pragma once

#if defined(SURREALDB_USE_MODULES) && !defined(SURREALDB_COMPILING_LIBRARY)
    import surrealdb;
#else
    #include <surrealdb/connection.hpp>
#endif
