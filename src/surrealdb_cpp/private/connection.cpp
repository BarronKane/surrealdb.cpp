#include <surrealdb/connection.hpp>
#include <utility>

extern "C" {
#include <surrealdb.h>
}

namespace surrealdb {

connection::connection() noexcept
    : db_(nullptr)
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
      last_error_(std::move(other.last_error_)),
      last_version_(std::move(other.last_version_))
{
    other.db_ = nullptr;
}

connection& connection::operator=(connection&& other) noexcept
{
    if (this != &other)
    {
        disconnect();
        clear_version();
        clear_error();

        db_ = other.db_;
        last_error_ = std::move(other.last_error_);
        last_version_ = std::move(other.last_version_);

        other.db_ = nullptr;
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
        if (err)
        {
            last_error_ = err;
            sr_free_string(err);
        }
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

result<std::string> connection::version()
{
    clear_version();
    clear_error();

    if (!db_)
    {
        return "";
    }

    sr_string_t err = nullptr;
    sr_string_t ver = nullptr;

    if (const int e = sr_version(db_, &err, &ver); e < 0)
    {
        if (err)
        {
            last_error_ = err;
            sr_free_string(err);
        }
        if (ver)
        {
            sr_free_string(ver);
        }
        return make_error<std::string>(e, last_error_);
    }

    // This should never fire, but leaving for the moment.
    if (err)
    {
        sr_free_string(err);
    }

    if (ver)
    {
        last_version_ = ver;
        sr_free_string(ver);
    }
    else
    {
        last_version_.clear();
    }

    return last_version_;
}

std::string connection::last_error() const noexcept
{
    return last_error_;
}

sr_surreal_t* connection::native_handle() const noexcept
{
    return db_;
}

void connection::clear_error() noexcept
{
    last_error_.clear();
}

void connection::clear_version() noexcept
{
    last_version_.clear();
}

} // namespace surrealdb
