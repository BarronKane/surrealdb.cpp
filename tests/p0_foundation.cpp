// P0 foundation tests: config / owned / error / result / invoke.
//
// Deliberately framework-free. P0 is the layer everything else stands on, so
// its tests must build with nothing but a compiler and the C library -- that is
// what makes it cheap to run them across the whole standard x stdlib matrix.

#include <surrealdb/config.hpp>
#include <surrealdb/error.hpp>
#include <surrealdb/detail/invoke.hpp>
#include <surrealdb/detail/owned.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const char* what, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

// --- a stand-in handle so ownership can be tested without a live database ----

int g_destroyed = 0;

struct fake_handle { int* tag; };

struct fake_policy {
    using handle = fake_handle;
    static handle null() noexcept { return fake_handle{nullptr}; }
    static bool valid(const handle& h) noexcept { return h.tag != nullptr; }
    static void destroy(handle) noexcept { ++g_destroyed; }
};

using owned_fake = surrealdb::detail::owned<fake_policy>;

int g_span_destroyed = 0;
int g_span_len = -1;

struct fake_span_policy {
    using element = int;
    static void destroy(int*, int n) noexcept {
        ++g_span_destroyed;
        g_span_len = n;
    }
};

using owned_fake_span = surrealdb::detail::owned_span<fake_span_policy>;

// ---------------------------------------------------------------------------

void test_config() {
    std::printf("config\n");
    // The standard actually in use must match what config.hpp concluded.
#if SURREALDB_HAS_CXX23
    CHECK(SURREALDB_CPLUSPLUS >= 202302L);
    CHECK(SURREALDB_HAS_CXX20 == 1);
#elif SURREALDB_HAS_CXX20
    CHECK(SURREALDB_CPLUSPLUS >= 202002L);
    CHECK(SURREALDB_CPLUSPLUS < 202302L);
#else
    CHECK(SURREALDB_CPLUSPLUS >= 201703L);
    CHECK(SURREALDB_HAS_CXX20 == 0);
#endif
    // Feature gates must never claim more than the standard allows.
    CHECK(!(SURREALDB_HAS_RANGES && !SURREALDB_HAS_CXX20));
    CHECK(!(SURREALDB_HAS_STD_EXPECTED && !SURREALDB_HAS_CXX23));
    // SURREALDB_REQUIRES must compile to something valid either way.
    CHECK(true);
}

void test_owned_basic() {
    std::printf("owned: lifetime\n");
    int tag = 1;
    g_destroyed = 0;
    {
        owned_fake a(fake_handle{&tag});
        CHECK(static_cast<bool>(a));
        CHECK(a.get().tag == &tag);
    }
    CHECK(g_destroyed == 1);

    // A default-constructed handle owns nothing and must not call the deleter.
    g_destroyed = 0;
    { owned_fake b; CHECK(!static_cast<bool>(b)); }
    CHECK(g_destroyed == 0);
}

void test_owned_move() {
    std::printf("owned: move semantics\n");
    int tag = 1;

    // Move construction transfers; only one destruction happens.
    g_destroyed = 0;
    {
        owned_fake a(fake_handle{&tag});
        owned_fake b(std::move(a));
        CHECK(!static_cast<bool>(a));
        CHECK(static_cast<bool>(b));
    }
    CHECK(g_destroyed == 1);

    // Move assignment destroys the overwritten handle.
    g_destroyed = 0;
    {
        owned_fake a(fake_handle{&tag});
        owned_fake b(fake_handle{&tag});
        b = std::move(a);
        CHECK(g_destroyed == 1); // b's original
    }
    CHECK(g_destroyed == 2);

    // Self-move-assignment must not destroy.
    g_destroyed = 0;
    {
        owned_fake a(fake_handle{&tag});
        owned_fake& ref = a;
        a = std::move(ref);
        CHECK(static_cast<bool>(a));
        CHECK(g_destroyed == 0);
    }
    CHECK(g_destroyed == 1);
}

void test_owned_release_reset() {
    std::printf("owned: release / reset / swap\n");
    int tag = 1;

    g_destroyed = 0;
    {
        owned_fake a(fake_handle{&tag});
        fake_handle h = a.release();
        CHECK(h.tag == &tag);
        CHECK(!static_cast<bool>(a));
    }
    CHECK(g_destroyed == 0); // released: caller owns it now

    g_destroyed = 0;
    {
        owned_fake a(fake_handle{&tag});
        a.reset(fake_handle{&tag});
        CHECK(g_destroyed == 1);
    }
    CHECK(g_destroyed == 2);

    g_destroyed = 0;
    {
        owned_fake a(fake_handle{&tag});
        owned_fake b;
        a.swap(b);
        CHECK(!static_cast<bool>(a));
        CHECK(static_cast<bool>(b));
    }
    CHECK(g_destroyed == 1);

    // owned must be move-only.
    static_assert(!std::is_copy_constructible<owned_fake>::value, "must be move-only");
    static_assert(!std::is_copy_assignable<owned_fake>::value, "must be move-only");
    static_assert(std::is_move_constructible<owned_fake>::value, "must be movable");
}

void test_owned_span() {
    std::printf("owned_span\n");
    int storage[4] = {10, 20, 30, 40};

    g_span_destroyed = 0;
    g_span_len = -1;
    {
        owned_fake_span s(storage, 4);
        CHECK(s.size() == 4);
        CHECK(!s.empty());
        CHECK(s[0] == 10);
        CHECK(s[3] == 40);

        int sum = 0;
        for (int v : s) sum += v;
        CHECK(sum == 100);
    }
    CHECK(g_span_destroyed == 1);
    CHECK(g_span_len == 4);

    // Negative lengths from a C call must be clamped, not propagated.
    g_span_destroyed = 0;
    { owned_fake_span s(storage, -1); CHECK(s.size() == 0); CHECK(s.empty()); }
    CHECK(g_span_destroyed == 1);

    // An empty span owns nothing.
    g_span_destroyed = 0;
    { owned_fake_span s; CHECK(s.size() == 0); }
    CHECK(g_span_destroyed == 0);

    g_span_destroyed = 0;
    {
        owned_fake_span s(storage, 4);
        owned_fake_span t(std::move(s));
        CHECK(s.size() == 0);
        CHECK(t.size() == 4);
    }
    CHECK(g_span_destroyed == 1);
}

void test_error() {
    std::printf("error\n");
    using surrealdb::error;
    using surrealdb::error_code;

    error e;
    CHECK(e.code() == error_code::ok);
    CHECK(e.message().empty());

    // adopt() takes ownership of a C-allocated string. Use a real one so the
    // deleter is exercised against the actual allocator.
    sr_string_t raw = nullptr;
    sr_surreal_t* db = nullptr;
    if (sr_connect(&raw, &db, "not-a-valid-endpoint://x") < 0 && raw != nullptr) {
        error adopted = error::adopt(SR_ERROR, raw);
        CHECK(adopted.code() == error_code::error);
        CHECK(!adopted.message().empty());
        CHECK(adopted.message_string() == std::string(adopted.message()));
    } else {
        std::printf("  note: bad endpoint did not produce an error string\n");
        if (raw) sr_string_free(raw);
        if (db) sr_surreal_disconnect(db);
    }

    CHECK(std::strcmp(surrealdb::to_string(error_code::fatal), "fatal") == 0);
    CHECK(surrealdb::is_ok(error_code::ok));
    CHECK(!surrealdb::is_ok(error_code::closed));

    error fatal(error_code::fatal, surrealdb::owned_string());
    CHECK(fatal.is_fatal());
    CHECK(!fatal.is_closed());
}

void test_error_is_small() {
    std::printf("error: layout\n");
    // The whole point of not holding a std::string: every successful result<T>
    // carries the error arm in its layout, so this must stay small.
    CHECK(sizeof(surrealdb::error) <= 16);
    std::printf("  sizeof(error)          = %zu\n", sizeof(surrealdb::error));
    std::printf("  sizeof(result<int>)    = %zu\n", sizeof(surrealdb::result<int>));
    std::printf("  sizeof(result<void>)   = %zu\n", sizeof(surrealdb::result<void>));
}

void test_result_value() {
    std::printf("result: value path\n");
    using surrealdb::result;

    result<int> r(42);
    CHECK(r.has_value());
    CHECK(static_cast<bool>(r));
    CHECK(r.value() == 42);
    CHECK(r.code() == surrealdb::error_code::ok);

    result<std::string> s(std::string("hello"));
    CHECK(s.has_value());
    CHECK(s.value() == "hello");
    CHECK(std::move(s).value() == "hello");
}

void test_result_error() {
    std::printf("result: error path\n");
    using surrealdb::result;
    using surrealdb::error;
    using surrealdb::error_code;

    result<int> r(error(error_code::closed, surrealdb::owned_string()));
    CHECK(!r.has_value());
    CHECK(!static_cast<bool>(r));
    CHECK(r.code() == error_code::closed);
    CHECK(r.error().is_closed());

    CHECK(std::move(r).value_or(7) == 7);
}

void test_result_move() {
    std::printf("result: move semantics\n");
    using surrealdb::result;

    result<std::string> a(std::string("abc"));
    result<std::string> b(std::move(a));
    CHECK(b.has_value());
    CHECK(b.value() == "abc");

    result<std::string> c(surrealdb::error(surrealdb::error_code::error,
                                           surrealdb::owned_string()));
    c = std::move(b);
    CHECK(c.has_value());
    CHECK(c.value() == "abc");

    static_assert(!std::is_copy_constructible<result<int>>::value,
                  "result must be move-only");

    // The owned handle inside an error must be released exactly once when a
    // result holding it is destroyed -- exercised under ASan by the runner.
    {
        result<int> tmp(surrealdb::error(surrealdb::error_code::error,
                                         surrealdb::owned_string()));
        (void)tmp;
    }
}

void test_result_void() {
    std::printf("result<void>\n");
    using surrealdb::result;

    result<void> good = surrealdb::ok();
    CHECK(good.has_value());
    CHECK(good.code() == surrealdb::error_code::ok);
    good.value();

    result<void> bad(surrealdb::error(surrealdb::error_code::fatal,
                                      surrealdb::owned_string()));
    CHECK(!bad.has_value());
    CHECK(bad.error().is_fatal());
}

void test_result_monadic() {
    std::printf("result: and_then / transform\n");
    using surrealdb::result;

    auto doubled = result<int>(21).transform([](int v) { return v * 2; });
    CHECK(doubled.has_value());
    CHECK(doubled.value() == 42);

    auto chained = result<int>(21).and_then([](int v) { return result<int>(v + 1); });
    CHECK(chained.has_value());
    CHECK(chained.value() == 22);

    // Errors short-circuit and are preserved.
    auto skipped = result<int>(surrealdb::error(surrealdb::error_code::closed,
                                                surrealdb::owned_string()))
                       .transform([](int v) { return v * 2; });
    CHECK(!skipped.has_value());
    CHECK(skipped.code() == surrealdb::error_code::closed);

    auto changed = result<int>(5).transform([](int v) { return std::to_string(v); });
    CHECK(changed.has_value());
    CHECK(changed.value() == "5");
}

#if SURREALDB_HAS_STD_EXPECTED
void test_std_interop() {
    std::printf("result: std::expected interop (C++23)\n");
    auto e = surrealdb::result<int>(42).to_std();
    CHECK(e.has_value());
    CHECK(*e == 42);

    auto bad = surrealdb::result<int>(
                   surrealdb::error(surrealdb::error_code::error,
                                    surrealdb::owned_string()))
                   .to_std();
    CHECK(!bad.has_value());
    CHECK(bad.error().code() == surrealdb::error_code::error);

    // result is NOT std::expected -- distinct types by design.
    static_assert(!std::is_same<surrealdb::result<int>,
                                std::expected<int, surrealdb::error>>::value,
                  "result<T> must never be an alias for std::expected");
}
#endif

void test_invoke_live() {
    std::printf("invoke: against a live in-memory database\n");
    namespace d = surrealdb::detail;

    // Success, value-producing: connect.
    sr_surreal_t* raw_db = nullptr;
    auto conn = d::invoke<surrealdb::owned_connection>(
        [&](sr_string_t* e) { return sr_connect(e, &raw_db, "memory"); },
        [&](int) { return surrealdb::owned_connection(raw_db); });

    CHECK(conn.has_value());
    if (!conn.has_value()) {
        std::printf("  cannot continue: %.*s\n",
                    static_cast<int>(conn.error().message().size()),
                    conn.error().message().data());
        return;
    }

    sr_surreal_t* db = conn.value().get();
    CHECK(db != nullptr);

    // Success, void-producing.
    auto ns = d::invoke([&](sr_string_t* e) { return sr_use_ns(db, e, "p0_ns"); });
    CHECK(ns.has_value());
    auto dbase = d::invoke([&](sr_string_t* e) { return sr_use_db(db, e, "p0_db"); });
    CHECK(dbase.has_value());

    // Failure: a syntactically invalid query must come back as an error with a
    // non-empty message, not a crash.
    sr_arr_res_t* qres = nullptr;
    auto bad = d::invoke<surrealdb::owned_arr_results>(
        [&](sr_string_t* e) { return sr_query(db, e, &qres, "NOT VALID SURQL !!", nullptr); },
        [&](int n) { return surrealdb::owned_arr_results(qres, n); });
    CHECK(!bad.has_value());
    if (!bad.has_value()) {
        CHECK(!bad.error().message().empty());
        std::printf("  error surfaced: %.*s\n",
                    static_cast<int>(bad.error().message().size() > 60
                                         ? 60 : bad.error().message().size()),
                    bad.error().message().data());
    }

    // Seed a row so the select below has something to find.
    //
    // NOTE: res_ptr is deliberately null here. sr_create writes its result with
    // `Box::leak` (surrealdb.c src/lib.rs:356) and the C API exposes no function
    // that frees that box -- `sr_object_free` takes the struct by value and
    // releases the inner object, not the 8-byte allocation holding it. Asking
    // for the created object therefore leaks 8 bytes per call with no way to
    // reclaim them. The C header documents `res_ptr may be null (result will be
    // discarded)`, which is the only leak-free path available until the C
    // library grows a matching destructor.
    {
        sr_object_t content = sr_object_new();
        sr_object_insert_str(&content, "name", "p0");

        // sr_create writes the created record by value into a caller-owned
        // slot, so owned<> adopts it directly and its destructor is the whole
        // release. It used to hand back a pointer to a Rust-side box that no C
        // function could reclaim, leaking 8 bytes per call.
        sr_object_t created{};
        auto c = d::invoke<surrealdb::owned_object>(
            [&](sr_string_t* e) { return sr_create(db, e, &created, "p0_tbl", &content); },
            [&](int) { return surrealdb::owned_object(created); });
        sr_object_free(content);
        CHECK(c.has_value());
        CHECK(static_cast<bool>(c.value()));
    }

    sr_value_t* vals = nullptr;
    auto sel = d::invoke<surrealdb::owned_values>(
        [&](sr_string_t* e) { return sr_select(db, e, &vals, "p0_tbl"); },
        [&](int n) { return surrealdb::owned_values(vals, n); });
    CHECK(sel.has_value());
    if (sel.has_value()) {
        std::printf("  select returned %d value(s) via the count policy\n",
                    sel.value().size());
        CHECK(sel.value().size() >= 1);
    }
    // conn's destructor disconnects.
}

} // namespace

int main() {
    std::printf("=== surrealdb.cpp P0 foundation tests ===\n");
    std::printf("standard: %ld  ranges:%d concepts:%d span:%d std_expected:%d\n\n",
                static_cast<long>(SURREALDB_CPLUSPLUS), SURREALDB_HAS_RANGES,
                SURREALDB_HAS_CONCEPTS, SURREALDB_HAS_SPAN,
                SURREALDB_HAS_STD_EXPECTED);

    test_config();
    test_owned_basic();
    test_owned_move();
    test_owned_release_reset();
    test_owned_span();
    test_error();
    test_error_is_small();
    test_result_value();
    test_result_error();
    test_result_move();
    test_result_void();
    test_result_monadic();
#if SURREALDB_HAS_STD_EXPECTED
    test_std_interop();
#endif
    test_invoke_live();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
