// The module consumed from an installed prefix.
//
// Installing a modules build used to produce a package with no
// `surrealdb::surrealdb_cpp_module` in it at all: the module existed only
// inside the build tree, so `find_package` succeeded and `import surrealdb;`
// was simply unavailable. Nothing else notices, because every other target
// builds against the sources.
import surrealdb;

// The module re-exports this library, not the standard library.
#include <cstdio>
#include <utility>

namespace sdb = surrealdb;

int main() {
    std::printf("module consumer: surrealdb.cpp %s, c++%ld\n",
                sdb::version_string, sdb::cpp_standard);

    auto opened = sdb::connection::connect("memory");
    if (!opened) return 1;
    auto db = std::move(opened).value();
    if (!db.use("n", "d")) return 1;

    sdb::array_builder tags;
    tags.push("a").push("b");
    sdb::object_builder rec;
    rec.set("name", "ada").set("age", 36).set("tags", tags);
    if (!db.create("person:ada", rec)) return 1;

    // The DSL operators are the ones that were unexported once.
    auto q = sdb::dsl::select("person").fields("name").where(sdb::dsl::f("age") > 30).build();
    if (!q) return 1;
    auto res = db.run(q.value());
    if (!res) return 1;
    auto rows = res.value().single();
    if (!rows) return 1;
    std::printf("%d row(s)\n", rows.value().size());

    // And geometry, the newest surface.
    auto pt = sdb::make::point(1.0, 2.0);
    auto coll = sdb::make::collection({sdb::value(pt.get())});
    if (sdb::geometry(sdb::value(coll.get())).kind() != sdb::geometry_kind::collection) return 1;
    std::printf("geometry ok\n");
    return 0;
}
