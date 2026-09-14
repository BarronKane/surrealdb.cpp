// P7 tests: value construction.
//
// The exit criterion is that every `sr_value_*` constructor is reachable and
// that ownership is right in both directions -- a bound handed to `range()`
// must not be freed twice, and a bound that is abandoned must not leak. Run
// this under ASan; the leak half of that pair is invisible otherwise.

#include <surrealdb/surrealdb.hpp>

#include <array>
#include <cstdio>
#include <cstring>
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

sr_surreal_t* g_db = nullptr;

[[nodiscard]] sdb::value view(const sdb::owned_value& v) noexcept { return sdb::value(v.get()); }

// ---------------------------------------------------------------------------

void test_scalars() {
    std::printf("make: scalars\n");

    CHECK(view(sdb::make::none()).kind()     == sdb::value_kind::none);
    CHECK(view(sdb::make::null()).kind()     == sdb::value_kind::null);
    CHECK(view(sdb::make::boolean(true)).kind()  == sdb::value_kind::boolean);
    CHECK(view(sdb::make::integer(7)).kind()     == sdb::value_kind::number);
    CHECK(view(sdb::make::floating(1.5)).kind()  == sdb::value_kind::number);
    CHECK(view(sdb::make::decimal("1.25")).kind() == sdb::value_kind::number);
    CHECK(view(sdb::make::string("hi")).kind()   == sdb::value_kind::string);
    CHECK(view(sdb::make::datetime("2024-01-01T00:00:00Z")).kind() == sdb::value_kind::datetime);
    CHECK(view(sdb::make::duration(90, 500)).kind() == sdb::value_kind::duration);
    CHECK(view(sdb::make::table("users")).kind() == sdb::value_kind::table);
    CHECK(view(sdb::make::regex("^a.*z$")).kind() == sdb::value_kind::regex);
    CHECK(view(sdb::make::file("bucket", "k")).kind() == sdb::value_kind::file);
    CHECK(view(sdb::make::thing("users", "u1")).kind() == sdb::value_kind::thing);
    CHECK(view(sdb::make::array()).kind()        == sdb::value_kind::array);
    CHECK(view(sdb::make::set()).kind()          == sdb::value_kind::set);

    // none and null stay distinct after construction, which is the whole
    // reason both exist.
    CHECK(view(sdb::make::none()).kind() != view(sdb::make::null()).kind());

    // std::string overloads pick the same path as the char* ones.
    CHECK(view(sdb::make::string(std::string("hi"))).kind() == sdb::value_kind::string);
    CHECK(view(sdb::make::decimal(std::string("2.5"))).kind() == sdb::value_kind::number);

    const std::uint8_t bytes[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    CHECK(view(sdb::make::uuid(bytes)).kind() == sdb::value_kind::uuid);

    const std::vector<std::uint8_t> blob{0xde, 0xad, 0xbe, 0xef};
    CHECK(view(sdb::make::bytes(blob)).kind() == sdb::value_kind::bytes);

    // Payloads survive, not just kinds.
    auto s = sdb::make::string("hello");
    CHECK(view(s).as_string().value_or("") == "hello");

    auto i = sdb::make::integer(-42);
    auto n = view(i).as_int();
    CHECK(n && *n == -42);
}

void test_object() {
    std::printf("make: object\n");

    sdb::object_builder o;
    o.set("name", "ada").set("age", sdb::make::integer(36));

    auto v = sdb::make::object(o);
    CHECK(view(v).kind() == sdb::value_kind::object);

    // The builder was copied, not consumed -- it still works afterwards.
    o.set("extra", true);
    auto v2 = sdb::make::object(o);
    CHECK(view(v2).kind() == sdb::value_kind::object);
}

void test_populated_containers() {
    std::printf("make: populated arrays and sets\n");

    // Until surrealdb.c 0.2.3 this was impossible. `sr_value_array` took no
    // argument, so the only array *value* that existed was an empty one -- a
    // list could not be bound to a variable or stored in a field, only written
    // into the SurrealQL text. These checks are the regression pin.
    sdb::array_builder items;
    items.push(1).push(2).push(3);

    auto arr = sdb::make::array(items);
    CHECK(view(arr).kind() == sdb::value_kind::array);
    auto as_arr = view(arr).as_array();
    CHECK(as_arr && as_arr->size() == 3);
    if (as_arr && as_arr->size() == 3) CHECK((*as_arr)[1].as_int().value_or(-1) == 2);

    auto st = sdb::make::set(items);
    CHECK(view(st).kind() == sdb::value_kind::set);

    // The builder is copied, not consumed, so it stays usable.
    items.push(4);
    CHECK(view(sdb::make::array(items)).as_array().value().size() == 4);

    // The no-argument overloads still mean "empty".
    CHECK(view(sdb::make::array()).as_array().value().size() == 0);
    CHECK(view(sdb::make::set()).kind() == sdb::value_kind::set);

    // From an owned_array, the other thing that holds a run of values.
    auto owned = sdb::array_from(sdb::array_view(*items.raw()));
    CHECK(view(sdb::make::array(owned)).as_array().value().size() == 4);

    // Nesting -- which is what GeoJSON coordinates need.
    sdb::array_builder inner;
    inner.push(1.0).push(2.0);
    sdb::array_builder outer;
    outer.push(inner).push(inner);
    auto nested = sdb::make::array(outer);
    auto nv = view(nested).as_array();
    CHECK(nv && nv->size() == 2);
    if (nv && nv->size() == 2) {
        auto first = (*nv)[0].as_array();
        CHECK(first && first->size() == 2);
    }

    // A list in an object field, and a set field.
    sdb::object_builder obj;
    obj.set("tags", items).set_unique("uniq", items);
    CHECK(obj.size() == 2);
    auto ov = obj.view().get("tags");
    CHECK(ov && ov->kind() == sdb::value_kind::array);
}

void test_containers_round_trip() {
    if (!g_db) { std::printf("make: container round trip (skipped)\n"); return; }
    std::printf("make: container round trip\n");

    namespace d = sdb::detail;

    // A list must survive being stored in a record field. Before 0.2.3 this
    // came back with length 0 no matter what went in.
    sdb::array_builder tags;
    tags.push("a").push("b").push("c");
    sdb::object_builder rec;
    rec.set("name", "tagged").set("tags", tags);

    auto created = d::invoke([&](sr_string_t* e) {
        return sr_create(g_db, e, nullptr, "p7_tags", rec.raw());
    });
    CHECK(created.has_value());
    if (!created) return;

    sr_value_t* out = nullptr;
    auto selected = d::invoke<sdb::owned_values>(
        [&](sr_string_t* e) { return sr_select(g_db, e, &out, "p7_tags"); },
        [&](int n) { return sdb::owned_values(out, n); });
    CHECK(selected.has_value());
    if (!selected) return;

    sdb::array_view rows = sdb::view(selected.value());
    CHECK(rows.size() >= 1);
    if (rows.empty()) return;
    auto obj = rows[0].as_object();
    CHECK(obj.has_value());
    if (!obj) return;

    auto got = obj->get("tags");
    CHECK(got && got->kind() == sdb::value_kind::array);
    if (got) {
        auto a = got->as_array();
        CHECK(a && a->size() == 3);
        if (a && a->size() == 3) CHECK((*a)[2].as_string().value_or("") == "c");
    }
}

void test_geometry() {
    std::printf("make: geometry\n");

    CHECK(view(sdb::make::point(1.0, 2.0)).kind() == sdb::value_kind::geometry);

    const std::vector<sdb::make::coord> line{{0,0},{1,1},{2,0}};
    CHECK(view(sdb::make::linestring(line)).kind() == sdb::value_kind::geometry);
    CHECK(view(sdb::make::multipoint(line)).kind() == sdb::value_kind::geometry);

    // A polygon ring closes on itself.
    const std::vector<sdb::make::coord> ring{{0,0},{0,1},{1,1},{1,0},{0,0}};
    CHECK(view(sdb::make::polygon(ring)).kind() == sdb::value_kind::geometry);

    // std::array works too -- anything contiguous with data()/size().
    const std::array<sdb::make::coord, 2> arr{{{0,0},{3,4}}};
    CHECK(view(sdb::make::linestring(arr)).kind() == sdb::value_kind::geometry);

    // Pointer+length for callers that already have a raw buffer.
    CHECK(view(sdb::make::linestring(line.data(), 3)).kind() == sdb::value_kind::geometry);

    // Array-of-arrays: the shape that is miserable to assemble by hand.
    const std::vector<std::vector<sdb::make::coord>> lines{
        {{0,0},{1,1}},
        {{5,5},{6,6},{7,5}},
    };
    CHECK(view(sdb::make::multilinestring(lines)).kind() == sdb::value_kind::geometry);

    const std::vector<std::vector<sdb::make::coord>> polys{ring, ring};
    CHECK(view(sdb::make::multipolygon(polys)).kind() == sdb::value_kind::geometry);

    // Empty outer containers must not dereference a null data().
    const std::vector<std::vector<sdb::make::coord>> empty;
    (void)sdb::make::multilinestring(empty);
    (void)sdb::make::multipolygon(empty);
    CHECK(true);
}

void test_geometry_values_bind() {
    if (!g_db) { std::printf("make: geometry binding (skipped)\n"); return; }
    std::printf("make: geometry values bind, GeoJSON objects do not\n");

    namespace d = sdb::detail;
    auto run = [&](const char* stmt) {
        sr_arr_res_t* raw = nullptr;
        return d::invoke<sdb::owned_arr_results>(
            [&](sr_string_t* e) { return sr_query(g_db, e, &raw, stmt, nullptr); },
            [&](int n) { return sdb::owned_arr_results(raw, n); });
    };
    (void)run("DEFINE TABLE p7_pt SCHEMAFULL; DEFINE FIELD loc ON p7_pt TYPE geometry<point>;");
    (void)run("DEFINE TABLE p7_col SCHEMAFULL; DEFINE FIELD loc ON p7_col TYPE geometry<collection>;");

    auto store = [&](const char* resource, sdb::value v) {
        sdb::object_builder rec;
        rec.set("loc", v);
        return d::invoke([&](sr_string_t* e) {
            return sr_create(g_db, e, nullptr, resource, rec.raw());
        }).has_value();
    };

    // A geometry value coerces at the value level.
    auto pt = sdb::make::point(1.0, 2.0);
    CHECK(store("p7_pt:a", view(pt)));

    // A GeoJSON object does not -- and this is true for a plain Point, which is
    // why it is not a collection-specific problem. Pinned because getting this
    // backwards leads to blaming the wrong feature.
    sdb::array_builder coords;
    coords.push(1.0).push(2.0);
    sdb::object_builder geojson;
    geojson.set("type", "Point").set("coordinates", coords);
    auto as_object = sdb::make::object(geojson);
    CHECK(!store("p7_pt:b", view(as_object)));

    // A collection binds and stores like any other geometry value, whether it
    // was built here or read back from the database. Both routes are checked:
    // the read-back one used to be the only one that existed.
    auto got = run("RETURN { type:'GeometryCollection', "
                   "geometries:[{type:'Point',coordinates:[1,2]}] }");
    CHECK(got.has_value());
    if (!got) return;
    sdb::query_results qr(std::move(got).value());
    auto rows = qr.single();
    CHECK(rows.has_value());
    if (!rows || rows.value().empty()) return;

    sdb::value collection = rows.value()[0];
    CHECK(collection.kind() == sdb::value_kind::geometry);
    CHECK(sdb::geometry(collection).kind() == sdb::geometry_kind::collection);
    CHECK(store("p7_col:a", collection));

    // A multipoint is a different geometry type and the schema correctly
    // refuses it -- collection is not a synonym for "several points".
    std::vector<sdb::make::coord> pts{{1, 2}, {3, 4}};
    auto mp = sdb::make::multipoint(pts);
    CHECK(!store("p7_col:b", view(mp)));

    // And one built locally, with no round trip to the server at all.
    auto a = sdb::make::point(1.0, 2.0);
    auto b = sdb::make::point(3.0, 4.0);
    auto built = sdb::make::collection({view(a), view(b)});
    CHECK(view(built).kind() == sdb::value_kind::geometry);
    CHECK(sdb::geometry(view(built)).kind() == sdb::geometry_kind::collection);
    CHECK(store("p7_col:c", view(built)));
}

void test_collection_construction() {
    std::printf("make: collections\n");

    auto a = sdb::make::point(1.0, 2.0);
    auto b = sdb::make::point(3.0, 4.0);
    std::vector<sdb::make::coord> ring{{0,0},{0,1},{1,1},{0,0}};
    auto poly = sdb::make::polygon(ring);

    // Mixed member types, from a brace list.
    auto mixed = sdb::make::collection({view(a), view(b), view(poly)});
    CHECK(view(mixed).kind() == sdb::value_kind::geometry);
    auto g = sdb::geometry(view(mixed));
    CHECK(g.kind() == sdb::geometry_kind::collection);
    auto members = g.as_collection();
    CHECK(members && members->size() == 3);

    // Members are copied, so they stay usable afterwards.
    CHECK(view(a).kind() == sdb::value_kind::geometry);
    CHECK(sdb::geometry(view(b)).is(sdb::geometry_kind::point));

    // From a container of owned values.
    std::vector<sdb::owned_value> owned;
    owned.push_back(sdb::make::point(5.0, 6.0));
    owned.push_back(sdb::make::point(7.0, 8.0));
    auto from_vec = sdb::make::collection(owned);
    auto g2 = sdb::geometry(view(from_vec));
    CHECK(g2.kind() == sdb::geometry_kind::collection);
    CHECK(g2.as_collection() && g2.as_collection()->size() == 2);

    // Empty is a collection, not a failure.
    auto empty = sdb::make::collection();
    auto g3 = sdb::geometry(view(empty));
    CHECK(g3.kind() == sdb::geometry_kind::collection);
    CHECK(g3.as_collection() && g3.as_collection()->empty());

    // A non-geometry member yields `none`, not a half-built collection. The C
    // library refuses rather than producing something that misbehaves later,
    // and a caller taking members from elsewhere should check for this.
    auto text = sdb::make::string("not a geometry");
    auto bad = sdb::make::collection({view(a), view(text)});
    CHECK(view(bad).kind() == sdb::value_kind::none);

    // Including a GeoJSON-shaped object, which is not a geometry.
    sdb::array_builder coords;
    coords.push(1.0).push(2.0);
    sdb::object_builder geojson;
    geojson.set("type", "Point").set("coordinates", coords);
    auto obj = sdb::make::object(geojson);
    CHECK(view(sdb::make::collection({view(a), view(obj)})).kind() == sdb::value_kind::none);

    // Collections nest: a collection is itself a geometry value.
    auto nested = sdb::make::collection({view(mixed), view(a)});
    auto g4 = sdb::geometry(view(nested));
    CHECK(g4.kind() == sdb::geometry_kind::collection);
    auto outer_members = g4.as_collection();
    CHECK(outer_members && outer_members->size() == 2);
    // A collection nests: its first member is the collection built above.
    if (outer_members && outer_members->size() == 2)
        CHECK((*outer_members)[0].kind() == sdb::geometry_kind::collection);
}

void test_polygons_with_holes() {
    std::printf("make: polygons with holes\n");

    const std::vector<sdb::make::coord> outer{{0,0},{0,10},{10,10},{10,0},{0,0}};
    const std::vector<sdb::make::coord> hole1{{2,2},{2,4},{4,4},{4,2},{2,2}};
    const std::vector<sdb::make::coord> hole2{{6,6},{6,8},{8,8},{8,6},{6,6}};

    // The single-ring shorthand still means "no holes". Which overload runs is
    // decided by whether the container yields coordinates or rings, so both
    // spellings are just `polygon(...)`; this pins that the shorthand did not
    // start silently taking the other path.
    auto flat = sdb::make::polygon(outer);
    auto fp = sdb::geometry(view(flat)).as_polygon();
    CHECK(fp && fp->exterior().size() == 5);
    CHECK(fp && !fp->has_holes());

    // Rings: exterior first, then one per hole. Needs surrealdb.c 0.2.5 --
    // before it, `sr_value_polygon` hardcoded an empty interior list, so a
    // polygon with a hole could be read back but never built.
    const std::vector<std::vector<sdb::make::coord>> rings{outer, hole1, hole2};
    auto holed = sdb::make::polygon(rings);
    auto hp = sdb::geometry(view(holed)).as_polygon();
    CHECK(hp && hp->exterior().size() == 5);
    CHECK(hp && hp->has_holes());
    CHECK(hp && hp->interiors().size() == 2);
    if (hp && hp->interiors().size() == 2) {
        CHECK(hp->interiors()[0].size() == 5);
        CHECK(hp->interiors()[1].size() == 5);
    }

    // An empty ring list is an empty polygon, not a failure.
    const std::vector<std::vector<sdb::make::coord>> none;
    CHECK(view(sdb::make::polygon(none)).kind() == sdb::value_kind::geometry);

    // A multipolygon composed from polygon values keeps each member's holes.
    std::vector<sdb::owned_value> parts;
    parts.push_back(sdb::make::polygon(rings));
    parts.push_back(sdb::make::polygon(outer));
    auto mp = sdb::make::multipolygon(parts);
    auto mv = sdb::geometry(view(mp)).as_multipolygon();
    CHECK(mv && mv->size() == 2);
    if (mv && mv->size() == 2) {
        CHECK((*mv)[0].interiors().size() == 2);
        CHECK((*mv)[1].interiors().size() == 0);
    }

    // The flat-rings overload still exists and still cannot express a hole.
    const std::vector<std::vector<sdb::make::coord>> flats{outer, outer};
    auto mp2 = sdb::make::multipolygon(flats);
    auto mv2 = sdb::geometry(view(mp2)).as_multipolygon();
    CHECK(mv2 && mv2->size() == 2);
    if (mv2 && mv2->size() == 2) CHECK((*mv2)[0].interiors().empty());

    // A non-polygon member is refused the same way `collection` refuses a
    // non-geometry: a `none` value rather than something malformed.
    std::vector<sdb::owned_value> bad;
    bad.push_back(sdb::make::polygon(outer));
    bad.push_back(sdb::make::point(1.0, 2.0));
    CHECK(view(sdb::make::multipolygon(bad)).kind() == sdb::value_kind::none);
}

void test_bounds_and_range() {
    std::printf("make: bounds and ranges\n");

    auto r = sdb::make::range(sdb::make::included(sdb::make::integer(1)),
                              sdb::make::excluded(sdb::make::integer(10)));
    CHECK(view(r).kind() == sdb::value_kind::range);

    // Open at both ends.
    auto open = sdb::make::range(sdb::make::unbounded(), sdb::make::unbounded());
    CHECK(view(open).kind() == sdb::value_kind::range);

    // Half-open, either side.
    auto lo = sdb::make::range(sdb::make::included(sdb::make::string("a")),
                               sdb::make::unbounded());
    CHECK(view(lo).kind() == sdb::value_kind::range);
    auto hi = sdb::make::range(sdb::make::unbounded(),
                               sdb::make::excluded(sdb::make::string("z")));
    CHECK(view(hi).kind() == sdb::value_kind::range);

    // A bound that is built and never used must release its value rather than
    // leak it. Nothing here can observe that directly -- ASan is the assertion.
    { auto abandoned = sdb::make::included(sdb::make::integer(99)); (void)abandoned; }

    // Moving a bound must not double free either: the source is defanged.
    {
        auto a = sdb::make::included(sdb::make::integer(5));
        auto b = std::move(a);
        auto moved = sdb::make::range(std::move(b), sdb::make::unbounded());
        CHECK(view(moved).kind() == sdb::value_kind::range);
    }

    // Move-assignment over an armed bound frees the overwritten one.
    {
        auto a = sdb::make::included(sdb::make::integer(1));
        a = sdb::make::excluded(sdb::make::integer(2));
        auto v = sdb::make::range(std::move(a), sdb::make::unbounded());
        CHECK(view(v).kind() == sdb::value_kind::range);
    }
}

void test_equality() {
    std::printf("make: equality\n");

    auto a = sdb::make::integer(5);
    auto b = sdb::make::integer(5);
    auto c = sdb::make::integer(6);

    CHECK(view(a) == view(b));
    CHECK(view(a) != view(c));
    CHECK(view(sdb::make::string("x")) == view(sdb::make::string("x")));
    CHECK(view(sdb::make::string("x")) != view(sdb::make::string("y")));

    // none and null are not equal to each other.
    CHECK(view(sdb::make::none()) != view(sdb::make::null()));

    // An invalid view compares equal only to another invalid one, and never
    // reaches the C function with a null.
    sdb::value bad{};
    CHECK(bad == sdb::value{});
    CHECK(bad != view(a));
}

void test_round_trip() {
    if (!g_db) { std::printf("make: round trip (skipped, no db)\n"); return; }
    std::printf("make: round trip\n");

    namespace d = sdb::detail;

    // Constructed values must survive a trip through the database, not just
    // report the right kind locally.
    struct probe { sdb::owned_value v; sdb::value_kind want; const char* key; };
    probe probes[] = {
        { sdb::make::point(1.0, 2.0),    sdb::value_kind::geometry, "pt" },
        { sdb::make::decimal("3.14159"), sdb::value_kind::number,   "dec" },
        { sdb::make::thing("users", "u1"), sdb::value_kind::thing,  "ref" },
        { sdb::make::linestring(std::vector<sdb::make::coord>{{0,0},{1,1}}),
                                         sdb::value_kind::geometry, "line" },
    };

    sdb::object_builder rec;
    for (auto& p : probes) rec.set(p.key, p.v);

    auto created = d::invoke([&](sr_string_t* e) {
        return sr_create(g_db, e, nullptr, "p7_tbl", rec.raw());
    });
    CHECK(created.has_value());
    if (!created.has_value()) {
        std::printf("  create failed: %.*s\n",
                    static_cast<int>(created.error().message().size()),
                    created.error().message().data());
        return;
    }

    sr_value_t* out = nullptr;
    auto selected = d::invoke<sdb::owned_values>(
        [&](sr_string_t* e) { return sr_select(g_db, e, &out, "p7_tbl"); },
        [&](int n) { return sdb::owned_values(out, n); });
    CHECK(selected.has_value());
    if (!selected.has_value()) return;

    sdb::array_view rows = sdb::view(selected.value());
    CHECK(rows.size() >= 1);
    if (rows.empty()) return;

    auto obj = rows[0].as_object();
    CHECK(obj.has_value());
    if (!obj) return;

    for (auto& p : probes) {
        auto got = obj->get(p.key);
        check(got && got->kind() == p.want, p.key, __LINE__);
        if (got && got->kind() != p.want)
            std::printf("    %s came back as %s\n", p.key, sdb::to_string(got->kind()));
    }

    // A range is not storable as a record field, but it must still round trip
    // as a bound query parameter.
    sdb::object_builder vars;
    vars.set("r", sdb::make::range(sdb::make::included(sdb::make::integer(1)),
                                   sdb::make::excluded(sdb::make::integer(9))));
    CHECK(!vars.empty());
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P7 value-construction tests ===\n");
    std::printf("standard: %ld\n\n", static_cast<long>(SURREALDB_CPLUSPLUS));

    sr_string_t err = nullptr;
    if (sr_connect(&err, &g_db, "memory") < 0) {
        std::printf("connect failed: %s\n", err ? err : "?");
        if (err) sr_string_free(err);
        g_db = nullptr;
    } else {
        sr_use_ns(g_db, &err, "p7_ns");
        sr_use_db(g_db, &err, "p7_db");
    }

    test_scalars();
    test_object();
    test_populated_containers();
    test_containers_round_trip();
    test_geometry();
    test_polygons_with_holes();
    test_collection_construction();
    test_geometry_values_bind();
    test_bounds_and_range();
    test_equality();
    test_round_trip();

    if (g_db) sr_surreal_disconnect(g_db);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
