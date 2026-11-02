#!/usr/bin/env python3
"""Automated correctness gate for the four Network I/O file-server variants.

Run from the repository root, for example:
  python3 code/tests/filesrv_gate.py \
    --select ./build/filesrv-select \
    --poll ./build/filesrv-poll \
    --epoll ./build/filesrv-epoll \
    --uring ./build/filesrv-uring

The suite starts and stops each server itself. It uses the repository's files/
fixtures, picks an available localhost port, and never modifies project files.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

ROOT = Path.cwd()
FIXTURES = {
    "/hello.txt": b"Hello from the CN file server!\n",
    "/second.txt": b"This is a second test file.\n",
    "/empty.txt": b"",
}


@dataclass
class Response:
    status: int
    headers: dict[str, str]
    body: bytes


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def recv_until(sock: socket.socket, marker: bytes, limit: int = 65536) -> bytes:
    data = bytearray()
    while marker not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data.extend(chunk)
        if len(data) > limit:
            raise AssertionError(f"response headers exceeded {limit} bytes")
    return bytes(data)


def read_response(sock: socket.socket) -> Response:
    raw = recv_until(sock, b"\r\n\r\n")
    if not raw:
        raise AssertionError("connection closed without an HTTP response")
    head, sep, remainder = raw.partition(b"\r\n\r\n")
    if not sep:
        raise AssertionError(f"incomplete HTTP response headers: {raw!r}")
    lines = head.split(b"\r\n")
    try:
        parts = lines[0].decode("latin-1").split(" ", 2)
        if len(parts) < 2 or not parts[0].startswith("HTTP/"):
            raise ValueError("bad status line")
        status = int(parts[1])
    except (ValueError, UnicodeDecodeError) as exc:
        raise AssertionError(f"invalid HTTP status line: {lines[0]!r}") from exc
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if b":" not in line:
            raise AssertionError(f"malformed response header: {line!r}")
        k, v = line.split(b":", 1)
        headers[k.decode("latin-1").strip().lower()] = v.decode("latin-1").strip()
    try:
        length = int(headers.get("content-length", "0"))
    except ValueError as exc:
        raise AssertionError("invalid Content-Length") from exc
    body = bytearray(remainder)
    while len(body) < length:
        chunk = sock.recv(min(65536, length - len(body)))
        if not chunk:
            raise AssertionError(f"body truncated: expected {length}, received {len(body)}")
        body.extend(chunk)
    if len(body) > length:
        # This suite issues one request at a time, so extra bytes imply response framing trouble.
        raise AssertionError(f"received {len(body)} body bytes, Content-Length says {length}")
    return Response(status, headers, bytes(body))


def request(port: int, path: str, method: str = "GET", timeout: float = 3.0) -> Response:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.settimeout(timeout)
        payload = f"{method} {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n".encode("ascii")
        s.sendall(payload)
        return read_response(s)


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def wait_ready(proc: subprocess.Popen, port: int, log_path: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            log = log_path.read_text(errors="replace") if log_path.exists() else ""
            raise RuntimeError(f"server exited early with code {proc.returncode}\n{log}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    raise RuntimeError(f"server did not listen within {timeout}s; last error={last_error}\n{log}")


def run_case(name: str, fn: Callable[[], None], results: list[tuple[str, bool, str]]) -> None:
    try:
        fn()
        results.append((name, True, ""))
    except Exception as exc:  # keep running so one failure doesn't hide later failures
        results.append((name, False, f"{type(exc).__name__}: {exc}"))


def test_server(label: str, binary: Path, args: argparse.Namespace) -> tuple[int, int]:
    if not binary.is_file():
        print(f"\n[{label}] SKIP/ERROR: executable not found: {binary}")
        return 0, 1
    port = reserve_port()
    results: list[tuple[str, bool, str]] = []
    with tempfile.TemporaryDirectory(prefix=f"filesrv-{label}-") as temp_dir:
        temp = Path(temp_dir)
        log_path = temp / "server.log"
        with log_path.open("w+") as log:
            command = [str(binary.resolve()), "--port", str(port)]
            try:
                proc = subprocess.Popen(
                    command,
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
            except Exception as exc:
                print(f"\n[{label}] FAIL to launch: {exc}")
                return 0, 1
            try:
                wait_ready(proc, port, log_path)

                def valid_files() -> None:
                    for path, expected in FIXTURES.items():
                        response = request(port, path)
                        check(response.status == 200, f"{path}: expected 200, got {response.status}")
                        check(response.body == expected, f"{path}: body mismatch {response.body!r}")
                        check(int(response.headers.get("content-length", "-1")) == len(expected),
                              f"{path}: incorrect Content-Length")

                run_case("valid + empty files and Content-Length", valid_files, results)

                def missing_file() -> None:
                    response = request(port, "/this-file-should-not-exist-7f3a.txt")
                    check(response.status == 404, f"expected 404, got {response.status}")
                    check(response.body == b"", f"expected empty 404 body, got {response.body!r}")

                run_case("missing file -> 404", missing_file, results)

                def unsupported_methods() -> None:
                    for method in ("POST", "DELETE"):
                        response = request(port, "/hello.txt", method)
                        check(response.status == 405, f"{method}: expected 405, got {response.status}")

                run_case("unsupported methods -> 405", unsupported_methods, results)

                def traversal() -> None:
                    for path in ("/../README.md", "/../../etc/passwd", "/..%2f..%2fetc%2fpasswd"):
                        response = request(port, path)
                        check(response.status != 200, f"unsafe path unexpectedly returned 200: {path}")
                        check(b"root:" not in response.body, f"possible /etc/passwd disclosure for {path}")

                run_case("path traversal not served", traversal, results)

                def malformed_request() -> None:
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as s:
                        s.settimeout(3)
                        s.sendall(b"THIS IS NOT A VALID HTTP REQUEST\r\n\r\n")
                        data = s.recv(4096)
                        check(data.startswith(b"HTTP/1.1 400") or data == b"",
                              f"expected 400 or safe close, got {data!r}")

                run_case("malformed request -> 400 or close", malformed_request, results)

                def incomplete_request() -> None:
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as s:
                        s.settimeout(3)
                        s.sendall(b"GET /hello.txt HTTP/1.1\r\nHost:")
                        s.shutdown(socket.SHUT_WR)
                        data = s.recv(4096)
                        check(data == b"" or not data.startswith(b"HTTP/1.1 200"),
                              f"incomplete request got unexpected response {data!r}")

                run_case("incomplete request closed safely", incomplete_request, results)

                def persistent_connection() -> None:
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as s:
                        s.settimeout(3)
                        for path, expected in (("/hello.txt", FIXTURES["/hello.txt"]),
                                               ("/second.txt", FIXTURES["/second.txt"])):
                            s.sendall(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
                            response = read_response(s)
                            check(response.status == 200, f"{path}: got {response.status}")
                            check(response.body == expected, f"{path}: incorrect body")
                            check(response.headers.get("connection", "keep-alive").lower() != "close",
                                  f"server closed persistent connection after {path}")

                run_case("two requests on one persistent connection", persistent_connection, results)

                def concurrency() -> None:
                    workers = args.workers
                    per_worker = args.requests_per_worker
                    jobs = workers * per_worker
                    paths = list(FIXTURES.items())

                    def one(i: int) -> str | None:
                        path, expected = paths[i % len(paths)]
                        try:
                            response = request(port, path, timeout=args.request_timeout)
                            if response.status != 200 or response.body != expected:
                                return f"{path}: status={response.status}, body={response.body!r}"
                            return None
                        except Exception as exc:
                            return f"{path}: {type(exc).__name__}: {exc}"

                    failures: list[str] = []
                    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
                        for error in pool.map(one, range(jobs)):
                            if error:
                                failures.append(error)
                    check(not failures,
                          f"{jobs} requests with {workers} workers: {len(failures)} failed; "
                          + "; ".join(failures[:5]))

                run_case(f"concurrency ({args.workers} workers x {args.requests_per_worker} = "
                         f"{args.workers * args.requests_per_worker} requests)", concurrency, results)

                def still_alive() -> None:
                    check(proc.poll() is None, f"server exited with code {proc.returncode}")
                    response = request(port, "/hello.txt")
                    check(response.status == 200 and response.body == FIXTURES["/hello.txt"],
                          "valid request after edge/concurrency tests failed")

                run_case("server survives tests and serves afterward", still_alive, results)
            except Exception as exc:
                results.append(("startup", False, f"{type(exc).__name__}: {exc}"))
            finally:
                if proc.poll() is None:
                    try:
                        os.killpg(proc.pid, signal.SIGINT)
                        proc.wait(timeout=2)
                    except (ProcessLookupError, subprocess.TimeoutExpired):
                        try:
                            os.killpg(proc.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        try:
                            proc.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            pass

        # Print server log only when there is a failure, to aid diagnosis.
        failed = [r for r in results if not r[1]]
        print(f"\n[{label}] {len(results) - len(failed)}/{len(results)} checks passed")
        for name, ok, detail in results:
            print(f"  {'PASS' if ok else 'FAIL'}  {name}")
            if detail:
                print(f"        {detail}")
        if failed and log_path.exists():
            log_text = log_path.read_text(errors="replace")
            if log_text.strip():
                print("  --- server log ---")
                print("\n".join("  " + line for line in log_text.splitlines()[-40:]))
        return len(results) - len(failed), len(failed)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--select", type=Path, required=True, help="path to filesrv-select")
    parser.add_argument("--poll", type=Path, required=True, help="path to filesrv-poll")
    parser.add_argument("--epoll", type=Path, required=True, help="path to filesrv-epoll")
    parser.add_argument("--uring", type=Path, required=True, help="path to filesrv-uring")
    parser.add_argument("--workers", type=int, default=50, help="concurrent client workers (default: 50)")
    parser.add_argument("--requests-per-worker", type=int, default=20,
                        help="requests per worker (default: 20; 50 x 20 = 1000 per backend)")
    parser.add_argument("--request-timeout", type=float, default=5.0,
                        help="per-request socket timeout in seconds (default: 5)")
    args = parser.parse_args()
    if args.workers < 1 or args.requests_per_worker < 1 or args.request_timeout <= 0:
        parser.error("workers, requests-per-worker, and request-timeout must be positive")

    print(f"Repository root (working directory): {ROOT}")
    print(f"Fixtures directory: {ROOT / 'files'}")
    if not (ROOT / "files").is_dir():
        print("ERROR: run this script from the repository root (the files/ directory was not found).", file=sys.stderr)
        return 2

    totals = [0, 0]
    for label, binary in (("select", args.select), ("poll", args.poll),
                          ("epoll", args.epoll), ("io_uring", args.uring)):
        passed, failed = test_server(label, binary, args)
        totals[0] += passed
        totals[1] += failed

    print("\n" + "=" * 60)
    print(f"TOTAL: {totals[0]} checks passed; {totals[1]} checks failed")
    if totals[1]:
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS — all four file-server backends passed this suite")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
