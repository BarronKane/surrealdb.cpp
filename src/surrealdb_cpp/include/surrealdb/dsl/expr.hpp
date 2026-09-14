#pragma once

/// @file dsl/expr.hpp
/// Condition expressions for the fluent surface.
///
/// Expression templates, so `f("age") > 30 && f("city") == "Austin"` builds a
/// type rather than a data structure: no allocation, no vector, and the whole
/// shape is known at compile time. Rendering walks that type and emits into the
/// arena through the same binding path the guards use, so a value is bound as
/// `$vN` here exactly as it is there.
///
/// This is why the fluent surface cannot build a query whose shape is decided
/// at runtime: each operator yields a *different type*, so conditions cannot be
/// accumulated in a loop or behind an `if`. That case belongs to the guards in
/// guards.hpp, which is not a limitation being worked around -- it is the line
/// between the two surfaces.

#include "../config.hpp"
#include "arena.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace surrealdb {
namespace dsl {

/// A field reference.
struct field_ref {
    std::string_view name;
};

/// Name a field. The short spelling is deliberate; it appears in every
/// condition.
[[nodiscard]] inline constexpr field_ref f(std::string_view name) noexcept {
    return field_ref{name};
}

/// `field OP value`.
template <class T>
struct comparison {
    field_ref lhs;
    const char* op;
    T rhs;

    void render(arena& a) const {
        a.identifier(lhs.name);
        a.space();
        a.put(op);
        a.space();
        a.bind(rhs);
    }
};

/// Two conditions joined, parenthesised so precedence survives the trip.
template <class L, class R>
struct junction {
    L lhs;
    R rhs;
    const char* joiner;

    void render(arena& a) const {
        a.put('(');
        lhs.render(a);
        a.space();
        a.put(joiner);
        a.space();
        rhs.render(a);
        a.put(')');
    }
};

/// A fragment written verbatim, for operators this surface does not name.
///
/// Nothing here is bound, so caller-supplied data must not reach it -- use a
/// comparison for that.
struct raw_expr {
    std::string_view text;
    void render(arena& a) const { a.put(text); }
};

[[nodiscard]] inline raw_expr raw(std::string_view text) noexcept {
    return raw_expr{text};
}

// -- comparison operators -----------------------------------------------------

template <class T> [[nodiscard]] comparison<T> operator==(field_ref f, T v) { return {f, "=", std::move(v)}; }
template <class T> [[nodiscard]] comparison<T> operator!=(field_ref f, T v) { return {f, "!=", std::move(v)}; }
template <class T> [[nodiscard]] comparison<T> operator< (field_ref f, T v) { return {f, "<", std::move(v)}; }
template <class T> [[nodiscard]] comparison<T> operator<=(field_ref f, T v) { return {f, "<=", std::move(v)}; }
template <class T> [[nodiscard]] comparison<T> operator> (field_ref f, T v) { return {f, ">", std::move(v)}; }
template <class T> [[nodiscard]] comparison<T> operator>=(field_ref f, T v) { return {f, ">=", std::move(v)}; }

// String literals decay to const char*, which the arena binds as a string.
[[nodiscard]] inline comparison<const char*> operator==(field_ref f, const char* v) { return {f, "=", v}; }
[[nodiscard]] inline comparison<const char*> operator!=(field_ref f, const char* v) { return {f, "!=", v}; }

// -- joining ------------------------------------------------------------------
//
// Constrained so these only apply to this library's expression types; an
// unconstrained operator&& on templates would hijack every `&&` in scope.

namespace detail_expr {
template <class T> struct is_expr : std::false_type {};
template <class T> struct is_expr<comparison<T>> : std::true_type {};
template <class L, class R> struct is_expr<junction<L, R>> : std::true_type {};
template <> struct is_expr<raw_expr> : std::true_type {};
} // namespace detail_expr

template <class L, class R,
          class = std::enable_if_t<detail_expr::is_expr<L>::value &&
                                   detail_expr::is_expr<R>::value>>
[[nodiscard]] junction<L, R> operator&&(L lhs, R rhs) {
    return {std::move(lhs), std::move(rhs), "AND"};
}

template <class L, class R,
          class = std::enable_if_t<detail_expr::is_expr<L>::value &&
                                   detail_expr::is_expr<R>::value>>
[[nodiscard]] junction<L, R> operator||(L lhs, R rhs) {
    return {std::move(lhs), std::move(rhs), "OR"};
}

} // namespace dsl
} // namespace surrealdb
