#pragma once

/// @file stream.hpp
/// Live query notifications.
///
/// Four properties of the C stream API shape everything here, and every one of
/// them is easy to get wrong:
///
/// **Every wait is bounded, and that is a choice this library is still making.**
/// A killed live query cannot be observed to end on the embedded path against
/// SurrealDB 3.2.4, so a reader parked with no deadline has no exit: nothing
/// further arrives, `SR_CLOSED` never comes, and `sr_stream_kill` frees the very
/// stream that reader is borrowing, so no other thread can release it. Only
/// ending the process would.
///
/// surrealdb.c withdrew `sr_stream_next` in 0.3.1 and **restored it in 0.3.2**,
/// ahead of the upstream fix, documenting it as the wrong call to reach for
/// until that fix ships. This library keeps it `= delete`d rather than
/// following, because the hazard has not changed: the symbol is back, the
/// deadlock is not fixed, and a compile error that explains itself is better
/// than a hang. `surrealdb::has_unbounded_stream_read` is the flag to watch;
/// it flips when the floor moves past the fix, and `next()` comes back with it.
///
/// The fix is [surrealdb/surrealdb#7520]. Until then `next_for()` and
/// `try_next()` are the whole reading surface here. A long bound costs nothing
/// -- the wait is a real timer, not a poll loop -- so an hour's bound is an
/// hour's block that still returns.
///
/// [surrealdb/surrealdb#7520]: https://github.com/surrealdb/surrealdb/pull/7520
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
/// anything else a failure -- and `== 0` (`SR_AGAIN`) means "nothing yet, the
/// stream is still open".
///
/// This changed in surrealdb.c 0.3.0 and changed *silently*: the end of a
/// stream used to be `SR_AGAIN` and is now `SR_CLOSED`, while `SR_AGAIN` went the
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
        // Retire the subscription and release the reader, so scope exit is the
        // correct teardown rather than half of it.
        if (handle_ && !alive_.expired()) { close(); return; }

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

    /// Block until the next notification.
    ///
    /// `nullopt` means the stream ended cleanly. An error result means it
    /// failed; either way the stream is finished afterwards.
    ///
    /// An unbounded wait cannot time out, so two states suffice here and the
    /// `optional` stays. `next_for()` is the one that needs a third.
    ///
    /// **This was withheld for two releases and is back.** A killed live query
    /// used to leave the stream open and silent, so a reader parked here could
    /// not be released by anything short of ending the process -- nothing
    /// arrived, `SR_CLOSED` never came, and `sr_stream_kill` frees the very
    /// stream the reader is borrowing. `KILL` and `REMOVE TABLE` now end the
    /// stream, so this returns. See `has_unbounded_stream_read`, which is what
    /// re-opened it.
    ///
    /// It still cannot wake for anything *other* than the stream. A reader that
    /// must also notice a shutdown flag wants `next_for()`.
    [[nodiscard]] result<std::optional<notification>> next() noexcept {
        if (done_ || !handle_) return std::optional<notification>{};

        sr_notification_t raw{};
        const int rc = ::sr_stream_next(handle_.get(), &raw);

        if (rc > 0) {
            remember_id(raw);
            return std::optional<notification>(notification(raw));
        }

        // Everything below finishes the stream.
        done_ = true;

        // `SR_CLOSED` is the clean end. `SR_AGAIN` cannot come back from a
        // blocking read -- it has nothing to report until it has something --
        // but it is folded in rather than made a failure, because the only
        // `error` it could produce would carry `error_code::ok`, and an error
        // that reports itself as ok is worse than a stream that stops.
        if (rc == SR_CLOSED || rc == SR_AGAIN) return std::optional<notification>{};

        failed_ = true;
        failure_ = error(static_cast<error_code>(rc), owned_string());
        return error(static_cast<error_code>(rc), owned_string());
    }

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

    /// Retire the live query and release the stream. Safe to call more than
    /// once, and called by the destructor.
    ///
    /// One call does both: `sr_stream_kill` retires the subscription in the
    /// datastore *and* frees the local reader, and it needs no live query id,
    /// which matters because `db.live()` never hands one back.
    ///
    /// That was not always true. Until the anchored dependency picked up the
    /// live-query teardown fixes, freeing the reader left the subscription
    /// registered -- `INFO FOR TABLE`'s `lives` count stayed up -- so tearing
    /// one down took this *and* `connection::kill()`. This wrapper papered over
    /// that by issuing the kill itself from a cached id, which also meant a
    /// stream that had never yielded a notification could not be retired at
    /// all. Neither is needed now; both are gone.
    ///
    /// `connection::kill()` covers the other case: a subscription you have an
    /// id for and no stream, such as a bare `LIVE SELECT` run through
    /// `query()`. Calling both is harmless, just unnecessary.
    void close() noexcept {
        done_ = true;
        if (handle_ && alive_.expired()) {
            (void)handle_.release();   // see the destructor
            return;
        }
        handle_.reset();
    }

    /// The live query's id, once one has been learned.
    ///
    /// Empty until the first notification arrives, because that is the only
    /// place the C exposes it. A stream that is still empty here cannot be
    /// retired by `close()` -- see its note.
    [[nodiscard]] std::optional<uuid_ref> query_id() const noexcept {
        if (!have_id_) return std::nullopt;
        return uuid_ref{query_id_};
    }

    // -- iteration ----------------------------------------------------------
    //
    // Back with `next()`, and with the same caveat it always had:
    //
    // **A range-for over a live stream blocks past the last event you know
    // about.** `for (...; it != end(); ++it)` runs the increment *before*
    // re-testing the condition, so a loop bounded by a counter in its condition
    // parks on an event that never comes. A killed query now ends the stream,
    // so that is a wait rather than the permanent hang it used to be -- but a
    // healthy quiet stream still parks there indefinitely.
    //
    // Either break from inside the body:
    //
    //     for (auto it = s.begin(); it != s.end(); ++it) {
    //         handle(*it);
    //         if (enough()) break;
    //     }
    //
    // or drive `next_for()`, which is the honest primitive for a consumer that
    // decides when to stop. The range is for the worker-thread case that
    // genuinely wants every event until the stream ends.

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = notification;
        using difference_type   = std::ptrdiff_t;
        using reference         = notification&;
        using pointer           = notification*;

        iterator() noexcept = default;
        explicit iterator(stream* s) noexcept : s_(s) { advance(); }

        [[nodiscard]] reference operator*() noexcept { return current_; }
        [[nodiscard]] pointer operator->() noexcept { return &current_; }

        iterator& operator++() noexcept { advance(); return *this; }
        void operator++(int) noexcept { advance(); }

        [[nodiscard]] friend bool operator==(const iterator& a, const iterator& b) noexcept {
            return a.s_ == b.s_;
        }
        [[nodiscard]] friend bool operator!=(const iterator& a, const iterator& b) noexcept {
            return !(a == b);
        }

    private:
        void advance() noexcept {
            if (!s_) return;
            auto r = s_->next();
            if (!r.has_value() || !r.value().has_value()) {
                s_ = nullptr;          // becomes the end iterator
                return;
            }
            current_ = std::move(r).value().value();
        }

        stream* s_{nullptr};
        notification current_;
    };

    using const_iterator = iterator;

    /// Begins by blocking for the first notification.
    [[nodiscard]] iterator begin() noexcept { return iterator(this); }
    [[nodiscard]] iterator end() noexcept { return iterator(); }

private:
    /// Cache the live query's id off a notification.
    ///
    /// The only place the C ever exposes it -- `sr_select_live` hands back a
    /// stream and no id -- and copied rather than borrowed, because the
    /// notification it came from is about to be handed away. `close()` no
    /// longer needs it, but `query_id()` still reports it and a caller may want
    /// it for `connection::kill()`.
    void remember_id(const sr_notification_t& n) noexcept {
        if (have_id_) return;
        for (int i = 0; i < 16; ++i) query_id_[i] = n.query_id._0[i];
        have_id_ = true;
    }

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

        if (rc > 0) {
            remember_id(raw);
            return poll<notification>(notification(raw));
        }

        // The one branch that must not fall through to the shared "finished"
        // handling below: the stream is still live and the caller should come
        // back. Marking it done here would abandon a stream over nothing more
        // than a quiet hundred milliseconds.
        //
        // This is `SR_AGAIN` as of 0.3.0, where it used to be `SR_TIMEOUT` and
        // `SR_AGAIN` meant the opposite. Getting it wrong either abandons a
        // healthy stream or spins on a dead one, and neither announces itself.
        if (rc == SR_AGAIN) return poll<notification>(poll_state::timed_out);

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

    /// Copied out of the first notification; see `query_id()`.
    std::uint8_t query_id_[16]{};
    bool have_id_{false};
    error failure_;
    bool done_{false};
    bool failed_{false};
};

} // namespace surrealdb
