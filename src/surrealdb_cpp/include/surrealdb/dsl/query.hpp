#pragma once

/// @file dsl/query.hpp
/// The fluent query surface.
///
/// **This is the default.** Use it whenever the shape of the query is known
/// where you write it; reach for the scope guards in guards.hpp only when the
/// shape is decided at runtime -- conditions in a loop, clauses behind a flag.
///
/// Every call here drives `select_writer`, the same object the guards drive.
/// The fluent surface owns no assembly of its own, which is what keeps a clause
/// added to one surface from being missing in the other.

#include "../config.hpp"
#include "arena.hpp"
#include "expr.hpp"
#include "guards.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace surrealdb {
namespace dsl {

/// A SELECT under construction, built by chaining.
///
/// Move-only: it owns the arena the statement is being written into.
class select_query {
public:
    explicit select_query(std::string_view target)
        : w_(a_, target) {}

    select_query(select_query&&) = delete;   // the writer holds a reference to a_
    select_query& operator=(select_query&&) = delete;
    select_query(const select_query&) = delete;
    select_query& operator=(const select_query&) = delete;

    template <class... Names>
    select_query& fields(Names&&... names) {
        (w_.field(std::string_view(names)), ...);
        return *this;
    }

    /// Filter. Called more than once, the conditions are ANDed.
    template <class Expr>
    select_query& where(const Expr& e) {
        w_.finish_fields();
        if (!w_.can_write("WHERE")) return *this;
        if (!where_written_) { a_.keyword("WHERE"); where_written_ = true; }
        else { a_.keyword("AND"); }
        e.render(a_);
        return *this;
    }

    select_query& order_by(std::string_view field, bool ascending = true) {
        w_.order_by(field, ascending); return *this;
    }
    select_query& limit(std::int64_t n) { w_.limit(n); return *this; }
    select_query& start(std::int64_t n) { w_.start(n); return *this; }
    select_query& fetch(std::string_view field) { w_.fetch(field); return *this; }

    /// Finish the statement.
    [[nodiscard]] result<built_query> build() {
        w_.terminate();
        if (a_.failed()) return error::local(error_code::error, a_.reason());
        return built_query(std::move(a_.buffer()), std::move(a_.variables()));
    }

    /// Hand the arena to the guard surface.
    ///
    /// This is the one direction mixing is allowed: a fluent statement can drop
    /// into guards for a clause whose shape is dynamic. The reverse -- splicing
    /// a guard into a chain -- is not offered, because it would make the two
    /// surfaces a matrix instead of a line.
    [[nodiscard]] where_guard where_dynamic() {
        w_.finish_fields();
        return where_guard(a_);
    }

private:
    arena a_;
    select_writer w_;
    bool where_written_{false};
};

/// Start a SELECT.
[[nodiscard]] inline select_query select(std::string_view target) {
    return select_query(target);
}

} // namespace dsl
} // namespace surrealdb
