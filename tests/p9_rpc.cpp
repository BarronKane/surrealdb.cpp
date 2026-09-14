// P9 tests: the RPC context, sessions, and the notification stream.
//
// These do not decode CBOR -- the library does not ship a codec, and neither
// does this test. A minimal request is hand-encoded (it is a handful of bytes)
// and replies are checked for being non-empty and well-framed. What is being
// tested is the C++ side: lifetimes, the session model, and that a stream
// outliving its context does not take the process with it.

#include <surrealdb/surrealdb.hpp>

#include <cstdio>
#include <string>
#include <vector>

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

// CBOR for {"id": 1, "method": <name>, "params": [...]}, hand-encoded.
//
// Written out by hand rather than pulled from a library, because that is the
// position a caller is in: this wrapper deliberately ships no CBOR codec, and
// a test that assumed one would be testing something users do not have.
void cbor_text(std::vector<std::uint8_t>& b, const std::string& s) {
    const std::size_t n = s.size();
    if (n < 24) {
        b.push_back(static_cast<std::uint8_t>(0x60 | n));
    } else if (n < 256) {
        b.push_back(0x78);                                  // text, 1-byte length
        b.push_back(static_cast<std::uint8_t>(n));
    } else {
        b.push_back(0x79);                                  // text, 2-byte length
        b.push_back(static_cast<std::uint8_t>(n >> 8));
        b.push_back(static_cast<std::uint8_t>(n & 0xFF));
    }
    for (char c : s) b.push_back(static_cast<std::uint8_t>(c));
}

std::vector<std::uint8_t> request(const std::string& method,
                                  const std::vector<std::string>& params = {}) {
    std::vector<std::uint8_t> b;
    b.push_back(0xA3);                                      // map(3)
    cbor_text(b, "id");     b.push_back(0x01);
    cbor_text(b, "method"); cbor_text(b, method);
    cbor_text(b, "params");
    b.push_back(static_cast<std::uint8_t>(0x80 | params.size()));
    for (const auto& p : params) cbor_text(b, p);
    return b;
}


// Count the per-statement `status` values in a reply without decoding it.
//
// The library ships no CBOR codec and neither does this test, but a CBOR text
// key is its bytes preceded by a length, so "status" followed by a short-string
// header is unambiguous enough to read the outcome of each statement.
std::vector<std::string> statuses(const sdb::owned_byte_array& reply) {
    std::vector<std::string> out;
    const std::string s(reinterpret_cast<const char*>(reply.data()),
                        static_cast<std::size_t>(reply.size()));
    for (std::size_t pos = s.find("status"); pos != std::string::npos;
         pos = s.find("status", pos)) {
        pos += 6;
        if (pos >= s.size()) break;
        const auto head = static_cast<std::uint8_t>(s[pos]);
        if ((head & 0xE0) == 0x60) {
            const std::size_t len = head & 0x1F;
            if (pos + 1 + len <= s.size()) out.push_back(s.substr(pos + 1, len));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------

void test_session_id() {
    std::printf("rpc: session_id\n");

    sdb::session_id zero;
    CHECK(zero.empty());
    CHECK(zero.to_string() == "00000000-0000-0000-0000-000000000000");

    const std::uint8_t raw[16] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                                  0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10};
    sdb::session_id id(raw);
    CHECK(!id.empty());
    CHECK(id.to_string() == "01234567-89ab-cdef-fedc-ba9876543210");

    sdb::session_id same(raw);
    CHECK(id == same);
    CHECK(id != zero);

    // Copyable, unlike most things here -- it is sixteen bytes with no owner.
    sdb::session_id copy = id;
    CHECK(copy == id);
}

void test_connect_and_execute(sdb::rpc& ctx) {
    std::printf("rpc: execute\n");

    auto req = request("version");
    auto res = ctx.execute(req);
    CHECK(res.has_value());
    if (!res) { report("execute", res.error()); return; }
    CHECK(!res.value().empty());

    // The container overload and the pointer/length one must agree.
    auto res2 = ctx.execute(req.data(), static_cast<int>(req.size()));
    CHECK(res2.has_value());
    if (res2) CHECK(res2.value().size() == res.value().size());

    // A malformed request must be reported, not silently accepted.
    const std::uint8_t junk[3] = {0xFF, 0xFF, 0xFF};
    auto bad = ctx.execute(junk, 3);
    CHECK(!bad.has_value());
}

void test_sessions(sdb::rpc& ctx) {
    std::printf("rpc: sessions\n");

    // The context mints one for itself at construction.
    auto def = ctx.default_session();
    CHECK(def.has_value());
    if (!def) { report("default_session", def.error()); return; }
    CHECK(!def.value().empty());

    auto before = ctx.sessions();
    CHECK(before.has_value());
    const int n_before = before ? before.value().size() : 0;
    CHECK(n_before >= 1);

    // A generated id.
    auto made = ctx.attach();
    CHECK(made.has_value());
    if (!made) { report("attach", made.error()); return; }
    sdb::session_id s1 = made.value();
    CHECK(!s1.empty());
    CHECK(s1 != def.value());

    // An id the caller chose.
    const std::uint8_t chosen_raw[16] = {0xaa,0xbb,0xcc,0xdd,0,0,0,0,0,0,0,0,0,0,0,1};
    sdb::session_id chosen(chosen_raw);
    auto took = ctx.attach(chosen);
    CHECK(took.has_value());
    if (took) CHECK(took.value() == chosen);

    // Attaching the same id twice is an error, not a silent no-op.
    auto again = ctx.attach(chosen);
    CHECK(!again.has_value());

    auto listed = ctx.sessions();
    CHECK(listed.has_value());
    if (listed) {
        CHECK(listed.value().size() == n_before + 2);
        // Every id we made must appear.
        CHECK(listed.value().contains(s1));
        CHECK(listed.value().contains(chosen));

        // And the list iterates as session_id, not as the C uuid struct.
        int seen = 0;
        for (sdb::session_id got : listed.value()) {
            CHECK(got.to_string().size() == 36);
            ++seen;
        }
        CHECK(seen == listed.value().size());
    }

    // Requests can be routed to a specific session.
    auto req = request("version");
    auto on_s1 = ctx.execute_on(s1, req);
    CHECK(on_s1.has_value());
    if (!on_s1) report("execute_on", on_s1.error());
    else CHECK(!on_s1.value().empty());

    // Reset keeps the id valid but clears what the session had selected.
    auto use_on_s1 = ctx.execute_on(s1, request("use", {"p9_ns", "p9_db"}));
    CHECK(use_on_s1.has_value());
    if (!use_on_s1) report("use on s1", use_on_s1.error());

    auto info_before = ctx.execute_on(s1, request("info"));
    CHECK(info_before.has_value());

    auto reset = ctx.reset(s1);
    CHECK(reset.has_value());
    if (!reset) report("reset", reset.error());

    // The id still works -- but the namespace selection is gone, which is what
    // makes reset different from detach.
    auto info_after = ctx.execute_on(s1, request("info"));
    CHECK(!info_after.has_value());
    auto still_there = ctx.sessions();
    CHECK(still_there.has_value() && still_there.value().contains(s1));

    // Detach removes it.
    auto gone = ctx.detach(chosen);
    CHECK(gone.has_value());
    if (!gone) report("detach", gone.error());

    auto final_list = ctx.sessions();
    CHECK(final_list.has_value());
    if (final_list) CHECK(!final_list.value().contains(chosen));

    // Executing on a detached session must fail rather than fall back to the
    // default one -- silently using the wrong session is the bug class the 3.1
    // session rework exists to prevent.
    //
    // Probed with `info` rather than `version`: `version` is answered locally
    // without ever consulting the session, so it succeeds on an id that was
    // never attached and proves nothing about isolation.
    auto probe = request("info");
    auto orphan = ctx.execute_on(chosen, probe);
    CHECK(!orphan.has_value());

    // And an id that was never attached at all is refused outright.
    const std::uint8_t never_raw[16] = {9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9};
    sdb::session_id never(never_raw);
    auto unknown = ctx.execute_on(never, probe);
    CHECK(!unknown.has_value());

    // The live session still answers, once it has a namespace again.
    auto reuse = ctx.execute_on(s1, request("use", {"p9_ns", "p9_db"}));
    CHECK(reuse.has_value());
    auto ok_on_s1 = ctx.execute_on(s1, probe);
    CHECK(ok_on_s1.has_value());
    if (!ok_on_s1) report("info on s1", ok_on_s1.error());
}

void test_query_over_rpc(sdb::rpc& ctx) {
    std::printf("rpc: query\n");

    // `query` returns DbResult::Query rather than DbResult::Other, and for a
    // while that made it the one RPC method a C caller could not use at all.
    // surrealdb.c c91b703 encodes every variant; this is the regression pin.
    (void)ctx.execute(request("use", {"p9_ns", "p9_db"}));

    auto one = ctx.execute(request("query", {"RETURN 1"}));
    CHECK(one.has_value());
    if (!one) { report("query", one.error()); return; }
    CHECK(!one.value().empty());
    CHECK(statuses(one.value()) == std::vector<std::string>{"OK"});

    // One entry per statement, in order.
    auto three = ctx.execute(request("query", {"RETURN 1; RETURN 2; RETURN 3"}));
    CHECK(three.has_value());
    if (three) CHECK(statuses(three.value()).size() == 3);

    // Two error channels, and they are not interchangeable.
    //
    // A parse error fails the call outright -- nothing ran, and the diagnostic
    // is the error message.
    auto parse = ctx.execute(request("query", {"SELECT * FROM"}));
    CHECK(!parse.has_value());

    // A runtime error succeeds *here* and is reported per statement, and the
    // statements after it still run. A caller checking only has_value() would
    // read this as a clean success, which is why it is documented.
    auto mixed = ctx.execute(request("query", {"RETURN 1; THROW 'boom'; RETURN 3"}));
    CHECK(mixed.has_value());
    if (mixed) {
        CHECK(statuses(mixed.value()) ==
              (std::vector<std::string>{"OK", "ERR", "OK"}));
    }
}

void test_notifications_shutdown() {
    std::printf("rpc: notification stream shutdown\n");

    auto made = sdb::rpc::connect("memory");
    CHECK(made.has_value());
    if (!made) { report("connect", made.error()); return; }
    sdb::rpc ctx = std::move(made).value();

    auto ns = ctx.notifications();
    CHECK(ns.has_value());
    if (!ns) { report("notifications", ns.error()); return; }
    sdb::rpc_stream s = std::move(ns).value();
    CHECK(s.valid());
    CHECK(!s.done());

    // Closing the stream by hand, with the context still alive, is the ordinary
    // path and must free rather than leak.
    s.close();
    CHECK(s.done());

    // next() on a closed stream is empty, not a crash and not an error.
    auto after = s.next();
    CHECK(after.has_value());
    if (after) CHECK(after.value().empty());
}

void test_stream_outliving_context() {
    std::printf("rpc: stream outliving its context\n");

    sdb::rpc_stream orphan;
    {
        auto made = sdb::rpc::connect("memory");
        CHECK(made.has_value());
        if (!made) return;
        sdb::rpc ctx = std::move(made).value();

        auto ns = ctx.notifications();
        CHECK(ns.has_value());
        if (!ns) return;
        orphan = std::move(ns).value();
        CHECK(orphan.valid());
    }
    // The context is gone. Freeing the stream now would run on a runtime that
    // no longer exists, so the destructor declines and leaks instead. What
    // must not happen is a use-after-free, which is what this scope exit is
    // really testing -- under ASan.
    CHECK(!orphan.valid());
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P9 RPC tests ===\n");
    std::printf("standard: %ld\n\n", static_cast<long>(SURREALDB_CPLUSPLUS));

    test_session_id();

    auto made = sdb::rpc::connect("memory");
    if (!made) {
        std::printf("rpc connect failed: %.*s\n",
                    static_cast<int>(made.error().message().size()),
                    made.error().message().data());
        return 1;
    }
    sdb::rpc ctx = std::move(made).value();
    CHECK(ctx.valid());
    CHECK(!ctx.poisoned());

    test_connect_and_execute(ctx);
    test_sessions(ctx);
    test_query_over_rpc(ctx);
    ctx.disconnect();
    CHECK(!ctx.valid());

    test_notifications_shutdown();
    test_stream_outliving_context();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
