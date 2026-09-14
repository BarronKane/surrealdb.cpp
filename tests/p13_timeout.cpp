// P13 tests: bounded waits.
//
// surrealdb.c 0.2.6 added `sr_stream_next_timeout` and
// `sr_rpc_stream_next_timeout`, which is the first time a reader in this
// library can give up. The interesting part is not that a timeout happens --
// it is that a timeout is kept distinct from the end of the stream, in both
// directions:
//
//   - a timeout reported as an end abandons a stream over a quiet moment
//   - an end reported as a timeout spins forever on a stream that is finished
//
// Every check below is ultimately about that line staying drawn. The duration
// conversion gets the same treatment, because `500us` truncating to 0 turns a
// bounded wait into a busy spin and nothing downstream would notice.

#include <surrealdb/surrealdb.hpp>

#include <chrono>
#include <cstdio>
#include <limits>
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

sdb::connection open() {
    auto c = sdb::connection::connect("memory");
    if (!c.has_value()) return sdb::connection();
    auto db = std::move(c).value();
    (void)db.use("p13_ns", "p13_db");
    return db;
}

sdb::result<sdb::stream> live_on(sdb::connection& db, const char* table) {
    char stmt[128];
    std::snprintf(stmt, sizeof(stmt), "DEFINE TABLE %s SCHEMALESS", table);
    (void)db.query(stmt);
    return db.live(table);
}

void touch(sdb::connection& db, const char* id) {
    sdb::object_builder rec;
    rec.set("v", 1);
    (void)db.create_discarding(id, rec);
}

// -- the duration conversion ------------------------------------------------
//
// Pure, so it is checked at compile time as well as run time. A static_assert
// here is worth more than a runtime check: it cannot be skipped by a missing
// database.

void test_timeout_conversion() {
    std::printf("timeout: duration conversion\n");
    using sdb::detail::to_timeout_ms;
    constexpr int max_ms = (std::numeric_limits<int>::max)();

    // Ordinary values pass through.
    static_assert(to_timeout_ms(std::chrono::milliseconds(100)) == 100, "");
    static_assert(to_timeout_ms(std::chrono::seconds(2)) == 2000, "");
    CHECK(to_timeout_ms(100ms) == 100);

    // Zero is a poll, and stays a poll.
    static_assert(to_timeout_ms(std::chrono::milliseconds(0)) == 0, "");

    // Sub-millisecond rounds UP. Truncating to 0 would silently turn a bounded
    // wait into a non-blocking poll, i.e. a busy spin in any loop.
    static_assert(to_timeout_ms(std::chrono::microseconds(1)) == 1, "");
    static_assert(to_timeout_ms(std::chrono::microseconds(500)) == 1, "");
    static_assert(to_timeout_ms(std::chrono::microseconds(1500)) == 2, "");
    static_assert(to_timeout_ms(std::chrono::nanoseconds(1)) == 1, "");

    // Negative clamps to a poll, never to "wait forever".
    static_assert(to_timeout_ms(std::chrono::seconds(-5)) == 0, "");
    static_assert(to_timeout_ms(std::chrono::milliseconds(-1)) == 0, "");

    // Overflow clamps to INT_MAX. Wrapping would produce a negative int, which
    // the C API reads as an unbounded wait -- a bounded wait becoming infinite
    // is the one failure mode here that hangs rather than returns.
    static_assert(to_timeout_ms(std::chrono::hours(24 * 365)) == max_ms, "");
    static_assert(to_timeout_ms(std::chrono::hours(1000000)) == max_ms, "");
    CHECK(to_timeout_ms(std::chrono::hours(24 * 365)) == max_ms);

    // A whole-second duration at the boundary does not tip over.
    CHECK(to_timeout_ms(std::chrono::milliseconds(max_ms)) == max_ms);

    // Floating representations work, and round up like the rest.
    CHECK(to_timeout_ms(std::chrono::duration<double, std::milli>(1.5)) == 2);
    CHECK(to_timeout_ms(std::chrono::duration<double>(-1.0)) == 0);
}

// -- poll<T> ----------------------------------------------------------------

void test_poll_states() {
    std::printf("timeout: poll states\n");

    // The default is `ended` -- the state that stops a loop. A default poll
    // read by mistake must not spin.
    sdb::poll<int> d;
    CHECK(d.ended());
    CHECK(!d.ready());
    CHECK(!d.timed_out());
    CHECK(!static_cast<bool>(d));

    sdb::poll<int> t(sdb::poll_state::timed_out);
    CHECK(t.timed_out());
    CHECK(!t.ended());
    CHECK(!t.ready());
    // A timeout is not a success to be unwrapped.
    CHECK(!static_cast<bool>(t));

    sdb::poll<int> r(7);
    CHECK(r.ready());
    CHECK(static_cast<bool>(r));
    CHECK(r.value() == 7);
    CHECK(*r == 7);

    CHECK(std::string(sdb::to_string(sdb::poll_state::ready)) == "ready");
    CHECK(std::string(sdb::to_string(sdb::poll_state::timed_out)) == "timed_out");
    CHECK(std::string(sdb::to_string(sdb::poll_state::ended)) == "ended");
}

void test_poll_take() {
    std::printf("timeout: poll take\n");

    sdb::poll<std::string> p(std::string("payload"));
    auto v = p.take();
    CHECK(v.has_value());
    CHECK(*v == "payload");

    // take() consumes the poll's claim, so there is nothing left to hand out
    // a second time -- which is what makes it safe where value() on a
    // temporary is not.
    CHECK(p.ended());
    CHECK(!p.take().has_value());

    sdb::poll<std::string> t(sdb::poll_state::timed_out);
    CHECK(!t.take().has_value());
    // take() on a timeout must not claim the stream ended: a caller that
    // checks state after taking still has to see "call again".
    CHECK(t.timed_out());
}

void test_error_code_timeout() {
    std::printf("timeout: error_code mirror\n");

    // Mirrored so the enum stays a complete map of the C codes...
    CHECK(static_cast<int>(sdb::error_code::timeout) == SR_TIMEOUT);
    CHECK(std::string(sdb::to_string(sdb::error_code::timeout)) == "timeout");
    // ...but it is not a success, and the readers below prove it never
    // actually reaches an error.
    CHECK(!sdb::is_ok(sdb::error_code::timeout));
}

// -- stream -----------------------------------------------------------------

void test_try_next_on_idle_stream() {
    std::printf("timeout: try_next on an idle stream\n");
    auto db = open();
    if (!db.valid()) { std::printf("  no connection; skipped\n"); return; }

    auto s = live_on(db, "p13_idle");
    if (!s.has_value()) { std::printf("  live unavailable; skipped\n"); return; }
    auto stream = std::move(s).value();

    auto p = stream.try_next();
    CHECK(p.has_value());                 // a timeout is not an error
    if (!p.has_value()) return;
    CHECK(p.value().timed_out());
    CHECK(!p.value().ended());
    // The critical one: a timeout leaves the stream usable. Marking it done
    // here would abandon a live query over nothing at all.
    CHECK(!stream.done());
}

void test_next_for_times_out() {
    std::printf("timeout: next_for expires\n");
    auto db = open();
    if (!db.valid()) { std::printf("  no connection; skipped\n"); return; }

    auto s = live_on(db, "p13_wait");
    if (!s.has_value()) { std::printf("  live unavailable; skipped\n"); return; }
    auto stream = std::move(s).value();

    const auto start = std::chrono::steady_clock::now();
    auto p = stream.next_for(120ms);
    const auto waited = std::chrono::steady_clock::now() - start;

    CHECK(p.has_value());
    if (!p.has_value()) return;
    CHECK(p.value().timed_out());
    // It really waited rather than falling straight through. Generous lower
    // bound: this asserts a bounded wait happened, not the scheduler's
    // accuracy.
    CHECK(waited >= 50ms);
    CHECK(!stream.done());
}

void test_next_for_receives() {
    std::printf("timeout: next_for receives\n");
    auto db = open();
    if (!db.valid()) { std::printf("  no connection; skipped\n"); return; }

    auto s = live_on(db, "p13_recv");
    if (!s.has_value()) { std::printf("  live unavailable; skipped\n"); return; }
    auto stream = std::move(s).value();

    touch(db, "p13_recv:a");

    // Poll in a loop, exactly as the documented usage does. This is also the
    // check that an expired wait consumes nothing: the event may well arrive
    // after one or more timeouts, and it must still be delivered.
    bool got = false;
    int timeouts = 0;
    for (int i = 0; i < 40 && !got; ++i) {
        auto p = stream.next_for(50ms);
        CHECK(p.has_value());
        if (!p.has_value()) break;
        if (p.value().timed_out()) { ++timeouts; continue; }
        if (p.value().ended()) break;
        auto n = p.value().take();
        CHECK(n.has_value());
        if (n.has_value()) CHECK(n.value().action() == sdb::action::create);
        got = true;
    }
    CHECK(got);
    std::printf("  (%d timeout%s before the event)\n",
                timeouts, timeouts == 1 ? "" : "s");
}

// The claim that an expired wait consumes nothing, tested directly rather
// than left to timing luck.
//
// `test_next_for_receives` above usually finds the event already queued on its
// first poll, so it never actually crosses a timeout. This one forces the
// order: drain into several guaranteed timeouts *first*, and only then produce
// the event. If an expired wait took anything off the channel, or quietly
// finished the stream, the event that follows would never arrive.
void test_timeout_then_event() {
    std::printf("timeout: an expired wait consumes nothing\n");
    auto db = open();
    if (!db.valid()) { std::printf("  no connection; skipped\n"); return; }

    auto s = live_on(db, "p13_order");
    if (!s.has_value()) { std::printf("  live unavailable; skipped\n"); return; }
    auto stream = std::move(s).value();

    // Nothing has been written yet, so every one of these must time out.
    for (int i = 0; i < 5; ++i) {
        auto p = stream.try_next();
        CHECK(p.has_value());
        if (!p.has_value()) return;
        CHECK(p.value().timed_out());
        CHECK(!stream.done());
    }

    touch(db, "p13_order:a");

    bool got = false;
    for (int i = 0; i < 40 && !got; ++i) {
        auto p = stream.next_for(50ms);
        CHECK(p.has_value());
        if (!p.has_value()) return;
        if (p.value().timed_out()) continue;
        if (p.value().ended()) break;
        auto n = p.value().take();
        CHECK(n.has_value());
        got = n.has_value();
    }
    // The event survived five expired waits.
    CHECK(got);
}

void test_ended_is_not_timed_out() {
    std::printf("timeout: ended is not timed out\n");
    auto db = open();
    if (!db.valid()) { std::printf("  no connection; skipped\n"); return; }

    auto s = live_on(db, "p13_end");
    if (!s.has_value()) { std::printf("  live unavailable; skipped\n"); return; }
    auto stream = std::move(s).value();

    stream.close();
    auto p = stream.try_next();
    CHECK(p.has_value());
    if (!p.has_value()) return;
    // A closed stream reports `ended`, never `timed_out`. Reporting a timeout
    // here would spin a correct reader forever.
    CHECK(p.value().ended());
    CHECK(!p.value().timed_out());
    CHECK(stream.done());
}

// -- rpc_stream -------------------------------------------------------------

void test_rpc_stream_try_next() {
    std::printf("timeout: rpc_stream try_next\n");
    auto c = sdb::rpc::connect("memory");
    if (!c.has_value()) { std::printf("  no rpc context; skipped\n"); return; }
    auto ctx = std::move(c).value();

    auto n = ctx.notifications();
    if (!n.has_value()) { std::printf("  notifications unavailable; skipped\n"); return; }
    auto stream = std::move(n).value();

    auto p = stream.try_next();
    CHECK(p.has_value());
    if (!p.has_value()) return;
    CHECK(p.value().timed_out());
    CHECK(!stream.done());

    // And a bounded wait behaves the same way.
    auto q = stream.next_for(60ms);
    CHECK(q.has_value());
    if (q.has_value()) CHECK(q.value().timed_out());
    CHECK(!stream.done());
}

void test_rpc_stream_closed_is_ended() {
    std::printf("timeout: rpc_stream closed is ended\n");
    auto c = sdb::rpc::connect("memory");
    if (!c.has_value()) { std::printf("  no rpc context; skipped\n"); return; }
    auto ctx = std::move(c).value();

    auto n = ctx.notifications();
    if (!n.has_value()) { std::printf("  notifications unavailable; skipped\n"); return; }
    auto stream = std::move(n).value();

    stream.close();
    auto p = stream.try_next();
    CHECK(p.has_value());
    if (p.has_value()) {
        CHECK(p.value().ended());
        CHECK(!p.value().timed_out());
    }
}

// -- sessions ---------------------------------------------------------------

void test_detach_and_reset_accept_sessions() {
    std::printf("timeout: session detach and reset\n");
    auto c = sdb::rpc::connect("memory");
    if (!c.has_value()) { std::printf("  no rpc context; skipped\n"); return; }
    auto ctx = std::move(c).value();

    auto a = ctx.attach();
    if (!a.has_value()) { std::printf("  attach unavailable; skipped\n"); return; }
    auto id = std::move(a).value();
    CHECK(!id.empty());

    // 0.2.6 made both of these cancel the session's live queries, each
    // emitting a final `killed` notification. There are none registered here,
    // so what is checked is that the calls still succeed and the session
    // model is unchanged -- the notification itself is covered by
    // surrealdb.c's own suite, which can drive a live query over RPC.
    auto r = ctx.reset(id);
    CHECK(r.has_value());

    auto sessions = ctx.sessions();
    CHECK(sessions.has_value());
    if (sessions.has_value()) CHECK(sessions.value().contains(id));

    auto d = ctx.detach(id);
    CHECK(d.has_value());

    auto after = ctx.sessions();
    // A detached session is gone, not merely emptied.
    if (after.has_value()) CHECK(!after.value().contains(id));
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P13 bounded-wait tests ===\n");
    std::printf("standard: %ld\n", static_cast<long>(SURREALDB_CPLUSPLUS));
    std::printf("surrealdb.c: %s\n\n", SR_VERSION_STRING);

    test_timeout_conversion();
    test_poll_states();
    test_poll_take();
    test_error_code_timeout();
    test_try_next_on_idle_stream();
    test_next_for_times_out();
    test_next_for_receives();
    test_timeout_then_event();
    test_ended_is_not_timed_out();
    test_rpc_stream_try_next();
    test_rpc_stream_closed_is_ended();
    test_detach_and_reset_accept_sessions();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
