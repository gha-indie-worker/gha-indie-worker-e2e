#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("check-carrier-main-policy.py")
spec = importlib.util.spec_from_file_location("carrier_policy", SCRIPT)
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)

HEAD = "a" * 40
BASE = "b" * 40
REPO = "trusted/ci"
REPO_ID = 42

def repo():
    return {
        "id": REPO_ID,
        "full_name": REPO,
        "private": True,
        "visibility": "private",
        "default_branch": "main",
        "archived": False,
        "disabled": False,
        "fork": False,
    }

def branch():
    return {"name": "main", "protected": True, "commit": {"sha": BASE}}

def commit():
    return {"sha": HEAD, "parents": [{"sha": BASE}]}

class CarrierMainPolicyTest(unittest.TestCase):
    def test_valid_exact_protected_parent_passes(self):
        self.assertEqual(policy.check(repo(), branch(), commit(), REPO, REPO_ID, HEAD), BASE)

    def test_unprotected_main_fails(self):
        b = branch()
        b["protected"] = False
        with self.assertRaisesRegex(policy.Denied, "not protected"):
            policy.check(repo(), b, commit(), REPO, REPO_ID, HEAD)

    def test_main_move_fails(self):
        b = branch()
        b["commit"]["sha"] = "c" * 40
        with self.assertRaisesRegex(policy.Denied, "moved"):
            policy.check(repo(), b, commit(), REPO, REPO_ID, HEAD)

    def test_merge_source_commit_fails(self):
        c = commit()
        c["parents"].append({"sha": "c" * 40})
        with self.assertRaisesRegex(policy.Denied, "exactly one parent"):
            policy.check(repo(), branch(), c, REPO, REPO_ID, HEAD)

    def test_repository_identity_replacement_fails(self):
        r = repo()
        r["id"] += 1
        with self.assertRaisesRegex(policy.Denied, "identity/privacy"):
            policy.check(r, branch(), commit(), REPO, REPO_ID, HEAD)

    def test_duplicate_json_key_fails(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "repo.json"
            p.write_text('{"id":42,"id":43}')
            with self.assertRaisesRegex(policy.Denied, "invalid"):
                policy.load_json(p, "fixture")

if __name__ == "__main__":
    unittest.main(verbosity=2)
