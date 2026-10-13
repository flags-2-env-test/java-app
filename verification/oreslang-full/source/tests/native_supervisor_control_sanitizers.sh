#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
compiler="${1:-clang}"
binary="$(mktemp /tmp/oreslang-supervisor-asan.XXXXXX)"
trap 'rm -f "$binary"' EXIT
"$compiler" -std=c11 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$root/src/main/cpp" \
  "$root/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
  "$root/src/main/cpp/oreslang/runtime/NativeActorFuture.c" \
  "$root/tests/native_supervisor_control_test.c" -o "$binary"
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" "$binary"
