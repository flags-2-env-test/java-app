#!/usr/bin/env bash
# A native compiler-generated Source->Transform->Sink guest tries a forward
# hop with its host TLS parent corrupted before publication. Rejection must
# preserve the already-admitted root and physically stop all actors.
set -euo pipefail
if [[ $# -ne 3 ]]; then
  echo "usage: $0 <compiler> <clang> <source-root>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
source="$3"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$compiler" "$source/tests/fixtures/source-actor-pipeline-bidirectional.ores" > "$tmp/guest.ll"
"$clang" -O1 -g -x ir "$tmp/guest.ll" -x c -std=c11 \
  -D_POSIX_C_SOURCE=200809L -DORESLANG_SOURCE_ACTOR_TESTING \
  -I "$source/src/main/cpp" \
  "$source/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c" \
  "$source/src/main/cpp/oreslang/runtime/MessagePackActorV2.c" \
  "$source/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
  "$source/src/main/cpp/oreslang/runtime/NativeActorFuture.c" \
  "$source/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c" \
  "$source/tests/native_pipeline_forged_causal_parent.c" -pthread -lm \
  -o "$tmp/guest"
for n in $(seq 1 20); do
  set +e
  timeout 10 "$tmp/guest" > "$tmp/stdout" 2> "$tmp/stderr"
  code=$?
  set -e
  if [[ "$code" -ne 73 ]]; then
    cat "$tmp/stderr"
    echo "expected recoverable rejected DATA admission status 73, received $code" >&2
    exit 1
  fi
  grep -q '^SOURCE_CAUSAL_FORGED_PARENT_INJECTED count=1$' "$tmp/stderr"
  grep -q '^SOURCE_PIPELINE_TERMINAL_STATUS code=3$' "$tmp/stderr"
  grep -q '^SOURCE_PIPELINE_CONTEXT_ADMIT flow=[1-9][0-9]* message=[1-9][0-9]* parent=0 target=0 direction=0' "$tmp/stderr"
  if [[ "$(grep -c '^SOURCE_PIPELINE_CONTEXT_ADMIT ' "$tmp/stderr")" -ne 1 ]]; then
    cat "$tmp/stderr"
    echo 'a forged descendant was admitted into the scheduler' >&2
    exit 1
  fi
  grep -q '^SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=3 peak=3 carriers=4$' "$tmp/stderr"
  if grep -q 'SOURCE_ACTOR_INVARIANT_FAILURE\|SOURCE_PIPELINE_ROUNDTRIP_PASS' "$tmp/stderr"; then
    cat "$tmp/stderr"
    exit 1
  fi
done
echo 'SOURCE_COMPILED_CAUSAL_PARENT_FENCING_PASS attempts=20 admitted_roots=20 rejected_descendants=20'
