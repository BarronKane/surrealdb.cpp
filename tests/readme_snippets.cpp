// Every code block from README.md, transcribed the way a reader would type it.
//
// Compiled, never run -- the live-query loop blocks forever without a writer,
// and the point is not the behaviour anyway. The point is that the README does
// not drift out of compiling.
//
// This is not busywork. `dsl::f("age") > 30` was broken for weeks because
// `bind` only took `std::int64_t` and every test wrote an explicit cast; it
// surfaced the first time the README was written the way a user would write
// it. The same bug class turned up again in `array_builder::push`. A snippet
// nobody compiles is a snippet that is wrong.
#include <surrealdb/surrealdb.hpp>
#include <chrono>
#include <vector>
#include <string>
using namespace surrealdb;

static void handle(action, value) {}

int main() {
    auto conn = connection::connect("memory").value();
    auto& db = conn;
    std::string city_from_user = "berlin";

    // -- Querying --
    auto q = dsl::select("person")
                 .fields("name", "age")
                 .where(dsl::f("age") > 30 && dsl::f("city") == city_from_user)
                 .order_by("name")
                 .limit(10)
                 .build();
    auto results = db.run(q.value());
    if (results) {
        auto rows = results.value().single();
        if (rows) { for (value row : rows.value()) (void)row; }

        // The batch reader.
        for (statement_result st : results.value()) {
            if (!st.ok()) continue;
            for (value row : st.rows()) (void)row;
        }
        (void)results.value().all_ok();
        (void)results.value().first_error();
    }

    // -- Live queries --
    {
        auto s = db.live("person");
        auto stream = std::move(s).value();
        for (;;) {
            auto r = stream.next_for(std::chrono::milliseconds(250));
            if (!r) break;

            auto& p = r.value();
            if (p.ended()) break;
            if (p.timed_out()) continue;
            auto n = p.take();
            handle(n->action(), n->data());
        }
        stream.close();
    }

    // -- Building values --
    auto pt    = make::point(13.41, 52.52);
    auto when  = make::datetime("2026-09-13T10:00:00Z");
    auto exact = make::decimal("0.1");
    (void)pt; (void)when; (void)exact;

    std::vector<make::coord> ring{{0,0}, {0,1}, {1,1}, {1,0}, {0,0}};
    auto area = make::polygon(ring);
    (void)area;

    auto a2 = make::point(1.0, 2.0);
    auto b2 = make::polygon(ring);
    auto region2 = make::collection({value(a2.get()), value(b2.get())});
    (void)region2;

    std::vector<make::coord> hole{{0,0}, {0,1}, {1,0}, {0,0}};
    std::vector<std::vector<make::coord>> rings{ring, hole};
    auto with_hole = make::polygon(rings);
    auto region    = make::multipolygon(rings);
    (void)with_hole; (void)region;

    std::vector<owned_value> parts;
    parts.push_back(make::polygon(rings));
    parts.push_back(make::polygon(ring));
    auto composed = make::multipolygon(parts);
    (void)composed;

    auto r = make::range(make::included(make::integer(1)),
                         make::excluded(make::integer(10)));
    (void)r;

    // Values compare structurally, and debug_print exists.
    bool same = value(make::integer(1).get()) == value(make::integer(1).get());
    (void)same;
    debug_print(value(r.get()));

    // -- Building arrays --
    array_builder args;
    args.push("hello").push(1).push(3);

    array_builder tags;
    tags.push("a").push("b").push("c");
    object_builder rec2;
    rec2.set("name", "ada").set("tags", tags);
    rec2.set_unique("seen", tags);
    object_builder vars2;
    vars2.set("ids", make::array(tags));

    array_builder coords2;  coords2.push(1.0).push(2.0);
    array_builder ring2;    ring2.push(coords2).push(coords2);

    // -- Records, edges, patches --
    object_builder edge;
    edge.set("in", make::thing("person", "a")).set("out", make::thing("person", "b"));
    auto tag = make::string("x");

    (void)db.call("string::uppercase", args);
    (void)db.insert_relation("follows", edge);
    (void)db.patch_add("person:ada", "/tags/0", value(tag.get()));

    // -- Reading geometry and record ids --
    auto some_geom = make::collection({value(a2.get()), value(b2.get())});
    {
        value row = value(some_geom.get());
        auto g = geometry(row);
        switch (g.kind()) {
            case geometry_kind::point:
                if (auto cc = g.as_point()) { (void)cc->x; (void)cc->y; }
                break;
            case geometry_kind::polygon:
                if (auto pp = g.as_polygon()) {
                    for (coord xy : pp->exterior()) (void)xy;
                    for (auto hole : pp->interiors()) (void)hole;
                }
                break;
            case geometry_kind::collection:
                if (auto members = g.as_collection())
                    for (geometry_ref member : *members) (void)member;
                break;
            default: break;
        }
    }
    {
        auto rid = make::thing("person", "ada");
        value row = value(rid.get());
        auto t = thing(row);
        if (t) {
            (void)t.table();
            switch (t.kind()) {
                case id_kind::text:   (void)*t.as_text();   break;
                case id_kind::number: (void)*t.as_number(); break;
                case id_kind::array:
                    if (auto items = t.as_array())
                        for (value v : *items) (void)v;
                    break;
                case id_kind::object: (void)*t.as_object(); break;
            }
        }
    }

    // -- Auth with the fields the access method reads --
    {
        object_builder fields;
        fields.set("email", "ada@example.com").set("pass", "s3cret").set("nickname", "countess");
        access acc{"ns", "db", "account"};
        (void)db.signup(acc, fields);
    }

    // -- Bounded waits --
    //
    // Transcribed as the README has it. Compiled, never run, like the rest of
    // this file -- the loop is unbounded on purpose there.
    {
        auto live = db.live("person");
        if (live) {
            stream stream_ = std::move(live).value();
            bool shutting_down = false;
            for (;;) {
                auto r = stream_.next_for(std::chrono::milliseconds(100));
                if (!r) break;                          // the stream failed

                auto& p = r.value();
                if (p.ended()) break;                   // the stream finished
                if (p.timed_out()) {                    // nothing yet
                    if (shutting_down) break;
                    continue;
                }
                auto n = p.take();
                handle(n->action(), n->data());
            }
            (void)stream_.try_next();
        }
    }

    // -- RPC and sessions --
    auto ctx = rpc::connect("memory").value();
    auto session = ctx.attach().value();
    std::vector<std::uint8_t> request_bytes{0xA0};
    auto reply = ctx.execute_on(session, request_bytes);
    (void)reply;
    (void)ctx.execute(request_bytes);
    // -- Typed queries on a session --
    {
        object_builder qvars;
        qvars.set("id", "person:alice");
        auto qrows = ctx.query_on(session,
                                  "SELECT * FROM person WHERE id = $id", &qvars);
        (void)qrows;
    }

    (void)ctx.reset(session);
    (void)ctx.detach(session);
}
