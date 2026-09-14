#pragma once

/// @file format.hpp
/// `std::format` support, where the standard library has it.
///
/// Included by the umbrella header, and inert unless the standard library has
/// `std::format` *and* exceptions are enabled -- everything here is inside that
/// gate, so a C++17 consumer, or one building with `-fno-exceptions`, pays
/// nothing and sees nothing.
///
/// Only types with one obvious textual form are covered: the status and kind
/// enums, and `session_id`. A `value` is deliberately absent. It can hold an
/// object, an array, a geometry or a range, printing one is a decision about
/// escaping and nesting and precision that belongs to the caller, and the C
/// library's own `sr_value_print` is a debugging aid with no stable format --
/// wrapping that in `std::format` would make it look like an interface.

#include "config.hpp"

// Both gates: `std::format`'s parse contract is to throw on a bad spec, so
// these cannot be compiled without exceptions. A consumer at C++20 with
// `-fno-exceptions` simply does not get formatters -- which is the additive
// rule doing its job, rather than the header failing to compile.
#if SURREALDB_HAS_FORMAT && SURREALDB_HAS_EXCEPTIONS

#include "error.hpp"
#include "geometry.hpp"
#include "rpc.hpp"
#include "value.hpp"

#include <format>
#include <string_view>

namespace surrealdb {
namespace detail {

/// Shared by every formatter here: these types take no format spec of their
/// own, so parsing is "accept an empty spec and nothing else" and the only
/// interesting part is rejecting the rest rather than ignoring it.
struct plain_formatter_base {
    constexpr auto parse(std::format_parse_context& ctx) {
        auto it = ctx.begin();
        if (it != ctx.end() && *it != '}') {
            throw std::format_error("surrealdb: this type takes no format spec");
        }
        return it;
    }
};

} // namespace detail
} // namespace surrealdb

/// `error_code` as its name: "ok", "closed", "error", "fatal".
template <>
struct std::formatter<surrealdb::error_code> : surrealdb::detail::plain_formatter_base {
    auto format(surrealdb::error_code c, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", surrealdb::to_string(c));
    }
};

/// `value_kind` as its name: "string", "geometry", "thing", ...
template <>
struct std::formatter<surrealdb::value_kind> : surrealdb::detail::plain_formatter_base {
    auto format(surrealdb::value_kind k, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", surrealdb::to_string(k));
    }
};

template <>
struct std::formatter<surrealdb::geometry_kind> : surrealdb::detail::plain_formatter_base {
    auto format(surrealdb::geometry_kind k, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", surrealdb::to_string(k));
    }
};

template <>
struct std::formatter<surrealdb::id_kind> : surrealdb::detail::plain_formatter_base {
    auto format(surrealdb::id_kind k, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", surrealdb::to_string(k));
    }
};

/// A session id in its canonical hyphenated form.
template <>
struct std::formatter<surrealdb::session_id> : surrealdb::detail::plain_formatter_base {
    auto format(const surrealdb::session_id& id, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", id.to_string());
    }
};

/// An error as `code: message`, or just the code when there is no message.
///
/// Formatting borrows -- `message()` is a `string_view` into the error -- so
/// this never copies the message the way `to_string()` would.
template <>
struct std::formatter<surrealdb::error> : surrealdb::detail::plain_formatter_base {
    auto format(const surrealdb::error& e, std::format_context& ctx) const {
        const std::string_view msg = e.message();
        if (msg.empty()) {
            return std::format_to(ctx.out(), "{}", surrealdb::to_string(e.code()));
        }
        return std::format_to(ctx.out(), "{}: {}", surrealdb::to_string(e.code()), msg);
    }
};

#endif // SURREALDB_HAS_FORMAT && SURREALDB_HAS_EXCEPTIONS
