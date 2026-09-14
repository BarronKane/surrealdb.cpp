#pragma once

/// @file rpc.hpp
/// The RPC context: sessions, and request/response in CBOR.
///
/// This is the other way into the database. `connection` speaks the typed
/// verbs; an RPC context speaks the wire protocol, which is what a server
/// proxying browser or WebSocket clients actually needs -- it can forward a
/// client's encoded request untouched and hand back the encoded reply.
///
/// **Payloads are CBOR, and this library does not encode or decode it.** Doing
/// so would mean either vendoring a codec or taking a dependency, and neither
/// belongs in a wrapper whose point is that it pulls in nothing. So requests go
/// in as bytes and replies come out as bytes, and the choice of codec stays
/// yours. What this file does provide is the part that is genuinely easy to get
/// wrong: the lifetimes, and the session model.
///
/// ### Sessions
///
/// SurrealDB 3.1 removed the implicit "current session" (GHSA-4vgr-h27g-cf9p);
/// every request now names one. A context mints a session for itself at
/// construction, and `execute()` uses it, so the simple case reads the same as
/// before. `attach()` makes more, and `execute_on()` picks between them --
/// which is how one context serves many clients without leaking authentication
/// state from one into another.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/invoke.hpp"
#include "detail/owned.hpp"
#include "error.hpp"
#include "options.hpp"
#include "value.hpp"

#include <cstdint>
#include <cstring>
#include <iterator>

#if SURREALDB_HAS_RANGES
#  include <ranges>
#endif
#include <memory>
#include <string>
#include <utility>

namespace surrealdb {

// ---------------------------------------------------------------------------
// session_id
// ---------------------------------------------------------------------------

/// A session identifier: sixteen owned bytes.
///
/// Owned rather than a `uuid_ref` because these outlive the call that produced
/// them -- a proxy holds one per connected client, long after the array
/// `sessions()` returned has been freed.
class session_id {
public:
    /// The all-zero id. Handing this to `attach()` asks the server to generate
    /// one and write it back, which is the usual way to make a session.
    session_id() noexcept : raw_{} {}

    explicit session_id(const sr_uuid_t& u) noexcept : raw_(u) {}
    explicit session_id(const std::uint8_t (&bytes)[16]) noexcept : raw_{} {
        std::memcpy(raw_._0, bytes, 16);
    }

    [[nodiscard]] const sr_uuid_t* raw() const noexcept { return &raw_; }
    [[nodiscard]] sr_uuid_t* raw() noexcept { return &raw_; }

    [[nodiscard]] const std::uint8_t* bytes() const noexcept { return raw_._0; }

    /// True for the all-zero id, which names no session.
    [[nodiscard]] bool empty() const noexcept {
        for (int i = 0; i < 16; ++i)
            if (raw_._0[i] != 0) return false;
        return true;
    }

    /// Canonical 8-4-4-4-12 hyphenated form.
    [[nodiscard]] std::string to_string() const {
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(36);
        for (int i = 0; i < 16; ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
            out.push_back(hex[(raw_._0[i] >> 4) & 0xF]);
            out.push_back(hex[raw_._0[i] & 0xF]);
        }
        return out;
    }

    [[nodiscard]] friend bool operator==(const session_id& a, const session_id& b) noexcept {
        return std::memcmp(a.raw_._0, b.raw_._0, 16) == 0;
    }
    [[nodiscard]] friend bool operator!=(const session_id& a, const session_id& b) noexcept {
        return !(a == b);
    }

private:
    sr_uuid_t raw_;
};

static_assert(sizeof(session_id) == 16, "session_id must be exactly its uuid");

/// The result of `rpc::sessions()`: an owned array of ids that iterates as
/// `session_id` rather than as the C `sr_uuid_t`.
///
/// It would be less code to hand back the raw span, and it would also hand the
/// caller a type with no comparison, no formatting, and a destructor they have
/// to remember. This owns the array and yields the C++ type by value.
class session_list
#if SURREALDB_HAS_RANGES
    : public std::ranges::view_interface<session_list>
#endif
{
public:
    session_list() noexcept = default;
    explicit session_list(owned_uuids ids) noexcept : ids_(std::move(ids)) {}

    session_list(session_list&&) noexcept = default;
    session_list& operator=(session_list&&) noexcept = default;
    session_list(const session_list&) = delete;
    session_list& operator=(const session_list&) = delete;

    [[nodiscard]] int size() const noexcept { return ids_.size(); }
    [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }

    [[nodiscard]] session_id operator[](int i) const noexcept {
        return session_id(ids_.data()[i]);
    }

    [[nodiscard]] bool contains(const session_id& id) const noexcept {
        for (int i = 0; i < size(); ++i)
            if ((*this)[i] == id) return true;
        return false;
    }

    /// A by-value input iterator: the ids are sixteen bytes each, so there is
    /// nothing to gain from handing out references into the array.
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = session_id;
        using difference_type   = std::ptrdiff_t;
        using reference         = session_id;
        using pointer           = const session_id*;

        iterator() noexcept = default;
        iterator(const sr_uuid_t* p) noexcept : p_(p) {}

        [[nodiscard]] session_id operator*() const noexcept { return session_id(*p_); }

        iterator& operator++() noexcept { ++p_; return *this; }
        iterator operator++(int) noexcept { iterator t = *this; ++p_; return t; }

        [[nodiscard]] friend bool operator==(iterator a, iterator b) noexcept {
            return a.p_ == b.p_;
        }
        [[nodiscard]] friend bool operator!=(iterator a, iterator b) noexcept {
            return a.p_ != b.p_;
        }

    private:
        const sr_uuid_t* p_{nullptr};
    };

    using const_iterator = iterator;

    [[nodiscard]] iterator begin() const noexcept { return iterator(ids_.data()); }
    [[nodiscard]] iterator end() const noexcept {
        return iterator(ids_.data() + ids_.size());
    }

private:
    owned_uuids ids_;
};

// ---------------------------------------------------------------------------
// rpc_stream
// ---------------------------------------------------------------------------

/// Live-query notifications from an RPC context, each a CBOR object carrying
/// `id`, `action` and `result`.
///
/// Shaped like `stream` deliberately -- same blocking contract, same iteration
/// caveat -- but it yields encoded bytes rather than a typed notification,
/// because that is what the RPC path produces.
class rpc_stream {
public:
    rpc_stream() noexcept = default;

    rpc_stream(rpc_stream&&) noexcept = default;
    rpc_stream& operator=(rpc_stream&&) noexcept = default;
    rpc_stream(const rpc_stream&) = delete;
    rpc_stream& operator=(const rpc_stream&) = delete;

    /// Leaks the handle rather than freeing it when the context is already
    /// gone, for the same reason `stream` does: freeing runs on the context's
    /// runtime, and a leak is preferable to a use-after-free.
    ~rpc_stream() {
        if (handle_ && alive_.expired()) (void)handle_.release();
    }

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(handle_) && !alive_.expired();
    }
    [[nodiscard]] bool done() const noexcept { return done_; }

    /// Block until the next notification.
    ///
    /// An empty (but successful) result means the stream closed cleanly, which
    /// is what disconnecting the context produces. There is no timeout and no
    /// cancellation: the only way to release a parked reader is to disconnect.
    [[nodiscard]] result<owned_byte_array> next() noexcept {
        if (done_ || !handle_) return owned_byte_array();

        std::uint8_t* out = nullptr;
        const int rc = ::sr_rpc_stream_next(handle_.get(), &out);

        if (rc > 0) return owned_byte_array(out, rc);

        done_ = true;
        // SR_CLOSED is the ordinary end of a stream, not a failure -- it is
        // what a caller gets for shutting the context down on purpose.
        if (rc == SR_CLOSED || rc == SR_NONE) return owned_byte_array();

        return error(static_cast<error_code>(rc), owned_string());
    }

    /// Close early. Safe to call more than once.
    void close() noexcept {
        done_ = true;
        if (handle_ && alive_.expired()) { (void)handle_.release(); return; }
        handle_.reset();
    }

    // -- iteration ----------------------------------------------------------
    //
    // The same warning as `stream`: `++it` runs before the condition is
    // re-tested, so a loop bounded by a counter in its condition blocks on an
    // event that may never arrive. Break from the body, or drive `next()`.

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = owned_byte_array;
        using difference_type   = std::ptrdiff_t;
        using reference         = owned_byte_array&;
        using pointer           = owned_byte_array*;

        iterator() noexcept = default;
        explicit iterator(rpc_stream* s) noexcept : s_(s) { advance(); }

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
            if (!r.has_value() || r.value().empty()) { s_ = nullptr; return; }
            current_ = std::move(r).value();
        }

        rpc_stream* s_{nullptr};
        owned_byte_array current_;
    };

    using const_iterator = iterator;

    [[nodiscard]] iterator begin() noexcept { return iterator(this); }
    [[nodiscard]] iterator end() noexcept { return iterator(); }

private:
    friend class rpc;
    rpc_stream(sr_RpcStream* raw, std::weak_ptr<const void> alive) noexcept
        : handle_(raw), alive_(std::move(alive)) {}

    owned_rpc_stream handle_;
    std::weak_ptr<const void> alive_;
    bool done_{false};
};

// ---------------------------------------------------------------------------
// rpc
// ---------------------------------------------------------------------------

/// An RPC context.
///
/// Move-only, like `connection`, and poisoned by the same rule: once any call
/// returns `SR_FATAL` the context is unusable and every later call fails
/// without touching the C library.
class rpc {
public:
    rpc() noexcept = default;

    rpc(rpc&&) noexcept = default;
    rpc& operator=(rpc&&) noexcept = default;
    rpc(const rpc&) = delete;
    rpc& operator=(const rpc&) = delete;

    [[nodiscard]] static result<rpc> connect(const char* endpoint) {
        return connect(endpoint, options{});
    }

    [[nodiscard]] static result<rpc> connect(const char* endpoint, const options& opts) {
        // The C view borrows from `view`, so it has to outlive the call.
        options::c_view view = opts.to_c();
        sr_surreal_rpc_t* raw = nullptr;
        return detail::invoke<rpc>(
            [&](sr_string_t* e) {
                return ::sr_surreal_rpc_new(e, &raw, endpoint, view.opts);
            },
            [&](int) { return rpc(raw); });
    }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }
    [[nodiscard]] bool poisoned() const noexcept { return poisoned_; }
    [[nodiscard]] const sr_surreal_rpc_t* raw() const noexcept { return handle_.get(); }

    // -- requests ------------------------------------------------------------

    /// Send an encoded request on this context's own session.
    ///
    /// `request` is CBOR; so is the reply. Nothing here inspects either.
    ///
    /// **A successful result does not mean the query succeeded.** There are two
    /// error channels and they mean different things:
    ///
    /// - a *parse* error fails this call, with the server's diagnostic as the
    ///   error message. Nothing ran.
    /// - a *runtime* error succeeds here and is reported inside the reply. A
    ///   `query` reply is an array with one entry per statement, each carrying
    ///   `status` (`"OK"` or `"ERR"`), `result`, `time` and `type`; a failing
    ///   statement does not stop the ones after it, so `RETURN 1; THROW 'x';
    ///   RETURN 3` comes back as OK, ERR, OK.
    ///
    /// That is the same split every other SurrealDB client sees, which is the
    /// point -- a proxy can forward the reply verbatim. But a caller who only
    /// checks `has_value()` will read a failed statement as a success.
    [[nodiscard]] result<owned_byte_array> execute(const std::uint8_t* request, int len) noexcept {
        std::uint8_t* out = nullptr;
        return track(detail::invoke<owned_byte_array>(
            [&](sr_string_t* e) {
                return ::sr_surreal_rpc_execute(raw(), e, &out, request, len);
            },
            [&](int n) { return owned_byte_array(out, n); }));
    }

    template <class Container>
    [[nodiscard]] result<owned_byte_array> execute(const Container& request) noexcept {
        return execute(request.data(), static_cast<int>(request.size()));
    }

    /// Send an encoded request on a named session.
    ///
    /// This is the call a proxy wants: one context, one session per connected
    /// client, and no authentication state shared between them.
    [[nodiscard]] result<owned_byte_array> execute_on(const session_id& session,
                                                      const std::uint8_t* request,
                                                      int len) noexcept {
        std::uint8_t* out = nullptr;
        return track(detail::invoke<owned_byte_array>(
            [&](sr_string_t* e) {
                return ::sr_rpc_execute_on(raw(), e, &out, session.raw(), request, len);
            },
            [&](int n) { return owned_byte_array(out, n); }));
    }

    template <class Container>
    [[nodiscard]] result<owned_byte_array> execute_on(const session_id& session,
                                                      const Container& request) noexcept {
        return execute_on(session, request.data(), static_cast<int>(request.size()));
    }

    // -- sessions ------------------------------------------------------------

    /// Register a new session, letting the server pick the id.
    [[nodiscard]] result<session_id> attach() noexcept { return attach(session_id{}); }

    /// Register a session under an id you choose.
    ///
    /// Pass the default-constructed (all-zero) id to have one generated
    /// instead; that is what the no-argument overload does. Attaching an id
    /// that already exists is an error rather than a no-op.
    [[nodiscard]] result<session_id> attach(session_id wanted) noexcept {
        auto r = track(detail::invoke([&](sr_string_t* e) {
            return ::sr_rpc_session_attach(raw(), e, wanted.raw());
        }));
        if (!r) return std::move(r).error();
        return wanted;   // filled in place when it went in zeroed
    }

    /// Close a session, cancelling the live queries and transactions it owns.
    [[nodiscard]] result<void> detach(const session_id& session) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_rpc_session_detach(raw(), e, session.raw());
        }));
    }

    /// Return a session to its initial state without closing it -- the cheap
    /// way to recycle one between clients, since the id stays valid.
    [[nodiscard]] result<void> reset(const session_id& session) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_rpc_session_reset(raw(), e, session.raw());
        }));
    }

    /// Every active session id.
    [[nodiscard]] result<session_list> sessions() noexcept {
        sr_uuid_t* out = nullptr;
        return track(detail::invoke<session_list>(
            [&](sr_string_t* e) { return ::sr_rpc_session_list(raw(), e, &out); },
            [&](int n) { return session_list(owned_uuids(out, n)); }));
    }

    /// The session this context minted for itself, which `execute()` uses.
    [[nodiscard]] result<session_id> default_session() noexcept {
        session_id id;
        auto r = track(detail::invoke([&](sr_string_t* e) {
            return ::sr_rpc_session_default(raw(), e, id.raw());
        }));
        if (!r) return std::move(r).error();
        return id;
    }

    // -- notifications -------------------------------------------------------

    /// Open the notification channel. One per context.
    [[nodiscard]] result<rpc_stream> notifications() noexcept {
        sr_RpcStream* raw_stream = nullptr;
        auto r = track(detail::invoke([&](sr_string_t* e) {
            return ::sr_surreal_rpc_notifications(raw(), e, &raw_stream);
        }));
        if (!r) return std::move(r).error();
        return rpc_stream(raw_stream, std::weak_ptr<const void>(alive_));
    }

    /// Disconnect early.
    ///
    /// This also closes the notification channel, which releases any thread
    /// parked in `rpc_stream::next()`. It is the only way to release one.
    void disconnect() noexcept {
        handle_.reset();
        alive_.reset();
    }

private:
    explicit rpc(sr_surreal_rpc_t* raw)
        : handle_(raw), alive_(std::make_shared<const char>('\0')) {}

    /// Latch the poison flag, and refuse to make any further call once set.
    template <class T>
    result<T> track(result<T> r) noexcept {
        if (!r && r.error().code() == error_code::fatal) poisoned_ = true;
        return r;
    }

    owned_rpc handle_;
    /// Liveness token handed to streams as a weak reference, so a stream that
    /// outlives this context can tell.
    std::shared_ptr<const void> alive_;
    bool poisoned_{false};
};

} // namespace surrealdb

#if SURREALDB_HAS_RANGES
// Deliberately *not* `enable_borrowed_range`: this owns its array, so an
// iterator outliving it really is dangling and the ranges dangling check is
// telling the truth. The geometry spans opt in because they only ever point
// into a value somebody else owns.
#endif
