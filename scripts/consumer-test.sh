#!/usr/bin/env bash
#
# Install to a throwaway prefix, then build and run a project that finds the
# library with `find_package`.
#
# Every other suite compiles against the source tree, so none of them can see a
# broken install: wrong export, missing header, unresolvable dependency. This is
# the only check that consumes the package the way a user does.
#
# Usage: scripts/consumer-test.sh <build-dir> [source-dir] [project]
#
# `project` selects which consumer under tests/ to build -- `consumer` for the
# headers, `consumer_module` for `import surrealdb;`.

set -euo pipefail

BUILD_DIR="${1:?usage: consumer-test.sh <build-dir> [source-dir] [project]}"
SRC_DIR="${2:-$(cd "$(dirname "$0")/.." && pwd)}"
PROJECT="${3:-consumer}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PREFIX="$WORK/prefix"
cmake --install "$BUILD_DIR" --prefix "$PREFIX" > "$WORK/install.log" 2>&1 || {
    echo "install failed:"; tail -20 "$WORK/install.log"; exit 1; }

# A test framework or other build-time dependency in the prefix would be a
# packaging leak, so fail on one rather than shipping it.
if find "$PREFIX" -iname '*unity*' -print -quit | grep -q .; then
    echo "FAIL: the install leaked Unity into the consumer prefix"
    find "$PREFIX" -iname '*unity*' | head
    exit 1
fi

GEN_ARGS=()
if [ "$PROJECT" = "consumer_module" ]; then
    # Modules need Ninja, and the same compiler that produced the BMI.
    GEN_ARGS=(-G Ninja -DCMAKE_CXX_COMPILER="${CMAKE_CXX_COMPILER:-clang++}")
fi

cmake -S "$SRC_DIR/tests/$PROJECT" -B "$WORK/build" \
      "${GEN_ARGS[@]}" \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
      > "$WORK/configure.log" 2>&1 || {
    echo "consumer configure failed:"; tail -20 "$WORK/configure.log"; exit 1; }

cmake --build "$WORK/build" > "$WORK/build.log" 2>&1 || {
    echo "consumer build failed:"; tail -25 "$WORK/build.log"; exit 1; }

"$WORK/build/app"
