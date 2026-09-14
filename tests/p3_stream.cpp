// P3 live-query tests: notifications, iteration, teardown ordering.
//
// sr_stream_next blocks with no timeout and no cancellation, so every call
// here is made only after an event has been produced. A test that blocks
// forever is worse than one that fails.

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
    if (!c.has_value()) return sdb::connection();
    auto db = std::move(c).value();
    (void)db.use("p3_ns", "p3_db");
    return db;
}

// A live query needs its table to exist first -- SurrealDB rejects one against
// an undefined table, which is not something the stream API can report later.
sdb::result<sdb::stream> live_on(sdb::connection& db, const char* table) {
    char stmt[128];
    std::snprintf(stmt, sizeof(stmt), "DEFINE TABLE %s SCHEMALESS", table);
    (void)db.query(stmt);
    return db.live(table);
}

// Produce an event the blocked reader is guaranteed to pick up.
void touch(sdb::connection& db, const char* id) {
    sdb::object_builder rec;
    rec.set("v", 1);
    (void)db.create_discarding(id, rec);
}

void test_open_and_close() {
    std::printf("stream: open and close\n");
    auto db = open();
    if (!db.valid()) { std::printf("  no connection; skipped\n"); return; }

    auto s = live_on(db, "p3_tbl");
    CHECK(s.has_value());
    if (!s.has_value()) {
        std::printf("  live unavailable: %.*s\n",
                    static_cast<int>(s.error().message().size()),
                    s.error().message().data());
        return;
    }
    auto stream = std::move(s).value();
    CHECK(stream.valid());
    CHECK(!stream.done());
    CHECK(stream.failure() == nullptr);

    stream.close();
    CHECK(stream.done());
    stream.close();   // idempotent
}

void test_receives_a_notification() {
    std::printf("stream: receives a notification\n");
    auto db = open();
    if (!db.valid()) return;

    auto s = live_on(db, "p3_tbl");
    if (!s.has_value()) { std::printf("  live unavailable; skipped\n"); return; }
    auto stream = std::move(s).value();

    touch(db, "p3_tbl:one");

    auto n = stream.next();
    CHECK(n.has_value());
    if (!n.has_value()) {
        std::printf("  next failed: %.*s\n",
                    static_cast<int>(n.error().message().size()),
                    n.error().message().data());
        stream.close();
        return;
    }
    CHECK(n.value().has_value());
    if (n.value().has_value()) {
        const sdb::notification& note = *n.value();
        CHECK(note.action() == sdb::action::create);
        CHECK(std::strcmp(sdb::to_string(note.action()), "create") == 0);
        CHECK(note.query_id().bytes != nullptr);
        // The payload is a live value, not SR_VALUE_NONE.
        CHECK(!note.data().is_none());
    }

    stream.close();
}

void test_iteration() {
    std::printf("stream: range-for over queued events\n");
    auto db = open();
    if (!db.valid()) return;

    auto s = live_on(db, "p3_iter");
    if (!s.has_value()) { std::printf("  live unavailable; skipped\n"); return; }
    auto stream = std::move(s).value();

    // Queue a known number of events, then read exactly that many. The loop
    // must not run past them -- there is no timeout to rescue it.
    const int want = 3;
    for (int i = 0; i < want; ++i) {
        char id[64];
        std::snprintf(id, sizeof(id), "p3_iter:%d", i);
        touch(db, id);
    }

    // Break *inside* the body rather than testing a counter in the loop
    // condition: a for-loop runs `++it` before re-testing, so the counter
    // version blocks forever waiting for an event after the last one. That is
    // a property of the stream, not of this test -- see stream.hpp.
    int seen = 0;
    for (auto it = stream.begin(); it != stream.end(); ++it) {
        CHECK(it->action() == sdb::action::create);
        if (++seen == want) break;
    }
    CHECK(seen == want);

    stream.close();
}

void test_end_of_stream_is_not_success() {
    std::printf("stream: a closed stream reports done, not a value\n");
    auto db = open();
    if (!db.valid()) return;

    auto s = live_on(db, "p3_end");
    if (!s.has_value()) return;
    auto stream = std::move(s).value();

    stream.close();

    // After close, next() reports exhaustion rather than blocking or erroring.
    auto n = stream.next();
    CHECK(n.has_value());
    CHECK(n.has_value() && !n.value().has_value());
    CHECK(stream.done());
}

void test_notification_owns_its_payload() {
    std::printf("stream: a notification owns its data\n");
    auto db = open();
    if (!db.valid()) return;

    auto s = live_on(db, "p3_own");
    if (!s.has_value()) return;
    auto stream = std::move(s).value();

    touch(db, "p3_own:one");

    auto n = stream.next();
    if (n.has_value() && n.value().has_value()) {
        // Moving the notification out of the result must not double-free; the
        // payload is released exactly once, when this scope ends.
        sdb::notification held = std::move(n).value().value();
        CHECK(held.data().kind() != sdb::value_kind::none);
    }
    stream.close();
}

void test_move_only() {
    std::printf("stream: move-only\n");
    static_assert(!std::is_copy_constructible<sdb::stream>::value,
                  "stream must be move-only");
    static_assert(std::is_move_constructible<sdb::stream>::value,
                  "stream must be movable");
    static_assert(!std::is_copy_constructible<sdb::notification>::value,
                  "notification must be move-only");

    auto db = open();
    if (!db.valid()) return;
    auto s = live_on(db, "p3_move");
    if (!s.has_value()) return;

    auto a = std::move(s).value();
    sdb::stream b = std::move(a);
    CHECK(b.valid());
    CHECK(!a.valid());     // moved-from is inert, not dangling
    b.close();
}

void test_ordered_teardown() {
    std::printf("stream: destroyed before its connection\n");
    // The natural spelling is also the correct one: a stream declared after
    // its connection is destroyed first. This is the ordering the C API
    // requires -- kill the stream, then disconnect.
    auto db = open();
    if (!db.valid()) return;
    {
        auto s = live_on(db, "p3_order");
        if (!s.has_value()) return;
        auto stream = std::move(s).value();
        CHECK(stream.valid());
    }   // stream dies here, connection outlives it
    CHECK(db.health().has_value());
}

void test_kill_by_id() {
    std::printf("stream: kill by query id\n");
    auto db = open();
    if (!db.valid()) return;

    // A well-formed but unknown id should be reported, not crash.
    auto r = db.kill("00000000-0000-0000-0000-000000000000");
    CHECK(r.has_value() || !r.error().message().empty());
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P3 stream tests ===\n");
    std::printf("standard: %ld\n\n", static_cast<long>(SURREALDB_CPLUSPLUS));

    test_open_and_close();
    test_receives_a_notification();
    test_iteration();
    test_end_of_stream_is_not_success();
    test_notification_owns_its_payload();
    test_move_only();
    test_ordered_teardown();
    test_kill_by_id();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
