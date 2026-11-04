#!/usr/bin/env python3
"""Check that all four file-server backends serve the benchmark fixture intact.

Run from the repository root after `make bench-fixture`, for example:
  python3 code/tests/File-Transfer-Server/filesrv_bench_check.py \
    --select ./build/filesrv-select --poll ./build/filesrv-poll \
    --epoll ./build/filesrv-epoll --uring ./build/filesrv-uring

For each backend this compares Content-Length and SHA-256 against the file on
disk for: one Connection: close request, three requests on one keep-alive
connection (the way wrk uses connections), and --connections concurrent
keep-alive connections with two requests each. It reuses the helpers of
filesrv_gate.py and leaves that suite unchanged.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import os
import signal
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from filesrv_gate import ROOT, check, read_response, reserve_port, wait_ready  # noqa: E402


def fetch(port: int, path: str, count: int, keep_alive: bool, timeout: float) -> list[tuple[int, int, str]]:
    """Issue count sequential GETs on one connection; return (status, length, sha256)."""
    results = []
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.settimeout(timeout)
        for i in range(count):
            last = i == count - 1
            connection = "" if keep_alive or not last else "Connection: close\r\n"
            s.sendall(f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n{connection}\r\n".encode("ascii"))
            response = read_response(s)
            check(int(response.headers.get("content-length", "-1")) == len(response.body),
                  f"{path}: Content-Length does not match the body")
            results.append((response.status, len(response.body), hashlib.sha256(response.body).hexdigest()))
    return results


def check_backend(label: str, binary: Path, path: str, size: int, digest: str,
                  connections: int, timeout: float) -> bool:
    if not binary.is_file():
        print(f"[{label}] FAIL: executable not found: {binary}")
        return False
    port = reserve_port()
    with tempfile.TemporaryDirectory(prefix=f"filesrv-bench-{label}-") as temp_dir:
        log_path = Path(temp_dir) / "server.log"
        with log_path.open("w+") as log:
            proc = subprocess.Popen([str(binary.resolve()), "--port", str(port)], cwd=ROOT,
                                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                wait_ready(proc, port, log_path)
                expected = (200, size, digest)
                runs = [fetch(port, path, 1, False, timeout), fetch(port, path, 3, True, timeout)]
                with concurrent.futures.ThreadPoolExecutor(max_workers=connections) as pool:
                    runs += list(pool.map(lambda _: fetch(port, path, 2, True, timeout), range(connections)))
                responses = [r for run in runs for r in run]
                bad = [r for r in responses if r != expected]
                check(not bad, f"{len(bad)}/{len(responses)} responses differ from the fixture, e.g. {bad[:2]}")
                check(proc.poll() is None, f"server exited with code {proc.returncode}")
                print(f"[{label}] PASS: {len(responses)} responses of {size} bytes, sha256 matches")
                return True
            except Exception as exc:  # report every backend, not only the first failure
                print(f"[{label}] FAIL: {type(exc).__name__}: {exc}")
                print("\n".join("  " + line for line in log_path.read_text(errors="replace").splitlines()[-20:]))
                return False
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGINT)
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--select", type=Path, required=True)
    parser.add_argument("--poll", type=Path, required=True)
    parser.add_argument("--epoll", type=Path, required=True)
    parser.add_argument("--uring", type=Path, required=True)
    parser.add_argument("--file", default="bench-4MiB.bin", help="fixture name under the file-server fixture directory")
    parser.add_argument("--connections", type=int, default=32)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    fixture = ROOT / "code" / "apps" / "File-Transfer-Server" / "files" / args.file
    if not fixture.is_file():
        print(f"ERROR: {fixture} not found; run `make bench-fixture` from the repository root.", file=sys.stderr)
        return 2
    data = fixture.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    print(f"Fixture: {fixture} ({len(data)} bytes, sha256 {digest})")

    ok = True
    for label, binary in (("select", args.select), ("poll", args.poll),
                          ("epoll", args.epoll), ("io_uring", args.uring)):
        ok &= check_backend(label, binary, "/" + args.file, len(data), digest,
                            args.connections, args.timeout)
    print("RESULT: PASS" if ok else "RESULT: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
