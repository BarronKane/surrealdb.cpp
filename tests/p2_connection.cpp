// P2 connection tests: lifecycle, options, operations, transactions, poisoning.

#include <surrealdb/surrealdb.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace sdb = surrealdb;

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  FAIL (line %d): %s\n", line, what); }
}
#define CHECK(e) check((e), #e, __LINE__)

sdb::connection open() {
    auto c = sdb::connection::connect("memory");
    if (!c.has_value()) {
        std::printf("  connect failed: %.*s\n",
                    static_cast<int>(c.error().message().size()),
                    c.error().message().data());
        return sdb::connection();
    }
    auto db = std::move(c).value();
    (void)db.use("p2_ns", "p2_db");
    return db;
}

void test_lifecycle() {
    std::printf("connection: lifecycle\n");
    auto db = open();
    CHECK(db.valid());
    CHECK(!db.poisoned());

    // Move-only, and moving leaves the source inert rather than dangling.
    sdb::connection moved = std::move(db);
    CHECK(moved.valid());
    CHECK(!db.valid());
    static_assert(!std::is_copy_constructible<sdb::connection>::value,
                  "connection must be move-only");

    moved.disconnect();
    CHECK(!moved.valid());

    // A default-constructed connection is inert, not a crash.
    sdb::connection empty;
    CHECK(!empty.valid());
}

void test_bad_endpoint() {
    std::printf("connection: a bad endpoint is an error, not a crash\n");
    auto c = sdb::connection::connect("not-a-real-scheme://nowhere");
    CHECK(!c.has_value());
    if (!c.has_value()) CHECK(!c.error().message().empty());
}

void test_options() {
    std::printf("connection: options and the capability sandbox\n");

    // A default-constructed options must behave exactly like no options.
    sdb::options plain;
    auto a = sdb::connection::connect("memory", plain);
    CHECK(a.has_value());

    sdb::options opts;
    opts.query_timeout(30).transaction_timeout(30);
    opts.capabilities().scripting = sdb::toggle::off;
    opts.capabilities().allow_network = sdb::target_set::none();
    opts.capabilities().deny_functions = sdb::target_set::only({"http"});

    auto b = sdb::connection::connect("memory", opts);
    CHECK(b.has_value());
    if (!b.has_value()) {
        std::printf("  %.*s\n", static_cast<int>(b.error().message().size()),
                    b.error().message().data());
    }

    // An unparseable capability name must be reported, never silently dropped.
    sdb::options bad;
    bad.capabilities().allow_experimental = sdb::target_set::only({"nonsense-name"});
    auto c = sdb::connection::connect("memory", bad);
    CHECK(!c.has_value());
    if (!c.has_value()) {
        CHECK(c.error().message().find("allow_experimental") != std::string_view::npos);
    }
}

void test_options_strings_outlive_the_call() {
    std::printf("options: owned names survive a temporary\n");
    // In C the caller must keep the string array alive; here the names are
    // owned, so building the set from temporaries is safe.
    sdb::options opts;
    {
        std::string name = "gql";
        opts.capabilities().allow_experimental.add(name);
        name = "clobbered";
    }
    auto c = sdb::connection::connect("memory", opts);
    CHECK(c.has_value());
}

void test_operations() {
    std::printf("connection: data operations\n");
    auto db = open();
    if (!db.valid()) return;

    auto v = db.version();
    CHECK(v.has_value());
    if (v.has_value()) CHECK(v.value().get() != nullptr);

    CHECK(db.health().has_value());

    sdb::object_builder rec;
    rec.set("name", "alice").set("age", 30);

    auto created = db.create("p2_tbl:alice", rec);
    CHECK(created.has_value());
    if (created.has_value()) {
        sdb::object_view ov = sdb::view(created.value());
        CHECK(ov.valid());
        auto name = ov.get("name");
        CHECK(name && name->as_string().value_or("") == "alice");
    }

    auto rows = db.select("p2_tbl");
    CHECK(rows.has_value());
    if (rows.has_value()) {
        sdb::array_view av = sdb::view(rows.value());
        CHECK(av.size() >= 1);
    }

    // Discarding form allocates nothing server-side.
    sdb::object_builder r2;
    r2.set("name", "bob");
    CHECK(db.create_discarding("p2_tbl:bob", r2).has_value());

    sdb::object_builder patch;
    patch.set("age", 31);
    CHECK(db.merge("p2_tbl:alice", patch).has_value());

    auto q = db.query("SELECT * FROM p2_tbl;");
    CHECK(q.has_value());
    if (q.has_value()) CHECK(q.value().size() >= 1);

    CHECK(db.remove("p2_tbl:bob").has_value());
}

void test_variables() {
    std::printf("connection: variables\n");
    auto db = open();
    if (!db.valid()) return;

    sdb::owned_value v(sr_value_int(7));
    CHECK(db.set("seven", sdb::value(v.get())).has_value());
    CHECK(db.unset("seven").has_value());
}

void test_errors_are_reported() {
    std::printf("connection: failures carry a message\n");
    auto db = open();
    if (!db.valid()) return;

    /* "THIS IS NOT SURQL" would be a poor choice: `IS NOT` is a real
       SurrealQL comparison operator, so it parses and succeeds. */
    auto bad = db.query("SELECT FROM WHERE");
    CHECK(!bad.has_value());
    if (!bad.has_value()) {
        CHECK(!bad.error().message().empty());
        CHECK(bad.error().code() != sdb::error_code::ok);
        // A query error is not fatal: the connection stays usable.
        CHECK(!db.poisoned());
        CHECK(db.health().has_value());
    }
}

// Transactions, through the only spelling that scopes.
//
// `db.begin()` is `= delete`d: sr_begin sends its own one-statement query, so
// the transaction opens and closes inside that call and a later write is not in
// it. Measured -- begin, write, cancel, and the row survives, with every call
// reporting success. Statements have to share a query instead.
int table_rows(sdb::connection& db, const char* table) {
    auto r = db.select(table);
    if (!r) return -1;
    return sdb::view(r.value()).size();
}

void test_transaction_commit() {
    std::printf("transaction: COMMIT in one query persists\n");
    auto db = open();
    if (!db.valid()) return;
    (void)db.query("DEFINE TABLE tx_c SCHEMALESS");
    (void)db.query("DELETE tx_c");

    auto r = db.query("BEGIN; CREATE tx_c:a SET v = 1; CREATE tx_c:b SET v = 2; COMMIT;");
    CHECK(r.has_value());
    CHECK(table_rows(db, "tx_c") == 2);
}

void test_transaction_cancel_rolls_back() {
    std::printf("transaction: CANCEL in one query rolls back\n");
    auto db = open();
    if (!db.valid()) return;
    (void)db.query("DEFINE TABLE tx_r SCHEMALESS");
    (void)db.query("DELETE tx_r");

    auto r = db.query("BEGIN; CREATE tx_r:a SET v = 1; CANCEL;");
    CHECK(r.has_value());
    CHECK(table_rows(db, "tx_r") == 0);
}

// The shape that looks like it works and does not.
//
// Kept as a test rather than a comment because it is the thing a reader will
// reach for first, and because if the C ever holds a transaction open across
// calls this starts failing -- which is the signal to un-delete begin().
void test_separate_calls_do_not_scope() {
    std::printf("transaction: separate calls are not scoped\n");
    auto db = open();
    if (!db.valid()) return;
    (void)db.query("DEFINE TABLE tx_s SCHEMALESS");
    (void)db.query("DELETE tx_s");

    CHECK(db.query("BEGIN;").has_value());
    sdb::object_builder rec;
    rec.set("v", 1);
    CHECK(db.create_discarding("tx_s:a", rec).has_value());
    CHECK(db.query("CANCEL;").has_value());

    // Not rolled back: the BEGIN's transaction ended with its own query.
    CHECK(table_rows(db, "tx_s") == 1);
}

void test_auth_surface() {
    std::printf("connection: auth surface compiles and reports\n");
    auto db = open();
    if (!db.valid()) return;

    // Root signin on an embedded instance may or may not be available; what
    // must hold is that it returns a result rather than crashing, and that a
    // malformed token is refused.
    auto tok = db.signin(sdb::scope::root, {"root", "root"});
    if (tok.has_value()) {
        CHECK(tok.value().get() != nullptr);
    }

    auto bad = db.authenticate("not-a-jwt");
    CHECK(!bad.has_value());
    if (!bad.has_value()) CHECK(!bad.error().message().empty());
}

void test_record_auth_with_params() {
    std::printf("connection: record auth carries custom fields\n");
    auto db = open();
    if (!db.valid()) return;
    if (!db.use("p2_auth_ns", "p2_auth_db")) { std::printf("  use failed\n"); return; }

    // A SIGNUP query is free to read any field, so user/password is the common
    // case and not the shape. The C++ wrapper used to drop the params object
    // entirely -- it passed a hardcoded null -- which made every access method
    // that reads more than credentials unreachable. This is the regression pin.
    auto def = db.query(
        "DEFINE TABLE p2_user SCHEMALESS PERMISSIONS FULL;"
        "DEFINE ACCESS p2_account ON DATABASE TYPE RECORD"
        "  SIGNUP ( CREATE p2_user SET email = $email,"
        "           pass = crypto::argon2::generate($pass),"
        "           nickname = $nickname, age = $age )"
        "  SIGNIN ( SELECT * FROM p2_user WHERE email = $email"
        "           AND crypto::argon2::compare(pass, $pass) )"
        "  DURATION FOR SESSION 1h;", nullptr);
    CHECK(def.has_value());
    if (!def) return;
    CHECK(def.value().all_ok());
    if (!def.value().all_ok()) {
        auto bad = def.value().first_error();
        std::printf("  define failed: %.*s\n",
                    static_cast<int>(bad.message().size()), bad.message().data());
        return;
    }

    const sdb::access acc{"p2_auth_ns", "p2_auth_db", "p2_account"};

    sdb::object_builder up;
    up.set("email", "ada@example.com").set("pass", "s3cret")
      .set("nickname", "countess").set("age", 36);
    auto signed_up = db.signup(acc, up);
    CHECK(signed_up.has_value());
    if (!signed_up) {
        std::printf("  signup failed: %.*s\n",
                    static_cast<int>(signed_up.error().message().size()),
                    signed_up.error().message().data());
        return;
    }
    CHECK(signed_up.value().get() != nullptr);

    // The fields the SIGNUP query read must actually be on the record --
    // a token alone would not prove they were sent.
    auto rows = db.query("SELECT nickname, age FROM p2_user", nullptr);
    CHECK(rows.has_value());
    if (rows) {
        auto only = rows.value().single();
        CHECK(only.has_value());
        if (only && only.value().size() == 1) {
            auto obj = only.value()[0].as_object();
            CHECK(obj.has_value());
            if (obj) {
                auto nick = obj->get("nickname");
                CHECK(nick && nick->as_string().value_or("") == "countess");
                auto age = obj->get("age");
                CHECK(age && age->as_int().value_or(0) == 36);
            }
        }
    }

    sdb::object_builder in;
    in.set("email", "ada@example.com").set("pass", "s3cret");
    auto signed_in = db.signin(acc, in);
    CHECK(signed_in.has_value());

    // A wrong password must be refused, not merely return an empty token.
    sdb::object_builder wrong;
    wrong.set("email", "ada@example.com").set("pass", "not-it");
    CHECK(!db.signin(acc, wrong).has_value());

    // The pointer-taking overload reaches the same place.
    auto explicit_form = db.signin(sdb::scope::record, sdb::credentials{}, &acc, &in);
    CHECK(explicit_form.has_value());
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P2 connection tests ===\n");
    std::printf("standard: %ld\n\n", static_cast<long>(SURREALDB_CPLUSPLUS));

    test_lifecycle();
    test_bad_endpoint();
    test_options();
    test_options_strings_outlive_the_call();
    test_operations();
    test_variables();
    test_errors_are_reported();
    test_transaction_commit();
    test_transaction_cancel_rolls_back();
    test_separate_calls_do_not_scope();
    test_auth_surface();
    test_record_auth_with_params();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
