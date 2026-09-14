#pragma once

/// @file geometry.hpp
/// Reading geometry and record ids.
///
/// `value::as_geometry()` and `value::as_thing()` used to hand back a
/// `const sr_geometry_t*` and a `const sr_thing_t*` -- tagged C unions the
/// caller had to switch on and reach into by hand, with the field names
/// (`_0`, `_1`, `sr_geometry_multiline`) as the only guide. Everything here
/// exists to make that unnecessary.
///
/// All of it is a *view*. Nothing owns; every span points into the value it
/// came from, so the value has to outlive whatever you read out of it -- the
/// same rule as `array_view` over `owned_values`.
///
/// The accessors return `std::optional`, so bind before you iterate:
///
///     if (auto members = g.as_collection())
///         for (geometry_ref m : *members) ...
///
/// A range-for straight over `*g.as_collection()` binds to a reference into an
/// optional temporary that dies at the semicolon. It cannot be made a compile
/// error -- range-for lvalue-izes its range -- but clang's `-Wdangling-gsl`
/// catches it, which is one reason the suites are swept under both compilers.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "value.hpp"

#include <cstdint>
#include <iterator>

#if SURREALDB_HAS_RANGES
#  include <ranges>
#endif
#if SURREALDB_HAS_SPAN
#  include <span>
#endif
#include <optional>
#include <string_view>
#include <variant>

namespace surrealdb {

/// A longitude/latitude pair, layout-identical to the C type.
using coord = sr_g_coord;

class geometry_ref;

namespace detail {

/// A borrowed run of `C`, handed out one `Ref` at a time.
///
/// The C geometry types are all `{ptr, len}` pairs of increasingly nested
/// structs, so one template covers coordinates, points, rings, polygons and
/// nested geometries alike rather than six near-identical classes.
template <class C, class Ref>
class geom_span
#if SURREALDB_HAS_RANGES
    : public std::ranges::view_interface<geom_span<C, Ref>>
#endif
{
public:
    geom_span() noexcept : p_(nullptr), n_(0) {}
    geom_span(const C* p, int n) noexcept : p_(p), n_(p && n > 0 ? n : 0) {}

    [[nodiscard]] int size() const noexcept { return n_; }
    [[nodiscard]] bool empty() const noexcept { return n_ == 0; }
    [[nodiscard]] const C* data() const noexcept { return p_; }

    /// Unchecked; bound-check with `size()`. This is a read loop.
    [[nodiscard]] Ref operator[](int i) const noexcept { return Ref(p_[i]); }

    [[nodiscard]] std::optional<Ref> at(int i) const noexcept {
        if (i < 0 || i >= n_) return std::nullopt;
        return Ref(p_[i]);
    }

    class iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type        = Ref;
        using difference_type   = std::ptrdiff_t;
        using reference         = Ref;
        using pointer           = const C*;

        iterator() noexcept = default;
        explicit iterator(const C* p) noexcept : p_(p) {}

        [[nodiscard]] Ref operator*() const noexcept { return Ref(*p_); }

        iterator& operator++() noexcept { ++p_; return *this; }
        iterator operator++(int) noexcept { iterator t = *this; ++p_; return t; }
        iterator& operator--() noexcept { --p_; return *this; }
        iterator& operator+=(difference_type n) noexcept { p_ += n; return *this; }
        iterator& operator-=(difference_type n) noexcept { p_ -= n; return *this; }

        [[nodiscard]] friend iterator operator+(iterator i, difference_type n) noexcept { return i += n; }
        [[nodiscard]] friend iterator operator-(iterator i, difference_type n) noexcept { return i -= n; }
        [[nodiscard]] friend difference_type operator-(iterator a, iterator b) noexcept { return a.p_ - b.p_; }
        [[nodiscard]] friend bool operator==(iterator a, iterator b) noexcept { return a.p_ == b.p_; }
        [[nodiscard]] friend bool operator!=(iterator a, iterator b) noexcept { return a.p_ != b.p_; }

    private:
        const C* p_{nullptr};
    };

    using const_iterator = iterator;
    [[nodiscard]] iterator begin() const noexcept { return iterator(p_); }
    [[nodiscard]] iterator end() const noexcept { return iterator(p_ + n_); }

#if SURREALDB_HAS_SPAN
    /// The underlying C structs as a `std::span`, for handing a contiguous run
    /// straight to a geometry library without copying.
    ///
    /// This is the raw element type, not `Ref` -- for coordinates the two are
    /// the same, and for the nested shapes it is the C struct, which is what a
    /// caller reaching for a span wants anyway.
    [[nodiscard]] std::span<const C> as_span() const noexcept {
        return std::span<const C>(p_, static_cast<std::size_t>(n_));
    }
#endif

private:
    const C* p_;
    int n_;
};

} // namespace detail

/// One point of a multipoint.
///
/// A distinct type from `coord` only because the C side wraps each one in an
/// `sr_g_point`; it converts to `coord` implicitly, so a multipoint reads the
/// same way a linestring does.
struct point_ref {
    point_ref() = default;
    point_ref(const sr_g_point& p) noexcept : value(p._0) {}   // NOLINT: span glue
    point_ref(const sr_g_coord& c) noexcept : value(c) {}      // NOLINT: span glue

    operator coord() const noexcept { return value; }          // NOLINT: deliberate
    [[nodiscard]] double x() const noexcept { return value.x; }
    [[nodiscard]] double y() const noexcept { return value.y; }

    coord value{};
};

using coord_span = detail::geom_span<sr_g_coord, coord>;

/// A line: an ordered run of coordinates.
class linestring_ref {
public:
    linestring_ref() noexcept : l_{} {}
    linestring_ref(const sr_g_linestring& l) noexcept : l_(l) {}   // NOLINT: span glue

    [[nodiscard]] coord_span coords() const noexcept {
        return coord_span(l_._0.ptr, l_._0.len);
    }
    [[nodiscard]] int size() const noexcept { return coords().size(); }
    [[nodiscard]] bool empty() const noexcept { return coords().empty(); }
    [[nodiscard]] coord operator[](int i) const noexcept { return coords()[i]; }

    [[nodiscard]] coord_span::iterator begin() const noexcept { return coords().begin(); }
    [[nodiscard]] coord_span::iterator end() const noexcept { return coords().end(); }

private:
    sr_g_linestring l_;
};

using linestring_span = detail::geom_span<sr_g_linestring, linestring_ref>;
using point_span      = detail::geom_span<sr_g_point, point_ref>;

/// A polygon: one exterior ring, plus a ring per hole.
///
/// Note the asymmetry with construction: `make::polygon` takes a single ring,
/// because `sr_value_polygon` does. Reading can hand back holes that the
/// constructors cannot produce.
class polygon_ref {
public:
    polygon_ref() noexcept : p_{} {}
    polygon_ref(const sr_g_polygon& p) noexcept : p_(p) {}         // NOLINT: span glue

    [[nodiscard]] linestring_ref exterior() const noexcept { return linestring_ref(p_._0); }
    [[nodiscard]] linestring_span interiors() const noexcept {
        return linestring_span(p_._1.ptr, p_._1.len);
    }
    [[nodiscard]] bool has_holes() const noexcept { return !interiors().empty(); }

private:
    sr_g_polygon p_;
};

using polygon_span  = detail::geom_span<sr_g_polygon, polygon_ref>;
using geometry_span = detail::geom_span<sr_geometry_t, geometry_ref>;

/// Which of the seven shapes a geometry is.
enum class geometry_kind : int {
    point         = SR_GEOMETRY_POINT,
    linestring    = SR_GEOMETRY_LINESTRING,
    polygon       = SR_GEOMETRY_POLYGON,
    multipoint    = SR_GEOMETRY_MULTIPOINT,
    multilinestring = SR_GEOMETRY_MULTILINE,
    multipolygon  = SR_GEOMETRY_MULTIPOLYGON,
    collection    = SR_GEOMETRY_COLLECTION,
    /// A shape added in a newer SurrealDB than this C API knows. Readable as
    /// "something is here", nothing more -- which is why it is not an error.
    unknown       = SR_GEOMETRY_UNIMPLEMENTED,
};

[[nodiscard]] constexpr const char* to_string(geometry_kind k) noexcept {
    switch (k) {
        case geometry_kind::point:           return "point";
        case geometry_kind::linestring:      return "linestring";
        case geometry_kind::polygon:         return "polygon";
        case geometry_kind::multipoint:      return "multipoint";
        case geometry_kind::multilinestring: return "multilinestring";
        case geometry_kind::multipolygon:    return "multipolygon";
        case geometry_kind::collection:      return "collection";
        case geometry_kind::unknown:         return "unknown";
    }
    return "unknown";
}

/// A geometry, read.
///
/// Each accessor returns `nullopt` unless the geometry is that shape, so a
/// wrong guess is empty rather than garbage -- the same contract as `value`'s
/// typed accessors.
class geometry_ref {
public:
    geometry_ref() noexcept : g_(nullptr) {}
    explicit geometry_ref(const sr_geometry_t* g) noexcept : g_(g) {}
    geometry_ref(const sr_geometry_t& g) noexcept : g_(&g) {}       // NOLINT: span glue

    [[nodiscard]] bool valid() const noexcept { return g_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] geometry_kind kind() const noexcept {
        return g_ ? static_cast<geometry_kind>(g_->tag) : geometry_kind::unknown;
    }
    [[nodiscard]] bool is(geometry_kind k) const noexcept { return g_ && kind() == k; }

    [[nodiscard]] std::optional<coord> as_point() const noexcept {
        if (!is(geometry_kind::point)) return std::nullopt;
        return g_->sr_geometry_point._0;
    }

    [[nodiscard]] std::optional<linestring_ref> as_linestring() const noexcept {
        if (!is(geometry_kind::linestring)) return std::nullopt;
        return linestring_ref(g_->sr_geometry_linestring);
    }

    [[nodiscard]] std::optional<polygon_ref> as_polygon() const noexcept {
        if (!is(geometry_kind::polygon)) return std::nullopt;
        return polygon_ref(g_->sr_geometry_polygon);
    }

    [[nodiscard]] std::optional<point_span> as_multipoint() const noexcept {
        if (!is(geometry_kind::multipoint)) return std::nullopt;
        const auto& a = g_->sr_geometry_multipoint._0;
        return point_span(a.ptr, a.len);
    }

    [[nodiscard]] std::optional<linestring_span> as_multilinestring() const noexcept {
        if (!is(geometry_kind::multilinestring)) return std::nullopt;
        const auto& a = g_->sr_geometry_multiline._0;
        return linestring_span(a.ptr, a.len);
    }

    [[nodiscard]] std::optional<polygon_span> as_multipolygon() const noexcept {
        if (!is(geometry_kind::multipolygon)) return std::nullopt;
        const auto& a = g_->sr_geometry_multipolygon._0;
        return polygon_span(a.ptr, a.len);
    }

    /// The members of a collection, each itself a geometry -- collections nest.
    [[nodiscard]] std::optional<geometry_span> as_collection() const noexcept {
        if (!is(geometry_kind::collection)) return std::nullopt;
        const auto& a = g_->sr_geometry_collection;
        return geometry_span(a.ptr, a.len);
    }

    [[nodiscard]] const sr_geometry_t* raw() const noexcept { return g_; }

    /// Dispatch on the shape.
    ///
    /// The visitor is called with a `coord`, `linestring_ref`, `polygon_ref`,
    /// `point_span`, `linestring_span`, `polygon_span`, `geometry_span`, or --
    /// for a shape this C API does not know -- `std::monostate`. Add an
    /// overload for that last one and a future SurrealDB release will not
    /// silently take a branch you meant for something else.
    template <class F>
    decltype(auto) visit(F&& f) const {
        switch (kind()) {
            case geometry_kind::point:
                return f(g_->sr_geometry_point._0);
            case geometry_kind::linestring:
                return f(linestring_ref(g_->sr_geometry_linestring));
            case geometry_kind::polygon:
                return f(polygon_ref(g_->sr_geometry_polygon));
            case geometry_kind::multipoint:
                return f(point_span(g_->sr_geometry_multipoint._0.ptr,
                                    g_->sr_geometry_multipoint._0.len));
            case geometry_kind::multilinestring:
                return f(linestring_span(g_->sr_geometry_multiline._0.ptr,
                                         g_->sr_geometry_multiline._0.len));
            case geometry_kind::multipolygon:
                return f(polygon_span(g_->sr_geometry_multipolygon._0.ptr,
                                      g_->sr_geometry_multipolygon._0.len));
            case geometry_kind::collection:
                return f(geometry_span(g_->sr_geometry_collection.ptr,
                                       g_->sr_geometry_collection.len));
            case geometry_kind::unknown:
                break;
        }
        return f(std::monostate{});
    }

private:
    const sr_geometry_t* g_;
};

// ---------------------------------------------------------------------------
// Record ids
// ---------------------------------------------------------------------------

/// What kind of id a record has.
///
/// All four arrive from the database. Only `text` has a direct constructor
/// (`make::thing`); the others are produced with `type::record($tb, $id)` and a
/// bound value.
enum class id_kind : int {
    number = SR_ID_NUMBER,
    text   = SR_ID_STRING,
    array  = SR_ID_ARRAY,
    object = SR_ID_OBJECT,
};

[[nodiscard]] constexpr const char* to_string(id_kind k) noexcept {
    switch (k) {
        case id_kind::number: return "number";
        case id_kind::text:   return "text";
        case id_kind::array:  return "array";
        case id_kind::object: return "object";
    }
    return "unknown";
}

/// A record id: a table name and an id that is one of four things.
class thing_ref {
public:
    thing_ref() noexcept : t_(nullptr) {}
    explicit thing_ref(const sr_thing_t* t) noexcept : t_(t) {}

    [[nodiscard]] bool valid() const noexcept { return t_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] std::string_view table() const noexcept {
        if (!t_ || !t_->table) return {};
        return std::string_view(t_->table);
    }

    [[nodiscard]] id_kind kind() const noexcept {
        return t_ ? static_cast<id_kind>(t_->id.tag) : id_kind::text;
    }
    [[nodiscard]] bool is(id_kind k) const noexcept { return t_ && kind() == k; }

    [[nodiscard]] std::optional<std::int64_t> as_number() const noexcept {
        if (!is(id_kind::number)) return std::nullopt;
        return t_->id.sr_id_number;
    }

    [[nodiscard]] std::optional<std::string_view> as_text() const noexcept {
        if (!is(id_kind::text) || !t_->id.sr_id_string) return std::nullopt;
        return std::string_view(t_->id.sr_id_string);
    }

    [[nodiscard]] std::optional<array_view> as_array() const noexcept {
        if (!is(id_kind::array) || !t_->id.sr_id_array) return std::nullopt;
        return array_view(*t_->id.sr_id_array);
    }

    [[nodiscard]] std::optional<object_view> as_object() const noexcept {
        if (!is(id_kind::object)) return std::nullopt;
        return object_view(&t_->id.sr_id_object);
    }

    [[nodiscard]] const sr_thing_t* raw() const noexcept { return t_; }

    /// Dispatch on the id kind: `std::int64_t`, `std::string_view`,
    /// `array_view` or `object_view`.
    template <class F>
    decltype(auto) visit(F&& f) const {
        switch (kind()) {
            case id_kind::number: return f(t_->id.sr_id_number);
            case id_kind::array:
                return f(t_->id.sr_id_array ? array_view(*t_->id.sr_id_array) : array_view());
            case id_kind::object: return f(object_view(&t_->id.sr_id_object));
            case id_kind::text:   break;
        }
        return f(t_ && t_->id.sr_id_string ? std::string_view(t_->id.sr_id_string)
                                           : std::string_view());
    }

private:
    const sr_thing_t* t_;
};

// ---------------------------------------------------------------------------
// Reading them off a value
// ---------------------------------------------------------------------------
//
// Free functions rather than members on `value`, because `value` is declared in
// value.hpp and these types are built on top of it. Same spelling as `view()`
// for arrays and objects.

/// The geometry in a value, or an invalid `geometry_ref` if it is not one.
[[nodiscard]] inline geometry_ref geometry(value v) noexcept {
    auto raw = v.as_geometry_raw();
    return raw ? geometry_ref(*raw) : geometry_ref();
}

/// The record id in a value, or an invalid `thing_ref` if it is not one.
[[nodiscard]] inline thing_ref thing(value v) noexcept {
    auto raw = v.as_thing_raw();
    return raw ? thing_ref(*raw) : thing_ref();
}

} // namespace surrealdb

#if SURREALDB_HAS_RANGES
// These borrow: every span points into the value it was read from, never at
// storage of its own, so a dangling check against one is a false positive. The
// same opt-in `array_view` takes.
template <class C, class Ref>
inline constexpr bool std::ranges::enable_borrowed_range<
    surrealdb::detail::geom_span<C, Ref>> = true;

template <>
inline constexpr bool std::ranges::enable_borrowed_range<surrealdb::linestring_ref> = true;
#endif
