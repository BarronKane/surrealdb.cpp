#pragma once

/// @file value.hpp
/// Non-owning views over the C value types.
///
/// `value`, `array_view` and `object_view` live in one header on purpose: they
/// are mutually recursive -- a value may be an array of values, an array
/// element is a value -- and splitting them would buy nothing but forward
/// declarations. `array.hpp` and `object.hpp` add the owning builders on top.
///
/// **Everything here is a view.** A `value` is one pointer, not a copy of the
/// 48-byte `sr_value_t` it refers to; these types are traversed in loops and
/// copying the union would be a six-fold size increase for nothing. Views
/// borrow: they are valid only while whatever owns the underlying storage is
/// alive. Ownership lives in `detail/owned.hpp`.

#include "config.hpp"
#include "detail/c_api.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

#if SURREALDB_HAS_RANGES
#  include <ranges>
#endif

namespace surrealdb {

class value;
class array_view;
class object_view;

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------

/// The eighteen shapes a value can take.
///
/// `none` and `null` are deliberately distinct, as are `array` and `set`.
/// Collapsing either pair loses information the database considers meaningful:
/// an absent field is not a field explicitly set to null. The C library had a
/// catch-all that flattened five of these to `none` until 0.2.0, so a caller
/// could not tell a file reference from a missing one.
enum class value_kind : int {
    none      = SR_VALUE_NONE,
    null      = SR_VALUE_NULL,
    boolean   = SR_VALUE_BOOL,
    number    = SR_VALUE_NUMBER,
    string    = SR_VALUE_STRAND,
    duration  = SR_VALUE_DURATION,
    datetime  = SR_VALUE_DATETIME,
    uuid      = SR_VALUE_UUID,
    array     = SR_VALUE_ARRAY,
    object    = SR_VALUE_OBJECT,
    geometry  = SR_GEOMETRY_OBJECT,
    bytes     = SR_VALUE_BYTES,
    thing     = SR_VALUE_THING,
    table     = SR_VALUE_TABLE,
    file      = SR_VALUE_FILE,
    range     = SR_VALUE_RANGE,
    regex     = SR_VALUE_REGEX,
    set       = SR_VALUE_SET,
};

[[nodiscard]] constexpr const char* to_string(value_kind k) noexcept {
    switch (k) {
        case value_kind::none:     return "none";
        case value_kind::null:     return "null";
        case value_kind::boolean:  return "bool";
        case value_kind::number:   return "number";
        case value_kind::string:   return "string";
        case value_kind::duration: return "duration";
        case value_kind::datetime: return "datetime";
        case value_kind::uuid:     return "uuid";
        case value_kind::array:    return "array";
        case value_kind::object:   return "object";
        case value_kind::geometry: return "geometry";
        case value_kind::bytes:    return "bytes";
        case value_kind::thing:    return "thing";
        case value_kind::table:    return "table";
        case value_kind::file:     return "file";
        case value_kind::range:    return "range";
        case value_kind::regex:    return "regex";
        case value_kind::set:      return "set";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Small payload types
// ---------------------------------------------------------------------------

/// Several kinds are carried as strings but mean different things. Wrapping
/// each keeps them distinguishable in `visit`, where a bare `string_view`
/// would make a datetime and a regex indistinguishable to the visitor.
struct datetime_ref { std::string_view text; };
struct table_ref    { std::string_view name; };
struct regex_ref    { std::string_view pattern; };
/// Decimals cross the C boundary as text to avoid losing precision.
struct decimal_ref  { std::string_view text; };

struct duration_ref {
    std::uint64_t seconds;
    std::uint32_t nanoseconds;
};

struct uuid_ref {
    const std::uint8_t* bytes;   ///< 16 bytes, borrowed
};

struct bytes_ref {
    const std::uint8_t* data;
    int size;

    [[nodiscard]] const std::uint8_t* begin() const noexcept { return data; }
    [[nodiscard]] const std::uint8_t* end() const noexcept {
        return data + (size < 0 ? 0 : size);
    }
    [[nodiscard]] bool empty() const noexcept { return size <= 0 || data == nullptr; }
};

struct file_ref {
    std::string_view bucket;
    std::string_view key;
};

/// Markers so `visit` can distinguish absence from explicit null without the
/// visitor inspecting a tag.
struct none_t {};
struct null_t {};

// ---------------------------------------------------------------------------
// value
// ---------------------------------------------------------------------------

/// A borrowed view of one `sr_value_t`.
///
/// Trivially copyable and one pointer wide. Every accessor checks the tag and
/// returns `std::nullopt` on a mismatch, so reading the wrong union member is
/// not expressible.
class value {
public:
    value() noexcept = default;
    explicit value(const sr_value_t* raw) noexcept : v_(raw) {}

    [[nodiscard]] const sr_value_t* raw() const noexcept { return v_; }
    [[nodiscard]] bool valid() const noexcept { return v_ != nullptr; }

    /// The kind of the referenced value. An invalid view reports `none`, which
    /// keeps callers from having to null-check before switching.
    [[nodiscard]] value_kind kind() const noexcept {
        return v_ ? static_cast<value_kind>(v_->tag) : value_kind::none;
    }

    [[nodiscard]] bool is(value_kind k) const noexcept { return kind() == k; }
    [[nodiscard]] bool is_none() const noexcept { return kind() == value_kind::none; }
    [[nodiscard]] bool is_null() const noexcept { return kind() == value_kind::null; }
    /// True for either absence or explicit null, for callers that genuinely do
    /// not care which -- the distinction is preserved, not forced.
    [[nodiscard]] bool is_nullish() const noexcept { return is_none() || is_null(); }

    // -- scalars ------------------------------------------------------------

    [[nodiscard]] std::optional<bool> as_bool() const noexcept {
        if (!is(value_kind::boolean)) return std::nullopt;
        return v_->sr_value_bool;
    }

    [[nodiscard]] std::optional<std::int64_t> as_int() const noexcept {
        if (!is(value_kind::number) || v_->sr_value_number.tag != SR_NUMBER_INT)
            return std::nullopt;
        return v_->sr_value_number.sr_number_int;
    }

    [[nodiscard]] std::optional<double> as_double() const noexcept {
        if (!is(value_kind::number) || v_->sr_value_number.tag != SR_NUMBER_FLOAT)
            return std::nullopt;
        return v_->sr_value_number.sr_number_float;
    }

    [[nodiscard]] std::optional<decimal_ref> as_decimal() const noexcept {
        if (!is(value_kind::number) || v_->sr_value_number.tag != SR_NUMBER_DECIMAL)
            return std::nullopt;
        return decimal_ref{sv(v_->sr_value_number.sr_number_decimal)};
    }

    /// Any numeric kind widened to double. Decimals are not converted -- doing
    /// so silently would defeat the reason they travel as text.
    [[nodiscard]] std::optional<double> as_number() const noexcept {
        if (auto i = as_int()) return static_cast<double>(*i);
        return as_double();
    }

    [[nodiscard]] std::optional<std::string_view> as_string() const noexcept {
        if (!is(value_kind::string)) return std::nullopt;
        return sv(v_->sr_value_strand);
    }

    [[nodiscard]] std::optional<datetime_ref> as_datetime() const noexcept {
        if (!is(value_kind::datetime)) return std::nullopt;
        return datetime_ref{sv(v_->sr_value_datetime)};
    }

    [[nodiscard]] std::optional<table_ref> as_table() const noexcept {
        if (!is(value_kind::table)) return std::nullopt;
        return table_ref{sv(v_->sr_value_table)};
    }

    [[nodiscard]] std::optional<regex_ref> as_regex() const noexcept {
        if (!is(value_kind::regex)) return std::nullopt;
        return regex_ref{sv(v_->sr_value_regex)};
    }

    [[nodiscard]] std::optional<duration_ref> as_duration() const noexcept {
        if (!is(value_kind::duration)) return std::nullopt;
        return duration_ref{v_->sr_value_duration.secs, v_->sr_value_duration.nanos};
    }

    [[nodiscard]] std::optional<uuid_ref> as_uuid() const noexcept {
        if (!is(value_kind::uuid)) return std::nullopt;
        return uuid_ref{v_->sr_value_uuid._0};
    }

    [[nodiscard]] std::optional<bytes_ref> as_bytes() const noexcept {
        if (!is(value_kind::bytes)) return std::nullopt;
        return bytes_ref{v_->sr_value_bytes.arr, v_->sr_value_bytes.len};
    }

    [[nodiscard]] std::optional<file_ref> as_file() const noexcept {
        if (!is(value_kind::file)) return std::nullopt;
        return file_ref{sv(v_->sr_value_file.bucket), sv(v_->sr_value_file.key)};
    }

    // -- containers (defined below, once the views are complete) -------------

    [[nodiscard]] std::optional<array_view> as_array() const noexcept;
    /// A set is an array whose elements are unique. Kept distinct from
    /// `as_array` so the guarantee is not silently discarded.
    [[nodiscard]] std::optional<array_view> as_set() const noexcept;
    [[nodiscard]] std::optional<object_view> as_object() const noexcept;

    // -- composite handles --------------------------------------------------

    /// Record id. The id half is a tagged union of its own; the raw handle is
    /// exposed rather than modelled, since nothing in this library needs to
    /// take it apart yet.
    /// The raw C record id. Prefer `thing(v)` from `geometry.hpp`.
    [[nodiscard]] std::optional<const sr_thing_t*> as_thing_raw() const noexcept {
        if (!is(value_kind::thing)) return std::nullopt;
        return &v_->sr_value_thing;
    }

    [[nodiscard]] std::optional<const sr_range_t*> as_range() const noexcept {
        if (!is(value_kind::range) || v_->sr_value_range == nullptr) return std::nullopt;
        return v_->sr_value_range;
    }

    /// The raw C geometry. Prefer `geometry(v)` from `geometry.hpp`, which
    /// returns a `geometry_ref` and saves you switching on the union tag; this
    /// stays for anything that view does not cover.
    [[nodiscard]] std::optional<const sr_geometry_t*> as_geometry_raw() const noexcept {
        if (!is(value_kind::geometry)) return std::nullopt;
        return &v_->sr_geometry_object;
    }

    /// Dispatch on the kind, calling `f` with the decoded payload.
    ///
    /// `none` and `null` arrive as `none_t` / `null_t`; the string-shaped kinds
    /// arrive as their distinct wrapper types, so a visitor can tell a datetime
    /// from a regex. Numbers arrive already resolved to `int64_t`, `double` or
    /// `decimal_ref`.
    template <class F>
    decltype(auto) visit(F&& f) const;

private:
    static std::string_view sv(const char* s) noexcept {
        return s ? std::string_view(s) : std::string_view();
    }

    const sr_value_t* v_{nullptr};
};

static_assert(sizeof(value) == sizeof(void*), "value must stay pointer-sized");

// ---------------------------------------------------------------------------
// array_view
// ---------------------------------------------------------------------------

/// A borrowed, contiguous range of values.
///
/// `sr_array_t` is already `{sr_value_t* arr; int len;}`, so this is a span
/// with a transform to `value` -- there is no iteration machinery to pay for.
/// Usable in a range-for on every supported standard; models
/// `std::ranges::view` from C++20 so it composes with pipelines.
class array_view
#if SURREALDB_HAS_RANGES
    : public std::ranges::view_interface<array_view>
#endif
{
public:
    class iterator {
    public:
        /// `operator*` yields a `value` by value -- it is a pointer-sized view,
        /// so there is nothing to return a reference to. That makes this a
        /// proxy iterator, and `operator->` needs somewhere for the temporary
        /// to live until the call completes.
        struct arrow_proxy {
            surrealdb::value held;
            [[nodiscard]] const surrealdb::value* operator->() const noexcept { return &held; }
        };

        using iterator_category = std::random_access_iterator_tag;
        using value_type        = surrealdb::value;
        using difference_type   = std::ptrdiff_t;
        using reference         = surrealdb::value;
        using pointer           = arrow_proxy;

        iterator() noexcept = default;
        explicit iterator(const sr_value_t* p) noexcept : p_(p) {}

        [[nodiscard]] reference operator*() const noexcept { return surrealdb::value(p_); }
        [[nodiscard]] pointer operator->() const noexcept {
            return arrow_proxy{surrealdb::value(p_)};
        }
        [[nodiscard]] reference operator[](difference_type n) const noexcept {
            return surrealdb::value(p_ + n);
        }

        iterator& operator++() noexcept { ++p_; return *this; }
        iterator operator++(int) noexcept { iterator t = *this; ++p_; return t; }
        iterator& operator--() noexcept { --p_; return *this; }
        iterator operator--(int) noexcept { iterator t = *this; --p_; return t; }
        iterator& operator+=(difference_type n) noexcept { p_ += n; return *this; }
        iterator& operator-=(difference_type n) noexcept { p_ -= n; return *this; }

        [[nodiscard]] friend iterator operator+(iterator i, difference_type n) noexcept { return i += n; }
        [[nodiscard]] friend iterator operator+(difference_type n, iterator i) noexcept { return i += n; }
        [[nodiscard]] friend iterator operator-(iterator i, difference_type n) noexcept { return i -= n; }
        [[nodiscard]] friend difference_type operator-(iterator a, iterator b) noexcept { return a.p_ - b.p_; }

        [[nodiscard]] friend bool operator==(iterator a, iterator b) noexcept { return a.p_ == b.p_; }
        [[nodiscard]] friend bool operator!=(iterator a, iterator b) noexcept { return a.p_ != b.p_; }
        [[nodiscard]] friend bool operator<(iterator a, iterator b) noexcept { return a.p_ < b.p_; }
        [[nodiscard]] friend bool operator>(iterator a, iterator b) noexcept { return a.p_ > b.p_; }
        [[nodiscard]] friend bool operator<=(iterator a, iterator b) noexcept { return a.p_ <= b.p_; }
        [[nodiscard]] friend bool operator>=(iterator a, iterator b) noexcept { return a.p_ >= b.p_; }

    private:
        const sr_value_t* p_{nullptr};
    };

    using const_iterator = iterator;

    array_view() noexcept = default;
    array_view(const sr_value_t* data, int size) noexcept
        : data_(data), size_(size < 0 ? 0 : size) {}
    explicit array_view(const sr_array_t& arr) noexcept
        : array_view(arr.arr, arr.len) {}

    [[nodiscard]] iterator begin() const noexcept { return iterator(data_); }
    [[nodiscard]] iterator end() const noexcept { return iterator(data_ + size_); }

    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] const sr_value_t* data() const noexcept { return data_; }

    /// Unchecked -- callers bound-check with `size()`. This is the hot path.
    ///
    /// `sr_array_get` and `sr_array_len` are the C equivalents of `at()` and
    /// `size()`, and are the only two exported functions this library never
    /// calls. `sr_array_t` is a plain `{ptr, len}` aggregate, so reading it
    /// directly is a load rather than a call across the FFI boundary -- per
    /// element, over a result set. The behaviour is the same, including
    /// `sr_array_get`'s out-of-bounds null, which `at()` reports as `nullopt`.
    [[nodiscard]] value operator[](int i) const noexcept { return value(data_ + i); }

    /// Bounds-checked alternative. Returns `nullopt` rather than throwing,
    /// since the library is built for `-fno-exceptions`.
    [[nodiscard]] std::optional<value> at(int i) const noexcept {
        if (i < 0 || i >= size_ || data_ == nullptr) return std::nullopt;
        return value(data_ + i);
    }

private:
    const sr_value_t* data_{nullptr};
    int size_{0};
};

// ---------------------------------------------------------------------------
// object_view
// ---------------------------------------------------------------------------

/// A borrowed view of an `sr_object_t`.
///
/// The C object is opaque, so unlike arrays there is no contiguous storage to
/// hand out. Lookup is by key. Enumerating keys allocates on the C side (see
/// `object.hpp`), which is why it is not offered as a plain range here.
class object_view {
public:
    object_view() noexcept = default;
    explicit object_view(const sr_object_t* obj) noexcept : o_(obj) {}

    [[nodiscard]] const sr_object_t* raw() const noexcept { return o_; }
    [[nodiscard]] bool valid() const noexcept { return o_ != nullptr && o_->_0 != nullptr; }

    [[nodiscard]] int size() const noexcept {
        return o_ ? ::sr_object_len(o_) : 0;
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Look up a key. The returned view borrows from this object.
    [[nodiscard]] std::optional<value> get(const char* key) const noexcept {
        if (!o_ || !key) return std::nullopt;
        const sr_value_t* v = ::sr_object_get(o_, key);
        if (!v) return std::nullopt;
        return value(v);
    }

    [[nodiscard]] bool contains(const char* key) const noexcept {
        return get(key).has_value();
    }

private:
    const sr_object_t* o_{nullptr};
};

// ---------------------------------------------------------------------------
// value container accessors
// ---------------------------------------------------------------------------

inline std::optional<array_view> value::as_array() const noexcept {
    if (!is(value_kind::array) || v_->sr_value_array == nullptr) return std::nullopt;
    return array_view(*v_->sr_value_array);
}

inline std::optional<array_view> value::as_set() const noexcept {
    if (!is(value_kind::set) || v_->sr_value_set == nullptr) return std::nullopt;
    return array_view(*v_->sr_value_set);
}

inline std::optional<object_view> value::as_object() const noexcept {
    if (!is(value_kind::object)) return std::nullopt;
    return object_view(&v_->sr_value_object);
}

// ---------------------------------------------------------------------------
// visit
// ---------------------------------------------------------------------------

template <class F>
decltype(auto) value::visit(F&& f) const {
    switch (kind()) {
        case value_kind::none:     return f(none_t{});
        case value_kind::null:     return f(null_t{});
        case value_kind::boolean:  return f(v_->sr_value_bool);
        case value_kind::number:
            switch (v_->sr_value_number.tag) {
                case SR_NUMBER_INT:   return f(v_->sr_value_number.sr_number_int);
                case SR_NUMBER_FLOAT: return f(v_->sr_value_number.sr_number_float);
                case SR_NUMBER_DECIMAL:
                    return f(decimal_ref{sv(v_->sr_value_number.sr_number_decimal)});
            }
            return f(none_t{});
        case value_kind::string:   return f(sv(v_->sr_value_strand));
        case value_kind::duration:
            return f(duration_ref{v_->sr_value_duration.secs, v_->sr_value_duration.nanos});
        case value_kind::datetime: return f(datetime_ref{sv(v_->sr_value_datetime)});
        case value_kind::uuid:     return f(uuid_ref{v_->sr_value_uuid._0});
        case value_kind::array:
            return f(v_->sr_value_array ? array_view(*v_->sr_value_array) : array_view());
        case value_kind::object:   return f(object_view(&v_->sr_value_object));
        case value_kind::geometry: return f(&v_->sr_geometry_object);
        case value_kind::bytes:
            return f(bytes_ref{v_->sr_value_bytes.arr, v_->sr_value_bytes.len});
        case value_kind::thing:    return f(&v_->sr_value_thing);
        case value_kind::table:    return f(table_ref{sv(v_->sr_value_table)});
        case value_kind::file:
            return f(file_ref{sv(v_->sr_value_file.bucket), sv(v_->sr_value_file.key)});
        case value_kind::range:    return f(v_->sr_value_range);
        case value_kind::regex:    return f(regex_ref{sv(v_->sr_value_regex)});
        case value_kind::set:
            return f(v_->sr_value_set ? array_view(*v_->sr_value_set) : array_view());
    }
    return f(none_t{});
}

} // namespace surrealdb

#if SURREALDB_HAS_RANGES
// Opt in to the borrowed-range concept: an array_view does not own its
// elements, so a dangling check against it would be a false positive.
template <>
inline constexpr bool std::ranges::enable_borrowed_range<surrealdb::array_view> = true;
#endif
