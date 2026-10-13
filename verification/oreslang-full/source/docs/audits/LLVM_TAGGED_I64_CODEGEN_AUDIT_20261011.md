# LLVM typed actor signed-i64 MessagePack audit — October 11, 2026

Status: DRAFT. Exact-source guest-compiler test is green; unrestricted typed
source actor runtime parity remains unproven.

## Audit finding and fix

The first executable LLVM nominal MessagePack actor subset accepted only
nonnegative int literals no larger than signed 32-bit, despite declaring the
payload as Oreslang int and advertising an I (signed-i64) MessagePack shape.
That mismatched the Java/GraalVM MessagePackCollectionCodec.signedInt(long)
encoder and the native decoder's signed-integer grammar.

- Parser.cpp now admits a single compile-time signed i64 literal (positive
  Integer or Negate(Integer)) for tagged enum int payloads. It continues to
  reject arbitrary runtime expressions until general typed expression lowering.
  The negative 2^63 literal is recognized and represented as INT64_MIN without
  parsing the positive out-of-range magnitude into signed storage.
- LlvmCodegen.cpp now separates the enum *tag* (positive signed 32-bit) from
  its i64 payload, emitting the smallest valid canonical MessagePack integer
  width for positive and negative values: fixint, uint8/16/32/64,
  negative fixint and int8/16/32/64. It does not downcast data to a nominal tag.
- Source receive and nominal identity enforcement remain unchanged: the
  owning schema, owner and tag are verified before a guest callback, with
  one actor mailbox, one capacity budget and one serialized execution lane.

## Executed exact-source native guest test

Funded GitHub Actions CI, **PASSED all job steps**:
https://github.com/zed-pkg-test/java-app/actions/runs/38198869667

The funded test organization mirrors exact Git blobs of the source compiler,
parser header, runtime, and fixtures. Its full native job compiled the LLVM
compiler, generated LLVM IR from .ores source, compiled that IR with the C
runtime, and ran the resulting executable, instead of manually invoking a
host C actor callback.

New fixture:
tests/fixtures/source-tagged-int64-canonical-boundaries.ores

The fixture delivered nine source-compiled nominal messages with two distinct
case tags sharing the same integer shape, including -1, -33, -129 (used as
Sub(-129)), -32769, 2147483648, 4294967296 (used as Sub(...)),
-2147483649, INT64_MAX and INT64_MIN. The scheduler delivered all nine turns
and reported exactly -4294999972 as the final actor state.

Additional gates passed:
- repeat guest execution 20 times with expected final state;
- signed underflow, positive overflow and nonliteral payload compile rejection
  with no leaked partial IR;
- ASan+UBSan compiled guest execution repeated eight times;
- legacy typed integer and payloadless nominal guest execution;
- updated native parser type/binder diagnostics with -Werror, plus the
  existing typed-int sanitizer run;
- exact source SHA verification at the funded test runner.

Two intermediate failures were resolved: a mixed old Parser.hpp source mirror
could not compile the new pipeline-bearing compiler, and an outdated parser
test expected the prior fail-closed diagnostic. A test-oracle arithmetic error
(Sub(-129) adds 129) was independently corrected and all tests rerun.

CMakeLists.txt now registers the new fixture in native CTest and sanitizer
CTest so it is exercised by future full LLVM CI.

## Remaining blockers

1. Only payloadless or single int literal enum payloads run end to end today.
   String, tuples, arrays, arbitrary typed expressions, nested matches and
   typed reply futures are not yet proven to execute in compiled LLVM source.
2. This test compared encoder width decisions to Java's
   MessagePackCollectionCodec.signedInt(long) implementation; it did not
   execute an independent byte-for-byte Java/LLVM test at every i64 boundary.
3. Native emitter limits closed actor variant tables to 64 while Java allows
   up to 4096. Portable domain-limit alignment and indexed lookup remain.
4. Opaque actor pointers still require external producer fencing to avoid
   stale-handle access after destruction; generation-checked public handles,
   cancellation, and Future<T> backpressure are separate release blockers.
5. This funded run is a targeted exact-source guest-compiler job, not full
   LLVM CMake/CTest or full Java/Graal native-image CI. Do not merge this
   PR solely from this gate.

Do not merge PR #180 until missing compiler and runtime release gates pass.
