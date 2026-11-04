#!/usr/bin/env python3
"""Run wrk traffic while curl clients read slowly from the same filesrv."""
from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--file", required=True)
    parser.add_argument("--connections", type=int, default=100)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--slow-readers", type=int, choices=(0, 10, 100), required=True)
    parser.add_argument("--duration", type=float, required=True)
    parser.add_argument(
        "--wrk-script",
        default="code/tools/File-Transfer-Server/wrk_report.lua",
    )
    parser.add_argument("--warmup", action="store_true")
    args = parser.parse_args()

    url = f"http://127.0.0.1:{args.port}/{args.file}"
    slow: list[subprocess.Popen[bytes]] = []
    try:
        for _ in range(args.slow_readers):
            slow.append(subprocess.Popen([
                "curl", "--silent", "--show-error", "--fail",
                "--limit-rate", "100K", "--max-time", str(args.duration + 10),
                "--output", os.devnull, url,
            ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE))

        # Let the slow sockets enter the server before the fast-client burst.
        if slow:
            time.sleep(0.2)

        wrk = [
            "wrk", f"-t{args.threads}", f"-c{args.connections}",
            f"-d{args.duration:g}s", "--timeout", "120s",
        ]
        if not args.warmup:
            wrk.extend(["--latency", "-s", args.wrk_script])
        wrk.append(url)
        completed = subprocess.run(wrk, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        sys.stdout.buffer.write(completed.stdout)
        sys.stdout.buffer.flush()
        if completed.returncode != 0:
            return completed.returncode

        # A slow transfer must remain alive throughout the fast workload. An
        # early curl exit indicates that this condition was not achieved.
        early_failures = [proc for proc in slow if proc.poll() not in (None, 0)]
        if early_failures:
            for proc in early_failures:
                detail = proc.stderr.read().decode(errors="replace") if proc.stderr else ""
                print(f"slow curl exited early ({proc.returncode}): {detail}", file=sys.stderr)
            return 1
        return 0
    finally:
        for proc in slow:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for proc in slow:
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
