#pragma once

/// @file error.hpp
/// `error_code`, `error` and `result<T>` -- the spine of the library.
///
/// **`result<T>` is this library's own type at every supported standard and is
/// never an alias for `std::expected`.** Two of the three targeted standards
/// lack `std::expected` entirely, and conditionally aliasing it would make the
/// type's identity depend on the consumer's `-std` -- an ODR violation that
/// links cleanly and corrupts at runtime. On C++23 `result` *gains* `to_std()`
/// for interop; its identity never changes. See config.hpp for the rule.
///
/// Nothing here throws. The library targets `-fno-exceptions` builds.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/owned.hpp"

#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if SURREALDB_HAS_STD_EXPECTED
#  include <expected>
#endif

namespace surrealdb {

/// Status codes returned by the C API.
///
/// `fatal` poisons the connection: the C library documents that once any
/// operation on any thread returns `SR_FATAL` the handle must not be used
/// again. `connection` latches this.
enum class error_code : int {
    ok     = SR_NONE,    //  0
    closed = SR_CLOSED,  // -1
    error  = SR_ERROR,   // -2
    fatal  = SR_FATAL,   // -3
    /// A bounded wait expired. Mirrored here so the enum stays a complete map
    /// of the C status codes, but it is **not a failure** and never reaches an
    /// `error`: the `_timeout` readers translate it into `poll_state::timed_out`
    /// before it can be mistaken for one. See poll.hpp.
    timeout = SR_TIMEOUT, // -4
};

[[nodiscard]] constexpr bool is_ok(error_code c) noexcept {
    return c == error_code::ok;
}

/// Human-readable name for a status code. Never null.
[[nodiscard]] constexpr const char* to_string(error_code c) noexcept {
    switch (c) {
        case error_code::ok:     return "ok";
        case error_code::closed: return "closed";
        case error_code::error:  return "error";
        case error_code::fatal:  return "fatal";
        case error_code::timeout: return "timeout";
    }
    return "unknown";
}

/// A failure, owning its message.
///
/// Deliberately *not* holding a `std::string`. The C side hands us a heap
/// `char*`; wrapping that in a `std::string` would allocate a second time,
/// copy, and then free the original -- two allocations where zero extra are
/// needed. Worse, it would inflate this type to 32-40 bytes that every
/// *successful* `result<T>` would carry in its layout.
///
/// Messages have two origins and they are freed differently: one comes from
/// the C API and belongs to Rust's allocator, the other is generated here (by
/// query construction, which has no C string to adopt) and belongs to `new[]`.
/// A one-byte tag distinguishes them, which fits in the padding beside the
/// status code -- so supporting both costs nothing in size.
class error {
public:
    error() noexcept = default;

    error(error_code code, owned_string msg) noexcept : code_(code) {
        msg_ = msg.release();
        origin_ = msg_ ? origin::c_api : origin::none;
    }

    /// Adopt a raw code and message straight from a C call. Takes ownership
    /// of `raw`, which belongs to Rust's allocator.
    [[nodiscard]] static error adopt(int code, sr_string_t raw) noexcept {
        error e;
        e.code_ = static_cast<error_code>(code);
        e.msg_ = raw;
        e.origin_ = raw ? origin::c_api : origin::none;
        return e;
    }

    /// An error raised by this library rather than returned by the C API.
    ///
    /// Freeing a locally-made string with `sr_string_free` would hand Rust's
    /// allocator memory it never issued, hence the separate origin.
    [[nodiscard]] static error local(error_code code, std::string_view message) {
        error e;
        e.code_ = code;
        if (!message.empty()) {
            char* buf = new (std::nothrow) char[message.size() + 1];
            if (buf) {
                std::memcpy(buf, message.data(), message.size());
                buf[message.size()] = '\0';
                e.msg_ = buf;
                e.origin_ = origin::local;
            }
        }
        return e;
    }

    error(error&& other) noexcept
        : code_(other.code_), origin_(other.origin_), msg_(other.msg_) {
        other.origin_ = origin::none;
        other.msg_ = nullptr;
    }

    error& operator=(error&& other) noexcept {
        if (this != &other) {
            release();
            code_ = other.code_;
            origin_ = other.origin_;
            msg_ = other.msg_;
            other.origin_ = origin::none;
            other.msg_ = nullptr;
        }
        return *this;
    }

    error(const error&) = delete;
    error& operator=(const error&) = delete;

    ~error() { release(); }

    [[nodiscard]] error_code code() const noexcept { return code_; }
    [[nodiscard]] bool is_fatal() const noexcept { return code_ == error_code::fatal; }
    [[nodiscard]] bool is_closed() const noexcept { return code_ == error_code::closed; }

    /// Borrowed view of the message. Valid while this `error` lives. Empty if
    /// there was none.
    [[nodiscard]] std::string_view message() const noexcept {
        return msg_ ? std::string_view(msg_) : std::string_view();
    }

    /// Owning copy, for callers that need it to outlive the error.
    /// Allocates -- prefer `message()`.
    [[nodiscard]] std::string message_string() const {
        return std::string(message());
    }

private:
    enum class origin : std::uint8_t {
        none,    ///< no message
        c_api,   ///< Rust's allocator; released with sr_string_free
        local,   ///< new[] here; released with delete[]
    };

    void release() noexcept {
        switch (origin_) {
            case origin::c_api: ::sr_string_free(msg_); break;
            case origin::local: delete[] msg_; break;
            case origin::none: break;
        }
        origin_ = origin::none;
        msg_ = nullptr;
    }

    error_code code_{error_code::ok};
    origin     origin_{origin::none};
    char*      msg_{nullptr};
};

// ---------------------------------------------------------------------------
// result<T>
// ---------------------------------------------------------------------------

/// The outcome of a fallible call: a `T` or an `error`, never both.
///
/// Move-only by design. Every `T` this library puts in a `result` is an owned
/// handle, and copying those is either wrong or expensive; a caller who wants a
/// copy can take one of the value itself.
///
/// `value()` does not throw and does not check in release builds -- it asserts.
/// Check with `has_value()` first, or use `value_or()`.
template <class T>
class result {
public:
    using value_type = T;
    using error_type = ::surrealdb::error;

    static_assert(!std::is_reference<T>::value, "result<T&> is not supported");

    result(T value) noexcept : has_(true) {
        ::new (static_cast<void*>(&val_)) T(std::move(value));
    }

    result(error_type err) noexcept : has_(false) {
        ::new (static_cast<void*>(&err_)) error_type(std::move(err));
    }

    result(result&& other) noexcept : has_(other.has_) {
        if (has_) ::new (static_cast<void*>(&val_)) T(std::move(other.val_));
        else      ::new (static_cast<void*>(&err_)) error_type(std::move(other.err_));
    }

    result& operator=(result&& other) noexcept {
        if (this != &other) {
            destroy();
            has_ = other.has_;
            if (has_) ::new (static_cast<void*>(&val_)) T(std::move(other.val_));
            else      ::new (static_cast<void*>(&err_)) error_type(std::move(other.err_));
        }
        return *this;
    }

    result(const result&) = delete;
    result& operator=(const result&) = delete;

    ~result() { destroy(); }

    [[nodiscard]] bool has_value() const noexcept { return has_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_; }

    [[nodiscard]] T& value() & noexcept {
        SURREALDB_ASSERT(has_, "result::value() on an error result");
        return val_;
    }
    [[nodiscard]] const T& value() const& noexcept {
        SURREALDB_ASSERT(has_, "result::value() on an error result");
        return val_;
    }
    [[nodiscard]] T&& value() && noexcept {
        SURREALDB_ASSERT(has_, "result::value() on an error result");
        return std::move(val_);
    }

    template <class U>
    [[nodiscard]] T value_or(U&& fallback) && {
        return has_ ? std::move(val_) : static_cast<T>(std::forward<U>(fallback));
    }

    [[nodiscard]] const error_type& error() const& noexcept {
        SURREALDB_ASSERT(!has_, "result::error() on a value result");
        return err_;
    }
    [[nodiscard]] error_type&& error() && noexcept {
        SURREALDB_ASSERT(!has_, "result::error() on a value result");
        return std::move(err_);
    }

    /// Status code of the failure, or `ok` when engaged.
    [[nodiscard]] error_code code() const noexcept {
        return has_ ? error_code::ok : err_.code();
    }

    /// Chain a call that itself returns a `result`. Short-circuits on error.
    template <class F>
    [[nodiscard]] auto and_then(F&& f) && -> decltype(f(std::declval<T&&>())) {
        using R = decltype(f(std::declval<T&&>()));
        if (!has_) return R(std::move(err_));
        return f(std::move(val_));
    }

    /// Map the value through `f`, preserving any error.
    template <class F>
    [[nodiscard]] auto transform(F&& f) && -> result<decltype(f(std::declval<T&&>()))> {
        using U = decltype(f(std::declval<T&&>()));
        if (!has_) return result<U>(std::move(err_));
        return result<U>(f(std::move(val_)));
    }

#if SURREALDB_HAS_STD_EXPECTED
    /// C++23 interop. Additive -- `result` is not `std::expected`, it converts
    /// to one on request.
    ///
    /// **Read the value with `*e`, not `e.value()`.** `error` is move-only, and
    /// `std::expected<T, E>::value()` has to be able to construct
    /// `bad_expected_access(error())` from an lvalue -- which copies `E`. With a
    /// move-only error that member simply cannot be instantiated, and you get a
    /// static assertion from inside `<expected>` rather than anything that
    /// names this library.
    ///
    ///     auto e = std::move(r).to_std();
    ///     if (e) use(*e);                 // fine
    ///     else   log(e.error().message());
    ///
    /// `operator*`, `operator->`, `has_value()` and `error()` are all usable;
    /// only the throwing accessor is not. Making `error` copyable would fix it
    /// and cost every `result<T>` the possibility of a silent deep copy of the
    /// message on a hot path, which is the worse trade.
    [[nodiscard]] std::expected<T, error_type> to_std() && {
        if (has_) return std::expected<T, error_type>(std::move(val_));
        return std::unexpected<error_type>(std::move(err_));
    }
#endif

private:
    void destroy() noexcept {
        if (has_) val_.~T();
        else      err_.~error();
    }

    bool has_;
    union {
        T          val_;
        error_type err_;
    };
};

/// Specialisation for calls that succeed or fail but produce nothing.
template <>
class result<void> {
public:
    using value_type = void;
    using error_type = ::surrealdb::error;

    result() noexcept : err_(), has_(true) {}
    result(error_type err) noexcept : err_(std::move(err)), has_(false) {}

    result(result&&) noexcept = default;
    result& operator=(result&&) noexcept = default;
    result(const result&) = delete;
    result& operator=(const result&) = delete;

    [[nodiscard]] bool has_value() const noexcept { return has_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_; }

    void value() const noexcept {
        SURREALDB_ASSERT(has_, "result<void>::value() on an error result");
    }

    [[nodiscard]] const error_type& error() const& noexcept {
        SURREALDB_ASSERT(!has_, "result<void>::error() on a value result");
        return err_;
    }
    [[nodiscard]] error_type&& error() && noexcept {
        SURREALDB_ASSERT(!has_, "result<void>::error() on a value result");
        return std::move(err_);
    }

    [[nodiscard]] error_code code() const noexcept {
        return has_ ? error_code::ok : err_.code();
    }

    template <class F>
    [[nodiscard]] auto and_then(F&& f) && -> decltype(f()) {
        using R = decltype(f());
        if (!has_) return R(std::move(err_));
        return f();
    }

#if SURREALDB_HAS_STD_EXPECTED
    [[nodiscard]] std::expected<void, error_type> to_std() && {
        if (has_) return std::expected<void, error_type>();
        return std::unexpected<error_type>(std::move(err_));
    }
#endif

private:
    error_type err_;
    bool       has_;
};

/// Convenience: a successful `result<void>`.
[[nodiscard]] inline result<void> ok() noexcept { return result<void>(); }

} // namespace surrealdb
