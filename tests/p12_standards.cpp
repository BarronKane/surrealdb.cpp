// P12: the standard this was compiled as, and what the library does about it.
//
// C++17 is a floor with no ceiling, so the library detects capabilities and
// adds to itself when they are present. Two things can go wrong with that and
// neither shows up in a normal test run:
//
//   1. A gate reports a capability the library does not actually use or
//      provide. `SURREALDB_HAS_SPAN` was defined and advertised for months
//      while nothing in the library ever touched `std::span`.
//   2. A gate is substitutive rather than additive -- a type quietly *becomes*
//      a different type at a higher standard. That links cleanly and corrupts
//      at runtime, which is why the rule exists.
//
// So this suite asserts the relationship between the gates and the surface,
// and CMake builds it once per supported standard. A claim made here is
// checked at 17, 20, 23 and 26 rather than at whatever the host happened to
// pick.

#include <surrealdb/surrealdb.hpp>

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#if SURREALDB_HAS_RANGES
#  include <ranges>
#  include <algorithm>
#endif
#if SURREALDB_HAS_SPAN
#  include <span>
#endif
#if SURREALDB_HAS_FORMAT && SURREALDB_HAS_EXCEPTIONS
#  include <format>
#endif

namespace sdb = surrealdb;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  FAIL (line %d): %s\n", line, what); }
}
#define CHECK(e) check((e), #e, __LINE__)

// Detection idiom, so "does this member exist" is a value this suite can test
// rather than something only a compiler knows.
template <class, class = void> struct has_as_span : std::false_type {};
template <class T> struct has_as_span<T, std::void_t<decltype(std::declval<const T&>().as_span())>>
    : std::true_type {};

template <class, class = void> struct has_to_std : std::false_type {};
template <class T> struct has_to_std<T, std::void_t<decltype(std::declval<T&&>().to_std())>>
    : std::true_type {};

// ---------------------------------------------------------------------------

void test_standard_is_reported_honestly() {
    std::printf("standards: the gates agree with the compiler\n");

    // The floor holds. Below this the header refuses to compile at all, so
    // reaching here already proves part of it.
    CHECK(SURREALDB_HAS_CXX17 == 1);
    CHECK(sdb::cpp_standard >= 201703L);
    CHECK(sdb::has_cxx17);

    // The macros and their constexpr mirrors must not disagree. They are
    // written twice -- once for `#if`, once for module consumers who cannot see
    // macros at all -- and two spellings of one fact drift.
    CHECK(sdb::has_cxx17 == (SURREALDB_HAS_CXX17 != 0));
    CHECK(sdb::has_cxx20 == (SURREALDB_HAS_CXX20 != 0));
    CHECK(sdb::has_cxx23 == (SURREALDB_HAS_CXX23 != 0));
    CHECK(sdb::has_concepts == (SURREALDB_HAS_CONCEPTS != 0));
    CHECK(sdb::has_ranges == (SURREALDB_HAS_RANGES != 0));
    CHECK(sdb::has_span == (SURREALDB_HAS_SPAN != 0));
    CHECK(sdb::has_std_expected == (SURREALDB_HAS_STD_EXPECTED != 0));
    CHECK(sdb::has_format == (SURREALDB_HAS_FORMAT != 0));
    CHECK(sdb::has_exceptions == (SURREALDB_HAS_EXCEPTIONS != 0));

    // The version is one fact in three places: CMake, the macros, the
    // constants. CMake checks itself against the header at configure time;
    // this checks the header against itself.
    CHECK(sdb::version_major == SURREALDB_CPP_VERSION_MAJOR);
    CHECK(sdb::version_minor == SURREALDB_CPP_VERSION_MINOR);
    CHECK(sdb::version_patch == SURREALDB_CPP_VERSION_PATCH);
    CHECK(sdb::version == SURREALDB_CPP_VERSION);
    CHECK(std::string(sdb::version_string) ==
          std::to_string(sdb::version_major) + "." +
          std::to_string(sdb::version_minor) + "." +
          std::to_string(sdb::version_patch));
}

void test_gates_are_one_way_doors() {
    std::printf("standards: gates only ever open\n");

    // Every gate is `>=` against a feature-test macro, so a newer standard
    // inherits everything an older one had. A gate that closed as the standard
    // rose would mean an `#elif` ladder had been introduced somewhere, which is
    // the decay this shape exists to prevent.
    if (sdb::has_cxx23) CHECK(sdb::has_cxx20);
    if (sdb::has_cxx20) CHECK(sdb::has_cxx17);

    // Library features imply the language standard that introduced them. The
    // converse is deliberately *not* asserted: a standard library can lag its
    // compiler, which is exactly why these are detected rather than inferred
    // from `__cplusplus`. libc++ still ships no `std::generator` at C++23.
    if (sdb::has_ranges)       CHECK(sdb::has_cxx20);
    if (sdb::has_span)         CHECK(sdb::has_cxx20);
    if (sdb::has_concepts)     CHECK(sdb::has_cxx20);
    if (sdb::has_format)       CHECK(sdb::has_cxx20);
    if (sdb::has_std_expected) CHECK(sdb::has_cxx23);

    // `SURREALDB_HAS_RANGES` additionally requires concepts, because the
    // library's range conformance is written in terms of them.
    if (sdb::has_ranges) CHECK(sdb::has_concepts);
}

void test_cxx17_baseline() {
    std::printf("standards: the C++17 surface is complete on its own\n");

    // Nothing below is gated. If any of it ever needs a gate, the floor moved.
    auto made = sdb::make::integer(7);
    CHECK(sdb::value(made.get()).as_int().value_or(0) == 7);

    sdb::array_builder b;
    b.push(1).push("two").push(3.0);
    CHECK(b.size() == 3);

    auto arr = sdb::make::array(b);
    CHECK(sdb::value(arr.get()).as_array().value().size() == 3);

    auto pt = sdb::make::point(1.0, 2.0);
    CHECK(sdb::geometry(sdb::value(pt.get())).kind() == sdb::geometry_kind::point);

    auto q = sdb::dsl::select("t").fields("a").where(sdb::dsl::f("n") > 1).build();
    CHECK(q.has_value());

    // result<T> is the library's own type at every standard -- never aliased to
    // std::expected, which would make it a different type either side of C++23.
    sdb::result<int> r(42);
    CHECK(r.has_value() && r.value() == 42);
    // Parenthesised: the comma inside the template argument list would
    // otherwise split the macro's arguments.
    CHECK((!std::is_same<sdb::result<int>, void>::value));
}

void test_cxx20_additions() {
    std::printf("standards: C++20 additions %s\n",
                sdb::has_ranges ? "present" : "absent (correctly)");

    // The claim under test is the biconditional: the capability exists exactly
    // when the gate says so. A gate that is on while the surface is missing is
    // the `SURREALDB_HAS_SPAN` bug; one that is off while the surface exists
    // would mean the gate is not what actually controls it.
    CHECK(has_as_span<sdb::coord_span>::value == sdb::has_span);

#if SURREALDB_HAS_RANGES
    // The view types are real C++20 ranges, not merely iterable.
    static_assert(std::ranges::view<sdb::array_view>);
    static_assert(std::ranges::borrowed_range<sdb::array_view>);
    static_assert(std::ranges::view<sdb::coord_span>);
    static_assert(std::ranges::borrowed_range<sdb::coord_span>);
    static_assert(std::ranges::view<sdb::geometry_span>);
    static_assert(std::ranges::range<sdb::query_results>);
    static_assert(std::ranges::range<sdb::session_list>);

    // Owning views are deliberately NOT borrowed ranges: an iterator outliving
    // a query_results really is dangling, and the check should say so.
    static_assert(!std::ranges::borrowed_range<sdb::query_results>);

    // And they work with the algorithms, which is the point of conforming.
    const std::vector<sdb::make::coord> ring{{0,0},{1,1},{2,2},{3,3}};
    auto line = sdb::make::linestring(ring);
    auto ls = sdb::geometry(sdb::value(line.get())).as_linestring();
    CHECK(ls.has_value());
    if (ls) {
        auto n = std::ranges::count_if(ls->coords(), [](sdb::coord c) { return c.x > 1.0; });
        CHECK(n == 2);
    }
#endif

#if SURREALDB_HAS_SPAN
    const std::vector<sdb::make::coord> ring2{{0,0},{1,1},{2,2}};
    auto line2 = sdb::make::linestring(ring2);
    auto ls2 = sdb::geometry(sdb::value(line2.get())).as_linestring();
    CHECK(ls2.has_value());
    if (ls2) {
        std::span<const sdb::coord> sp = ls2->coords().as_span();
        CHECK(sp.size() == 3);
        CHECK(sp[2].x == 2.0);
    }
#endif
}

void test_format_support() {
    std::printf("standards: std::format %s\n",
                (sdb::has_format && sdb::has_exceptions) ? "present" : "absent (correctly)");

    // The formatters need std::format *and* exceptions -- `parse` throws on a
    // bad spec, so under -fno-exceptions they cannot be compiled at all. A
    // consumer at C++20 without exceptions correctly gets none of them, which
    // is why the gate is a conjunction rather than just has_format.
#if SURREALDB_HAS_FORMAT && SURREALDB_HAS_EXCEPTIONS
    CHECK(std::format("{}", sdb::error_code::fatal) == "fatal");
    CHECK(std::format("{}", sdb::value_kind::geometry) == "geometry");
    CHECK(std::format("{}", sdb::geometry_kind::collection) == "collection");
    CHECK(std::format("{}", sdb::id_kind::number) == "number");

    const std::uint8_t raw[16] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                                  0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10};
    sdb::session_id id(raw);
    CHECK(std::format("{}", id) == "01234567-89ab-cdef-fedc-ba9876543210");

    auto e = sdb::error::local(sdb::error_code::error, "something went wrong");
    CHECK(std::format("{}", e) == "error: something went wrong");

    // No message: the code alone, not a dangling separator.
    sdb::error bare;
    CHECK(std::format("{}", bare) == "ok");
#endif
}

void test_cxx23_additions() {
    std::printf("standards: C++23 additions %s\n",
                sdb::has_std_expected ? "present" : "absent (correctly)");

    // `to_std()` exists exactly when std::expected does.
    CHECK(has_to_std<sdb::result<int>>::value == sdb::has_std_expected);

#if SURREALDB_HAS_STD_EXPECTED
    sdb::result<int> ok(7);
    auto e = std::move(ok).to_std();
    // `*e`, not `e.value()`: the throwing accessor needs to copy the error to
    // build bad_expected_access, and `error` is move-only, so that member
    // cannot be instantiated at all. Documented on to_std(); asserted here so
    // the documentation stays true.
    CHECK(e.has_value() && *e == 7);

    sdb::result<int> bad(sdb::error::local(sdb::error_code::error, "nope"));
    auto e2 = std::move(bad).to_std();
    CHECK(!e2.has_value());
    CHECK(e2.error().message() == "nope");

    // Additive, not substitutive: result is still the library's own type.
    static_assert(!std::is_same_v<sdb::result<int>, std::expected<int, sdb::error>>);
#endif
}

void test_cxx26_additions() {
    std::printf("standards: C++26 additions %s\n",
                sdb::has_deleted_reasons ? "present" : "absent (correctly)");

    // Nothing to call -- the C++26 addition is that the deleted lifetime
    // guards carry an explanation. What is testable is that the constant
    // matches the feature-test macro, and that the guards are still deleted
    // either way, which `tests/negative_compile.cpp` proves per case.
#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
    CHECK(sdb::has_deleted_reasons);
#else
    CHECK(!sdb::has_deleted_reasons);
#endif
}

void test_behaviour_is_standard_independent() {
    std::printf("standards: behaviour does not vary with the standard\n");

    // The library adds capabilities at higher standards; it must never compute
    // a different answer. These are the same assertions at every standard, so a
    // divergence shows up as one configuration failing while the rest pass.
    auto opened = sdb::connection::connect("memory");
    CHECK(opened.has_value());
    if (!opened) return;
    auto db = std::move(opened).value();
    CHECK(db.use("p12_ns", "p12_db").has_value());

    sdb::array_builder tags;
    tags.push("a").push("b");
    sdb::object_builder rec;
    rec.set("name", "ada").set("age", 36).set("tags", tags);
    CHECK(db.create("person:ada", rec).has_value());

    auto q = sdb::dsl::select("person").fields("name", "age")
                 .where(sdb::dsl::f("age") > 30).build();
    CHECK(q.has_value());
    if (!q) return;
    CHECK(q.value().text() == "SELECT name, age FROM person WHERE age > $v0;");

    auto res = db.run(q.value());
    CHECK(res.has_value());
    if (!res) return;
    auto rows = res.value().single();
    CHECK(rows.has_value());
    if (!rows) return;
    CHECK(rows.value().size() == 1);

    auto obj = rows.value()[0].as_object();
    CHECK(obj.has_value());
    if (obj) {
        auto name = obj->get("name");
        CHECK(name && name->as_string().value_or("") == "ada");
    }

    // Errors are reported the same way at every standard.
    CHECK(!db.query("SELECT * FROM", nullptr).has_value());

    auto thrown = db.query("THROW 'boom'", nullptr);
    CHECK(thrown.has_value());
    if (thrown) CHECK(!thrown.value()[0].ok());
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P12 standard-coverage tests ===\n");
    std::printf("compiled as c++%ld\n", sdb::cpp_standard);
    std::printf("  cxx17:%d cxx20:%d cxx23:%d\n",
                (int)sdb::has_cxx17, (int)sdb::has_cxx20, (int)sdb::has_cxx23);
    std::printf("  concepts:%d ranges:%d span:%d expected:%d format:%d\n",
                (int)sdb::has_concepts, (int)sdb::has_ranges, (int)sdb::has_span,
                (int)sdb::has_std_expected, (int)sdb::has_format);
    std::printf("  exceptions:%d deleted_reasons:%d\n\n",
                (int)sdb::has_exceptions, (int)sdb::has_deleted_reasons);

    test_standard_is_reported_honestly();
    test_gates_are_one_way_doors();
    test_cxx17_baseline();
    test_cxx20_additions();
    test_format_support();
    test_cxx23_additions();
    test_cxx26_additions();
    test_behaviour_is_standard_independent();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
