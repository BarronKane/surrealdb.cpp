#pragma once

/// @file array.hpp
/// Owning array construction, and adapters from owned results to `array_view`.
/// The read-only `array_view` itself is in value.hpp.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/owned.hpp"
#include "value.hpp"

#include <cstddef>
#include <string>
#include <vector>
#include <type_traits>

namespace surrealdb {

/// View the values returned by a query as an `array_view`.
///
/// `sr_select` and friends hand back a `(ptr, len)` block owned by
/// `owned_values`; this borrows it. The view is valid only while the owner is.
[[nodiscard]] inline array_view view(const owned_values& values) noexcept {
    return array_view(values.data(), values.size());
}

/// Deleted for temporaries. The view borrows, so
/// `view(db.select("t").value())` would read freed memory the moment the
/// statement ends -- a heap-use-after-free that compiles cleanly and is the
/// most natural way to write the call. Bind the owner to a local first.
array_view view(owned_values&&)
    SURREALDB_DELETED("view() over a temporary: the values are freed at the "
                      "semicolon and the view is left dangling. Name the owner first.");


namespace detail {
/// Values reach the bulk builder as either borrowed views or owned handles.
inline const sr_value_t* element_ptr(value v) noexcept { return v.raw(); }
inline const sr_value_t* element_ptr(const owned_value& v) noexcept { return v.get(); }
} // namespace detail

/// Build an array from values that are already to hand, in one allocation.
///
/// Linear, where repeated `array_builder::push` is quadratic. The values are
/// deep-copied, so the block you pass stays yours to release.
[[nodiscard]] inline owned_array array_from(const sr_value_t* values, int len) noexcept {
    return owned_array(::sr_array_from_values(values, len));
}

/// Same, from anything this library already hands you as a run of values --
/// a query result, another array, an `owned_values` span.
[[nodiscard]] inline owned_array array_from(array_view v) noexcept {
    return array_from(v.data(), v.size());
}

/// Same, from a container of `value` or `owned_value`.
///
/// The C entry point wants a contiguous block of `sr_value_t`, which is the one
/// place a caller would otherwise have to name a C type to get the linear path.
/// The block is built here and thrown away; `sr_array_from_values` deep-copies,
/// so the values stay owned by whatever the caller passed in.
template <class Container>
[[nodiscard]] owned_array array_from(const Container& values) {
    std::vector<sr_value_t> block;
    block.reserve(values.size());
    for (const auto& v : values) {
        const sr_value_t* p = detail::element_ptr(v);
        if (p) block.push_back(*p);
    }
    if (block.empty()) return array_from(nullptr, 0);
    return array_from(block.data(), static_cast<int>(block.size()));
}

/// Builds an `sr_array_t` for passing into the C API.
///
/// **Appending is quadratic.** `sr_array_push` does not mutate: it allocates a
/// *new* array of length n+1, copies, and returns it, so building n elements
/// copies 1 + 2 + ... + n values. Measured, -O2:
///
///     n       push()      array_from()
///     1000     19.8 ms        0.051 ms
///     2000     94.8 ms        0.071 ms
///     4000    427.5 ms        0.256 ms
///
/// That is the right trade for the handful of arguments a query binds, and
/// badly the wrong one for bulk data.
///
/// It cannot be made linear while `push` keeps its contract. A borrowed
/// `value` is copied *at the call*, which is what lets a caller push something
/// and drop it on the next line; deferring the copy to a single bulk build at
/// the end would leave those borrows dangling, and there is no
/// `sr_value_clone` to own them with instead.
///
/// So when the elements already exist as a run of values, skip the builder and
/// use `array_from`, which is one allocation -- that is exactly what
/// `sr_array_from_values` is for.
class array_builder {
public:
    array_builder() noexcept = default;

    /// Bulk construction: linear, unlike pushing the same values one by one.
    explicit array_builder(array_view values) noexcept
        : arr_(::sr_array_from_values(values.data(), values.size())) {}

    array_builder(array_builder&&) noexcept = default;
    array_builder& operator=(array_builder&&) noexcept = default;
    array_builder(const array_builder&) = delete;
    array_builder& operator=(const array_builder&) = delete;

    /// Append a borrowed value. The C side copies it.
    array_builder& push(value v) noexcept {
        if (!v.valid()) return *this;
        const sr_array_t empty{nullptr, 0};
        const sr_array_t* current = arr_ ? arr_.get() : &empty;
        if (sr_array_t* next = ::sr_array_push(current, v.raw())) {
            arr_.reset(next);
        }
        return *this;
    }

    array_builder& push(const owned_value& v) noexcept {
        return v ? push(value(v.get())) : *this;
    }

    array_builder& push(bool v) noexcept         { return push_owned(::sr_value_bool(v)); }

    /// Nest a list inside this one.
    ///
    /// Needs surrealdb.c 0.2.3 or newer: before `sr_value_array` took an
    /// argument there was no array *value* to nest, so nothing with a nested
    /// list -- GeoJSON coordinates, a list of lists -- could be built at all.
    array_builder& push(const array_builder& nested) noexcept {
        return push_owned(::sr_value_array(nested.raw()));
    }
    array_builder& push(const char* v) noexcept  { return push_owned(::sr_value_string(v)); }
    array_builder& push(const std::string& v) noexcept {
        return push_owned(::sr_value_string(v.c_str()));
    }

    /// Any integral value, widened to 64 bits.
    ///
    /// A template rather than an `std::int64_t` overload: alongside `double`
    /// and `bool`, a plain `push(5)` is three equally-ranked conversions and
    /// so ambiguous. Every literal would need a cast, which is not a library
    /// anyone wants to use. `bool` is excluded so it keeps its own overload
    /// instead of arriving as 0 or 1.
    template <class T,
              std::enable_if_t<std::is_integral<T>::value &&
                                   !std::is_same<T, bool>::value,
                               int> = 0>
    array_builder& push(T v) noexcept {
        return push_owned(::sr_value_int(static_cast<std::int64_t>(v)));
    }

    /// Any floating-point value, widened to `double`.
    template <class T,
              std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
    array_builder& push(T v) noexcept {
        return push_owned(::sr_value_float(static_cast<double>(v)));
    }

    [[nodiscard]] const sr_array_t* raw() const& noexcept { return arr_.get(); }
    const sr_array_t* raw() const&&
        SURREALDB_DELETED("raw() on an array_builder temporary outlives the array.");

    [[nodiscard]] array_view view() const& noexcept {
        return arr_ ? array_view(*arr_.get()) : array_view();
    }
    array_view view() const&&
        SURREALDB_DELETED("view() on an array_builder temporary outlives the array.");

    [[nodiscard]] int size() const noexcept { return view().size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Hand ownership to the caller. The builder is empty afterwards.
    [[nodiscard]] owned_array release() noexcept { return std::move(arr_); }

private:
    array_builder& push_owned(sr_value_t* made) noexcept {
        owned_value v(made);
        return v ? push(value(v.get())) : *this;
    }

    owned_array arr_;
};

/// An array argument: anything the C can read as one.
///
/// The counterpart to `object_arg`. Unlike that one this cannot be a bare
/// pointer: an `array_view` is a `(ptr, len)` pair with no `sr_array_t` behind
/// it to point at, so the two-word header is carried here by value and `raw()`
/// hands back its own address. That makes the class self-contained under copy,
/// which matters because it is passed by value.
class array_arg {
public:
    /// No array. What an omitted argument list means.
    array_arg() noexcept = default;
    array_arg(std::nullptr_t) noexcept {}

    array_arg(const array_builder& b) noexcept : array_arg(b.raw()) {}
    array_arg(const owned_array& a) noexcept : array_arg(a.get()) {}
    array_arg(array_view v) noexcept
        // `sr_array_t::arr` is not const-qualified, and the constructors this
        // feeds only ever read it -- they deep-copy. The cast buys the view
        // its way into a C struct that predates the read/write split.
        : block_{const_cast<sr_value_t*>(v.data()), v.size()}, have_(true) {}
    array_arg(const sr_array_t* a) noexcept
        : block_(a ? *a : sr_array_t{nullptr, 0}), have_(a != nullptr) {}

    /// Null when there is no array at all, which is distinct from an array
    /// that is present and empty -- the C tells those apart.
    [[nodiscard]] const sr_array_t* raw() const noexcept {
        return have_ ? &block_ : nullptr;
    }
    [[nodiscard]] bool valid() const noexcept { return have_; }

private:
    sr_array_t block_{nullptr, 0};
    bool have_{false};
};

} // namespace surrealdb
