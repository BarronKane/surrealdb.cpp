#pragma once

/// @file make.hpp
/// Constructing values.
///
/// `value.hpp` only reads; this is the other half. Every constructor returns an
/// `owned_value`, so ownership is stated at the call site and there is no
/// builder object to forget to release.
///
/// Geometry and ranges are only reachable from here -- an object builder can
/// take a `value`, but nothing else could produce a polygon or a range to give
/// it.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/owned.hpp"
#include "array.hpp"
#include "object.hpp"
#include "value.hpp"

#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

namespace surrealdb {
namespace make {

// -- scalars -----------------------------------------------------------------

/// Absence of a value. Distinct from `null`.
[[nodiscard]] inline owned_value none() { return owned_value(::sr_value_none()); }
/// An explicit null. Distinct from `none`.
[[nodiscard]] inline owned_value null() { return owned_value(::sr_value_null()); }

[[nodiscard]] inline owned_value boolean(bool v) { return owned_value(::sr_value_bool(v)); }
[[nodiscard]] inline owned_value integer(std::int64_t v) { return owned_value(::sr_value_int(v)); }
[[nodiscard]] inline owned_value floating(double v) { return owned_value(::sr_value_float(v)); }

/// An exact decimal. Carried as text so precision survives the trip -- which is
/// why this takes a string rather than a `double`.
[[nodiscard]] inline owned_value decimal(const char* text) {
    return owned_value(::sr_value_decimal(text));
}
[[nodiscard]] inline owned_value decimal(const std::string& text) { return decimal(text.c_str()); }

[[nodiscard]] inline owned_value string(const char* v) { return owned_value(::sr_value_string(v)); }
[[nodiscard]] inline owned_value string(const std::string& v) { return string(v.c_str()); }

/// RFC 3339 text.
[[nodiscard]] inline owned_value datetime(const char* rfc3339) {
    return owned_value(::sr_value_datetime(rfc3339));
}
[[nodiscard]] inline owned_value duration(std::uint64_t seconds, std::uint32_t nanoseconds = 0) {
    return owned_value(::sr_value_duration(seconds, nanoseconds));
}

/// Sixteen raw bytes. Taken as a sized array so a short buffer cannot be passed
/// by accident -- the C function reads 16 regardless.
[[nodiscard]] inline owned_value uuid(const std::uint8_t (&bytes)[16]) {
    return owned_value(::sr_value_uuid(bytes));
}

[[nodiscard]] inline owned_value bytes(const std::uint8_t* data, int len) {
    return owned_value(::sr_value_bytes(data, len));
}
template <class Container>
[[nodiscard]] owned_value bytes(const Container& c) {
    return bytes(c.data(), static_cast<int>(c.size()));
}

/// A table name as a value, as distinct from a table named in a statement.
[[nodiscard]] inline owned_value table(const char* name) { return owned_value(::sr_value_table(name)); }
/// A regular expression, given as its source pattern.
[[nodiscard]] inline owned_value regex(const char* pattern) { return owned_value(::sr_value_regex(pattern)); }
/// A file in a bucket. Reading or writing one needs the `files` capability.
[[nodiscard]] inline owned_value file(const char* bucket, const char* key) {
    return owned_value(::sr_value_file(bucket, key));
}
// -- record ids --------------------------------------------------------------
//
// A record id key has four shapes, and all four turn up in ordinary use:
//
//     CREATE t:abc        -> text      CREATE t:['a', 1]  -> array
//     CREATE t:1          -> number    CREATE t:{ x: 1 }  -> object
//
// `thing_ref` reads all four and dispatches on them; for two releases only the
// text one could be written. That was not a missing convenience. A key of the
// wrong shape is a *well-formed* record id that refers to nothing, and the
// database answers a query against it with zero rows and no error -- so the
// mistake is silent at every layer. `t:1` and `t:"1"` are different records.
//
// Each overload here is named by the shape it writes, so choosing wrongly is a
// thing you can see at the call site rather than a result set that is quietly
// empty.

/// A record id with a text key: `table:abc`.
///
/// **Not the one to reach for by default.** If the record was created with
/// `CREATE t:1` its key is a number and this will not find it; see the note
/// above and `thing(table, std::int64_t)`.
[[nodiscard]] inline owned_value thing(const char* table_name, const char* id) {
    return owned_value(::sr_value_thing(table_name, id));
}

/// A record id with a text key, from a `std::string`.
[[nodiscard]] inline owned_value thing(const char* table_name, const std::string& id) {
    return thing(table_name, id.c_str());
}

/// A record id with a numeric key: `table:1`.
///
/// A template over the integral types rather than a fixed `std::int64_t`
/// overload, for the reason `object_builder::set` is one: with `const char*`
/// also in the set, `thing("t", 0)` would otherwise be ambiguous between a
/// numeric key and a null string. An exact integral match wins outright.
template <class T,
          std::enable_if_t<std::is_integral<T>::value &&
                               !std::is_same<T, bool>::value,
                           int> = 0>
[[nodiscard]] owned_value thing(const char* table_name, T id) {
    return owned_value(
        ::sr_value_thing_num(table_name, static_cast<std::int64_t>(id)));
}

/// A record id with an array key: `table:['a', 1]`.
///
/// The key is copied, so whatever produced it stays yours.
[[nodiscard]] inline owned_value thing(const char* table_name, array_arg id) {
    return owned_value(::sr_value_thing_arr(table_name, id.raw()));
}

/// A record id with an object key: `table:{ x: 1 }`.
///
/// The key is copied. Takes anything an object reaches this library as --
/// including the `object_view` off a `thing_ref::as_object()`, which is how a
/// composite id read out of a result gets echoed back.
[[nodiscard]] inline owned_value thing(const char* table_name, object_arg id) {
    return owned_value(::sr_value_thing_obj(table_name, id.raw()));
}

/// Wrap an object. The object is copied, so the source stays usable.
///
/// Takes an `object_arg`, so a builder, a view, an `owned_object` off
/// `create()`, or a raw `const sr_object_t*` all work -- the three-overload
/// shape `array()` and `set()` have had since 0.2.3, which objects lacked. A
/// library whose most common return type could not be fed back into its own
/// inputs is one you can only read out of.
[[nodiscard]] inline owned_value object(object_arg o) {
    return owned_value(::sr_value_object(o.raw()));
}

/// An empty array.
[[nodiscard]] inline owned_value array() { return owned_value(::sr_value_array(nullptr)); }
/// An empty set. Distinct from an array: elements are unique.
[[nodiscard]] inline owned_value set() { return owned_value(::sr_value_set(nullptr)); }

/// An array value with contents.
///
/// This is the bridge that makes a list usable as *data*: until surrealdb.c
/// 0.2.3 the only array value that could be built was an empty one, so a
/// populated list could not be bound to a query variable or stored in a record
/// field -- it had to be written into the SurrealQL text, which is exactly what
/// binding exists to avoid. `sr_value_array` now mirrors `sr_value_object`.
///
/// The array is copied, so the builder stays yours and stays usable.
[[nodiscard]] inline owned_value array(array_arg arr) {
    return owned_value(::sr_value_array(arr.raw()));
}

/// A set value with contents. Duplicates are discarded when it reaches the
/// database, which is what makes it different from an array.
[[nodiscard]] inline owned_value set(array_arg arr) {
    return owned_value(::sr_value_set(arr.raw()));
}

// -- geometry ----------------------------------------------------------------

/// A longitude/latitude pair. Layout-identical to the C type, so a container of
/// these can be handed over without a conversion pass.
using coord = sr_g_coord;

static_assert(sizeof(coord) == 2 * sizeof(double), "coord must match sr_g_coord");

[[nodiscard]] inline owned_value point(double x, double y) {
    return owned_value(::sr_value_point(x, y));
}

[[nodiscard]] inline owned_value linestring(const coord* coords, int len) {
    return owned_value(::sr_value_linestring(coords, len));
}
[[nodiscard]] inline owned_value polygon(const coord* coords, int len) {
    return owned_value(::sr_value_polygon(coords, len));
}
[[nodiscard]] inline owned_value multipoint(const coord* coords, int len) {
    return owned_value(::sr_value_multipoint(coords, len));
}

namespace detail {

/// Members arrive as either borrowed views or owned handles; both reduce to the
/// same pointer, and overloading here keeps the container templates from caring
/// which they were given.
inline const sr_value_t* member_ptr(value v) noexcept { return v.raw(); }
inline const sr_value_t* member_ptr(const owned_value& v) noexcept { return v.get(); }

template <class T>
using range_elem_t = typename std::remove_cv<typename std::remove_reference<
    decltype(*std::begin(std::declval<const T&>()))>::type>::type;

/// True when `T` iterates coordinates -- a single ring.
template <class T, class = void>
struct is_coord_range : std::false_type {};
template <class T>
struct is_coord_range<T, typename std::enable_if<
    std::is_convertible<range_elem_t<T>, coord>::value>::type> : std::true_type {};

/// True when `T` iterates values -- geometry values, for the composing
/// constructors.
template <class T, class = void>
struct is_value_range : std::false_type {};
template <class T>
struct is_value_range<T, typename std::enable_if<
    std::is_convertible<range_elem_t<T>, value>::value ||
    std::is_same<range_elem_t<T>, owned_value>::value>::type> : std::true_type {};

} // namespace detail

/// Container overloads, for anything contiguous -- `std::vector<coord>`, a raw
/// array, `std::array`.
template <class C> [[nodiscard]] owned_value linestring(const C& c) {
    return linestring(c.data(), static_cast<int>(c.size()));
}
template <class C> [[nodiscard]] owned_value multipoint(const C& c) {
    return multipoint(c.data(), static_cast<int>(c.size()));
}

/// A polygon from one ring, the common case.
template <class C,
          typename std::enable_if<detail::is_coord_range<C>::value, int>::type = 0>
[[nodiscard]] owned_value polygon(const C& ring) {
    return polygon(ring.data(), static_cast<int>(ring.size()));
}

/// A polygon with holes: rings, exterior first, then one per hole.
///
/// This is GeoJSON's `coordinates` shape for a Polygon, so a caller holding
/// GeoJSON passes it through unchanged. The single-ring overload above is
/// shorthand and cannot express a hole -- which is which is decided by whether
/// the container yields coordinates or yields rings, so both spellings are just
/// `polygon(...)`.
///
/// Needs surrealdb.c 0.2.5.
template <class Outer,
          typename std::enable_if<!detail::is_coord_range<Outer>::value, int>::type = 0>
[[nodiscard]] owned_value polygon(const Outer& rings) {
    std::vector<const coord*> ptrs;
    std::vector<int> lens;
    ptrs.reserve(rings.size());
    lens.reserve(rings.size());
    for (const auto& r : rings) {
        ptrs.push_back(r.data());
        lens.push_back(static_cast<int>(r.size()));
    }
    if (ptrs.empty()) return owned_value(::sr_value_polygon_rings(nullptr, nullptr, 0));
    return owned_value(
        ::sr_value_polygon_rings(ptrs.data(), lens.data(), static_cast<int>(ptrs.size())));
}

/// Several linestrings.
///
/// The C entry point wants two parallel arrays -- a pointer per ring and a
/// length per ring -- which is exactly the shape nobody wants to assemble by
/// hand. Pass a container of containers and this builds them.
template <class Outer>
[[nodiscard]] owned_value multilinestring(const Outer& rings) {
    std::vector<const coord*> ptrs;
    std::vector<int> lens;
    ptrs.reserve(rings.size());
    lens.reserve(rings.size());
    for (const auto& r : rings) {
        ptrs.push_back(r.data());
        lens.push_back(static_cast<int>(r.size()));
    }
    if (ptrs.empty()) return owned_value(::sr_value_multilinestring(nullptr, nullptr, 0));
    return owned_value(
        ::sr_value_multilinestring(ptrs.data(), lens.data(), static_cast<int>(ptrs.size())));
}

/// Several polygons, each a single ring, in the same shape as `multilinestring`.
///
/// None of the members can have a hole; pass built polygon values instead when
/// any of them does.
template <class Outer,
          typename std::enable_if<!detail::is_value_range<Outer>::value, int>::type = 0>
[[nodiscard]] owned_value multipolygon(const Outer& polys) {
    std::vector<const coord*> ptrs;
    std::vector<int> lens;
    ptrs.reserve(polys.size());
    lens.reserve(polys.size());
    for (const auto& p : polys) {
        ptrs.push_back(p.data());
        lens.push_back(static_cast<int>(p.size()));
    }
    if (ptrs.empty()) return owned_value(::sr_value_multipolygon(nullptr, nullptr, 0));
    return owned_value(
        ::sr_value_multipolygon(ptrs.data(), lens.data(), static_cast<int>(ptrs.size())));
}

/// Several polygons, composed from polygon *values* -- so any of them may have
/// holes.
///
/// **Every member must be a Polygon.** Anything else yields a `none` value
/// rather than a malformed multipolygon, the same refusal `collection()` makes.
///
/// Needs surrealdb.c 0.2.5.
template <class Container,
          typename std::enable_if<detail::is_value_range<Container>::value, int>::type = 0>
[[nodiscard]] owned_value multipolygon(const Container& polys) {
    std::vector<const sr_value_t*> ptrs;
    ptrs.reserve(polys.size());
    for (const auto& p : polys) ptrs.push_back(detail::member_ptr(p));
    if (ptrs.empty()) return owned_value(::sr_value_multipolygon_from(nullptr, 0));
    return owned_value(
        ::sr_value_multipolygon_from(ptrs.data(), static_cast<int>(ptrs.size())));
}

/// A GeometryCollection, from geometry values.
///
/// The members are copied, so the values you pass stay yours and stay usable.
///
/// **Every member must be a geometry.** Hand it anything else -- a string, a
/// none, an object shaped like GeoJSON -- and the result is a `none` value
/// rather than a malformed collection, which is the C library refusing to build
/// something that would misbehave later. Check the kind if the members came
/// from somewhere you do not control:
///
///     auto c = make::collection({view(a), view(b)});
///     if (value(c.get()).kind() != value_kind::geometry) { /* a member was not */ }
///
/// Note that a GeoJSON *object* is not a geometry and never becomes one by
/// binding: SurrealDB coerces `{type:'Point', coordinates:[1,2]}` only when it
/// is written as a literal in query text. Build members with `point()`,
/// `polygon()` and the rest.
[[nodiscard]] inline owned_value collection(const sr_value_t* const* geoms, int len) {
    return owned_value(::sr_value_collection(geoms, len));
}

/// An empty collection.
[[nodiscard]] inline owned_value collection() {
    return owned_value(::sr_value_collection(nullptr, 0));
}

/// From any container of `value` or `owned_value`.
template <class Container>
[[nodiscard]] owned_value collection(const Container& geoms) {
    std::vector<const sr_value_t*> ptrs;
    ptrs.reserve(geoms.size());
    for (const auto& g : geoms) ptrs.push_back(detail::member_ptr(g));
    if (ptrs.empty()) return collection();
    return collection(ptrs.data(), static_cast<int>(ptrs.size()));
}

/// From a brace-enclosed list, which is how most call sites read.
[[nodiscard]] inline owned_value collection(std::initializer_list<value> geoms) {
    std::vector<const sr_value_t*> ptrs;
    ptrs.reserve(geoms.size());
    for (value g : geoms) ptrs.push_back(g.raw());
    if (ptrs.empty()) return collection();
    return collection(ptrs.data(), static_cast<int>(ptrs.size()));
}

// -- ranges ------------------------------------------------------------------

/// One end of a range.
///
/// **A bound takes ownership of the value it is built from** -- the C function
/// reclaims the box -- so `included()` and `excluded()` consume an
/// `owned_value` rather than borrowing one. A bound that is never handed to
/// `range()` releases the value itself, so an abandoned bound does not leak.
class bound {
public:
    bound() noexcept : b_(::sr_bound_unbounded()), armed_(false) {}

    bound(bound&& other) noexcept : b_(other.b_), armed_(other.armed_) {
        other.armed_ = false;
        other.b_ = ::sr_bound_unbounded();
    }
    bound& operator=(bound&& other) noexcept {
        if (this != &other) {
            release();
            b_ = other.b_;
            armed_ = other.armed_;
            other.armed_ = false;
            other.b_ = ::sr_bound_unbounded();
        }
        return *this;
    }
    bound(const bound&) = delete;
    bound& operator=(const bound&) = delete;

    ~bound() { release(); }

    [[nodiscard]] sr_bound_t take() noexcept {
        armed_ = false;
        sr_bound_t b = b_;
        b_ = ::sr_bound_unbounded();
        return b;
    }

private:
    friend bound unbounded();
    friend bound included(owned_value);
    friend bound excluded(owned_value);
    friend owned_value range(bound, bound);

    bound(sr_bound_t b, bool armed) noexcept : b_(b), armed_(armed) {}

    /// Free the held value if this bound was never consumed. The value lives
    /// behind whichever union arm the tag names; an unbounded bound holds none.
    void release() noexcept {
        if (!armed_) return;
        armed_ = false;
        if (b_.tag == SR_BOUND_INCLUDED && b_.sr_bound_included) {
            ::sr_value_free(b_.sr_bound_included);
        } else if (b_.tag == SR_BOUND_EXCLUDED && b_.sr_bound_excluded) {
            ::sr_value_free(b_.sr_bound_excluded);
        }
        b_ = ::sr_bound_unbounded();
    }

    sr_bound_t b_;
    bool armed_;
};

/// Open at this end.
[[nodiscard]] inline bound unbounded() { return bound(::sr_bound_unbounded(), false); }
/// Closed: the range includes this value.
[[nodiscard]] inline bound included(owned_value v) {
    return bound(::sr_bound_included(v.release()), true);
}
/// Half-open: the range stops before this value.
[[nodiscard]] inline bound excluded(owned_value v) {
    return bound(::sr_bound_excluded(v.release()), true);
}

/// A range between two bounds. Consumes both.
[[nodiscard]] inline owned_value range(bound start, bound end) {
    return owned_value(::sr_value_range(start.take(), end.take()));
}

} // namespace make

// -- comparison and debugging -------------------------------------------------

/// Structural equality, as SurrealDB defines it.
[[nodiscard]] inline bool operator==(value lhs, value rhs) noexcept {
    if (!lhs.valid() || !rhs.valid()) return lhs.valid() == rhs.valid();
    return ::sr_value_eq(lhs.raw(), rhs.raw());
}
[[nodiscard]] inline bool operator!=(value lhs, value rhs) noexcept { return !(lhs == rhs); }

/// Print a value to stdout, for debugging. The format is the C library's and is
/// not a stable interface.
inline void debug_print(value v) noexcept {
    if (v.valid()) ::sr_value_print(v.raw());
}

/// Print a notification to stdout, for debugging. Same caveat.
inline void debug_print(const sr_notification_t& n) noexcept { ::sr_print_notification(&n); }

} // namespace surrealdb
