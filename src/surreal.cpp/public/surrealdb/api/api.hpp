#pragma once

extern "C" {
#include <surrealdb.h>
}

#include <cstdio>

inline int hello_surreal()
{
    sr_string_t err = NULL;
    sr_surreal_t* db = NULL;
    sr_string_t ver = NULL;

    if (sr_connect(&err, &db, "mem://") < 0)
    {
        printf("Connection failed: %s\n", err);
        return 1;
    }

    if (sr_version(db, &err, &ver) < 0)
    {
        printf("Failed to get db version: %s", err);
        return 1;
    }

    printf("Surrealdb server version: %s\n", ver);

    sr_surreal_disconnect(db);
    sr_free_string(ver);
    sr_free_string(err);

    return 0;
}
