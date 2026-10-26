#!/usr/bin/env python3
"""Fail closed unless the live private carrier main still protects the source-bearing base."""
import argparse
import json
from pathlib import Path
import re
import sys

HEX40 = re.compile(r"^[0-9a-f]{40}$")
MAX_JSON = 1024 * 1024

class Denied(RuntimeError):
    pass

def unique_object(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise Denied("duplicate JSON key")
        out[key] = value
    return out

def load_json(path, label):
    try:
        raw = Path(path).read_bytes()
    except OSError as exc:
        raise Denied(label + " unavailable") from exc
    if not raw or len(raw) > MAX_JSON or raw.startswith(b"\xef\xbb\xbf"):
        raise Denied(label + " invalid")
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
    except (UnicodeError, ValueError, RecursionError, Denied) as exc:
        raise Denied(label + " invalid") from exc
    if not isinstance(value, dict):
        raise Denied(label + " invalid")
    return value

def sha(value, label):
    if not isinstance(value, str) or not HEX40.fullmatch(value):
        raise Denied(label)
    return value

def check(repo, branch, commit, expected_repo, expected_repo_id, head_sha):
    head_sha = sha(head_sha, "carrier head SHA invalid")
    if (repo.get("id") != expected_repo_id
            or repo.get("full_name") != expected_repo
            or repo.get("private") is not True
            or repo.get("visibility") != "private"
            or repo.get("default_branch") != "main"
            or repo.get("archived") is not False
            or repo.get("disabled") is not False
            or repo.get("fork") is not False):
        raise Denied("carrier identity/privacy no longer eligible")

    if branch.get("name") != "main" or branch.get("protected") is not True:
        raise Denied("carrier main is not protected")
    branch_commit = branch.get("commit")
    if not isinstance(branch_commit, dict):
        raise Denied("carrier main commit metadata invalid")
    main_sha = sha(branch_commit.get("sha"), "carrier main SHA invalid")

    if commit.get("sha") != head_sha:
        raise Denied("source-bearing commit identity changed")
    parents = commit.get("parents")
    if not isinstance(parents, list) or len(parents) != 1:
        raise Denied("source-bearing commit must have exactly one parent")
    parent = parents[0]
    if not isinstance(parent, dict):
        raise Denied("source-bearing parent metadata invalid")
    parent_sha = sha(parent.get("sha"), "source-bearing parent SHA invalid")
    if parent_sha != main_sha:
        raise Denied("carrier main moved or no longer matches source-bearing base")
    return main_sha

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-json", required=True)
    parser.add_argument("--branch-json", required=True)
    parser.add_argument("--commit-json", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--repository-id", required=True, type=int)
    parser.add_argument("--head-sha", required=True)
    args = parser.parse_args(argv)
    try:
        base = check(
            load_json(args.repo_json, "carrier repository metadata"),
            load_json(args.branch_json, "carrier main metadata"),
            load_json(args.commit_json, "source-bearing commit metadata"),
            args.repository,
            args.repository_id,
            args.head_sha,
        )
    except Denied as exc:
        print("CARRIER_MAIN_POLICY_DENIED: " + str(exc), file=sys.stderr)
        return 1
    print("CARRIER_MAIN_POLICY_PASS base_sha=" + base)
    return 0

if __name__ == "__main__":
    sys.exit(main())
