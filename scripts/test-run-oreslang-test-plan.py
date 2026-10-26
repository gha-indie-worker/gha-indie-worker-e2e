#!/usr/bin/env python3
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("run-oreslang-test-plan.py")
spec = importlib.util.spec_from_file_location("plan_runner", SCRIPT)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class TrustedPlanRunnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.old_cwd = Path.cwd()
        os.chdir(self.root)
        self.addCleanup(lambda: os.chdir(self.old_cwd))
        (self.root/"private-oreslang-source/formal").mkdir(parents=True)
        (self.root/"private-oreslang-source/formal/input.txt").write_text("synthetic\n")
        (self.root/"ci/test-plans").mkdir(parents=True)

    def write(self, command=None, source_repo="synthetic/private", pr=7):
        command = command or {
            "argv":["python3","-c","print('TRUSTED_PLAN_SYNTHETIC_PASS')"],
            "cwd":"formal",
            "timeout_seconds":30,
            "stdout_contains":"TRUSTED_PLAN_SYNTHETIC_PASS",
        }
        plan = {
            "schema":"ores-private-ci-test-plan-v1",
            "source_repo":source_repo,
            "allowed_prs":[pr],
            "commands":[command],
        }
        plan_bytes=(json.dumps(plan,sort_keys=True,separators=(",",":"))+"\n").encode()
        plan_path=self.root/"ci/test-plans/synthetic.json"
        plan_path.write_bytes(plan_bytes)
        receipt={
            "schema":"ores-private-ci-receipt-v3",
            "source":{"repo":"synthetic/private","pr":7},
            "test_plan":{
                "path":"ci/test-plans/synthetic.json",
                "sha256":hashlib.sha256(plan_bytes).hexdigest(),
            },
        }
        receipt_path=self.root/"ci/oreslang-receipt.json"
        receipt_path.write_text(json.dumps(receipt))
        return receipt_path

    def test_happy_path_executes_without_shell(self):
        receipt=self.write()
        result=runner.run_plan(str(receipt),"private-oreslang-source")
        self.assertEqual(len(result),1)
        self.assertEqual(result[0]["argv"][0],"python3")

    def test_plan_digest_mismatch_fails(self):
        receipt=self.write()
        plan=self.root/"ci/test-plans/synthetic.json"
        plan.write_text(plan.read_text()+" ")
        with self.assertRaisesRegex(runner.Denied,"digest mismatch"):
            runner.run_plan(str(receipt),"private-oreslang-source")

    def test_shell_executable_rejected(self):
        receipt=self.write({
            "argv":["bash","-lc","echo should-not-run"],
            "cwd":".",
            "timeout_seconds":30,
        })
        with self.assertRaisesRegex(runner.Denied,"unapproved executable"):
            runner.run_plan(str(receipt),"private-oreslang-source")

    def test_cwd_escape_rejected(self):
        receipt=self.write({
            "argv":["python3","-c","print('x')"],
            "cwd":"../",
            "timeout_seconds":30,
        })
        with self.assertRaisesRegex(runner.Denied,"unsafe command cwd|escapes"):
            runner.run_plan(str(receipt),"private-oreslang-source")

    def test_stdout_evidence_required(self):
        receipt=self.write({
            "argv":["python3","-c","print('actual')"],
            "cwd":".",
            "timeout_seconds":30,
            "stdout_contains":"expected",
        })
        with self.assertRaisesRegex(runner.Denied,"missing required stdout"):
            runner.run_plan(str(receipt),"private-oreslang-source")

    def test_plan_cannot_authorize_different_source(self):
        receipt=self.write(source_repo="other/private")
        with self.assertRaisesRegex(runner.Denied,"source repository mismatch"):
            runner.run_plan(str(receipt),"private-oreslang-source")


if __name__ == "__main__":
    unittest.main(verbosity=2)
