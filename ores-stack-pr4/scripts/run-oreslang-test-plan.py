#!/usr/bin/env python3
"""Execute a reviewed Oreslang private-CI test plan without shell evaluation."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

MAX_COMMANDS = 16
MAX_ARGS = 64
MAX_ARG_LENGTH = 4096
MAX_TIMEOUT = 1800
MAX_LOG_CHARS = 64 * 1024
SAFE_ENV_KEYS = ("PATH", "HOME", "JAVA_HOME", "GRAALVM_HOME", "MAVEN_OPTS", "LANG", "LC_ALL")
SAFE_EXECUTABLE = re.compile(r"^(?:python3|mvn|cmake|ctest|g\+\+|clang\+\+|/tmp/ores-[A-Za-z0-9_.-]+)$")


class Denied(RuntimeError):
    pass


def safe_plan_path(value):
    if not isinstance(value, str):
        raise Denied("test plan path")
    parts = PurePosixPath(value).parts
    if (not value.startswith("ci/test-plans/")
            or not value.endswith(".json")
            or not parts
            or ".." in parts
            or ".git" in parts
            or "\\" in value):
        raise Denied("unsafe test plan path")
    return value


def load_context(receipt_path):
    try:
        receipt = json.loads(Path(receipt_path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise Denied("invalid receipt") from exc
    if not isinstance(receipt, dict) or receipt.get("schema") != "ores-private-ci-receipt-v3":
        raise Denied("receipt schema")
    source = receipt.get("source")
    test_plan = receipt.get("test_plan")
    if not isinstance(source, dict) or not isinstance(test_plan, dict):
        raise Denied("receipt source/test plan")
    plan_path = safe_plan_path(test_plan.get("path"))
    plan_file = Path(plan_path)
    if plan_file.is_symlink() or not plan_file.is_file():
        raise Denied("test plan missing or unsafe")
    try:
        plan_bytes = plan_file.read_bytes()
    except OSError as exc:
        raise Denied("test plan unreadable") from exc
    expected_digest = test_plan.get("sha256")
    actual_digest = hashlib.sha256(plan_bytes).hexdigest()
    if expected_digest != actual_digest:
        raise Denied("test plan digest mismatch")
    try:
        plan = json.loads(plan_bytes.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise Denied("invalid test plan") from exc
    if not isinstance(plan, dict) or plan.get("schema") != "ores-private-ci-test-plan-v1":
        raise Denied("test plan schema")
    if plan.get("source_repo") != source.get("repo"):
        raise Denied("test plan source repository mismatch")
    allowed = plan.get("allowed_prs")
    if (not isinstance(allowed, list)
            or source.get("pr") not in allowed
            or any(not isinstance(value, int) or isinstance(value, bool) or value <= 0
                   for value in allowed)):
        raise Denied("source PR not authorized by test plan")
    return receipt, plan


def confined_cwd(root, relative):
    if not isinstance(relative, str) or not relative:
        raise Denied("command cwd")
    if relative == ".":
        candidate = root
    else:
        parts = PurePosixPath(relative).parts
        if not parts or ".." in parts or ".git" in parts or "\\" in relative:
            raise Denied("unsafe command cwd")
        candidate = root.joinpath(*parts)
    try:
        root_real = root.resolve(strict=True)
        candidate_real = candidate.resolve(strict=True)
        candidate_real.relative_to(root_real)
    except (OSError, RuntimeError, ValueError) as exc:
        raise Denied("command cwd escapes mirrored source") from exc
    if not candidate_real.is_dir():
        raise Denied("command cwd is not a directory")
    return candidate_real


def validate_command(command, root):
    if not isinstance(command, dict):
        raise Denied("command entry")
    argv = command.get("argv")
    if (not isinstance(argv, list) or not argv or len(argv) > MAX_ARGS
            or any(not isinstance(arg, str) or not arg or len(arg) > MAX_ARG_LENGTH
                   or "\x00" in arg for arg in argv)):
        raise Denied("command argv")
    if not SAFE_EXECUTABLE.fullmatch(argv[0]):
        raise Denied("unapproved executable: " + argv[0])
    timeout = command.get("timeout_seconds")
    if (not isinstance(timeout, int) or isinstance(timeout, bool)
            or timeout < 1 or timeout > MAX_TIMEOUT):
        raise Denied("command timeout")
    cwd = confined_cwd(root, command.get("cwd", "."))
    expected = command.get("stdout_contains")
    if expected is not None and (not isinstance(expected, str)
                                 or not expected
                                 or len(expected) > 4096):
        raise Denied("stdout assertion")
    return argv, cwd, timeout, expected


def run_plan(receipt_path="ci/oreslang-receipt.json",
             source_dir="private-oreslang-source"):
    receipt, plan = load_context(receipt_path)
    commands = plan.get("commands")
    if not isinstance(commands, list) or not commands or len(commands) > MAX_COMMANDS:
        raise Denied("test plan command count")
    source_root = Path(source_dir)
    if source_root.is_symlink() or not source_root.is_dir():
        raise Denied("mirrored source directory missing or unsafe")
    source_root = source_root.resolve(strict=True)

    env = {key: os.environ[key] for key in SAFE_ENV_KEYS if key in os.environ}
    env.update({"CI": "true", "NO_COLOR": "1"})
    results = []
    for index, command in enumerate(commands, start=1):
        argv, cwd, timeout, expected = validate_command(command, source_root)
        print(f"PLAN_COMMAND_{index}_START executable={argv[0]} cwd={cwd.relative_to(source_root)}")
        try:
            completed = subprocess.run(
                argv,
                cwd=cwd,
                env=env,
                text=True,
                capture_output=True,
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise Denied(f"command {index} unavailable or timed out") from exc
        stdout = completed.stdout[-MAX_LOG_CHARS:]
        stderr = completed.stderr[-MAX_LOG_CHARS:]
        if stdout:
            print(stdout, end="" if stdout.endswith("\n") else "\n")
        if stderr:
            print(stderr, file=sys.stderr, end="" if stderr.endswith("\n") else "\n")
        if completed.returncode != 0:
            raise Denied(f"command {index} failed with exit {completed.returncode}")
        if expected is not None and expected not in completed.stdout:
            raise Denied(f"command {index} missing required stdout evidence")
        print(f"PLAN_COMMAND_{index}_PASS")
        results.append({"index": index, "argv": argv, "returncode": completed.returncode})
    print(f"TEST_PLAN_PASS commands={len(results)} source={receipt['source']['repo']} pr={receipt['source']['pr']}")
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", default="ci/oreslang-receipt.json")
    parser.add_argument("--source-dir", default="private-oreslang-source")
    args = parser.parse_args(argv)
    try:
        run_plan(args.receipt, args.source_dir)
    except Denied as exc:
        print("TEST_PLAN_DENIED: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
