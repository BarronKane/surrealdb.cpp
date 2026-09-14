// A consumer that knows nothing about this repository's layout.
//
// It reaches the library the way anyone else would -- `find_package`, an
// installed prefix, `#include <surrealdb/surrealdb.hpp>` -- so it exercises the
// packaging rather than the code. Everything else in the suite builds against
// the source tree and therefore cannot see a broken install.
//
// It caught one: the exported target carried `INTERFACE_LINK_LIBRARIES
// "surrealdb_c"` because CMake resolves ALIAS targets when it writes an export,
// so `find_package` configured cleanly and then failed with "cannot find
// -lsurrealdb_c" -- with the library sitting correctly in the same prefix.
#include <surrealdb/surrealdb.hpp>
#include <cstdio>
namespace sdb = surrealdb;
int main(){
  std::printf("surrealdb.cpp %s (v%d), c++%ld\n",
              sdb::version_string, sdb::version, sdb::cpp_standard);
  auto c = sdb::connection::connect("memory");
  if (!c) { std::printf("connect failed\n"); return 1; }
  auto db = std::move(c).value();
  if (!db.use("n","d")) return 1;
  if (auto v = db.version()) std::printf("server %s\n", v.value().get());

  sdb::array_builder tags; tags.push("a").push("b");
  sdb::object_builder rec; rec.set("name","ada").set("age", 36).set("tags", tags);
  if (!db.create("person:ada", rec)) { std::printf("create failed\n"); return 1; }

  auto q = sdb::dsl::select("person").fields("name","age").where(sdb::dsl::f("age") > 30).build();
  if (!q) return 1;
  auto res = db.run(q.value());
  if (!res) return 1;
  auto rows = res.value().single();
  if (!rows) { std::printf("statement failed\n"); return 1; }
  std::printf("%d row(s)\n", rows.value().size());
  for (sdb::value row : rows.value())
    if (auto o = row.as_object())
      if (auto n = o->get("name"))
        std::printf("  %.*s\n", (int)n->as_string().value_or("").size(),
                    n->as_string().value_or("").data());
  return 0;
}
