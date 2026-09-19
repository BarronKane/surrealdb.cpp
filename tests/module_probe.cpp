// The breadth probe again, consumed through `import surrealdb;`.
//
// Its job is breadth, not depth: touch every header, instantiate every
// template, and exit. The per-phase suites test behaviour; this one answers
// "does the whole library still compile and run with these flags" -- which is
// the question that matters when a consumer sets the standard, not us.
//
// It must terminate unattended, so nothing here blocks: no live-query loop, no
// notification read. It must also build with `-fno-exceptions -fno-rtti`,
// which is why there is not a `try` or a `dynamic_cast` anywhere.

import surrealdb;

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace sdb = surrealdb;

namespace {

int g_failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}

void probe_values() {
    // Every constructor, so every one gets instantiated under these flags.
    auto vals = {
        sdb::make::none(), sdb::make::null(), sdb::make::boolean(true),
        sdb::make::integer(1), sdb::make::floating(1.5),
        sdb::make::decimal("1.5"), sdb::make::string("s"),
        sdb::make::datetime("2026-01-01T00:00:00Z"), sdb::make::duration(1, 2),
        sdb::make::table("t"), sdb::make::regex("^a$"),
        sdb::make::file("b", "k"), sdb::make::thing("t", "id"),
        sdb::make::array(), sdb::make::set(), sdb::make::point(1, 2),
    };
    expect(vals.size() == 16, "constructors");

    const std::uint8_t u[16] = {0};
    expect(sdb::value(sdb::make::uuid(u).get()).kind() == sdb::value_kind::uuid, "uuid");

    const std::vector<std::uint8_t> blob{1, 2, 3};
    expect(sdb::value(sdb::make::bytes(blob).get()).kind() == sdb::value_kind::bytes, "bytes");

    // Geometry, including the array-of-arrays templates.
    const std::vector<sdb::make::coord> ring{{0,0},{0,1},{1,1},{0,0}};
    const std::vector<std::vector<sdb::make::coord>> rings{ring, ring};
    expect(static_cast<bool>(sdb::make::linestring(ring)), "linestring");
    expect(static_cast<bool>(sdb::make::polygon(ring)), "polygon");
    expect(static_cast<bool>(sdb::make::multipoint(ring)), "multipoint");
    expect(static_cast<bool>(sdb::make::multilinestring(rings)), "multilinestring");
    expect(static_cast<bool>(sdb::make::multipolygon(rings)), "multipolygon");

    // Rings with holes, and a composed multipolygon. Needs 0.2.5.
    auto holed = sdb::make::polygon(rings);
    expect(sdb::geometry(sdb::value(holed.get())).as_polygon().value().has_holes(),
           "polygon with holes");
    std::vector<sdb::owned_value> parts;
    parts.push_back(sdb::make::polygon(rings));
    parts.push_back(sdb::make::polygon(ring));
    expect(sdb::geometry(sdb::value(sdb::make::multipolygon(parts).get()))
               .as_multipolygon().value().size() == 2,
           "composed multipolygon");

    // Collections: geometry values in, geometry value out. Needs 0.2.4.
    auto gp1 = sdb::make::point(1, 2);
    auto gp2 = sdb::make::point(3, 4);
    auto coll = sdb::make::collection({sdb::value(gp1.get()), sdb::value(gp2.get())});
    expect(sdb::value(coll.get()).kind() == sdb::value_kind::geometry, "collection");
    expect(static_cast<bool>(sdb::make::collection()), "empty collection");
    auto notgeom = sdb::make::string("x");
    expect(sdb::value(sdb::make::collection({sdb::value(gp1.get()),
                                             sdb::value(notgeom.get())}).get()).kind()
               == sdb::value_kind::none,
           "collection rejects non-geometry");

    // The typed geometry view, over every shape.
    expect(sdb::geometry(sdb::value(gp1.get())).kind() == sdb::geometry_kind::point,
           "geometry_ref point");
    expect(sdb::geometry(sdb::value(coll.get())).as_collection().value().size() == 2,
           "geometry_ref collection");
    expect(sdb::geometry(sdb::value(sdb::make::linestring(ring).get()))
               .as_linestring().value().size() == static_cast<int>(ring.size()),
           "geometry_ref linestring");
    expect(sdb::geometry(sdb::value(sdb::make::polygon(ring).get()))
               .as_polygon().value().exterior().size() == static_cast<int>(ring.size()),
           "geometry_ref polygon");
    expect(sdb::geometry(sdb::value(sdb::make::multipoint(ring).get()))
               .as_multipoint().value().size() == static_cast<int>(ring.size()),
           "geometry_ref multipoint");
    expect(sdb::geometry(sdb::value(sdb::make::multilinestring(rings).get()))
               .as_multilinestring().value().size() == static_cast<int>(rings.size()),
           "geometry_ref multilinestring");
    expect(sdb::geometry(sdb::value(sdb::make::multipolygon(rings).get()))
               .as_multipolygon().value().size() == static_cast<int>(rings.size()),
           "geometry_ref multipolygon");
    expect(!sdb::geometry(sdb::value(notgeom.get())).valid(), "geometry_ref invalid");

    // The typed record-id view.
    auto rid = sdb::make::thing("person", "ada");
    auto tr = sdb::thing(sdb::value(rid.get()));
    expect(tr.valid() && tr.table() == "person", "thing_ref table");
    expect(tr.kind() == sdb::id_kind::text, "thing_ref kind");
    expect(tr.as_text().value_or("") == "ada", "thing_ref text");

    // Bounds and ranges, including an abandoned bound.
    { auto dropped = sdb::make::included(sdb::make::integer(1)); (void)dropped; }
    auto r = sdb::make::range(sdb::make::included(sdb::make::integer(1)),
                              sdb::make::excluded(sdb::make::integer(9)));
    expect(sdb::value(r.get()).kind() == sdb::value_kind::range, "range");

    expect(sdb::value(sdb::make::integer(2).get()) == sdb::value(sdb::make::integer(2).get()),
           "equality");

    // Builders, with uncast literals -- the shape that was ambiguous before.
    sdb::array_builder arr;
    arr.push(1).push(2u).push(3L).push(1.5).push(1.5f).push(true)
       .push("s").push(std::string("s"));
    expect(arr.size() == 8, "array_builder");

    // The linear bulk path, through the container overload -- no C type named.
    std::vector<sdb::owned_value> held;
    for (int i = 0; i < 4; ++i) held.push_back(sdb::make::integer(i));
    auto bulk = sdb::array_from(held);
    expect(bulk && sdb::array_view(*bulk.get()).size() == 4, "array_from");
    expect(sdb::array_builder(sdb::array_view(*bulk.get())).size() == 4, "bulk ctor");

    // Populated containers, and nesting -- needs surrealdb.c 0.2.3+.
    sdb::array_builder items; items.push(1).push(2).push(3);
    expect(sdb::value(sdb::make::array(items).get()).as_array().value().size() == 3, "make::array");
    expect(sdb::value(sdb::make::set(items).get()).kind() == sdb::value_kind::set, "make::set");
    sdb::array_builder outer; outer.push(items);
    expect(sdb::value(sdb::make::array(outer).get()).as_array().value().size() == 1, "nested array");
    sdb::object_builder listed; listed.set("tags", items).set_unique("u", items);
    expect(listed.size() == 2, "array field");

    sdb::object_builder obj;
    obj.set("i", 1).set("u", 2u).set("l", 3L).set("d", 1.5).set("f", 1.5f)
       .set("b", true).set("s", "x").set("str", std::string("y"));
    expect(obj.size() == 8, "object_builder");
}

void probe_dsl() {
    auto q = sdb::dsl::select("person")
                 .fields("name", "age")
                 .where(sdb::dsl::f("age") > 30 && sdb::dsl::f("city") == "berlin")
                 .order_by("name")
                 .limit(10)
                 .build();
    expect(q.has_value(), "dsl build");

    // The guard surface, which shares its writer with the fluent one.
    // Literals uncast on purpose: that is how a user writes it.
    sdb::dsl::query gq;
    {
        auto sel = gq.select("person");
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
    expect(gq.build().has_value(), "dsl guards");
}

void probe_connection() {
    auto made = sdb::connection::connect("memory");
    expect(made.has_value(), "connect");
    if (!made) return;
    sdb::connection db = std::move(made).value();
    expect(db.use("probe_ns", "probe_db").has_value(), "use");

    sdb::object_builder rec;
    rec.set("name", "probe").set("n", 1);
    expect(db.create("probe_tbl:one", rec).has_value(), "create");
    expect(db.select("probe_tbl").has_value(), "select");

    sdb::array_builder args;
    args.push(std::string("x"));
    expect(db.call("string::uppercase", args).has_value(), "call");
    expect(db.call("time::now").has_value(), "call no args");

    auto tag = sdb::make::string("t");
    expect(db.patch_add("probe_tbl:one", "/tag", sdb::value(tag.get())).has_value(),
           "patch_add");
    expect(db.patch_replace("probe_tbl:one", "/tag", sdb::value(tag.get())).has_value(),
           "patch_replace");
    expect(db.patch_remove("probe_tbl:one", "/tag").has_value(), "patch_remove");

    sdb::object_builder edge;
    edge.set("in", sdb::make::thing("probe_tbl", "one"))
        .set("out", sdb::make::thing("probe_tbl", "one"));
    expect(db.insert_relation("probe_edge", edge).has_value(), "insert_relation");

    // Query results, both readers.
    auto q = db.query("RETURN 1; THROW 'x'; RETURN 3", nullptr);
    expect(q.has_value(), "query");
    if (q) {
        expect(q.value().size() == 3, "statement count");
        expect(!q.value().all_ok(), "all_ok");
        expect(!q.value().first_error().ok(), "first_error");
        expect(!q.value().single().has_value(), "single refuses a batch");
        int n = 0;
        for (sdb::statement_result st : q.value()) n += st.ok() ? 1 : 0;
        expect(n == 2, "iteration");
    }
    auto one = db.query("RETURN 1", nullptr);
    expect(one.has_value() && one.value().single().has_value(), "single");

    // Opened and closed without ever reading: next() would block.
    auto live = db.live("probe_tbl");
    expect(live.has_value(), "live");
    if (live) {
        sdb::stream s = std::move(live).value();
        // A forked session: same engine, its own state.
    {
        auto forked = db.fork_session();
        expect(forked.has_value(), "fork_session");
        if (forked) {
            auto f = std::move(forked).value();
            expect(f.valid(), "fork valid");
            expect(f.poisoned() == db.poisoned(), "fork shares poisoning");
            expect(f.query("RETURN 1").has_value(), "fork query");
        }
        auto fresh = db.new_session();
        expect(fresh.has_value(), "new_session");
    }

    // Bounded waits, through whichever spelling this probe is testing.
        // `try_next` cannot block, so it is safe here where a blocking read
        // would hang a probe that has produced no events.
        auto p = s.try_next();
        expect(p.has_value(), "stream try_next");
        if (p) expect(p.value().timed_out() || p.value().ended(), "stream poll state");
        auto q = s.next_for(std::chrono::milliseconds(1));
        expect(q.has_value(), "stream next_for");
        s.close();
    }

    // A real transaction handle: statements across separate calls, scoped
    // together, cancelled on scope exit unless committed.
    {
        auto begun = db.begin();
        expect(begun.has_value(), "begin");
        if (begun) {
            auto tx = std::move(begun).value();
            expect(tx.active(), "transaction active");
            expect(tx.query("CREATE probe_tx:a SET v = 1").has_value(), "tx query");
            expect(tx.cancel().has_value(), "tx cancel");
            expect(!tx.active(), "transaction consumed");
        }
    }
}

void probe_rpc() {
    auto made = sdb::rpc::connect("memory");
    expect(made.has_value(), "rpc connect");
    if (!made) return;
    sdb::rpc ctx = std::move(made).value();

    expect(ctx.default_session().has_value(), "default_session");

    auto s = ctx.attach();
    expect(s.has_value(), "attach");
    if (s) {
        auto listed = ctx.sessions();
        expect(listed.has_value() && listed.value().contains(s.value()), "sessions");
        if (listed) for (sdb::session_id id : listed.value()) expect(!id.empty(), "session id");
        expect(ctx.reset(s.value()).has_value(), "reset");
        expect(ctx.detach(s.value()).has_value(), "detach");
    }

    // Request bytes go in as bytes; nothing here decodes the reply.
    // {"id":1,"method":"query","params":["RETURN 1"]}, hand-encoded -- `query`
    // is the method that returns DbResult::Query, which was once unreachable.
    const std::vector<std::uint8_t> req{
        0xA3,
        0x62,'i','d', 0x01,
        0x66,'m','e','t','h','o','d', 0x65,'q','u','e','r','y',
        0x66,'p','a','r','a','m','s', 0x81, 0x68,'R','E','T','U','R','N',' ','1',
    };
    expect(ctx.execute(req).has_value(), "rpc query");

    // Forked sessions and the runtime knobs -- 0.3.2.
    {
        sdb::runtime_options ro;
        ro.kvs_threadpool_size(0);          // a no-op value, so ordering is moot
        (void)ro.to_c();
        expect(sdb::c_version >= sdb::required_c_version, "c version floor");
        // Not pinned to a value -- it tracks the anchored dependency. What is
        // pinned is that the constant and the macro agree, so a module
        // consumer reading the constant is told the same thing as a header
        // consumer reading the macro.
        expect(sdb::has_unbounded_stream_read ==
               (SURREALDB_HAS_UNBOUNDED_STREAM_READ != 0),
               "unbounded read flag mirrors its macro");
    }

    // Typed queries on a session -- 0.3.0's sr_rpc_query_on.
    {
        auto sid = ctx.attach();
        expect(sid.has_value(), "rpc attach");
        if (sid) {
            auto s = std::move(sid).value();
            sdb::object_builder vars;
            vars.set("n", 1);
            auto qr = ctx.query_on(s, "RETURN $n", &vars);
            expect(qr.has_value(), "rpc query_on");
            if (qr) expect(qr.value().size() == 1, "rpc query_on statements");
            expect(ctx.detach(s).has_value(), "rpc detach");
        }
    }

    // Opened and closed without reading, for the same reason as `live`.
    auto ns = ctx.notifications();
    expect(ns.has_value(), "notifications");
    if (ns) {
        sdb::rpc_stream st = std::move(ns).value();
        auto p = st.try_next();
        expect(p.has_value(), "rpc_stream try_next");
        if (p) expect(p.value().timed_out() || p.value().ended(), "rpc_stream poll state");
        auto q = st.next_for(std::chrono::milliseconds(1));
        expect(q.has_value(), "rpc_stream next_for");
        st.close();
    }
}


// poll<T> and the duration clamp, independent of any connection.
//
// Cheap, and the part of the new surface that must exist identically at every
// standard and through both spellings -- a module consumer gets these as
// exported names, not macros.
void probe_poll() {
    sdb::poll<int> ready(5);
    expect(ready.ready() && ready.value() == 5, "poll ready");

    sdb::poll<int> t(sdb::poll_state::timed_out);
    expect(t.timed_out() && !t.ended(), "poll timed_out");

    sdb::poll<int> d;
    expect(d.ended(), "poll default is ended");

    auto taken = ready.take();
    expect(taken.has_value() && *taken == 5 && ready.ended(), "poll take");

    expect(std::string(sdb::to_string(sdb::poll_state::timed_out)) == "timed_out",
           "poll_state to_string");
    // 0.3.0 removed error_code::timeout; a timeout is poll_state, not a code.
    expect(sdb::to_string(sdb::error_code::closed) != nullptr, "error_code closed");
}

} // namespace

int main() {
    // Through the module these are constants, not macros -- see config.hpp.
    std::printf("module probe: c++%ld ranges:%d span:%d expected:%d\n",
                sdb::cpp_standard, static_cast<int>(sdb::has_ranges),
                static_cast<int>(sdb::has_span),
                static_cast<int>(sdb::has_std_expected));

    probe_poll();
    probe_values();
    probe_dsl();
    probe_connection();
    probe_rpc();

    std::printf("%s\n", g_failures == 0 ? "ok" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
