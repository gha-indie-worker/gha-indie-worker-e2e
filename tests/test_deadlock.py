"""Conformance tests for observed and potential deadlock diagnostics."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
from oreslang_test.deadlock import analyze_deadlocks
from oreslang_test.race import TraceError
from oreslang_test.cli import main


def trace(folder, events):
    path = folder / "events.jsonl"
    records = ([{"op": "begin", "version": 1, "run_id": "run-1", "backend": "fixture"}]
               + events
               + [{"op": "end", "run_id": "run-1", "events": len(events)}])
    path.write_text("".join(json.dumps(record) + "\n" for record in records),
                    encoding="utf-8")
    return path


class DeadlockGraphTests(unittest.TestCase):
    def test_potential_lock_order_inversion_reports_cycle(self):
        with tempfile.TemporaryDirectory() as d:
            events = [
                {"op": "task", "task": "a"},
                {"op": "task", "task": "b"},
                {"op": "lock", "task": "a", "name": "left"},
                {"op": "lock", "task": "a", "name": "right"},
                {"op": "unlock", "task": "a", "name": "right"},
                {"op": "unlock", "task": "a", "name": "left"},
                {"op": "lock", "task": "b", "name": "right"},
                {"op": "lock", "task": "b", "name": "left"},
                {"op": "unlock", "task": "b", "name": "left"},
                {"op": "unlock", "task": "b", "name": "right"},
            ]
            result = analyze_deadlocks(trace(Path(d), events))
            self.assertEqual(result["potential_lock_order_cycles"],
                             [["left", "right", "left"]])
            self.assertEqual(result["observed_wait_cycles"], [])

    def test_consistent_lock_order_no_cycle(self):
        with tempfile.TemporaryDirectory() as d:
            events = [
                {"op": "task", "task": "a"},
                {"op": "task", "task": "b"},
            ]
            for task in ("a", "b"):
                events += [
                    {"op": "lock", "task": task, "name": "left"},
                    {"op": "lock", "task": task, "name": "right"},
                    {"op": "unlock", "task": task, "name": "right"},
                    {"op": "unlock", "task": task, "name": "left"},
                ]
            result = analyze_deadlocks(trace(Path(d), events))
            self.assertEqual(result["potential_lock_order_cycles"], [])
            self.assertEqual(result["observed_wait_cycles"], [])

    def test_two_actor_wait_cycle_at_trace_end(self):
        with tempfile.TemporaryDirectory() as d:
            events = [
                {"op": "task", "task": "a"},
                {"op": "task", "task": "b"},
                {"op": "lock", "task": "a", "name": "left"},
                {"op": "lock", "task": "b", "name": "right"},
                {"op": "wait", "task": "a", "name": "right"},
                {"op": "wait", "task": "b", "name": "left"},
            ]
            result = analyze_deadlocks(trace(Path(d), events))
            self.assertEqual(result["observed_wait_cycles"], [["a", "b", "a"]])

    def test_cancelled_wait_is_not_a_persistent_deadlock(self):
        with tempfile.TemporaryDirectory() as d:
            events = [
                {"op": "task", "task": "a"},
                {"op": "task", "task": "b"},
                {"op": "lock", "task": "a", "name": "left"},
                {"op": "lock", "task": "b", "name": "right"},
                {"op": "wait", "task": "a", "name": "right"},
                {"op": "wait", "task": "b", "name": "left"},
                {"op": "unwait", "task": "a", "name": "right"},
                {"op": "unwait", "task": "b", "name": "left"},
                {"op": "unlock", "task": "a", "name": "left"},
                {"op": "unlock", "task": "b", "name": "right"},
            ]
            result = analyze_deadlocks(trace(Path(d), events))
            self.assertEqual(result["observed_wait_cycles"], [])

    def test_no_lock_evidence_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(TraceError, "inconclusive"):
                analyze_deadlocks(trace(Path(d), [{"op": "task", "task": "a"}]))

    def test_double_owned_lock_and_unmatched_wait_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            base = [{"op": "task", "task": "a"},
                    {"op": "task", "task": "b"},
                    {"op": "lock", "task": "a", "name": "left"}]
            with self.assertRaisesRegex(TraceError, "already held"):
                analyze_deadlocks(trace(Path(d), base + [
                    {"op": "lock", "task": "b", "name": "left"}]))
            with self.assertRaisesRegex(TraceError, "unwait"):
                analyze_deadlocks(trace(Path(d), base + [
                    {"op": "unwait", "task": "b", "name": "left"}]))

    def test_stale_trace_nonce_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(TraceError, "stale"):
                analyze_deadlocks(trace(Path(d), [
                    {"op": "task", "task": "a"},
                    {"op": "lock", "task": "a", "name": "left"}]),
                    expected_run_id="different")

    def test_cli_deadlock_mode_reports_nonzero_for_lock_cycle(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            trace(root, [
                {"op": "task", "task": "a"},
                {"op": "task", "task": "b"},
                {"op": "lock", "task": "a", "name": "x"},
                {"op": "lock", "task": "a", "name": "y"},
                {"op": "unlock", "task": "a", "name": "y"},
                {"op": "unlock", "task": "a", "name": "x"},
                {"op": "lock", "task": "b", "name": "y"},
                {"op": "lock", "task": "b", "name": "x"},
            ])
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = main(["--root", d, "--deadlock", "--trace", "events.jsonl"])
            self.assertEqual(status, 1)
            self.assertIn("POTENTIAL LOCK-ORDER CYCLE:", output.getvalue())

    def test_graal_runtime_cycle_receipt_reports_nonzero(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "runtime.trace").write_text(
                "ORES_CONCURRENCY_TRACE_V1|seed=42\n"
                "task-turn|1\n"
                "deadlock-future-cycle|0\n", encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = main(["--root", d, "--deadlock", "--trace", "runtime.trace"])
            self.assertEqual(code, 1)
            self.assertIn("RUNTIME CYCLE DETECTED", output.getvalue())

    def test_graal_v2_trace_preserves_cycle_and_rejects_malformed_metadata(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            header = ("ORES_CONCURRENCY_TRACE_V2|seed=11|entry-sha256="
                      + "a" * 64 + "|backend=jit-server|probe=1")
            path = root / "v2.trace"
            path.write_text(header + "\n" + "deadlock-mutex-cycle|0\n", encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = main(["--root", d, "--deadlock", "--trace", "v2.trace"])
            self.assertEqual(status, 1)
            self.assertIn("RUNTIME CYCLE DETECTED", output.getvalue())
            path.write_text(header.replace("probe=1", "probe=2")
                            + "\n" + "deadlock-mutex-cycle|0\n",
                            encoding="utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                status = main(["--root", d, "--deadlock", "--trace", "v2.trace"])
            self.assertEqual(status, 2)
            self.assertIn("invalid runtime checkpoint trace header", stderr.getvalue())

    def test_graal_v3_source_snapshot_trace_detects_cycle(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            header = ("ORES_CONCURRENCY_TRACE_V3|seed=12|entry-sha256="
                      + "a" * 64 + "|workspace-sha256=" + "b" * 64
                      + "|backend=jit-server|probe=3")
            trace = root / "v3.trace"
            trace.write_text(header + "\n" + "deadlock-future-cycle|0\n",
                             encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = main(["--root", d, "--deadlock", "--trace", "v3.trace"])
            self.assertEqual(status, 1)
            self.assertIn("RUNTIME CYCLE DETECTED", output.getvalue())
            trace.write_text(header.replace("workspace-sha256=" + "b" * 64,
                                            "workspace-sha256=bad")
                             + "\n" + "deadlock-future-cycle|0\n",
                             encoding="utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                status = main(["--root", d, "--deadlock", "--trace", "v3.trace"])
            self.assertEqual(status, 2)
            self.assertIn("invalid runtime checkpoint trace header", stderr.getvalue())

    def test_graal_checkpoint_only_trace_does_not_certify_deadlock_freedom(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "runtime.trace").write_text(
                "ORES_CONCURRENCY_TRACE_V1|seed=42\n"
                "task-turn|1\n", encoding="utf-8")
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = main(["--root", d, "--deadlock", "--trace", "runtime.trace"])
            self.assertEqual(code, 2)
            self.assertIn("inconclusive", stderr.getvalue())

    def test_cli_no_trace_refuses_false_success(self):
        with tempfile.TemporaryDirectory() as d:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                status = main(["--root", d, "--deadlock"])
            self.assertEqual(status, 2)
            self.assertIn("refusing false PASS", err.getvalue())


if __name__ == "__main__":
    unittest.main()
