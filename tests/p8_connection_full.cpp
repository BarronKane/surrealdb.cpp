// P8 tests: the remaining connection verbs.
//
// `call` (database functions), `insert_relation` (a graph edge given whole),
// and the three JSON Patch operations. These are the calls that had no C++
// path at all, so the bar here is that they reach the server and return
// something shaped the way the docs claim.

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

void report(const char* what, const sdb::error& e) {
    std::printf("  %s: %.*s\n", what,
                static_cast<int>(e.message().size()), e.message().data());
}

// ---------------------------------------------------------------------------

void test_call(sdb::connection& db) {
    std::printf("call: database functions\n");

    // A built-in with arguments.
    sdb::array_builder args;
    args.push(std::string("hello world"));

    auto up = db.call("string::uppercase", args);
    CHECK(up.has_value());
    if (!up) { report("uppercase", up.error()); }
    else {
        sdb::array_view rows = sdb::view(up.value());
        CHECK(rows.size() == 1);
        // A scalar result is normalised into a one-element array, so this is
        // never a bare string.
        if (!rows.empty())
            CHECK(rows[0].as_string().value_or("") == "HELLO WORLD");
    }

    // A built-in with no arguments at all -- the null-args overload.
    auto now = db.call("time::now");
    CHECK(now.has_value());
    if (!now) report("time::now", now.error());
    else {
        sdb::array_view rows = sdb::view(now.value());
        CHECK(rows.size() == 1);
        if (!rows.empty()) CHECK(rows[0].kind() == sdb::value_kind::datetime);
    }

    // Multiple arguments, of mixed type.
    sdb::array_builder three;
    three.push("hello").push(1).push(3);
    auto sl = db.call("string::slice", three);
    CHECK(sl.has_value());
    if (!sl) report("string::slice", sl.error());
    else {
        sdb::array_view rows = sdb::view(sl.value());
        if (rows.size() == 1) CHECK(rows[0].as_string().value_or("") == "el");
    }

    // A function that does not exist must fail rather than return empty.
    auto bad = db.call("fn::definitely_not_defined");
    CHECK(!bad.has_value());
}

void test_insert_relation(sdb::connection& db) {
    std::printf("insert_relation: edge given whole\n");

    sdb::object_builder a; a.set("name", "a");
    sdb::object_builder b; b.set("name", "b");
    auto ra = db.create("p8_node:a", a);
    auto rb = db.create("p8_node:b", b);
    CHECK(ra.has_value() && rb.has_value());
    if (!ra) { report("create a", ra.error()); return; }

    // The edge carries its own ends, unlike relate() which names them.
    sdb::object_builder edge;
    edge.set("in", sdb::make::thing("p8_node", "a"))
        .set("out", sdb::make::thing("p8_node", "b"))
        .set("weight", 7);

    auto rel = db.insert_relation("p8_edge", edge);
    CHECK(rel.has_value());
    if (!rel) { report("insert_relation", rel.error()); return; }

    sdb::array_view rows = sdb::view(rel.value());
    CHECK(rows.size() >= 1);
    if (rows.empty()) return;

    auto obj = rows[0].as_object();
    CHECK(obj.has_value());
    if (obj) {
        auto w = obj->get("weight");
        CHECK(w && w->as_int().value_or(0) == 7);
    }
}

void test_patch(sdb::connection& db) {
    std::printf("patch: add / replace / remove\n");

    sdb::object_builder rec;
    rec.set("name", "ada").set("city", "london");
    auto created = db.create("p8_patch:one", rec);
    CHECK(created.has_value());
    if (!created) { report("create", created.error()); return; }

    // Add a field that was not there.
    auto added = db.patch_add("p8_patch:one", "/nickname", sdb::value(sdb::make::string("countess").get()));
    CHECK(added.has_value());
    if (!added) report("patch_add", added.error());

    // Hold the value so it outlives the call -- a temporary owned_value would
    // be gone before the server saw it.
    {
        auto v = sdb::make::string("paris");
        auto replaced = db.patch_replace("p8_patch:one", "/city", sdb::value(v.get()));
        CHECK(replaced.has_value());
        if (!replaced) report("patch_replace", replaced.error());
    }

    auto removed = db.patch_remove("p8_patch:one", "/name");
    CHECK(removed.has_value());
    if (!removed) report("patch_remove", removed.error());

    // Read the record back and confirm all three landed.
    auto sel = db.select("p8_patch:one");
    CHECK(sel.has_value());
    if (!sel) return;

    sdb::array_view rows = sdb::view(sel.value());
    CHECK(rows.size() == 1);
    if (rows.empty()) return;
    auto obj = rows[0].as_object();
    CHECK(obj.has_value());
    if (!obj) return;

    auto nick = obj->get("nickname");
    CHECK(nick && nick->as_string().value_or("") == "countess");
    auto city = obj->get("city");
    CHECK(city && city->as_string().value_or("") == "paris");
    CHECK(!obj->get("name").has_value());

    // SurrealDB does not reject a replace on a path that is not there -- it
    // builds the parents and writes the leaf. Pinned here because it differs
    // from RFC 6902 and a caller porting a patch document will assume
    // otherwise.
    auto v = sdb::make::string("x");
    auto nowhere = db.patch_replace("p8_patch:one", "/not/a/path", sdb::value(v.get()));
    CHECK(nowhere.has_value());
    if (nowhere) {
        auto again = db.select("p8_patch:one");
        if (again) {
            sdb::array_view r2 = sdb::view(again.value());
            if (!r2.empty()) {
                auto o2 = r2[0].as_object();
                CHECK(o2 && o2->get("not").has_value());
            }
        }
    }

    // Likewise a remove of something absent is a no-op, not an error.
    auto absent = db.patch_remove("p8_patch:one", "/never_existed");
    CHECK(absent.has_value());
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P8 connection-completeness tests ===\n");
    std::printf("standard: %ld\n\n", static_cast<long>(SURREALDB_CPLUSPLUS));

    auto conn = sdb::connection::connect("memory");
    if (!conn) {
        std::printf("connect failed: %.*s\n",
                    static_cast<int>(conn.error().message().size()),
                    conn.error().message().data());
        return 1;
    }
    sdb::connection db = std::move(conn).value();
    auto used = db.use("p8_ns", "p8_db");
    if (!used) { std::printf("use failed\n"); return 1; }

    test_call(db);
    test_insert_relation(db);
    test_patch(db);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
