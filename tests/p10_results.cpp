// P10 tests: query results, one entry per statement.
//
// The thing under test is a trap rather than a feature. `sr_query` returns
// `{ok, err}` pairs, a failed statement has `ok.len == 0`, and so a reader that
// only looks at `ok` sees a thrown error as an empty table. Both of this
// library's examples were written that way. Every check here is aimed at that.

#include <surrealdb/surrealdb.hpp>

#include <cstdio>
#include <string>

namespace sdb = surrealdb;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  FAIL (line %d): %s\n", line, what); }
}
#define CHECK(e) check((e), #e, __LINE__)

// ---------------------------------------------------------------------------

void test_single_statement(sdb::connection& db) {
    std::printf("results: one statement\n");

    auto r = db.query("RETURN 1", nullptr);
    CHECK(r.has_value());
    if (!r) return;

    CHECK(r.value().size() == 1);
    CHECK(r.value().all_ok());
    CHECK(!r.value().empty());

    auto st = r.value()[0];
    CHECK(st.ok());
    CHECK(static_cast<bool>(st));
    CHECK(st.code() == sdb::error_code::ok);
    CHECK(st.message().empty());
    CHECK(st.rows().size() == 1);

    auto only = r.value().single();
    CHECK(only.has_value());
    if (only) CHECK(only.value().size() == 1);
}

void test_failed_statement_is_not_empty(sdb::connection& db) {
    std::printf("results: a failed statement is not an empty one\n");

    auto r = db.query("THROW 'boom'", nullptr);

    // The call succeeds -- the failure is per statement, not per query.
    CHECK(r.has_value());
    if (!r) return;
    CHECK(r.value().size() == 1);

    auto st = r.value()[0];
    CHECK(!st.ok());
    CHECK(st.code() != sdb::error_code::ok);
    CHECK(!st.message().empty());
    CHECK(st.message().find("boom") != std::string_view::npos);

    // This is the whole point: rows() is empty, exactly as it would be for a
    // statement that simply matched nothing. Only the status tells them apart.
    CHECK(st.rows().empty());

    // checked() and single() both surface it rather than returning no rows.
    CHECK(!st.checked().has_value());
    auto only = r.value().single();
    CHECK(!only.has_value());
    if (!only) CHECK(only.error().message().find("boom") != std::string_view::npos);

    // And the message survives the results being destroyed -- `single()` copies
    // it, because the borrowed one dies with the array.
    std::string kept;
    {
        auto inner = db.query("THROW 'transient'", nullptr);
        if (inner) { auto e = inner.value().single(); if (!e) kept = std::string(e.error().message()); }
    }
    CHECK(kept.find("transient") != std::string::npos);
}

void test_mixed_batch(sdb::connection& db) {
    std::printf("results: a batch with one bad statement\n");

    auto r = db.query("RETURN 1; THROW 'boom'; RETURN 3", nullptr);
    CHECK(r.has_value());
    if (!r) return;

    CHECK(r.value().size() == 3);
    CHECK(!r.value().all_ok());

    // A failing statement does not stop the ones after it.
    CHECK(r.value()[0].ok());
    CHECK(!r.value()[1].ok());
    CHECK(r.value()[2].ok());

    auto first_bad = r.value().first_error();
    CHECK(!first_bad.ok());
    CHECK(first_bad.message().find("boom") != std::string_view::npos);

    // single() refuses a batch rather than quietly reading statement zero.
    auto only = r.value().single();
    CHECK(!only.has_value());

    // Iteration yields statement_result, not the C struct.
    int seen = 0, failed = 0;
    for (sdb::statement_result st : r.value()) {
        ++seen;
        if (!st.ok()) ++failed;
    }
    CHECK(seen == 3);
    CHECK(failed == 1);
}

void test_all_ok_batch(sdb::connection& db) {
    std::printf("results: a batch that all succeeds\n");

    auto r = db.query("RETURN 1; RETURN 2", nullptr);
    CHECK(r.has_value());
    if (!r) return;
    CHECK(r.value().size() == 2);
    CHECK(r.value().all_ok());
    CHECK(!r.value().first_error().ok());   // default-constructed: no failure found
    CHECK(r.value().first_error().raw() == nullptr);
}

void test_parse_error_fails_the_call(sdb::connection& db) {
    std::printf("results: a parse error fails the call\n");

    // The other error channel. Nothing ran, so there are no statements to
    // report against and the call itself fails.
    auto r = db.query("SELECT * FROM", nullptr);
    CHECK(!r.has_value());
    if (!r) CHECK(!r.error().message().empty());
}

void test_dsl_path(sdb::connection& db) {
    std::printf("results: the DSL path returns the same type\n");

    auto q = sdb::dsl::select("p10_tbl").fields("name").build();
    CHECK(q.has_value());
    if (!q) return;

    auto r = db.run(q.value());
    CHECK(r.has_value());
    if (!r) return;
    CHECK(r.value().size() == 1);
    CHECK(r.value().single().has_value());
}

void test_empty_versus_failed(sdb::connection& db) {
    std::printf("results: empty and failed are distinguishable\n");

    // A query that matches nothing: succeeds, zero rows.
    auto empty = db.query("SELECT * FROM p10_tbl WHERE name = 'nobody'", nullptr);
    CHECK(empty.has_value());
    if (empty) {
        auto st = empty.value()[0];
        CHECK(st.ok());
        CHECK(st.rows().empty());
    }

    // A query that fails: also zero rows, but not ok. Same rows(), different
    // status -- which is the distinction the old raw shape threw away.
    auto failed = db.query("THROW 'boom'", nullptr);
    CHECK(failed.has_value());
    if (failed) {
        auto st = failed.value()[0];
        CHECK(!st.ok());
        CHECK(st.rows().empty());
    }
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P10 query-result tests ===\n");
    std::printf("standard: %ld\n\n", static_cast<long>(SURREALDB_CPLUSPLUS));

    auto made = sdb::connection::connect("memory");
    if (!made) {
        std::printf("connect failed: %.*s\n",
                    static_cast<int>(made.error().message().size()),
                    made.error().message().data());
        return 1;
    }
    sdb::connection db = std::move(made).value();
    if (!db.use("p10_ns", "p10_db")) { std::printf("use failed\n"); return 1; }

    sdb::object_builder seed;
    seed.set("name", "seed");
    (void)db.create("p10_tbl:one", seed);

    test_single_statement(db);
    test_failed_statement_is_not_empty(db);
    test_mixed_batch(db);
    test_all_ok_batch(db);
    test_parse_error_fails_the_call(db);
    test_dsl_path(db);
    test_empty_versus_failed(db);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
