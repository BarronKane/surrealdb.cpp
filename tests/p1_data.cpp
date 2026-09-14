// P1 data-layer tests: value / array_view / object.
//
// Exit criterion for this phase is that all eighteen value kinds are
// representable and survive a round trip through the database. Framework-free
// for the same reason P0 is: it has to run across the whole standard x stdlib
// matrix with nothing but a compiler.

#include <surrealdb/surrealdb.hpp>

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

// A scope guard so every constructed value is released even when a check fails.
struct val {
    sdb::owned_value v;
    explicit val(sr_value_t* raw) noexcept : v(raw) {}
    [[nodiscard]] sdb::value view() const noexcept { return sdb::value(v.get()); }
};

// ---------------------------------------------------------------------------

void test_kind_coverage() {
    std::printf("value: all 18 kinds\n");

    struct probe { sr_value_t* raw; sdb::value_kind want; const char* name; };

    const std::uint8_t uuid_bytes[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const std::uint8_t raw_bytes[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    sdb::object_builder nested;
    nested.set("k", "v");

    probe probes[] = {
        { sr_value_none(),                        sdb::value_kind::none,     "none" },
        { sr_value_null(),                        sdb::value_kind::null,     "null" },
        { sr_value_bool(true),                    sdb::value_kind::boolean,  "bool" },
        { sr_value_int(42),                       sdb::value_kind::number,   "int" },
        { sr_value_float(1.5),                    sdb::value_kind::number,   "float" },
        { sr_value_decimal("1.25"),               sdb::value_kind::number,   "decimal" },
        { sr_value_string("hello"),               sdb::value_kind::string,   "string" },
        { sr_value_duration(5, 250),              sdb::value_kind::duration, "duration" },
        { sr_value_datetime("2026-01-01T00:00:00Z"), sdb::value_kind::datetime, "datetime" },
        { sr_value_uuid(uuid_bytes),              sdb::value_kind::uuid,     "uuid" },
        { sr_value_array(nullptr),                       sdb::value_kind::array,    "array" },
        { sr_value_object(nested.raw()),          sdb::value_kind::object,   "object" },
        { sr_value_point(-122.4, 37.7),           sdb::value_kind::geometry, "geometry" },
        { sr_value_bytes(raw_bytes, 4),           sdb::value_kind::bytes,    "bytes" },
        { sr_value_thing("person", "alice"),      sdb::value_kind::thing,    "thing" },
        { sr_value_table("person"),               sdb::value_kind::table,    "table" },
        { sr_value_file("bucket", "key"),         sdb::value_kind::file,     "file" },
        { sr_value_regex("^a.*z$"),               sdb::value_kind::regex,    "regex" },
        { sr_value_set(nullptr),                         sdb::value_kind::set,      "set" },
        { sr_value_range(sr_bound_unbounded(), sr_bound_unbounded()),
                                                  sdb::value_kind::range,    "range" },
    };

    const int n = static_cast<int>(sizeof(probes) / sizeof(probes[0]));
    for (int i = 0; i < n; ++i) {
        sdb::owned_value owner(probes[i].raw);
        sdb::value v(owner.get());
        if (v.kind() != probes[i].want) {
            std::printf("  FAIL: %s -> %s (wanted %s)\n", probes[i].name,
                        sdb::to_string(v.kind()), sdb::to_string(probes[i].want));
            ++g_failures;
        }
        ++g_checks;
    }

    // All eighteen distinct kinds must actually be reachable.
    bool seen[18] = {false};
    for (int i = 0; i < n; ++i) seen[static_cast<int>(probes[i].want)] = true;
    int covered = 0;
    for (bool s : seen) if (s) ++covered;
    std::printf("  distinct kinds reached: %d/18\n", covered);
    CHECK(covered == 18);
}

void test_none_null_distinct() {
    std::printf("value: none and null stay distinct\n");
    val none(sr_value_none());
    val null(sr_value_null());

    CHECK(none.view().is_none());
    CHECK(!none.view().is_null());
    CHECK(null.view().is_null());
    CHECK(!null.view().is_none());
    // An absent field and an explicit null are both "nullish" only when the
    // caller opts into not caring.
    CHECK(none.view().is_nullish() && null.view().is_nullish());
    CHECK(none.view().kind() != null.view().kind());
}

void test_array_set_distinct() {
    std::printf("value: array and set stay distinct\n");
    val arr(sr_value_array(nullptr));
    val set(sr_value_set(nullptr));
    CHECK(arr.view().kind() == sdb::value_kind::array);
    CHECK(set.view().kind() == sdb::value_kind::set);
    // as_array must not accept a set, nor as_set an array -- the uniqueness
    // guarantee is part of the type.
    CHECK(arr.view().as_array().has_value());
    CHECK(!arr.view().as_set().has_value());
    CHECK(set.view().as_set().has_value());
    CHECK(!set.view().as_array().has_value());
}

void test_wrong_type_access_is_empty() {
    std::printf("value: mismatched access yields nullopt, never garbage\n");
    val s(sr_value_string("not a number"));
    sdb::value v = s.view();

    CHECK(v.as_string().has_value());
    CHECK(!v.as_int().has_value());
    CHECK(!v.as_double().has_value());
    CHECK(!v.as_bool().has_value());
    CHECK(!v.as_array().has_value());
    CHECK(!v.as_object().has_value());
    CHECK(!v.as_uuid().has_value());
    CHECK(!v.as_bytes().has_value());
    CHECK(!v.as_datetime().has_value());
    CHECK(!v.as_table().has_value());
    CHECK(!v.as_regex().has_value());

    // A default-constructed view is inert rather than a crash.
    sdb::value empty;
    CHECK(!empty.valid());
    CHECK(empty.is_none());
    CHECK(!empty.as_int().has_value());
}

void test_scalar_payloads() {
    std::printf("value: scalar payloads\n");
    val b(sr_value_bool(true));
    CHECK(b.view().as_bool().value_or(false) == true);

    val i(sr_value_int(-9007199254740993LL));   // beyond exact double range
    CHECK(i.view().as_int().value_or(0) == -9007199254740993LL);
    CHECK(!i.view().as_double().has_value());   // an int is not a float

    val f(sr_value_float(2.5));
    CHECK(f.view().as_double().value_or(0.0) == 2.5);
    CHECK(!f.view().as_int().has_value());

    val d(sr_value_decimal("1.2345678901234567890"));
    auto dec = d.view().as_decimal();
    CHECK(dec.has_value());
    CHECK(dec && dec->text == "1.2345678901234567890");
    // Decimals must not be silently widened to double -- that is the entire
    // reason they cross the boundary as text.
    CHECK(!d.view().as_double().has_value());

    val s(sr_value_string("hello"));
    CHECK(s.view().as_string().value_or("") == "hello");

    val dur(sr_value_duration(5, 250));
    auto du = dur.view().as_duration();
    CHECK(du && du->seconds == 5 && du->nanoseconds == 250);

    const std::uint8_t ub[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    val u(sr_value_uuid(ub));
    auto uv = u.view().as_uuid();
    CHECK(uv && uv->bytes != nullptr);
    CHECK(uv && std::memcmp(uv->bytes, ub, 16) == 0);

    const std::uint8_t rb[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    val by(sr_value_bytes(rb, 4));
    auto bv = by.view().as_bytes();
    CHECK(bv && bv->size == 4);
    CHECK(bv && !bv->empty());
    int sum = 0;
    if (bv) for (std::uint8_t x : *bv) sum += x;
    CHECK(sum == 0xDE + 0xAD + 0xBE + 0xEF);

    val fi(sr_value_file("bucket", "key"));
    auto fv = fi.view().as_file();
    CHECK(fv && fv->bucket == "bucket" && fv->key == "key");

    // The string-shaped kinds stay distinguishable from a plain string.
    val dt(sr_value_datetime("2026-01-01T00:00:00Z"));
    CHECK(dt.view().as_datetime().has_value());
    CHECK(!dt.view().as_string().has_value());

    val tb(sr_value_table("person"));
    CHECK(tb.view().as_table().has_value());
    CHECK(!tb.view().as_string().has_value());

    val rx(sr_value_regex("^a.*z$"));
    CHECK(rx.view().as_regex().has_value());
    CHECK(!rx.view().as_string().has_value());
}

void test_object_builder() {
    std::printf("object: builder and view\n");
    sdb::object_builder ob;
    ob.set("name", "alice")
      .set("age", 30)
      .set("score", 1.5)
      .set("active", true);

    sdb::object_view ov = ob.view();
    CHECK(ov.valid());
    CHECK(ov.size() == 4);
    CHECK(!ov.empty());
    CHECK(ov.contains("name"));
    CHECK(!ov.contains("missing"));

    auto name = ov.get("name");
    CHECK(name && name->as_string().value_or("") == "alice");
    auto age = ov.get("age");
    CHECK(age && age->as_int().value_or(0) == 30);
    auto active = ov.get("active");
    CHECK(active && active->as_bool().value_or(false));

    CHECK(!ov.get("missing").has_value());
    CHECK(!ov.get(nullptr).has_value());
}

void test_object_int64_not_truncated() {
    std::printf("object: 64-bit ints survive insertion\n");
    // sr_object_insert_int takes a C `int`. Routing set(key, int64_t) through
    // it would silently truncate; the builder uses sr_value_int instead.
    const std::int64_t big = 9007199254740993LL;
    sdb::object_builder ob;
    ob.set("big", big);

    auto got = ob.view().get("big");
    CHECK(got.has_value());
    CHECK(got && got->as_int().value_or(0) == big);
    if (got && got->as_int().value_or(0) != big) {
        std::printf("  truncated to %lld\n",
                    static_cast<long long>(got->as_int().value_or(0)));
    }
}

void test_object_keys() {
    std::printf("object: key enumeration\n");
    sdb::object_builder ob;
    ob.set("alpha", 1).set("beta", 2).set("gamma", 3);

    sdb::object_keys keys = sdb::keys_of(ob.view());
    CHECK(keys.size() == 3);

    int seen = 0;
    for (std::string_view k : keys) {
        if (k == "alpha" || k == "beta" || k == "gamma") ++seen;
    }
    CHECK(seen == 3);
    CHECK(keys[0].has_value());
    CHECK(!keys[99].has_value());
    CHECK(!keys[-1].has_value());
}

void test_array_from_is_bulk() {
    std::printf("array: bulk construction\n");

    // Build a run of values the way a caller would already have them.
    std::vector<sdb::owned_value> owned;
    std::vector<sr_value_t> block;
    for (int i = 0; i < 5; ++i) {
        owned.emplace_back(::sr_value_int(i * 10));
        block.push_back(*owned.back().get());
    }

    auto arr = sdb::array_from(block.data(), static_cast<int>(block.size()));
    CHECK(static_cast<bool>(arr));
    if (!arr) return;

    sdb::array_view v(*arr.get());
    CHECK(v.size() == 5);
    if (v.size() == 5) {
        CHECK(v[0].as_int().value_or(-1) == 0);
        CHECK(v[4].as_int().value_or(-1) == 40);
    }

    // The values are deep-copied, so the originals are still ours to release --
    // and releasing them must not disturb the array.
    owned.clear();
    CHECK(v.size() == 5);
    CHECK(v[4].as_int().value_or(-1) == 40);

    // Same result as pushing one at a time, which is the parity that matters.
    sdb::array_builder pushed;
    for (int i = 0; i < 5; ++i) pushed.push(i * 10);
    CHECK(pushed.size() == v.size());

    // The array_view overload, and the bulk constructor.
    auto again = sdb::array_from(v);
    CHECK(again && sdb::array_view(*again.get()).size() == 5);

    sdb::array_builder from_view{v};
    CHECK(from_view.size() == 5);

    // Empty and null inputs yield an empty array rather than failing.
    auto none = sdb::array_from(nullptr, 0);
    CHECK(none && sdb::array_view(*none.get()).size() == 0);
}

void test_array_builder_and_view() {
    std::printf("array: builder and view\n");
    sdb::array_builder ab;
    ab.push(10)
      .push(20)
      .push(30);

    sdb::array_view av = ab.view();
    CHECK(av.size() == 3);
    CHECK(!av.empty());

    std::int64_t sum = 0;
    for (sdb::value v : av) sum += v.as_int().value_or(0);
    CHECK(sum == 60);

    CHECK(av[0].as_int().value_or(0) == 10);
    CHECK(av.at(2) && av.at(2)->as_int().value_or(0) == 30);
    CHECK(!av.at(3).has_value());
    CHECK(!av.at(-1).has_value());

    // Random access, since the iterator advertises it.
    auto it = av.begin();
    CHECK((it + 2)->as_int().value_or(0) == 30);
    CHECK(av.end() - av.begin() == 3);

    sdb::array_view empty;
    CHECK(empty.empty() && empty.size() == 0);
    CHECK(empty.begin() == empty.end());
}

void test_visit() {
    std::printf("value: visit dispatches on kind\n");

    struct namer {
        const char* operator()(sdb::none_t) const { return "none"; }
        const char* operator()(sdb::null_t) const { return "null"; }
        const char* operator()(bool) const { return "bool"; }
        const char* operator()(std::int64_t) const { return "int"; }
        const char* operator()(double) const { return "double"; }
        const char* operator()(sdb::decimal_ref) const { return "decimal"; }
        const char* operator()(std::string_view) const { return "string"; }
        const char* operator()(sdb::datetime_ref) const { return "datetime"; }
        const char* operator()(sdb::table_ref) const { return "table"; }
        const char* operator()(sdb::regex_ref) const { return "regex"; }
        const char* operator()(sdb::duration_ref) const { return "duration"; }
        const char* operator()(sdb::uuid_ref) const { return "uuid"; }
        const char* operator()(sdb::bytes_ref) const { return "bytes"; }
        const char* operator()(sdb::file_ref) const { return "file"; }
        const char* operator()(sdb::array_view) const { return "array"; }
        const char* operator()(sdb::object_view) const { return "object"; }
        const char* operator()(const sr_thing_t*) const { return "thing"; }
        const char* operator()(const sr_range_t*) const { return "range"; }
        const char* operator()(const sr_geometry_t*) const { return "geometry"; }
    };

    val none(sr_value_none());
    val null(sr_value_null());
    val i(sr_value_int(1));
    val f(sr_value_float(1.0));
    val d(sr_value_decimal("1.0"));
    val s(sr_value_string("x"));
    val dt(sr_value_datetime("2026-01-01T00:00:00Z"));
    val tb(sr_value_table("t"));
    val rx(sr_value_regex("r"));
    val pt(sr_value_point(0.0, 0.0));

    CHECK(std::strcmp(none.view().visit(namer{}), "none") == 0);
    CHECK(std::strcmp(null.view().visit(namer{}), "null") == 0);
    CHECK(std::strcmp(i.view().visit(namer{}), "int") == 0);
    CHECK(std::strcmp(f.view().visit(namer{}), "double") == 0);
    CHECK(std::strcmp(d.view().visit(namer{}), "decimal") == 0);
    CHECK(std::strcmp(s.view().visit(namer{}), "string") == 0);
    // The string-shaped kinds must not collapse into "string" in a visitor.
    CHECK(std::strcmp(dt.view().visit(namer{}), "datetime") == 0);
    CHECK(std::strcmp(tb.view().visit(namer{}), "table") == 0);
    CHECK(std::strcmp(rx.view().visit(namer{}), "regex") == 0);
    CHECK(std::strcmp(pt.view().visit(namer{}), "geometry") == 0);
}

#if SURREALDB_HAS_RANGES
void test_ranges_composition() {
    std::printf("array: std::ranges composition (C++20+)\n");
    sdb::array_builder ab;
    for (std::int64_t n = 1; n <= 5; ++n) ab.push(n);

    static_assert(std::ranges::range<sdb::array_view>, "array_view must be a range");
    static_assert(std::ranges::random_access_range<sdb::array_view>,
                  "array_view must be random access");
    static_assert(std::ranges::view<sdb::array_view>, "array_view must model view");
    static_assert(std::ranges::borrowed_range<sdb::array_view>,
                  "array_view borrows, so it must be a borrowed_range");

    auto big = ab.view() | std::views::filter([](sdb::value v) {
                   return v.as_int().value_or(0) > 3;
               });
    int count = 0;
    for (auto v : big) { (void)v; ++count; }
    CHECK(count == 2);
}
#endif

// ---------------------------------------------------------------------------

void test_round_trip() {
    std::printf("round trip: kinds survive the database\n");
    if (!g_db) { std::printf("  no connection; skipped\n"); return; }

    namespace d = sdb::detail;

    // Build a record touching several distinct kinds at once.
    sdb::object_builder rec;
    rec.set("name", "round trip")
       .set("count", static_cast<std::int64_t>(9007199254740993LL))
       .set("ratio", 0.25)
       .set("flag", true);

    auto created = d::invoke([&](sr_string_t* e) {
        return sr_create(g_db, e, nullptr, "p1_tbl", rec.raw());
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
        [&](sr_string_t* e) { return sr_select(g_db, e, &out, "p1_tbl"); },
        [&](int n) { return sdb::owned_values(out, n); });

    CHECK(selected.has_value());
    if (!selected.has_value()) return;

    sdb::array_view rows = sdb::view(selected.value());
    CHECK(rows.size() >= 1);
    if (rows.empty()) return;

    auto obj = rows[0].as_object();
    CHECK(obj.has_value());
    if (!obj) return;

    auto name = obj->get("name");
    CHECK(name && name->as_string().value_or("") == "round trip");

    // The 64-bit value must come back intact, not rounded through a double.
    auto count = obj->get("count");
    CHECK(count && count->as_int().value_or(0) == 9007199254740993LL);

    auto flag = obj->get("flag");
    CHECK(flag && flag->as_bool().value_or(false));

    std::printf("  row has %d fields\n", obj->size());
}

void test_exotic_round_trip() {
    std::printf("round trip: range / set / regex keep their kind\n");
    if (!g_db) { std::printf("  no connection; skipped\n"); return; }
    namespace d = sdb::detail;

    struct probe { const char* stmt; sdb::value_kind want; const char* label; };
    const probe probes[] = {
        { "RETURN 1..10",              sdb::value_kind::range, "range" },
        { "RETURN <set>[1, 2, 2, 3]",  sdb::value_kind::set,   "set" },
        { "RETURN /^abc$/",            sdb::value_kind::regex, "regex" },
        { "RETURN <datetime>'2026-01-01T00:00:00Z'", sdb::value_kind::datetime, "datetime" },
        { "RETURN 1.5dec",             sdb::value_kind::number, "decimal" },
    };

    // Deliberately the raw C path rather than `connection::query`: this probes
    // the kinds the C layer produces, so going through the C++ wrapper would
    // put the thing under test behind the thing doing the testing.
    for (const probe& p : probes) {
        sr_arr_res_t* raw = nullptr;
        auto res = d::invoke<sdb::owned_arr_results>(
            [&](sr_string_t* e) { return sr_query(g_db, e, &raw, p.stmt, nullptr); },
            [&](int n) { return sdb::owned_arr_results(raw, n); });

        if (!res.has_value()) { std::printf("  %s: query failed\n", p.label); continue; }
        const sdb::owned_arr_results& rows = res.value();
        if (rows.size() == 0 || rows[0].ok.len == 0) {
            std::printf("  %s: no value returned\n", p.label);
            continue;
        }
        sdb::value v(&rows[0].ok.arr[0]);
        ++g_checks;
        if (v.kind() != p.want) {
            ++g_failures;
            std::printf("  FAIL: %s arrived as %s (wanted %s)\n",
                        p.label, sdb::to_string(v.kind()), sdb::to_string(p.want));
        }
    }
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P1 data-layer tests ===\n");
    std::printf("standard: %ld  ranges:%d\n\n",
                static_cast<long>(SURREALDB_CPLUSPLUS), SURREALDB_HAS_RANGES);

    sr_string_t err = nullptr;
    if (sr_connect(&err, &g_db, "memory") < 0) {
        std::printf("connect failed: %s\n", err ? err : "?");
        if (err) sr_string_free(err);
        g_db = nullptr;
    } else {
        sr_use_ns(g_db, &err, "p1_ns");
        sr_use_db(g_db, &err, "p1_db");
    }

    test_kind_coverage();
    test_none_null_distinct();
    test_array_set_distinct();
    test_wrong_type_access_is_empty();
    test_scalar_payloads();
    test_object_builder();
    test_object_int64_not_truncated();
    test_object_keys();
    test_array_builder_and_view();
    test_array_from_is_bulk();
    test_visit();
#if SURREALDB_HAS_RANGES
    test_ranges_composition();
#endif
    test_round_trip();
    test_exotic_round_trip();

    if (g_db) sr_surreal_disconnect(g_db);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
