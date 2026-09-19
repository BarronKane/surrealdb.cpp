#pragma once

/// @file detail/c_api.hpp
/// The single point at which the C API enters this library.
///
/// Every other header includes *this*, never `<surrealdb.h>` directly, so the C
/// dependency stays auditable from one place.
///
/// Including the C header in consumer translation units is deliberate and
/// harmless: it declares functions and types and defines exactly ten macros,
/// every one `SR_`-prefixed (`SR_AGAIN`, `SR_CLOSED`, `SR_ERROR`, `SR_FATAL`,
/// `SR_VERSION_*`). There is no collision surface, which is why this library is
/// header-only rather than hiding the C API behind a compiled boundary -- a
/// boundary would buy nothing and would cost cross-translation-unit inlining,
/// and the whole design depends on `db.select(...)` collapsing to the underlying
/// `sr_select` call with a not-taken error branch.
///
/// **Diagnostics are suppressed across the include.** cbindgen maps the Rust
/// `Value` enum onto anonymous structs nested in an anonymous union, which is a
/// GNU/C11 extension rather than standard C++. Left alone it emits 32 warnings
/// per translation unit under `-Wpedantic` and breaks anyone building with
/// `-Werror`. Since this library is header-only, that header reaches every
/// consumer TU, so containing its diagnostics here is the library's job. The
/// suppression is scoped to the include and restored immediately -- it never
/// leaks into user code.

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wpedantic"
#  pragma clang diagnostic ignored "-Wnested-anon-types"
#  pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#elif defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4201) // nameless struct/union
#endif

extern "C" {
#include <surrealdb.h>
}

#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// Minimum surrealdb.c
// ---------------------------------------------------------------------------
//
// This library calls functions that do not exist in every release -- 0.1.4
// needs `sr_session_fork`, `sr_runtime_init`, `sr_rpc_kill_on` and the
// transaction handle API --
// and building against an older header does not fail anywhere useful. It fails
// as an undeclared identifier partway down a header the user did not write, or,
// if the declaration happens to exist but the symbol does not, at the link with
// a mangled name and no hint about which half is stale.
//
// The floor matters more than usual across 0.3.x, because those releases
// *reassigned*, *withdrew* and *renamed* rather than only adding. Zero meant
// "stream ended" in 0.2.x and means "nothing yet, still open" from 0.3.0; it
// was spelled `SR_NONE` until 0.3.2 and is `SR_AGAIN` now; a negative
// `timeout_ms` meant "wait forever", became an error in 0.3.1, and means
// "forever" again in 0.3.2. Only the rename fails to compile. The rest are live
// semantic changes behind a stable spelling, which is why the readers here work
// from the sign and why this floor is asserted rather than assumed.
//
// One assertion at the point the C API enters turns a stale dependency into a
// sentence. The floor is checked, not the exact version: surrealdb.c is
// additive within a patch, so newer is fine and only older is a problem.
#define SURREALDB_CPP_REQUIRES_C_MAJOR 0
#define SURREALDB_CPP_REQUIRES_C_MINOR 3
#define SURREALDB_CPP_REQUIRES_C_PATCH 2

#if !defined(SR_VERSION)
#  error "surrealdb.h defines no SR_VERSION. surrealdb.cpp needs surrealdb.c v0.3.2 or newer."
#endif

static_assert(SR_VERSION >= SR_VERSION_ENCODE(SURREALDB_CPP_REQUIRES_C_MAJOR,
                                              SURREALDB_CPP_REQUIRES_C_MINOR,
                                              SURREALDB_CPP_REQUIRES_C_PATCH),
              "surrealdb.cpp requires surrealdb.c v0.3.2 or newer "
              "(SR_VERSION_STRING reports what was actually found). Update the "
              "surrealdb.c submodule, or point SURREALDB_C_ROOT at a newer one.");

// ---------------------------------------------------------------------------
// Capabilities of the C, as opposed to capabilities of the standard
// ---------------------------------------------------------------------------
//
// config.hpp owns the `SURREALDB_HAS_*` gates that describe the *language*.
// This one describes the *dependency*, so it lives here where `SR_VERSION` is
// visible.
//
// `sr_stream_next` blocks with no bound. That was unusable for two releases:
// a killed live query never reported its end, so a reader parked in it could
// not be released by anything short of ending the process. `stream::next()` and
// stream iteration were `= delete`d on this flag.
//
// The anchored surrealdb.c now carries the live-query teardown fixes, so `KILL`
// and `REMOVE TABLE` end a stream and the parked reader returns. The flag is on
// and both members are back.
//
// Still pinned to the dependency rather than probed: the fix is in the Rust
// surrealdb.c builds against, not in any symbol or macro the C header exposes.
// If the anchor is ever moved back to a published release that lacks it, set
// this to 0 and the deleted members return with their explanation.
#define SURREALDB_HAS_UNBOUNDED_STREAM_READ 1

namespace surrealdb {

/// Whether `stream::next()` and stream iteration exist. False while the
/// upstream live-query teardown fix was outstanding; see the note above.
/// `rpc_stream::next()` was never affected -- that path reads the datastore's
/// broker channel directly and never passed through the gate that dropped the
/// terminal notification.
inline constexpr bool has_unbounded_stream_read =
    SURREALDB_HAS_UNBOUNDED_STREAM_READ != 0;

/// The minimum surrealdb.c this library was built to call.
inline constexpr int required_c_version =
    SR_VERSION_ENCODE(SURREALDB_CPP_REQUIRES_C_MAJOR,
                      SURREALDB_CPP_REQUIRES_C_MINOR,
                      SURREALDB_CPP_REQUIRES_C_PATCH);

/// The surrealdb.c actually being compiled against, same encoding.
inline constexpr int c_version = SR_VERSION;

} // namespace surrealdb
