#!/usr/bin/env python3
"""CTest harness: verify safe rejection, verified IR, and optional native execution."""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile



def verify_three_actor_pipeline_stderr(stderr):
    """Validate bounded *observed* native result and physical STOP evidence.

    Do not use Python `assert` for an evidence security gate: -O disables
    assertions. This is still an aggregate witness, not a transition trace,
    runtime attestation, or compiler/runtime refinement proof.
    """
    if not isinstance(stderr, str) or len(stderr) > 131072:
        raise ValueError("invalid or oversized native evidence")
    lines = stderr.splitlines()
    marker = "SOURCE_PIPELINE_ROUNDTRIP_PASS actors=3 typed=1 forward=2 backward=2 result=19"
    if lines.count(marker) != 1:
        raise ValueError("missing or duplicated compiled roundtrip")
    if any(token in stderr for token in (
        "SOURCE_PIPELINE_FAILURE", "SOURCE_PIPELINE_TERMINAL_STATUS",
        "SOURCE_ACTOR_INVARIANT_FAILURE"
    )):
        raise ValueError("guest pipeline reported terminal failure")
    stop_prefix = "SOURCE_ACTOR_SPAWN_STOP_VERIFIED "
    expected = [
        stop_prefix + "constructor_on_worker=1 released=1 processed=1 last=13",
        stop_prefix + "constructor_on_worker=1 released=1 processed=2 last=14",
        stop_prefix + "constructor_on_worker=1 released=1 processed=2 last=19",
    ]
    indexed = [(i, line) for i, line in enumerate(lines)]
    stops = [(i, line) for i, line in indexed if line.startswith(stop_prefix)]
    if [line for _, line in stops] != expected:
        raise ValueError("physical actor finalizers/state mismatch")
    group = "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=3 peak=3 carriers=4"
    groups = [(i, line) for i, line in indexed
              if line.startswith("SOURCE_ACTOR_GROUP_VERIFIED")]
    if len(groups) != 1 or groups[0][1] != group:
        raise ValueError("final physical ActorGroup barrier missing or duplicated")
    marker_index = lines.index(marker)
    if not (marker_index < stops[0][0] < stops[1][0] < stops[2][0]
            < groups[0][0]):
        raise ValueError("completion and physical teardown event order invalid")



def verify_three_actor_flow_context(stderr):
    """Validate causal scheduler-issued DATA provenance on actual compiled guest.

    This checks the existing one-flow guest route, NOT concurrent-flow parity.
    Never depend on Python assertions: -O must not disable this security gate.
    """
    if not isinstance(stderr, str) or len(stderr) > 131072:
        raise ValueError("invalid or oversized host-issued context evidence")
    prefix = "SOURCE_PIPELINE_CONTEXT_ADMIT "
    rows = [line for line in stderr.splitlines() if line.startswith(prefix)]
    if len(rows) != 5:
        raise ValueError("expected five real scheduler DATA context commits")
    pattern = re.compile(
        r"SOURCE_PIPELINE_CONTEXT_ADMIT flow=(\d+) message=(\d+) "
        r"parent=(\d+) target=(\d+) direction=(\d+) "
        r"domain=(\d+) generation=(\d+)"
    )
    parsed = []
    for row in rows:
        match = pattern.fullmatch(row)
        if match is None:
            raise ValueError("malformed host-issued context record")
        parsed.append(tuple(map(int, match.groups())))
    flow_id = parsed[0][0]
    if flow_id <= 0:
        raise ValueError("zero flow identity")
    if [(r[3], r[4]) for r in parsed] != [
        (0, 0), (1, 1), (2, 1), (1, 2), (0, 2)
    ]:
        raise ValueError("compiled guest routed to an unauthorized target/direction")
    if any(r[0] != flow_id for r in parsed):
        raise ValueError("cross-flow DATA contamination")
    if len({r[1] for r in parsed}) != 5:
        raise ValueError("duplicate message identity")
    if any(r[1] <= 0 or r[5] <= 0 or r[6] <= 0 for r in parsed):
        raise ValueError("zero message, domain, or actor incarnation")
    if len({r[5] for r in parsed}) != 1:
        raise ValueError("cross-domain DATA route")
    if parsed[0][2] != 0:
        raise ValueError("root ingress forged its causal parent")
    if any(parsed[i][2] != parsed[i - 1][1] for i in range(1, 5)):
        raise ValueError("broken runtime-owned causal chain")


def require(condition, message):
    if not condition:
        raise ValueError(str(message))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--ir")
    parser.add_argument("--ir-not", action="append",
                        help="Forbidden LLVM IR regex (checked even with python -O)")
    parser.add_argument("--diagnostic")
    parser.add_argument("--diagnostic-not", action="append",
                        help="Forbidden compiler diagnostic text in negative tests")
    parser.add_argument("--run", type=int)
    parser.add_argument("--clang")
    parser.add_argument("--native-runtime")
    parser.add_argument("--actor-runtime")
    parser.add_argument("--native-harness")
    parser.add_argument("--scheduler-runtime")
    parser.add_argument("--source-actor-runtime")
    parser.add_argument("--stderr")
    parser.add_argument("--stderr-count", type=int)
    parser.add_argument("--pipeline-three-actor-summary", action="store_true")
    parser.add_argument("--pipeline-context-summary", action="store_true")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if args.pipeline_three_actor_summary:
        if (args.diagnostic or args.run != 43 or not args.clang
                or not args.source_actor_runtime
                or pathlib.Path(args.fixture).name != "source-actor-pipeline-bidirectional.ores"):
            raise ValueError("three-actor gate requires canonical compiled guest execution")
    compile_result = subprocess.run(
        [args.compiler, args.fixture], capture_output=True, text=True, check=False, timeout=20
    )
    require(not args.diagnostic_not or args.diagnostic,
            "--diagnostic-not requires --diagnostic")
    if args.diagnostic:
        require(compile_result.returncode != 0, "invalid source unexpectedly accepted")
        require(args.diagnostic in compile_result.stderr, compile_result.stderr)
        for forbidden in args.diagnostic_not or ():
            require(forbidden not in compile_result.stderr,
                    "unexpected compiler rejection reason: " + forbidden)
        require(not compile_result.stdout.strip(), "emitted IR after rejection")
        return
    require(compile_result.returncode == 0, compile_result.stderr)
    require(compile_result.stdout.startswith("; ModuleID"), compile_result.stdout)
    require(not compile_result.stderr, compile_result.stderr)
    if args.ir:
        require(args.ir in compile_result.stdout, compile_result.stdout)
    for forbidden in args.ir_not or ():
        require(re.search(forbidden, compile_result.stdout) is None,
                "forbidden LLVM IR evidence: " + forbidden)
    if args.run is None:
        return
    require(args.clang, "--run requires a clang executable")
    with tempfile.TemporaryDirectory(prefix="oreslang-llvm-test-") as directory:
        executable = str(pathlib.Path(directory) / "guest")
        clang_args = [args.clang, "-x", "ir", "-"]
        if args.sanitize:
            clang_args.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        if args.native_runtime:
            native_source = pathlib.Path(args.native_runtime)
            clang_args.extend([
                "-x", "c", str(native_source),
                "-std=c11", "-I", str(native_source.parents[2]),
            ])
        if args.actor_runtime:
            path = pathlib.Path(args.actor_runtime)
            clang_args.extend([
                "-x", "c", str(path), "-std=c11",
                "-I", str(path.parents[2]),
            ])
        if args.scheduler_runtime:
            path = pathlib.Path(args.scheduler_runtime)
            clang_args.extend(["-x", "c", str(path), "-std=c11",
                               "-I", str(path.parents[2]), "-pthread"])
        if args.source_actor_runtime:
            path = pathlib.Path(args.source_actor_runtime)
            native_future = path.parent / "NativeActorFuture.c"
            native_messagepack = path.parent / "MessagePackActorV2.c"
            clang_args.extend(["-x", "c", str(path), str(native_future),
                               str(native_messagepack), "-std=c11",
                               "-DORESLANG_SOURCE_ACTOR_TESTING",
                               "-I", str(path.parents[2]), "-pthread"])
        if args.native_harness:
            harness = pathlib.Path(args.native_harness)
            # The actor source module may contain no top-level main;
            # the trusted test harness supplies native actor turns.
            clang_args.extend(["-x", "c", str(harness), "-std=c11"])
        clang_args.extend(["-o", executable])
        native = subprocess.run(
            clang_args, input=compile_result.stdout,
            capture_output=True, text=True, check=False, timeout=30
        )
        require(native.returncode == 0, native.stderr)
        require(1 <= args.runs <= 100, "unreasonable repeat count")
        for trial in range(args.runs):
            result = subprocess.run(
                [executable], capture_output=True, text=True, check=False, timeout=10
            )
            # The native platform truncates main's i64 return to its exit code.
            require(result.returncode == args.run, (
                f"trial {trial}: native exit {result.returncode}, "
                f"expected {args.run}: {result.stderr}"
            ))
            if args.stderr:
                require(args.stderr in result.stderr, result.stderr)
            if args.stderr_count is not None:
                require(args.stderr, "--stderr-count needs --stderr")
                require(result.stderr.count(args.stderr) == args.stderr_count, (
                    f"trial {trial}: {result.stderr}"
                ))
            if args.pipeline_three_actor_summary:
                if result.returncode != 43:
                    raise ValueError("compiled three-actor guest returned wrong exit status")
                if result.stdout:
                    raise ValueError("unexpected compiled actor guest stdout")
                verify_three_actor_pipeline_stderr(result.stderr)
            if args.pipeline_context_summary:
                if not args.pipeline_three_actor_summary:
                    raise ValueError("context gate requires full physical guest transcript gate")
                verify_three_actor_flow_context(result.stderr)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f"contract test failed: {error}", file=sys.stderr)
        sys.exit(1)
