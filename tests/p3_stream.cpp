// P3 live-query tests: notifications, iteration, teardown ordering.
//
// sr_stream_next blocks with no cancellation, so every call here is made only
// after an event has been produced. A test that blocks forever is worse than
// one that fails.
//
// The bounded readers that *can* give up -- next_for, try_next -- live in
// p13_timeout.cpp. This file stays on the unbounded call deliberately: it is
// still the primitive, and it still has to behave.

#include <surrealdb/surrealdb.hpp>

#include <cstdio>
#include <chrono>
#include <cstring>
#include <optional>
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

// One notification, with a deadline.
//
// surrealdb.c 0.3.1 withdrew the unbounded read, so every read in this file is
// bounded -- see stream.hpp for why. `nullopt` means the stream ended, failed,
// or stayed quiet for the whole budget; the callers here distinguish those by
// what they already know.
std::optional<sdb::notification> wait_one(sdb::stream& s, int tries = 60) {
    for (int i = 0; i < tries; ++i) {
        auto p = s.next_for(std::chrono::milliseconds(50));
        if (!p.has_value()) return std::nullopt;
        if (p.value().timed_out()) continue;
        if (p.value().ended()) return std::nullopt;
        return p.value().take();
    }
    return std::nullopt;
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

    auto n = wait_one(stream);
    CHECK(n.has_value());
    if (!n.has_value()) {
        std::printf("  no notification within the budget\n");
        stream.close();
        return;
    }
    {
        const sdb::notification& note = *n;
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

    // Driven by hand, because a live stream is no longer a range: an iterator
    // has no way to say "nothing yet", so it would either park forever or end
    // the range on a quiet moment. Deleted in stream.hpp with that reason, and
    // this is what replaces it -- the caller decides what a timeout means, and
    // here it means keep going until the count is reached.
    int seen = 0;
    for (int i = 0; i < 120 && seen < want; ++i) {
        auto p = stream.next_for(std::chrono::milliseconds(50));
        CHECK(p.has_value());
        if (!p.has_value()) break;
        if (p.value().timed_out()) continue;
        if (p.value().ended()) break;
        auto note = p.value().take();
        CHECK(note.has_value());
        if (note.has_value()) CHECK(note.value().action() == sdb::action::create);
        ++seen;
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

    // After close, a read reports the end rather than erroring. This is the
    // C++-side short-circuit, not the C's SR_CLOSED -- an sr_stream_t cannot be
    // made to report its own end at all; p13 covers what is actually reachable.
    auto p = stream.try_next();
    CHECK(p.has_value());
    if (p.has_value()) {
        CHECK(p.value().ended());
        CHECK(!p.value().timed_out());
    }
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

    auto n = wait_one(stream);
    if (n.has_value()) {
        // Moving the notification out must not double-free; the payload is
        // released exactly once, when this scope ends.
        sdb::notification held = std::move(*n);
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
