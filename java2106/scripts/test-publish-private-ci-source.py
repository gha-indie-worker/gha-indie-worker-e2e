#!/usr/bin/env python3
"""Offline adversarial tests for the private destination-bound publisher."""
import importlib.util
import base64
import hashlib
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).with_name("publish-private-ci-source.py")
spec = importlib.util.spec_from_file_location("private_publisher", MODULE)
pub = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pub)
ORIGINAL_VERIFY_PROTECTED_ORIGIN = pub.verify_protected_origin
ORIGINAL_VERIFY_EXACT_SOURCE_TREE = pub.verified_exact_source_tree
ORIGINAL_REQUIRE_PREFLIGHT = pub.require_preflight
ORIGINAL_REQUIRE_RESOLVED = pub.require_resolved_blocking_incidents
A, B, C, D, E = ("a"*40, "b"*40, "c"*40, "d"*40, "e"*40)


class DestinationBoundPublisherTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.root / ".github").mkdir()
        self.approval = self.root / ".github" / "private-ci-approved-repositories.txt"
        self.approval.write_text("trusted/ci 246810\n")
        (self.source / "hello.ores").write_text("pub fnc main(): int { return 42; }\n")
        self.route_calls = []
        self.visibility = ["private"] * 100
        self.preflight = patch.object(pub, "require_preflight")
        self.preflight.start()
        self.addCleanup(self.preflight.stop)
        # Other publisher tests exercise the downstream write path. The real
        # incident API predicate is tested separately with identity fixtures.
        self.incident_guard = patch.object(pub, "require_resolved_blocking_incidents")
        self.incident_mock = self.incident_guard.start()
        self.addCleanup(self.incident_guard.stop)
        # Existing isolated transfer/FS tests model the downstream publisher;
        # protected-source provenance gets independent real-guard tests below.
        self.origin_guard = patch.object(pub, "verify_protected_origin")
        self.origin_guard.start()
        self.addCleanup(self.origin_guard.stop)
        # Existing transfer/FS tests verify post-attestation behavior; the
        # source Git-tree verifier is tested separately against mocked origin.
        self.source_tree_guard = patch.object(
            pub, "verified_exact_source_tree", return_value=D)
        self.source_tree_mock = self.source_tree_guard.start()
        self.addCleanup(self.source_tree_guard.stop)

    def incident_api(self, states=None, repository_id=None, issue_ids=None):
        states = states or {
            22: ("closed", "completed"),
            77: ("closed", "completed"),
            127: ("closed", "completed"),
            171: ("closed", "completed"),
            172: ("closed", "completed"),
        }
        repository_id = (
            pub.BLOCKING_INCIDENT_REPOSITORY_ID
            if repository_id is None else repository_id
        )
        issue_ids = dict(pub.BLOCKING_INCIDENT_IDS if issue_ids is None else issue_ids)
        def api(route, method="GET", payload=None):
            if route == "repos/" + pub.BLOCKING_INCIDENT_REPO:
                return {
                    "id": repository_id,
                    "full_name": pub.BLOCKING_INCIDENT_REPO,
                    "private": True,
                    "visibility": "private",
                    "archived": False,
                    "disabled": False,
                    "fork": False,
                }
            prefix = "repos/" + pub.BLOCKING_INCIDENT_REPO + "/issues/"
            if route.startswith(prefix):
                number = int(route[len(prefix):])
                state, reason = states[number]
                repository_url = (
                    "https://api.github.com/repos/"
                    + pub.BLOCKING_INCIDENT_REPO
                )
                return {
                    "id": issue_ids[number],
                    "number": number,
                    "repository_url": repository_url,
                    "url": repository_url + "/issues/" + str(number),
                    "html_url": (
                        "https://github.com/"
                        + pub.BLOCKING_INCIDENT_REPO
                        + "/issues/"
                        + str(number)
                    ),
                    "state": state,
                    "state_reason": reason,
                }
            raise AssertionError((method, route))
        return api

    def test_blocking_p0_incidents_accept_only_completed_closure(self):
        with patch.object(pub, "gh_api", side_effect=self.incident_api()):
            ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_open_issue(self):
        states = {
            22: ("closed", "completed"),
            77: ("open", None),
        }
        with patch.object(pub, "gh_api", side_effect=self.incident_api(states)):
            with self.assertRaisesRegex(pub.Denied, "#77 remains unresolved"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_open_issue_127(self):
        states = {
            22: ("closed", "completed"),
            77: ("closed", "completed"),
            127: ("open", None),
        }
        with patch.object(pub, "gh_api", side_effect=self.incident_api(states)):
            with self.assertRaisesRegex(pub.Denied, "#127 remains unresolved"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_not_planned_closure(self):
        states = {
            22: ("closed", "not_planned"),
            77: ("closed", "completed"),
        }
        with patch.object(pub, "gh_api", side_effect=self.incident_api(states)):
            with self.assertRaisesRegex(pub.Denied, "#22 remains unresolved"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_issue_identity_drift(self):
        ids = dict(pub.BLOCKING_INCIDENT_IDS)
        ids[22] += 1
        with patch.object(
            pub, "gh_api",
            side_effect=self.incident_api(issue_ids=ids),
        ):
            with self.assertRaisesRegex(pub.Denied, "incident identity"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_issue_127_identity_drift(self):
        ids = dict(pub.BLOCKING_INCIDENT_IDS)
        ids[127] += 1
        with patch.object(
            pub, "gh_api",
            side_effect=self.incident_api(issue_ids=ids),
        ):
            with self.assertRaisesRegex(pub.Denied, "incident identity"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_repository_identity_drift(self):
        with patch.object(
            pub, "gh_api",
            side_effect=self.incident_api(
                repository_id=pub.BLOCKING_INCIDENT_REPOSITORY_ID + 1
            ),
        ):
            with self.assertRaisesRegex(pub.Denied, "repository identity/state"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_quarantined_repository_id_rejected_even_after_slug_rename(self):
        renamed = b"renamed-owner/renamed-carrier 1323575966\n"
        with self.assertRaisesRegex(pub.Denied, "repository ID is quarantined"):
            pub._parse_policy_approval(
                renamed,
                "renamed-owner/renamed-carrier",
                "private CI approval policy",
            )

        metadata = {
            "id": 1323575966,
            "full_name": "renamed-owner/renamed-carrier",
            "visibility": "private",
            "private": True,
            "archived": False,
            "disabled": False,
            "fork": False,
            "default_branch": "main",
        }
        with patch.object(pub, "gh_api", return_value=metadata):
            with self.assertRaisesRegex(pub.Denied, "repository ID is quarantined"):
                pub.checked_repo("renamed-owner/renamed-carrier", 1323575966)

    def test_quarantined_incident_carrier_rejected_before_incident_or_local_guard(self):
        with (
            patch.object(pub, "require_resolved_blocking_incidents") as incidents,
            patch.object(pub.subprocess, "run") as run,
            patch.object(pub, "gh_api") as api,
        ):
            with self.assertRaisesRegex(pub.Denied, "quarantined"):
                ORIGINAL_REQUIRE_PREFLIGHT("zed-pkg-test/java-app", self.root)
            incidents.assert_not_called()
            run.assert_not_called()
            api.assert_not_called()

    def test_blocking_p0_incidents_reject_open_issue_171(self):
        states = {
            22: ("closed", "completed"),
            77: ("closed", "completed"),
            127: ("closed", "completed"),
            171: ("open", None),
        }
        with patch.object(pub, "gh_api", side_effect=self.incident_api(states)):
            with self.assertRaisesRegex(pub.Denied, "#171 remains unresolved"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_issue_171_identity_drift(self):
        ids = dict(pub.BLOCKING_INCIDENT_IDS)
        ids[171] += 1
        with patch.object(
            pub, "gh_api", side_effect=self.incident_api(issue_ids=ids),
        ):
            with self.assertRaisesRegex(pub.Denied, "incident identity"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_issue_171_not_planned(self):
        states = {
            22: ("closed", "completed"),
            77: ("closed", "completed"),
            127: ("closed", "completed"),
            171: ("closed", "not_planned"),
        }
        with patch.object(pub, "gh_api", side_effect=self.incident_api(states)):
            with self.assertRaisesRegex(pub.Denied, "#171 remains unresolved"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_open_issue_172(self):
        states = {
            22: ("closed", "completed"),
            77: ("closed", "completed"),
            127: ("closed", "completed"),
            171: ("closed", "completed"),
            172: ("open", None),
        }
        with patch.object(pub, "gh_api", side_effect=self.incident_api(states)):
            with self.assertRaisesRegex(pub.Denied, "#172 remains unresolved"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_issue_172_identity_drift(self):
        ids = dict(pub.BLOCKING_INCIDENT_IDS)
        ids[172] += 1
        with patch.object(
            pub, "gh_api", side_effect=self.incident_api(issue_ids=ids),
        ):
            with self.assertRaisesRegex(pub.Denied, "incident identity"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_blocking_p0_incidents_reject_issue_172_not_planned(self):
        states = {
            22: ("closed", "completed"),
            77: ("closed", "completed"),
            127: ("closed", "completed"),
            171: ("closed", "completed"),
            172: ("closed", "not_planned"),
        }
        with patch.object(pub, "gh_api", side_effect=self.incident_api(states)):
            with self.assertRaisesRegex(pub.Denied, "#172 remains unresolved"):
                ORIGINAL_REQUIRE_RESOLVED()

    def test_quarantined_recovery_carrier_rejected_before_any_source_access(self):
        with (
            patch.object(pub, "require_resolved_blocking_incidents") as incidents,
            patch.object(pub, "checked_policy_approval") as policy,
            patch.object(pub, "checked_repo") as identity,
            patch.object(pub, "collect_source") as collect,
            patch.object(pub, "gh_api") as api,
        ):
            with self.assertRaisesRegex(pub.Denied, "quarantined"):
                ORIGINAL_REQUIRE_PREFLIGHT(
                    "agent-pontifex-test/agent-pontifex-e2e-tests", self.root,
                )
            incidents.assert_not_called()
            policy.assert_not_called()
            identity.assert_not_called()
            collect.assert_not_called()
            api.assert_not_called()

    def test_quarantined_llvm_carrier_rejected_before_any_source_access(self):
        with (
            patch.object(pub, "require_resolved_blocking_incidents") as incidents,
            patch.object(pub, "checked_policy_approval") as policy,
            patch.object(pub, "checked_repo") as identity,
            patch.object(pub, "collect_source") as collect,
            patch.object(pub, "gh_api") as api,
        ):
            with self.assertRaisesRegex(pub.Denied, "quarantined"):
                ORIGINAL_REQUIRE_PREFLIGHT("led-dynamo/leddy-e2e", self.root)
            incidents.assert_not_called()
            policy.assert_not_called()
            identity.assert_not_called()
            collect.assert_not_called()
            api.assert_not_called()

    def test_preflight_checks_blocking_incidents_before_local_guard(self):
        with (
            patch.object(
                pub, "require_resolved_blocking_incidents",
                side_effect=pub.Denied("blocking incident"),
            ),
            patch.object(pub.subprocess, "run") as run,
        ):
            with self.assertRaisesRegex(pub.Denied, "blocking incident"):
                ORIGINAL_REQUIRE_PREFLIGHT("trusted/ci", self.root)
            run.assert_not_called()

    def test_legacy_cli_is_unconditionally_disabled(self):
        with patch.object(pub, "publish") as publish:
            self.assertEqual(pub.main([]), 1)
            publish.assert_not_called()

    def test_github_api_environment_rejects_ambient_transport_and_config(self):
        hostile = {
            "GH_TOKEN": "token",
            "HTTP_PROXY": "http://attacker.invalid:8080",
            "https_proxy": "http://attacker.invalid:8080",
            "SSL_CERT_FILE": "/tmp/evil-ca.pem",
            "NODE_EXTRA_CA_CERTS": "/tmp/evil-node-ca.pem",
            "GH_CONFIG_DIR": "/tmp/evil-gh",
            "GITHUB_API_URL": "https://attacker.invalid/api/v3",
            "HOME": "/tmp/evil-home",
            "XDG_CONFIG_HOME": "/tmp/evil-xdg",
        }
        with patch.dict(os.environ, hostile, clear=True):
            env = pub.github_api_environment()
        self.assertEqual(env["GH_TOKEN"], "token")
        self.assertEqual(env["GH_HOST"], "github.com")
        self.assertEqual(env["HOME"], "/nonexistent")
        self.assertEqual(env["XDG_CONFIG_HOME"], "/nonexistent")
        self.assertEqual(env["GIT_CONFIG_NOSYSTEM"], "1")
        self.assertEqual(env["GIT_CONFIG_GLOBAL"], "/dev/null")
        for name in (
            "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY",
            "http_proxy", "https_proxy", "all_proxy", "no_proxy",
            "SSL_CERT_FILE", "SSL_CERT_DIR", "SSLKEYLOGFILE",
            "NODE_EXTRA_CA_CERTS", "REQUESTS_CA_BUNDLE", "CURL_CA_BUNDLE",
            "GH_CONFIG_DIR", "GH_ENTERPRISE_TOKEN", "GITHUB_API_URL",
        ):
            self.assertNotIn(name, env)

    def test_github_api_environment_requires_exactly_one_explicit_token(self):
        for values in ({}, {"GH_TOKEN": "a", "GITHUB_TOKEN": "b"}):
            with self.subTest(values=values), patch.dict(os.environ, values, clear=True):
                with self.assertRaisesRegex(pub.Denied, "exactly one explicit"):
                    pub.github_api_environment()

    def test_checked_repo_rejects_fork_archived_disabled_and_non_main(self):
        base = {
            "id": 246810,
            "full_name": "TRUSTED/CI",
            "visibility": "private",
            "private": True,
            "archived": False,
            "disabled": False,
            "fork": False,
            "default_branch": "main",
        }
        with patch.object(pub, "gh_api", return_value=dict(base)):
            self.assertEqual(pub.checked_repo("trusted/ci", 246810)["id"], 246810)
        for field, value in (
            ("archived", True),
            ("disabled", True),
            ("fork", True),
            ("default_branch", "develop"),
        ):
            changed = dict(base)
            changed[field] = value
            with self.subTest(field=field), patch.object(pub, "gh_api", return_value=changed):
                with self.assertRaisesRegex(pub.Denied, "identity/state"):
                    pub.checked_repo("trusted/ci", 246810)

    def test_preflight_treats_checkout_guard_as_data_only(self):
        digest = b"x" * 32
        with (
            patch.object(pub, "require_resolved_blocking_incidents") as incidents,
            patch.object(
                pub, "checked_policy_approval",
                return_value=(digest, 246810),
            ) as policy,
            patch.object(pub, "checked_repo", return_value={"id": 246810}) as repo,
            patch.object(pub.subprocess, "run") as run,
        ):
            ORIGINAL_REQUIRE_PREFLIGHT("trusted/ci", self.root)
            incidents.assert_called_once_with()
            policy.assert_called_once_with(self.root, "trusted/ci")
            repo.assert_called_once_with("trusted/ci", 246810)
            run.assert_not_called()

    def test_gh_api_uses_root_owned_binary_and_isolated_token_environment(self):
        process = type("Result", (), {
            "returncode": 0, "stdout": '{"ok":true}', "stderr": "",
        })()
        poisoned = {
            "GH_TOKEN": "synthetic-test-token",
            "PYTHONPATH": "/attacker",
            "GH_CONFIG_DIR": "/attacker",
            "HTTPS_PROXY": "http://attacker.invalid",
            "PATH": "/attacker",
            "HOME": "/attacker",
        }
        with (
            patch.dict(pub.os.environ, poisoned, clear=True),
            patch.object(pub, "trusted_github_cli", return_value="/usr/bin/gh") as cli,
            patch.object(pub.subprocess, "run", return_value=process) as run,
        ):
            self.assertEqual(pub.gh_api("repos/trusted/ci"), {"ok": True})
        cli.assert_called_once_with()
        argv = run.call_args.args[0]
        env = run.call_args.kwargs["env"]
        self.assertEqual(argv[0], "/usr/bin/gh")
        self.assertEqual(env["GH_TOKEN"], "synthetic-test-token")
        self.assertEqual(env["PATH"], "/usr/bin:/bin")
        self.assertEqual(env["HOME"], "/nonexistent")
        self.assertNotIn("PYTHONPATH", env)
        self.assertNotIn("GH_CONFIG_DIR", env)
        self.assertNotIn("HTTPS_PROXY", env)
        self.assertNotIn("synthetic-test-token", argv)

    def test_gh_api_denies_missing_or_ambiguous_credentials(self):
        for variables in ({}, {"GH_TOKEN": "a", "GITHUB_TOKEN": "b"}):
            with (
                self.subTest(tokens=sorted(variables)),
                patch.dict(pub.os.environ, variables, clear=True),
                patch.object(pub, "trusted_github_cli", return_value="/usr/bin/gh"),
                patch.object(pub.subprocess, "run") as run,
            ):
                with self.assertRaisesRegex(pub.Denied, "exactly one explicit"):
                    pub.gh_api("repos/trusted/ci")
                run.assert_not_called()

    def test_strict_github_json_rejects_duplicates_nonfinite_and_overflow(self):
        for payload in ('{"x":1,"x":2}', '{"x":NaN}', '{"x":Infinity}', '{"x":1e999}'):
            with self.subTest(payload=payload), self.assertRaises(pub.Denied):
                pub.strict_json_loads(payload)

        process = type("Result", (), {
            "returncode": 0, "stdout": '{"ok":true}', "stderr": "0123456789",
        })()
        with (
            patch.dict(pub.os.environ, {"GH_TOKEN": "synthetic"}, clear=True),
            patch.object(pub, "trusted_github_cli", return_value="/usr/bin/gh"),
            patch.object(pub, "MAX_GH_API_RESPONSE_CHARS", 8),
            patch.object(pub.subprocess, "run", return_value=process),
        ):
            with self.assertRaisesRegex(pub.Denied, "response exceeds size limit"):
                pub.gh_api("repos/trusted/ci")

    def test_private_ci_policy_rejects_noncanonical_control_bytes(self):
        for raw in (
            b"trusted/ci 246810\r\n",
            b"trusted/ci 246810\x00",
            b"trusted/ci 246810\t\n",
            b"trusted/ci 246810\xe2\x80\xa8",
        ):
            with self.subTest(raw=raw), self.assertRaises(pub.Denied):
                pub._parse_policy_approval(
                    raw, "trusted/ci", "protected source policy"
                )

    def test_descriptor_safety_flags_require_distinct_positive_integers(self):
        with (
            patch.object(pub.os, "O_DIRECTORY", 1, create=True),
            patch.object(pub.os, "O_NOFOLLOW", 2, create=True),
            patch.object(pub.os, "O_NONBLOCK", 4, create=True),
        ):
            self.assertTrue(pub.safe_descriptor_flags_available())

        invalid = (
            (0, 2, 4),
            (1, 1, 4),
            (1, 2, 3),
            ("1", 2, 4),
            (True, 2, 4),
        )
        for directory, nofollow, nonblock in invalid:
            with self.subTest(flags=(directory, nofollow, nonblock)):
                with (
                    patch.object(pub.os, "O_DIRECTORY", directory, create=True),
                    patch.object(pub.os, "O_NOFOLLOW", nofollow, create=True),
                    patch.object(pub.os, "O_NONBLOCK", nonblock, create=True),
                ):
                    self.assertFalse(pub.safe_descriptor_flags_available())

    def fake_api(self, route, method="GET", payload=None):
        self.route_calls.append((method, route, payload))
        if route == "repos/trusted/ci":
            return dict(id=246810, full_name="trusted/ci", visibility=self.visibility.pop(0),
                        private=True, archived=False, disabled=False, fork=False,
                        default_branch="main")
        if route == "repos/trusted/ci/git/blobs":
            raw = base64.b64decode(payload["content"])
            return {"sha": hashlib.sha1(b"blob " + str(len(raw)).encode("ascii")
                                        + bytes([0]) + raw).hexdigest()}
        if route == "repos/trusted/ci/git/trees":
            # A fresh file tree and its parent root tree must be distinct.
            return {"sha": E if "base_tree" in payload else D}
        mapping = {
            "repos/trusted/ci/git/ref/heads/main": {"object": {"sha": A}},
            "repos/trusted/ci/git/commits/"+A: {"tree": {"sha": B}},
            "repos/trusted/ci/git/commits": {"sha": A},
            "repos/trusted/ci/git/refs": {"ref": "refs/heads/ci/oreslang-test"},
        }
        if route not in mapping:
            raise AssertionError("unexpected or cross-repository GitHub API route " + route)
        return mapping[route]

    def publish(self, repo="trusted/ci", branch="ci/oreslang-test"):
        with patch.object(pub, "gh_api", side_effect=self.fake_api):
            return pub.publish(repo, self.source, branch, self.root, A)

    def test_reopened_p0_blocks_legacy_write_before_git_object_creation(self):
        self.incident_mock.side_effect = pub.Denied(
            "blocking P0 incident #77 remains unresolved"
        )
        with self.assertRaisesRegex(pub.Denied, "#77 remains unresolved"):
            self.publish()
        self.incident_mock.assert_called_once_with()
        self.assertFalse(
            any(method == "POST" for method, _, _ in self.route_calls)
        )

    def verified_origin_api(self, protected=True, policy=b"trusted/ci 246810\n",
                            codeowners=True, stale=True, admins=True,
                            approvals=1, force=False, deletions=False,
                            last_push=True, conversations=True,
                            bypasses=None, strict_checks=True, contexts=None,
                            named_checks=None, source_id=None, codeowners_file=None):
        routes = []
        if contexts is None:
            contexts = [pub.SOURCE_POLICY_CHECK]
        if named_checks is None:
            named_checks = [{
                "context": pub.SOURCE_POLICY_CHECK,
                "app_id": pub.GITHUB_ACTIONS_APP_ID,
            }]
        if source_id is None:
            source_id = pub.SOURCE_REPOSITORY_ID
        if codeowners_file is None:
            codeowners_file = (
                "# Terminal private-export ownership rules\n"
                + "\n".join(
                    path + " @ORESoftware"
                    for path in pub.SECURITY_CODEOWNER_PATHS
                )
                + "\n"
            ).encode("utf-8")
        def fake(route, method="GET", payload=None):
            routes.append(route)
            if route == "repos/"+pub.SOURCE_REPO:
                return {"id": source_id, "full_name": pub.SOURCE_REPO, "private": True}
            if route == "repos/"+pub.SOURCE_REPO+"/branches/main":
                return {"name": "main", "protected": protected,
                        "commit": {"sha": A}}
            if route == "repos/"+pub.SOURCE_REPO+"/branches/main/protection":
                reviews = {"require_code_owner_reviews": codeowners,
                           "dismiss_stale_reviews": stale,
                           "require_last_push_approval": last_push,
                           "required_approving_review_count": approvals}
                if bypasses is not None:
                    reviews["bypass_pull_request_allowances"] = bypasses
                return {"required_pull_request_reviews": reviews,
                        "required_status_checks": {
                            "strict": strict_checks,
                            "contexts": list(contexts),
                            "checks": list(named_checks),
                        },
                        "enforce_admins": {"enabled": admins},
                        "allow_force_pushes": {"enabled": force},
                        "allow_deletions": {"enabled": deletions},
                        "required_conversation_resolution": {"enabled": conversations}}
            if route == ("repos/"+pub.SOURCE_REPO+"/contents/"
                         +pub.POLICY_PATH+"?ref="+A):
                return {"path": pub.POLICY_PATH, "type": "file",
                        "encoding": "base64",
                        "content": base64.b64encode(policy).decode("ascii")}
            if route == ("repos/"+pub.SOURCE_REPO+"/contents/"
                         +pub.CODEOWNERS_PATH+"?ref="+A):
                return {"path": pub.CODEOWNERS_PATH, "type": "file",
                        "encoding": "base64",
                        "content": base64.b64encode(codeowners_file).decode("ascii")}
            return self.fake_api(route, method, payload)
        return fake, routes

    def test_origin_verification_returns_the_authenticated_main_commit(self):
        api, _ = self.verified_origin_api(protected=True)
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        with patch.object(pub, "gh_api", side_effect=api):
            self.assertEqual(
                ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest),
                A,
            )

    def test_origin_rejects_main_move_during_attestation(self):
        api, _ = self.verified_origin_api(protected=True)
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        calls = [0]
        def moved(route, method="GET", payload=None):
            if route == "repos/"+pub.SOURCE_REPO+"/branches/main":
                calls[0] += 1
                if calls[0] == 2:
                    return {"name": "main", "protected": True,
                            "commit": {"sha": B}}
            return api(route, method, payload)
        with patch.object(pub, "gh_api", side_effect=moved):
            with self.assertRaisesRegex(pub.Denied, "changed during"):
                ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)
        self.assertEqual(calls[0], 2)

    def test_origin_rejects_protection_change_during_attestation(self):
        api, _ = self.verified_origin_api(protected=True, approvals=2)
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        calls = [0]
        def changed(route, method="GET", payload=None):
            if route.endswith("/branches/main/protection"):
                calls[0] += 1
                result = api(route, method, payload)
                if calls[0] == 2:
                    result = dict(result)
                    reviews = dict(result["required_pull_request_reviews"])
                    reviews["required_approving_review_count"] = 1
                    result["required_pull_request_reviews"] = reviews
                return result
            return api(route, method, payload)
        with patch.object(pub, "gh_api", side_effect=changed):
            with self.assertRaisesRegex(pub.Denied, "protection changed during"):
                ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)
        self.assertEqual(calls[0], 2)

    def test_origin_rejects_source_repository_id_replacement(self):
        api, _ = self.verified_origin_api(
            source_id=pub.SOURCE_REPOSITORY_ID + 1
        )
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        with patch.object(pub, "gh_api", side_effect=api):
            with self.assertRaisesRegex(pub.Denied, "identity and privacy"):
                ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)

    def test_origin_requires_last_push_approval_and_conversation_resolution(self):
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        for options, message in (
            ({"last_push": False}, "last-push approval"),
            ({"conversations": False}, "conversation resolution"),
        ):
            with self.subTest(options=options):
                api, _ = self.verified_origin_api(**options)
                with patch.object(pub, "gh_api", side_effect=api):
                    with self.assertRaisesRegex(pub.Denied, message):
                        ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)

    def test_origin_requires_strict_issuer_bound_security_check(self):
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        cases = (
            ({"strict_checks": False}, "strict required CI"),
            ({"contexts": [], "named_checks": []}, "issuer binding"),
            ({"contexts": [""]}, "malformed"),
            (
                {"contexts": [], "named_checks": [{"context": ""}]},
                "identity is ambiguous",
            ),
            (
                {
                    "contexts": [],
                    "named_checks": [{
                        "context": pub.SOURCE_POLICY_CHECK,
                        "app_id": 999999,
                    }],
                },
                "untrusted issuer",
            ),
            (
                {
                    "contexts": [],
                    "named_checks": [{
                        "context": "unrelated-actions-job",
                        "app_id": pub.GITHUB_ACTIONS_APP_ID,
                    }],
                },
                "security CI check missing",
            ),
        )
        for options, message in cases:
            with self.subTest(options=options):
                api, _ = self.verified_origin_api(**options)
                with patch.object(pub, "gh_api", side_effect=api):
                    with self.assertRaisesRegex(pub.Denied, message):
                        ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)

    def test_origin_rejects_author_codeowner_instead_of_independent_security_owner(self):
        bad = (
            "\n".join(path + " @the1mills"
                      for path in pub.SECURITY_CODEOWNER_PATHS)
            + "\n"
        ).encode("utf-8")
        api, _ = self.verified_origin_api(codeowners_file=bad)
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        with patch.object(pub, "gh_api", side_effect=api):
            with self.assertRaisesRegex(pub.Denied, "independent security owner"):
                ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)

    def test_origin_rejects_mixed_independent_and_author_codeowners(self):
        mixed = (
            "\n".join(path + " @ORESoftware @the1mills"
                      for path in pub.SECURITY_CODEOWNER_PATHS)
            + "\n"
        ).encode("utf-8")
        api, _ = self.verified_origin_api(codeowners_file=mixed)
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        with patch.object(pub, "gh_api", side_effect=api):
            with self.assertRaisesRegex(pub.Denied, "solely owned"):
                ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)

    def test_origin_rejects_duplicate_required_check_identity(self):
        api, _ = self.verified_origin_api(
            contexts=[],
            named_checks=[
                {
                    "context": pub.SOURCE_POLICY_CHECK,
                    "app_id": pub.GITHUB_ACTIONS_APP_ID,
                },
                {
                    "context": pub.SOURCE_POLICY_CHECK,
                    "app_id": pub.GITHUB_ACTIONS_APP_ID,
                },
            ],
        )
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        with patch.object(pub, "gh_api", side_effect=api):
            with self.assertRaisesRegex(pub.Denied, "identity is ambiguous"):
                ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)

    def test_origin_rejects_nonterminal_security_codeowners(self):
        valid = (
            "\n".join(path + " @ORESoftware"
                      for path in pub.SECURITY_CODEOWNER_PATHS)
            + "\n/scripts/ @the1mills\n"
        ).encode("utf-8")
        api, _ = self.verified_origin_api(codeowners_file=valid)
        digest = hashlib.sha256(b"trusted/ci 246810\n").digest()
        with patch.object(pub, "gh_api", side_effect=api):
            with self.assertRaisesRegex(pub.Denied, "terminal"):
                ORIGINAL_VERIFY_PROTECTED_ORIGIN("trusted/ci", digest)

    def test_policy_approval_returns_reviewed_numeric_repository_id(self):
        digest, repository_id = pub.checked_policy_approval(
            self.root, "trusted/ci"
        )
        self.assertEqual(
            digest, hashlib.sha256(b"trusted/ci 246810\n").digest()
        )
        self.assertEqual(repository_id, 246810)

    def test_repository_id_substitution_rejected_before_private_write(self):
        original = self.fake_api
        def replaced(route, method="GET", payload=None):
            result = original(route, method, payload)
            if route == "repos/trusted/ci":
                result = dict(result, id=246811)
            return result
        with patch.object(pub, "gh_api", side_effect=replaced):
            with self.assertRaisesRegex(pub.Denied, "numeric repository ID"):
                pub.publish("trusted/ci", self.source,
                            "ci/oreslang-test", self.root, A)
        self.assertFalse(any(method == "POST" for method, _, _ in self.route_calls))

    def test_origin_main_must_be_protected_before_export(self):
        api, routes = self.verified_origin_api(protected=False)
        with patch.object(pub, "verify_protected_origin",
                          side_effect=ORIGINAL_VERIFY_PROTECTED_ORIGIN):
            with patch.object(pub, "gh_api", side_effect=api):
                with patch.object(pub, "collect_source") as collect:
                    with self.assertRaisesRegex(pub.Denied, "not protected"):
                        pub.publish("trusted/ci", self.source,
                                    "ci/oreslang-test", self.root, A)
                    collect.assert_not_called()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))
        self.assertNotIn("repos/"+pub.SOURCE_REPO+"/contents/"
                         +pub.POLICY_PATH+"?ref="+A, routes)

    def test_origin_main_requires_real_codeowner_and_admin_enforcement(self):
        for rule in ("codeowners", "stale", "admins"):
            with self.subTest(rule=rule):
                options = {rule: False}
                api, routes = self.verified_origin_api(**options)
                with patch.object(pub, "verify_protected_origin",
                                  side_effect=ORIGINAL_VERIFY_PROTECTED_ORIGIN):
                    with patch.object(pub, "gh_api", side_effect=api):
                        with patch.object(pub, "collect_source") as collect:
                            with self.assertRaisesRegex(pub.Denied, "CODEOWNER"):
                                pub.publish("trusted/ci", self.source,
                                            "ci/oreslang-test", self.root, A)
                            collect.assert_not_called()
                self.assertNotIn("repos/"+pub.SOURCE_REPO+"/contents/"
                                 +pub.POLICY_PATH+"?ref="+A, routes)
                self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_origin_main_rejects_force_push_delete_and_review_bypasses(self):
        for config in ({"approvals": 0}, {"force": True},
                       {"deletions": True},
                       {"bypasses": {"users": ["someone"], "teams": [], "apps": []}},
                       {"bypasses": {"users": "invalid"}}):
            with self.subTest(config=config):
                api, routes = self.verified_origin_api(**config)
                with patch.object(pub, "verify_protected_origin",
                                  side_effect=ORIGINAL_VERIFY_PROTECTED_ORIGIN):
                    with patch.object(pub, "gh_api", side_effect=api):
                        with patch.object(pub, "collect_source") as collect:
                            with self.assertRaises(pub.Denied):
                                pub.publish("trusted/ci", self.source,
                                            "ci/oreslang-test", self.root, A)
                            collect.assert_not_called()
                self.assertNotIn(
                    "repos/"+pub.SOURCE_REPO+"/contents/"
                    +pub.POLICY_PATH+"?ref="+A, routes)
                self.assertFalse(any(method == "POST"
                                     for method, _, _ in self.route_calls))

    def test_origin_protection_api_permission_denial_rejects_export(self):
        api, _ = self.verified_origin_api()
        def denied(route, method="GET", payload=None):
            if route.endswith("/branches/main/protection"):
                raise pub.Denied("GitHub protection settings API unavailable")
            return api(route, method, payload)
        with patch.object(pub, "verify_protected_origin",
                          side_effect=ORIGINAL_VERIFY_PROTECTED_ORIGIN):
            with patch.object(pub, "gh_api", side_effect=denied):
                with patch.object(pub, "collect_source") as collect:
                    with self.assertRaisesRegex(pub.Denied, "unavailable"):
                        pub.publish("trusted/ci", self.source,
                                    "ci/oreslang-test", self.root, A)
                    collect.assert_not_called()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_origin_main_must_match_local_approved_policy(self):
        api, _ = self.verified_origin_api(
            protected=True, policy=b"# no authorized destination\n")
        with patch.object(pub, "verify_protected_origin",
                          side_effect=ORIGINAL_VERIFY_PROTECTED_ORIGIN):
            with patch.object(pub, "gh_api", side_effect=api):
                with patch.object(pub, "collect_source") as collect:
                    with self.assertRaisesRegex(pub.Denied, "diverges"):
                        pub.publish("trusted/ci", self.source,
                                    "ci/oreslang-test", self.root, A)
                    collect.assert_not_called()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_protected_origin_verification_before_every_write(self):
        api, routes = self.verified_origin_api(protected=True)
        with patch.object(pub, "verify_protected_origin",
                          side_effect=ORIGINAL_VERIFY_PROTECTED_ORIGIN):
            with patch.object(pub, "gh_api", side_effect=api):
                sha, count = pub.publish("trusted/ci", self.source,
                                         "ci/oreslang-test", self.root, A)
        self.assertEqual((sha, count), (A, 1))
        branch_reads = [r for r in routes
                        if r == "repos/"+pub.SOURCE_REPO+"/branches/main"]
        # Every attestation reads main twice (start/end), and publish performs
        # one initial attestation plus one before each of five writes.
        self.assertEqual(len(branch_reads), 12)
        protection_reads = [r for r in routes
                            if r == "repos/"+pub.SOURCE_REPO
                            +"/branches/main/protection"]
        self.assertEqual(len(protection_reads), 12)

    def test_protected_origin_recheck_rejects_last_minute_revocation(self):
        api, routes = self.verified_origin_api(protected=True)
        calls = [0]
        def revoke(route, method="GET", payload=None):
            if route == "repos/"+pub.SOURCE_REPO+"/branches/main":
                calls[0] += 1
                # Reads 1-2 are the initial stable attestation. Revoke at the
                # start of the per-write attestation before the first blob.
                if calls[0] == 3:
                    return {"name": "main", "protected": False,
                            "commit": {"sha": A}}
            return api(route, method, payload)
        with patch.object(pub, "verify_protected_origin",
                          side_effect=ORIGINAL_VERIFY_PROTECTED_ORIGIN):
            with patch.object(pub, "gh_api", side_effect=revoke):
                with self.assertRaisesRegex(pub.Denied, "not protected"):
                    pub.publish("trusted/ci", self.source,
                                "ci/oreslang-test", self.root, A)
        self.assertEqual(calls[0], 3)
        self.assertFalse(any(r == "repos/trusted/ci/git/refs"
                             for m, r, _ in self.route_calls if m == "POST"))

    def test_protected_origin_revocation_before_first_write_blocks_all_posts(self):
        calls = [0]
        def authority(*args, **kwargs):
            calls[0] += 1
            if calls[0] > 1:
                raise pub.Denied("source main is not protected; CODEOWNER approval is unenforced")
            return A
        with patch.object(pub, "verify_protected_origin", side_effect=authority):
            with self.assertRaisesRegex(pub.Denied, "not protected"):
                self.publish()
        self.assertGreaterEqual(calls[0], 2)
        self.assertFalse(any(method == "POST"
                             for method, _, _ in self.route_calls))

    def test_protected_origin_revocation_after_blob_blocks_next_write(self):
        original = self.fake_api
        revoked = [False]
        def api(route, method="GET", payload=None):
            result = original(route, method, payload)
            if method == "POST" and route == "repos/trusted/ci/git/blobs":
                revoked[0] = True
            return result
        def authority(*args, **kwargs):
            if revoked[0]:
                raise pub.Denied("source main is not protected; CODEOWNER approval is unenforced")
            return A
        with patch.object(pub, "verify_protected_origin", side_effect=authority):
            with patch.object(pub, "gh_api", side_effect=api):
                with self.assertRaisesRegex(pub.Denied, "not protected"):
                    pub.publish("trusted/ci", self.source,
                                "ci/oreslang-test", self.root, A)
        writes = [route for method, route, _ in self.route_calls
                  if method == "POST"]
        self.assertEqual(writes, ["repos/trusted/ci/git/blobs"])

    def test_git_tree_proof_exactly_matches_all_local_files(self):
        data = (self.source / "hello.ores").read_bytes()
        git_blob = hashlib.sha1(
            b"blob "+str(len(data)).encode("ascii")+bytes([0])+data
        ).hexdigest()
        tree = [{"path": "hello.ores", "type": "blob",
                 "mode": "100644", "sha": git_blob}]
        seen = []
        def origin_api(route, method="GET", payload=None):
            seen.append(route)
            if route == "repos/"+pub.SOURCE_REPO+"/git/commits/"+A:
                return {"sha": A, "tree": {"sha": D}}
            if route == "repos/"+pub.SOURCE_REPO+"/git/trees/"+D+"?recursive=1":
                return {"sha": D, "truncated": False, "tree": tree}
            raise AssertionError("unexpected origin route: "+route)
        with patch.object(pub, "gh_api", side_effect=origin_api):
            self.assertEqual(
                ORIGINAL_VERIFY_EXACT_SOURCE_TREE(
                    [("hello.ores", data, "100644")], A), D)
            with self.assertRaisesRegex(pub.Denied, "differs"):
                ORIGINAL_VERIFY_EXACT_SOURCE_TREE(
                    [("hello.ores", data+b"dirty", "100644")], A)
            with self.assertRaisesRegex(pub.Denied, "differs"):
                ORIGINAL_VERIFY_EXACT_SOURCE_TREE(
                    [("hello.ores", data, "100755")], A)
            with self.assertRaisesRegex(pub.Denied, "differs"):
                ORIGINAL_VERIFY_EXACT_SOURCE_TREE(
                    [("hello.ores", data, "100644"),
                     ("untracked.secret", b"secret", "100644")], A)
            with self.assertRaisesRegex(pub.Denied, "differs"):
                ORIGINAL_VERIFY_EXACT_SOURCE_TREE([], A)
        self.assertTrue(seen)

    def test_git_tree_attestation_refuses_truncation_and_symlinks(self):
        data = (self.source / "hello.ores").read_bytes()
        def fake(route, method="GET", payload=None):
            if route.endswith("/git/commits/"+A):
                return {"sha": A, "tree": {"sha": D}}
            if route.endswith("/git/trees/"+D+"?recursive=1"):
                return {"sha": D, "truncated": True, "tree": []}
            raise AssertionError(route)
        with patch.object(pub, "gh_api", side_effect=fake):
            with self.assertRaisesRegex(pub.Denied, "incomplete"):
                ORIGINAL_VERIFY_EXACT_SOURCE_TREE(
                    [("hello.ores", data, "100644")], A)
        def symlink(route, method="GET", payload=None):
            if route.endswith("/git/commits/"+A):
                return {"sha": A, "tree": {"sha": D}}
            if route.endswith("/git/trees/"+D+"?recursive=1"):
                return {"sha": D, "truncated": False,
                        "tree": [{"path":"link", "type":"blob",
                                  "mode":"120000", "sha": B}]}
            raise AssertionError(route)
        with patch.object(pub, "gh_api", side_effect=symlink):
            with self.assertRaisesRegex(pub.Denied, "symlink"):
                ORIGINAL_VERIFY_EXACT_SOURCE_TREE(
                    [("hello.ores", data, "100644")], A)

    def test_publisher_rejects_mismatched_uploaded_tree_before_branch(self):
        # GitHub returns a different tree SHA even though blobs matched.
        self.source_tree_mock.return_value = C
        with self.assertRaisesRegex(pub.Denied, "uploaded source tree differs"):
            self.publish()
        self.assertNotIn("repos/trusted/ci/git/refs",
                         [route for method, route, _ in self.route_calls
                          if method == "POST"])

    def test_source_commit_is_mandatory_exact_sha(self):
        with patch.object(pub, "collect_source") as source:
            with self.assertRaises(pub.Denied):
                pub.publish("trusted/ci", self.source, "ci/oreslang-test",
                            self.root, "main")
            source.assert_not_called()

    def test_only_exact_private_repo_write_routes_and_new_ref(self):
        sha, count = self.publish()
        self.assertEqual((sha, count), (A, 1))
        writes = [(m, r, p) for m, r, p in self.route_calls if m == "POST"]
        self.assertEqual([r for _, r, _ in writes], [
            "repos/trusted/ci/git/blobs", "repos/trusted/ci/git/trees",
            "repos/trusted/ci/git/trees", "repos/trusted/ci/git/commits",
            "repos/trusted/ci/git/refs"])
        self.assertEqual(writes[-1][2]["ref"], "refs/heads/ci/oreslang-test")
        self.assertEqual(writes[1][2]["tree"][0]["path"], "hello.ores")
        self.assertNotIn("base_tree", writes[1][2])
        self.assertEqual(writes[2][2]["tree"], [
            {"path": "private-oreslang-source", "mode": "040000",
             "type": "tree", "sha": D}])
        self.assertEqual(writes[2][2]["base_tree"], B)
        self.assertEqual(writes[3][2]["tree"], E)
        self.assertTrue(all(r.startswith("repos/trusted/ci") for _, r, _ in writes))

    def test_existing_stale_files_cannot_survive_clean_source_snapshot(self):
        # Assume base_tree contains a stale private-oreslang-source/secret.ores.
        # A new isolated source tree MUST NOT be based on that default tree.
        (self.source / "current.ores").write_text("new source")
        self.publish()
        tree_writes = [payload for method, route, payload in self.route_calls
                       if method == "POST" and route.endswith("/git/trees")]
        self.assertEqual(len(tree_writes), 2)
        self.assertNotIn("base_tree", tree_writes[0])
        self.assertEqual([item["path"] for item in tree_writes[0]["tree"]],
                         ["current.ores", "hello.ores"])
        self.assertEqual(tree_writes[1]["tree"], [
            {"path": pub.PREFIX, "mode": "040000", "type": "tree", "sha": D}])
        self.assertEqual(tree_writes[1]["base_tree"], B)

    def test_public_visibility_aborts_before_any_write(self):
        self.visibility = ["public"]
        with self.assertRaises(pub.Denied):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_visibility_flip_aborts_before_next_write(self):
        self.visibility = ["private", "private", "public"]
        with self.assertRaises(pub.Denied):
            self.publish()
        self.assertEqual([r for m, r, _ in self.route_calls if m == "POST"],
                         ["repos/trusted/ci/git/blobs"])

    def test_mid_transfer_policy_revocation_aborts_without_new_branch(self):
        original = self.fake_api
        def revoke_after_blob(route, method="GET", payload=None):
            result = original(route, method, payload)
            if route == "repos/trusted/ci/git/blobs":
                self.approval.write_text("# approval revoked")
            return result
        with patch.object(pub, "gh_api", side_effect=revoke_after_blob):
            with self.assertRaisesRegex(pub.Denied, "approval.*(revoked|changed)"):
                pub.publish("trusted/ci", self.source, "ci/oreslang-test", self.root, A)
        writes = [r for m, r, _ in self.route_calls if m == "POST"]
        self.assertEqual(writes, ["repos/trusted/ci/git/blobs"])

    def test_initial_unapproved_policy_fails_even_with_stale_preflight(self):
        self.approval.write_text("# reviewer removed the destination")
        with self.assertRaisesRegex(pub.Denied, "revoked or absent"):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_symlinked_approval_policy_fails_before_upload(self):
        self.approval.unlink()
        self.approval.symlink_to(self.source / "hello.ores")
        with self.assertRaisesRegex(pub.Denied, "approval policy"):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_wrong_repo_and_invalid_branch_rejected_before_preflight(self):
        for repo, branch in [("public/test;evil", "ci/oreslang-test"),
                             ("trusted/ci", "main"),
                             ("trusted/ci", "refs/heads/main"),
                             ("trusted/ci", "ci/other")]:
            with self.subTest(repo=repo, branch=branch):
                with self.assertRaises(pub.Denied):
                    self.publish(repo, branch)
        self.assertEqual(self.route_calls, [])

    def test_symlinked_source_rejected_before_upload(self):
        (self.source / "secret").symlink_to(self.root)
        with self.assertRaises(pub.Denied):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_symlinked_directory_rejected_before_upload(self):
        (self.source / "linked").symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(pub.Denied):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_nested_regular_source_files_keep_expected_paths(self):
        directory = self.source / "nested"
        directory.mkdir()
        (directory / "inside.ores").write_text("safe")
        _, count = self.publish()
        self.assertEqual(count, 2)
        new_tree = [payload for method, route, payload in self.route_calls
                    if method == "POST" and route.endswith("/git/trees")]
        self.assertEqual(len(new_tree), 2)
        paths = [item["path"] for item in new_tree[0]["tree"]]
        self.assertEqual(paths, ["hello.ores", "nested/inside.ores"])
        self.assertNotIn("base_tree", new_tree[0])
        self.assertEqual(new_tree[1]["tree"][0]["path"], "private-oreslang-source")

    def test_directory_entry_quota_rejects_before_upload(self):
        (self.source / "another.ores").write_text("another")
        with patch.object(pub, "MAX_DIRECTORY_ENTRIES", 1):
            with self.assertRaisesRegex(pub.Denied, "entry count"):
                self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_parent_directory_swap_to_symlink_fails_before_upload(self):
        nested = self.source / "nested"
        nested.mkdir()
        (nested / "inside.ores").write_text("safe")
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "private.ores").write_text("do not export")
        original_open = pub.os.open
        swapped = [False]
        def swap_parent(name, flags, *args, **kwargs):
            if name == "nested" and kwargs.get("dir_fd") is not None and not swapped[0]:
                swapped[0] = True
                nested.rename(self.source / "nested-original")
                nested.symlink_to(outside, target_is_directory=True)
            return original_open(name, flags, *args, **kwargs)
        with patch.object(pub.os, "open", side_effect=swap_parent):
            with self.assertRaises(pub.Denied):
                self.publish()
        self.assertTrue(swapped[0])
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_file_swap_to_symlink_fails_before_upload(self):
        sensitive = self.root / "outside-secret"
        sensitive.write_text("secret")
        in_source = self.source / "hello.ores"
        original_open = pub.os.open
        swapped = [False]
        def swap_file(name, flags, *args, **kwargs):
            if name == "hello.ores" and kwargs.get("dir_fd") is not None and not swapped[0]:
                swapped[0] = True
                in_source.unlink()
                in_source.symlink_to(sensitive)
            return original_open(name, flags, *args, **kwargs)
        with patch.object(pub.os, "open", side_effect=swap_file):
            with self.assertRaises(pub.Denied):
                self.publish()
        self.assertTrue(swapped[0])
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_hardlink_to_outside_file_fails_before_upload(self):
        sensitive = self.root / "outside-secret"
        sensitive.write_text("secret")
        (self.source / "hardlink.ores").hardlink_to(sensitive)
        with self.assertRaisesRegex(pub.Denied, "hardlinked"):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_nonregular_source_fifo_fails_before_upload(self):
        import os
        import stat
        fifo = self.source / "suspicious-fifo"
        os.mkfifo(fifo)
        self.assertTrue(stat.S_ISFIFO(fifo.stat().st_mode))
        with self.assertRaises(pub.Denied):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_policy_symlink_swap_between_check_and_read_rejected(self):
        outside = self.root / "outside-approved-policy"
        outside.write_text("trusted/ci 246810\n")
        self.approval.write_text("# no approval\n")
        original_read = Path.read_bytes
        def malicious_read(path):
            if path == self.approval:
                saved = original_read(path)
                path.unlink()
                path.symlink_to(outside)
                data = original_read(path)
                path.unlink()
                path.write_bytes(saved)
                return data
            return original_read(path)
        with patch.object(Path, "read_bytes", malicious_read):
            with self.assertRaises(pub.Denied):
                self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_policy_parent_directory_swap_rejected(self):
        outside = self.root / "external-approved-dir"
        outside.mkdir()
        (outside / "private-ci-approved-repositories.txt").write_text("trusted/ci 246810\n")
        self.approval.write_text("# no approval\n")
        original_open = pub.os.open
        replaced = [False]
        def redirect(name, flags, *args, **kwargs):
            if name == ".github" and kwargs.get("dir_fd") is not None and not replaced[0]:
                replaced[0] = True
                (self.root / ".github").rename(self.root / "old-policy-dir")
                (self.root / ".github").symlink_to(outside, target_is_directory=True)
            return original_open(name, flags, *args, **kwargs)
        with patch.object(pub.os, "open", side_effect=redirect):
            with self.assertRaises(pub.Denied):
                self.publish()
        self.assertTrue(replaced[0])
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_hardlinked_approval_policy_rejected(self):
        outside = self.root / "outside-approved-policy"
        outside.write_bytes(self.approval.read_bytes())
        self.approval.unlink()
        self.approval.hardlink_to(outside)
        with self.assertRaisesRegex(pub.Denied, "single-link"):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_oversized_approval_policy_rejected(self):
        self.approval.write_text("#" + "x" * (16 * 1024 + 1))
        with self.assertRaisesRegex(pub.Denied, "size limit"):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_repeated_failed_reads_do_not_leak_descriptors(self):
        import os
        if not Path("/proc/self/fd").is_dir():
            self.skipTest("requires Linux /proc/self/fd")
        bad = self.source / "nested"
        bad.mkdir()
        (bad / "escape").symlink_to(self.root)
        before = len(os.listdir("/proc/self/fd"))
        for _ in range(200):
            with self.assertRaises(pub.Denied):
                pub.collect_source(self.source, self.root)
        after = len(os.listdir("/proc/self/fd"))
        self.assertLessEqual(after, before + 2)

    def test_concurrent_ancestor_swaps_do_not_expose_external_bytes(self):
        import os
        import threading
        stop = threading.Event()
        root = self.source / "nested"
        moved = self.source / "moved"
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "private.ores").write_text("EXTERNAL SECRET")
        def attacker():
            for _ in range(700):
                if stop.is_set(): break
                try:
                    if root.is_symlink(): root.unlink()
                    if root.is_dir():
                        for item in root.iterdir(): item.unlink()
                        root.rmdir()
                    root.mkdir()
                    (root / "safe.ores").write_text("safe")
                    root.rename(moved)
                    root.symlink_to(outside, target_is_directory=True)
                    root.unlink()
                    for item in moved.iterdir(): item.unlink()
                    moved.rmdir()
                except OSError: pass
        before = len(os.listdir("/proc/self/fd")) if Path("/proc/self/fd").is_dir() else None
        worker = threading.Thread(target=attacker, daemon=True)
        worker.start()
        try:
            for _ in range(450):
                try: files = pub.collect_source(self.source, self.root)
                except pub.Denied: continue
                self.assertFalse(any(b"EXTERNAL SECRET" in data for _, data, _ in files))
        finally:
            stop.set()
            worker.join(3)
        self.assertFalse(worker.is_alive())
        if before is not None:
            self.assertLessEqual(len(os.listdir("/proc/self/fd")), before + 2)

    def test_concurrent_policy_symlink_swap_never_grants_approval(self):
        import os
        import threading
        outside = self.root / "outside-policy"
        outside.write_text("trusted/ci 246810\n")
        self.approval.write_text("# no approval\n")
        stop = threading.Event()
        def attacker():
            for _ in range(700):
                if stop.is_set(): break
                try:
                    if self.approval.is_file() or self.approval.is_symlink():
                        self.approval.unlink()
                    self.approval.symlink_to(outside)
                    self.approval.unlink()
                    self.approval.write_text("# no approval\n")
                except OSError: pass
        before = len(os.listdir("/proc/self/fd")) if Path("/proc/self/fd").is_dir() else None
        worker = threading.Thread(target=attacker, daemon=True)
        worker.start()
        try:
            for _ in range(450):
                with self.assertRaises(pub.Denied):
                    pub.checked_policy_fingerprint(self.root, "trusted/ci")
        finally:
            stop.set()
            worker.join(3)
        self.assertFalse(worker.is_alive())
        if before is not None:
            self.assertLessEqual(len(os.listdir("/proc/self/fd")), before + 2)

    def test_invalid_api_sha_rejects_without_commit(self):
        original = self.fake_api
        def bad(route, method="GET", payload=None):
            if route == "repos/trusted/ci/git/blobs":
                return {"sha": "not a sha"}
            return original(route, method, payload)
        with patch.object(pub, "gh_api", side_effect=bad):
            with self.assertRaises(pub.Denied):
                pub.publish("trusted/ci", self.source, "ci/oreslang-test", self.root, A)
        self.assertFalse(any(r.endswith("/git/refs") for _, r, _ in self.route_calls))

    def test_valid_looking_but_incorrect_blob_sha_is_rejected(self):
        original = self.fake_api
        def wrong(route, method="GET", payload=None):
            if route == "repos/trusted/ci/git/blobs":
                return {"sha": C}
            return original(route, method, payload)
        with patch.object(pub, "gh_api", side_effect=wrong):
            with self.assertRaisesRegex(pub.Denied, "digest"):
                pub.publish("trusted/ci", self.source, "ci/oreslang-test", self.root, A)
        self.assertFalse(any(r.endswith("/git/refs") for _, r, _ in self.route_calls))

    def test_source_must_be_under_trusted_checkout(self):
        outside = self.root.parent / ("unsafe-" + self.root.name)
        outside.mkdir()
        try:
            (outside / "outside.ores").write_text("private data")
            with patch.object(pub, "gh_api", side_effect=self.fake_api):
                with self.assertRaisesRegex(pub.Denied, "trusted checkout"):
                    pub.publish("trusted/ci", outside, "ci/oreslang-test", self.root, A)
        finally:
            (outside / "outside.ores").unlink()
            outside.rmdir()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_denied_preflight_halts_before_api(self):
        with patch.object(pub, "require_preflight", side_effect=pub.Denied("not approved")):
            with self.assertRaisesRegex(pub.Denied, "not approved"):
                self.publish()
        self.assertEqual(self.route_calls, [])

    def test_oversized_source_file_rejected(self):
        (self.source / "large").write_bytes(b"x" * (pub.MAX_SINGLE + 1))
        with self.assertRaises(pub.Denied):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))

    def test_git_metadata_directory_rejected(self):
        (self.source / ".git").mkdir()
        with self.assertRaises(pub.Denied):
            self.publish()
        self.assertFalse(any(m == "POST" for m, _, _ in self.route_calls))


if __name__ == "__main__":
    unittest.main(verbosity=2)

