#!/usr/bin/env python3
"""Source-free unit tests for the protected v3 private-CI dispatcher."""
import base64
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("publish-private-ci-source-v3.py")
SPEC = importlib.util.spec_from_file_location("private_ci_v3", SCRIPT)
v3 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(v3)

SOURCE_TREE = "a" * 40
ROOT_TREE = "b" * 40
COMMIT = "c" * 40
BASE = "d" * 40
WORKFLOW = "e" * 40
PLAN_BLOB = "f" * 40
PROTECTED = "1" * 40
POLICY = b"p" * 32
PLAN = (
    json.dumps(
        {
            "schema": "ores-private-ci-test-plan-v1",
            "source_repo": v3.legacy.SOURCE_REPO,
            "allowed_prs": [1482],
            "commands": [
                {
                    "argv": ["python3", "-c", "print('ok')"],
                    "cwd": ".",
                    "timeout_seconds": 30,
                }
            ],
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    + "\n"
).encode()

CARRIER_CODEOWNERS = (
    b"* @ORESoftware\n"
    b"/.github/ @ORESoftware\n"
    b"/.github/CODEOWNERS @ORESoftware\n"
)
CARRIER_CODEOWNERS_BLOB = v3.git_blob_sha(CARRIER_CODEOWNERS)


def contract():
    return {
        "repository_id": 1323598177,
        "base_sha": BASE,
        "workflow_path": ".github/workflows/oreslang-exact-tree-carrier.yml",
        "workflow_blob_sha": WORKFLOW,
        "test_plan_path": "ci/test-plans/java-pr1482.json",
        "test_plan_blob_sha": PLAN_BLOB,
    }


class PrivateCiV3Test(unittest.TestCase):
    def test_strict_json_rejects_nonfinite_contract_plan_and_ref_values(self):
        for payload in ('{"v":NaN}', '{"v":Infinity}', '{"v":1e999}', '{"v":1,"v":2}'):
            with self.subTest(payload=payload), self.assertRaises(v3.Denied):
                v3.strict_json_loads(payload, "untrusted v3 JSON")

        bad_plan = (
            '{"schema":"ores-private-ci-test-plan-v1",'
            '"source_repo":"' + v3.legacy.SOURCE_REPO + '",'
            '"allowed_prs":[1482],"commands":[], "x":1e999}'
        ).encode()
        with self.assertRaises(v3.Denied):
            v3.validate_test_plan(bad_plan, 1482)

    def test_source_pr_requires_latest_independent_exact_head_review(self):
        approved = {
            "id": 101,
            "user": {
                "id": v3.CARRIER_REVIEW_USER_ID,
                "login": v3.CARRIER_REVIEW_LOGIN,
            },
            "submitted_at": "2026-10-20T19:00:00Z",
            "state": "APPROVED",
            "commit_id": "8" * 40,
        }
        author = {"id": 424242, "login": "PR-author"}
        with patch.object(v3, "gh_api_list", return_value=[approved]) as reviews:
            v3.require_source_pr_independent_review(
                1482, "8" * 40, author
            )
        reviews.assert_called_once_with(
            "repos/" + v3.legacy.SOURCE_REPO
            + "/pulls/1482/reviews?per_page=100"
        )

        for incorrect in (
            dict(approved, commit_id="9" * 40),
            dict(approved, state="CHANGES_REQUESTED"),
            dict(approved, user={"id": 1234, "login": v3.CARRIER_REVIEW_LOGIN}),
            dict(
                approved,
                user={"id": v3.CARRIER_REVIEW_USER_ID, "login": "forged-name"},
            ),
        ):
            with (
                self.subTest(incorrect=incorrect),
                patch.object(v3, "gh_api_list", return_value=[incorrect]),
            ):
                with self.assertRaises(v3.Denied):
                    v3.require_source_pr_independent_review(
                        1482, "8" * 40, author
                    )

    def test_source_pr_latest_review_revocation_pagination_and_self_review_fail_closed(self):
        earlier = {
            "id": 10,
            "user": {
                "id": v3.CARRIER_REVIEW_USER_ID,
                "login": v3.CARRIER_REVIEW_LOGIN,
            },
            "submitted_at": "2026-10-20T19:00:00Z",
            "state": "APPROVED",
            "commit_id": "8" * 40,
        }
        revoked = dict(
            earlier,
            id=11,
            submitted_at="2026-10-20T19:01:00Z",
            state="DISMISSED",
        )
        author = {"id": 424242}
        with patch.object(v3, "gh_api_list", return_value=[earlier, revoked]):
            with self.assertRaisesRegex(v3.Denied, "stale or revoked"):
                v3.require_source_pr_independent_review(
                    1482, "8" * 40, author
                )

        with patch.object(
            v3, "gh_api_list", return_value=[dict(earlier, id=i + 1) for i in range(100)]
        ):
            with self.assertRaisesRegex(v3.Denied, "incomplete"):
                v3.require_source_pr_independent_review(
                    1482, "8" * 40, author
                )

        with patch.object(v3, "gh_api_list") as reviews:
            with self.assertRaisesRegex(v3.Denied, "self-authorize"):
                v3.require_source_pr_independent_review(
                    1482,
                    "8" * 40,
                    {"id": v3.CARRIER_REVIEW_USER_ID},
                )
            reviews.assert_not_called()

    def test_verify_source_pr_requires_ready_same_repo_pr_before_review(self):
        repo = {
            "id": v3.SOURCE_REPOSITORY_ID,
            "full_name": v3.legacy.SOURCE_REPO,
            "private": True,
            "visibility": "private",
            "archived": False,
            "disabled": False,
            "fork": False,
        }
        base_pr = {
            "number": 1482,
            "state": "open",
            "draft": False,
            "user": {"id": 424242, "login": "author"},
            "head": {
                "sha": "8" * 40,
                "repo": {"full_name": v3.legacy.SOURCE_REPO},
            },
            "base": {
                "ref": v3.legacy.SOURCE_PROTECTED_BRANCH,
                "repo": {"full_name": v3.legacy.SOURCE_REPO},
            },
        }
        for field, value in (
            ("draft", True),
            ("base_repo", {"full_name": "other/repo"}),
        ):
            bad = json.loads(json.dumps(base_pr))
            if field == "draft":
                bad["draft"] = value
            else:
                bad["base"]["repo"] = value
            with (
                self.subTest(field=field),
                patch.object(v3.legacy, "gh_api", side_effect=[repo, bad]),
                patch.object(v3, "require_source_pr_independent_review") as review,
            ):
                with self.assertRaisesRegex(v3.Denied, "exact open PR head"):
                    v3.verify_source_pr(1482, "8" * 40)
                review.assert_not_called()

    def test_authorized_main_rejects_unprotected_source(self):
        branch = {
            "name": v3.legacy.SOURCE_PROTECTED_BRANCH,
            "protected": False,
            "commit": {"sha": PROTECTED},
        }
        with (
            patch.object(v3.legacy, "gh_api", return_value=branch),
            patch.object(v3.legacy, "verify_protected_origin") as protected,
        ):
            with self.assertRaisesRegex(
                v3.Denied, "source main is not protected"
            ):
                v3.authorized_main_sha("trusted/ci", POLICY, 246810)
            protected.assert_not_called()

    def test_authorized_main_requires_classic_protection_when_available(self):
        branch = {
            "name": v3.legacy.SOURCE_PROTECTED_BRANCH,
            "protected": True,
            "commit": {"sha": PROTECTED},
        }
        with (
            patch.object(v3.legacy, "gh_api", return_value=branch),
            patch.object(
                v3.legacy, "verify_protected_origin",
                return_value=PROTECTED,
            ) as protected,
        ):
            self.assertEqual(
                v3.authorized_main_sha("trusted/ci", POLICY, 246810),
                PROTECTED,
            )
            protected.assert_called_once_with("trusted/ci", POLICY, 246810)

    def test_maven_plan_requires_offline_mode_and_exact_pom_blob(self):
        base = {
            "schema": "ores-private-ci-test-plan-v1",
            "source_repo": v3.legacy.SOURCE_REPO,
            "allowed_prs": [1482],
            "commands": [{
                "argv": ["mvn", "-B", "test"],
                "cwd": ".",
                "timeout_seconds": 300,
            }],
        }
        raw = (json.dumps(base) + "\n").encode()
        with self.assertRaisesRegex(v3.Denied, "offline"):
            v3.validate_test_plan(raw, 1482)

        base["commands"][0]["argv"] = ["mvn", "-o", "-B", "test"]
        raw = (json.dumps(base) + "\n").encode()
        with self.assertRaisesRegex(v3.Denied, "strict checksums"):
            v3.validate_test_plan(raw, 1482)

        base["commands"][0]["argv"] = ["mvn", "-o", "-C", "-B", "test"]
        raw = (json.dumps(base) + "\n").encode()
        with self.assertRaisesRegex(v3.Denied, "bind exact pom.xml"):
            v3.validate_test_plan(raw, 1482)

        pom = b"<project/>\n"
        pom_sha = v3.git_blob_sha(pom)
        base["maven_pom_blob_sha"] = pom_sha
        base["maven_prefetch_artifacts"] = [
            "org.apache.maven.surefire:surefire-junit-platform:3.5.2",
            "org.junit.platform:junit-platform-launcher:1.11.4",
        ]
        raw = (json.dumps(base) + "\n").encode()
        meta = v3.validate_test_plan(raw, 1482)
        self.assertTrue(meta["uses_maven"])
        self.assertEqual(meta["maven_pom_blob_sha"], pom_sha)
        self.assertEqual(meta["required_source_blobs"]["pom.xml"], pom_sha)
        self.assertEqual(
            meta["maven_prefetch_artifacts"],
            base["maven_prefetch_artifacts"],
        )
        v3.verify_required_source_blobs(
            [("pom.xml", pom, "100644")],
            meta["required_source_blobs"],
        )

        # Preserve compatibility with already-reviewed v1 plans that used
        # required_source_blobs rather than maven_pom_blob_sha.
        legacy_plan = dict(base)
        legacy_plan.pop("maven_pom_blob_sha")
        legacy_plan.pop("maven_prefetch_artifacts")
        legacy_plan["required_source_blobs"] = {"pom.xml": pom_sha}
        meta = v3.validate_test_plan(
            (json.dumps(legacy_plan) + "\n").encode(), 1482
        )
        self.assertEqual(meta["maven_pom_blob_sha"], pom_sha)

    def test_maven_plan_rejects_conflicting_or_unsafe_dependency_controls(self):
        pom = v3.git_blob_sha(b"<project/>\n")
        base = {
            "schema": "ores-private-ci-test-plan-v1",
            "source_repo": v3.legacy.SOURCE_REPO,
            "allowed_prs": [1482],
            "maven_pom_blob_sha": pom,
            "commands": [{
                "argv": ["mvn", "-o", "-C", "-B", "test"],
                "cwd": ".",
                "timeout_seconds": 300,
            }],
        }

        for arg in (
            "-s/tmp/settings.xml",
            "--settings=/tmp/settings.xml",
            "-Dmaven.repo.local=/tmp/m2",
            "-DskipTests",
            "-Dmaven.test.skip=true",
            "-DskipITs=true",
        ):
            with self.subTest(arg=arg):
                doc = json.loads(json.dumps(base))
                doc["commands"][0]["argv"].insert(2, arg)
                with self.assertRaisesRegex(v3.Denied, "trusted build controls"):
                    v3.validate_test_plan(
                        (json.dumps(doc) + "\n").encode(), 1482
                    )

        conflict = json.loads(json.dumps(base))
        conflict["required_source_blobs"] = {"pom.xml": "0" * 40}
        with self.assertRaisesRegex(v3.Denied, "pins disagree"):
            v3.validate_test_plan(
                (json.dumps(conflict) + "\n").encode(), 1482
            )

    def test_maven_prefetch_artifacts_are_exact_bounded_and_maven_only(self):
        pom = v3.git_blob_sha(b"<project/>\n")
        base = {
            "schema": "ores-private-ci-test-plan-v1",
            "source_repo": v3.legacy.SOURCE_REPO,
            "allowed_prs": [1482],
            "maven_pom_blob_sha": pom,
            "commands": [{
                "argv": ["mvn", "-o", "-C", "test"],
                "cwd": ".",
                "timeout_seconds": 300,
            }],
        }
        invalids = [
            ["bad"],
            ["g:a:v:jar:classifier:extra"],
            ["g:a:v", "g:a:v"],
            ["g:a:v"] * (v3.MAX_MAVEN_PREFETCH_ARTIFACTS + 1),
        ]
        for extras in invalids:
            with self.subTest(extras=extras):
                doc = json.loads(json.dumps(base))
                doc["maven_prefetch_artifacts"] = extras
                with self.assertRaisesRegex(v3.Denied, "prefetch artifacts"):
                    v3.validate_test_plan(
                        (json.dumps(doc) + "\n").encode(), 1482
                    )

        non_maven = json.loads(PLAN)
        non_maven["maven_prefetch_artifacts"] = ["g:a:v"]
        with self.assertRaisesRegex(v3.Denied, "non-Maven"):
            v3.validate_test_plan(
                (json.dumps(non_maven) + "\n").encode(), 1482
            )

        non_maven = json.loads(PLAN)
        non_maven["maven_pom_blob_sha"] = pom
        with self.assertRaisesRegex(v3.Denied, "non-Maven"):
            v3.validate_test_plan(
                (json.dumps(non_maven) + "\n").encode(), 1482
            )

    def test_required_source_blob_mismatch_fails_before_upload(self):
        expected = v3.git_blob_sha(b"reviewed pom\n")
        with self.assertRaisesRegex(v3.Denied, "required blob: pom.xml"):
            v3.verify_required_source_blobs(
                [("pom.xml", b"changed pom\n", "100644")],
                {"pom.xml": expected},
            )

    def test_test_plan_accepts_exact_cpp_compiler_executables(self):
        for executable in ("g++", "clang++"):
            with self.subTest(executable=executable):
                doc = json.loads(PLAN)
                doc["commands"][0]["argv"] = [executable, "--version"]
                meta = v3.validate_test_plan(
                    (json.dumps(doc) + "\n").encode(), 1482
                )
                self.assertFalse(meta["uses_maven"])

        for executable in (r"g\\+\\+", r"clang\\+\\+", "g+", "clang+"):
            with self.subTest(executable=executable):
                doc = json.loads(PLAN)
                doc["commands"][0]["argv"] = [executable, "--version"]
                with self.assertRaisesRegex(v3.Denied, "command invalid"):
                    v3.validate_test_plan(
                        (json.dumps(doc) + "\n").encode(), 1482
                    )

    def test_test_plan_rejects_unapproved_executable_cwd_and_stdout_shape(self):
        for argv in (
            ["bash", "-lc", "true"],
            ["sh", "-c", "true"],
            ["git", "status"],
            ["curl", "https://example.invalid"],
        ):
            with self.subTest(argv=argv):
                doc = json.loads(PLAN)
                doc["commands"][0]["argv"] = argv
                with self.assertRaisesRegex(v3.Denied, "command invalid"):
                    v3.validate_test_plan(
                        (json.dumps(doc) + "\n").encode(), 1482
                    )

        doc = json.loads(PLAN)
        doc["commands"][0]["cwd"] = "../escape"
        with self.assertRaisesRegex(v3.Denied, "command invalid"):
            v3.validate_test_plan((json.dumps(doc) + "\n").encode(), 1482)

        doc = json.loads(PLAN)
        doc["commands"][0]["stdout_contains"] = "ok"
        meta = v3.validate_test_plan((json.dumps(doc) + "\n").encode(), 1482)
        self.assertFalse(meta["uses_maven"])

        doc["commands"][0]["stdout_contains"] = ""
        with self.assertRaisesRegex(v3.Denied, "command invalid"):
            v3.validate_test_plan((json.dumps(doc) + "\n").encode(), 1482)

    def test_test_plan_rejects_unreviewed_extension_fields(self):
        doc = json.loads(PLAN)
        doc["network"] = "allow"
        raw = (json.dumps(doc) + "\n").encode()
        with self.assertRaisesRegex(v3.Denied, "unexpected fields"):
            v3.validate_test_plan(raw, 1482)

    def test_remote_protected_file_rejects_blob_sha_mismatch(self):
        raw = b"reviewed-bytes\n"
        encoded = base64.b64encode(raw).decode("ascii")

        with patch.object(
            v3.legacy,
            "gh_api",
            return_value={
                "path": "scripts/publish-private-ci-source-v3.py",
                "type": "file",
                "encoding": "base64",
                "content": encoded,
                "sha": "0" * 40,
            },
        ):
            with self.assertRaisesRegex(v3.Denied, "blob digest mismatch"):
                v3.remote_file_at_source_main(
                    "scripts/publish-private-ci-source-v3.py",
                    PROTECTED,
                    4096,
                )

    def test_local_control_plane_read_requires_descriptor_safety_flags(self):
        with (
            patch.object(v3.legacy, "safe_descriptor_flags_available", return_value=False),
            patch.object(v3.os, "open") as opened,
        ):
            with self.assertRaisesRegex(v3.Denied, "cannot safely read"):
                v3.read_local_protected_file(
                    Path("."), "scripts/publish-private-ci-source-v3.py", 4096
                )
            opened.assert_not_called()

    def test_control_plane_rejects_local_runtime_divergence(self):
        with (
            patch.object(v3, "require_control_plane_location"),
            patch.object(
                v3,
                "read_local_protected_file",
                return_value=b"modified-local-code\n",
            ),
            patch.object(
                v3,
                "remote_file_at_source_main",
                return_value=(b"reviewed-protected-code\n", "1" * 40),
            ),
        ):
            with self.assertRaisesRegex(v3.Denied, "control plane diverges"):
                v3.checked_control_plane(Path("."), PROTECTED)

    def test_control_plane_requires_executing_modules_under_trusted_root(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            scripts = root / "scripts"
            scripts.mkdir()
            v3_path = scripts / "publish-private-ci-source-v3.py"
            legacy_path = scripts / "publish-private-ci-source.py"
            v3_path.write_text("# reviewed v3\n")
            legacy_path.write_text("# reviewed legacy\n")

            with (
                patch.object(v3, "__file__", str(v3_path)),
                patch.object(v3, "LEGACY_PATH", legacy_path),
            ):
                v3.require_control_plane_location(root)

            outside = root / "modified-copy.py"
            outside.write_text("# modified\n")
            with (
                patch.object(v3, "__file__", str(outside)),
                patch.object(v3, "LEGACY_PATH", legacy_path),
            ):
                with self.assertRaisesRegex(v3.Denied, "outside the trusted control plane"):
                    v3.require_control_plane_location(root)

    def test_safe_policy_path_rejects_encoded_noncanonical_and_git_routes(self):
        self.assertEqual(
            v3.safe_policy_path(
                "ci/test-plans/java-pr1482.json",
                "ci/test-plans/",
                ".json",
            ),
            "ci/test-plans/java-pr1482.json",
        )
        for value in (
            "ci/test-plans/../x.json",
            "ci/test-plans/x%2fescape.json",
            "ci/test-plans/x.json?ref=other",
            "ci/test-plans//x.json",
            "ci/test-plans/./x.json",
            "ci/test-plans/.git/x.json",
            "ci/test-plans/.GIT/x.json",
        ):
            with self.subTest(value=value), self.assertRaises(v3.Denied):
                v3.safe_policy_path(value, "ci/test-plans/", ".json")

    def test_handoff_ref_is_exact_tree_or_authorization_sha_only(self):
        self.assertRegex("ci/oreslang-" + SOURCE_TREE, v3.HANDOFF_REF)
        self.assertRegex("ci/authorized-" + COMMIT, v3.HANDOFF_REF)
        for value in (
            "ci/oreslang-test",
            "ci/oreslang-" + "a" * 39,
            "ci/oreslang-" + "A" * 40,
            "ci/authorized-test",
            "ci/unrelated-" + COMMIT,
        ):
            with self.subTest(value=value):
                self.assertIsNone(v3.HANDOFF_REF.fullmatch(value))

    def test_parse_contract_requires_exact_pr_and_pinned_fields(self):
        raw = json.dumps(
            {
                "schema": 1,
                "carriers": {
                    "trusted/ci": {
                        "repository_id": 42,
                        "base_sha": BASE,
                        "workflow_path": ".github/workflows/oreslang-exact-tree-carrier.yml",
                        "workflow_blob_sha": WORKFLOW,
                        "test_plans": {
                            "ci/test-plans/java-pr1482.json": {
                                "blob_sha": PLAN_BLOB,
                                "allowed_prs": [1482],
                            }
                        },
                    }
                },
            }
        ).encode()
        got = v3.parse_contract(
            raw, "trusted/ci", 1482, "ci/test-plans/java-pr1482.json"
        )
        self.assertEqual(got["repository_id"], 42)
        self.assertEqual(got["base_sha"], BASE)
        self.assertEqual(got["workflow_blob_sha"], WORKFLOW)
        self.assertEqual(got["test_plan_blob_sha"], PLAN_BLOB)
        self.assertNotIn("review_pr", got)
        with self.assertRaisesRegex(v3.Denied, "not authorized"):
            v3.parse_contract(
                raw, "trusted/ci", 1483, "ci/test-plans/java-pr1482.json"
            )

    def test_contract_rejects_review_attestation_escape_hatch(self):
        raw = {
            "schema": 1,
            "carriers": {
                "trusted/ci": {
                    "repository_id": 42,
                    "base_sha": BASE,
                    "workflow_path": ".github/workflows/oreslang-exact-tree-carrier.yml",
                    "workflow_blob_sha": WORKFLOW,
                    "review_pr": 17,
                    "test_plans": {
                        "ci/test-plans/java-pr1482.json": {
                            "blob_sha": PLAN_BLOB,
                            "allowed_prs": [1482],
                        }
                    },
                }
            },
        }
        with self.assertRaisesRegex(v3.Denied, "fields are not exact"):
            v3.parse_contract(
                json.dumps(raw).encode(),
                "trusted/ci", 1482, "ci/test-plans/java-pr1482.json",
            )

    def test_unprotected_carrier_is_always_denied(self):
        carrier = contract()

        def api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": False,
                    "commit": {"sha": BASE},
                }
            raise AssertionError(route)

        with patch.object(v3.legacy, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "not protected"):
                v3.checked_carrier_main("trusted/ci", carrier)

    def test_required_ci_checks_are_issuer_bound_and_unambiguous(self):
        app_id = v3.legacy.GITHUB_ACTIONS_APP_ID
        accepted = {
            "strict": True,
            "checks": [{"context": "private-ci-policy", "app_id": app_id}],
        }
        for contexts in (None, [], ["private-ci-policy"]):
            with self.subTest(contexts=contexts):
                policy = dict(accepted)
                if contexts is not None:
                    policy["contexts"] = contexts
                v3.legacy.require_issuer_bound_actions_checks(
                    policy, "fixture", "private-ci-policy"
                )

        rejected = (
            {"strict": True, "contexts": ["private-ci-policy"], "checks": []},
            {
                "strict": True,
                "contexts": ["private-ci-policy"],
                "checks": [{"context": "private-ci-policy", "app_id": None}],
            },
            {
                "strict": True,
                "contexts": ["private-ci-policy"],
                "checks": [{"context": "private-ci-policy", "app_id": 999999}],
            },
            {
                "strict": True,
                "contexts": ["unbound"],
                "checks": [{"context": "private-ci-policy", "app_id": app_id}],
            },
            {
                "strict": True,
                "contexts": [],
                "checks": [
                    {"context": "dup", "app_id": app_id},
                    {"context": "dup", "app_id": app_id},
                ],
            },
        )
        for policy in rejected:
            with self.subTest(policy=policy), self.assertRaises(v3.Denied):
                v3.legacy.require_issuer_bound_actions_checks(
                    policy, "fixture", "private-ci-policy"
                )

        with self.assertRaisesRegex(v3.Denied, "security CI check missing"):
            v3.legacy.require_issuer_bound_actions_checks(
                {
                    "strict": True,
                    "contexts": [],
                    "checks": [
                        {
                            "context": "unrelated-actions-job",
                            "app_id": v3.legacy.GITHUB_ACTIONS_APP_ID,
                        }
                    ],
                },
                "fixture",
                "private-ci-policy",
            )

    def test_carrier_rejects_untrusted_required_ci_issuer(self):
        carrier = contract()
        protection = {
            "required_status_checks": {
                "strict": True,
                "contexts": ["verifier-tests"],
                "checks": [
                    {"context": "verifier-tests", "app_id": 999999}
                ],
            },
            "required_pull_request_reviews": {
                "require_code_owner_reviews": True,
                "dismiss_stale_reviews": True,
                "require_last_push_approval": True,
                "required_approving_review_count": 1,
                "bypass_pull_request_allowances": {
                    "users": [], "teams": [], "apps": [],
                },
            },
            "enforce_admins": {"enabled": True},
            "allow_force_pushes": {"enabled": False},
            "allow_deletions": {"enabled": False},
            "required_conversation_resolution": {"enabled": True},
        }

        def api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": True,
                    "commit": {"sha": BASE},
                }
            if route == "repos/trusted/ci/branches/main/protection":
                return protection
            raise AssertionError(route)

        with patch.object(v3.legacy, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "untrusted issuer"):
                v3.checked_carrier_main("trusted/ci", carrier)

    def test_protected_carrier_still_requires_codeowner_admin_and_no_bypass(self):
        carrier = contract()
        protection = {
            "required_status_checks": {
                "strict": True,
                "contexts": ["verifier-tests"],
                "checks": [
                    {
                        "context": "verifier-tests",
                        "app_id": v3.legacy.GITHUB_ACTIONS_APP_ID,
                    }
                ],
            },
            "required_pull_request_reviews": {
                "require_code_owner_reviews": True,
                "dismiss_stale_reviews": True,
                "require_last_push_approval": True,
                "required_approving_review_count": 1,
                "bypass_pull_request_allowances": {
                    "users": [], "teams": [], "apps": [],
                },
            },
            "enforce_admins": {"enabled": True},
            "allow_force_pushes": {"enabled": False},
            "allow_deletions": {"enabled": False},
            "required_conversation_resolution": {"enabled": True},
        }

        def api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": True,
                    "commit": {"sha": BASE},
                }
            if route == "repos/trusted/ci/branches/main/protection":
                return protection
            if route == "repos/trusted/ci/contents/.github/CODEOWNERS?ref=" + BASE:
                return {
                    "path": ".github/CODEOWNERS",
                    "type": "file",
                    "encoding": "base64",
                    "content": base64.b64encode(CARRIER_CODEOWNERS).decode(),
                    "sha": CARRIER_CODEOWNERS_BLOB,
                }
            raise AssertionError(route)

        with patch.object(v3.legacy, "gh_api", side_effect=api):
            self.assertEqual(
                v3.checked_carrier_main("trusted/ci", carrier), BASE
            )

        weak = json.loads(json.dumps(protection))
        weak["required_pull_request_reviews"]["bypass_pull_request_allowances"]["users"] = [{"login": "bypass"}]

        def weak_api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": True,
                    "commit": {"sha": BASE},
                }
            if route == "repos/trusted/ci/branches/main/protection":
                return weak
            raise AssertionError(route)

        with patch.object(v3.legacy, "gh_api", side_effect=weak_api):
            with self.assertRaisesRegex(v3.Denied, "bypasses"):
                v3.checked_carrier_main("trusted/ci", carrier)

        no_checks = json.loads(json.dumps(protection))
        del no_checks["required_status_checks"]

        def no_checks_api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": True,
                    "commit": {"sha": BASE},
                }
            if route == "repos/trusted/ci/branches/main/protection":
                return no_checks
            raise AssertionError(route)

        with patch.object(v3.legacy, "gh_api", side_effect=no_checks_api):
            with self.assertRaisesRegex(v3.Denied, "strict required CI"):
                v3.checked_carrier_main("trusted/ci", carrier)

    def test_carrier_requires_last_push_approval_and_conversation_resolution(self):
        carrier = contract()
        base = {
            "required_status_checks": {
                "strict": True,
                "contexts": ["verifier-tests"],
                "checks": [{
                    "context": "verifier-tests",
                    "app_id": v3.legacy.GITHUB_ACTIONS_APP_ID,
                }],
            },
            "required_pull_request_reviews": {
                "require_code_owner_reviews": True,
                "dismiss_stale_reviews": True,
                "require_last_push_approval": True,
                "required_approving_review_count": 1,
                "bypass_pull_request_allowances": {
                    "users": [], "teams": [], "apps": [],
                },
            },
            "enforce_admins": {"enabled": True},
            "allow_force_pushes": {"enabled": False},
            "allow_deletions": {"enabled": False},
            "required_conversation_resolution": {"enabled": True},
        }

        def run(protection):
            def api(route, method="GET", payload=None):
                if route == "repos/trusted/ci/branches/main":
                    return {
                        "name": "main",
                        "protected": True,
                        "commit": {"sha": BASE},
                    }
                if route == "repos/trusted/ci/branches/main/protection":
                    return protection
                raise AssertionError(route)
            with patch.object(v3.legacy, "gh_api", side_effect=api):
                return v3.checked_carrier_main("trusted/ci", carrier)

        weak = json.loads(json.dumps(base))
        weak["required_pull_request_reviews"]["require_last_push_approval"] = False
        with self.assertRaisesRegex(v3.Denied, "last-push approval"):
            run(weak)

        weak = json.loads(json.dumps(base))
        weak["required_conversation_resolution"]["enabled"] = False
        with self.assertRaisesRegex(v3.Denied, "conversation resolution"):
            run(weak)

    def test_contract_rejects_duplicate_json_keys(self):
        raw = (
            '{"schema":1,"schema":1,"carriers":{"trusted/ci":{'
            '"repository_id":42,"base_sha":"' + BASE + '",'
            '"workflow_path":".github/workflows/x.yml",'
            '"workflow_blob_sha":"' + WORKFLOW + '",'
            '"test_plans":{}}}}'
        ).encode()
        with self.assertRaisesRegex(v3.Denied, "duplicate JSON key"):
            v3.parse_contract(raw, "trusted/ci", 1, "ci/test-plans/x.json")

    def test_checked_destination_rejects_source_bearing_reviewed_base(self):
        carrier = contract()
        def api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/git/ref/heads/main":
                return {"object": {"sha": BASE}}
            if route == "repos/trusted/ci/git/commits/" + BASE:
                return {"sha": BASE, "tree": {"sha": ROOT_TREE}}
            if route == "repos/trusted/ci/git/trees/" + ROOT_TREE + "?recursive=1":
                return {
                    "sha": ROOT_TREE,
                    "truncated": False,
                    "tree": [
                        {
                            "path": v3.legacy.PREFIX + "/old.ores",
                            "type": "blob",
                            "mode": "100644",
                            "sha": "2" * 40,
                        }
                    ],
                }
            raise AssertionError(route)
        with patch.object(
            v3.legacy,
            "checked_repo",
            return_value={
                "id": carrier["repository_id"],
                "private": True,
                "visibility": "private",
                "archived": False,
                "disabled": False,
                "fork": False,
                "default_branch": "main",
            },
        ), patch.object(
            v3, "checked_carrier_main", return_value=BASE
        ), patch.object(v3.legacy, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "not source-free"):
                v3.checked_destination("trusted/ci", carrier)

    def test_verify_source_pr_requires_exact_open_head(self):
        def api(route, method="GET", payload=None):
            if route == "repos/" + v3.legacy.SOURCE_REPO:
                return {
                    "id": v3.SOURCE_REPOSITORY_ID,
                    "full_name": v3.legacy.SOURCE_REPO,
                    "private": True,
                    "visibility": "private",
                    "archived": False,
                    "disabled": False,
                    "fork": False,
                }
            if route == "repos/" + v3.legacy.SOURCE_REPO + "/pulls/1482":
                return {
                    "number": 1482,
                    "state": "open",
                    "head": {
                        "sha": "9" * 40,
                        "repo": {"full_name": v3.legacy.SOURCE_REPO},
                    },
                    "base": {"ref": "main"},
                }
            raise AssertionError(route)
        with patch.object(v3.legacy, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "exact open PR head"):
                v3.verify_source_pr(1482, "8" * 40)

    def test_verify_source_pr_rejects_source_repository_id_replacement(self):
        def api(route, method="GET", payload=None):
            if route == "repos/" + v3.legacy.SOURCE_REPO:
                return {
                    "id": v3.SOURCE_REPOSITORY_ID + 1,
                    "full_name": v3.legacy.SOURCE_REPO,
                    "private": True,
                    "visibility": "private",
                    "archived": False,
                    "disabled": False,
                    "fork": False,
                }
            raise AssertionError(route)
        with patch.object(v3.legacy, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "identity/state"):
                v3.verify_source_pr(1482, "8" * 40)

    def test_authorize_write_rechecks_carrier_eligibility(self):
        carrier = contract()
        ineligible = {
            "id": carrier["repository_id"],
            "full_name": "trusted/ci",
            "private": True,
            "visibility": "private",
            "archived": True,
            "disabled": False,
            "fork": False,
            "default_branch": "main",
        }
        with (
            patch.object(v3.legacy, "require_resolved_blocking_incidents"),
            patch.object(
                v3.legacy,
                "checked_policy_approval",
                return_value=(POLICY, carrier["repository_id"]),
            ),
            patch.object(v3.legacy, "checked_repo", return_value=ineligible),
        ):
            with self.assertRaisesRegex(v3.Denied, "identity/state"):
                v3.authorize_write(
                    Path("."), "trusted/ci", POLICY, carrier,
                    1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
                )

    def test_authorize_write_rejects_reviewed_repository_id_change(self):
        carrier = contract()
        with (
            patch.object(v3.legacy, "require_resolved_blocking_incidents"),
            patch.object(
                v3.legacy,
                "checked_policy_approval",
                return_value=(POLICY, carrier["repository_id"] + 1),
            ),
            patch.object(v3.legacy, "checked_repo") as checked_repo,
        ):
            with self.assertRaisesRegex(v3.Denied, "repository identity changed"):
                v3.authorize_write(
                    Path("."), "trusted/ci", POLICY, carrier,
                    1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
                )
            checked_repo.assert_not_called()

    def test_authorize_write_rechecks_carrier_governance_each_write(self):
        carrier = contract()
        eligible = {
            "id": carrier["repository_id"],
            "full_name": "trusted/ci",
            "private": True,
            "visibility": "private",
            "archived": False,
            "disabled": False,
            "fork": False,
            "default_branch": "main",
        }

        with (
            patch.object(v3.legacy, "require_resolved_blocking_incidents"),
            patch.object(
                v3.legacy, "checked_policy_approval",
                return_value=(POLICY, carrier["repository_id"]),
            ),
            patch.object(v3.legacy, "checked_repo", return_value=eligible),
            patch.object(
                v3, "checked_carrier_main", return_value=carrier["base_sha"]
            ) as carrier_governance,
            patch.object(
                v3, "verify_source_pr",
                return_value=v3.SOURCE_REPOSITORY_ID,
            ),
            patch.object(v3, "revalidate_source_governance") as governance,
        ):
            v3.authorize_write(
                Path("."), "trusted/ci", POLICY, carrier,
                1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
            )
        carrier_governance.assert_called_once_with("trusted/ci", carrier)
        governance.assert_called_once_with(
            Path("."), "trusted/ci", POLICY, carrier, 1482, PROTECTED
        )

        with (
            patch.object(v3.legacy, "require_resolved_blocking_incidents"),
            patch.object(
                v3.legacy, "checked_policy_approval",
                return_value=(POLICY, carrier["repository_id"]),
            ),
            patch.object(v3.legacy, "checked_repo", return_value=eligible),
            patch.object(
                v3, "checked_carrier_main",
                side_effect=v3.Denied("carrier main is not protected; private export denied"),
            ),
            patch.object(v3, "verify_source_pr") as source_pr_check,
        ):
            with self.assertRaisesRegex(v3.Denied, "not protected"):
                v3.authorize_write(
                    Path("."), "trusted/ci", POLICY, carrier,
                    1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
                )
        source_pr_check.assert_not_called()

    def test_authorize_write_rechecks_source_pr_authorization_each_write(self):
        carrier = contract()
        eligible = {
            "id": carrier["repository_id"],
            "full_name": "trusted/ci",
            "private": True,
            "visibility": "private",
            "archived": False,
            "disabled": False,
            "fork": False,
            "default_branch": "main",
        }
        def api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "commit": {"sha": carrier["base_sha"]},
                }
            raise AssertionError(route)

        with (
            patch.object(v3.legacy, "require_resolved_blocking_incidents"),
            patch.object(
                v3.legacy, "checked_policy_approval",
                return_value=(POLICY, carrier["repository_id"]),
            ),
            patch.object(v3.legacy, "checked_repo", return_value=eligible),
            patch.object(v3, "checked_carrier_main", return_value=carrier["base_sha"]),
            patch.object(
                v3, "verify_source_pr",
                return_value=v3.SOURCE_REPOSITORY_ID,
            ) as source_pr_check,
            patch.object(v3, "revalidate_source_governance") as governance,
        ):
            v3.authorize_write(
                Path("."), "trusted/ci", POLICY, carrier,
                1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
            )
        source_pr_check.assert_called_once_with(1482, "8" * 40)
        governance.assert_called_once_with(
            Path("."), "trusted/ci", POLICY, carrier, 1482, PROTECTED
        )

        with (
            patch.object(v3.legacy, "require_resolved_blocking_incidents"),
            patch.object(
                v3.legacy, "checked_policy_approval",
                return_value=(POLICY, carrier["repository_id"]),
            ),
            patch.object(v3.legacy, "checked_repo", return_value=eligible),
            patch.object(v3, "checked_carrier_main", return_value=carrier["base_sha"]),
            patch.object(
                v3, "verify_source_pr",
                side_effect=v3.Denied("source PR security owner approval is stale or revoked"),
            ),
        ):
            with self.assertRaisesRegex(v3.Denied, "stale or revoked"):
                v3.authorize_write(
                    Path("."), "trusted/ci", POLICY, carrier,
                    1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
                )

    def test_revalidate_source_governance_rejects_authority_change(self):
        carrier = contract()
        with (
            patch.object(
                v3, "authorized_main_sha", return_value="2" * 40
            ),
            patch.object(v3, "checked_control_plane") as control_plane,
            patch.object(v3, "checked_contract") as checked_contract,
        ):
            with self.assertRaisesRegex(v3.Denied, "authority changed"):
                v3.revalidate_source_governance(
                    Path("."), "trusted/ci", POLICY, carrier,
                    1482, PROTECTED
                )
            control_plane.assert_not_called()
            checked_contract.assert_not_called()

    def test_revalidate_source_governance_rechecks_control_plane_and_contract(self):
        carrier = contract()
        with (
            patch.object(
                v3, "authorized_main_sha", return_value=PROTECTED
            ),
            patch.object(v3, "checked_control_plane") as control_plane,
            patch.object(
                v3, "checked_contract", return_value=carrier
            ) as checked_contract,
        ):
            v3.revalidate_source_governance(
                Path("."), "trusted/ci", POLICY, carrier,
                1482, PROTECTED
            )
        control_plane.assert_called_once_with(Path("."), PROTECTED)
        checked_contract.assert_called_once_with(
            Path("."), "trusted/ci", 1482,
            carrier["test_plan_path"], PROTECTED
        )

    def test_revalidate_source_governance_rejects_contract_drift(self):
        carrier = contract()
        changed = dict(carrier, workflow_blob_sha="9" * 40)
        with (
            patch.object(
                v3, "authorized_main_sha", return_value=PROTECTED
            ),
            patch.object(v3, "checked_control_plane"),
            patch.object(v3, "checked_contract", return_value=changed),
        ):
            with self.assertRaisesRegex(v3.Denied, "contract changed"):
                v3.revalidate_source_governance(
                    Path("."), "trusted/ci", POLICY, carrier,
                    1482, PROTECTED
                )

    def test_authorize_write_rejects_reopened_incident_before_api_or_blob(self):
        """Fail closed when a P0 changes after preflight but before a write."""
        carrier = contract()
        with (
            patch.object(
                v3.legacy, "require_resolved_blocking_incidents",
                side_effect=v3.Denied("blocking P0 incident #77 remains unresolved"),
            ) as incident_gate,
            patch.object(v3.legacy, "checked_policy_approval") as policy,
            patch.object(v3.legacy, "checked_repo") as repo,
        ):
            with self.assertRaisesRegex(v3.Denied, "#77 remains unresolved"):
                v3.authorize_write(
                Path("."), "trusted/ci", POLICY, carrier,
                1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
            )
            incident_gate.assert_called_once_with()
            policy.assert_not_called()
            repo.assert_not_called()

    def test_authorize_write_rejects_reopened_incident_127_before_api_or_blob(self):
        """Every source-bearing write must fail closed if P0 #127 reopens."""
        carrier = contract()
        with (
            patch.object(
                v3.legacy, "require_resolved_blocking_incidents",
                side_effect=v3.Denied("blocking P0 incident #127 remains unresolved"),
            ) as incident_gate,
            patch.object(v3.legacy, "checked_policy_approval") as policy,
            patch.object(v3.legacy, "checked_repo") as repo,
        ):
            with self.assertRaisesRegex(v3.Denied, "#127 remains unresolved"):
                v3.authorize_write(
                Path("."), "trusted/ci", POLICY, carrier,
                1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
            )
            incident_gate.assert_called_once_with()
            policy.assert_not_called()
            repo.assert_not_called()

    def test_authorize_write_rejects_open_carrier_incident_171_before_api_or_blob(self):
        """V3 must recheck unresolved #171 before every source-bearing write."""
        carrier = contract()
        with (
            patch.object(
                v3.legacy, "require_resolved_blocking_incidents",
                side_effect=v3.Denied("blocking P0 incident #171 remains unresolved"),
            ) as incident_gate,
            patch.object(v3.legacy, "checked_policy_approval") as policy,
            patch.object(v3.legacy, "checked_repo") as repo,
        ):
            with self.assertRaisesRegex(v3.Denied, "#171 remains unresolved"):
                v3.authorize_write(
                    Path("."), "trusted/ci", POLICY, carrier,
                    1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
                )
            incident_gate.assert_called_once_with()
            policy.assert_not_called()
            repo.assert_not_called()

    def test_authorize_write_rejects_open_llvm_carrier_incident_172_before_api_or_blob(self):
        """V3 must recheck #172 before every source-bearing write."""
        carrier = contract()
        with (
            patch.object(
                v3.legacy, "require_resolved_blocking_incidents",
                side_effect=v3.Denied("blocking P0 incident #172 remains unresolved"),
            ) as incident_gate,
            patch.object(v3.legacy, "checked_policy_approval") as policy,
            patch.object(v3.legacy, "checked_repo") as repo,
        ):
            with self.assertRaisesRegex(v3.Denied, "#172 remains unresolved"):
                v3.authorize_write(
                    Path("."), "trusted/ci", POLICY, carrier,
                    1482, "8" * 40, v3.SOURCE_REPOSITORY_ID, PROTECTED
                )
            incident_gate.assert_called_once_with()
            policy.assert_not_called()
            repo.assert_not_called()

    def test_publish_rejects_contract_repository_id_not_in_reviewed_allowlist(self):
        carrier = contract()
        mismatched = dict(carrier, repository_id=carrier["repository_id"] + 1)
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "snapshot"
            source.mkdir()
            with (
                patch.object(v3.legacy, "require_preflight"),
                patch.object(
                    v3.legacy, "checked_policy_approval",
                    return_value=(POLICY, carrier["repository_id"]),
                ),
                patch.object(
                    v3, "authorized_main_sha",
                    return_value=PROTECTED,
                ),
                patch.object(v3, "checked_control_plane"),
                patch.object(v3, "checked_contract", return_value=mismatched),
                patch.object(v3, "verify_source_pr") as verify_source_pr,
            ):
                with self.assertRaisesRegex(
                    v3.Denied, "contract repository ID"
                ):
                    v3.publish(
                        "trusted/ci", source, "4" * 40, 1482,
                        "ci/test-plans/java-pr1482.json", root,
                    )
                verify_source_pr.assert_not_called()

    def test_publish_binds_receipt_branch_and_authorization_ref(self):
        carrier = contract()
        writes = []
        blob_payloads = []
        commit_payload = {}

        def api(route, method="GET", payload=None):
            writes.append((method, route, payload))
            if method == "POST" and route.endswith("/git/blobs"):
                data = base64.b64decode(payload["content"], validate=True)
                blob_payloads.append(data)
                return {"sha": v3.git_blob_sha(data)}
            if method == "POST" and route.endswith("/git/trees"):
                if payload.get("base_tree") is None:
                    return {"sha": SOURCE_TREE}
                return {"sha": ROOT_TREE}
            if method == "POST" and route.endswith("/git/commits"):
                commit_payload.update(payload)
                return {"sha": COMMIT}
            if method == "POST" and route.endswith("/git/refs"):
                return {"ref": payload["ref"], "object": {"sha": payload["sha"]}}
            raise AssertionError((method, route))

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "snapshot"
            source.mkdir()
            (source / "hello.ores").write_text("synthetic\n")
            with (
                patch.object(v3.legacy, "require_preflight"),
                patch.object(v3.legacy, "checked_policy_fingerprint", return_value=POLICY),
                patch.object(
                    v3.legacy, "checked_policy_approval",
                    return_value=(POLICY, carrier["repository_id"]),
                ),
                patch.object(
                    v3, "authorized_main_sha", return_value=PROTECTED
                ) as verify_origin,
                patch.object(v3, "checked_control_plane") as control_plane,
                patch.object(v3, "checked_contract", return_value=carrier) as checked_contract,
                patch.object(
                    v3,
                    "verify_source_pr",
                    return_value=v3.SOURCE_REPOSITORY_ID,
                ) as verify_source_pr,
                patch.object(
                    v3,
                    "checked_destination",
                    return_value=(
                        {
                            "id": carrier["repository_id"],
                            "private": True,
                            "visibility": "private",
                        },
                        "3" * 40,
                        PLAN,
                    ),
                ) as checked_destination,
                patch.object(
                    v3.legacy,
                    "collect_source",
                    return_value=[("hello.ores", b"synthetic\n", "100644")],
                ),
                patch.object(
                    v3.legacy,
                    "verified_exact_source_tree",
                    return_value=SOURCE_TREE,
                ),
                patch.object(v3, "authorize_write") as authorize_write,
                patch.object(v3, "require_exact_ref") as require_exact_ref,
                patch.object(v3.legacy, "gh_api", side_effect=api),
            ):
                result = v3.publish(
                    "trusted/ci",
                    source,
                    "4" * 40,
                    1482,
                    "ci/test-plans/java-pr1482.json",
                    root,
                    now=dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc),
                )

        self.assertEqual(verify_origin.call_count, 3)
        self.assertEqual(control_plane.call_count, 3)
        self.assertEqual(checked_contract.call_count, 3)
        self.assertEqual(checked_destination.call_count, 3)
        self.assertEqual(verify_source_pr.call_count, 3)
        self.assertEqual(authorize_write.call_count, 7)

        self.assertEqual(result["branch"], "ci/oreslang-" + SOURCE_TREE)
        self.assertEqual(
            result["authorization_ref"], "ci/authorized-" + COMMIT
        )
        receipt_bytes = next(
            item for item in blob_payloads if b'"schema":"ores-private-ci-receipt-v3"' in item
        )
        receipt = json.loads(receipt_bytes)
        self.assertEqual(receipt["expires_at"], "2026-10-21T00:00:00Z")
        self.assertEqual(receipt["source"]["pr"], 1482)
        self.assertEqual(
            receipt["source"]["repo_id"], v3.SOURCE_REPOSITORY_ID)
        self.assertEqual(receipt["source"]["head_sha"], "4" * 40)
        self.assertEqual(receipt["source"]["root_tree_sha"], SOURCE_TREE)
        self.assertEqual(receipt["mirror"]["root_tree_sha"], SOURCE_TREE)
        self.assertEqual(receipt["destination"]["repo_id"], 1323598177)
        self.assertEqual(receipt["test_plan"]["path"], carrier["test_plan_path"])
        self.assertEqual(
            receipt["test_plan"]["sha256"], hashlib.sha256(PLAN).hexdigest()
        )
        digest = v3.canonical_digest(receipt)
        self.assertEqual(result["receipt_sha256"], digest)
        self.assertEqual(
            commit_payload["message"],
            (
                "ci: exact private Oreslang tree "
                + "4" * 40
                + " tree "
                + SOURCE_TREE
                + "\ncarrier-base "
                + BASE
                + " tree "
                + "3" * 40
                + "\nconnector-authorized source-repository "
                + v3.legacy.SOURCE_REPO
                + " id "
                + str(v3.SOURCE_REPOSITORY_ID)
                + "\nOreslang-CI-Receipt-SHA256: "
                + digest
            ),
        )
        self.assertIn(
            "Oreslang-CI-Receipt-SHA256: " + digest,
            commit_payload["message"].splitlines(),
        )
        refs = [
            payload["ref"]
            for method, route, payload in writes
            if method == "POST" and route.endswith("/git/refs")
        ]
        self.assertEqual(
            refs,
            [
                "refs/heads/ci/oreslang-" + SOURCE_TREE,
                "refs/heads/ci/authorized-" + COMMIT,
            ],
        )
        self.assertEqual(verify_source_pr.call_count, 3)
        self.assertEqual(
            [call.args for call in require_exact_ref.call_args_list],
            [
                (
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT,
                    carrier["repository_id"],
                ),
                (
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT,
                    carrier["repository_id"],
                ),
                (
                    "trusted/ci", "ci/authorized-" + COMMIT, COMMIT,
                    carrier["repository_id"],
                ),
            ],
        )

    def test_publish_rolls_back_payload_if_protection_changes_before_authorization(self):
        carrier = contract()

        def api(route, method="GET", payload=None):
            if method == "POST" and route.endswith("/git/blobs"):
                data = base64.b64decode(payload["content"], validate=True)
                return {"sha": v3.git_blob_sha(data)}
            if method == "POST" and route.endswith("/git/trees"):
                return {"sha": SOURCE_TREE if payload.get("base_tree") is None else ROOT_TREE}
            if method == "POST" and route.endswith("/git/commits"):
                return {"sha": COMMIT}
            if method == "POST" and route.endswith("/git/refs"):
                return {"ref": payload["ref"], "object": {"sha": payload["sha"]}}
            raise AssertionError((method, route))

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "snapshot"
            source.mkdir()
            (source / "hello.ores").write_text("synthetic\n")
            with (
                patch.object(v3.legacy, "require_preflight"),
                patch.object(v3.legacy, "checked_policy_fingerprint", return_value=POLICY),
                patch.object(
                    v3.legacy, "checked_policy_approval",
                    return_value=(POLICY, carrier["repository_id"]),
                ),
                patch.object(
                    v3,
                    "authorized_main_sha",
                    side_effect=[PROTECTED, PROTECTED, v3.Denied("protection revoked")],
                ),
                patch.object(v3, "checked_control_plane"),
                patch.object(v3, "checked_contract", return_value=carrier),
                patch.object(v3, "verify_source_pr", return_value=v3.SOURCE_REPOSITORY_ID),
                patch.object(
                    v3, "checked_destination",
                    return_value=(
                        {"id": carrier["repository_id"], "private": True, "visibility": "private"},
                        "3" * 40,
                        PLAN,
                    ),
                ),
                patch.object(
                    v3.legacy, "collect_source",
                    return_value=[("hello.ores", b"synthetic\n", "100644")],
                ),
                patch.object(v3.legacy, "verified_exact_source_tree", return_value=SOURCE_TREE),
                patch.object(v3, "authorize_write"),
                patch.object(v3, "require_exact_ref"),
                patch.object(v3.legacy, "gh_api", side_effect=api),
                patch.object(v3, "delete_exact_ref") as delete_ref,
            ):
                with self.assertRaisesRegex(v3.Denied, "protection revoked"):
                    v3.publish(
                        "trusted/ci", source, "4" * 40, 1482,
                        "ci/test-plans/java-pr1482.json", root,
                        now=dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc),
                    )

        delete_ref.assert_called_once_with(
            "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT,
            carrier["repository_id"]
        )

    def test_publish_rolls_back_payload_if_pr_head_moves_before_authorization(self):
        carrier = contract()
        writes = []

        def api(route, method="GET", payload=None):
            writes.append((method, route, payload))
            if method == "POST" and route.endswith("/git/blobs"):
                data = base64.b64decode(payload["content"], validate=True)
                return {"sha": v3.git_blob_sha(data)}
            if method == "POST" and route.endswith("/git/trees"):
                return {"sha": SOURCE_TREE if payload.get("base_tree") is None else ROOT_TREE}
            if method == "POST" and route.endswith("/git/commits"):
                return {"sha": COMMIT}
            if method == "POST" and route.endswith("/git/refs"):
                return {"ref": payload["ref"], "object": {"sha": payload["sha"]}}
            raise AssertionError((method, route))

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "snapshot"
            source.mkdir()
            (source / "hello.ores").write_text("synthetic\n")
            verify_calls = [v3.SOURCE_REPOSITORY_ID, v3.SOURCE_REPOSITORY_ID, v3.Denied("PR head moved")]
            with (
                patch.object(v3.legacy, "require_preflight"),
                patch.object(v3.legacy, "checked_policy_fingerprint", return_value=POLICY),
                patch.object(
                    v3.legacy, "checked_policy_approval",
                    return_value=(POLICY, carrier["repository_id"]),
                ),
                patch.object(v3, "authorized_main_sha", return_value=PROTECTED),
                patch.object(v3, "checked_control_plane"),
                patch.object(v3, "checked_contract", return_value=carrier),
                patch.object(v3, "verify_source_pr", side_effect=verify_calls),
                patch.object(
                    v3, "checked_destination",
                    return_value=(
                        {"id": carrier["repository_id"], "private": True, "visibility": "private"},
                        "3" * 40,
                        PLAN,
                    ),
                ),
                patch.object(
                    v3.legacy, "collect_source",
                    return_value=[("hello.ores", b"synthetic\n", "100644")],
                ),
                patch.object(v3.legacy, "verified_exact_source_tree", return_value=SOURCE_TREE),
                patch.object(v3, "authorize_write"),
                patch.object(v3, "require_exact_ref"),
                patch.object(v3.legacy, "gh_api", side_effect=api),
                patch.object(v3, "delete_exact_ref") as delete_ref,
            ):
                with self.assertRaisesRegex(v3.Denied, "PR head moved"):
                    v3.publish(
                        "trusted/ci", source, "4" * 40, 1482,
                        "ci/test-plans/java-pr1482.json", root,
                        now=dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc),
                    )

        delete_ref.assert_called_once_with(
            "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT,
            carrier["repository_id"]
        )
        ref_posts = [
            payload["ref"]
            for method, route, payload in writes
            if method == "POST" and route.endswith("/git/refs")
        ]
        self.assertEqual(ref_posts, ["refs/heads/ci/oreslang-" + SOURCE_TREE])

    def test_publish_rolls_back_payload_if_authorization_creation_fails(self):
        carrier = contract()

        def api(route, method="GET", payload=None):
            if method == "POST" and route.endswith("/git/blobs"):
                data = base64.b64decode(payload["content"], validate=True)
                return {"sha": v3.git_blob_sha(data)}
            if method == "POST" and route.endswith("/git/trees"):
                return {"sha": SOURCE_TREE if payload.get("base_tree") is None else ROOT_TREE}
            if method == "POST" and route.endswith("/git/commits"):
                return {"sha": COMMIT}
            if method == "POST" and route.endswith("/git/refs"):
                if payload["ref"].startswith("refs/heads/ci/authorized-"):
                    raise v3.Denied("authorization creation failed")
                return {"ref": payload["ref"], "object": {"sha": payload["sha"]}}
            raise AssertionError((method, route))

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "snapshot"
            source.mkdir()
            (source / "hello.ores").write_text("synthetic\n")
            with (
                patch.object(v3.legacy, "require_preflight"),
                patch.object(v3.legacy, "checked_policy_fingerprint", return_value=POLICY),
                patch.object(
                    v3.legacy, "checked_policy_approval",
                    return_value=(POLICY, carrier["repository_id"]),
                ),
                patch.object(v3, "authorized_main_sha", return_value=PROTECTED),
                patch.object(v3, "checked_control_plane"),
                patch.object(v3, "checked_contract", return_value=carrier),
                patch.object(v3, "verify_source_pr", return_value=v3.SOURCE_REPOSITORY_ID),
                patch.object(
                    v3, "checked_destination",
                    return_value=(
                        {"id": carrier["repository_id"], "private": True, "visibility": "private"},
                        "3" * 40,
                        PLAN,
                    ),
                ),
                patch.object(
                    v3.legacy, "collect_source",
                    return_value=[("hello.ores", b"synthetic\n", "100644")],
                ),
                patch.object(v3.legacy, "verified_exact_source_tree", return_value=SOURCE_TREE),
                patch.object(v3, "authorize_write"),
                patch.object(v3, "require_exact_ref"),
                patch.object(v3.legacy, "gh_api", side_effect=api),
                patch.object(v3, "delete_exact_ref") as delete_ref,
            ):
                with self.assertRaisesRegex(v3.Denied, "authorization creation failed"):
                    v3.publish(
                        "trusted/ci", source, "4" * 40, 1482,
                        "ci/test-plans/java-pr1482.json", root,
                        now=dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc),
                    )

        self.assertEqual(
            [call.args for call in delete_ref.call_args_list],
            [
                (
                    "trusted/ci", "ci/authorized-" + COMMIT, COMMIT,
                    carrier["repository_id"],
                ),
                (
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT,
                    carrier["repository_id"],
                ),
            ],
        )

    def test_publish_rolls_back_both_refs_on_keyboard_interrupt_during_authorization_post(self):
        carrier = contract()

        def api(route, method="GET", payload=None):
            if method == "POST" and route.endswith("/git/blobs"):
                data = base64.b64decode(payload["content"], validate=True)
                return {"sha": v3.git_blob_sha(data)}
            if method == "POST" and route.endswith("/git/trees"):
                return {"sha": SOURCE_TREE if payload.get("base_tree") is None else ROOT_TREE}
            if method == "POST" and route.endswith("/git/commits"):
                return {"sha": COMMIT}
            if method == "POST" and route.endswith("/git/refs"):
                if payload["ref"].startswith("refs/heads/ci/authorized-"):
                    raise KeyboardInterrupt()
                return {"ref": payload["ref"], "object": {"sha": payload["sha"]}}
            raise AssertionError((method, route))

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "snapshot"
            source.mkdir()
            (source / "hello.ores").write_text("synthetic\n")
            with (
                patch.object(v3.legacy, "require_preflight"),
                patch.object(v3.legacy, "checked_policy_fingerprint", return_value=POLICY),
                patch.object(
                    v3.legacy, "checked_policy_approval",
                    return_value=(POLICY, carrier["repository_id"]),
                ),
                patch.object(v3, "authorized_main_sha", return_value=PROTECTED),
                patch.object(v3, "checked_control_plane"),
                patch.object(v3, "checked_contract", return_value=carrier),
                patch.object(v3, "verify_source_pr", return_value=v3.SOURCE_REPOSITORY_ID),
                patch.object(
                    v3, "checked_destination",
                    return_value=(
                        {"id": carrier["repository_id"], "private": True, "visibility": "private"},
                        "3" * 40,
                        PLAN,
                    ),
                ),
                patch.object(
                    v3.legacy, "collect_source",
                    return_value=[("hello.ores", b"synthetic\n", "100644")],
                ),
                patch.object(v3.legacy, "verified_exact_source_tree", return_value=SOURCE_TREE),
                patch.object(v3, "authorize_write"),
                patch.object(v3, "require_exact_ref"),
                patch.object(v3.legacy, "gh_api", side_effect=api),
                patch.object(v3, "delete_exact_ref") as delete_ref,
            ):
                with self.assertRaises(KeyboardInterrupt):
                    v3.publish(
                        "trusted/ci", source, "4" * 40, 1482,
                        "ci/test-plans/java-pr1482.json", root,
                        now=dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc),
                    )

        self.assertEqual(
            [call.args for call in delete_ref.call_args_list],
            [
                (
                    "trusted/ci", "ci/authorized-" + COMMIT, COMMIT,
                    carrier["repository_id"],
                ),
                (
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT,
                    carrier["repository_id"],
                ),
            ],
        )

    def test_publish_rolls_back_both_refs_on_authorization_confirmation_mismatch(self):
        carrier = contract()

        def api(route, method="GET", payload=None):
            if method == "POST" and route.endswith("/git/blobs"):
                data = base64.b64decode(payload["content"], validate=True)
                return {"sha": v3.git_blob_sha(data)}
            if method == "POST" and route.endswith("/git/trees"):
                return {"sha": SOURCE_TREE if payload.get("base_tree") is None else ROOT_TREE}
            if method == "POST" and route.endswith("/git/commits"):
                return {"sha": COMMIT}
            if method == "POST" and route.endswith("/git/refs"):
                if payload["ref"].startswith("refs/heads/ci/authorized-"):
                    return {"ref": payload["ref"], "object": {"sha": "9" * 40}}
                return {"ref": payload["ref"], "object": {"sha": payload["sha"]}}
            raise AssertionError((method, route))

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "snapshot"
            source.mkdir()
            (source / "hello.ores").write_text("synthetic\n")
            with (
                patch.object(v3.legacy, "require_preflight"),
                patch.object(v3.legacy, "checked_policy_fingerprint", return_value=POLICY),
                patch.object(
                    v3.legacy, "checked_policy_approval",
                    return_value=(POLICY, carrier["repository_id"]),
                ),
                patch.object(v3, "authorized_main_sha", return_value=PROTECTED),
                patch.object(v3, "checked_control_plane"),
                patch.object(v3, "checked_contract", return_value=carrier),
                patch.object(v3, "verify_source_pr", return_value=v3.SOURCE_REPOSITORY_ID),
                patch.object(
                    v3, "checked_destination",
                    return_value=(
                        {"id": carrier["repository_id"], "private": True, "visibility": "private"},
                        "3" * 40,
                        PLAN,
                    ),
                ),
                patch.object(
                    v3.legacy, "collect_source",
                    return_value=[("hello.ores", b"synthetic\n", "100644")],
                ),
                patch.object(v3.legacy, "verified_exact_source_tree", return_value=SOURCE_TREE),
                patch.object(v3, "authorize_write"),
                patch.object(v3, "require_exact_ref"),
                patch.object(v3.legacy, "gh_api", side_effect=api),
                patch.object(v3, "delete_exact_ref") as delete_ref,
            ):
                with self.assertRaisesRegex(v3.Denied, "dispatcher authorization ref"):
                    v3.publish(
                        "trusted/ci", source, "4" * 40, 1482,
                        "ci/test-plans/java-pr1482.json", root,
                        now=dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc),
                    )

        self.assertEqual(
            [call.args for call in delete_ref.call_args_list],
            [
                (
                    "trusted/ci", "ci/authorized-" + COMMIT, COMMIT,
                    carrier["repository_id"],
                ),
                (
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT,
                    carrier["repository_id"],
                ),
            ],
        )

    def test_publish_rolls_back_both_refs_on_unexpected_post_confirmation_error(self):
        carrier = contract()
        ref_posts = []

        def api(route, method="GET", payload=None):
            if method == "POST" and route.endswith("/git/blobs"):
                data = base64.b64decode(payload["content"], validate=True)
                return {"sha": v3.git_blob_sha(data)}
            if method == "POST" and route.endswith("/git/trees"):
                return {"sha": SOURCE_TREE if payload.get("base_tree") is None else ROOT_TREE}
            if method == "POST" and route.endswith("/git/commits"):
                return {"sha": COMMIT}
            if method == "POST" and route.endswith("/git/refs"):
                ref_posts.append(payload["ref"])
                return {"ref": payload["ref"], "object": {"sha": payload["sha"]}}
            raise AssertionError((method, route))

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "snapshot"
            source.mkdir()
            (source / "hello.ores").write_text("synthetic\n")
            with (
                patch.object(v3.legacy, "require_preflight"),
                patch.object(v3.legacy, "checked_policy_fingerprint", return_value=POLICY),
                patch.object(
                    v3.legacy, "checked_policy_approval",
                    return_value=(POLICY, carrier["repository_id"]),
                ),
                patch.object(v3, "authorized_main_sha", return_value=PROTECTED),
                patch.object(v3, "checked_control_plane"),
                patch.object(v3, "checked_contract", return_value=carrier),
                patch.object(v3, "verify_source_pr", return_value=v3.SOURCE_REPOSITORY_ID),
                patch.object(
                    v3, "checked_destination",
                    return_value=(
                        {"id": carrier["repository_id"], "private": True, "visibility": "private"},
                        "3" * 40,
                        PLAN,
                    ),
                ),
                patch.object(
                    v3.legacy, "collect_source",
                    return_value=[("hello.ores", b"synthetic\n", "100644")],
                ),
                patch.object(v3.legacy, "verified_exact_source_tree", return_value=SOURCE_TREE),
                patch.object(v3, "authorize_write"),
                patch.object(
                    v3, "require_exact_ref",
                    side_effect=[None, RuntimeError("unexpected confirmation error")],
                ),
                patch.object(v3.legacy, "gh_api", side_effect=api),
                patch.object(v3, "delete_exact_ref") as delete_ref,
            ):
                with self.assertRaisesRegex(RuntimeError, "unexpected confirmation error"):
                    v3.publish(
                        "trusted/ci", source, "4" * 40, 1482,
                        "ci/test-plans/java-pr1482.json", root,
                        now=dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc),
                    )

        self.assertEqual(
            ref_posts,
            [
                "refs/heads/ci/oreslang-" + SOURCE_TREE,
                "refs/heads/ci/authorized-" + COMMIT,
            ],
        )
        self.assertEqual(
            [call.args for call in delete_ref.call_args_list],
            [
                (
                    "trusted/ci", "ci/authorized-" + COMMIT, COMMIT,
                    carrier["repository_id"],
                ),
                (
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT,
                    carrier["repository_id"],
                ),
            ],
        )

    def test_ref_request_accepts_exact_authorization_ref_and_uses_get_route(self):
        response = type(
            "Result", (),
            {"returncode": 0, "stderr": "", "stdout": "{}"},
        )()
        ref_name = "ci/authorized-" + COMMIT
        with (
            patch.object(v3, "trusted_github_cli", return_value="/usr/bin/gh"),
            patch.object(v3, "github_api_environment", return_value={"GH_TOKEN": "x"}),
            patch.object(v3, "run_bounded_gh", return_value=response) as run,
        ):
            self.assertIs(
                v3._gh_ref_request("trusted/ci", ref_name, "GET"),
                response,
            )
        argv = run.call_args.args[0]
        self.assertEqual(argv[0], "/usr/bin/gh")
        self.assertIn(
            "repos/trusted/ci/git/ref/heads/ci%2Fauthorized-" + COMMIT,
            argv,
        )
        self.assertEqual(run.call_args.kwargs["env"], {"GH_TOKEN": "x"})

    def test_ref_request_uses_plural_delete_route(self):
        response = type(
            "Result", (),
            {"returncode": 0, "stderr": "", "stdout": ""},
        )()
        ref_name = "ci/authorized-" + COMMIT
        with (
            patch.object(v3, "trusted_github_cli", return_value="/usr/bin/gh"),
            patch.object(v3, "github_api_environment", return_value={"GITHUB_TOKEN": "x"}),
            patch.object(v3, "run_bounded_gh", return_value=response) as run,
        ):
            self.assertIs(
                v3._gh_ref_request("trusted/ci", ref_name, "DELETE"),
                response,
            )
        argv = run.call_args.args[0]
        self.assertIn(
            "repos/trusted/ci/git/refs/heads/ci%2Fauthorized-" + COMMIT,
            argv,
        )
        self.assertEqual(run.call_args.kwargs["env"], {"GITHUB_TOKEN": "x"})

    def test_github_api_environment_is_minimal_and_requires_one_token(self):
        with patch.dict(v3.os.environ, {}, clear=True):
            with self.assertRaisesRegex(v3.Denied, "exactly one"):
                v3.github_api_environment()
        with patch.dict(
            v3.os.environ,
            {"GH_TOKEN": "one", "GITHUB_TOKEN": "two", "AWS_SECRET_ACCESS_KEY": "leak"},
            clear=True,
        ):
            with self.assertRaisesRegex(v3.Denied, "exactly one"):
                v3.github_api_environment()
        with patch.dict(
            v3.os.environ,
            {"GITHUB_TOKEN": "one", "AWS_SECRET_ACCESS_KEY": "leak", "HOME": "/tmp/leak"},
            clear=True,
        ):
            env = v3.github_api_environment()
        self.assertEqual(env["GITHUB_TOKEN"], "one")
        self.assertEqual(env["PATH"], "/usr/bin:/bin")
        self.assertEqual(env["HOME"], "/nonexistent")
        self.assertEqual(env["XDG_CONFIG_HOME"], "/nonexistent")
        self.assertEqual(env["GIT_CONFIG_NOSYSTEM"], "1")
        self.assertEqual(env["GIT_CONFIG_GLOBAL"], "/dev/null")
        self.assertNotIn("AWS_SECRET_ACCESS_KEY", env)
        self.assertEqual(
            set(env),
            {
                "GITHUB_TOKEN", "GH_HOST", "GH_PROMPT_DISABLED",
                "PATH", "HOME", "XDG_CONFIG_HOME", "LC_ALL",
                "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL",
            },
        )

    def test_http_status_match_is_first_line_and_exact(self):
        self.assertTrue(v3._is_http_status("gh: Not Found (HTTP 404)\nbody", 404))
        self.assertFalse(v3._is_http_status("body mentions HTTP 404", 404))
        self.assertFalse(v3._is_http_status("gh: Not Found (HTTP 404)", 404))
        self.assertFalse(v3._is_http_status("gh: forged reason (HTTP 404)\nbody", 404))
        self.assertFalse(v3._is_http_status("gh: Forbidden (HTTP 403)", 404))

    def test_bounded_gh_rejects_output_at_limit(self):
        def fake_run(argv, **kwargs):
            kwargs["stdout"].write(b"x" * 8)
            kwargs["stderr"].write(b"")
            return v3.subprocess.CompletedProcess(argv, 0)

        with (
            patch.object(v3, "MAX_GH_API_RESPONSE_BYTES", 8),
            patch.object(v3.subprocess, "run", side_effect=fake_run),
        ):
            with self.assertRaisesRegex(v3.Denied, "exceeds size limit"):
                v3.run_bounded_gh(["/usr/bin/gh"], env={}, timeout=1)

    def test_ref_request_rejects_unrelated_ci_namespace(self):
        with self.assertRaisesRegex(v3.Denied, "target is invalid"):
            v3._gh_ref_request("trusted/ci", "ci/not-an-oreslang-handoff", "GET")

    def test_live_ref_404_requires_carrier_identity_to_remain_visible(self):
        response = type(
            "Result", (),
            {"returncode": 1, "stderr": "gh: Not Found (HTTP 404)\nbody", "stdout": ""},
        )()
        with (
            patch.object(v3, "_gh_ref_request", return_value=response),
            patch.object(
                v3.legacy,
                "checked_repo",
                side_effect=v3.Denied("carrier no longer visible"),
            ),
        ):
            with self.assertRaisesRegex(v3.Denied, "no longer visible"):
                v3.live_ref_sha(
                    "trusted/ci", "ci/authorized-" + COMMIT, 1323598177
                )

    def test_live_ref_rejects_carrier_repository_id_replacement(self):
        response = type(
            "Result", (),
            {"returncode": 0, "stderr": "", "stdout": "{}"},
        )()
        with (
            patch.object(v3, "_gh_ref_request", return_value=response),
            patch.object(
                v3.legacy,
                "checked_repo",
                return_value={
                    "id": 999,
                    "full_name": "trusted/ci",
                    "private": True,
                    "visibility": "private",
                },
            ),
        ):
            with self.assertRaisesRegex(v3.Denied, "repository identity changed"):
                v3.live_ref_sha(
                    "trusted/ci", "ci/authorized-" + COMMIT, 1323598177
                )

    def test_live_ref_404_is_absent_only_after_private_carrier_recheck(self):
        response = type(
            "Result", (),
            {"returncode": 1, "stderr": "gh: Not Found (HTTP 404)\nbody", "stdout": ""},
        )()
        with (
            patch.object(v3, "_gh_ref_request", return_value=response),
            patch.object(
                v3.legacy,
                "checked_repo",
                return_value={
                    "id": 1323598177,
                    "full_name": "trusted/ci",
                    "private": True,
                    "visibility": "private",
                },
            ) as checked_repo,
        ):
            self.assertIsNone(
                v3.live_ref_sha(
                    "trusted/ci", "ci/authorized-" + COMMIT, 1323598177
                )
            )
        checked_repo.assert_called_once_with("trusted/ci", 1323598177)

    def test_delete_exact_ref_missing_is_idempotent(self):
        with patch.object(v3, "live_ref_sha", return_value=None):
            self.assertFalse(
                v3.delete_exact_ref(
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT, 1323598177
                )
            )

    def test_delete_exact_ref_refuses_moved_ref(self):
        with patch.object(v3, "live_ref_sha", return_value="9" * 40):
            with self.assertRaisesRegex(v3.Denied, "moved"):
                v3.delete_exact_ref(
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT, 1323598177
                )

    def test_delete_exact_ref_verifies_absence_after_delete(self):
        response = type("Result", (), {"returncode": 0, "stderr": ""})()
        with (
            patch.object(v3, "live_ref_sha", side_effect=[COMMIT, None]),
            patch.object(v3, "_gh_ref_request", return_value=response) as request,
        ):
            self.assertTrue(
                v3.delete_exact_ref(
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT, 1323598177
                )
            )
        request.assert_called_once_with(
            "trusted/ci", "ci/oreslang-" + SOURCE_TREE, "DELETE"
        )

    def test_delete_exact_ref_rejects_still_present_after_delete(self):
        response = type("Result", (), {"returncode": 0, "stderr": ""})()
        with (
            patch.object(v3, "live_ref_sha", side_effect=[COMMIT, COMMIT]),
            patch.object(v3, "_gh_ref_request", return_value=response),
        ):
            with self.assertRaisesRegex(v3.Denied, "still exists"):
                v3.delete_exact_ref(
                    "trusted/ci", "ci/oreslang-" + SOURCE_TREE, COMMIT, 1323598177
                )

    def test_manifest_matches_carrier_row_shape(self):
        rows, digest = v3.manifest(
            [
                ("b", b"two", "100755"),
                ("a", b"one", "100644"),
            ]
        )
        self.assertEqual([row[0] for row in rows], ["a", "b"])
        self.assertEqual(
            digest, hashlib.sha256(v3.canonical_bytes(rows)).hexdigest()
        )

    def test_checked_in_contract_pins_pr616_private_carrier_exactly(self):
        path = SCRIPT.parent.parent / ".github" / "private-ci-carrier-contracts.json"
        doc = json.loads(path.read_text())
        carrier = doc["carriers"][
            "ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-infra"
        ]
        self.assertEqual(carrier["repository_id"], 1412142815)
        self.assertEqual(
            carrier["base_sha"],
            "90ab94fc768be6b2acae313b5537f642f63321cf",
        )
        self.assertEqual(
            carrier["workflow_path"],
            ".github/workflows/private-oreslang-exact-tree.yml",
        )
        self.assertEqual(
            carrier["workflow_blob_sha"],
            "3107feea0215ae9874ff35a2249c69ab6b9e7ffe",
        )
        self.assertEqual(
            carrier["test_plans"]["ci/test-plans/java-pr616.json"],
            {
                "blob_sha": "4728c2ecb583a809f6978db7a0235cef771a89f7",
                "allowed_prs": [616],
            },
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)

