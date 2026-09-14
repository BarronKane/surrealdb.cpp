#pragma once

/// @file poll.hpp
/// The outcome of a wait that is allowed to give up.
///
/// A bounded read has **three** outcomes, not two, and the distance between
/// the last two is the entire reason this type exists:
///
/// - a value arrived                              -- `ready`
/// - the wait expired, the source is still live   -- `timed_out`, call again
/// - the source is finished                       -- `ended`, stop
///
/// Collapsing `timed_out` into `ended` turns a merely slow notification into an
/// abandoned stream. Collapsing it the other way turns a finished stream into a
/// spin. surrealdb.c 0.2.6 went out of its way to keep them apart -- `SR_TIMEOUT`
/// is a distinct code from `SR_NONE`, and its header says so at length -- so
/// flattening them back into an empty `optional`, which cannot say which it was,
/// would undo that at the boundary.
///
/// `result<poll<T>>` is therefore four states: failed, ended, timed out, ready.
/// Every one of them has to be handled by a reader that loops, which is the
/// point.

#include "config.hpp"
#include "error.hpp"

#include <chrono>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace surrealdb {

/// Which of the three outcomes a `poll` holds.
enum class poll_state : int {
    /// A value arrived and is available.
    ready,
    /// The wait expired with the source still open. Call again.
    timed_out,
    /// The source is finished. Calling again is pointless.
    ended,
};

/// Human-readable name for a poll state. Never null.
[[nodiscard]] constexpr const char* to_string(poll_state s) noexcept {
    switch (s) {
        case poll_state::ready:     return "ready";
        case poll_state::timed_out: return "timed_out";
        case poll_state::ended:     return "ended";
    }
    return "unknown";
}

namespace detail {

/// Convert a `std::chrono::duration` to the `int` milliseconds the C API takes,
/// with `poll(2)` semantics: negative waits forever, zero polls once.
///
/// Written as a plain cast this is wrong three separate ways, and two of them
/// fail *silently* in the dangerous direction:
///
/// - **Truncation.** `500us` casts to 0, and 0 means "return immediately". A
///   caller who asked for a short bounded wait would get a busy spin instead.
///   Rounding up keeps a sub-millisecond request a wait.
/// - **Overflow.** A duration past `INT_MAX` milliseconds wraps to a negative
///   `int`, and negative means "wait forever". A bounded wait must never
///   quietly become unbounded, so it clamps -- to about 24 days, which is
///   indistinguishable from forever for anyone who meant it.
/// - **Negative input.** `-5s` would read as "wait forever" too. It clamps to
///   zero: whatever a caller passing a negative duration meant, it was not
///   "block this thread until the process is killed".
///
/// The `!(x > 0)` spellings are deliberate -- they take the safe branch for a
/// NaN count, which a `duration<double>` can carry.
template <class Rep, class Period>
[[nodiscard]] constexpr int to_timeout_ms(std::chrono::duration<Rep, Period> d) noexcept {
    constexpr int max_ms = (std::numeric_limits<int>::max)();

    if (!(d.count() > 0)) return 0;

    // Bound it in a floating representation first. This is what keeps the
    // integer conversion below from ever running on a value large enough to
    // overflow it, rather than relying on it to notice afterwards.
    using ms_f = std::chrono::duration<long double, std::milli>;
    const long double as_ms = std::chrono::duration_cast<ms_f>(d).count();
    if (!(as_ms < static_cast<long double>(max_ms))) return max_ms;

    using ms_ll = std::chrono::duration<long long, std::milli>;
    const long long m = std::chrono::ceil<ms_ll>(d).count();
    return m >= static_cast<long long>(max_ms) ? max_ms : static_cast<int>(m);
}

} // namespace detail

/// A value, a timeout, or the end of the source.
///
/// Owning and move-only in practice, because every `T` used with it is.
template <class T>
class poll {
public:
    using value_type = T;

    /// Defaults to `ended` -- the state that stops a loop.
    ///
    /// Not `timed_out`, and not a valueless `ready`. If a default-constructed
    /// poll is ever read by mistake, terminating the read is the recoverable
    /// outcome; spinning forever on a source that was never opened is not.
    poll() noexcept = default;

    /// A poll carrying a value.
    explicit poll(T v) : state_(poll_state::ready), value_(std::move(v)) {}

    /// A poll carrying no value. `ready` is rejected: it would have nothing to
    /// hand back, and every accessor would then assert at the call site rather
    /// than here where the mistake was made.
    explicit poll(poll_state s) noexcept : state_(s) {
        SURREALDB_ASSERT(s != poll_state::ready,
            "poll(poll_state::ready) carries no value; construct from a T instead");
    }

    poll(poll&&) = default;
    poll& operator=(poll&&) = default;
    poll(const poll&) = delete;
    poll& operator=(const poll&) = delete;

    [[nodiscard]] poll_state state() const noexcept { return state_; }

    [[nodiscard]] bool ready() const noexcept { return state_ == poll_state::ready; }
    [[nodiscard]] bool timed_out() const noexcept { return state_ == poll_state::timed_out; }
    [[nodiscard]] bool ended() const noexcept { return state_ == poll_state::ended; }

    /// True only for `ready`. A timeout is not a success to be unwrapped.
    [[nodiscard]] explicit operator bool() const noexcept { return ready(); }

    [[nodiscard]] T& value() & noexcept {
        SURREALDB_ASSERT(ready(), "poll::value() on a poll that is not ready");
        return *value_;
    }
    [[nodiscard]] const T& value() const& noexcept {
        SURREALDB_ASSERT(ready(), "poll::value() on a poll that is not ready");
        return *value_;
    }

    /// Moving out of a temporary is the lifetime bug this library keeps
    /// finding, so it is not available. `owned_byte_array` is a range, and
    ///
    ///     for (auto b : *rs.try_next().value()) { }
    ///
    /// binds the loop to bytes owned by a poll that died at the semicolon --
    /// the exact shape that put a heap-use-after-free in `query().single()` and
    /// a dangling range-for in the README. Name the poll, then `take()`.
    T&& value() &&
        SURREALDB_DELETED("value() on a poll temporary hands back a reference "
                          "into storage that dies at the semicolon. Name the "
                          "poll and call take().");
    const T&& value() const&&
        SURREALDB_DELETED("value() on a poll temporary hands back a reference "
                          "into storage that dies at the semicolon. Name the "
                          "poll and call take().");

    [[nodiscard]] T& operator*() & noexcept { return value(); }
    [[nodiscard]] const T& operator*() const& noexcept { return value(); }
    T&& operator*() &&
        SURREALDB_DELETED("operator* on a poll temporary hands back a reference "
                          "into storage that dies at the semicolon. Name the "
                          "poll and call take().");

    [[nodiscard]] T* operator->() noexcept { return &value(); }
    [[nodiscard]] const T* operator->() const noexcept { return &value(); }

    /// Move the value out, leaving the poll `ended`.
    ///
    /// The sanctioned way to get ownership: it consumes the poll's claim on the
    /// value rather than handing out a reference that outlives it, so there is
    /// nothing left to dangle. Returns `nullopt` unless the poll was `ready`.
    [[nodiscard]] std::optional<T> take() noexcept {
        if (!ready()) return std::nullopt;
        std::optional<T> out(std::move(*value_));
        value_.reset();
        state_ = poll_state::ended;
        return out;
    }

private:
    poll_state state_{poll_state::ended};
    std::optional<T> value_;
};

} // namespace surrealdb
