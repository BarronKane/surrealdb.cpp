// surrealdb.cpp — module example
//
// Identical to the header example except for the import: the module is a
// re-export of the same headers, so the two spellings are interchangeable.
// Nothing here throws; every fallible call returns a result<T>.

import surrealdb;

// The module re-exports this library, not the standard library. Anything from
// `std` a consumer uses, it includes itself -- which is the same rule as for
// the header build, just enforced rather than accidental.
#include <cstdio>
#include <tuple>
#include <utility>

namespace sdb = surrealdb;
namespace dsl = surrealdb::dsl;

int main() {
    // "mem://" in-memory, "surrealkv://file.skv" on disk,
    // "ws://host:8000" remote.
    auto opened = sdb::connection::connect("mem://");
    if (!opened) {
        std::printf("connect failed: %.*s\n",
                    static_cast<int>(opened.error().message().size()),
                    opened.error().message().data());
        return 1;
    }
    auto db = std::move(opened).value();

    if (auto r = db.use("example", "demo"); !r) {
        std::printf("use failed: %.*s\n",
                    static_cast<int>(r.error().message().size()),
                    r.error().message().data());
        return 1;
    }

    if (auto v = db.version()) {
        std::printf("server version: %s\n", v.value().get());
    }

    // Records are built as objects; nothing is stringified.
    for (auto [id, name, age, lon, lat] : {
             std::tuple{"person:alice", "Alice", 34, 13.41, 52.52},
             std::tuple{"person:bob", "Bob", 22, -0.12, 51.51},
         }) {
        sdb::object_builder rec;
        rec.set("name", name).set("age", age).set("at", sdb::make::point(lon, lat));
        if (auto r = db.create_discarding(id, rec); !r) {
            std::printf("create %s failed: %.*s\n", id,
                        static_cast<int>(r.error().message().size()),
                        r.error().message().data());
            return 1;
        }
    }

    // The DSL binds values rather than interpolating them: 30 travels as a
    // variable, so nothing a caller supplies can alter the statement.
    auto query = dsl::select("person")
                     .fields("name", "age")
                     .where(dsl::f("age") > 30)
                     .order_by("name")
                     .build();
    if (!query) {
        std::printf("could not build the query: %.*s\n",
                    static_cast<int>(query.error().message().size()),
                    query.error().message().data());
        return 1;
    }
    std::printf("query: %s\n", query.value().c_str());

    auto results = db.run(query.value());
    if (!results) {
        std::printf("query failed: %.*s\n",
                    static_cast<int>(results.error().message().size()),
                    results.error().message().data());
        return 1;
    }

    // One entry per statement; this query had one, so `single()` applies. It
    // reports a statement that failed at runtime instead of handing back an
    // empty row set, which is what reading the rows directly would do.
    auto rows_or = results.value().single();
    if (!rows_or) {
        std::printf("statement failed: %.*s\n",
                    static_cast<int>(rows_or.error().message().size()),
                    rows_or.error().message().data());
        return 1;
    }

    {
        sdb::array_view rows = rows_or.value();
        std::printf("%d row(s)\n", rows.size());
        for (sdb::value row : rows) {
            auto obj = row.as_object();
            if (!obj) continue;
            auto name = obj->get("name");
            auto age = obj->get("age");
            std::printf("  %.*s, %lld\n",
                        static_cast<int>(name ? name->as_string().value_or("").size() : 0),
                        name ? name->as_string().value_or("").data() : "",
                        static_cast<long long>(age ? age->as_int().value_or(0) : 0));
        }
    }

    // The other way in: an RPC context speaks the wire protocol rather than
    // typed verbs, which is what a server proxying clients wants. Requests and
    // replies are CBOR bytes -- this library ships no codec, so the choice
    // stays yours.
    //
    // Sessions are the part worth seeing here. Since SurrealDB 3.1 every
    // request names one, and a context mints its own so the simple path still
    // works. `attach()` makes more, one per connected client, so no client can
    // see another's authentication state.
    if (auto ctx = sdb::rpc::connect("mem://")) {
        auto rpc_ctx = std::move(ctx).value();

        if (auto own = rpc_ctx.default_session())
            std::printf("rpc default session: %s\n", own.value().to_string().c_str());

        if (auto client = rpc_ctx.attach()) {
            std::printf("attached session:    %s\n", client.value().to_string().c_str());
            if (auto all = rpc_ctx.sessions())
                std::printf("%d session(s) active\n", all.value().size());
            (void)rpc_ctx.detach(client.value());
        }
    }

    return 0;
}
