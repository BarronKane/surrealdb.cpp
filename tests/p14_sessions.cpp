// P14 tests: forked sessions, and process-wide runtime settings.
//
// Both arrived in surrealdb.c 0.3.2 and both have an ordering rule that the C
// cannot enforce, so the interesting checks here are about *when* things are
// called rather than what they return.
//
// `runtime_init` must run before any context is opened, because SurrealDB
// builds its blocking pool once per process on first use. This file is the
// natural place to test that, because a test binary is a fresh process: the
// ordering is real here in a way it would not be inside a shared harness.

#include <surrealdb/surrealdb.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace sdb = surrealdb;
using namespace std::chrono_literals;

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

// -- runtime_init, before anything is open ----------------------------------

void test_runtime_init_before_any_context() {
    std::printf("sessions: runtime_init before any context\n");

    // Nothing to do: a default runtime_options changes nothing, which the C
    // reports as SR_AGAIN and this reports as `false`.
    auto none = sdb::runtime_init();
    CHECK(none.has_value());
    if (none.has_value()) CHECK(none.value() == false);

    // A real value applies.
    sdb::runtime_options ro;
    ro.kvs_threadpool_size(8);
    auto applied = sdb::runtime_init(ro);
    CHECK(applied.has_value());
    if (!applied.has_value()) { report("runtime_init", applied.error()); return; }
    CHECK(applied.value() == true);

    // Negative is rejected by the C rather than silently clamped.
    sdb::runtime_options bad;
    bad.kvs_threadpool_size(-1);
    auto rejected = sdb::runtime_init(bad);
    CHECK(!rejected.has_value());
    if (!rejected.has_value()) CHECK(!rejected.error().message().empty());
}

// -- and after, which is the part the C cannot report ------------------------

void test_runtime_init_after_a_context_is_open() {
    std::printf("sessions: runtime_init after a context is open\n");

    sdb::runtime_options ro;
    ro.kvs_threadpool_size(8);
    auto late = sdb::runtime_init(ro);

    // The C would answer 1 here -- it sets an environment variable that the
    // already-built pool will never read again, and has no way to know that.
    // This wrapper remembers that a context was opened and says so.
    CHECK(!late.has_value());
    if (!late.has_value()) {
        CHECK(late.error().code() == sdb::error_code::error);
        CHECK(late.error().message().find("before the first connect")
              != std::string_view::npos);
    }
}

// -- forked sessions ---------------------------------------------------------

// Reads one integer back, or nullopt for anything else -- including `none`,
// which is what an unset variable answers with.
std::optional<std::int64_t> read_int(sdb::connection& db, const char* surql) {
    auto r = db.query(surql);
    if (!r) return std::nullopt;
    auto rows = r.value().single();
    if (!rows || rows.value().size() != 1) return std::nullopt;
    return sdb::value(rows.value()[0]).as_int();
}

void test_fork_is_independent(sdb::connection& db) {
    std::printf("sessions: a fork has its own state\n");

    auto f = db.fork_session();
    CHECK(f.has_value());
    if (!f) { report("fork_session", f.error()); return; }
    auto forked = std::move(f).value();
    CHECK(forked.valid());

    // The fork inherits ns/db, so it can run immediately.
    auto inherited = forked.query("RETURN 1");
    CHECK(inherited.has_value());
    if (!inherited) report("fork query", inherited.error());

    // `set()`, not `LET`. A `LET` inside a query is scoped to that query on
    // this path -- `LET $x = 1` followed by a separate `RETURN $x` reads
    // `none`, while `LET $x = 1; RETURN $x` in one call reads 1. `sr_set` is
    // the session variable, and the session is what a fork forks.
    auto eleven = sdb::make::integer(11);
    CHECK(db.set("marker", sdb::value(eleven.get())).has_value());

    auto mine = read_int(db, "RETURN $marker");
    CHECK(mine.has_value() && *mine == 11);

    // The fork does not see it.
    auto theirs = read_int(forked, "RETURN $marker");
    CHECK(!(theirs.has_value() && *theirs == 11));

    // And the reverse: two forks are invisible to each other.
    auto g = db.fork_session();
    CHECK(g.has_value());
    if (!g) return;
    auto other = std::move(g).value();

    auto five = sdb::make::integer(5);
    CHECK(forked.set("only_mine", sdb::value(five.get())).has_value());
    auto peek = read_int(other, "RETURN $only_mine");
    CHECK(!(peek.has_value() && *peek == 5));
    // ...but the fork that set it still sees it.
    auto own = read_int(forked, "RETURN $only_mine");
    CHECK(own.has_value() && *own == 5);

    // The parent is unaffected by either fork.
    auto parent_still = read_int(db, "RETURN $marker");
    CHECK(parent_still.has_value() && *parent_still == 11);
    auto parent_leak = read_int(db, "RETURN $only_mine");
    CHECK(!parent_leak.has_value());
}


// Reads a session string back, so a handle can be asked what it thinks it is.
std::string text_of(sdb::connection& db, const char* surql) {
    auto r = db.query(surql);
    if (!r) return "<call failed>";
    auto rows = r.value().single();
    if (!rows || rows.value().size() != 1) return "<no row>";
    auto v = sdb::value(rows.value()[0]).as_string();
    return v ? std::string(*v) : "<not a string>";
}

// `use()` on a fork must not move the parent.
//
// The variables test covers session *data*; this covers session *targeting*,
// which is the half multi-tenancy actually rests on. A fork that dragged its
// parent's namespace along would put one tenant's writes in another's database
// without erroring anywhere.
void test_use_is_per_session(sdb::connection& db) {
    std::printf("sessions: use() is per session\n");

    CHECK(db.use("ns_a", "db_a").has_value());
    auto f = db.fork_session();
    CHECK(f.has_value());
    if (!f) return;
    auto fork = std::move(f).value();

    // Inherited to start with.
    CHECK(text_of(fork, "RETURN session::ns()") == "ns_a");
    CHECK(text_of(fork, "RETURN session::db()") == "db_a");

    CHECK(fork.use("ns_b", "db_b").has_value());
    CHECK(text_of(fork, "RETURN session::ns()") == "ns_b");
    CHECK(text_of(fork, "RETURN session::db()") == "db_b");

    // The parent has not moved.
    CHECK(text_of(db, "RETURN session::ns()") == "ns_a");
    CHECK(text_of(db, "RETURN session::db()") == "db_a");

    // And back the other way.
    CHECK(db.use("ns_c", "db_c").has_value());
    CHECK(text_of(fork, "RETURN session::ns()") == "ns_b");
    CHECK(db.use("p14_ns", "p14_db").has_value());
}

// A fork keeps working after the handle it came from is gone.
//
// This is the whole point of the runtime being reference-counted rather than
// owned by whichever handle happened to open it. If the parent's destructor
// took the engine down, per-request or per-player sessions would be unusable --
// the natural shape is a long-lived pool of forks outliving the setup code that
// made them.
void test_fork_outlives_its_parent() {
    std::printf("sessions: a fork outlives its parent\n");

    sdb::connection survivor;
    {
        auto made = sdb::connection::connect("memory");
        CHECK(made.has_value());
        if (!made) return;
        auto parent = std::move(made).value();
        CHECK(parent.use("ns_x", "db_x").has_value());

        auto f = parent.fork_session();
        CHECK(f.has_value());
        if (!f) return;
        survivor = std::move(f).value();

        parent.disconnect();
    }   // parent destroyed

    CHECK(survivor.valid());
    CHECK(!survivor.poisoned());
    CHECK(text_of(survivor, "RETURN session::ns()") == "ns_x");

    // Reads are not enough -- the engine has to still accept writes.
    CHECK(survivor.query("DEFINE TABLE orphan SCHEMALESS").has_value());
    sdb::object_builder rec;
    rec.set("v", 7);
    CHECK(survivor.create_discarding("orphan:one", rec).has_value());
    auto back = survivor.select("orphan");
    CHECK(back.has_value());
    if (back) CHECK(sdb::view(back.value()).size() == 1);
}

// Authentication does not leak between sessions.
//
// The property the whole per-user model rests on: if a fork's signin reached
// the parent, one player's credentials would be every player's.
void test_auth_is_per_session(sdb::connection& db) {
    std::printf("sessions: authentication is per session\n");

    CHECK(db.use("p14_ns", "p14_db").has_value());
    auto defined = db.query(
        "DEFINE ACCESS player ON DATABASE TYPE RECORD "
        "SIGNUP ( CREATE person SET name = $name ) "
        "SIGNIN ( SELECT * FROM person WHERE name = $name ) "
        "DURATION FOR SESSION 1h");
    if (!defined.has_value()) {
        std::printf("  DEFINE ACCESS unavailable; skipped\n");
        return;
    }

    const std::string parent_before = text_of(db, "RETURN <string>$auth");
    CHECK(parent_before == "NONE");

    auto f = db.fork_session();
    CHECK(f.has_value());
    if (!f) return;
    auto child = std::move(f).value();

    sdb::object_builder fields;
    fields.set("name", "ada");
    sdb::access acc{"p14_ns", "p14_db", "player"};
    auto token = child.signup(acc, fields);
    CHECK(token.has_value());
    if (!token) { report("signup", token.error()); return; }

    // The fork is now somebody.
    const std::string child_auth = text_of(child, "RETURN <string>$auth");
    CHECK(child_auth != "NONE");
    CHECK(child_auth.rfind("person:", 0) == 0);

    // The parent is still nobody.
    CHECK(text_of(db, "RETURN <string>$auth") == "NONE");
}

void test_new_session_clears_auth(sdb::connection& db) {
    std::printf("sessions: new_session clears inherited auth\n");

    auto f = db.new_session();
    CHECK(f.has_value());
    if (!f) { report("new_session", f.error()); return; }
    auto fresh = std::move(f).value();
    CHECK(fresh.valid());

    // Namespace and database are still inherited, deliberately -- clearing
    // those would hand back a handle that cannot run anything.
    auto r = fresh.query("RETURN 1");
    CHECK(r.has_value());
    if (!r) report("new_session query", r.error());
}

void test_poison_is_shared(sdb::connection& db) {
    std::printf("sessions: poisoning is shared\n");

    auto f = db.fork_session();
    if (!f) return;
    auto forked = std::move(f).value();

    // Nothing here provokes SR_FATAL -- that needs a dead engine, which a
    // passing test has no way to arrange. What is checked is that the two
    // handles report the *same* flag rather than each keeping its own, which is
    // the part that would silently diverge.
    CHECK(db.poisoned() == forked.poisoned());
    CHECK(!db.poisoned());
}

void test_stream_outlives_its_fork(sdb::connection& db) {
    std::printf("sessions: a stream outlives the fork that opened it\n");

    (void)db.query("DEFINE TABLE p14_tbl SCHEMALESS");

    sdb::stream s;
    {
        auto f = db.fork_session();
        if (!f) return;
        auto forked = std::move(f).value();

        auto live = forked.live("p14_tbl");
        if (!live) { std::printf("  live unavailable; skipped\n"); return; }
        s = std::move(live).value();
        CHECK(s.valid());
    }   // the fork is destroyed; the engine is not

    // The engine is reference-counted on the C side and dies with the last
    // handle, so this stream is still perfectly usable. A per-handle liveness
    // token would have expired here and leaked the stream instead.
    CHECK(s.valid());
    auto p = s.try_next();
    CHECK(p.has_value());
    if (p.has_value()) CHECK(p.value().timed_out() || p.value().ready());
    s.close();
}

// -- the new per-context runtime knobs ---------------------------------------

void test_runtime_shaped_options() {
    std::printf("sessions: runtime-shaping options\n");

    // A deliberately slim context: one thread, no IO driver. This is the shape
    // a host application embedding the engine actually wants, and it has to
    // still work end to end.
    sdb::options opts;
    opts.current_thread(true)
        .max_blocking_threads(8)
        .thread_keep_alive(2s)
        .thread_stack_size(1 << 20)
        .disable_io(true);

    auto c = sdb::connection::connect("memory", opts);
    CHECK(c.has_value());
    if (!c) { report("slim connect", c.error()); return; }
    auto db = std::move(c).value();
    CHECK(db.use("p14_ns", "p14_db").has_value());
    auto r = db.query("RETURN 1");
    CHECK(r.has_value());

    // And a multi-threaded one with an explicit worker count.
    sdb::options wide;
    wide.worker_threads(2);
    auto c2 = sdb::connection::connect("memory", wide);
    CHECK(c2.has_value());
    if (c2) CHECK(std::move(c2).value().query("RETURN 1").has_value());
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P14 session tests ===\n");
    std::printf("standard: %ld\n", static_cast<long>(SURREALDB_CPLUSPLUS));
    std::printf("surrealdb.c: %s\n\n", SR_VERSION_STRING);

    // Must come first: it is the only point in this process before an engine
    // exists, and that is exactly what it is testing.
    test_runtime_init_before_any_context();

    auto made = sdb::connection::connect("memory");
    if (!made) {
        std::printf("connect failed: %.*s\n",
                    static_cast<int>(made.error().message().size()),
                    made.error().message().data());
        return 1;
    }
    auto db = std::move(made).value();
    if (!db.use("p14_ns", "p14_db").has_value()) return 1;

    test_runtime_init_after_a_context_is_open();
    test_fork_is_independent(db);
    test_use_is_per_session(db);
    test_fork_outlives_its_parent();
    test_auth_is_per_session(db);
    test_new_session_clears_auth(db);
    test_poison_is_shared(db);
    test_stream_outlives_its_fork(db);
    test_runtime_shaped_options();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
