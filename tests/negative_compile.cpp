// Code that must NOT compile.
//
// Every case here is a lifetime bug that used to build cleanly and read freed
// memory at run time. A guard that stops them is only worth having if
// something notices when it stops working, and a passing test suite cannot
// notice: the whole point is that these never become programs.
//
// So CMake compiles this file once per case with -DSURREALDB_NEG_CASE=<n> and
// marks the test WILL_FAIL. Case 0 is the control -- it must compile, or a
// typo in this file would make every other case "pass" for the wrong reason.

#include <surrealdb/surrealdb.hpp>

namespace sdb = surrealdb;

#ifndef SURREALDB_NEG_CASE
#  define SURREALDB_NEG_CASE 0
#endif

int main() {
    auto opened = sdb::connection::connect("memory");
    if (!opened) return 1;
    sdb::connection db = std::move(opened).value();

#if SURREALDB_NEG_CASE == 0
    // Control: the correct spellings, which must keep compiling.
    auto results = db.query("RETURN 1", nullptr);
    if (!results) return 1;
    auto rows = results.value().single();
    if (!rows) return 1;
    (void)rows.value().size();

    auto selected = db.select("t");
    if (!selected) return 1;
    sdb::array_view all = sdb::view(selected.value());
    (void)all.size();

    // all_ok() borrows nothing, so it stays legal on a temporary.
    (void)db.query("RETURN 1", nullptr).value().all_ok();

    // The sanctioned bounded-wait spellings. take() moves ownership out, so it
    // is legal on a named poll and leaves nothing behind to dangle.
    auto live = db.live("t");
    if (live) {
        sdb::stream s = std::move(live).value();
        auto polled = s.try_next();
        if (polled) {
            auto& p = polled.value();
            if (p.ready()) { auto n = p.take(); (void)n; }
        }
        s.close();
    }

#elif SURREALDB_NEG_CASE == 1
    // single() on a temporary: the view outlives the results that own it.
    auto rows = db.query("RETURN 1", nullptr).value().single();
    (void)rows;

#elif SURREALDB_NEG_CASE == 2
    // Indexing a temporary: same borrow, same dangle.
    auto st = db.query("RETURN 1", nullptr).value()[0];
    (void)st;

// Case 3 is deliberately absent.
//
// `for (st : db.query(...).value())` dangles at C++17 and C++20 and is safe at
// C++23 (P2718R0 extends every temporary in the range expression). It cannot be
// made a compile error: range-for binds the range to a reference, so the
// accessors see an lvalue and the deleted rvalue overloads never fire. It is
// the same hazard `std::optional` and `std::map::at` have, and the fix is the
// same -- name the owner. Documented in results.hpp instead of pretended away.

#elif SURREALDB_NEG_CASE == 4
    // view() over a temporary owned_values.
    sdb::array_view rows = sdb::view(db.select("t").value());
    (void)rows;

#elif SURREALDB_NEG_CASE == 5
    // view() over a temporary owned_object.
    sdb::object_builder o;
    o.set("k", 1);
    sdb::object_view ov = sdb::view(db.create("t:1", o).value());
    (void)ov;

#elif SURREALDB_NEG_CASE == 6
    // first_error() on a temporary.
    auto bad = db.query("RETURN 1", nullptr).value().first_error();
    (void)bad;

#elif SURREALDB_NEG_CASE == 7
    // value() on a poll temporary. `owned_byte_array` is a range, so
    //
    //     for (auto b : *rs.try_next().value())
    //
    // would iterate bytes owned by a poll that died at the semicolon. This is
    // the shape that produced a heap-use-after-free in query().single().
    auto live = db.live("t");
    if (live) {
        sdb::stream s = std::move(live).value();
        auto&& n = s.try_next().value().value();
        (void)n;
    }

#elif SURREALDB_NEG_CASE == 8
    // operator* on a poll temporary: the same borrow by a shorter spelling.
    auto live2 = db.live("t");
    if (live2) {
        sdb::stream s = std::move(live2).value();
        auto&& n = *s.try_next().value();
        (void)n;
    }

#endif
    return 0;
}
