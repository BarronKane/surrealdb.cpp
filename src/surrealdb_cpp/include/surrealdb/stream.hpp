#pragma once

/// @file stream.hpp
/// Live query notifications.
///
/// Four properties of the C stream API shape everything here, and every one of
/// them is easy to get wrong:
///
/// **Every wait is bounded, because the unbounded one was withdrawn.**
/// surrealdb.c 0.3.1 removed `sr_stream_next` outright. A killed live query
/// cannot be observed to end on this path -- an upstream core defect, not
/// something either library can paper over -- so a reader parked with no
/// deadline had no exit: nothing further arrives, `SR_CLOSED` never comes, and
/// `sr_stream_kill` frees the very stream that reader is borrowing, so no other
/// thread can release it. Only ending the process would.
///
/// So `next_for()` and `try_next()` are the whole reading surface here. A long
/// bound costs nothing -- the wait is a real timer, not a poll loop -- so an
/// hour's bound is an hour's block that still returns. `next()` and iteration
/// are `= delete`d with reasons rather than removed, so 0.2.x code is told what
/// to do instead of failing as a missing member. Both come back if upstream
/// lands the fix; the C keeps its implementation commented in place for that.
///
/// A bounded wait has three outcomes rather than two -- value, timeout, end --
/// so these return `result<poll<notification>>`; see poll.hpp for why the last
/// two must not be merged.
///
/// **`close()` retires a live query; `connection::kill()` strands it.**
/// `sr_stream_kill` stops the underlying query by a route the defect does not
/// touch, and releases the stream in the same step. `sr_kill` stops delivery
/// but leaves the stream open forever. So closing the stream is the way to end
/// a live query you own.
///
/// **Teardown is ordered.** A stream borrows the runtime owned by its
/// connection, so the stream must be killed *before* the connection is
/// disconnected. Disconnecting first drops the runtime out from under the
/// stream and the kill then runs against a runtime that is already shut down.
/// C++ destruction order makes the natural spelling correct -- a stream
/// declared after its connection dies first -- but a stream that is moved
/// somewhere longer-lived breaks it silently. `stream` therefore holds a
/// liveness token and refuses to kill a stream whose connection is already
/// gone: leaking one stream is a great deal better than running a kill against
/// a dead runtime.
///
/// **The status codes split on the sign, and 0 does not mean success.**
/// `> 0` is a notification, `< 0` says stop -- `SR_CLOSED` for a clean end,
/// anything else a failure -- and `== 0` (`SR_NONE`) means "nothing yet, the
/// stream is still open".
///
/// This changed in surrealdb.c 0.3.0 and changed *silently*: the end of a
/// stream used to be `SR_NONE` and is now `SR_CLOSED`, while `SR_NONE` went the
/// other way and now means not-yet. Both misreadings look plausible at run
/// time, which is why this file works from the sign rather than from a list of
/// codes. A negative `timeout_ms` is also no longer "wait forever" -- 0.3.1
/// rejects it with `SR_ERROR` -- which makes the clamping in
/// `detail::to_timeout_ms` load-bearing rather than merely defensive.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/invoke.hpp"
#include "detail/owned.hpp"
#include "error.hpp"
#include "poll.hpp"
#include "value.hpp"

#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>

namespace surrealdb {

/// What happened to the record a notification is about.
enum class action : int {
    create = SR_ACTION_CREATE,
    update = SR_ACTION_UPDATE,
    remove = SR_ACTION_DELETE,
    killed = SR_ACTION_KILLED,
    /// The live query itself failed. Distinct from `killed`, which is an
    /// ordinary termination.
    failed = SR_ACTION_ERROR,
};

[[nodiscard]] constexpr const char* to_string(action a) noexcept {
    switch (a) {
        case action::create: return "create";
        case action::update: return "update";
        case action::remove: return "delete";
        case action::killed: return "killed";
        case action::failed: return "error";
    }
    return "unknown";
}

/// One live-query notification, owning its payload.
class notification {
public:
    notification() noexcept = default;
    explicit notification(sr_notification_t raw) noexcept : n_(raw) {}

    // Both of these go through `addr()`, not `get()`.
    //
    // `owned::get()` returns the handle **by value**, and this handle is a
    // struct: `n_.get().query_id._0` takes a pointer into a temporary that
    // dies at the end of the full-expression, and `uuid_ref` is nothing but
    // that pointer. It read a dead stack frame -- ids that changed between
    // calls and carried recognisable fragments of x86-64 pointers where the
    // UUID version nibble should be. `action()` was safe only by accident,
    // because it copies an int out before the temporary dies; using `addr()`
    // for both means the next member added here cannot pick the wrong one.
    [[nodiscard]] surrealdb::action action() const noexcept {
        return static_cast<surrealdb::action>(n_.addr()->action);
    }

    /// The live query this came from.
    ///
    /// Borrows from this notification, like `data()`, so it must not outlive it.
    [[nodiscard]] uuid_ref query_id() const noexcept {
        return uuid_ref{n_.addr()->query_id._0};
    }

    /// The record. Borrows from this notification, so it must not outlive it.
    [[nodiscard]] surrealdb::value data() const noexcept {
        return surrealdb::value(&n_.addr()->data);
    }

    [[nodiscard]] const sr_notification_t* raw() const noexcept { return n_.addr(); }

private:
    owned_notification n_;
};

/// A live query.
///
/// Single-pass and move-only. Iterating blocks; see the file comment.
class stream {
public:
    stream() noexcept = default;

    stream(stream&&) noexcept = default;
    stream& operator=(stream&&) noexcept = default;
    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    ~stream() {
        // If the connection is already gone its runtime went with it, and
        // sr_stream_kill would run against a shut-down runtime. Leak instead:
        // a leaked stream is recoverable, a use-after-free is not.
        if (handle_ && alive_.expired()) {
            SURREALDB_ASSERT(false,
                "surrealdb::stream outlived its connection; the stream was leaked "
                "rather than killed against a dead runtime. Destroy streams before "
                "the connection they came from.");
            (void)handle_.release();
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(handle_) && !alive_.expired();
    }

    /// True once the stream has ended, by exhaustion or by error.
    [[nodiscard]] bool done() const noexcept { return done_; }

    /// The failure that ended the stream, if it ended by failing.
    [[nodiscard]] const error* failure() const noexcept {
        return failed_ ? &failure_ : nullptr;
    }

    /// The unbounded read, removed in surrealdb.c 0.3.1.
    ///
    /// `sr_stream_next` is gone -- no declaration, no symbol -- and this is
    /// deleted rather than left to fail as a missing member so that code
    /// written against 0.2.x gets told what to do instead.
    ///
    /// The reason is not tidiness. A killed live query cannot be observed to
    /// end on this path (an upstream core defect, documented on `kill()`), so a
    /// reader parked here had no exit at all: nothing further arrives,
    /// `SR_CLOSED` never comes, and `sr_stream_kill` frees the very stream that
    /// reader is borrowing, so another thread cannot release it either. The
    /// process had to die. surrealdb.c withdrew the call rather than document
    /// that as a caveat and let callers walk into it.
    ///
    /// `next_for()` replaces it, and a long bound is cheap -- the wait is a
    /// real timer, so `next_for(std::chrono::hours(1))` costs what blocking for
    /// an hour would have cost and still returns.
    ///
    /// It comes back when the upstream fix lands; the C keeps its
    /// implementation commented in place for exactly that.
    result<std::optional<notification>> next()
        SURREALDB_DELETED("surrealdb.c 0.3.1 withdrew the unbounded stream read: "
                          "a killed live query never reports its end, so a parked "
                          "reader could not be released by anything. Use next_for() "
                          "with a bound, or try_next().");

    /// Wait for the next notification, giving up after `timeout`.
    ///
    /// Four outcomes, and a reader that loops has to handle all four:
    ///
    ///     for (;;) {
    ///         auto r = s.next_for(std::chrono::milliseconds(100));
    ///         if (!r) { report(r.error()); break; }   // stream failed
    ///
    ///         auto& p = r.value();
    ///         if (p.ended()) break;                   // stream finished
    ///         if (p.timed_out()) {                    // nothing yet
    ///             if (shutting_down) break;
    ///             continue;
    ///         }
    ///         handle(*p.take());
    ///     }
    ///
    /// **A timeout does not consume anything.** Notifications queue in a
    /// channel and an expired wait leaves any later arrival in place, so
    /// polling in a loop cannot lose an event.
    ///
    /// A timeout also leaves the stream open: unlike `next()`, returning here
    /// is not the end of anything, and `done()` stays false. Only an end or a
    /// failure finishes the stream.
    ///
    /// Durations are rounded up to whole milliseconds and clamped to about 24
    /// days; negative durations poll once rather than blocking forever. See
    /// `detail::to_timeout_ms`.
    template <class Rep, class Period>
    [[nodiscard]] result<poll<notification>> next_for(
            std::chrono::duration<Rep, Period> timeout) noexcept {
        return next_timeout_ms(detail::to_timeout_ms(timeout));
    }

    /// Take a notification if one is already waiting, otherwise report a
    /// timeout immediately. Never blocks.
    ///
    /// This is `next_for(0ms)`, spelled out because a non-blocking poll is a
    /// different intent from a short wait and reads badly as a magic zero.
    [[nodiscard]] result<poll<notification>> try_next() noexcept {
        return next_timeout_ms(0);
    }

    /// Close the stream early, and with it the live query. Safe to call more
    /// than once.
    ///
    /// This is the way to retire a live query you own: it stops the underlying
    /// query by a route the upstream KILL defect does not touch, and releases
    /// the stream in the same step. `connection::kill()` does not -- see its
    /// note.
    void close() noexcept {
        done_ = true;
        if (handle_ && alive_.expired()) {
            (void)handle_.release();   // see the destructor
            return;
        }
        handle_.reset();
    }

    // -- iteration, withdrawn with the blocking read -------------------------
    //
    // A range cannot express this stream any more, and the failure modes of
    // pretending otherwise are both bad.
    //
    // An iterator has exactly two answers: here is an element, or the range is
    // over. A bounded read has three, and the third -- "nothing yet, still
    // open" -- is the one with nowhere to go. Advancing on a timeout means
    // looping until something arrives, which is the unbounded park that
    // surrealdb.c just removed. Ending the range on a timeout means a quiet
    // moment silently truncates a live query, which is worse: it looks like
    // success.
    //
    // So iteration is deleted rather than quietly redefined. Drive `next_for()`
    // in a loop and decide for yourself what a run of timeouts means -- that
    // decision is the caller's and cannot be made here.
    //
    // `rpc_stream` keeps its iteration. The RPC path has no such defect: its
    // stream really does end when the context is destroyed, so `next()` there
    // still terminates and a range over it still means something.

    class iterator;   // not defined

    iterator begin()
        SURREALDB_DELETED("a live stream is no longer a range: a bounded read can "
                          "report 'nothing yet', which an iterator cannot express "
                          "without either parking forever or silently ending the "
                          "range. Loop on next_for() instead.");
    iterator end()
        SURREALDB_DELETED("a live stream is no longer a range: a bounded read can "
                          "report 'nothing yet', which an iterator cannot express "
                          "without either parking forever or silently ending the "
                          "range. Loop on next_for() instead.");

private:
    /// The shared body of `next_for` and `try_next`.
    ///
    /// Not public: `int` milliseconds is the C's spelling, where a negative
    /// value silently means "forever". Exposing that would put the one
    /// conversion mistake this library guards against back in the caller's
    /// hands, and `next()` already says "forever" without a sentinel.
    [[nodiscard]] result<poll<notification>> next_timeout_ms(int ms) noexcept {
        if (done_ || !handle_) return poll<notification>(poll_state::ended);

        sr_notification_t raw{};
        const int rc = ::sr_stream_next_timeout(handle_.get(), &raw, ms);

        if (rc > 0) return poll<notification>(notification(raw));

        // The one branch that must not fall through to the shared "finished"
        // handling below: the stream is still live and the caller should come
        // back. Marking it done here would abandon a stream over nothing more
        // than a quiet hundred milliseconds.
        //
        // This is `SR_NONE` as of 0.3.0, where it used to be `SR_TIMEOUT` and
        // `SR_NONE` meant the opposite. Getting it wrong either abandons a
        // healthy stream or spins on a dead one, and neither announces itself.
        if (rc == SR_NONE) return poll<notification>(poll_state::timed_out);

        done_ = true;
        if (rc == SR_CLOSED) return poll<notification>(poll_state::ended);

        failed_ = true;
        failure_ = error(static_cast<error_code>(rc), owned_string());
        return error(static_cast<error_code>(rc), owned_string());
    }

    friend class connection;
    stream(sr_stream_t* raw, std::weak_ptr<const void> alive) noexcept
        : handle_(raw), alive_(std::move(alive)) {}

    owned_stream handle_;
    std::weak_ptr<const void> alive_;
    error failure_;
    bool done_{false};
    bool failed_{false};
};

} // namespace surrealdb
