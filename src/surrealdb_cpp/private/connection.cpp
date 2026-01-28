#include <surrealdb/connection.hpp>

extern "C" {
#include <surrealdb.h>
}

namespace surrealdb {

connection::connection() noexcept
    : db_(nullptr), last_error_(nullptr), last_version_(nullptr)
{
}

connection::connection(const char* endpoint)
    : connection()
{
    connect(endpoint);
}

connection::~connection()
{
    disconnect();
    clear_version();
    clear_error();
}

connection::connection(connection&& other) noexcept
    : db_(other.db_),
      last_error_(other.last_error_),
      last_version_(other.last_version_)
{
    other.db_ = nullptr;
    other.last_error_ = nullptr;
    other.last_version_ = nullptr;
}

connection& connection::operator=(connection&& other) noexcept
{
    if (this != &other)
    {
        disconnect();
        clear_version();
        clear_error();

        db_ = other.db_;
        last_error_ = other.last_error_;
        last_version_ = other.last_version_;

        other.db_ = nullptr;
        other.last_error_ = nullptr;
        other.last_version_ = nullptr;
    }
    return *this;
}

bool connection::connect(const char* endpoint)
{
    disconnect();
    clear_version();
    clear_error();

    sr_string_t err = nullptr;
    sr_surreal_t* db = nullptr;

    if (sr_connect(&err, &db, endpoint) < 0)
    {
        last_error_ = err;
        return false;
    }

    if (err)
    {
        sr_free_string(err);
    }

    db_ = db;
    return true;
}

void connection::disconnect() noexcept
{
    if (db_)
    {
        sr_surreal_disconnect(db_);
        db_ = nullptr;
    }
}

bool connection::is_connected() const noexcept
{
    return db_ != nullptr;
}

const char* connection::version()
{
    clear_version();
    clear_error();

    if (!db_)
    {
        return "";
    }

    sr_string_t err = nullptr;
    sr_string_t ver = nullptr;

    if (sr_version(db_, &err, &ver) < 0)
    {
        last_error_ = err;
        if (ver)
        {
            sr_free_string(ver);
        }
        return "";
    }

    if (err)
    {
        sr_free_string(err);
    }

    last_version_ = ver;
    return last_version_ ? last_version_ : "";
}

const char* connection::last_error() const noexcept
{
    return last_error_ ? last_error_ : "";
}

sr_surreal_t* connection::native_handle() const noexcept
{
    return db_;
}

void connection::clear_error() noexcept
{
    if (last_error_)
    {
        sr_free_string(last_error_);
        last_error_ = nullptr;
    }
}

void connection::clear_version() noexcept
{
    if (last_version_)
    {
        sr_free_string(last_version_);
        last_version_ = nullptr;
    }
}

} // namespace surrealdb
