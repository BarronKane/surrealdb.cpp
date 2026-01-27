#pragma once

#if defined(SURREALDB_USE_MODULES)
    import surrealdb;
#else
    #include <surrealdb/api/api.hpp>
#endif
