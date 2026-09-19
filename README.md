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

### Reading, changing, writing back

A record you read comes back as an `object_view`, which is read-only. To change
a field and send it back, copy it into a builder:

```cpp
auto made = db.create("person:ada", seed);
object_builder edited{view(made.value())};    // every field, `id` included
edited.set("age", 37);
auto r = db.update("person:ada", edited);
```

The copy is `O(n)` and allocates, where `array_builder(array_view)` is one bulk
copy — objects are opaque on the C side, so the keys have to be enumerated and
each value looked up. It needs **surrealdb.c 0.3.2 or newer** for
`sr_object_from_entries`, which at least makes the write half one call.

Every entry point that takes object content — `create`, `update`, `merge`,
`insert`, `relate`, query variables, auth params — accepts any of the shapes an
object arrives as, so nothing has to be rebuilt just to change its type:

```cpp
db.query("RETURN $n + 1", vars);          // a builder
db.query("RETURN $n + 1", &vars);         // a pointer to one
db.query("RETURN $n + 1", vars.view());   // a borrowed view
db.query("RETURN 1");                     // nothing at all
```

`make::object` takes the same set, which is what keeps an `owned_object` off
`create()` from being a dead end. `array_arg` does the same job for arrays,
including the `array_view` off a query result.

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
stream.close();
```

Three things about this API are worth knowing before you use it:

- **`next()` blocks, and a killed query now ends the stream.** That second half
  was not true until recently: a killed live query stayed open and silent, so a
  reader parked here could not be released by anything short of ending the
  process. `next()` and range-for were `= delete`d on that for two releases.
  They are back — `surrealdb::has_unbounded_stream_read` is the flag, and it
  tracks the anchored dependency rather than the standard.
- **`next()` still cannot wake for anything else.** A reader that also has to
  notice a shutdown flag wants `next_for()` below, because killing the *stream*
  from another thread frees the very object that reader is borrowing.
- **Teardown is ordered.** A stream borrows its connection's runtime, so it must
  be destroyed first. Natural scoping already does this; a stream moved
  somewhere longer-lived does not, and the library detects that rather than
  running a kill against a dead runtime.

A range-for works and reads better for a dedicated worker thread, with one
caveat: it blocks *past* the last event you know about, because the loop
increments before re-testing its condition. Break from inside the body.

### Bounded waits

A reader that has to stay responsive — to a shutdown flag, usually — waits with
a bound instead.

```cpp
for (;;) {
    auto r = stream.next_for(std::chrono::milliseconds(100));
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
```

`try_next()` is the same thing with a zero timeout: take an event if one is
already there, otherwise say so immediately.

**A timeout is not the end of the stream, and the two must not be merged.** Read
a timeout as an end and you abandon a live query over a quiet hundred
milliseconds; read an end as a timeout and you spin forever on a stream that is
finished. That is why these return `poll<T>` rather than an `optional` that
cannot tell you which happened — four states, all of which a looping reader has
to answer for.

An expired wait consumes nothing. Notifications queue in a channel and a poll
that finds it empty leaves any later arrival in place, so polling in a loop
cannot lose an event.

`rpc_stream` has the same pair. One difference, inherited from the C: its wait
is a poll at millisecond granularity rather than a timer, because an
`rpc_stream` outlives the context it came from and must not hold a handle to a
runtime that may already be shut down. Prefer tens of milliseconds over one.

### Ending a live query

```cpp
stream.close();     // retires the subscription and releases the reader
```

One call, no id needed, and the destructor calls it — so scope exit is already
correct. `connection::kill()` covers the other case: a subscription you have an
id for and no stream, such as a bare `LIVE SELECT` run through `query()`.
Calling both is harmless, just unnecessary.

Killing a subscription now *tells* any stream reading it: the stream reports its
end rather than going quiet.

None of that used to be true, and the history is worth a paragraph because it
explains the shape of this API. Freeing a reader left the subscription
registered — `INFO FOR TABLE` carries a `lives` count, and it stayed up — so
teardown needed both calls. Killing a live query left its stream open and
silent forever, on every route: not just `KILL` but `REMOVE TABLE`, which no
caller asked for, so "only block when an event is coming" was not a discipline
anyone could keep. That is what made the unbounded read unusable.

Four upstream defects sat behind that, fixed in
[surrealdb/surrealdb#7527](https://github.com/surrealdb/surrealdb/pull/7527):
the terminal notification carried no session id so the embedded router dropped
it; the WebSocket client rejected `KILLED` when decoding it; `Stream::drop`
built malformed SurrealQL under a blank session so freeing a stream never
retired anything; and `REMOVE DATABASE`/`REMOVE NAMESPACE` destroyed
subscriptions without telling them. Until that PR ships, surrealdb.c anchors on
a fork carrying the fixes — see [Requirements](#requirements).

Sessions are the exception, and they are tidier about it. On an `rpc` context,
`detach()` and `reset()` both retire the session's live queries — so do the
authentication calls, `signin`, `signup`, `authenticate`, `refresh` and
`invalidate`, since the caller those queries were authorised for no longer
exists. Each cancelled query emits one final notification with
`action::killed`, and that *is* the signal that nothing further is coming. A
reader that ignores `killed` waits for events that can no longer arrive.

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

`make::thing` writes all four, one overload per shape:

```cpp
make::thing("k", "abc");        // k:abc
make::thing("k", 1);            // k:1

array_builder key; key.push("a").push(1);
make::thing("k", key);          // k:['a', 1]

object_builder composite; composite.set("x", 1);
make::thing("k", composite);    // k:{ x: 1 }
```

**Pick the one the record was created with.** A key of the wrong shape is a
well-formed record id that matches nothing, and the database answers a query
against it with zero rows and no error — `k:1` and `k:"1"` are different
records, and nothing anywhere tells you which one you asked for. That silence
is why there is an overload per shape rather than a string and a cast.

`make::polygon` still builds only a single ring; for a polygon with holes, bind
the rings and let the server assemble it.

Everything here is a **view** into the value it came from. The value has to
outlive it, the same rule as `array_view`.

## Transactions

```cpp
auto tx = std::move(db.begin()).value();

if (!tx.query("UPDATE account:a SET balance -= 100")) return;   // cancels
if (!tx.query("UPDATE account:b SET balance += 100")) return;   // cancels

auto done = tx.commit();
```

Statements run on the handle are scoped together: nothing they write is visible
outside until `commit()`, and `cancel()` discards the lot. **It cancels on scope
exit unless committed** — an early return, or any path that forgets, rolls back.
That is not a convenience: losing the handle without committing or cancelling
would leave the transaction open in the datastore holding its locks until it
timed out, so the destructor is load-bearing.

The point of a handle over `BEGIN; ...; COMMIT;` as one query is that **your own
code runs between statements**. A failing statement does not roll the
transaction back by itself — the error lands in that statement's slot and you
decide whether to carry on or cancel. The single-query form still works and is
the right shape when the whole transaction is known up front.

A transaction runs on a **forked session**, copied from the connection at
`begin()`, so it inherits the namespace, database and authentication in force
then. A later `use()` on the connection does not move a transaction already
open.

Both `commit()` and `cancel()` consume the handle even when they fail, because a
failed commit otherwise leaves you holding a pointer that is good for nothing.
Using one afterwards is reported, not undefined.

<details>
<summary>This was <code>= delete</code>d for a while, and why</summary>

The C used to send `BEGIN`, `COMMIT` and `CANCEL` each as its own
single-statement query — and a bare `BEGIN` is a complete query, so the
transaction opened and closed inside that one call and everything afterwards ran
outside it. All three returned success. Measured: begin, write, cancel, and the
row was still there.

A guarantee that silently is not one is worse than no guarantee, so `begin()`
became a compile error with the measurement attached rather than a caveat nobody
reads. surrealdb.c now hands back a handle that threads a transaction id through
every statement, which is what makes any of this real, and the tests assert
against rows rather than return codes for the same reason.
</details>

## Records, edges, and patches## Records, edges, and patches

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

## Sessions

One embedded database, several independent users — players, tenants, requests.
Needs **surrealdb.c 0.3.2**.

```cpp
auto player = db.new_session().value();     // own auth, inherits ns/db
player.use("game", "shard_04");
player.signin(creds);
```

A fork talks to the same engine over the same runtime and carries its own
session state: its own `USE` namespace and database, its own session variables,
its own authentication. It is **not** a second connection — no engine starts, no
runtime is built, no threads are added.

- `fork_session()` inherits everything, including authentication.
- `new_session()` clears the authentication. Reach for this one whenever the new
  session belongs to a different principal, since inheriting a parent's
  credentials is the exact leak sessions exist to prevent. Namespace and
  database are still inherited, deliberately — clearing those would hand back a
  handle that cannot run anything until you pick them again, and `use()` is
  right there.

Either handle may be destroyed first; the engine goes away with the last one.
A `stream` opened on a fork stays valid after that fork is gone, because the
runtime it borrows is reference-counted and outlives any single handle.

**Poisoning is shared.** `SR_FATAL` means the engine itself is gone, so every
handle derived from it reports poisoned too — a sibling still claiming health
while talking to a dead engine would be worse than the failure.

One thing that catches people, and caught me: **`LET` inside a query is scoped
to that query.** `LET $x = 1` followed by a separate `RETURN $x` reads `none`,
while `LET $x = 1; RETURN $x` in one call reads 1. `set()` is the session
variable, and the session is what a fork forks.

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

Unless you want an ordinary query, in which case you no longer need a codec at
all. `query_on` runs SurrealQL against a chosen session and returns the same
`query_results` `connection::query` does, per-statement error channel included
— **surrealdb.c 0.3.0**:

```cpp
object_builder vars;
vars.set("id", user_id);
auto rows = ctx.query_on(session, "SELECT * FROM person WHERE id = $id", &vars);
```

That was the hole in the session model: the typed calls live on `connection`,
which has no sessions, while sessions live here where everything had to be
hand-encoded. Per-session state applies — whatever that session did with `USE`,
with authentication, or with `LET`.

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
clears what the session had selected, detach removes it. Both cancel the
session's live queries — see [Live queries](#live-queries).

To retire a single live query on a session, use `kill_on(session, query_id)`
rather than writing `KILL` into query text. Both kill the subscription, so
either works on the database; the difference is that an `rpc` keeps a registry
of the live queries each session owns, and core only reports a kill to the
transport when the response carries a uuid — which `KILL` does not, since it
resolves to `NONE`. `kill_on` carries the id in its own parameters, so the
registry stays exact. A `KILL` in query text still kills; it just leaves a dead
entry until the session is torn down.

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

One option deserves reading before you set it. `session_dir` persists RPC
sessions across restarts, and **those files hold credentials**: a session is
serialised whole, including its authentication token, its record-authentication
data and its variables, as plain JSON. Nothing is encrypted, so anything that
can read the file can replay the session. surrealdb.c restricts the directory to
the current user (0700/0600 on unix; elsewhere the inherited ACL is all there
is), which is enough on a server — the case it was built for. It is not disk
encryption. Keep it off shared or synced volumes, and think hard before enabling
it on hardware the end user controls, where "the current user" and "the
attacker" are the same account. Left unset, nothing is written.

### Sizing the runtime

**surrealdb.c 0.3.2.** A context builds its own tokio runtime, and the defaults
are sized for a server rather than for a database embedded inside something
else:

```cpp
sdb::options slim;
slim.current_thread(true)          // one thread for the whole scheduler
    .max_blocking_threads(8)
    .thread_keep_alive(std::chrono::seconds(2))
    .disable_io(true);             // only if nothing reaches the network
```

`current_thread` is a mode rather than a count, and it is the genuinely slim
option for intermittent work. `disable_io` is free for an embedded-only context
but it is what `http://` and `ws://` endpoints — and SurrealQL's `http::*`
functions — are built on, so a context that reaches the network must leave it
alone. Zero everywhere means "the library default", so a default-constructed
`options` still changes nothing.

`slow_log` is the other one to read before setting: **slow-query logs include
bound parameters**, and parameters are how credentials travel — a `signin`
carries its password as one. The log goes wherever the host's `tracing`
subscriber points. Enabling it is a decision about credential handling, not
about verbosity.

### Process-wide settings

One setting belongs to the process rather than to a context, and must be applied
before anything opens:

```cpp
sdb::runtime_options ro;
ro.kvs_threadpool_size(8);
if (auto r = sdb::runtime_init(ro); !r) { /* r.error().message() */ }

auto db = sdb::connection::connect("mem://").value();   // after, always
```

Worth setting even at the default's own size. SurrealDB spends one worker per
core on machines with 16 or more cores and 16 below that — so a 32-core machine
spends 32 threads before a single query runs — and it *pins* one worker per core
when the size equals the core count and that count is at least 16, which fights
any engine managing its own affinity. Any value that differs from the core count
drops the pinning.

**It is not idempotent and cannot be.** The pool is built once per process, on
first use, from an environment variable read behind a lock, so a call after the
first connection does nothing — and the C, not knowing the pool already exists,
cannot tell you that it did nothing. This wrapper can for the in-process case:
it remembers whether a context has been opened and returns an error rather than
succeeding silently. Call it first and the distinction never arises.

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

### Build it with Ninja

```bash
cmake -S . -B build -G Ninja && cmake --build build
```

Not a style preference. Every test binary statically links the whole Rust
library, and in a Debug build that archive is over a gigabyte of mostly debug
info — so a link costs about 2.5 GB, and there are twenty-five of them. `make
-j` is one number for compiling and linking both, so it will start twenty links
together and ask for forty gigabytes. Ninja supports job pools, and this project
configures one: compiles stay at full parallelism while links are capped by
available memory. The configure summary prints which number it picked, and says
so when the generator cannot honour it.

It also selects `mold` or `lld` when one is on `PATH`, because the default
linker is about ten times slower on an archive this size — measured at 9.8 s a
link against 1.0 s. Note the two changes pull in opposite directions on memory:
lld is faster *and* hungrier per link, so the job pool is what makes it safe to
switch. Either can be overridden:

```bash
cmake -S . -B build -G Ninja \
      -DSURREALDB_CPP_LINKER=bfd \
      -DSURREALDB_CPP_LINK_JOBS=2
```

Set `SURREALDB_RELEASE=ON` if you do not need to debug into the C library — a
release archive is about 269 MB against 1.4 GB, which makes every link cheaper
still. Both settings are top-level only: an embedding host picks its own linker
and its own parallelism.

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
| `SURREALDB_CPP_LINKER` | auto | `mold`/`lld` if present; or name one, or empty to leave alone |
| `SURREALDB_CPP_LINK_JOBS` | auto | concurrent links, sized from RAM. Ninja only |

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
- [surrealdb.c](https://github.com/surrealdb/surrealdb.c) **v0.3.2 or newer** —
  currently anchored to a development commit on its `0.3-dev` branch rather than
  a tag, because that commit points its own dependency at a fork of SurrealDB
  carrying the live-query fixes in
  [surrealdb/surrealdb#7527](https://github.com/surrealdb/surrealdb/pull/7527).
  When that PR lands and a release carries it, the anchor moves back to a tag.
  
  found automatically, or cloned if missing. The floor is asserted twice: at
  configure time when the header can be located, and by a `static_assert` in
  `detail/c_api.hpp` that fires wherever the headers come from. Building against
  an older copy fails with a sentence rather than an undeclared identifier
  halfway down a header you did not write.

  While the anchor is a commit rather than a tag, that floor is loose: the
  functions this library needs landed *after* 0.3.2, so `SR_VERSION` reads
  0.3.2 on a checkout that has them and on one that does not. The submodule
  pointer and `SURREALDB_C_GIT_TAG` are the real pin until there is a release to
  name; `detail/c_api.hpp` names each required symbol so a stale checkout at
  least fails at the top of the dependency rather than in the middle of it.
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
