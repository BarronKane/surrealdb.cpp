#pragma once

extern "C" {
#include <surrealdb.h>
}
#include <string>
#include <expected>

#include <surrealdb_cpp_export.h>

enum class sr_error: int
{
    closed = sr_SR_CLOSED,
    error = sr_SR_ERROR,
    fatal = sr_SR_FATAL,
    // ^ negatives
    ok = sr_SR_NONE, // 0
};

struct error
{
    sr_error code;
    std::string message;
};

/**
 * Any function returning a result<T> should use `[[nodiscard]]`.
 */
template<class T>
using result = std::expected<T, error>;

template<class T>
std::unexpected<error> make_error(int code, std::string msg)
{
    return std::unexpected(error { static_cast<sr_error>(code), msg });
}
