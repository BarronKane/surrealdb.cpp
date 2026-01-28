#ifdef SURREALDB_USE_MODULES
module;
#include <cstdio>
module surrealdb;
import :internal;
#else
#include <cstdio>
#define SURREALDB_COMPILING_LIBRARY
#include <surrealdb/surrealdb.hpp>
#include "internal_impl.hpp"
#endif

int hello_surreal()
{
    sr_string_t err = NULL;
    sr_surreal_t* db = NULL;
    sr_string_t ver = NULL;

    if (sr_connect(&err, &db, "mem://") < 0)
    {
        if (err) std::printf("Connection failed: %s\n", err);
        else std::printf("Connection failed\n");
        return 1;
    }

    if (sr_version(db, &err, &ver) < 0)
    {
        if (err) std::printf("Failed to get db version: %s\n", err);
        else std::printf("Failed to get db version\n");
        return 1;
    }

    std::printf("Surrealdb server version: %s\n", surrealdb::internal::get_version_string(ver));

    sr_surreal_disconnect(db);
    sr_free_string(ver);
    sr_free_string(err);

    return 0;
}
