import surrealdb;
#include <cstdio>

int main()
{
    surrealdb::connection db;
    if (!db.connect("mem://"))
    {
        std::printf("Connection failed: %s\n", db.last_error().c_str());
        return 1;
    }

    const auto version = db.version();
    if (!version || version->empty())
    {
        const auto message = version ? db.last_error() : version.error().message;
        std::printf("Failed to get db version: %s\n", message.c_str());
        return 1;
    }

    std::printf("Surrealdb server version: %s\n", version->c_str());
    return 0;
}
