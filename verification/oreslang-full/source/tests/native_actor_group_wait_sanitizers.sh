#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
target="$(mktemp /tmp/oreslang-child-wait-asan.XXXXXX)"
trap 'rm -f "$target"' EXIT
clang -std=c11 -O1 -g -pthread -Wall -Wextra -Werror -Wpedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$root/src/main/cpp" \
  "$root/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
  "$root/tests/native_actor_group_wait_done_test.c" -o "$target"
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" "$target"
