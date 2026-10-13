#!/usr/bin/env python3
"""Compile actual Oreslang LLVM actor source; hit real loopback HTTP sockets."""
import argparse
import concurrent.futures
import pathlib
import socket
import subprocess
import tempfile
import time

def request(port, method, path):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        sock.sendall((f"{method} {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n").encode("ascii"))
        stream = sock.makefile("rb")
        head = stream.readline().decode("ascii")
        if not head.startswith("HTTP/1.1 "):
            raise AssertionError(f"invalid HTTP status: {head!r}")
        status = int(head.split()[1])
        headers = {}
        while True:
            line = stream.readline()
            if line == b"\r\n": break
            if not line: raise AssertionError("truncated HTTP headers")
            name, value = line.decode("ascii").split(":", 1)
            headers[name.lower()] = value.strip()
        length = int(headers["content-length"])
        body = stream.read(length)
        if len(body) != length: raise AssertionError("truncated HTTP response body")
        return status, body.decode("ascii")

def main():
    p = argparse.ArgumentParser()
    for opt in ("compiler", "fixture", "clang", "actor_runtime", "arena_runtime", "native_runtime", "harness", "include"):
        p.add_argument("--" + opt.replace("_", "-"), required=True)
    cfg = p.parse_args()
    ir = subprocess.run([cfg.compiler, cfg.fixture], check=True, capture_output=True, text=True, timeout=20).stdout
    assert "oreslang_actor_Counter_add" in ir
    assert not any(s in ir for s in ("gc.statepoint", "gc.relocate", "@System.gc"))
    with tempfile.TemporaryDirectory(prefix="ores-llvm-http-") as scratch:
        exe = str(pathlib.Path(scratch) / "http-server")
        cmd = [cfg.clang, "-x", "ir", "-", "-x", "c", cfg.actor_runtime,
               cfg.arena_runtime, cfg.native_runtime, cfg.harness,
               "-std=c11", "-pthread", "-I", cfg.include, "-o", exe]
        built = subprocess.run(cmd, input=ir, capture_output=True, text=True, timeout=40)
        assert built.returncode == 0, built.stderr
        start = time.monotonic()
        process = subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            line = process.stdout.readline()
            assert line.startswith("PORT="), (line, process.poll())
            port = int(line.split("=", 1)[1])
            assert request(port, "GET", "/health") == (200, "42")
            assert request(port, "GET", "/missing") == (404, "not found")
            assert request(port, "POST", "/counter") == (405, "method not allowed")
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
                output = list(pool.map(lambda _: request(port, "GET", "/counter"), range(96)))
            assert all(status == 200 for status, body in output)
            pairs = [tuple(map(int, body.split(","))) for _, body in output]
            assert sorted(pairs) == [(n, 43*n) for n in range(1, 97)], sorted(pairs)[:8]
            assert request(port, "GET", "/health") == (200, "42")
            assert request(port, "GET", "/shutdown") == (200, "bye")
            status = process.wait(timeout=8)
            assert status == 0, (status, process.stderr.read())
            elapsed = time.monotonic() - start
            print(f"LLVM_HTTP_LOOPBACK_PASS requests=101 counter=96 concurrent_clients=12 seconds={elapsed:.3f} compiled_actor=yes native_arena=yes")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

if __name__ == "__main__":
    main()
