#!/usr/bin/env python3
import unittest
from pathlib import Path
import sys
from unittest.mock import patch
from subprocess import CompletedProcess

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run

class DiagnosticProof(unittest.TestCase):
    def test_only_semantic_exit_one_is_rejection(self):
        for status in (0, 2, 3, 126, 127, -9, -11):
            with self.subTest(status=status):
                self.assertFalse(run.check_diagnostic(status, "", "unknown parameter", "unknown_name", "llvm"))
        self.assertTrue(run.check_diagnostic(1, "", "unknown parameter 'x'", "unknown_name", "llvm"))

    def test_unrelated_error_cannot_pass_negative(self):
        self.assertFalse(run.check_diagnostic(1, "", "wrong call arity", "unknown_name", "llvm"))
        self.assertFalse(run.check_diagnostic(1, "", "", "unknown_name", "llvm"))
        self.assertFalse(run.check_diagnostic(1, "partial IR", "unknown parameter", "unknown_name", "llvm"))
        self.assertTrue(run.check_diagnostic(1, "", "duplicate function", "duplicate_declaration", "llvm"))

    def test_command_requires_one_placeholder(self):
        for command in ("", "clang foo", "tool {source} {source}"):
            with self.subTest(command=command):
                with self.assertRaises(ValueError):
                    run.validate_command(command)
        self.assertEqual(run.validate_command("compiler {source}"), ["compiler", "{source}"])

    def test_validates_strict_schema_fixture_inventory(self):
        suite, digest = run.validate_suite()
        self.assertGreaterEqual(len(suite["cases"]), 10)
        self.assertEqual(len(digest), 64)

    def test_mutating_fixture_or_schema_changes_suite_identity(self):
        import shutil
        import tempfile
        original_suite, original_digest = run.validate_suite()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            shutil.copytree(run.ROOT, root, dirs_exist_ok=True)
            with patch.object(run, "ROOT", root):
                _, untouched = run.validate_suite()
                self.assertEqual(original_digest, untouched)
                case = original_suite["cases"][0]
                fixture = root / case["source"]
                before = fixture.read_bytes()
                fixture.write_bytes(before + b" // unexpected backend divergence\n")
                _, changed_source = run.validate_suite()
                self.assertNotEqual(original_digest, changed_source)
                fixture.write_bytes(before)
                schema = root / "schema.json"
                schema.write_text(schema.read_text() + " ")
                _, changed_schema = run.validate_suite()
                self.assertNotEqual(original_digest, changed_schema)

    def test_compiler_crash_never_counts_as_pass(self):
        case = {"id":"diagnostic.crash","kind":"diagnostic","source":"fixtures/unknown.ores",
                "diagnostic_class":"unknown_name"}
        for exit_code in (-11, 2, 127):
            with patch.object(run.subprocess, "run", return_value=CompletedProcess(
                    args=[], returncode=exit_code, stdout="", stderr="unknown parameter")):
                result = run.run_case(case, "llvm", ["fake", "{source}"], "", 3)
                self.assertEqual("fail", result["status"])

    def test_native_test_does_not_skip_missing_clang(self):
        case = {"id":"native.add","kind":"native_exit","source":"fixtures/add.ores","expected_exit":42}
        with patch.object(run.subprocess, "run", return_value=CompletedProcess(
                args=[], returncode=0, stdout="; ModuleID = 'foo'\n", stderr="")):
            result = run.run_case(case, "llvm", ["fake", "{source}"], "", 3)
            self.assertEqual("fail", result["status"])

if __name__ == "__main__":
    unittest.main()
