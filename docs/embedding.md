# Embedding surrealdb.cpp

This library is meant to be driven by whatever build system you already have,
rather than to impose one. That has a few concrete consequences worth stating,
because the failure mode when a library gets this wrong is quiet: the tree
still configures, still builds, and produces objects compiled differently from
everything they are about to be linked against.

## What this project will not set for you

When `surrealdb.cpp` is not the top-level project — added with
`add_subdirectory`, pulled in by `FetchContent`, or configured with a toolchain
file — it sets **no global CMake state**. Specifically it does not touch:

- `CMAKE_CXX_STANDARD`, `CMAKE_C_STANDARD`, or the `_REQUIRED` / `EXTENSIONS`
  variants
- `CMAKE_CXX_FLAGS`, `CMAKE_EXE_LINKER_FLAGS`
- output directories
- `add_compile_definitions` / `add_definitions` of any kind
- `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS`, `_WIN32_WINNT`

Those are the host's to choose. Building this project *directly* still gets
sensible defaults; they are gated on `PROJECT_IS_TOP_LEVEL`.

What it does express is a **requirement**, on the target rather than globally:

```cmake
target_compile_features(surrealdb_cpp INTERFACE cxx_std_17)
```

which raises a consumer that asked for less and never lowers one that asked for
more.

## Driving it from a host toolchain

`cmake/toolchains/host.cmake` is a generic entry point:

```sh
cmake -S . -B build \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/host.cmake \
      -DHOST_CXX_COMPILER=/path/to/clang++ \
      -DHOST_CXX_STANDARD=20 \
      -DHOST_CXX_FLAGS="-fno-exceptions -fno-rtti"
```

Everything is optional; what you do not supply is left to CMake's detection.
It also accepts `HOST_C_COMPILER`, `HOST_AR`, `HOST_RANLIB`,
`HOST_TARGET_TRIPLE`, `HOST_SYSROOT`, `HOST_C_STANDARD`,
`HOST_CXX_EXTENSIONS`, `HOST_C_FLAGS` and `HOST_EXE_LINKER_FLAGS`.

Nothing about it is engine-specific — it serves a Yocto SDK, a vendored NDK, a
superbuild, or a distro packager who needs a particular compiler just as well.

## Host-specific toolchains

`host.cmake` is the only toolchain file this project ships, and deliberately so:
it provides the *mechanism*, and a host supplies the *answers*. Knowledge about
any particular host — where its compiler lives, which standard its build system
selects, what flags it compiles with — belongs on that host's side of the
boundary, not in this library.

If the host you are embedding into bundles its own compiler and standard
library, three things need to match and none of them are optional:

- **The compiler.** Use the one the host uses.
- **The standard library.** A host that compiles `-nostdinc++` against a
  vendored libc++ needs you to do the same. A translation unit built against a
  different standard library disagrees about the layout of every `std::` type.
  **That does not produce a link error — it produces memory corruption at
  runtime.**
- **Exception and RTTI settings.** Objects built with exceptions enabled link
  cleanly against objects built without them, and misbehave later.

In the common case none of this arises: surrealdb.cpp is header-only, so the
host's own build system compiles it directly with its own flags — which is
exactly why the headers are kept standard-agnostic. The toolchain file matters
for the pieces that *are* compiled here and will end up in the host's address
space.

## Verifying

```sh
scripts/compat-matrix.sh [probe.cpp]
```

Builds **and runs** across 30 configurations: C++17/20/23/2b/2c x
libstdc++/libc++ x default / `-fno-exceptions` / `-fno-exceptions -fno-rtti`.

A host with its own toolchain should verify the same way on its side: configure
through `host.cmake`, read the compiler and flags back out of the CMake cache,
and build with exactly those — so what gets tested is what the toolchain
produces rather than a hand-written approximation of it.
