#pragma once

/// @file config.hpp
/// Feature detection and portability macros.
///
/// **C++17 is a floor, not a target, and there is no ceiling.** This library is
/// header-only: it is compiled with whatever flags its consumer already uses,
/// and it has no build of its own from which to impose a standard. Hosts that
/// embed a library like this routinely compile against a draft standard, with
/// exceptions and RTTI disabled, using their own toolchain -- none of which is
/// negotiable from here.
///
/// So every gate below is written as `>=` against a feature-test macro, never
/// as an equality or a range. A standard newer than any that existed when this
/// was written enables everything, rather than falling off the end of a ladder
/// of `#elif`s into the C++17 path -- which is the failure mode this shape
/// exists to avoid. `scripts/compat-matrix.sh` builds and runs the suites
/// across 17/20/23/2b/2c on both standard libraries, with and without
/// exceptions and RTTI.
///
/// This is the ONLY file in the library permitted to inspect `__cplusplus`,
/// `_MSVC_LANG` or any `__cpp_lib_*` macro. Everything else asks the
/// `SURREALDB_*` macros defined here. A `#if` on a standard-library feature
/// anywhere else is a defect -- it is how a codebase that claims three language
/// standards quietly decays into three codebases.
///
/// Gating rule: **gates are additive, never substitutive.** A type must never
/// *become a different type* depending on the language standard. Aliasing
/// `result<T>` to `std::expected` on C++23 and to an own implementation on
/// C++17 would produce two distinct types sharing one name -- which links
/// cleanly and corrupts at runtime. Higher standards may only *add* members or
/// capabilities to a type whose identity is fixed.

#if defined(__has_include)
#  if __has_include(<version>)
#    include <version>
#  endif
#endif

// ---------------------------------------------------------------------------
// Language standard
// ---------------------------------------------------------------------------

// MSVC reports 199711L in __cplusplus unless /Zc:__cplusplus is passed, so it
// gets asked separately. The CMake build passes the flag; this keeps the
// headers correct for consumers who do not.
#if defined(_MSVC_LANG)
#  define SURREALDB_CPLUSPLUS _MSVC_LANG
#else
#  define SURREALDB_CPLUSPLUS __cplusplus
#endif

#if SURREALDB_CPLUSPLUS < 201703L
#  error "surrealdb.cpp requires C++17 or later."
#endif

// Deliberately no upper bound. Each of these is a one-way door: once a standard
// is new enough it stays enabled, so a future standard inherits everything
// rather than needing this file edited to recognise it.

#define SURREALDB_HAS_CXX17 1
#if SURREALDB_CPLUSPLUS >= 202002L
#  define SURREALDB_HAS_CXX20 1
#else
#  define SURREALDB_HAS_CXX20 0
#endif
#if SURREALDB_CPLUSPLUS >= 202302L
#  define SURREALDB_HAS_CXX23 1
#else
#  define SURREALDB_HAS_CXX23 0
#endif

// ---------------------------------------------------------------------------
// Standard library features
// ---------------------------------------------------------------------------

// Measured 2026-09-12 on clang 22.1.8 / libc++ 220108:
//   C++17: none of the below.  C++20: ranges/span/concepts.  C++23: + expected.
// UE bundles libc++, so `std::generator` is deliberately absent from this list
// -- it is still unimplemented there and the notification stream hand-rolls an
// input range instead.

#if defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L
#  define SURREALDB_HAS_CONCEPTS 1
#else
#  define SURREALDB_HAS_CONCEPTS 0
#endif

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#  define SURREALDB_HAS_SPAN 1
#else
#  define SURREALDB_HAS_SPAN 0
#endif

#if defined(__cpp_lib_ranges) && __cpp_lib_ranges >= 201911L && SURREALDB_HAS_CONCEPTS
#  define SURREALDB_HAS_RANGES 1
#else
#  define SURREALDB_HAS_RANGES 0
#endif

/// Interop only. `result<T>` is never an alias for `std::expected` -- see the
/// gating rule above.
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#  define SURREALDB_HAS_STD_EXPECTED 1
#else
#  define SURREALDB_HAS_STD_EXPECTED 0
#endif

// ---------------------------------------------------------------------------
// Constraint macro
// ---------------------------------------------------------------------------

/// Expands to a `requires` clause where concepts exist and to nothing where
/// they do not. This is what keeps C++20 support from forking the codebase:
/// constraints sharpen diagnostics on newer standards and evaporate on C++17,
/// with no parallel SFINAE implementation to keep in sync.
// `SURREALDB_REQUIRES` used to live here, expanding to a `requires` clause at
// C++20 and to nothing below it. Removed: every constrained template in this
// library is constrained with `enable_if`, which works at C++17 and therefore at
// every standard, and adding a `requires` clause beside it would change nothing
// about which overload is chosen. It was a macro consumers could see and no
// code could use.

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

/// Contract violation check.
///
/// The library is built to work under `-fno-exceptions`, which hosts that
/// embed a library like this commonly require, so a broken precondition cannot
/// throw. Misuse that is *reachable*
/// from ordinary input is reported through `result` instead; this macro is only
/// for contracts the caller is required to have checked already -- for example
/// `result::value()` on an error state.
///
/// Define `SURREALDB_ASSERT` before including any surrealdb header to route
/// these into whatever assertion facility the host already uses.
#ifndef SURREALDB_ASSERT
#  include <cassert>
#  define SURREALDB_ASSERT(cond, msg) assert(((void)(msg), (cond)))
#endif

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

#if defined(_MSC_VER)
#  define SURREALDB_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#  define SURREALDB_FORCE_INLINE inline __attribute__((always_inline))
#else
#  define SURREALDB_FORCE_INLINE inline
#endif

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------
//
// Nothing in this library throws -- every fallible call returns `result<T>` --
// but knowing whether the *consumer* has exceptions matters for what can be
// offered them. `std::format`'s `parse` contract is to throw `format_error` on
// a bad spec, so the formatters cannot exist under `-fno-exceptions`: they
// would fail to compile, taking the whole umbrella header with them. Hosts that
// embed a library like this frequently build without exceptions, so that is not
// a corner case.

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#  define SURREALDB_HAS_EXCEPTIONS 1
#else
#  define SURREALDB_HAS_EXCEPTIONS 0
#endif

#if defined(__cpp_lib_format) && __cpp_lib_format >= 201907L
#  define SURREALDB_HAS_FORMAT 1
#else
#  define SURREALDB_HAS_FORMAT 0
#endif

// There is deliberately no gate for `std::unreachable`, and it is worth saying
// why: the obvious place for it is the line after an exhaustive switch over one
// of this library's enums. That would be wrong. Those enums mirror C tags, and
// the C API can hand back a value none of them name -- `SR_GEOMETRY_UNIMPLEMENTED`
// exists for exactly that. The defensive `return "unknown"` is the correct
// answer there; `std::unreachable()` would turn a forward-compatible read into
// undefined behaviour the first time SurrealDB adds a shape.
//
// `std::to_underlying` likewise has no use here. A gate the library never acts
// on is worse than no gate: it advertises a capability to consumers and then
// does nothing with it, which is how `SURREALDB_HAS_SPAN` sat dead for months.

// ---------------------------------------------------------------------------
// Deleted functions that say why
// ---------------------------------------------------------------------------
//
// C++26 lets `= delete` carry a message. This library deletes a good deal on
// purpose -- every rvalue overload that would hand out a view into a temporary
// -- and "use of deleted function" is a poor way to learn that you wrote a
// dangling read. Where the standard allows it, the guard explains itself; below
// C++26 it is an ordinary `= delete` and the comment above it has to do the
// work.

#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
#  define SURREALDB_DELETED(why) = delete(why)
#else
#  define SURREALDB_DELETED(why) = delete
#endif

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
//
// The library's own version, not surrealdb.c's and not SurrealDB's. Available
// as macros for `#if` and as constants below for module consumers.

#define SURREALDB_CPP_VERSION_MAJOR 0
#define SURREALDB_CPP_VERSION_MINOR 2
#define SURREALDB_CPP_VERSION_PATCH 0
#define SURREALDB_CPP_VERSION_STRING "0.2.0"

/// Comparable as one number: 0.1.0 is 100, 1.2.3 is 10203.
#define SURREALDB_CPP_VERSION                       \
    (SURREALDB_CPP_VERSION_MAJOR * 10000 +          \
     SURREALDB_CPP_VERSION_MINOR * 100 +            \
     SURREALDB_CPP_VERSION_PATCH)

// ---------------------------------------------------------------------------
// The same answers, as values
// ---------------------------------------------------------------------------
//
// Everything above is a macro, and **macros do not cross a module boundary**.
// A consumer who writes `import surrealdb;` sees none of them, so a library
// that only reported its configuration through macros would be telling module
// users nothing at all.
//
// These mirror the macros as `constexpr`, which the module can export. Use
// whichever fits: the macros for `#if`, these for `if constexpr` and for
// anything a module consumer needs to read.

namespace surrealdb {

/// The standard this translation unit is being compiled as -- `_MSVC_LANG`
/// where that is the honest answer, `__cplusplus` elsewhere.
inline constexpr long cpp_standard = SURREALDB_CPLUSPLUS;

inline constexpr bool has_cxx17        = SURREALDB_HAS_CXX17 != 0;
inline constexpr bool has_cxx20        = SURREALDB_HAS_CXX20 != 0;
inline constexpr bool has_cxx23        = SURREALDB_HAS_CXX23 != 0;

inline constexpr bool has_concepts     = SURREALDB_HAS_CONCEPTS != 0;
inline constexpr bool has_ranges       = SURREALDB_HAS_RANGES != 0;
inline constexpr bool has_span         = SURREALDB_HAS_SPAN != 0;
inline constexpr bool has_std_expected = SURREALDB_HAS_STD_EXPECTED != 0;
inline constexpr bool has_format         = SURREALDB_HAS_FORMAT != 0;
inline constexpr bool has_exceptions     = SURREALDB_HAS_EXCEPTIONS != 0;

/// True when `= delete` can carry a reason (C++26). The guards below use
/// `SURREALDB_DELETED`, which degrades to a plain `= delete`.
inline constexpr bool has_deleted_reasons =
#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
    true;
#else
    false;
#endif

inline constexpr int version_major = SURREALDB_CPP_VERSION_MAJOR;
inline constexpr int version_minor = SURREALDB_CPP_VERSION_MINOR;
inline constexpr int version_patch = SURREALDB_CPP_VERSION_PATCH;
inline constexpr int version       = SURREALDB_CPP_VERSION;
inline constexpr const char* version_string = SURREALDB_CPP_VERSION_STRING;

} // namespace surrealdb
