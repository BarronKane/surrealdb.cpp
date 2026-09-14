#pragma once

/// @file detail/c_api.hpp
/// The single point at which the C API enters this library.
///
/// Every other header includes *this*, never `<surrealdb.h>` directly, so the C
/// dependency stays auditable from one place.
///
/// Including the C header in consumer translation units is deliberate and
/// harmless: it declares functions and types and defines exactly ten macros,
/// every one `SR_`-prefixed (`SR_NONE`, `SR_CLOSED`, `SR_ERROR`, `SR_FATAL`,
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
// This library calls functions that do not exist in every release -- 0.2.0
// needs `sr_rpc_query_on`, added in surrealdb.c 0.3.0 -- and building against
// an older header does not fail anywhere useful.
//
// The floor matters more than usual across 0.3.x, because those releases
// *reassigned* and *withdrew* rather than only adding. `SR_NONE` meant "stream
// ended" in 0.2.x and means "nothing yet, still open" from 0.3.0; a negative
// `timeout_ms` meant "wait forever" and is an error from 0.3.1; and
// `sr_stream_next` is gone entirely. Against a 0.2.x header the renamed types
// fail to compile, which is a mercy -- the alternative is a build that succeeds
// and then reads every quiet moment as a dead stream. It fails as an undeclared identifier partway down
// stream.hpp, or, if the declaration happens to exist but the symbol does not,
// at the link with a mangled name and no hint about which half is stale.
//
// One assertion at the point the C API enters turns that into a sentence. The
// floor is checked, not the exact version: surrealdb.c is additive within a
// minor, so newer is fine and only older is a problem.
#define SURREALDB_CPP_REQUIRES_C_MAJOR 0
#define SURREALDB_CPP_REQUIRES_C_MINOR 3
#define SURREALDB_CPP_REQUIRES_C_PATCH 1

#if !defined(SR_VERSION)
#  error "surrealdb.h defines no SR_VERSION. surrealdb.cpp needs surrealdb.c v0.3.1 or newer."
#endif

static_assert(SR_VERSION >= SR_VERSION_ENCODE(SURREALDB_CPP_REQUIRES_C_MAJOR,
                                              SURREALDB_CPP_REQUIRES_C_MINOR,
                                              SURREALDB_CPP_REQUIRES_C_PATCH),
              "surrealdb.cpp requires surrealdb.c v0.3.1 or newer "
              "(SR_VERSION_STRING reports what was actually found). Update the "
              "surrealdb.c submodule, or point SURREALDB_C_ROOT at a newer one.");
