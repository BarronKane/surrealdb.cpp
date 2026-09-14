// P4 DSL tests: the fluent surface, the guard surface, and the guarantee that
// they emit the same statement.

#include <surrealdb/surrealdb.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sdb = surrealdb;
namespace dsl = surrealdb::dsl;

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  FAIL (line %d): %s\n", line, what); }
}
#define CHECK(e) check((e), #e, __LINE__)

void expect_text(const sdb::result<dsl::built_query>& r, const char* want, int line) {
    ++g_checks;
    if (!r.has_value()) {
        ++g_failures;
        std::printf("  FAIL (line %d): build failed: %.*s\n", line,
                    static_cast<int>(r.error().message().size()),
                    r.error().message().data());
        return;
    }
    if (r.value().text() != want) {
        ++g_failures;
        std::printf("  FAIL (line %d):\n    want: %s\n    got:  %s\n",
                    line, want, r.value().text().c_str());
    }
}
#define EXPECT_TEXT(r, want) expect_text((r), (want), __LINE__)

// ---------------------------------------------------------------------------

void test_fluent_basics() {
    std::printf("dsl: fluent surface\n");

    auto q1 = dsl::select("person").build();
    EXPECT_TEXT(q1, "SELECT * FROM person;");

    auto q2 = dsl::select("person").fields("name", "age").build();
    EXPECT_TEXT(q2, "SELECT name, age FROM person;");

    auto q3 = dsl::select("person").limit(10).build();
    EXPECT_TEXT(q3, "SELECT * FROM person LIMIT $v0;");

    auto q4 = dsl::select("person")
                  .fields("name")
                  .order_by("age", false)
                  .limit(5)
                  .start(10)
                  .fetch("friends")
                  .build();
    EXPECT_TEXT(q4, "SELECT name FROM person ORDER BY age DESC "
                    "LIMIT $v0 START $v1 FETCH friends;");
}

void test_values_are_bound_not_interpolated() {
    std::printf("dsl: values are bound\n");

    auto q = dsl::select("person").where(dsl::f("name") == "alice").build();
    // The literal must not appear in the statement -- that is the whole point.
    EXPECT_TEXT(q, "SELECT * FROM person WHERE name = $v0;");
    CHECK(q.has_value() && q.value().text().find("alice") == std::string::npos);
    if (q.has_value()) {
        auto v = q.value().vars().view().get("v0");
        CHECK(v && v->as_string().value_or("") == "alice");
    }

    // A value that would otherwise close the statement stays inert.
    auto nasty = dsl::select("person")
                     .where(dsl::f("name") == "'; DROP TABLE person; --")
                     .build();
    EXPECT_TEXT(nasty, "SELECT * FROM person WHERE name = $v0;");
    CHECK(nasty.has_value() &&
          nasty.value().text().find("DROP") == std::string::npos);
}

void test_expression_composition() {
    std::printf("dsl: expression composition\n");

    auto q = dsl::select("person")
                 .where(dsl::f("age") > 30)
                 .build();
    EXPECT_TEXT(q, "SELECT * FROM person WHERE age > $v0;");

    // Parenthesised so precedence survives.
    auto q2 = dsl::select("person")
                  .where((dsl::f("city") == "Austin") || (dsl::f("city") == "Denver"))
                  .build();
    EXPECT_TEXT(q2, "SELECT * FROM person WHERE (city = $v0 OR city = $v1);");

    auto q3 = dsl::select("person")
                  .where((dsl::f("age") > 30) &&
                         ((dsl::f("city") == "Austin") || (dsl::f("city") == "Denver")))
                  .build();
    EXPECT_TEXT(q3, "SELECT * FROM person WHERE "
                    "(age > $v0 AND (city = $v1 OR city = $v2));");

    // Repeated where() ANDs.
    auto q4 = dsl::select("person")
                  .where(dsl::f("a") == 1)
                  .where(dsl::f("b") == 2)
                  .build();
    EXPECT_TEXT(q4, "SELECT * FROM person WHERE a = $v0 AND b = $v1;");
}

void test_guard_surface() {
    std::printf("dsl: guard surface\n");

    dsl::query q;
    {
        auto sel = q.select("person");
        sel.fields("name", "age");
        {
            auto w = sel.where();
            w.gt("age", 30);
            {
                auto g = w.any_of();
                g.eq("city", "Austin");
                g.eq("city", "Denver");
            }
        }
        sel.limit(10);
    }
    auto built = q.build();
    EXPECT_TEXT(built, "SELECT name, age FROM person WHERE age > $v0 "
                       "AND (city = $v1 OR city = $v2) LIMIT $v3;");
}

void test_surfaces_agree() {
    std::printf("dsl: both surfaces emit the same statement\n");

    // This is the parity guarantee. If it ever fails, the fluent surface has
    // grown a path of its own and the two have started to drift.
    auto fluent = dsl::select("person")
                      .fields("name", "age")
                      .where(dsl::f("age") > 30)
                      .limit(10)
                      .build();

    dsl::query gq;
    {
        auto sel = gq.select("person");
        sel.fields("name", "age");
        { auto w = sel.where(); w.gt("age", 30); }
        sel.limit(10);
    }
    auto guarded = gq.build();

    CHECK(fluent.has_value() && guarded.has_value());
    if (fluent.has_value() && guarded.has_value()) {
        CHECK(fluent.value().text() == guarded.value().text());
        if (fluent.value().text() != guarded.value().text()) {
            std::printf("    fluent: %s\n    guard:  %s\n",
                        fluent.value().text().c_str(),
                        guarded.value().text().c_str());
        }
    }
}

void test_guards_do_what_fluent_cannot() {
    std::printf("dsl: guards build a runtime-shaped query\n");

    // The case the fluent surface structurally cannot reach: conditions
    // accumulated in a loop. Each chained call there would change the
    // expression's type.
    struct filter { const char* field; const char* value; };
    const std::vector<filter> filters = {{"city", "Austin"}, {"role", "admin"}};

    dsl::query q;
    {
        auto sel = q.select("person");
        {
            auto w = sel.where();
            for (const filter& x : filters) w.eq(x.field, x.value);
        }
    }
    auto built = q.build();
    EXPECT_TEXT(built, "SELECT * FROM person WHERE city = $v0 AND role = $v1;");

    // And with no filters at all, the WHERE must disappear rather than emit
    // a dangling keyword.
    dsl::query empty;
    {
        auto sel = empty.select("person");
        { auto w = sel.where(); (void)w; }
    }
    EXPECT_TEXT(empty.build(), "SELECT * FROM person;");
}

void test_misuse_is_an_error_not_a_bad_statement() {
    std::printf("dsl: misuse becomes a result error\n");

    // Balance is guaranteed by destructors; ordering is not. Writing to the
    // outer clause while an inner guard is alive would put LIMIT inside the
    // WHERE -- so it is refused and latched instead.
    dsl::query q;
    {
        auto sel = q.select("person");
        auto w = sel.where();
        w.gt("age", 30);
        sel.limit(10);          // wrong: w is still open
    }
    auto built = q.build();
    CHECK(!built.has_value());
    if (!built.has_value()) {
        CHECK(built.error().message().find("still open") != std::string_view::npos);
    }

    // An identifier that is not one is refused rather than pasted in.
    auto bad = dsl::select("person; DROP TABLE x").build();
    CHECK(!bad.has_value());
    if (!bad.has_value()) {
        CHECK(bad.error().message().find("identifier") != std::string_view::npos);
    }
}

void test_literals_need_no_cast() {
    std::printf("dsl: plain literals work without a cast\n");

    // `f("age") > 30` hands over an `int`. Requiring int64_t at every literal
    // is the kind of papercut that makes a DSL not worth using -- and every
    // other test here casts explicitly, which is exactly how it went unnoticed.
    auto a = dsl::select("t").where(dsl::f("age") > 30).build();
    EXPECT_TEXT(a, "SELECT * FROM t WHERE age > $v0;");

    auto b = dsl::select("t").where(dsl::f("score") >= 1.5).build();
    EXPECT_TEXT(b, "SELECT * FROM t WHERE score >= $v0;");

    auto c = dsl::select("t").where(dsl::f("ok") == true).build();
    EXPECT_TEXT(c, "SELECT * FROM t WHERE ok = $v0;");

    auto d = dsl::select("t").limit(10).build();
    EXPECT_TEXT(d, "SELECT * FROM t LIMIT $v0;");

    // And the bound value keeps its type rather than being stringified.
    CHECK(a.has_value() &&
          a.value().vars().view().get("v0")->as_int().value_or(0) == 30);
    CHECK(b.has_value() &&
          b.value().vars().view().get("v0")->as_double().has_value());
    CHECK(c.has_value() &&
          c.value().vars().view().get("v0")->as_bool().value_or(false));
}

void test_against_the_database() {
    std::printf("dsl: statements run\n");
    auto c = sdb::connection::connect("memory");
    if (!c.has_value()) { std::printf("  no connection; skipped\n"); return; }
    auto db = std::move(c).value();
    (void)db.use("p4_ns", "p4_db");

    sdb::object_builder a;
    a.set("name", "alice").set("age", 30);
    CHECK(db.create_discarding("p4_tbl:alice", a).has_value());
    sdb::object_builder b;
    b.set("name", "bob").set("age", 20);
    CHECK(db.create_discarding("p4_tbl:bob", b).has_value());

    auto q = dsl::select("p4_tbl")
                 .fields("name")
                 .where(dsl::f("age") >= 25)
                 .build();
    CHECK(q.has_value());
    if (!q.has_value()) return;

    auto rows = db.run(q.value());
    CHECK(rows.has_value());
    if (!rows.has_value()) {
        std::printf("  run failed: %.*s\n",
                    static_cast<int>(rows.error().message().size()),
                    rows.error().message().data());
        return;
    }
    CHECK(rows.value().size() == 1);

    // One statement, so `single()` is the right reader: it folds a statement
    // that failed into the result rather than handing back zero rows.
    auto only = rows.value().single();
    CHECK(only.has_value());
    if (!only) {
        std::printf("    statement failed: %.*s\n",
                    static_cast<int>(only.error().message().size()),
                    only.error().message().data());
        return;
    }
    // Exactly one of the two records matches the bound predicate.
    CHECK(only.value().size() == 1);
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P4 DSL tests ===\n");
    std::printf("standard: %ld\n\n", static_cast<long>(SURREALDB_CPLUSPLUS));

    test_fluent_basics();
    test_values_are_bound_not_interpolated();
    test_expression_composition();
    test_guard_surface();
    test_surfaces_agree();
    test_guards_do_what_fluent_cannot();
    test_misuse_is_an_error_not_a_bad_statement();
    test_literals_need_no_cast();
    test_against_the_database();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
