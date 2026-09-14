# surrealdb.cpp

A C++ wrapper for SurrealDB, built on the official C SDK.

> **This is an unofficial, community project.** It is not affiliated with,
> endorsed by, or supported by SurrealDB. The library it sits on top of —
> [surrealdb.c](https://github.com/surrealdb/surrealdb.c) — *is* official; this
> one is not. Pre-1.0 and under active development: expect the API to move.

C++17 or later, no dependencies beyond the C SDK and the standard library.

The C++ layer compiles to nothing: it is headers, and an INTERFACE target, so
there is no build of its own, no ABI to match and no standard or exception mode
baked in ahead of you. That is not the same as drop-in — linking still pulls in
surrealdb.c, a Rust staticlib with the whole database engine inside it. See
[Building](#building).

## Getting started

```cpp
#include <surrealdb/surrealdb.hpp>

namespace sdb = surrealdb;

auto opened = sdb::connection::connect("mem://");
if (!opened) { /* opened.error().message() */ return 1; }
auto db = std::move(opened).value();

if (auto r = db.use("example", "demo"); !r) return 1;

sdb::object_builder person;
person.set("name", "Alice").set("age", static_cast<std::int64_t>(34));
auto created = db.create("person:alice", person);
```

Nothing throws. Every fallible call returns `result<T>`, which is this library's
own type at every supported standard — never an alias for `std::expected`, so
its identity cannot depend on the `-std` a consumer happens to compile with. On
C++23 it gains `to_std()` for interop.

Handles are move-only and release themselves. A `connection` disconnects, a
`stream` is killed, a value is freed — there is no `close()` you can forget.

## Querying

Values are **bound, never interpolated**:

```cpp
auto q = dsl::select("person")
             .fields("name", "age")
             .where(dsl::f("age") > 30 && dsl::f("city") == city_from_user)
             .order_by("name")
             .limit(10)
             .build();

auto rows = db.run(q.value());
```

`city_from_user` travels as a query variable, so a value containing a quote or a
semicolon cannot change the shape of the statement. Identifiers cannot be bound —
SurrealQL takes them as syntax — so they are validated instead and a bad one
fails the build rather than reaching the server.

### Building arrays

```cpp
array_builder tags;
tags.push("a").push("b").push("c");

object_builder rec;
rec.set("name", "ada").set("tags", tags);     // a list in a field
rec.set_unique("seen", tags);                 // stored as a set

object_builder vars;
vars.set("ids", make::array(tags));           // bound: WHERE x IN $ids
```

Lists nest, which is what GeoJSON coordinates need:

```cpp
array_builder coords;  coords.push(1.0).push(2.0);
array_builder ring;    ring.push(coords).push(coords);
```

This needs **surrealdb.c 0.2.3 or newer**. Before that `sr_value_array` took no
argument, so the only array value that existed was an empty one — a list could
not be bound or stored, only written into the query text.

`array_builder::push` copies at the call, which is what lets you push a value
and drop it on the next line. The cost is that appending is quadratic — the C
API's push allocates a new array each time. That is the right trade for the
handful of arguments a query binds.

When the values already exist as a run, use `array_from` instead. One
allocation, measured at `-O2`:

| n | `push()` | `array_from()` |
|---:|---:|---:|
| 1000 | 19.8 ms | 0.051 ms |
| 2000 | 94.8 ms | 0.071 ms |
| 4000 | 427.5 ms | 0.256 ms |

### Reading the results

A query returns one entry per statement, and **a statement can fail without the
query failing**. That matters more than it sounds: a failed statement has no
rows, so a reader that goes straight for the rows sees a thrown error as an
empty table.

For the usual one-statement query, `single()` folds that away:

```cpp
auto results = db.run(q.value());
if (!results) return;                  // the query itself failed to parse

auto rows = results.value().single();  // fails if the statement did
if (!rows) { log(rows.error().message()); return; }

for (value row : rows.value()) { ... }
```

Note the named `results`. Rows are a *view* into the results that own them, so
reading straight off a temporary —

```cpp
auto rows = db.run(q.value()).value().single();   // does not compile
```

— would leave the view pointing at freed memory. That one is a compile error:
the accessors that hand out borrows are `&`-qualified with their rvalue
overloads deleted. One relative does slip through, because no amount of
qualification can catch it:

```cpp
for (auto st : db.run(q.value()).value()) { ... }   // dangles below C++23
```

Range-for extends the reference `value()` returned, not the temporary behind
it. C++23 fixed this (P2718R0); at C++17 and C++20 it reads freed memory. It is
the same hole `std::optional` has, and the same fix — name the owner.

For a batch, iterate — and check each statement, because one failing does not
stop the rest:

```cpp
for (statement_result st : results.value()) {
    if (!st.ok()) { log(st.message()); continue; }
    for (value row : st.rows()) { ... }
}
```

`all_ok()` and `first_error()` are there for when you just want to know whether
the batch went through. `single()` deliberately refuses a multi-statement
result rather than quietly handing back the first one.

### When the shape is decided at runtime

Chaining changes the expression's type at each step, so it cannot accumulate in
a loop or behind a condition. For that, use the scope-guard surface:

```cpp
dsl::query q;
{
    auto sel = q.select("person");
    {
        auto w = sel.where();
        for (const auto& filter : filters)   // any number, including none
            w.eq(filter.field, filter.value);
    }
    sel.limit(10);
}
auto built = q.build();
```

Clauses close when their scope ends, and an empty `WHERE` disappears rather than
emitting a dangling keyword. Both surfaces write through the same assembler, so
they emit identical SurrealQL — there is a test that compares them.

**Use the fluent form.** Reach for the guards when the query's shape is decided
at runtime.

## Live queries

```cpp
auto s = db.live("person");
auto stream = std::move(s).value();

while (auto n = stream.next()) {
    if (!n.value()) break;              // stream ended
    handle(n.value()->action(), n.value()->data());
}
```

Three things about this API are worth knowing before you use it:

- **It blocks.** There is no timeout and no non-blocking variant. Drive it from
  a dedicated thread, not a latency-sensitive one.
- **There is no cancellation.** A reader parked in `next()` cannot be released
  from another thread, because killing the stream frees the very thing that
  reader is using. Call it only when an event is expected.
- **Teardown is ordered.** A stream borrows its connection's runtime, so it must
  be destroyed first. Natural scoping already does this; a stream moved
  somewhere longer-lived does not, and the library detects that rather than
  running a kill against a dead runtime.

A range-for works, but note it blocks *past* the last event you know about — the
loop increments before re-testing its condition. Break from inside the body.

## Building values

`value` is a read-only view. Everything that *makes* a value lives in
`surrealdb::make`, and every one of them returns an `owned_value`, so who frees
what is visible at the call site:

```cpp
auto pt   = make::point(13.41, 52.52);
auto when = make::datetime("2026-09-13T10:00:00Z");
auto exact = make::decimal("0.1");          // text, so the precision survives
```

Geometry takes containers, not pointer-and-length:

```cpp
std::vector<make::coord> ring{{0,0}, {0,1}, {1,1}, {1,0}, {0,0}};
auto area = make::polygon(ring);                    // one ring, no holes

std::vector<std::vector<make::coord>> rings{ring, hole};
auto with_hole = make::polygon(rings);              // exterior first, then holes
auto region    = make::multipolygon(rings);         // one ring per member
```

`polygon` reads its argument: a container of coordinates is a single ring, a
container of rings is exterior-plus-holes — the same shape as GeoJSON's
`coordinates`, so GeoJSON passes through unchanged. Polygons with holes need
**surrealdb.c 0.2.5**.

For a multipolygon whose members have holes, compose it from polygon values
rather than from rings:

```cpp
std::vector<owned_value> parts;
parts.push_back(make::polygon(rings));              // this one has a hole
parts.push_back(make::polygon(ring));
auto region = make::multipolygon(parts);
```

Every member must be a polygon; anything else gives a `none` value rather than
a malformed multipolygon.

Ranges are the one place ownership is not obvious, so the API states it:
**a bound consumes the value you build it from.**

```cpp
auto r = make::range(make::included(make::integer(1)),
                     make::excluded(make::integer(10)));
```

`make::included` takes its `owned_value` by value because the C function
reclaims the box. A bound you build and then never pass to `make::range` frees
its own value rather than leaking it.

Values compare structurally with `==`, and `debug_print` exists for when you
want to see one — its format is the C library's and is not stable.

## Reading geometry and record ids

Both are tagged unions in C. Neither looks like one here:

```cpp
auto g = geometry(row);                     // a geometry_ref
switch (g.kind()) {
    case geometry_kind::point:
        if (auto c = g.as_point()) use(c->x, c->y);
        break;
    case geometry_kind::polygon:
        if (auto p = g.as_polygon()) {
            for (coord xy : p->exterior()) use(xy);
            for (auto hole : p->interiors()) use(hole);
        }
        break;
    case geometry_kind::collection:
        if (auto members = g.as_collection())
            for (geometry_ref member : *members) recurse(member);
        break;
    default: break;
}
```

Note the `if (auto members = ...)` rather than iterating `*g.as_collection()`
directly: the accessor returns an `optional` by value, and a range-for over
`*optional` binds to a reference into a temporary that dies at the semicolon.
Naming it first costs one line and is what the compiler wants.

An accessor for the wrong shape returns `nullopt` rather than reinterpreting
the union, and a value that is not a geometry gives an invalid `geometry_ref`
instead of a null dereference. `visit` dispatches if you would rather not
switch — give it an overload taking `std::monostate` and a shape added in a
future SurrealDB will land there rather than silently taking a branch meant for
something else.

Record ids are the same idea. All four id kinds arrive from the database:

```cpp
auto t = thing(row);
if (t) {
    use(t.table());                          // string_view
    switch (t.kind()) {
        case id_kind::text:   use(*t.as_text());   break;
        case id_kind::number: use(*t.as_number()); break;
        case id_kind::array:
            if (auto items = t.as_array())
                for (value v : *items) use(v);
            break;
        case id_kind::object: use(*t.as_object()); break;
    }
}
```

One asymmetry worth knowing: `make::thing` builds only text ids, and
`make::polygon` builds only a single ring. For a numeric or composite id, bind
the parts and let the server assemble it — `type::record($tb, $id)` — which
keeps the value out of the query text.

Everything here is a **view** into the value it came from. The value has to
outlive it, the same rule as `array_view`.

## Records, edges, and patches

Beyond the usual `select` / `create` / `update` / `merge` / `delete`:

Signing in and up take the fields the access method's query reads, which
record-level auth almost always needs:

```cpp
object_builder fields;
fields.set("email", email).set("pass", pass).set("nickname", nick);

access acc{"ns", "db", "account"};
auto token = db.signup(acc, fields);
```

A SIGNIN clause may select on any field, so a username and password pair is the
common case rather than the shape. The root, namespace and database levels
authenticate on credentials alone and ignore the extra fields.

```cpp
db.call("string::uppercase", args);         // any fn:: definition or built-in
db.insert_relation("follows", edge);        // an edge that carries its own ends
db.patch_add("person:ada", "/tags/0", tag); // edit one field, not the record
```

`patch_add`, `patch_replace` and `patch_remove` take a JSON Pointer. Be aware
that **SurrealDB is not RFC 6902 here**: a missing path is not an error.
`patch_replace(r, "/not/a/path", v)` succeeds and leaves `{not:{a:{path:v}}}`
behind, and removing a path that is not there is a silent no-op. If you need
strict behaviour, read the record first — these calls will not tell you.

## RPC and sessions

`connection` speaks typed verbs. An `rpc` context speaks the wire protocol,
which is what you want when you are proxying clients rather than querying
yourself: a request arrives encoded, and you can forward it untouched.

```cpp
auto ctx = rpc::connect("memory").value();

auto session = ctx.attach().value();        // one per connected client
auto reply   = ctx.execute_on(session, request_bytes);
```

**Payloads are CBOR, and this library neither encodes nor decodes it.** Doing
so would mean vendoring a codec or taking a dependency, and the point of this
wrapper is that it pulls in nothing. Bytes go in, bytes come out, and the codec
stays your choice.

Note that **a successful result does not mean the query succeeded.** A parse
error fails the call, with the server's diagnostic attached, and nothing runs.
A *runtime* error succeeds at this level and is reported inside the reply: a
`query` reply is an array with one entry per statement, each carrying `status`
(`"OK"` or `"ERR"`), `result`, `time` and `type`, and a failing statement does
not stop the ones after it. That is the same split every other SurrealDB client
sees — which is exactly what lets a proxy forward the reply untouched — but a
caller who checks only `has_value()` will read a failed statement as a success.

What this does give you is the part that is easy to get wrong. SurrealDB 3.1
removed the implicit "current session" ([GHSA-4vgr-h27g-cf9p][ghsa]); every
request names one. A context mints a session for itself so `execute()` reads
the same as before, and `attach()` / `execute_on()` are how one context serves
many clients without leaking authentication state between them. An id that was
never attached is refused rather than quietly falling back.

`reset` and `detach` are different on purpose: reset keeps the id valid but
clears what the session had selected, detach removes it and cancels the live
queries and transactions it owned.

[ghsa]: https://github.com/surrealdb/surrealdb/security/advisories/GHSA-4vgr-h27g-cf9p

## What is covered

Every capability surrealdb.c exports is reachable from here. Three of its 102
functions are never called, each for a stated reason:

| Function | Instead |
|---|---|
| `sr_array_len`, `sr_array_get` | `array_view::size()` / `at()` read the `{ptr, len}` struct directly, rather than crossing the FFI boundary once per element |
| `sr_object_insert_int` | `sr_value_int` + insert. The direct shim took a C `int` and truncated past 32 bits before surrealdb.c 0.2.1, and this library can be built against a system copy of either version. One allocation beats a quiet wrong answer |

Five of SurrealDB's 38 RPC methods have no dedicated C function, so they are
reached by sending a request through `rpc` rather than by calling a method:
`info`, `refresh`, `revoke`, `gql` and `graphql`. All five work — `gql` (the
MATCH-based graph language) and `graphql` need `allow_experimental` set in the
connection options, and `graphql` also wants `DEFINE CONFIG GRAPHQL` on the
database. This is a missing shortcut, not a missing capability.

Nothing in SurrealDB's type system is out of reach. Every value kind can be
built, bound and read back — including a `GeometryCollection`, which needs
surrealdb.c 0.2.4:

```cpp
auto a = make::point(1.0, 2.0);
auto b = make::polygon(ring);
auto region = make::collection({value(a.get()), value(b.get())});
```

Members are copied, so they stay yours. **Every member must be a geometry** —
pass anything else and you get a `none` value rather than a half-built
collection, so check the kind when the members came from somewhere you don't
control.

One thing that trips people up: a GeoJSON *object* is not a geometry.
SurrealDB accepts `{ type: 'Point', coordinates: [1,2] }` as geometry only when
it is written as a literal in the query text, never as a bound object — and
that is true for every geometry type. Build geometry with `make`, not with
`object_builder`.

## Options

```cpp
sdb::options opts;
opts.query_timeout(30);
opts.capabilities().scripting = sdb::toggle::off;
opts.capabilities().allow_network = sdb::target_set::none();
opts.capabilities().allow_experimental = sdb::target_set::only({"gql", "files"});

auto db = sdb::connection::connect("mem://", opts);
```

Every field defaults to "leave SurrealDB's default alone", so a
default-constructed `options` changes nothing. Unlike the C struct, the target
names are owned, so they cannot dangle. A name that cannot be parsed is an
error, never a silent skip — quietly widening a sandbox is the one outcome that
must not happen.

## Building

Nothing in *this* repository compiles. What compiles is surrealdb.c, and it is
not small: a `cargo build` of SurrealDB itself, minutes and gigabytes on a cold
cache — the debug staticlib alone is over a gigabyte, because a Rust `.a` keeps
every dependency. Everything after the first build is cached.

One exception to "nothing compiles" — `SURREALDB_USE_MODULES=ON` builds the
C++20 module interface into a static library. The header path stays headers.

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build
```

`find_package(surrealdb_c)` locates the C SDK, preferring an installed package
over the bundled checkout, so a system install from a distro package is used
without editing anything. Set `SURREALDB_C_FORCE_SUBPROJECT=ON` to invert that
while developing against a local checkout.

### As a consumer

```cmake
find_package(surrealdb.cpp CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE surrealdb::surrealdb_cpp)
```

### Embedded in another build

When this is not the top-level project it sets **no global CMake state** — not
the language standard, not flags, not output directories, not compile
definitions. Those belong to whoever is driving the build. The only thing it
asserts is `cxx_std_17` as an INTERFACE requirement on the target, which raises
a consumer that asked for less and never lowers one that asked for more.

To drive it from an existing toolchain:

```sh
cmake -S . -B build \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/host.cmake \
      -DHOST_CXX_COMPILER=/path/to/clang++ \
      -DHOST_CXX_STANDARD=20 \
      -DHOST_CXX_FLAGS="-fno-exceptions -fno-rtti"
```

See [docs/embedding.md](docs/embedding.md) — including why matching the host's
compiler, standard library and exception settings is not optional.

### Options

| Option | Default | |
|---|---|---|
| `BUILD_TESTING` | ON | build and register the test suites |
| `BUILD_EXAMPLES` | ON | build the two examples |
| `SURREALDB_USE_MODULES` | OFF | also build the C++20 module |
| `SURREALDB_C_FORCE_SUBPROJECT` | OFF | ignore an installed surrealdb.c |
| `SURREALDB_C_ALLOW_FETCH` | ON | clone surrealdb.c if it is missing |
| `GIT_SUBMODULE` | ON | initialise missing submodules |
| `SURREALDB_RELEASE` | OFF | build surrealdb.c's Rust library in release mode |
| `SURREALDB_SANITIZE` | "" | `address`, `undefined`, `thread`, or a comma-separated list — instruments every test target |
| `SURREALDB_CPP_SWEEP_STANDARDS` | ON | compile every suite at every supported C++ standard |

### C++20 modules

```cpp
import surrealdb;
```

The module re-exports the same headers, so the two spellings are
interchangeable — `examples/modules` and `examples/no_modules` are the same
program and print the same thing.

It re-exports *this* library, not the standard library: include what you use
from `std` as you would anywhere else.

Two build requirements, both easy to trip over:

```bash
cmake -S . -B build -G Ninja -DSURREALDB_USE_MODULES=ON -DCMAKE_CXX_STANDARD=20
```

CMake supports C++20 modules only under **Ninja** (and Visual Studio); the
Makefile generator will configure happily and then fail to find the module.

An installed package carries the module too — the `.cppm`, the compiled
interface, and a `surrealdb::surrealdb_cpp_module` target — so
`find_package(surrealdb.cpp)` followed by `import surrealdb;` works against an
installed prefix, not just this build tree.

## Standards

**C++17 is a floor, not a target, and there is no ceiling.** The C++ layer is
headers only, so it compiles with whatever flags you already use — there is no
prebuilt object to match a standard, an ABI or an exception mode against. Set
C++17, or 23, or whatever your toolchain calls the current draft; it works, and
newer features are used where they exist.

Everything the library does is available at C++17. Newer standards **add**,
never substitute:

| | C++17 | C++20 | C++23 | C++26 |
|---|---|---|---|---|
| the whole API | full | full | full | full |
| views are C++20 ranges | — | `view`, `borrowed_range` | same | same |
| `std::span` from geometry | — | `as_span()` | same | same |
| `import surrealdb;` | — | yes | yes | yes |
| `std::expected` interop | — | — | `to_std()` | same |
| `std::format` for ids, kinds, errors | — | yes¹ | yes | yes |
| deleted guards explain themselves | — | — | — | yes |

¹ needs exceptions as well as `std::format`: the `parse` contract is to throw
on a bad format spec, so under `-fno-exceptions` the formatters are absent
rather than broken.

The last row is the flavour of the whole table: the lifetime guards are
`= delete` at every standard, and at C++26 the deletion carries the reason, so
`single()` on a temporary says *"returns a view into an array that dies at the
semicolon"* instead of "use of deleted function".

Gates are detected, never inferred from `__cplusplus` — a standard library can
lag its compiler, and libc++ still ships no `std::generator` at C++23. Each gate
is a `>=` against a feature-test macro, so a standard newer than any that
existed when this was written enables everything rather than falling back to the
C++17 path. One header decides all of it; nothing else in the library looks at
`__cplusplus`.

Macros cannot cross a module boundary, so every gate also exists as a
`constexpr` constant — `surrealdb::has_ranges`, `has_span`, `cpp_standard` — and
those are what `import surrealdb;` consumers ask.

Nothing throws, and nothing requires RTTI, so `-fno-exceptions` and `-fno-rtti`
are supported — hosts that embed a library like this frequently require both.

### How that is checked

`ctest` builds **every suite at every standard the toolchain supports** and runs
the ones whose results can differ. `tests/p12_standards.cpp` asserts the
relationship between each gate and the surface it claims — that a capability
exists exactly when its gate is on, and that gates only ever open as the
standard rises. It is compiled and run at 17, 20, 23 and 26, so a claim made
there is checked in every column rather than in whichever one the host picked.

Linking is the expensive part — every binary carries the whole Rust staticlib —
so the suites whose behaviour cannot vary are compiled but not linked. Turn the
whole thing off with `-DSURREALDB_CPP_SWEEP_STANDARDS=OFF`.

`scripts/compat-matrix.sh` covers the dimension CMake does not: **30
configurations** of C++17/20/23/2b/2c × libstdc++/libc++ × default /
`-fno-exceptions` / `-fno-exceptions -fno-rtti`, driving the compiler directly
rather than through a generator.

## Requirements

- **CMake 3.31.11+** to build this repository. Consuming an installed copy needs
  far less — the package config and the headers are ordinary CMake — but the
  build here pins a newer CMake for its Ninja C23/C++23 codegen fixes.
- A **C++17** compiler. That is the floor, not the target: see
  [Standards](#standards).
- [surrealdb.c](https://github.com/surrealdb/surrealdb.c) — found automatically,
  or cloned if missing.
- A **Rust toolchain** (`cargo`), unless surrealdb.c is already installed. The C
  SDK is a Rust staticlib and is built from source; `find_package(surrealdb_c)`
  finding an installed one is what lets you skip this.

Tested on **Linux** across every configuration listed under
[Standards](#standards), with both libstdc++ and libc++. **macOS and Windows are
untested** — nothing is known to be wrong on either, but nobody has run it.

## Licence

**surrealdb.cpp is [Apache-2.0](LICENSE)** — the same licence as surrealdb.c,
which it wraps. Matching it means there is no compatibility question between
the two halves, and the patent grant reads the same on both sides of the
boundary.

That covers the wrapper in this repository. It does **not** cover what you link
against. surrealdb.c statically links the `surrealdb` and `surrealdb-core`
crates, and those are **Business Source License 1.1** — source-available rather
than open source, converting to Apache-2.0 on 2030-01-01. A binary built
against this library therefore contains BSL code, and shipping it is subject to
those terms as well as these.

The BSL's Additional Use Grant permits everything except offering the licensed
work as a *Database Service* to third parties. Embedding the engine in an
application — the case this library exists for — sits squarely inside the
grant; selling database-as-a-service built on it does not.

That is a summary written by a programmer, not advice from a lawyer.
[NOTICE](NOTICE) carries the attributions; read the BSL itself and
[surrealdb.com/legal](https://surrealdb.com/legal) before relying on any of it.
