#!/usr/bin/env python3
"""Correctness gate for the echo app (design plan sections 7 and 13).

Assumptions / unresolved setup:
- The plan asks for ``make test``, but this checkout has no Makefile or build
  targets. Run this script directly and pass one already-built echo executable
  for each mechanism with --select/--poll/--epoll/--uring.
- T12 needs ASan+UBSan builds. Pass them with --sanitized; the same deterministic
  T1-T11 checks are run against each supplied sanitizer binary.
- The server accepts --port, --max-conns, --msg-size, --stats-file and
  --buf-size as declared by core_config_t. The test uses a loopback ephemeral
  port, 512-byte frames and a 4 KiB app buffer unless overridden.
- T4's memory condition is checked as stable VmRSS across the five-second
  backpressure hold. RSS can be affected by unrelated process activity; this
  is a practical check, not a kernel-level allocation bound.
- T4 compares the complete-frame prefix: a final partial frame is deliberately
  not expected back because the echo owner discards incomplete frames on close.
- T8 treats an immediate close/reset or a failed connect for the extra client
  as a clean refusal because TCP may complete the handshake before the server
  rejects a connection at its application limit.
"""

from __future__ import annotations

import argparse
import errno
import os
import random
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable

MECHANISMS = ("select", "poll", "epoll", "uring")
FRAME_SIZE = 512
BUF_SIZE = 4096
TIMEOUT = 10.0


def recv_exact(sock: socket.socket, size: int, timeout: float = TIMEOUT) -> bytes:
    sock.settimeout(timeout)
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise AssertionError(f"connection closed after {len(result)} of {size} bytes")
        result.extend(chunk)
    return bytes(result)


def recv_until_eof(sock: socket.socket, timeout: float = TIMEOUT) -> bytes:
    sock.settimeout(timeout)
    result = bytearray()
    while True:
        part = sock.recv(65536)
        if not part:
            return bytes(result)
        result.extend(part)


def connect(port: int) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return sock


def frame_bytes(count: int, seed: int) -> bytes:
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(count * FRAME_SIZE))


def wait_listening(port: int, proc: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 8.0
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError(f"server exited during startup with status {proc.returncode}")
        try:
            with connect(port):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise AssertionError(f"server did not listen on port {port}: {last_error}")


def rss_bytes(pid: int) -> int | None:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    except (OSError, ValueError):
        return None
    return None


def fd_count(pid: int) -> int | None:
    try:
        return len(list(Path(f"/proc/{pid}/fd").iterdir()))
    except OSError:
        return None


def run_cases(port: int, case_names: tuple[str, ...]) -> None:
    # T1: split one frame; no response before the second half arrives.
    if "T1" in case_names:
        data = frame_bytes(1, 1)
        with connect(port) as s:
            s.sendall(data[: FRAME_SIZE // 2])
            s.settimeout(0.01)
            try:
                early = s.recv(1)
                if early:
                    raise AssertionError("T1: reply arrived before the full frame")
            except socket.timeout:
                pass
            s.settimeout(TIMEOUT)
            s.sendall(data[FRAME_SIZE // 2 :])
            if recv_exact(s, len(data)) != data:
                raise AssertionError("T1: reply differs from input")

    # T2: three frames in one write, exact order and bytes.
    if "T2" in case_names:
        data = frame_bytes(3, 2)
        with connect(port) as s:
            s.sendall(data)
            if recv_exact(s, len(data)) != data:
                raise AssertionError("T2: coalesced replies differ or are out of order")

    # T3: one byte per send, preserving a single TCP stream.
    if "T3" in case_names:
        data = frame_bytes(1, 3)
        with connect(port) as s:
            for byte in data:
                s.sendall(bytes((byte,)))
            if recv_exact(s, len(data)) != data:
                raise AssertionError("T3: byte-at-a-time reply differs")

    # T4: fill the client send path until its nonblocking send returns EAGAIN,
    # hold without reading for five seconds, then verify every accepted byte.
    if "T4" in case_names:
        with connect(port) as s:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
            s.setblocking(False)
            seed = 40
            rng = random.Random(seed)
            sent = bytearray()
            block = bytearray(FRAME_SIZE * 32)
            blocked = False
            while len(sent) < 128 * 1024 * 1024:
                for i in range(len(block)):
                    block[i] = rng.randrange(256)
                # Keep sends frame aligned so the server has no partial tail.
                limit = len(block) - len(block) % FRAME_SIZE
                try:
                    n = s.send(memoryview(block)[:limit])
                    if n > 0:
                        sent.extend(block[:n])
                except BlockingIOError as exc:
                    if exc.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                        raise
                    blocked = True
                    break
            if not blocked:
                raise AssertionError("T4: client did not reach EAGAIN before the 128 MiB safety cap")
            complete_len = len(sent) - (len(sent) % FRAME_SIZE)
            if complete_len == 0:
                raise AssertionError("T4: client could not enqueue any complete frame")
            s.setblocking(True)
            before = rss_bytes(server_pid)
            time.sleep(5.0)
            after = rss_bytes(server_pid)
            if before is not None and after is not None and before != after:
                raise AssertionError(f"T4: server VmRSS changed during stall ({before} -> {after})")
            expected = bytes(sent[:complete_len])
            echoed = recv_exact(s, complete_len, timeout=max(TIMEOUT, complete_len / 65536 + 10))
            if echoed != expected:
                raise AssertionError("T4: backpressure echo differs from bytes accepted by the client")

    # T5/T6: EOF and RST partway through a frame; server must remain alive.
    if "T5" in case_names:
        with connect(port) as s:
            s.sendall(b"x" * (FRAME_SIZE // 3))
        time.sleep(0.02)
        if server_proc.poll() is not None:
            raise AssertionError("T5: server exited after client close mid-frame")

    if "T6" in case_names:
        s = connect(port)
        s.sendall(b"r" * (FRAME_SIZE // 3))
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()
        time.sleep(0.02)
        if server_proc.poll() is not None:
            raise AssertionError("T6: server exited after reset mid-frame")

    # T7: 10,000 connect/use/close cycles; process fd count must return to its
    # post-start baseline (within one descriptor for transient bookkeeping).
    if "T7" in case_names:
        baseline = fd_count(server_pid)
        baseline_rss = rss_bytes(server_pid)
        payload = frame_bytes(1, 7)
        for _ in range(10_000):
            with connect(port) as s:
                s.sendall(payload)
                if recv_exact(s, len(payload)) != payload:
                    raise AssertionError("T7: incorrect reply during churn")
        time.sleep(0.1)
        final = fd_count(server_pid)
        if baseline is not None and final is not None and final > baseline + 1:
            raise AssertionError(f"T7: server fd count grew from {baseline} to {final}")
        final_rss = rss_bytes(server_pid)
        if baseline_rss is not None and final_rss is not None and abs(final_rss - baseline_rss) > 512 * 1024:
            raise AssertionError(f"T7: server RSS did not return near baseline ({baseline_rss} -> {final_rss})")

    # T8: hold 1,000 live connections; the 1,001st must be refused or promptly
    # closed. Server is launched with max_conns=1000.
    if "T8" in case_names:
        held: list[socket.socket] = []
        try:
            for _ in range(1000):
                held.append(connect(port))
            extra = None
            try:
                extra = connect(port)
                extra.settimeout(1.0)
                try:
                    if extra.recv(1) != b"":
                        raise AssertionError("T8: extra connection was not refused")
                except socket.timeout as exc:
                    raise AssertionError("T8: 1,001st connection remained open") from exc
                except ConnectionResetError:
                    pass
            except OSError as exc:
                if exc.errno not in (errno.ECONNREFUSED, errno.ECONNRESET, errno.ETIMEDOUT):
                    raise
            finally:
                if extra is not None:
                    extra.close()
        finally:
            for s in held:
                s.close()

    # T9/T11: deterministic recorded stream, randomly chunked and replayed on
    # every mechanism. Byte equality makes the cross-build session identical.
    if "T9" in case_names or "T11" in case_names:
        rng = random.Random(0xC0FFEE)
        data = frame_bytes(100, 9)
        with connect(port) as s:
            offset = 0
            while offset < len(data):
                amount = min(len(data) - offset, rng.randint(1, 1700))
                s.sendall(data[offset : offset + amount])
                offset += amount
            if recv_exact(s, len(data), timeout=20.0) != data:
                raise AssertionError("T9/T11: recorded randomized stream differs")

    # T10: client half-closes after sending; all replies must arrive before EOF.
    if "T10" in case_names:
        data = frame_bytes(5, 10)
        with connect(port) as s:
            s.sendall(data)
            s.shutdown(socket.SHUT_WR)
            if recv_until_eof(s) != data:
                raise AssertionError("T10: half-close did not receive all bytes then EOF")

    # T14: sustained duplex traffic while application reads in small pieces.
    if "T14" in case_names:
        data = frame_bytes(1, 14)
        with connect(port) as s:
            s.settimeout(35.0)
            deadline = time.monotonic() + 30.0
            sent = bytearray()
            received = bytearray()
            while time.monotonic() < deadline:
                s.sendall(data)
                sent.extend(data)
                target = len(received) + len(data)
                while len(received) < target:
                    received.extend(s.recv(min(1024, target - len(received))))
                    time.sleep(0.01)
            if received != sent:
                raise AssertionError("T14: slow-reader stream lost or changed bytes")


def run_binary(label: str, binary: Path, cases: tuple[str, ...], max_conns: int,
               extra_args: list[str] | None = None) -> None:
    global server_proc, server_pid
    binary = binary.resolve()
    if not binary.is_file():
        raise FileNotFoundError(f"{label}: executable not found: {binary}")
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="echo-gate-") as tmp:
        stats_path = Path(tmp) / "stats.jsonl"
        command = [str(binary), "--port", str(port), "--max-conns", str(max_conns),
                   "--msg-size", str(FRAME_SIZE), "--buf-size", str(BUF_SIZE),
                   "--stats-file", str(stats_path)]
        command.extend(extra_args or [])
        with open(os.devnull, "wb") as sink:
            server_proc = subprocess.Popen(command, stdout=sink, stderr=sink)
            server_pid = server_proc.pid
            try:
                wait_listening(port, server_proc)
                # wait_listening opens a probe connection; allow its close callback.
                time.sleep(0.02)
                run_cases(port, cases)
            finally:
                server_proc.terminate()
                try:
                    server_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server_proc.kill()
                    server_proc.wait(timeout=5)
            if server_proc.returncode not in (0, -15):
                raise AssertionError(f"{label}: server exited with {server_proc.returncode}")
    print(f"PASS {label}: {', '.join(cases)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for mechanism in MECHANISMS:
        parser.add_argument(f"--{mechanism}", type=Path, help=f"baseline {mechanism} echo executable")
        parser.add_argument(f"--{mechanism}-arg", action="append", default=[], metavar="FLAG",
                            help=f"additional {mechanism} server flag; repeat for flag values")
    parser.add_argument("--sanitized", action="append", default=[], metavar="NAME=PATH",
                        help="ASan+UBSan executable; repeat once for each mechanism")
    args = parser.parse_args()
    binaries = {name: getattr(args, name) for name in MECHANISMS if getattr(args, name) is not None}
    if set(binaries) != set(MECHANISMS):
        parser.error("provide all four baseline binaries: --select --poll --epoll --uring")
    cases = tuple(f"T{i}" for i in range(1, 12)) + ("T14",)
    # T12 uses all T1-T11 against explicitly supplied sanitizer binaries.
    sanitizer_bins: list[tuple[str, Path]] = []
    for item in args.sanitized:
        if "=" not in item:
            parser.error(f"invalid --sanitized {item!r}; expected NAME=PATH")
        name, path = item.split("=", 1)
        sanitizer_bins.append((name, Path(path)))
    try:
        for mechanism in MECHANISMS:
            run_binary(mechanism, binaries[mechanism], cases, 1000,
                       getattr(args, f"{mechanism}_arg"))
        for name, binary in sanitizer_bins:
            run_binary(f"T12 {name}", binary, tuple(f"T{i}" for i in range(1, 12)), 1000)
    except (AssertionError, OSError, subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    if len(sanitizer_bins) < 4:
        print("INCOMPLETE T12: provide four ASan+UBSan builds with --sanitized NAME=PATH", file=sys.stderr)
        return 2
    print("PASS echo correctness gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
