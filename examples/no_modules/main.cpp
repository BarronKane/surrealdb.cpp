#include <cstdio>
#include <surrealdb/surrealdb.hpp>

int main()
{
    surrealdb::connection db;
    if (!db.connect("mem://"))
    {
        std::printf("Connection failed: %s\n", db.last_error());
        return 1;
    }

    const char* version = db.version();
    if (!version || version[0] == '\0')
    {
        std::printf("Failed to get db version: %s\n", db.last_error());
        return 1;
    }

    std::printf("Surrealdb server version: %s\n", version);
    return 0;
}
