#pragma once

/// @file results.hpp
/// Query results, one entry per statement.
///
/// `sr_query` hands back an array of `sr_arr_res_t`, and each of those is
/// `{ ok, err }` -- a row array and an error side by side. Returning that
/// directly is where this library used to leak its C underneath, and the leak
/// was not cosmetic:
///
///     for (int i = 0; i < results.value().size(); ++i) {
///         array_view rows(results.value()[i].ok);   // <-- .err never read
///         ...
///     }
///
/// A statement that failed at runtime has `ok.len == 0`, so that loop reads a
/// thrown error as an empty result set and carries on. Both of this library's
/// own examples were written that way, which is the clearest evidence that the
/// shape invites it.
///
/// So a statement is a type that cannot be read without its status being in
/// reach, and the single-statement case -- which is most of them -- gets a
/// `single()` that folds the failure into the `result<T>` the caller is
/// already checking.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/owned.hpp"
#include "error.hpp"
#include "value.hpp"

#include <iterator>

#if SURREALDB_HAS_RANGES
#  include <ranges>
#endif
#include <string_view>
#include <utility>

namespace surrealdb {

/// One statement's outcome: rows, or the error that replaced them.
///
/// A view. It borrows from the `query_results` it came from, which owns the
/// rows and the message.
class statement_result {
public:
    statement_result() noexcept : r_(nullptr) {}
    explicit statement_result(const sr_arr_res_t* r) noexcept : r_(r) {}

    /// False if this statement failed. **Check this before reading `rows()`**
    /// -- a failed statement has no rows, and is otherwise indistinguishable
    /// from one that matched nothing.
    [[nodiscard]] bool ok() const noexcept { return r_ && r_->err.code == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    /// The rows. Empty when the statement failed, which is exactly why `ok()`
    /// exists.
    [[nodiscard]] array_view rows() const noexcept {
        return r_ ? array_view(r_->ok) : array_view();
    }

    /// The status code. `error_code::ok` when the statement succeeded.
    [[nodiscard]] error_code code() const noexcept {
        return r_ ? static_cast<error_code>(r_->err.code) : error_code::ok;
    }

    /// The failure message, empty when there was none. Borrowed, not copied --
    /// it lives in the `query_results` that owns this statement.
    [[nodiscard]] std::string_view message() const noexcept {
        if (!r_ || !r_->err.msg) return {};
        return std::string_view(r_->err.msg);
    }

    /// The rows if the statement succeeded, its failure otherwise.
    ///
    /// For a caller who would rather propagate than branch. `error::local`
    /// copies the message, because the borrowed one dies with the results.
    [[nodiscard]] result<array_view> checked() const {
        if (ok()) return rows();
        return error::local(code(), message());
    }

    /// The underlying C struct, for anything this class does not cover.
    [[nodiscard]] const sr_arr_res_t* raw() const noexcept { return r_; }

private:
    const sr_arr_res_t* r_;
};

/// Every statement's outcome, in the order they were written.
///
/// Owns the array. Each `statement_result` it yields borrows from it, so this
/// has to outlive anything read out of it -- the same rule as `array_view` over
/// `owned_values`.
class query_results
#if SURREALDB_HAS_RANGES
    : public std::ranges::view_interface<query_results>
#endif
{
public:
    query_results() noexcept = default;
    explicit query_results(owned_arr_results res) noexcept : res_(std::move(res)) {}

    query_results(query_results&&) noexcept = default;
    query_results& operator=(query_results&&) noexcept = default;
    query_results(const query_results&) = delete;
    query_results& operator=(const query_results&) = delete;

    [[nodiscard]] int size() const noexcept { return res_.size(); }
    [[nodiscard]] bool empty() const noexcept { return res_.empty(); }

    // -- borrowing accessors -------------------------------------------------
    //
    // One hazard survives all of this and you should know about it:
    //
    //     for (auto st : db.query("...", nullptr).value())   // BAD below C++23
    //
    // `value()` returns a reference into the `result` temporary, and range-for
    // extends the reference rather than what it points at. That dangles at
    // C++17 and C++20; C++23 fixed it (P2718R0). It cannot be made a compile
    // error, because range-for binds the range to a reference and the deleted
    // overloads below only see rvalues. `std::optional` and `std::map::at` have
    // exactly the same hole. Name the owner and the problem goes away.
    //
    // Every one of these hands out something that points into the array this
    // object owns, so every one is `&`-qualified with the rvalue overload
    // deleted. Without that,
    //
    //     auto rows = db.query("...", nullptr).value().single();
    //
    // compiles and reads freed memory: the results temporary dies at the
    // semicolon and the view outlives it. That is a heap-use-after-free, and it
    // is the most natural way to write the call -- so it has to be a compile
    // error rather than a documented hazard. Bind the results to a named local
    // and read from that.

    [[nodiscard]] statement_result operator[](int i) const& noexcept {
        return statement_result(res_.data() + i);
    }
    statement_result operator[](int) const&&
        SURREALDB_DELETED("indexing a query_results temporary: the statement borrows "
                          "the array that is about to be destroyed. Name the results first.");

    /// True when every statement succeeded. Safe on a temporary: it returns a
    /// bool and keeps nothing.
    [[nodiscard]] bool all_ok() const noexcept {
        for (int i = 0; i < size(); ++i)
            if (!(*this)[i].ok()) return false;
        return true;
    }

    /// The first statement that failed, or a default-constructed (invalid) one.
    /// Useful for reporting: a batch usually fails for one reason.
    [[nodiscard]] statement_result first_error() const& noexcept {
        for (int i = 0; i < size(); ++i)
            if (!(*this)[i].ok()) return (*this)[i];
        return statement_result();
    }
    statement_result first_error() const&&
        SURREALDB_DELETED("first_error() on a query_results temporary borrows an array "
                          "that dies at the semicolon. Name the results first.");

    /// The rows of a single-statement query.
    ///
    /// The common case, and the one worth making hard to misread: this fails if
    /// the query was not exactly one statement, and fails if that statement
    /// did. A caller who checks the `result` it already has cannot silently
    /// read a thrown error as an empty table.
    [[nodiscard]] result<array_view> single() const& {
        if (size() != 1) {
            return error::local(
                error_code::error,
                size() == 0 ? std::string_view("query returned no statements")
                            : std::string_view("query returned more than one statement; "
                                               "iterate instead of calling single()"));
        }
        return (*this)[0].checked();
    }
    result<array_view> single() const&&
        SURREALDB_DELETED("single() on a query_results temporary returns a view into "
                          "an array that dies at the semicolon. Name the results first.");

    /// The raw span, for anything this class does not cover.
    [[nodiscard]] const owned_arr_results& raw() const& noexcept { return res_; }
    const owned_arr_results& raw() const&&
        SURREALDB_DELETED("raw() on a query_results temporary outlives what it refers to.");

    // -- iteration ----------------------------------------------------------

    /// By-value, like `session_list`: a `statement_result` is one pointer, so
    /// there is nothing to gain from handing out references.
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = statement_result;
        using difference_type   = std::ptrdiff_t;
        using reference         = statement_result;
        using pointer           = const statement_result*;

        iterator() noexcept = default;
        explicit iterator(const sr_arr_res_t* p) noexcept : p_(p) {}

        [[nodiscard]] statement_result operator*() const noexcept {
            return statement_result(p_);
        }

        iterator& operator++() noexcept { ++p_; return *this; }
        iterator operator++(int) noexcept { iterator t = *this; ++p_; return t; }

        [[nodiscard]] friend bool operator==(iterator a, iterator b) noexcept {
            return a.p_ == b.p_;
        }
        [[nodiscard]] friend bool operator!=(iterator a, iterator b) noexcept {
            return a.p_ != b.p_;
        }

    private:
        const sr_arr_res_t* p_{nullptr};
    };

    using const_iterator = iterator;

    [[nodiscard]] iterator begin() const& noexcept { return iterator(res_.data()); }
    [[nodiscard]] iterator end() const& noexcept {
        return iterator(res_.data() + res_.size());
    }
    iterator begin() const&&
        SURREALDB_DELETED("iterating a query_results temporary: it owns the array and "
                          "destroys it at the semicolon. Name the results first.");
    iterator end() const&& SURREALDB_DELETED("see begin().");

private:
    owned_arr_results res_;
};

} // namespace surrealdb

#if SURREALDB_HAS_RANGES
// Deliberately *not* `enable_borrowed_range`: this owns its array, so an
// iterator outliving it really is dangling and the ranges dangling check is
// telling the truth. The geometry spans opt in because they only ever point
// into a value somebody else owns.
#endif
