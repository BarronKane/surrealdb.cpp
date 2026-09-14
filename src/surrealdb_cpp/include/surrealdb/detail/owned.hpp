#pragma once

/// @file detail/owned.hpp
/// RAII ownership for C handles.
///
/// The C API has thirteen destructors in three shapes:
///
///   1. pointer-owning   `sr_value_free(sr_value_t*)`, `sr_array_free`, ...
///   2. by-value-owning  `sr_object_free(sr_object_t)`, `sr_string_free(char*)`, ...
///   3. span-owning      `sr_values_free(sr_value_t*, int)`, ...
///
/// Shapes 1 and 2 unify: in both the deleter is a callable taking the handle by
/// value. That is `owned`. Shape 3 needs a length alongside the pointer; that is
/// `owned_span`. Two templates, not thirteen hand-written classes.
///
/// Both are layout-identical to the raw handle they wrap and generate the same
/// instructions -- the destructor is the `sr_*_free` call you would have written
/// by hand, placed where you cannot forget it. Safety here is free, not cheap.

#include "../config.hpp"
#include "c_api.hpp"

#include <cstdint>
#include <utility>

namespace surrealdb {
namespace detail {

/// Ownership policy contract.
///
/// A policy supplies four things, which together cover every shape above:
///
///   using handle = ...;                        the thing being owned
///   static handle null() noexcept;             the disengaged value
///   static bool valid(const handle&) noexcept; is this engaged?
///   static void destroy(handle) noexcept;      release it
///
/// The indirection exists because not every C handle is a pointer. `sr_object_t`
/// is a struct, so `h != nullptr` does not compile for it; the policy knows how
/// to ask.

/// Policy for a plain pointer handle freed by `Fn(ptr)`.
template <class T, void (*Fn)(T*)>
struct ptr_policy {
    using handle = T*;
    static constexpr handle null() noexcept { return nullptr; }
    static constexpr bool valid(handle h) noexcept { return h != nullptr; }
    static void destroy(handle h) noexcept { Fn(h); }
};

/// Policy for a `(pointer, length)` pair freed by `Fn(ptr, len)`.
template <class T, void (*Fn)(T*, int)>
struct span_policy {
    using element = T;
    static void destroy(T* p, int n) noexcept { Fn(p, n); }
};

// ---------------------------------------------------------------------------
// owned
// ---------------------------------------------------------------------------

/// Exclusive ownership of a single C handle. Move-only.
template <class Policy>
class owned {
public:
    using handle = typename Policy::handle;

    owned() noexcept : h_(Policy::null()) {}
    explicit owned(handle h) noexcept : h_(h) {}

    owned(owned&& other) noexcept : h_(other.release()) {}
    owned& operator=(owned&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    owned(const owned&) = delete;
    owned& operator=(const owned&) = delete;

    ~owned() { reset(); }

    [[nodiscard]] handle get() const noexcept { return h_; }

    /// Address of the stored handle, for C calls that mutate through a pointer
    /// to the struct itself -- `sr_object_insert(sr_object_t*, ...)` being the
    /// motivating case. Does not transfer ownership; the pointer is valid only
    /// while this object is.
    [[nodiscard]] handle* addr() noexcept { return &h_; }
    [[nodiscard]] const handle* addr() const noexcept { return &h_; }

    /// Relinquish ownership without destroying. The caller becomes responsible.
    [[nodiscard]] handle release() noexcept {
        handle h = h_;
        h_ = Policy::null();
        return h;
    }

    /// Destroy the current handle and optionally adopt another.
    void reset(handle h = Policy::null()) noexcept {
        if (Policy::valid(h_)) Policy::destroy(h_);
        h_ = h;
    }

    void swap(owned& other) noexcept {
        handle t = h_;
        h_ = other.h_;
        other.h_ = t;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return Policy::valid(h_); }

private:
    handle h_;
};

template <class Policy>
void swap(owned<Policy>& a, owned<Policy>& b) noexcept { a.swap(b); }

// ---------------------------------------------------------------------------
// owned_span
// ---------------------------------------------------------------------------

/// Exclusive ownership of a contiguous C-allocated array freed as
/// `Fn(ptr, len)`. Move-only. Doubles as a contiguous range over its elements.
template <class Policy>
class owned_span {
public:
    using element = typename Policy::element;
    using iterator = element*;
    using const_iterator = const element*;

    owned_span() noexcept : p_(nullptr), n_(0) {}
    owned_span(element* p, int n) noexcept : p_(p), n_(n < 0 ? 0 : n) {}

    owned_span(owned_span&& other) noexcept : p_(other.p_), n_(other.n_) {
        other.p_ = nullptr;
        other.n_ = 0;
    }
    owned_span& operator=(owned_span&& other) noexcept {
        if (this != &other) {
            reset();
            p_ = other.p_;
            n_ = other.n_;
            other.p_ = nullptr;
            other.n_ = 0;
        }
        return *this;
    }

    owned_span(const owned_span&) = delete;
    owned_span& operator=(const owned_span&) = delete;

    ~owned_span() { reset(); }

    [[nodiscard]] element* data() const noexcept { return p_; }
    [[nodiscard]] int size() const noexcept { return n_; }
    [[nodiscard]] bool empty() const noexcept { return n_ == 0; }

    [[nodiscard]] iterator begin() const noexcept { return p_; }
    [[nodiscard]] iterator end() const noexcept { return p_ + n_; }

    /// Unchecked. Callers are expected to have bounds-checked via `size()`;
    /// this is the hot path and does not pay for a branch.
    [[nodiscard]] element& operator[](int i) const noexcept { return p_[i]; }

    [[nodiscard]] std::pair<element*, int> release() noexcept {
        std::pair<element*, int> r{p_, n_};
        p_ = nullptr;
        n_ = 0;
        return r;
    }

    void reset() noexcept {
        if (p_ != nullptr) Policy::destroy(p_, n_);
        p_ = nullptr;
        n_ = 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }

private:
    element* p_;
    int n_;
};

// ---------------------------------------------------------------------------
// Concrete policies -- one per destructor in the C API
// ---------------------------------------------------------------------------

namespace policy {

// Shape 1: pointer-owning.
using value_ptr  = ptr_policy<sr_value_t,     ::sr_value_free>;
using array      = ptr_policy<sr_array_t,     ::sr_array_free>;
using rpc_stream = ptr_policy<sr_RpcStream,   ::sr_rpc_stream_free>;
using stream     = ptr_policy<sr_stream_t,    ::sr_stream_kill>;
using surreal    = ptr_policy<sr_surreal_t,   ::sr_surreal_disconnect>;
using surreal_rpc = ptr_policy<sr_surreal_rpc_t, ::sr_surreal_rpc_disconnect>;

/// `sr_string_t` is `char*`, freed by value.
struct string {
    using handle = sr_string_t;
    static constexpr handle null() noexcept { return nullptr; }
    static constexpr bool valid(handle h) noexcept { return h != nullptr; }
    static void destroy(handle h) noexcept { ::sr_string_free(h); }
};

/// Shape 2: `sr_object_t` is a struct wrapping one opaque pointer.
struct object {
    using handle = sr_object_t;
    static handle null() noexcept { return sr_object_t{nullptr}; }
    static bool valid(const handle& h) noexcept { return h._0 != nullptr; }
    static void destroy(handle h) noexcept { ::sr_object_free(h); }
};

/// Shape 2: `sr_notification_t` carries an owned Value in the caller's own
/// storage, so `sr_notification_free` takes it by value. Note `sr_value_free`
/// must NOT be used on `notification.data`: that reclaims a Box, and this value
/// was never boxed.
struct notification {
    using handle = sr_notification_t;
    static handle null() noexcept { return sr_notification_t{}; }
    /// A zeroed notification carries SR_VALUE_NONE data, which owns nothing.
    static bool valid(const handle& h) noexcept {
        return h.data.tag != SR_VALUE_NONE;
    }
    static void destroy(handle h) noexcept { ::sr_notification_free(h); }
};

// There is deliberately no policy for `sr_bytes_t` or `sr_arr_res_t`.
//
// Both once had by-value destructors (`sr_bytes_free`, `sr_arr_res_free`), and
// an earlier draft of this file mechanically wrapped them -- producing two RAII
// types that could never be legitimately constructed, because nothing in the C
// API hands out either struct by value. The only way to obtain one was to copy
// it out of a container that still owned the contents, making every release a
// double free. Those destructors were removed in 0.2.1; bytes and query results
// are owned as spans instead, via `owned_byte_array` and `owned_arr_results`.

// Shape 3: span-owning.
using values     = span_policy<sr_value_t,   ::sr_values_free>;
using strings    = span_policy<char*,        ::sr_string_arr_free>;
using arr_results = span_policy<sr_arr_res_t, ::sr_arr_res_arr_free>;
using byte_array = span_policy<std::uint8_t, ::sr_byte_arr_free>;
using uuids      = span_policy<sr_uuid_t,     ::sr_uuid_arr_free>;

} // namespace policy

} // namespace detail

// ---------------------------------------------------------------------------
// Public aliases
// ---------------------------------------------------------------------------

using owned_string      = detail::owned<detail::policy::string>;
using owned_object      = detail::owned<detail::policy::object>;
using owned_notification = detail::owned<detail::policy::notification>;
using owned_value       = detail::owned<detail::policy::value_ptr>;
using owned_array       = detail::owned<detail::policy::array>;
using owned_stream      = detail::owned<detail::policy::stream>;
using owned_rpc_stream  = detail::owned<detail::policy::rpc_stream>;
using owned_connection  = detail::owned<detail::policy::surreal>;
using owned_rpc         = detail::owned<detail::policy::surreal_rpc>;

using owned_values      = detail::owned_span<detail::policy::values>;
using owned_strings     = detail::owned_span<detail::policy::strings>;
using owned_arr_results = detail::owned_span<detail::policy::arr_results>;
using owned_byte_array  = detail::owned_span<detail::policy::byte_array>;
using owned_uuids       = detail::owned_span<detail::policy::uuids>;

} // namespace surrealdb
