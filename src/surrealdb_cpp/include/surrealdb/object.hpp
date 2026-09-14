#pragma once

/// @file object.hpp
/// Owning object construction. The read-only `object_view` is in value.hpp.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/owned.hpp"
#include "array.hpp"
#include "value.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

namespace surrealdb {

/// Builds an `sr_object_t` for passing to create/update/merge and friends.
///
/// Move-only; the underlying object is released by `owned_object`. Inserts
/// mutate in place, so building an object is linear -- unlike arrays, which the
/// C API can only grow by copying (see `array_builder`).
class object_builder {
public:
    object_builder() noexcept : obj_(::sr_object_new()) {}

    object_builder(object_builder&&) noexcept = default;
    object_builder& operator=(object_builder&&) noexcept = default;
    object_builder(const object_builder&) = delete;
    object_builder& operator=(const object_builder&) = delete;

    // -- typed setters ------------------------------------------------------
    //
    // Keys and string values are taken as `const char*` or `std::string`
    // rather than `std::string_view`: the C API needs null termination, and a
    // string_view overload would have to allocate a terminated copy behind the
    // caller's back. Both accepted forms are already terminated.

    object_builder& set(const char* key, bool v) noexcept {
        return insert_owned(key, ::sr_value_bool(v));
    }

    /// Any integral value, widened to 64 bits.
    ///
    /// Deliberately routed through `sr_value_int` rather than the direct
    /// `sr_object_insert_int` shim, which costs one extra box. That shim took a
    /// C `int` and silently truncated past 32 bits until surrealdb.c 0.2.1
    /// widened it, and this library can be built against a system copy of
    /// either. A quiet wrong answer is worse than an allocation.
    ///
    /// A template rather than fixed `int`/`std::int64_t` overloads: with
    /// `double` and `bool` in the set, an unsigned or `short` argument is
    /// ambiguous between all three, and callers should not have to cast every
    /// literal. `bool` keeps its own overload so it does not arrive as 0 or 1.
    template <class T,
              std::enable_if_t<std::is_integral<T>::value &&
                                   !std::is_same<T, bool>::value,
                               int> = 0>
    object_builder& set(const char* key, T v) noexcept {
        return insert_owned(key, ::sr_value_int(static_cast<std::int64_t>(v)));
    }


    /// A `float`, inserted directly.
    ///
    /// The direct shims are used here where they are not for integers: they
    /// never had a width problem, and they skip the boxed `sr_value_t` that
    /// `insert_owned` would allocate and immediately free. Field inserts are a
    /// hot path.
    object_builder& set(const char* key, float v) noexcept {
        if (raw() && key) ::sr_object_insert_float(raw(), key, v);
        return *this;
    }

    /// Any other floating-point value, widened to `double`.
    template <class T,
              std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
    object_builder& set(const char* key, T v) noexcept {
        if (raw() && key) ::sr_object_insert_double(raw(), key, static_cast<double>(v));
        return *this;
    }

    object_builder& set(const char* key, const char* v) noexcept {
        if (raw() && key && v) ::sr_object_insert_str(raw(), key, v);
        return *this;
    }

    object_builder& set(const char* key, const std::string& v) noexcept {
        return set(key, v.c_str());
    }

    /// Insert a borrowed value. The C side clones it, so the caller keeps
    /// ownership of whatever it passed.
    object_builder& set(const char* key, value v) noexcept {
        if (raw() && key && v.valid()) ::sr_object_insert(raw(), key, v.raw());
        return *this;
    }

    object_builder& set(const char* key, const owned_value& v) noexcept {
        if (raw() && key && v) ::sr_object_insert(raw(), key, v.get());
        return *this;
    }

    /// Insert a nested object. Takes it by value and consumes it, since the C
    /// call clones and the source would otherwise be a redundant live copy.
    /// Store a list in a field.
    ///
    /// Only possible since surrealdb.c 0.2.3 gave `sr_value_array` an argument.
    /// Before that an array field could only ever be written empty, and the
    /// list had to go into the SurrealQL text instead.
    object_builder& set(const char* key, const array_builder& list) noexcept {
        return insert_owned(key, ::sr_value_array(list.raw()));
    }

    /// Store a list as a set: duplicates are discarded by the database.
    object_builder& set_unique(const char* key, const array_builder& list) noexcept {
        return insert_owned(key, ::sr_value_set(list.raw()));
    }

    object_builder& set(const char* key, object_builder nested) noexcept {
        if (raw() && key) {
            owned_value wrapped(::sr_value_object(nested.raw()));
            if (wrapped) ::sr_object_insert(raw(), key, wrapped.get());
        }
        return *this;
    }

    // -- access -------------------------------------------------------------

    [[nodiscard]] sr_object_t* raw() noexcept { return obj_ ? obj_.addr() : nullptr; }
    // Not ref-qualified, unlike `view(const owned_object&)` below: the mutable
    // `raw()` above has no ref-qualifier and C++ forbids mixing the two on one
    // name. The exposure is also far smaller -- a builder is something you
    // named and are filling in, not a temporary that fell out of `.value()`.
    [[nodiscard]] const sr_object_t* raw() const noexcept {
        return obj_ ? obj_.addr() : nullptr;
    }

    [[nodiscard]] object_view view() const noexcept { return object_view(raw()); }
    [[nodiscard]] int size() const noexcept { return view().size(); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Hand ownership to the caller. The builder is empty afterwards.
    [[nodiscard]] owned_object release() noexcept { return std::move(obj_); }

private:
    object_builder& insert_owned(const char* key, sr_value_t* made) noexcept {
        owned_value v(made);
        if (raw() && key && v) ::sr_object_insert(raw(), key, v.get());
        return *this;
    }

    owned_object obj_;
};

/// The keys of an object, owned.
///
/// `sr_object_keys` allocates an array of C strings, so enumerating an object
/// costs one allocation -- which is why `object_view` offers lookup but not
/// iteration. Hold on to the result if you need the keys more than once.
class object_keys {
public:
    explicit object_keys(const sr_object_t* obj) noexcept {
        char** raw = nullptr;
        const int n = obj ? ::sr_object_keys(obj, &raw) : 0;
        if (n > 0 && raw) keys_ = owned_strings(raw, n);
    }

    [[nodiscard]] int size() const noexcept { return keys_.size(); }
    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }

    [[nodiscard]] std::optional<std::string_view> operator[](int i) const noexcept {
        if (i < 0 || i >= keys_.size()) return std::nullopt;
        const char* k = keys_[i];
        if (!k) return std::nullopt;
        return std::string_view(k);
    }

    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = std::string_view;
        using difference_type   = std::ptrdiff_t;
        using reference         = std::string_view;
        using pointer           = void;

        iterator() noexcept = default;
        explicit iterator(char* const* p) noexcept : p_(p) {}

        [[nodiscard]] reference operator*() const noexcept {
            return *p_ ? std::string_view(*p_) : std::string_view();
        }
        iterator& operator++() noexcept { ++p_; return *this; }
        iterator operator++(int) noexcept { iterator t = *this; ++p_; return t; }
        [[nodiscard]] friend bool operator==(iterator a, iterator b) noexcept { return a.p_ == b.p_; }
        [[nodiscard]] friend bool operator!=(iterator a, iterator b) noexcept { return a.p_ != b.p_; }

    private:
        char* const* p_{nullptr};
    };

    [[nodiscard]] iterator begin() const noexcept { return iterator(keys_.data()); }
    [[nodiscard]] iterator end() const noexcept {
        return iterator(keys_.data() + keys_.size());
    }

private:
    owned_strings keys_;
};

[[nodiscard]] inline object_keys keys_of(object_view obj) noexcept {
    return object_keys(obj.raw());
}

/// Borrow an owned object as a view.
///
/// `owned_object::get()` hands back the `sr_object_t` *by value* -- it is a
/// one-pointer struct, not a pointer -- so it cannot be tested or dereferenced
/// like a handle. This is the way to look inside one; the view is valid only
/// while the owner is. Mirrors `view(const owned_values&)` in array.hpp.
[[nodiscard]] inline object_view view(const owned_object& obj) noexcept {
    return object_view(obj ? obj.addr() : nullptr);
}

/// Deleted for temporaries. The view borrows, so
/// `view(db.create(...).value())` would read freed memory the moment the
/// statement ends -- a heap-use-after-free that compiles cleanly and is the
/// most natural way to write the call. Bind the owner to a local first.
object_view view(owned_object&&)
    SURREALDB_DELETED("view() over a temporary: the object is freed at the "
                      "semicolon and the view is left dangling. Name the owner first.");


} // namespace surrealdb
