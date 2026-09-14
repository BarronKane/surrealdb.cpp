#!/usr/bin/env bash
#
# Build and run a probe across every toolchain configuration this library
# claims to support.
#
# The point is that surrealdb.cpp is header-only and gets compiled with whatever
# flags its consumer already uses -- it has no build of its own from which to
# impose a standard. So "compiles at C++17/20/23" is not the claim. The claim is
# "compiles at whatever the consumer sets, from 17 up, with or without
# exceptions and RTTI", and that includes standards newer than the ones that
# existed when this was written.
#
# This overlaps with the per-standard sweep `ctest` now runs, and the division
# is deliberate: CMake covers standards with one toolchain, and this covers the
# axes a generator cannot reach -- the other standard library, and building with
# exceptions or RTTI switched off. Neither subsumes the other.
#
# Usage: scripts/compat-matrix.sh [path/to/probe.cpp]

set -uo pipefail
cd "$(dirname "$0")/.."

PROBE="${1:-tests/compat_probe.cpp}"
INC="src/surrealdb_cpp/include"
CINC="surrealdb.c/include"
LIB="$PWD/surrealdb.c/target/debug"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

STDS=(17 20 23 2b 2c)
declare -a MODE_NAMES=("default" "no-exceptions" "no-exceptions,no-rtti")
declare -a MODE_FLAGS=("" "-fno-exceptions" "-fno-exceptions -fno-rtti")

pass=0; fail=0
printf "%-8s %-11s %-24s %s\n" "std" "stdlib" "flags" "result"
printf -- "------------------------------------------------------------------\n"

for std in "${STDS[@]}"; do
  for tc in "g++::libstdc++" "clang++:-stdlib=libc++:libc++"; do
    CXX="${tc%%:*}"; rest="${tc#*:}"; SF="${rest%%:*}"; SL="${rest#*:}"
    command -v "$CXX" >/dev/null 2>&1 || continue
    for i in "${!MODE_FLAGS[@]}"; do
      # Arrays, not a string: an unquoted variable holding two flags is one
      # argument in zsh and two in bash, and getting that wrong silently
      # reports every configuration as broken.
      read -r -a mode <<< "${MODE_FLAGS[$i]}"
      sf=(); [ -n "$SF" ] && sf=("$SF")
      bin="$OUT/probe-$std-$SL-$i"

      if out=$("$CXX" -std="c++$std" "${sf[@]}" "${mode[@]}" -Wall -Wextra -O1 \
                 -I"$INC" -I"$CINC" "$PROBE" \
                 -L"$LIB" -Wl,-rpath,"$LIB" -lsurrealdb_c -lpthread -ldl -lm \
                 -o "$bin" 2>&1); then
        warns=$(printf '%s' "$out" | grep -c "warning:" || true)
        if timeout 120 "$bin" >/dev/null 2>&1; then
          [ "$warns" -eq 0 ] && r="ok" || r="ok ($warns warnings)"
          pass=$((pass+1))
        else
          r="RUN FAIL"; fail=$((fail+1))
        fi
      else
        r="BUILD FAIL"; fail=$((fail+1))
      fi

      printf "%-8s %-11s %-24s %s\n" "c++$std" "$SL" "${MODE_NAMES[$i]}" "$r"
      [ "$r" = "BUILD FAIL" ] && printf '%s\n' "$out" | grep -E "error" | head -3 | sed 's/^/     /'
    done
  done
done

printf -- "------------------------------------------------------------------\n"
printf "%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
