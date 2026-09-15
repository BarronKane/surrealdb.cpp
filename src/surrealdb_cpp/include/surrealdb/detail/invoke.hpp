#pragma once

/// @file detail/invoke.hpp
/// The adapter from the C error convention to `result<T>`.
///
/// Every fallible call in the C API has the same shape:
///
///     int sr_x(const sr_surreal_t* db, sr_string_t* err_ptr, R** res_ptr, ...);
///
/// It returns `SR_AGAIN` (0) on success or a negative status; on failure it
/// writes an owned message into `err_ptr`; on success it writes the result into
/// `res_ptr`. That regularity is the single highest-leverage fact about the C
/// library -- it means roughly fifty hand-written check-free-wrap blocks
/// collapse into the two functions below, and every public method in this
/// library becomes one line on top of them.
///
/// **The count caveat.** Not every call returns 0 on success: `sr_select`
/// returns the number of values it produced. Rather than a policy enum, the
/// value-returning overload hands the return code to the caller's factory, so a
/// call site decides for itself whether that integer is "success" or "length".
/// Both readings cost nothing -- the lambdas inline away entirely.

#include "../config.hpp"
#include "../error.hpp"
#include "c_api.hpp"

#include <utility>

namespace surrealdb {
namespace detail {

/// Invoke a fallible C call that produces no value.
///
/// `fn` receives the address of an `sr_string_t` to fill on failure and returns
/// the status code.
///
///     return detail::invoke([&](sr_string_t* e) {
///         return sr_use_ns(db, e, ns);
///     });
template <class Fn>
[[nodiscard]] result<void> invoke(Fn&& fn) {
    sr_string_t err = nullptr;
    const int rc = fn(&err);
    if (rc < 0) return result<void>(error::adopt(rc, err));
    // A non-negative code never sets the error string, but release it if the C
    // library ever hands one back anyway rather than leaking it.
    if (err != nullptr) ::sr_string_free(err);
    return result<void>();
}

/// Invoke a fallible C call that produces a value.
///
/// `fn` performs the call and returns the status code; `make` turns the
/// now-populated out-parameter into `T`. `make` receives the status code so
/// that count-returning calls can use it as a length:
///
///     sr_value_t* out = nullptr;
///     return detail::invoke<owned_values>(
///         [&](sr_string_t* e) { return sr_select(db, e, &out, resource); },
///         [&](int n)          { return owned_values(out, n); });
template <class T, class Fn, class Make>
[[nodiscard]] result<T> invoke(Fn&& fn, Make&& make) {
    sr_string_t err = nullptr;
    const int rc = fn(&err);
    if (rc < 0) return result<T>(error::adopt(rc, err));
    if (err != nullptr) ::sr_string_free(err);
    return result<T>(make(rc));
}

} // namespace detail
} // namespace surrealdb
