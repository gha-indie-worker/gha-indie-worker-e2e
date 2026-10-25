#!/usr/bin/env python3
"""Verify an Oreslang private CI carrier commit without contacting the source repo."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
MAX_FILES = 1024
MAX_TOTAL = 40 * 1024 * 1024
MAX_FILE = 10 * 1024 * 1024


class Denied(RuntimeError):
    pass


def git(*args):
    try:
        return subprocess.check_output(["git", *args], text=True).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise Denied("required Git metadata unavailable") from exc


def canonical_digest(value):
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
    return hashlib.sha256(raw).hexdigest()


def require_hex(value, regex, label):
    if not isinstance(value, str) or not regex.fullmatch(value):
        raise Denied(label)
    return value


def parse_expiry(value, now=None):
    if not isinstance(value, str) or not value.endswith("Z"):
        raise Denied("receipt expiry")
    try:
        expiry = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise Denied("receipt expiry") from exc
    now = now or dt.datetime.now(dt.timezone.utc)
    if expiry <= now:
        raise Denied("receipt expired")
    if expiry - now > dt.timedelta(days=7):
        raise Denied("receipt retention window exceeds seven days")
    return expiry


def tree_manifest(tree_sha):
    require_hex(tree_sha, HEX40, "invalid source tree SHA")
    try:
        raw = subprocess.check_output(["git", "ls-tree", "-r", "-z", tree_sha])
    except (OSError, subprocess.CalledProcessError) as exc:
        raise Denied("source tree unavailable") from exc
    rows, total = [], 0
    for entry in raw.split(b"\0"):
        if not entry:
            continue
        try:
            meta, path_b = entry.split(b"\t", 1)
            mode_b, type_b, sha_b = meta.split(b" ", 2)
            path = path_b.decode("utf-8")
        except (ValueError, UnicodeDecodeError) as exc:
            raise Denied("malformed tree entry") from exc
        mode, kind, sha = mode_b.decode(), type_b.decode(), sha_b.decode()
        if kind != "blob" or mode not in ("100644", "100755"):
            raise Denied("unsupported Git entry: " + path)
        parts = PurePosixPath(path).parts
        if not parts or ".git" in parts or path.startswith("/") or ".." in parts:
            raise Denied("unsafe source path: " + path)
        size_text = git("cat-file", "-s", sha)
        try:
            size = int(size_text)
        except ValueError as exc:
            raise Denied("invalid blob size") from exc
        if size < 0 or size > MAX_FILE:
            raise Denied("per-file size bound")
        total += size
        if total > MAX_TOTAL:
            raise Denied("aggregate size bound")
        rows.append([path, mode, require_hex(sha, HEX40, "invalid blob SHA"), size])
        if len(rows) > MAX_FILES:
            raise Denied("file-count bound")
    if not rows:
        raise Denied("empty source tree")
    rows.sort(key=lambda row: row[0])
    return rows, total, canonical_digest(rows)


def verify(receipt_path, source_dir, workflow_path, repository, repository_id, head_sha, ref_name, now=None):
    if not ref_name.startswith("ci/oreslang-"):
        raise Denied("unexpected carrier branch")
    if not repository or "/" not in repository:
        raise Denied("destination repository context")
    try:
        repository_id = int(repository_id)
    except (TypeError, ValueError) as exc:
        raise Denied("destination repository id") from exc

    receipt_file = Path(receipt_path)
    if not receipt_file.is_file() or receipt_file.is_symlink():
        raise Denied("receipt missing or unsafe")
    try:
        receipt = json.loads(receipt_file.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise Denied("invalid receipt") from exc
    if not isinstance(receipt, dict) or receipt.get("schema") != "ores-private-ci-receipt-v3":
        raise Denied("receipt schema")
    parse_expiry(receipt.get("expires_at"), now=now)

    actual_head = require_hex(git("rev-parse", "HEAD"), HEX40, "invalid carrier head")
    if actual_head != head_sha:
        raise Denied("head/context mismatch")
    parent = require_hex(git("rev-parse", "HEAD^"), HEX40, "invalid carrier parent")

    destination = receipt.get("destination")
    if not isinstance(destination, dict):
        raise Denied("destination receipt")
    if destination.get("repo") != repository or destination.get("repo_id") != repository_id:
        raise Denied("destination identity")
    if destination.get("base_sha") != parent:
        raise Denied("destination base commit")

    changed_raw = subprocess.check_output(["git", "diff", "--name-only", "-z", parent, actual_head])
    changed = [p.decode("utf-8") for p in changed_raw.split(b"\0") if p]
    if receipt_path not in changed:
        raise Denied("receipt not committed with mirror")
    if not any(p.startswith(source_dir + "/") for p in changed):
        raise Denied("source subtree not changed")
    for path in changed:
        if path == receipt_path:
            continue
        if not path.startswith(source_dir + "/"):
            raise Denied("unexpected destination change: " + path)

    workflow_blob = require_hex(git("rev-parse", f"HEAD:{workflow_path}"), HEX40,
                               "carrier workflow blob")
    parent_workflow_blob = require_hex(git("rev-parse", f"HEAD^:{workflow_path}"), HEX40,
                                      "parent carrier workflow blob")
    if workflow_blob != parent_workflow_blob:
        raise Denied("carrier workflow changed in source-bearing commit")
    carrier = receipt.get("carrier")
    if not isinstance(carrier, dict) or carrier.get("workflow_blob_sha") != workflow_blob:
        raise Denied("carrier workflow receipt binding")

    source_tree = require_hex(git("rev-parse", f"HEAD:{source_dir}"), HEX40,
                              "source root tree")
    if ref_name != "ci/oreslang-" + source_tree:
        raise Denied("carrier branch/source tree mismatch")
    source = receipt.get("source")
    mirror = receipt.get("mirror")
    if not isinstance(source, dict) or not isinstance(mirror, dict):
        raise Denied("source/mirror receipt")
    if source.get("root_tree_sha") != source_tree or mirror.get("root_tree_sha") != source_tree:
        raise Denied("source root tree mismatch")
    if not isinstance(source.get("repo"), str) or "/" not in source["repo"]:
        raise Denied("source repository identity")
    if not isinstance(source.get("repo_id"), int) or source["repo_id"] <= 0:
        raise Denied("source repository id")
    if not isinstance(source.get("pr"), int) or source["pr"] <= 0:
        raise Denied("source PR number")
    require_hex(source.get("head_sha"), HEX40, "source head SHA")

    rows, total, manifest = tree_manifest(source_tree)
    if source.get("manifest_sha256") != manifest:
        raise Denied("manifest digest mismatch")

    test_plan = receipt.get("test_plan")
    if not isinstance(test_plan, dict):
        raise Denied("test plan receipt")
    plan_path = test_plan.get("path")
    if not isinstance(plan_path, str):
        raise Denied("test plan path")
    plan_parts = PurePosixPath(plan_path).parts
    if (not plan_path.startswith("ci/test-plans/")
            or not plan_path.endswith(".json")
            or not plan_parts
            or ".." in plan_parts
            or ".git" in plan_parts
            or "\\" in plan_path):
        raise Denied("unsafe test plan path")
    plan_file = Path(plan_path)
    if plan_file.is_symlink() or not plan_file.is_file():
        raise Denied("test plan missing or unsafe")
    plan_blob = require_hex(git("rev-parse", f"HEAD:{plan_path}"), HEX40,
                            "test plan blob")
    parent_plan_blob = require_hex(git("rev-parse", f"HEAD^:{plan_path}"), HEX40,
                                   "parent test plan blob")
    if plan_blob != parent_plan_blob:
        raise Denied("test plan changed in source-bearing commit")
    plan_bytes = plan_file.read_bytes()
    plan_digest = hashlib.sha256(plan_bytes).hexdigest()
    if test_plan.get("sha256") != plan_digest:
        raise Denied("test plan digest mismatch")
    try:
        plan_data = json.loads(plan_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise Denied("invalid test plan JSON") from exc
    if not isinstance(plan_data, dict) or plan_data.get("schema") != "ores-private-ci-test-plan-v1":
        raise Denied("test plan schema")
    if plan_data.get("source_repo") != source["repo"]:
        raise Denied("test plan source repository mismatch")
    allowed_prs = plan_data.get("allowed_prs")
    if (not isinstance(allowed_prs, list)
            or any(not isinstance(value, int) or isinstance(value, bool) or value <= 0
                   for value in allowed_prs)
            or source["pr"] not in allowed_prs):
        raise Denied("source PR not authorized by test plan")

    receipt_digest = canonical_digest(receipt)
    require_hex(receipt_digest, HEX64, "receipt digest")
    message_lines = git("log", "-1", "--format=%B").splitlines()
    if "Oreslang-CI-Receipt-SHA256: " + receipt_digest not in message_lines:
        raise Denied("commit/receipt digest binding")

    return {
        "source_repo": source["repo"],
        "source_pr": source["pr"],
        "source_head": source["head_sha"],
        "source_tree": source_tree,
        "file_count": len(rows),
        "byte_count": total,
        "manifest_sha256": manifest,
        "receipt_sha256": receipt_digest,
        "workflow_blob_sha": workflow_blob,
        "test_plan_path": plan_path,
        "test_plan_sha256": plan_digest,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", default="ci/oreslang-receipt.json")
    parser.add_argument("--source-dir", default="private-oreslang-source")
    parser.add_argument("--workflow", default=".github/workflows/oreslang-exact-tree-carrier.yml")
    args = parser.parse_args(argv)
    try:
        result = verify(
            args.receipt,
            args.source_dir,
            args.workflow,
            os.environ.get("GITHUB_REPOSITORY", ""),
            os.environ.get("GITHUB_REPOSITORY_ID", ""),
            os.environ.get("GITHUB_SHA", ""),
            os.environ.get("GITHUB_REF_NAME", ""),
        )
    except (Denied, OSError, subprocess.CalledProcessError) as exc:
        print("CARRIER_PROVENANCE_DENIED: " + str(exc), file=sys.stderr)
        return 1
    for key, value in result.items():
        print(f"{key.upper()}={value}")
    print("CARRIER_PROVENANCE=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
