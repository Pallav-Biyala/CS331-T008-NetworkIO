#!/usr/bin/env python3
"""Focused tests for benchmark connection acceptance validation."""

import importlib.machinery
import importlib.util
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
LOADER = importlib.machinery.SourceFileLoader(
    "run_experiment", str(ROOT / "code/tools/File-Transfer-Server/run_experiment")
)
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
runner = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(runner)


class FinishedClient:
    def poll(self):
        return 0


class RunningServer:
    returncode = None

    def poll(self):
        return None

    def send_signal(self, _signal):
        pass


class AcceptanceValidationTests(unittest.TestCase):
    def stats_file(self, directory, accepts, peak):
        path = pathlib.Path(directory) / "stats.jsonl"
        path.write_text(
            json.dumps({"stats": {
                "accepts": accepts,
                "active_connections": 0,
                "peak_connections": peak,
            }}) + "\n",
            encoding="utf-8",
        )
        return path

    def test_completed_client_can_pass_on_observed_peak(self):
        with tempfile.TemporaryDirectory() as directory:
            stats = self.stats_file(directory, accepts=9, peak=10)
            result = runner.wait_for_peak_connections(
                RunningServer(), stats, 10, timeout=0.01,
                client=FinishedClient(),
            )
        self.assertEqual(result["stats"]["peak_connections"], 10)
        self.assertLess(result["stats"]["accepts"], 10)

    def test_peak_below_configured_concurrency_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            stats = self.stats_file(directory, accepts=9, peak=9)
            with self.assertRaisesRegex(TimeoutError, "9/10"):
                runner.wait_for_peak_connections(
                    RunningServer(), stats, 10, timeout=0.01,
                    client=FinishedClient(),
                )


if __name__ == "__main__":
    unittest.main()
