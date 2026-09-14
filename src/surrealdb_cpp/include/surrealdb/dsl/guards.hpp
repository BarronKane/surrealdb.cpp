#pragma once

/// @file dsl/guards.hpp
/// The scope-guard query surface.
///
/// Entering a clause returns a guard; leaving the scope closes it. Nesting is
/// literal C++ scope, so a clause cannot be left unclosed.
///
/// **Reach for this when the shape of the query is decided at runtime** --
/// conditions built in a loop, clauses included by a flag, filters coming from
/// user input. The fluent surface in `query.hpp` cannot do that: chaining
/// changes the expression's type at every step, so it cannot accumulate. For a
/// query whose shape is known when you write it, use that instead; it reads
/// better and checks more.
///
/// Guards are non-movable on purpose. A guard *is* a scope, and one that could
/// be returned or stored would no longer say anything about when its clause
/// closes.
///
/// **Balance is guaranteed; ordering is not.** Destructors make every clause
/// close, but nothing about C++ stops a caller writing to an outer guard while
/// an inner one is alive -- which would put the text in the wrong clause. The
/// arena tracks open children and latches an error instead, so the mistake
/// surfaces when the query is built rather than as a malformed statement sent
/// to the server.

#include "../config.hpp"
#include "arena.hpp"

#include <cstdint>
#include <string_view>

namespace surrealdb {
namespace dsl {

/// Base for every guard: registers with the arena while alive.
class guard_base {
public:
    guard_base(const guard_base&) = delete;
    guard_base& operator=(const guard_base&) = delete;
    guard_base(guard_base&&) = delete;
    guard_base& operator=(guard_base&&) = delete;

protected:
    /// Registers this guard and remembers how deep it sits, so `can_write`
    /// can tell "something is open inside me" from "I am open".
    explicit guard_base(arena& a) noexcept : a_(a) {
        a_.push_child();
        depth_ = a_.depth();
    }
    ~guard_base() { a_.pop_child(); }

    /// May this guard write? False also latches the reason.
    [[nodiscard]] bool can_write(const char* what) {
        if (a_.failed()) return false;
        if (a_.depth() != depth_) { a_.fail_ordering(what); return false; }
        return true;
    }

    arena& a_;
    int depth_{0};
};

/// A parenthesised group of conditions joined by AND or OR.
class group_guard : private guard_base {
public:
    group_guard(arena& a, const char* joiner)
        : guard_base(a), joiner_(joiner) {
        a_.space();
        a_.put('(');
        open_ = a_.mark();
    }

    ~group_guard() {
        // An empty group would be `()`, which is a syntax error. Rewinding is
        // simpler and cheaper than deciding in advance whether anything will
        // be added.
        if (!a_.grew_since(open_)) {
            a_.rewind(open_ > 0 ? open_ - 1 : 0);
        } else {
            a_.put(')');
        }
    }

    template <class T>
    group_guard& compare(std::string_view field, const char* op, T&& v) {
        join();
        a_.identifier(field);
        a_.space();
        a_.put(op);
        a_.space();
        a_.bind(std::forward<T>(v));
        return *this;
    }

    template <class T> group_guard& eq(std::string_view f, T&& v) { return compare(f, "=", std::forward<T>(v)); }
    template <class T> group_guard& ne(std::string_view f, T&& v) { return compare(f, "!=", std::forward<T>(v)); }
    template <class T> group_guard& lt(std::string_view f, T&& v) { return compare(f, "<", std::forward<T>(v)); }
    template <class T> group_guard& le(std::string_view f, T&& v) { return compare(f, "<=", std::forward<T>(v)); }
    template <class T> group_guard& gt(std::string_view f, T&& v) { return compare(f, ">", std::forward<T>(v)); }
    template <class T> group_guard& ge(std::string_view f, T&& v) { return compare(f, ">=", std::forward<T>(v)); }

    /// A raw fragment, for operators this surface does not name. The text is
    /// written as given, so it must not carry caller-supplied data -- bind
    /// that with the comparison helpers instead.
    group_guard& raw(std::string_view fragment) {
        join();
        a_.put(fragment);
        return *this;
    }

    /// A nested group, joined into this one.
    [[nodiscard]] group_guard all_of() { join(); return group_guard(a_, "AND"); }
    [[nodiscard]] group_guard any_of() { join(); return group_guard(a_, "OR"); }

private:
    void join() {
        if (!can_write("condition")) return;
        if (first_) { first_ = false; return; }
        a_.space();
        a_.put(joiner_);
        a_.space();
    }

    const char* joiner_;
    std::size_t open_{0};
    bool first_{true};
};

/// The WHERE clause.
class where_guard : private guard_base {
public:
    explicit where_guard(arena& a) : guard_base(a) {
        // Recorded before the keyword so an empty clause rewinds exactly,
        // whatever the keyword's length or spacing.
        before_ = a_.mark();
        a_.keyword("WHERE");
        open_ = a_.mark();
    }

    ~where_guard() {
        // `WHERE` with nothing after it does not parse; drop the keyword.
        if (!a_.grew_since(open_)) a_.rewind(before_);
    }

    template <class T>
    where_guard& compare(std::string_view field, const char* op, T&& v) {
        join();
        a_.identifier(field);
        a_.space();
        a_.put(op);
        a_.space();
        a_.bind(std::forward<T>(v));
        return *this;
    }

    template <class T> where_guard& eq(std::string_view f, T&& v) { return compare(f, "=", std::forward<T>(v)); }
    template <class T> where_guard& ne(std::string_view f, T&& v) { return compare(f, "!=", std::forward<T>(v)); }
    template <class T> where_guard& lt(std::string_view f, T&& v) { return compare(f, "<", std::forward<T>(v)); }
    template <class T> where_guard& le(std::string_view f, T&& v) { return compare(f, "<=", std::forward<T>(v)); }
    template <class T> where_guard& gt(std::string_view f, T&& v) { return compare(f, ">", std::forward<T>(v)); }
    template <class T> where_guard& ge(std::string_view f, T&& v) { return compare(f, ">=", std::forward<T>(v)); }

    where_guard& raw(std::string_view fragment) { join(); a_.put(fragment); return *this; }

    [[nodiscard]] group_guard all_of() { join(); return group_guard(a_, "AND"); }
    [[nodiscard]] group_guard any_of() { join(); return group_guard(a_, "OR"); }

private:
    void join() {
        if (!can_write("condition")) return;
        if (first_) { first_ = false; return; }
        a_.keyword("AND");
    }

    std::size_t before_{0};
    std::size_t open_{0};
    bool first_{true};
};

/// The SELECT sequencing, shared by both surfaces.
///
/// Extracted so the fluent surface in query.hpp can drive the *same* writer
/// rather than reimplementing clause order. That is the rule that keeps the two
/// surfaces from drifting: the guards below and the fluent builder both call
/// into this, and neither has a private path.
class select_writer {
public:
    select_writer(arena& a, std::string_view target) : a_(a), target_(target) {
        a_.put("SELECT");
        depth_ = a_.depth();
    }

    /// Same rule as the guards: refuse when something is open inside us.
    [[nodiscard]] bool can_write(const char* what) {
        if (a_.failed()) return false;
        if (a_.depth() != depth_) { a_.fail_ordering(what); return false; }
        return true;
    }

    void field(std::string_view name) {
        if (!can_write("SELECT field")) return;
        if (first_field_) { a_.space(); first_field_ = false; }
        else { a_.put(", "); }
        a_.identifier(name);
    }

    /// Close the projection and emit FROM.
    ///
    /// Every clause after the field list calls this, so the target lands in the
    /// right place however the caller orders the clauses.
    void finish_fields() {
        if (from_written_) return;
        from_written_ = true;
        if (first_field_) { a_.space(); a_.put('*'); }
        a_.keyword("FROM");
        a_.identifier(target_);
    }

    void order_by(std::string_view field, bool ascending) {
        finish_fields();
        if (!can_write("ORDER BY")) return;
        if (!ordered_) { a_.keyword("ORDER BY"); ordered_ = true; }
        else { a_.put(", "); }
        a_.identifier(field);
        a_.put(ascending ? " ASC" : " DESC");
    }

    void limit(std::int64_t n) {
        finish_fields();
        if (!can_write("LIMIT")) return;
        a_.keyword("LIMIT");
        a_.bind(n);
    }

    void start(std::int64_t n) {
        finish_fields();
        if (!can_write("START")) return;
        a_.keyword("START");
        a_.bind(n);
    }

    void fetch(std::string_view field) {
        finish_fields();
        if (!can_write("FETCH")) return;
        if (!fetched_) { a_.keyword("FETCH"); fetched_ = true; }
        else { a_.put(", "); }
        a_.identifier(field);
    }

    void terminate() {
        if (terminated_) return;
        terminated_ = true;
        finish_fields();
        a_.put(';');
    }

    [[nodiscard]] arena& a() noexcept { return a_; }

private:
    arena& a_;
    std::string_view target_;
    bool first_field_{true};
    bool from_written_{false};
    bool ordered_{false};
    bool fetched_{false};
    bool terminated_{false};
    int depth_{0};
};

/// A SELECT statement, as a scope guard.
class select_guard : private guard_base {
public:
    select_guard(arena& a, std::string_view target) : guard_base(a), w_(a, target) {}

    ~select_guard() { w_.terminate(); }

    select_guard& field(std::string_view name) { w_.field(name); return *this; }

    template <class... Names>
    select_guard& fields(Names&&... names) {
        (w_.field(std::string_view(names)), ...);
        return *this;
    }

    [[nodiscard]] where_guard where() {
        w_.finish_fields();
        return where_guard(a_);
    }

    select_guard& order_by(std::string_view f, bool ascending = true) {
        w_.order_by(f, ascending); return *this;
    }
    select_guard& limit(std::int64_t n) { w_.limit(n); return *this; }
    select_guard& start(std::int64_t n) { w_.start(n); return *this; }
    select_guard& fetch(std::string_view f) { w_.fetch(f); return *this; }

private:
    select_writer w_;
};

/// A statement under construction.
class query {
public:
    query() = default;

    [[nodiscard]] select_guard select(std::string_view target) {
        return select_guard(a_, target);
    }

    /// Finish. Fails if a clause was written out of order, or an identifier
    /// was not a valid one.
    [[nodiscard]] result<built_query> build() {
        if (a_.failed()) {
            // No C string to adopt: this failure originated here.
            return error::local(error_code::error, a_.reason());
        }
        return built_query(std::move(a_.buffer()), std::move(a_.variables()));
    }

    [[nodiscard]] arena& raw_arena() noexcept { return a_; }

private:
    arena a_;
};

} // namespace dsl
} // namespace surrealdb
