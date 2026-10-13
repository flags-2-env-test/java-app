#!/usr/bin/env bash
# Compiled guest must complete normally after rejected first root envelopes.
set -euo pipefail
if [[ $# != 3 ]]; then
  echo "usage: $0 <oreslang-llvmc> <clang> <source-root>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
source="$3"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$compiler" "$source/tests/fixtures/source-actor-pipeline-two-stage.ores" > "$tmp/guest.ll"
"$clang" -O1 -g -x ir "$tmp/guest.ll" -x c -std=c11 \
  -DORESLANG_SOURCE_ACTOR_TESTING -I "$source/src/main/cpp" \
  "$source/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c" \
  "$source/src/main/cpp/oreslang/runtime/MessagePackActorV2.c" \
  "$source/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
  "$source/src/main/cpp/oreslang/runtime/NativeActorFuture.c" \
  "$source/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c" \
  "$source/tests/native_pipeline_root_rejected_credit.c" -pthread -lm \
  -Wl,--wrap=oreslang_source_actor_pipeline_connect_many -o "$tmp/guest"
for n in $(seq 1 20); do
  set +e
  timeout 10 "$tmp/guest" > "$tmp/stdout" 2> "$tmp/stderr"
  exit_code=$?
  set -e
  if [[ "$exit_code" -ne 43 ]]; then
    cat "$tmp/stderr"
    echo "expected guest exit 43; received $exit_code" >&2
    exit 1
  fi
  grep -q 'SOURCE_ROOT_REJECT_CREDIT_PASS malformed=2 valid_roundtrip=1' "$tmp/stderr"
  grep -q 'SOURCE_PIPELINE_ROUNDTRIP_PASS actors=2 typed=1 forward=1 backward=1 result=24' "$tmp/stderr"
  if grep -q 'SOURCE_PIPELINE_FAILURE\|SOURCE_ACTOR_INVARIANT_FAILURE' "$tmp/stderr"; then
    cat "$tmp/stderr"
    exit 1
  fi
done
echo 'NATIVE_ROOT_REJECT_ADMISSION_LINEARIZATION_PASS cases=20'
