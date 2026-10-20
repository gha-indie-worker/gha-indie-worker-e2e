import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
from oreslang_test.race import analyze_trace, TraceError


def check(events):
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "race.jsonl"
        trace = [{"op": "begin", "version": 1, "backend": "fixture", "run_id": "audit"}]
        trace.extend(events)
        trace.append({"op": "end", "run_id": "audit", "events": len(events)})
        path.write_text("".join(json.dumps(x) + "\n" for x in trace), encoding="utf-8")
        return analyze_trace(path)


class RaceHistoryTests(unittest.TestCase):
    def test_write_history_survives_later_writer(self):
        events = [
            {"op": "task", "task": "a"},
            {"op": "task", "task": "b"},
            {"op": "task", "task": "c"},
            {"op": "access", "task": "a", "location": "counter", "mode": "W"},
            {"op": "access", "task": "b", "location": "counter", "mode": "W"},
            {"op": "send", "task": "b", "message": "m1"},
            {"op": "recv", "task": "c", "message": "m1"},
            {"op": "access", "task": "c", "location": "counter", "mode": "R"},
        ]
        self.assertTrue(any(r["first"] == "a" and r["second"] == "c"
                            for r in check(events)["races"]))

    def test_read_history_survives_writer(self):
        events = [
            {"op": "task", "task": "a"},
            {"op": "task", "task": "b"},
            {"op": "task", "task": "c"},
            {"op": "access", "task": "a", "location": "counter", "mode": "R"},
            {"op": "access", "task": "b", "location": "counter", "mode": "W"},
            {"op": "send", "task": "b", "message": "m1"},
            {"op": "recv", "task": "c", "message": "m1"},
            {"op": "access", "task": "c", "location": "counter", "mode": "W"},
        ]
        self.assertTrue(any(r["first"] == "a" and r["second"] == "c"
                            for r in check(events)["races"]))

    def test_finished_child_join_orders_accesses(self):
        events = [
            {"op": "task", "task": "parent"},
            {"op": "fork", "task": "parent", "child": "child"},
            {"op": "access", "task": "child", "location": "x", "mode": "W"},
            {"op": "finish", "task": "child"},
            {"op": "join", "task": "parent", "child": "child"},
            {"op": "access", "task": "parent", "location": "x", "mode": "R"},
        ]
        self.assertEqual(check(events)["races"], [])

    def test_nested_mutex_only_orders_after_final_unlock(self):
        events = [
            {"op": "task", "task": "a"},
            {"op": "task", "task": "b"},
            {"op": "lock", "task": "a", "name": "critical"},
            {"op": "lock", "task": "a", "name": "critical"},
            {"op": "access", "task": "a", "location": "state", "mode": "W"},
            {"op": "unlock", "task": "a", "name": "critical"},
            {"op": "unlock", "task": "a", "name": "critical"},
            {"op": "lock", "task": "b", "name": "critical"},
            {"op": "access", "task": "b", "location": "state", "mode": "R"},
            {"op": "unlock", "task": "b", "name": "critical"},
        ]
        self.assertEqual(check(events)["race_count"], 0)

    def test_other_owner_cannot_acquire_while_recursive_lock_held(self):
        events = [
            {"op": "task", "task": "a"},
            {"op": "task", "task": "b"},
            {"op": "lock", "task": "a", "name": "critical"},
            {"op": "lock", "task": "a", "name": "critical"},
            {"op": "unlock", "task": "a", "name": "critical"},
            {"op": "lock", "task": "b", "name": "critical"},
            {"op": "access", "task": "b", "location": "state", "mode": "W"},
        ]
        with self.assertRaisesRegex(TraceError, "different participant"):
            check(events)

    def test_consumed_message_token_cannot_be_republished(self):
        events = [
            {"op": "task", "task": "a"},
            {"op": "task", "task": "b"},
            {"op": "send", "task": "a", "message": "once"},
            {"op": "recv", "task": "b", "message": "once"},
            {"op": "send", "task": "b", "message": "once"},
            {"op": "access", "task": "b", "location": "x", "mode": "W"},
        ]
        with self.assertRaisesRegex(TraceError, "cannot be reused"):
            check(events)

    def test_sibling_lifetimes_remain_concurrent(self):
        events = [
            {"op": "task", "task": "parent"},
            {"op": "fork", "task": "parent", "child": "child1"},
            {"op": "fork", "task": "parent", "child": "child2"},
            {"op": "access", "task": "child1", "location": "shared", "mode": "W"},
            {"op": "access", "task": "child2", "location": "shared", "mode": "W"},
            {"op": "finish", "task": "child1"},
            {"op": "finish", "task": "child2"},
        ]
        self.assertEqual(check(events)["race_count"], 1)

    def test_join_before_finish_refused(self):
        events = [
            {"op": "task", "task": "parent"},
            {"op": "fork", "task": "parent", "child": "child"},
            {"op": "access", "task": "child", "location": "x", "mode": "W"},
            {"op": "join", "task": "parent", "child": "child"},
            {"op": "finish", "task": "child"},
        ]
        with self.assertRaisesRegex(TraceError, "join requires"):
            check(events)

    def test_foreign_join_refused(self):
        events = [
            {"op": "task", "task": "parent"},
            {"op": "task", "task": "stranger"},
            {"op": "fork", "task": "parent", "child": "child"},
            {"op": "access", "task": "child", "location": "x", "mode": "W"},
            {"op": "finish", "task": "child"},
            {"op": "join", "task": "stranger", "child": "child"},
        ]
        with self.assertRaisesRegex(TraceError, "join requires"):
            check(events)

    def test_activity_after_finish_refused(self):
        events = [
            {"op": "task", "task": "a"},
            {"op": "access", "task": "a", "location": "x", "mode": "W"},
            {"op": "finish", "task": "a"},
            {"op": "access", "task": "a", "location": "x", "mode": "W"},
        ]
        with self.assertRaisesRegex(TraceError, "completed task"):
            check(events)

    def test_unfinished_child_refused(self):
        events = [
            {"op": "task", "task": "a"},
            {"op": "fork", "task": "a", "child": "b"},
            {"op": "access", "task": "a", "location": "x", "mode": "W"},
        ]
        with self.assertRaisesRegex(TraceError, "unfinished"):
            check(events)

    def test_duplicate_json_property_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "trace.jsonl"
            p.write_text('\n'.join([
                '{"op":"begin","version":1,"run_id":"a","backend":"fixture"}',
                '{"op":"task","task":"x","task":"y"}',
                '{"op":"access","task":"x","location":"x","mode":"W"}',
                '{"op":"end","run_id":"a","events":2}']) + '\n', encoding="utf-8")
            with self.assertRaisesRegex(TraceError, "duplicate"):
                analyze_trace(p)

    def test_bad_utf8_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "trace.jsonl"
            p.write_bytes(b'\xff')
            with self.assertRaises(TraceError):
                analyze_trace(p)

    def test_trace_read_budget_rejects_oversized_jsonl(self):
        with tempfile.TemporaryDirectory() as d:
            trace = Path(d) / "too-big.jsonl"
            trace.write_bytes(b"x" * 10_000_001)
            with self.assertRaisesRegex(TraceError, "10 MB"):
                analyze_trace(trace)

    def test_boolean_version_refused(self):
        events = [{"op": "task", "task": "a"}, {"op": "access", "task": "a", "location": "x", "mode": "R"}]
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "trace.jsonl"
            records = [{"op": "begin", "version": True, "run_id": "a", "backend": "fixture"}, *events,
                       {"op": "end", "run_id": "a", "events": len(events)}]
            p.write_text('\n'.join(json.dumps(x) for x in records) + '\n', encoding="utf-8")
            with self.assertRaisesRegex(TraceError, "begin"):
                analyze_trace(p)

    def test_race_reports_are_bounded(self):
        events = [{"op": "task", "task": f"t{i}"} for i in range(16)]
        events += [{"op": "access", "task": f"t{i}", "location": "x", "mode": "W"}
                   for i in range(16)]
        output = check(events)
        self.assertGreater(output["race_count"], 100)
        self.assertEqual(len(output["races"]), 100)


class ModelAndFuzzTests(unittest.TestCase):
    def test_unbounded_model_rejected(self):
        from oreslang_test.logical import explore, ModelError
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "large.json"
            path.write_text(" " * 2_000_001, encoding="utf-8")
            with self.assertRaisesRegex(ModelError, "2 MB"):
                explore(path)

    def test_unknown_model_operation_rejected(self):
        from oreslang_test.logical import explore, ModelError
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "model.json"
            data = {"initial": {"x": 0}, "expected": {"x": 0}, "actors": [
                {"name": "a", "turns": [[{"op": "nonsense", "key": "x"}]]},
                {"name": "b", "turns": [[{"op": "atomic_add", "key": "x", "value": 0}]]}]}
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ModelError, "unknown operation"):
                explore(path)

    def test_undeclared_local_rejected(self):
        from oreslang_test.logical import explore, ModelError
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "model.json"
            data = {"initial": {"x": 0}, "expected": {"x": 0}, "actors": [
                {"name": "a", "turns": [[{"op": "write", "key": "x", "value": {"local": "missing"}}]]},
                {"name": "b", "turns": [[{"op": "atomic_add", "key": "x", "value": 0}]]}]}
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(ModelError, "prior local"):
                explore(path)

    def test_flaky_fuzz_failure_does_not_write_case(self):
        import contextlib
        import io
        from oreslang_test.cli import main
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            program = root / "harness.py"
            program.write_text('from pathlib import Path\nimport sys\nc=Path("count.txt")\nn=int(c.read_text()) if c.exists() else 0\nc.write_text(str(n+1))\nsys.exit(7 if n==0 else 0)\n', encoding="utf-8")
            manifest = root / "oreslang-test.toml"
            manifest.write_text('[fuzz]\ncommand = [' + json.dumps(sys.executable) + ', "harness.py"]\n', encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as output:
                status = main(["--root", str(root), "--fuzz", "--cases", "1"])
            self.assertEqual(status, 2)
            self.assertIn("FUZZ FLAKY", output.getvalue())
            self.assertFalse((root / ".oreslang-test").exists())

    def test_output_limit_fails_closed(self):
        from oreslang_test.cli import _run, UsageError
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(UsageError, "4 MB"):
                _run([sys.executable, "-c", "import sys; sys.stdout.write('X'*4000001)"], Path(d), 5)


class ProcessBudgetTests(unittest.TestCase):
    def test_stderr_flood_is_stopped_before_timeout(self):
        import time
        from oreslang_test.cli import _run, UsageError
        with tempfile.TemporaryDirectory() as d:
            started = time.monotonic()
            with self.assertRaisesRegex(UsageError, "4 MB"):
                _run([sys.executable, "-c",
                      "import os; b=b'x'*65536\nwhile True: os.write(2,b)"], Path(d), 10)
            self.assertLess(time.monotonic() - started, 5)

    @unittest.skipUnless(__import__("os").name == "posix", "requires POSIX process groups")
    def test_orphan_output_cannot_report_success(self):
        from oreslang_test.cli import _run, UsageError
        command = [sys.executable, "-c", "import subprocess,sys; "
                   "subprocess.Popen([sys.executable,'-c','import time; time.sleep(20)'])"]
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(UsageError, "descendants outlived"):
                _run(command, Path(d), 10)


if __name__ == "__main__":
    unittest.main()
