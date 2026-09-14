#pragma once

/// @file stream.hpp
/// Live query notifications.
///
/// Three properties of the C stream API shape everything here, and all three
/// are easy to get wrong:
///
/// **It blocks, with no timeout and no cancellation.** `sr_stream_next` parks
/// until an event arrives. There is no way to release a parked reader from
/// another thread -- `sr_stream_kill` frees the very stream that reader is
/// borrowing. So this is driven from a dedicated worker thread, and only when
/// an event is expected -- never from a thread that has to stay responsive.
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
/// **`sr_stream_next` does not use SR_CLOSED.** It returns 1 with a
/// notification, `SR_NONE` (0) at end of stream, and a negative status on
/// error. Treating 0 as success is the mistake the C suite's own test was
/// written to catch.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/invoke.hpp"
#include "detail/owned.hpp"
#include "error.hpp"
#include "value.hpp"

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

    [[nodiscard]] surrealdb::action action() const noexcept {
        return static_cast<surrealdb::action>(n_.get().action);
    }

    /// The live query this came from.
    [[nodiscard]] uuid_ref query_id() const noexcept {
        return uuid_ref{n_.get().query_id._0};
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

    /// Block until the next notification.
    ///
    /// `nullopt` means the stream ended cleanly. An error result means it
    /// failed; either way the stream is finished afterwards.
    [[nodiscard]] result<std::optional<notification>> next() noexcept {
        if (done_ || !handle_) return std::optional<notification>{};

        sr_notification_t raw{};
        const int rc = ::sr_stream_next(handle_.get(), &raw);

        if (rc > 0) return std::optional<notification>(notification(raw));

        // SR_NONE here is end-of-stream, not success. Anything else is a
        // failure. Both finish the stream.
        done_ = true;
        if (rc == 0) return std::optional<notification>{};

        failed_ = true;
        failure_ = error(static_cast<error_code>(rc), owned_string());
        return error(static_cast<error_code>(rc), owned_string());
    }

    /// Close the stream early. Safe to call more than once.
    void close() noexcept {
        done_ = true;
        if (handle_ && alive_.expired()) {
            (void)handle_.release();   // see the destructor
            return;
        }
        handle_.reset();
    }

    // -- iteration ----------------------------------------------------------
    //
    // **A range-for over a live stream blocks past the last event you know
    // about.** `for (...; it != end(); ++it)` runs the increment *before*
    // re-testing the condition, so a loop bounded by a counter in its condition
    // parks on an event that never comes. Since there is no cancellation, that
    // is a hang, not a delay.
    //
    // Either break from inside the body:
    //
    //     for (auto it = s.begin(); it != s.end(); ++it) {
    //         handle(*it);
    //         if (enough()) break;
    //     }
    //
    // or drive it with next(), which is the honest primitive for a consumer
    // that decides when to stop. The range is for the worker-thread case that
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
