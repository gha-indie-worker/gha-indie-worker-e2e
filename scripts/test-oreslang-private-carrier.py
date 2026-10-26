#!/usr/bin/env python3
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("verify-oreslang-private-carrier.py")
spec = importlib.util.spec_from_file_location("carrier", SCRIPT)
carrier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(carrier)


def run(*args, cwd, env=None):
    subprocess.run(args, cwd=cwd, env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def out(*args, cwd, env=None):
    return subprocess.check_output(args, cwd=cwd, env=env, text=True).strip()


class CarrierVerifierTest(unittest.TestCase):
    def test_expiry_bounds(self):
        now = dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc)
        carrier.parse_expiry("2026-10-21T00:00:00Z", now=now)
        with self.assertRaises(carrier.Denied):
            carrier.parse_expiry("2026-10-19T23:59:59Z", now=now)
        with self.assertRaises(carrier.Denied):
            carrier.parse_expiry("2026-10-28T00:00:01Z", now=now)

    def test_hex_validation(self):
        self.assertEqual("a"*40, carrier.require_hex("a"*40, carrier.HEX40, "x"))
        for value in ("A"*40, "a"*39, "g"*40, None):
            with self.subTest(value=value), self.assertRaises(carrier.Denied):
                carrier.require_hex(value, carrier.HEX40, "x")

    def _repo(self, extra_source_change=None, mutate_workflow=False):
        td = tempfile.TemporaryDirectory()
        root = Path(td.name)
        run("git", "init", "-q", cwd=root)
        run("git", "config", "user.name", "Synthetic CI", cwd=root)
        run("git", "config", "user.email", "ci@example.invalid", cwd=root)
        workflow = root/".github/workflows/oreslang-exact-tree-carrier.yml"
        workflow.parent.mkdir(parents=True)
        workflow.write_text("name: synthetic carrier\n")
        (root/"README.md").write_text("carrier base\n")
        plan_path = root/"ci/test-plans/synthetic.json"
        plan_path.parent.mkdir(parents=True)
        plan_data = {
            "schema":"ores-private-ci-test-plan-v1",
            "source_repo":"synthetic/private",
            "allowed_prs":[7],
            "commands":[
                {"argv":["python3","-c","print('synthetic plan')"],
                 "cwd":"private-oreslang-source",
                 "timeout_seconds":30}
            ]
        }
        plan_bytes = (json.dumps(plan_data, sort_keys=True, separators=(",",":"))+"\n").encode()
        plan_path.write_bytes(plan_bytes)
        plan_sha256 = hashlib.sha256(plan_bytes).hexdigest()
        run("git", "add", "-A", cwd=root)
        run("git", "commit", "-qm", "base", cwd=root)
        base_sha = out("git", "rev-parse", "HEAD", cwd=root)
        workflow_blob = out("git", "rev-parse", "HEAD:.github/workflows/oreslang-exact-tree-carrier.yml", cwd=root)

        source = root/"private-oreslang-source"
        (source/"stdlib").mkdir(parents=True)
        (source/"core.txt").write_text("synthetic-only\n")
        runner = source/"stdlib/runner.sh"
        runner.write_text("#!/bin/sh\nexit 0\n")
        runner.chmod(0o755)
        run("git", "add", "private-oreslang-source", cwd=root)
        source_tree = out("git", "write-tree", cwd=root)
        subtree = out("git", "rev-parse", source_tree+":private-oreslang-source", cwd=root)
        old_cwd = os.getcwd()
        os.chdir(root)
        try:
            _, _, manifest = carrier.tree_manifest(subtree)
        finally:
            os.chdir(old_cwd)

        if mutate_workflow:
            workflow.write_text("name: mutated in source-bearing commit\n")
        if extra_source_change:
            path, content = extra_source_change
            target = root/path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)

        receipt = {
            "schema":"ores-private-ci-receipt-v3",
            "expires_at":"2099-01-02T00:00:00Z",
            "carrier":{"workflow_blob_sha":workflow_blob},
            "destination":{
                "repo":"synthetic/carrier",
                "repo_id":42,
                "base_sha":base_sha
            },
            "mirror":{"root_tree_sha":subtree},
            "source":{
                "repo":"synthetic/private",
                "repo_id":1,
                "pr":7,
                "head_sha":"1"*40,
                "root_tree_sha":subtree,
                "manifest_sha256":manifest
            },
            "test_plan":{"path":"ci/test-plans/synthetic.json","sha256":plan_sha256}
        }
        receipt_path = root/"ci/oreslang-receipt.json"
        receipt_path.parent.mkdir(exist_ok=True)
        receipt_path.write_text(json.dumps(receipt, sort_keys=True, separators=(",",":"))+"\n")
        run("git", "add", "-A", cwd=root)
        digest = carrier.canonical_digest(receipt)
        run("git", "commit", "-qm",
            "synthetic mirror\n\nOreslang-CI-Receipt-SHA256: "+digest, cwd=root)
        head = out("git", "rev-parse", "HEAD", cwd=root)
        return td, root, head

    def _verify(self, root, head):
        source_tree = out("git", "rev-parse", "HEAD:private-oreslang-source", cwd=root)
        old_cwd = os.getcwd()
        os.chdir(root)
        try:
            return carrier.verify(
                "ci/oreslang-receipt.json",
                "private-oreslang-source",
                ".github/workflows/oreslang-exact-tree-carrier.yml",
                "synthetic/carrier", 42, head,
                "ci/oreslang-" + source_tree,
                now=dt.datetime(2099, 1, 1, tzinfo=dt.timezone.utc),
            )
        finally:
            os.chdir(old_cwd)

    def test_complete_synthetic_history_passes(self):
        td, root, head = self._repo()
        try:
            result = self._verify(root, head)
            self.assertEqual(result["source_tree"], out(
                "git", "rev-parse", "HEAD:private-oreslang-source", cwd=root))
            self.assertEqual(result["file_count"], 2)
            self.assertGreater(result["byte_count"], 0)
        finally:
            td.cleanup()

    def test_unexpected_destination_change_fails(self):
        td, root, head = self._repo(("outside.txt", "not allowed\n"))
        try:
            with self.assertRaisesRegex(carrier.Denied, "unexpected destination change"):
                self._verify(root, head)
        finally:
            td.cleanup()

    def test_workflow_mutation_in_source_commit_fails(self):
        td, root, head = self._repo(mutate_workflow=True)
        try:
            with self.assertRaisesRegex(carrier.Denied, "unexpected destination change|workflow changed"):
                self._verify(root, head)
        finally:
            td.cleanup()

    def test_symlink_git_entry_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            run("git", "init", "-q", cwd=root)
            (root/"target").write_text("x")
            (root/"link").symlink_to("target")
            run("git", "add", "-A", cwd=root)
            tree = out("git", "write-tree", cwd=root)
            old_cwd = os.getcwd()
            os.chdir(root)
            try:
                with self.assertRaisesRegex(carrier.Denied, "unsupported Git entry"):
                    carrier.tree_manifest(tree)
            finally:
                os.chdir(old_cwd)


if __name__ == "__main__":
    unittest.main(verbosity=2)
