import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
from oreslang_test.cli import main
from oreslang_test.logical import explore, ModelError
from oreslang_test.race import analyze_trace, TraceError

ROOT = Path(__file__).resolve().parent


class RaceTests(unittest.TestCase):
    def test_conflicting_unordered_accesses(self):
        result = analyze_trace(ROOT / "traces/racy.jsonl")
        self.assertEqual(len(result["races"]), 1)
        self.assertEqual(result["races"][0]["location"], "heap:object1.value")

    def test_send_receive_establishes_happens_before(self):
        result = analyze_trace(ROOT / "traces/ordered.jsonl")
        self.assertEqual(result["races"], [])

    def test_rejects_stale_nonce(self):
        with self.assertRaises(TraceError):
            analyze_trace(ROOT / "traces/ordered.jsonl", expected_run_id="different")

    def test_rejects_incomplete_trace(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "trace.jsonl"
            p.write_text('{"op":"begin","version":1,"run_id":"a","backend":"llvm"}\n', encoding="utf-8")
            with self.assertRaises(TraceError):
                analyze_trace(p)

    def test_rejects_unsynchronized_lock_release(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "trace.jsonl"
            p.write_text('\n'.join([
                '{"op":"begin","version":1,"run_id":"a","backend":"llvm"}',
                '{"op":"task","task":"a"}',
                '{"op":"unlock","task":"a","name":"mutex"}',
                '{"op":"end","run_id":"a","events":2}']) + '\n', encoding="utf-8")
            with self.assertRaises(TraceError):
                analyze_trace(p)


class LogicalTests(unittest.TestCase):
    def test_lost_update_detected(self):
        found = explore(ROOT / "models/lost-update.json")
        self.assertEqual(found["violation"]["expected"], {"count": 2})
        self.assertEqual(found["violation"]["final"]["count"], 1)

    def test_atomic_update_passes(self):
        result = explore(ROOT / "models/atomic-update.json")
        self.assertIsNone(result["violation"])
        self.assertEqual(result["schedules"], 2)

    def test_limit_refuses_unproven_success(self):
        with self.assertRaises(ModelError):
            explore(ROOT / "models/atomic-update.json", max_schedules=1)


class CommandTests(unittest.TestCase):
    def call(self, *args):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = main(args)
        return status, stdout.getvalue(), stderr.getvalue()

    def test_race_and_logical_commands(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / "oreslang-test.toml").write_text("", encoding="utf-8")
            target = p / "model.json"
            target.write_bytes((ROOT / "models/lost-update.json").read_bytes())
            status, output, _ = self.call("--root", d, "--race", "--logical", "--model", "model.json")
            self.assertEqual(status, 1)
            self.assertIn("LOGICAL RACE", output)
            status, _, err = self.call("--root", d, "--race")
            self.assertEqual(status, 2)
            self.assertIn("refusing a false PASS", err)

    def test_fuzz_failure_minimized_and_persisted(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            script = p / "harness.py"
            script.write_text("import sys\ns=sys.stdin.buffer.read()\nsys.exit(7 if b'\\xff' in s else 0)\n", encoding="utf-8")
            (p / "oreslang-test.toml").write_text(
                '[fuzz]\ncommand = ["' + sys.executable + '", "harness.py"]\nmax_bytes=16\n', encoding="utf-8")
            status, output, _ = self.call("--root", d, "--fuzz", "--seed", "5", "--cases", "4")
            self.assertEqual(status, 1)
            self.assertIn("FUZZ FAILURE", output)
            failures = list((p / ".oreslang-test/failures").glob("*.bin"))
            self.assertEqual(len(failures), 1)
            self.assertEqual(failures[0].read_bytes(), b"\xff")

    def test_fuzz_success(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / "oreslang-test.toml").write_text(
                '[fuzz]\ncommand = ["' + sys.executable + '", "-c", "import sys; sys.stdin.buffer.read()"]\n', encoding="utf-8")
            status, output, _ = self.call("--root", d, "--fuzz", "--seed", "2", "--cases", "7")
            self.assertEqual(status, 0)
            self.assertIn("FUZZ PASS cases=7 seed=2", output)

    def test_native_only_skipped_cases_fail_closed(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / "tests").mkdir()
            (p / "tests/test_skipped.ores").write_text("// test", encoding="utf-8")
            runner = p / "runner"
            runner.write_text("#!" + sys.executable + "\n"
                              + "print('ORES_TEST|SKIP|suite/skipped|')\n"
                              + "print('ORES_TEST|SUMMARY|total=1|passed=0|failed=0|skipped=1|filtered=0')\n",
                              encoding="utf-8")
            runner.chmod(0o755)
            import os
            prior = os.environ.get("ORESLANG_BIN")
            os.environ["ORESLANG_BIN"] = str(runner)
            try:
                status, output, _ = self.call("--root", d)
                self.assertEqual(status, 1)
                self.assertIn("FAIL tests/test_skipped.ores", output)
                runner.write_text("#!" + sys.executable + "\n"
                                  + "print('ORES_TEST|PASS|suite/case|')\n"
                                  + "print('ORES_TEST|SUMMARY|total=1|passed=1|passed=1|failed=0|skipped=0|filtered=0')\n",
                                  encoding="utf-8")
                status, output, _ = self.call("--root", d)
                self.assertEqual(status, 1)
                self.assertIn("FAIL tests/test_skipped.ores", output)
            finally:
                if prior is None:
                    os.environ.pop("ORESLANG_BIN", None)
                else:
                    os.environ["ORESLANG_BIN"] = prior

    def test_native_machine_protocol(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p / "tests").mkdir()
            (p / "tests/test_math.ores").write_text("// placeholder", encoding="utf-8")
            script = p / "host.py"
            script.write_text('print("ORES_TEST|PASS|suite/case|")\nprint("ORES_TEST|SUMMARY|total=1|passed=1|failed=0|skipped=0|filtered=0")\n', encoding="utf-8")
            runner = p / "host"
            runner.write_text(f"#!{sys.executable}\n" + script.read_text(), encoding="utf-8")
            runner.chmod(0o755)
            import os
            prior = os.environ.get("ORESLANG_BIN")
            os.environ["ORESLANG_BIN"] = str(runner)
            try:
                (p / "oreslang-test.toml").write_text("", encoding="utf-8")
                status, output, _ = self.call("--root", d)
                self.assertEqual(status, 0)
                self.assertIn("PASS tests/test_math.ores", output)
            finally:
                if prior is None:
                    os.environ.pop("ORESLANG_BIN", None)
                else:
                    os.environ["ORESLANG_BIN"] = prior


if __name__ == "__main__":
    unittest.main()
