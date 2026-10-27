#!/usr/bin/env python3
"""Publish a snapshot branch ONLY into a reviewed private GitHub repository.

No arbitrary command execution or user-selectable GitHub API endpoints. All
writes target the exact repository checked by the private-source policy.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import stat
from pathlib import Path
import re
import subprocess
import sys

NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*$")
APPROVAL = re.compile(r"^([A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*) ([1-9][0-9]*)$")
BRANCH = re.compile(r"^ci/oreslang-[A-Za-z0-9][A-Za-z0-9_.-]{0,70}$")
SHA = re.compile(r"^[a-f0-9]{40}$")
MAX_FILES = 1024
MAX_BYTES = 40 * 1024 * 1024
MAX_SINGLE = 10 * 1024 * 1024
MAX_DIRECTORY_ENTRIES = 4096
MAX_DIRECTORIES = 1024
MAX_GH_API_RESPONSE_CHARS = 24 * 1024 * 1024
PREFIX = "private-oreslang-source"
SOURCE_REPO = "ores-truffle-oreslang/oreslang-source.java"
SOURCE_REPOSITORY_ID = 1400792274
BLOCKING_INCIDENT_REPO = "ores-truffle-oreslang/oreslang-platform-conformance"
BLOCKING_INCIDENT_REPOSITORY_ID = 1413795502
BLOCKING_INCIDENT_IDS = {
    22: 5832921751,
    77: 5904679716,
    127: 5935338521,
    171: 6042555082,  # recovery/runtime source-bearing private carrier; pending classification
    172: 6042602777,  # LLVM exact-tree carrier; independent placement review outstanding
}
BLOCKING_INCIDENTS = (22, 77, 127, 171, 172)
QUARANTINED_DESTINATIONS = (
    "zed-pkg-test/java-app",
    "agent-pontifex-test/agent-pontifex-e2e-tests",
    "led-dynamo/leddy-e2e",
)
QUARANTINED_DESTINATION_IDS = (1323575966,)
SOURCE_PROTECTED_BRANCH = "main"
POLICY_PATH = ".github/private-ci-approved-repositories.txt"
CODEOWNERS_PATH = ".github/CODEOWNERS"
SECURITY_CODEOWNER_PATHS = (
    "/.github/CODEOWNERS",
    "/.github/private-ci-approved-repositories.txt",
    "/.github/private-ci-carrier-contracts.json",
    "/scripts/check-private-ci-destination.sh",
    "/scripts/run-in-private-ci-destination.sh",
    "/scripts/publish-private-ci-source.py",
    "/scripts/publish-private-ci-source-v3.py",
)
SECURITY_CODEOWNER = "@ORESoftware"
GITHUB_ACTIONS_APP_ID = 15368
SOURCE_POLICY_CHECK = "preflight-regressions"


class Denied(RuntimeError):
    pass


def safe_descriptor_flags_available():
    """Require distinct positive descriptor-safety flag bits."""
    flags = tuple(
        getattr(os, name, None)
        for name in ("O_DIRECTORY", "O_NOFOLLOW", "O_NONBLOCK")
    )
    if not all(type(value) is int and value > 0 for value in flags):
        return False
    return all(
        (left & right) == 0
        for index, left in enumerate(flags)
        for right in flags[index + 1:]
    )


def trusted_github_cli():
    """Resolve GitHub API access only through the root-controlled OS binary."""
    target = "/usr/bin/gh"
    for directory in ("/", "/usr", "/usr/bin"):
        try:
            info = os.lstat(directory)
        except OSError as exc:
            raise Denied("trusted GitHub CLI directory is unavailable") from exc
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != 0
                or info.st_mode & 0o022):
            raise Denied("trusted GitHub CLI directory is not root-controlled")
    try:
        info = os.lstat(target)
    except OSError as exc:
        raise Denied("trusted GitHub CLI is unavailable") from exc
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0
            or info.st_mode & 0o6022 or not info.st_mode & 0o111):
        raise Denied("trusted GitHub CLI executable is not root-controlled")
    return target


def github_api_environment():
    """Use exactly one explicit token and no ambient transport/config state."""
    provided = [
        name for name in ("GH_TOKEN", "GITHUB_TOKEN")
        if os.environ.get(name)
    ]
    if len(provided) != 1:
        raise Denied("exactly one explicit GitHub API token is required")
    return {
        provided[0]: os.environ[provided[0]],
        "GH_HOST": "github.com",
        "GH_PROMPT_DISABLED": "1",
        "PATH": "/usr/bin:/bin",
        "HOME": "/nonexistent",
        "XDG_CONFIG_HOME": "/nonexistent",
        "LC_ALL": "C.UTF-8",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": "/dev/null",
    }


def strict_json_loads(text, label="invalid GitHub API JSON response"):
    """Reject duplicate keys and non-finite JSON numbers in security metadata."""
    if not isinstance(text, str):
        raise Denied(label)

    def unique_fields(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise Denied(label + " contains duplicate fields")
            result[key] = value
        return result

    def reject_constant(_value):
        raise Denied(label + " contains nonfinite JSON constant")

    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            raise Denied(label + " contains nonfinite JSON number")
        return number

    try:
        return json.loads(
            text,
            object_pairs_hook=unique_fields,
            parse_constant=reject_constant,
            parse_float=finite_float,
        )
    except (json.JSONDecodeError, RecursionError, OverflowError, ValueError) as exc:
        if isinstance(exc, Denied):
            raise
        raise Denied(label) from exc


def canonical_security_lines(contents, label):
    """Accept only printable ASCII records separated by LF."""
    if (not isinstance(contents, bytes)
            or any(byte != 10 and not 32 <= byte <= 126 for byte in contents)):
        raise Denied(label + " contains noncanonical ASCII LF bytes")
    return contents.decode("ascii").split("\n")


def require_security_codeowners(contents):
    """Require terminal exact rules owned by an explicit Oreslang team."""
    if not isinstance(contents, bytes) or not contents or len(contents) > 16 * 1024:
        raise Denied("protected CODEOWNERS is missing or oversized")
    lines = canonical_security_lines(contents, "protected CODEOWNERS")
    rules = [line.strip() for line in lines
             if line.strip() and not line.lstrip().startswith("#")]
    if len(rules) < len(SECURITY_CODEOWNER_PATHS):
        raise Denied("protected CODEOWNERS lacks terminal export rules")
    tail = rules[-len(SECURITY_CODEOWNER_PATHS):]
    for expected_path, rule in zip(SECURITY_CODEOWNER_PATHS, tail):
        fields = rule.split()
        if fields != [expected_path, SECURITY_CODEOWNER]:
            raise Denied(
                "protected CODEOWNERS must end with terminal exact export rules solely owned by the independent security owner"
            )


def valid_sha(value):
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise Denied("GitHub returned an invalid object SHA")
    return value


def gh_api(route, method="GET", payload=None):
    """Authenticated GitHub API through a trusted executable and sealed environment."""
    argv = [trusted_github_cli(), "api", "--method", method, route,
            "--header", "Accept: application/vnd.github+json"]
    if payload is not None:
        argv.extend(["--input", "-"])
    try:
        p = subprocess.run(
            argv,
            input=json.dumps(payload) if payload is not None else None,
            capture_output=True,
            text=True,
            env=github_api_environment(),
            timeout=45,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise Denied("authenticated GitHub API request unavailable") from exc
    if (not isinstance(p.stdout, str)
            or len(p.stdout) > MAX_GH_API_RESPONSE_CHARS
            or not isinstance(p.stderr, str)
            or len(p.stderr) > MAX_GH_API_RESPONSE_CHARS):
        raise Denied("authenticated GitHub API response exceeds size limit")
    if p.returncode != 0:
        # Never echo stderr: a client can include private file bytes or credentials.
        raise Denied("authenticated GitHub API request failed")
    data = strict_json_loads(p.stdout)
    if not isinstance(data, dict):
        raise Denied("unexpected GitHub response shape")
    return data


def checked_repo(repository, expected_repository_id=None):
    result = gh_api("repos/" + repository)
    if (not isinstance(result.get("full_name"), str)
            or result["full_name"].casefold() != repository.casefold()
            or result.get("visibility") != "private"
            or result.get("private") is not True
            or result.get("archived") is not False
            or result.get("disabled") is not False
            or result.get("fork") is not False
            or result.get("default_branch") != "main"):
        raise Denied("destination identity/state or private visibility changed")
    repository_id = result.get("id")
    if type(repository_id) is not int or isinstance(repository_id, bool) or repository_id <= 0:
        raise Denied("destination numeric repository ID is invalid")
    if repository_id in QUARANTINED_DESTINATION_IDS:
        raise Denied("destination repository ID is quarantined by private-source incident policy")
    if expected_repository_id is not None:
        if (type(expected_repository_id) is not int
                or isinstance(expected_repository_id, bool)
                or expected_repository_id <= 0
                or repository_id != expected_repository_id):
            raise Denied("destination numeric repository ID differs from reviewed approval")
    return result


def require_resolved_blocking_incidents():
    """Fail closed until the independently tracked P0 export incidents are resolved."""
    repo = gh_api("repos/" + BLOCKING_INCIDENT_REPO)
    if (repo.get("id") != BLOCKING_INCIDENT_REPOSITORY_ID
            or repo.get("full_name") != BLOCKING_INCIDENT_REPO
            or repo.get("private") is not True
            or repo.get("visibility") != "private"
            or repo.get("archived") is not False
            or repo.get("disabled") is not False
            or repo.get("fork") is not False):
        raise Denied("blocking incident repository identity/state invalid")
    expected_repository_url = "https://api.github.com/repos/" + BLOCKING_INCIDENT_REPO
    for number in BLOCKING_INCIDENTS:
        issue = gh_api(
            "repos/" + BLOCKING_INCIDENT_REPO + "/issues/" + str(number)
        )
        expected_issue_url = expected_repository_url + "/issues/" + str(number)
        expected_html_url = (
            "https://github.com/" + BLOCKING_INCIDENT_REPO + "/issues/" + str(number)
        )
        if (issue.get("id") != BLOCKING_INCIDENT_IDS[number]
                or issue.get("number") != number
                or issue.get("repository_url") != expected_repository_url
                or issue.get("url") != expected_issue_url
                or issue.get("html_url") != expected_html_url
                or "pull_request" in issue):
            raise Denied("blocking P0 incident identity invalid")
        if (issue.get("state") != "closed"
                or issue.get("state_reason") != "completed"):
            raise Denied(
                "blocking P0 incident #" + str(number) + " remains unresolved"
            )


def require_preflight(repository, root):
    """Authorize without executing code from the candidate checkout.

    The Bash destination checker remains a human/CI diagnostic, but a
    privileged publisher must not execute checkout-controlled shell before
    protected-origin authorization. Read the reviewed policy as data through
    descriptor-safe Python and bind the destination's immutable repository ID.
    """
    if repository.casefold() in {item.casefold() for item in QUARANTINED_DESTINATIONS}:
        raise Denied("destination is quarantined by private-source incident policy")
    require_resolved_blocking_incidents()
    _, approved_repository_id = checked_policy_approval(root, repository)
    checked_repo(repository, approved_repository_id)

def _parse_policy_approval(contents, repository, label):
    if not isinstance(contents, bytes):
        raise Denied(label + " has invalid contents")
    lines = canonical_security_lines(contents, label)
    approved_id = None
    seen = set()
    for line in lines:
        if not line or line.startswith("#"):
            continue
        match = APPROVAL.fullmatch(line)
        if match is None:
            raise Denied(label + " requires exact owner/repo numeric-id pairs")
        slug, numeric_id = match.groups()
        candidate_id = int(numeric_id)
        if candidate_id in QUARANTINED_DESTINATION_IDS:
            raise Denied("destination repository ID is quarantined by private-source incident policy")
        key = slug.casefold()
        if key in seen:
            raise Denied(label + " contains duplicate repository identities")
        seen.add(key)
        if key == repository.casefold():
            approved_id = candidate_id
    if approved_id is None:
        raise Denied("destination approval was revoked or absent")
    return approved_id


def checked_policy_approval(root, repository):
    """Return the exact policy digest and immutable approved repository ID."""
    if not safe_descriptor_flags_available():
        raise Denied("OS cannot safely read the destination approval policy")
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    policy_flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
    try:
        root_fd = os.open(root, directory_flags)
        try:
            policy_dir_fd = os.open(".github", directory_flags, dir_fd=root_fd)
            try:
                policy_fd = os.open("private-ci-approved-repositories.txt",
                                    policy_flags, dir_fd=policy_dir_fd)
                try:
                    stream = os.fdopen(policy_fd, "rb")
                except BaseException:
                    os.close(policy_fd)
                    raise
                with stream:
                    details = os.fstat(stream.fileno())
                    if not stat.S_ISREG(details.st_mode) or details.st_nlink != 1:
                        raise Denied("private CI approval policy is not a regular single-link file")
                    contents = stream.read(16 * 1024 + 1)
            finally:
                os.close(policy_dir_fd)
        finally:
            os.close(root_fd)
    except OSError as exc:
        raise Denied("private CI approval policy cannot be opened safely") from exc
    if len(contents) > 16 * 1024:
        raise Denied("private CI approval policy exceeds size limit")
    approved_id = _parse_policy_approval(
        contents, repository, "private CI approval policy"
    )
    return hashlib.sha256(contents).digest(), approved_id


def checked_policy_fingerprint(root, repository):
    """Compatibility wrapper returning the reviewed policy fingerprint."""
    return checked_policy_approval(root, repository)[0]

def require_issuer_bound_actions_checks(required_checks, label, expected_context):
    """Require every mandatory CI context to be GitHub-Actions-bound and pin the security check."""
    if (not isinstance(expected_context, str)
            or not expected_context.strip()):
        raise Denied(label + " expected required CI context invalid")
    if not isinstance(required_checks, dict):
        raise Denied(label + " required status-check policy missing")
    contexts = required_checks.get("contexts", [])
    named_checks = required_checks.get("checks")
    if (not isinstance(contexts, list)
            or any(not isinstance(name, str) or not name.strip()
                   for name in contexts)
            or len(contexts) != len(set(contexts))):
        raise Denied(label + " legacy required status checks are malformed")
    if not isinstance(named_checks, list) or not named_checks:
        raise Denied(label + " required CI issuer binding missing")

    seen = set()
    bound = []
    for item in named_checks:
        if not isinstance(item, dict):
            raise Denied(label + " required CI check metadata malformed")
        name = item.get("context")
        app_id = item.get("app_id")
        if (not isinstance(name, str) or not name.strip() or name in seen):
            raise Denied(label + " required CI check identity is ambiguous")
        if (type(app_id) is not int
                or app_id != GITHUB_ACTIONS_APP_ID):
            raise Denied(label + " required CI check has untrusted issuer")
        seen.add(name)
        bound.append((name, app_id))

    if any(name not in seen for name in contexts):
        raise Denied(label + " legacy required status check lacks issuer binding")
    if expected_context not in seen:
        raise Denied(label + " required security CI check missing")
    return tuple(sorted(contexts)), tuple(sorted(bound))


def _require_strict_source_protection(protection):
    """Validate the security-relevant classic branch-protection contract."""
    reviews = protection.get("required_pull_request_reviews")
    checks = protection.get("required_status_checks")
    administrators = protection.get("enforce_admins")
    force_pushes = protection.get("allow_force_pushes")
    deletions = protection.get("allow_deletions")
    conversations = protection.get("required_conversation_resolution")
    if (not isinstance(reviews, dict)
            or reviews.get("require_code_owner_reviews") is not True
            or reviews.get("dismiss_stale_reviews") is not True
            or reviews.get("require_last_push_approval") is not True
            or not isinstance(reviews.get("required_approving_review_count"), int)
            or isinstance(reviews["required_approving_review_count"], bool)
            or reviews["required_approving_review_count"] < 1
            or not isinstance(checks, dict)
            or checks.get("strict") is not True
            or not isinstance(administrators, dict)
            or administrators.get("enabled") is not True
            or not isinstance(force_pushes, dict)
            or force_pushes.get("enabled") is not False
            or not isinstance(deletions, dict)
            or deletions.get("enabled") is not False
            or not isinstance(conversations, dict)
            or conversations.get("enabled") is not True):
        raise Denied(
            "origin main must enforce CODEOWNER approvals, stale-review dismissal, "
            "last-push approval, conversation resolution, strict required CI, "
            "administrator protection and deny forced pushes/deletions")
    contexts, named_contract = require_issuer_bound_actions_checks(
        checks, "origin main", SOURCE_POLICY_CHECK
    )
    bypasses = reviews.get("bypass_pull_request_allowances")
    if bypasses is not None:
        if (not isinstance(bypasses, dict)
                or any(not isinstance(bypasses.get(category, []), list)
                       or bypasses.get(category)
                       for category in ("users", "teams", "apps"))):
            raise Denied("origin main has PR review bypass allowances")
    return (
        reviews["require_code_owner_reviews"],
        reviews["dismiss_stale_reviews"],
        reviews["require_last_push_approval"],
        reviews["required_approving_review_count"],
        conversations["enabled"],
        checks["strict"],
        contexts,
        named_contract,
        administrators["enabled"],
        force_pushes["enabled"],
        deletions["enabled"],
    )


def verify_protected_origin(repository, expected_policy_digest, expected_repository_id=None):
    """Fail closed unless local policy matches CODEOWNER-controlled protected main.

    An unreviewed test checkout can add its own allowlist line; consulting
    only the local file is not authorization. Always pin the remote policy
    lookup to the HEAD commit of the official protected origin/main. A
    GitHub "protected: false" branch is explicitly insufficient even when
    CODEOWNERS exists (without branch protection, its review is optional).
    The branch head and security-relevant protection contract are re-read
    before success so movement or weakening during attestation fails closed.
    """
    origin = gh_api(f"repos/{SOURCE_REPO}")
    if (origin.get("full_name") != SOURCE_REPO
            or origin.get("id") != SOURCE_REPOSITORY_ID
            or origin.get("private") is not True):
        raise Denied("cannot attest identity and privacy of source origin")
    branch = gh_api(f"repos/{SOURCE_REPO}/branches/{SOURCE_PROTECTED_BRANCH}")
    if (branch.get("name") != SOURCE_PROTECTED_BRANCH
            or branch.get("protected") is not True):
        raise Denied("source main is not protected; CODEOWNER approval is unenforced")
    # A "protected" branch might enforce only status checks, without
    # mandatory CODEOWNER approval, or allow administrators to bypass review.
    # Require the actual classic branch-protection settings to be readable.
    # On plans/credentials where GitHub denies this API, fail closed.
    protection = gh_api(
        f"repos/{SOURCE_REPO}/branches/{SOURCE_PROTECTED_BRANCH}/protection")
    protection_contract = _require_strict_source_protection(protection)
    commit_sha = valid_sha(branch.get("commit", {}).get("sha"))
    policy = gh_api(f"repos/{SOURCE_REPO}/contents/{POLICY_PATH}?ref={commit_sha}")
    if (policy.get("path") != POLICY_PATH or policy.get("type") != "file"
            or policy.get("encoding") != "base64"):
        raise Denied("protected origin approval policy has invalid metadata")
    encoded = policy.get("content")
    if not isinstance(encoded, str):
        raise Denied("protected origin approval policy has no content")
    try:
        # Whitespace may separate base64 lines in GitHub Contents responses,
        # but any non-whitespace/non-base64 character must be rejected.
        packed = "".join(encoded.split())
        remote_contents = base64.b64decode(packed, validate=True)
    except (ValueError, base64.binascii.Error) as exc:
        raise Denied("protected origin approval policy has invalid base64") from exc
    if len(remote_contents) > 16 * 1024:
        raise Denied("protected origin approval policy exceeds size limit")
    if hashlib.sha256(remote_contents).digest() != expected_policy_digest:
        raise Denied("source policy diverges from reviewed protected main")
    remote_repository_id = _parse_policy_approval(
        remote_contents, repository, "protected origin approval policy"
    )
    if (expected_repository_id is not None
            and remote_repository_id != expected_repository_id):
        raise Denied("protected origin repository ID differs from local reviewed approval")

    codeowners = gh_api(
        f"repos/{SOURCE_REPO}/contents/{CODEOWNERS_PATH}?ref={commit_sha}"
    )
    if (codeowners.get("path") != CODEOWNERS_PATH
            or codeowners.get("type") != "file"
            or codeowners.get("encoding") != "base64"):
        raise Denied("protected CODEOWNERS has invalid metadata")
    codeowners_encoded = codeowners.get("content")
    if not isinstance(codeowners_encoded, str):
        raise Denied("protected CODEOWNERS has no content")
    try:
        codeowners_contents = base64.b64decode(
            "".join(codeowners_encoded.split()), validate=True
        )
    except (ValueError, base64.binascii.Error) as exc:
        raise Denied("protected CODEOWNERS has invalid base64") from exc
    require_security_codeowners(codeowners_contents)

    # Close the attestation-internal TOCTOU window: main or its enforcement
    # may change while the pinned policy/CODEOWNERS objects are being read.
    final_branch = gh_api(
        f"repos/{SOURCE_REPO}/branches/{SOURCE_PROTECTED_BRANCH}")
    if (final_branch.get("name") != SOURCE_PROTECTED_BRANCH
            or final_branch.get("protected") is not True
            or valid_sha(final_branch.get("commit", {}).get("sha")) != commit_sha):
        raise Denied("source main changed during protected-origin attestation")
    final_protection = gh_api(
        f"repos/{SOURCE_REPO}/branches/{SOURCE_PROTECTED_BRANCH}/protection")
    if _require_strict_source_protection(final_protection) != protection_contract:
        raise Denied("source main protection changed during protected-origin attestation")
    return commit_sha


def collect_source(root, trusted_root):
    """Snapshot regular files using descriptor-relative, no-follow traversal.

    O_NOFOLLOW on the final file is insufficient when a parent directory can
    be replaced with a symlink between path validation and file open. All
    descendants are opened relative to directory descriptors anchored at
    the trusted checkout, so a rename cannot redirect them outside it.
    """
    if not safe_descriptor_flags_available():
        raise Denied("OS cannot enforce descriptor-relative no-follow reads")
    if root.is_symlink() or not root.is_dir():
        raise Denied("source path must be a real directory")
    try:
        trusted = trusted_root.resolve(strict=True)
        source = root.resolve(strict=True)
        relative = source.relative_to(trusted)
    except (OSError, RuntimeError, ValueError) as exc:
        raise Denied("source directory escapes trusted checkout") from exc

    directories = []
    entries = []
    total = 0
    visited = 0
    entered_dirs = 0
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    file_flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK

    def walk(directory_fd, prefix, depth):
        nonlocal total, visited, entered_dirs
        entered_dirs += 1
        if depth > 64 or entered_dirs > MAX_DIRECTORIES:
            raise Denied("source directory nesting or count exceeds safety limit")
        # scandir() can stream entries from an opened descriptor. Do not use
        # os.listdir(), which materializes an unbounded directory before any
        # quota can be checked.
        names = []
        try:
            with os.scandir(directory_fd) as scan:
                for entry in scan:
                    visited += 1
                    if visited > MAX_DIRECTORY_ENTRIES:
                        raise Denied("source directory entry count exceeds safety limit")
                    names.append(entry.name)
        except OSError as exc:
            raise Denied("source directory cannot be listed safely") from exc
        names.sort()
        for name in names:
            if (name in (".", "..", ".git")
                    or any(ord(ch) < 32 or ord(ch) == 127 for ch in name)
                    or "\\" in name):
                raise Denied("source path contains unsupported name")
            rel = prefix + name
            try:
                metadata = os.stat(name, dir_fd=directory_fd,
                                   follow_symlinks=False)
                if stat.S_ISDIR(metadata.st_mode):
                    child_fd = os.open(name, directory_flags, dir_fd=directory_fd)
                    try:
                        if not stat.S_ISDIR(os.fstat(child_fd).st_mode):
                            raise Denied("source directory changed during traversal")
                        walk(child_fd, rel + "/", depth + 1)
                    finally:
                        os.close(child_fd)
                elif stat.S_ISREG(metadata.st_mode):
                    file_fd = os.open(name, file_flags, dir_fd=directory_fd)
                    try:
                        stream = os.fdopen(file_fd, "rb")
                    except BaseException:
                        os.close(file_fd)
                        raise
                    with stream:
                        metadata = os.fstat(stream.fileno())
                        if not stat.S_ISREG(metadata.st_mode):
                            raise Denied("source file changed type during traversal")
                        # A hardlink may refer to outside-checkout bytes.
                        if metadata.st_nlink != 1:
                            raise Denied("hardlinked source files are not allowed")
                        if metadata.st_size > MAX_SINGLE:
                            raise Denied("file exceeds source mirror limit")
                        data = stream.read(MAX_SINGLE + 1)
                    if len(data) > MAX_SINGLE:
                        raise Denied("file exceeds source mirror limit")
                    total += len(data)
                    if total > MAX_BYTES or len(entries) >= MAX_FILES:
                        raise Denied("source mirror exceeds bounded capacity")
                    mode = "100755" if metadata.st_mode & stat.S_IXUSR else "100644"
                    entries.append((rel, data, mode))
                else:
                    raise Denied("source contains symlink or special file")
            except OSError as exc:
                raise Denied("source path changed or cannot be opened safely") from exc

    try:
        anchor_fd = os.open(trusted, directory_flags)
        directories.append(anchor_fd)
        for component in relative.parts:
            child_fd = os.open(component, directory_flags,
                               dir_fd=directories[-1])
            directories.append(child_fd)
        walk(directories[-1], "", 0)
    except OSError as exc:
        raise Denied("source checkout path changed during traversal") from exc
    finally:
        for fd in reversed(directories):
            os.close(fd)
    if not entries:
        raise Denied("source directory is empty")
    return sorted(entries, key=lambda item: item[0])


def verified_exact_source_tree(files, source_commit):
    """Match every local upload blob/mode/path to a pinned origin commit tree.

    Source-dir scanning alone is not a Git-tree snapshot: dirty/untracked
    bytes could be exported under a valid destination approval. Require a
    full (non-truncated) authoritative tree at an exact 40-hex commit SHA
    before uploading anything. Git tree entries also reject submodules and
    symlinks even if a caller's local snapshot omits them.
    """
    source_commit = valid_sha(source_commit)
    commit = gh_api(f"repos/{SOURCE_REPO}/git/commits/{source_commit}")
    if commit.get("sha") != source_commit:
        raise Denied("source commit SHA is not attested by origin")
    tree_sha = valid_sha(commit.get("tree", {}).get("sha"))
    source_tree = gh_api(
        f"repos/{SOURCE_REPO}/git/trees/{tree_sha}?recursive=1")
    if source_tree.get("sha") != tree_sha or source_tree.get("truncated") is not False:
        raise Denied("source Git tree is incomplete or its SHA changed")
    raw_entries = source_tree.get("tree")
    if not isinstance(raw_entries, list) or len(raw_entries) > 4096:
        raise Denied("source Git tree entries missing or over safety limit")
    tracked = {}
    for entry in raw_entries:
        if not isinstance(entry, dict):
            raise Denied("malformed source Git tree entry")
        kind = entry.get("type")
        if kind == "tree":
            continue
        if kind != "blob" or entry.get("mode") not in ("100644", "100755"):
            raise Denied("source Git tree contains symlink or unsupported file mode")
        path = entry.get("path")
        if not isinstance(path, str) or not path or path in tracked:
            raise Denied("malformed/duplicate path in source Git tree")
        tracked[path] = (entry["mode"], valid_sha(entry.get("sha")))
    local = {}
    for path, data, mode in files:
        expected_blob = hashlib.sha1(
            b"blob " + str(len(data)).encode("ascii") + bytes([0]) + data
        ).hexdigest()
        if path in local:
            raise Denied("duplicate local source path")
        local[path] = (mode, expected_blob)
    if not local or local != tracked:
        raise Denied("source directory differs from pinned Git tree")
    return tree_sha


def publish(repository, source, branch, trusted_root, source_commit):
    if not NAME.fullmatch(repository) or not BRANCH.fullmatch(branch):
        raise Denied("invalid destination or isolated CI branch name")
    source_commit = valid_sha(source_commit)
    # Approval comes before reading potentially sensitive source files.
    require_preflight(repository, trusted_root)
    approved_policy, approved_repository_id = checked_policy_approval(
        trusted_root, repository
    )
    # Must check the GitHub origin's protected main before reading private bytes.
    verify_protected_origin(
        repository, approved_policy, approved_repository_id
    )
    trusted = trusted_root.resolve(strict=True)
    checked_source = source.resolve(strict=True)
    try:
        checked_source.relative_to(trusted)
    except ValueError as exc:
        raise Denied("source directory escapes the trusted checkout") from exc
    files = collect_source(source, trusted_root)
    expected_source_tree = verified_exact_source_tree(files, source_commit)
    base = checked_repo(repository, approved_repository_id)
    default = base.get("default_branch")
    if not isinstance(default, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,80}", default):
        raise Denied("destination default branch is invalid")
    base_ref = gh_api(f"repos/{repository}/git/ref/heads/{default}")
    base_sha = valid_sha(base_ref.get("object", {}).get("sha"))
    base_commit = gh_api(f"repos/{repository}/git/commits/{base_sha}")
    base_tree = valid_sha(base_commit.get("tree", {}).get("sha"))
    def authorize_write():
        # Every authorization input is mutable after preflight. Re-attest the
        # complete protected source authority before each source-bearing write.
        require_resolved_blocking_incidents()
        if checked_policy_approval(trusted_root, repository) != (
                approved_policy, approved_repository_id):
            raise Denied("private CI destination approval policy changed during transfer")
        checked_repo(repository, approved_repository_id)
        verify_protected_origin(
            repository, approved_policy, approved_repository_id
        )

    elements = []
    for rel, data, mode in files:
        authorize_write()  # recheck approval and identity before each write
        blob = gh_api(f"repos/{repository}/git/blobs", method="POST", payload={
            "content": base64.b64encode(data).decode("ascii"), "encoding": "base64"
        })
        blob_sha = valid_sha(blob.get("sha"))
        expected = hashlib.sha1(b"blob " + str(len(data)).encode("ascii")
                                + bytes([0]) + data).hexdigest()
        if blob_sha != expected:
            raise Denied("uploaded blob digest does not match local source bytes")
        elements.append({"path": rel, "mode": mode,
                         "type": "blob", "sha": blob_sha})
    # Construct the source subtree from scratch, without base_tree. Overlaying
    # PREFIX/file onto the destination base tree retains old private files
    # that are no longer present in this snapshot.
    authorize_write()
    isolated = gh_api(f"repos/{repository}/git/trees", method="POST",
                      payload={"tree": elements})
    isolated_sha = valid_sha(isolated.get("sha"))
    if isolated_sha != expected_source_tree:
        raise Denied("uploaded source tree differs from pinned Git commit")
    # Replace the entire PREFIX subtree atomically in the new root tree.
    # Preserve unrelated destination files and CI workflows from base_tree.
    authorize_write()
    new_tree = gh_api(f"repos/{repository}/git/trees", method="POST",
                      payload={"base_tree": base_tree, "tree": [
                          {"path": PREFIX, "mode": "040000", "type": "tree",
                           "sha": isolated_sha}]})
    new_tree_sha = valid_sha(new_tree.get("sha"))
    authorize_write()
    commit = gh_api(f"repos/{repository}/git/commits", method="POST", payload={
        "message": "ci: source snapshot from approved private Oreslang checkout",
        "tree": new_tree_sha, "parents": [base_sha]
    })
    commit_sha = valid_sha(commit.get("sha"))
    authorize_write()
    # CREATE a new CI ref, never UPDATE an existing branch, tag, or main.
    created = gh_api(f"repos/{repository}/git/refs", method="POST", payload={
        "ref": "refs/heads/" + branch, "sha": commit_sha
    })
    if created.get("ref") != "refs/heads/" + branch:
        raise Denied("GitHub did not confirm creation of requested isolated CI branch")
    return commit_sha, len(files)


def main(argv=None):
    # Direct legacy publication is intentionally disabled. V3 is the only
    # supported transfer entrypoint; keeping this module importable preserves
    # shared verifier helpers and offline adversarial tests without exposing a
    # second operational publisher path.
    del argv
    print(
        "PRIVATE_CI_DESTINATION_REJECTED: legacy private-source publisher "
        "is disabled; use run-in-private-ci-destination.sh (V3)",
        file=sys.stderr,
    )
    return 1

if __name__ == "__main__":
    sys.exit(main())

