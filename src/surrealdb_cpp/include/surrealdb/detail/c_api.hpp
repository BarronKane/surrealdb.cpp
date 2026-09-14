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
