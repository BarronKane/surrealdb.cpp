#ifndef SURREALDB_CONNECTION_HPP
#define SURREALDB_CONNECTION_HPP

#include <surrealdb_cpp_export.h>

struct sr_surreal_t;

namespace surrealdb {

class SURREALDB_CPP_EXPORT connection {

public:
    connection() noexcept;
    explicit connection(const char* endpoint);
    ~connection();

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    connection(connection&& other) noexcept;
    connection& operator=(connection&& other) noexcept;

    bool connect(const char* endpoint);
    void disconnect() noexcept;
    bool is_connected() const noexcept;

    const char* version();
    const char* last_error() const noexcept;
    sr_surreal_t* native_handle() const noexcept;

private:
    void clear_error() noexcept;
    void clear_version() noexcept;

    sr_surreal_t* db_;
    char* last_error_;
    char* last_version_;
};

}

#endif // SURREALDB_CONNECTION_HPP
