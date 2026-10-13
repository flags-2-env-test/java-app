#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
compiler="${1:-clang}"
binary="$(mktemp /tmp/oreslang-actor-ready-asan.XXXXXX)"
trap 'rm -f "$binary"' EXIT
"$compiler" -std=c11 -O1 -g -pthread \
  -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo/src/main/cpp" \
  "$repo/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
  "$repo/tests/native_actor_scheduler_test.c" \
  -o "$binary"
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
  "$binary"
