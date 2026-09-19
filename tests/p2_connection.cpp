// P2 connection tests: lifecycle, options, operations, transactions, poisoning.

#include <surrealdb/surrealdb.hpp>

#include <cstdio>
#include <optional>
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

// Transactions.
//
// These assert against the *database*, not against return codes. The previous
// implementation returned success from all three calls and scoped nothing, so a
// suite that only checked return values passed against an API that did not
// work. Every check here reads rows back.
int table_rows(sdb::connection& db, const char* table) {
    auto r = db.select(table);
    if (!r) return -1;
    return sdb::view(r.value()).size();
}

sdb::connection tx_db(const char* table) {
    auto db = open();
    if (!db.valid()) return db;
    char stmt[128];
    std::snprintf(stmt, sizeof(stmt), "DEFINE TABLE %s SCHEMALESS", table);
    (void)db.query(stmt);
    std::snprintf(stmt, sizeof(stmt), "DELETE %s", table);
    (void)db.query(stmt);
    return db;
}

void test_transaction_commit_persists() {
    std::printf("transaction: commit persists\n");
    auto db = tx_db("tx_c");
    if (!db.valid()) return;

    auto b = db.begin();
    CHECK(b.has_value());
    if (!b) return;
    auto tx = std::move(b).value();
    CHECK(tx.active());

    CHECK(tx.query("CREATE tx_c:a SET v = 1").has_value());
    CHECK(tx.commit().has_value());
    CHECK(!tx.active());
    CHECK(table_rows(db, "tx_c") == 1);
}

void test_transaction_cancel_rolls_back() {
    std::printf("transaction: cancel rolls back\n");
    auto db = tx_db("tx_r");
    if (!db.valid()) return;

    auto b = db.begin();
    if (!b) return;
    auto tx = std::move(b).value();

    CHECK(tx.query("CREATE tx_r:a SET v = 1").has_value());
    CHECK(tx.cancel().has_value());
    CHECK(table_rows(db, "tx_r") == 0);
}

// The whole point of a handle: work spread over separate calls, with the
// caller's own code in between, still lands or rolls back together.
void test_transaction_spans_calls() {
    std::printf("transaction: spans separate calls atomically\n");
    auto db = tx_db("tx_s");
    if (!db.valid()) return;

    auto b = db.begin();
    if (!b) return;
    auto tx = std::move(b).value();

    CHECK(tx.query("CREATE tx_s:a SET v = 1").has_value());
    // Arbitrary caller code between statements -- this is what one big
    // BEGIN; ...; COMMIT; query cannot do.
    const int decided = table_rows(db, "tx_s");
    CHECK(tx.query("CREATE tx_s:b SET v = 2").has_value());
    CHECK(tx.commit().has_value());

    CHECK(decided == 0);                    // invisible while open
    CHECK(table_rows(db, "tx_s") == 2);     // both landed together
}

// Isolation, asserted from outside rather than inferred.
void test_transaction_is_invisible_until_commit() {
    std::printf("transaction: uncommitted writes are invisible outside\n");
    auto db = tx_db("tx_i");
    if (!db.valid()) return;

    auto b = db.begin();
    if (!b) return;
    auto tx = std::move(b).value();

    CHECK(tx.query("CREATE tx_i:a SET v = 1").has_value());

    // The connection that opened it cannot see the write...
    CHECK(table_rows(db, "tx_i") == 0);
    // ...but the transaction can.
    auto inside = tx.query("SELECT * FROM tx_i");
    CHECK(inside.has_value());
    if (inside) {
        auto rows = inside.value().single();
        CHECK(rows.has_value() && rows.value().size() == 1);
    }

    CHECK(tx.commit().has_value());
    CHECK(table_rows(db, "tx_i") == 1);
}

// Scope exit cancels. This is the guarantee the old implementation advertised
// and did not have, so it is asserted against rows rather than a flag.
void test_transaction_cancels_on_scope_exit() {
    std::printf("transaction: cancels when it falls out of scope\n");
    auto db = tx_db("tx_x");
    if (!db.valid()) return;

    {
        auto b = db.begin();
        if (!b) return;
        auto tx = std::move(b).value();
        CHECK(tx.query("CREATE tx_x:a SET v = 1").has_value());
        // No commit, no cancel -- an early return is the case this models.
    }

    CHECK(table_rows(db, "tx_x") == 0);
}

// A failed statement does not roll the transaction back by itself; the caller
// decides. That choice is the reason to hold a handle.
void test_failing_statement_leaves_the_choice() {
    std::printf("transaction: a failing statement leaves the choice open\n");
    auto db = tx_db("tx_f");
    if (!db.valid()) return;

    auto b = db.begin();
    if (!b) return;
    auto tx = std::move(b).value();

    CHECK(tx.query("CREATE tx_f:a SET v = 1").has_value());

    // Something the server will reject at runtime.
    auto bad = tx.query("SELECT * FROM type::table($nope)");
    (void)bad;   // either channel is acceptable; what matters is what follows

    // The transaction is still usable, and committing keeps the good write.
    CHECK(tx.active());
    auto more = tx.query("CREATE tx_f:b SET v = 2");
    if (more.has_value()) {
        CHECK(tx.commit().has_value());
        CHECK(table_rows(db, "tx_f") == 2);
    } else {
        CHECK(tx.cancel().has_value());
        CHECK(table_rows(db, "tx_f") == 0);
    }
}

// Both commit and cancel consume the handle, even when they fail -- so a second
// call has to be reported, not repeated against a freed pointer.
void test_use_after_completion_is_rejected() {
    std::printf("transaction: use after completion is rejected\n");
    auto db = tx_db("tx_u");
    if (!db.valid()) return;

    auto b = db.begin();
    if (!b) return;
    auto tx = std::move(b).value();
    CHECK(tx.commit().has_value());
    CHECK(!tx.active());

    auto again = tx.commit();
    CHECK(!again.has_value());
    if (!again) CHECK(!again.error().message().empty());

    auto cancelled = tx.cancel();
    CHECK(!cancelled.has_value());

    auto q = tx.query("CREATE tx_u:a SET v = 1");
    CHECK(!q.has_value());
    CHECK(table_rows(db, "tx_u") == 0);
}

// The transaction runs on a forked session, so it keeps the namespace and
// database it was opened with even if the connection moves afterwards.
void test_transaction_session_is_forked() {
    std::printf("transaction: keeps the session it was opened with\n");
    auto db = tx_db("tx_k");
    if (!db.valid()) return;

    auto b = db.begin();
    if (!b) return;
    auto tx = std::move(b).value();

    // Move the connection somewhere else entirely.
    CHECK(db.use("other_ns", "other_db").has_value());

    // The open transaction is unaffected.
    CHECK(tx.query("CREATE tx_k:a SET v = 1").has_value());
    CHECK(tx.commit().has_value());

    CHECK(db.use("p2_ns", "p2_db").has_value());
    CHECK(table_rows(db, "tx_k") == 1);
}

// A transaction whose connection is gone must not run against a dead runtime.
//
// `commit`, `cancel` and the destructor all go through the connection's tokio
// runtime, which dies with the last handle onto it. The library holds a
// liveness token and abandons the transaction rather than touching a freed
// runtime -- the same trade `stream` makes. The datastore times the
// transaction out; a use-after-free does not recover.
//
// Written because those branches existed with nothing reaching them. Under
// ASan this is where a mistake would show.
void test_transaction_outliving_its_connection() {
    std::printf("transaction: outliving its connection is abandoned, not crashed\n");

    std::optional<sdb::transaction> orphan;
    {
        auto made = sdb::connection::connect("memory");
        CHECK(made.has_value());
        if (!made) return;
        auto db = std::move(made).value();
        CHECK(db.use("p2_ns", "p2_db").has_value());
        (void)db.query("DEFINE TABLE tx_o SCHEMALESS");

        auto b = db.begin();
        CHECK(b.has_value());
        if (!b) return;
        orphan.emplace(std::move(b).value());
        CHECK(orphan->query("CREATE tx_o:a SET v = 1").has_value());
    }   // the connection is destroyed with the transaction still open

    // Every route has to report rather than run.
    auto q = orphan->query("CREATE tx_o:b SET v = 2");
    CHECK(!q.has_value());

    auto c = orphan->commit();
    CHECK(!c.has_value());
    if (!c) CHECK(c.error().code() == sdb::error_code::closed);
    CHECK(!orphan->active());

    // And the destructor, below, must not touch the dead runtime either.
    orphan.reset();
}

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
    test_transaction_commit_persists();
    test_transaction_cancel_rolls_back();
    test_transaction_spans_calls();
    test_transaction_is_invisible_until_commit();
    test_transaction_cancels_on_scope_exit();
    test_failing_statement_leaves_the_choice();
    test_use_after_completion_is_rejected();
    test_transaction_session_is_forked();
    test_transaction_outliving_its_connection();
    test_separate_calls_do_not_scope();
    test_auth_surface();
    test_record_auth_with_params();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
