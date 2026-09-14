// P11 tests: reading geometry and record ids.
//
// Both used to come back as raw C tagged unions -- `const sr_geometry_t*` and
// `const sr_thing_t*` -- leaving the caller to switch on a tag and reach into
// fields named `_0`, `_1` and `sr_geometry_multiline`. The views under test
// exist so that is never necessary; these checks walk every shape.

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

sdb::connection g_db;

[[nodiscard]] sdb::value view(const sdb::owned_value& v) noexcept { return sdb::value(v.get()); }

// ---------------------------------------------------------------------------

void test_every_shape() {
    std::printf("geometry: every shape reads back\n");

    const std::vector<sdb::make::coord> line{{0,0},{1,1},{2,0}};
    const std::vector<sdb::make::coord> ring{{0,0},{0,1},{1,1},{1,0},{0,0}};
    const std::vector<std::vector<sdb::make::coord>> lines{line, line};
    const std::vector<std::vector<sdb::make::coord>> polys{ring, ring};

    auto pt = sdb::make::point(13.41, 52.52);
    auto g = sdb::geometry(view(pt));
    CHECK(g.kind() == sdb::geometry_kind::point);
    auto c = g.as_point();
    CHECK(c && c->x == 13.41 && c->y == 52.52);
    // A wrong guess is empty, not garbage.
    CHECK(!g.as_polygon().has_value());
    CHECK(!g.as_collection().has_value());

    auto ls = sdb::make::linestring(line);
    auto gl = sdb::geometry(view(ls));
    CHECK(gl.kind() == sdb::geometry_kind::linestring);
    auto l = gl.as_linestring();
    CHECK(l && l->size() == 3);
    if (l && l->size() == 3) {
        CHECK((*l)[2].x == 2.0);
        int n = 0;
        for (sdb::coord xy : *l) { (void)xy; ++n; }
        CHECK(n == 3);
    }

    auto poly = sdb::make::polygon(ring);
    auto gp = sdb::geometry(view(poly));
    CHECK(gp.kind() == sdb::geometry_kind::polygon);
    auto p = gp.as_polygon();
    CHECK(p && p->exterior().size() == 5);
    CHECK(p && !p->has_holes());

    // A polygon with holes, so `interiors()` is read with something in it.
    const std::vector<sdb::make::coord> hole{{2,2},{2,3},{3,3},{3,2},{2,2}};
    const std::vector<std::vector<sdb::make::coord>> rings{ring, hole};
    auto holed = sdb::make::polygon(rings);
    auto hp = sdb::geometry(view(holed)).as_polygon();
    CHECK(hp && hp->has_holes());
    CHECK(hp && hp->interiors().size() == 1);
    if (hp && hp->interiors().size() == 1) {
        CHECK(hp->interiors()[0].size() == 5);
        int n = 0;
        for (sdb::linestring_ref r : hp->interiors()) { (void)r; ++n; }
        CHECK(n == 1);
    }

    auto mp = sdb::make::multipoint(line);
    auto gm = sdb::geometry(view(mp));
    CHECK(gm.kind() == sdb::geometry_kind::multipoint);
    auto pts = gm.as_multipoint();
    CHECK(pts && pts->size() == 3);
    if (pts && pts->size() == 3) CHECK((*pts)[1].x() == 1.0);

    auto mls = sdb::make::multilinestring(lines);
    auto gml = sdb::geometry(view(mls));
    CHECK(gml.kind() == sdb::geometry_kind::multilinestring);
    auto ml = gml.as_multilinestring();
    CHECK(ml && ml->size() == 2);
    if (ml && ml->size() == 2) CHECK((*ml)[0].size() == 3);

    auto mpoly = sdb::make::multipolygon(polys);
    auto gmp = sdb::geometry(view(mpoly));
    CHECK(gmp.kind() == sdb::geometry_kind::multipolygon);
    auto mpv = gmp.as_multipolygon();
    CHECK(mpv && mpv->size() == 2);
    if (mpv && mpv->size() == 2) CHECK((*mpv)[0].exterior().size() == 5);

    auto coll = sdb::make::collection({view(pt), view(ls), view(poly)});
    auto gc = sdb::geometry(view(coll));
    CHECK(gc.kind() == sdb::geometry_kind::collection);
    auto members = gc.as_collection();
    CHECK(members && members->size() == 3);
    if (members && members->size() == 3) {
        CHECK((*members)[0].kind() == sdb::geometry_kind::point);
        CHECK((*members)[1].kind() == sdb::geometry_kind::linestring);
        CHECK((*members)[2].kind() == sdb::geometry_kind::polygon);
    }
}

void test_invalid_and_wrong_kind() {
    std::printf("geometry: non-geometry values yield an invalid ref\n");

    auto s = sdb::make::string("not a geometry");
    auto g = sdb::geometry(view(s));
    CHECK(!g.valid());
    CHECK(!static_cast<bool>(g));
    CHECK(!g.as_point().has_value());
    CHECK(g.raw() == nullptr);

    // A default-constructed ref behaves the same rather than dereferencing.
    sdb::geometry_ref none;
    CHECK(!none.valid());
    CHECK(!none.as_collection().has_value());
}

void test_visit() {
    std::printf("geometry: visit dispatches on shape\n");

    struct namer {
        const char* operator()(sdb::coord) const { return "point"; }
        const char* operator()(sdb::linestring_ref) const { return "linestring"; }
        const char* operator()(sdb::polygon_ref) const { return "polygon"; }
        const char* operator()(sdb::point_span) const { return "multipoint"; }
        const char* operator()(sdb::linestring_span) const { return "multilinestring"; }
        const char* operator()(sdb::polygon_span) const { return "multipolygon"; }
        const char* operator()(sdb::geometry_span) const { return "collection"; }
        const char* operator()(std::monostate) const { return "unknown"; }
    };

    const std::vector<sdb::make::coord> line{{0,0},{1,1}};
    const std::vector<std::vector<sdb::make::coord>> lines{line};

    auto pt = sdb::make::point(1, 2);
    CHECK(std::string(sdb::geometry(view(pt)).visit(namer{})) == "point");
    auto ls = sdb::make::linestring(line);
    CHECK(std::string(sdb::geometry(view(ls)).visit(namer{})) == "linestring");
    auto pg = sdb::make::polygon(line);
    CHECK(std::string(sdb::geometry(view(pg)).visit(namer{})) == "polygon");
    auto mp = sdb::make::multipoint(line);
    CHECK(std::string(sdb::geometry(view(mp)).visit(namer{})) == "multipoint");
    auto ml = sdb::make::multilinestring(lines);
    CHECK(std::string(sdb::geometry(view(ml)).visit(namer{})) == "multilinestring");
    auto mpoly = sdb::make::multipolygon(lines);
    CHECK(std::string(sdb::geometry(view(mpoly)).visit(namer{})) == "multipolygon");
    auto coll = sdb::make::collection({view(pt)});
    CHECK(std::string(sdb::geometry(view(coll)).visit(namer{})) == "collection");

    // Every shape this C API knows is covered above, so nothing should reach
    // the monostate arm today. It exists for a SurrealDB that adds one.
}

void test_record_ids() {
    std::printf("thing: all four id kinds\n");
    if (!g_db.valid()) { std::printf("  no connection; skipped\n"); return; }

    auto probe = [&](const char* stmt, sdb::object_builder* vars) -> sdb::owned_values {
        auto r = g_db.query(stmt, vars);
        if (!r) return {};
        // Copy the row out: the results die with this scope otherwise.
        return {};
    };
    (void)probe;

    struct kase { const char* stmt; sdb::id_kind want; const char* label; };
    const kase cases[] = {
        { "RETURN type::record('person', 'ada')", sdb::id_kind::text,   "text" },
        { "RETURN type::record('person', 123)",   sdb::id_kind::number, "number" },
        { "RETURN type::record('person', ['a', 1])", sdb::id_kind::array,  "array" },
        { "RETURN type::record('person', { k: 'v' })", sdb::id_kind::object, "object" },
    };

    for (const kase& k : cases) {
        auto r = g_db.query(k.stmt, nullptr);
        if (!r) { std::printf("  %s: query failed\n", k.label); ++g_failures; ++g_checks; continue; }
        auto rows = r.value().single();
        if (!rows || rows.value().empty()) {
            std::printf("  %s: no rows\n", k.label); ++g_failures; ++g_checks; continue;
        }
        sdb::value v = rows.value()[0];
        check(v.kind() == sdb::value_kind::thing, k.label, __LINE__);

        auto t = sdb::thing(v);
        check(t.valid(), k.label, __LINE__);
        check(t.table() == "person", k.label, __LINE__);
        check(t.kind() == k.want, k.label, __LINE__);

        switch (k.want) {
            case sdb::id_kind::text:
                check(t.as_text().value_or("") == "ada", k.label, __LINE__);
                check(!t.as_number().has_value(), k.label, __LINE__);
                break;
            case sdb::id_kind::number:
                check(t.as_number().value_or(0) == 123, k.label, __LINE__);
                check(!t.as_text().has_value(), k.label, __LINE__);
                break;
            case sdb::id_kind::array: {
                auto a = t.as_array();
                check(a.has_value() && a->size() == 2, k.label, __LINE__);
                if (a && a->size() == 2)
                    check((*a)[1].as_int().value_or(0) == 1, k.label, __LINE__);
                break;
            }
            case sdb::id_kind::object: {
                auto o = t.as_object();
                check(o.has_value(), k.label, __LINE__);
                if (o) {
                    auto got = o->get("k");
                    check(got && got->as_string().value_or("") == "v", k.label, __LINE__);
                }
                break;
            }
        }
    }
}

void test_thing_visit_and_invalid() {
    std::printf("thing: visit, and non-things\n");

    auto s = sdb::make::string("nope");
    auto t = sdb::thing(view(s));
    CHECK(!t.valid());
    CHECK(t.table().empty());
    CHECK(!t.as_number().has_value());

    auto made = sdb::make::thing("person", "ada");
    auto tt = sdb::thing(view(made));
    CHECK(tt.valid());
    CHECK(tt.table() == "person");
    CHECK(tt.kind() == sdb::id_kind::text);
    CHECK(tt.as_text().value_or("") == "ada");

    struct namer {
        const char* operator()(std::int64_t) const { return "number"; }
        const char* operator()(std::string_view) const { return "text"; }
        const char* operator()(sdb::array_view) const { return "array"; }
        const char* operator()(sdb::object_view) const { return "object"; }
    };
    CHECK(std::string(tt.visit(namer{})) == "text");
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P11 geometry and record-id tests ===\n");
    std::printf("standard: %ld\n\n", static_cast<long>(SURREALDB_CPLUSPLUS));

    sdb::options opts;
    opts.capabilities().allow_arbitrary_query = sdb::target_set::all();
    auto made = sdb::connection::connect("memory", opts);
    if (made) {
        g_db = std::move(made).value();
        (void)g_db.use("p11_ns", "p11_db");
    } else {
        std::printf("connect failed: %.*s\n",
                    static_cast<int>(made.error().message().size()),
                    made.error().message().data());
    }

    test_every_shape();
    test_invalid_and_wrong_kind();
    test_visit();
    test_record_ids();
    test_thing_visit_and_invalid();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
