#!/usr/bin/env python3
"""Check traces produced by an executing native HungryCarrier probe.

Requires a real compiled native executable; it fails closed on missing
toolchain, nonzero exits, malformed JSON, invalid lifecycle or wrong IDs.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from validate import check_trace

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=8)
    parser.add_argument("--timeout", type=int, default=12)
    args = parser.parse_args()
    if args.repeat < 1 or args.repeat > 100:
        parser.error("repeat must be 1..100")
    if args.timeout < 1 or args.timeout > 300:
        parser.error("timeout must be 1..300")
    for iteration in range(args.repeat):
        proc = subprocess.run([str(args.binary.resolve()), "--trace"],
                              capture_output=True, text=True,
                              check=False, timeout=args.timeout)
        if proc.returncode != 0 or proc.stderr:
            raise RuntimeError(f"native runtime failed at iteration {iteration}: "
                               f"code={proc.returncode} stderr={proc.stderr[:300]!r}")
        trace = json.loads(proc.stdout)
        metrics = check_trace(trace)
        events = trace["events"]
        admissions = [e["message_id"] for e in events if e["kind"] == "admit"]
        receives = [e["message_id"] for e in events if e["kind"] == "receive"]
        expected = [f"m{i}" for i in range(1, 25)]
        if admissions != expected or receives != expected:
            raise ValueError(f"wrong observed native IDs: {admissions}, {receives}")
        if metrics["actors"] != 1 or metrics["messages_received"] != 24 or len(events) != 50:
            raise ValueError(f"unexpected real carrier event counts: {metrics}")
    print(f"NATIVE_ACTOR_TRACE passed={args.repeat}/{args.repeat} messages={24*args.repeat}")
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(f"NATIVE_ACTOR_TRACE FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
