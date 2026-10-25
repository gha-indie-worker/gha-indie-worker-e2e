#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).with_name("verify-oreslang-authorization-ref.py")
spec = importlib.util.spec_from_file_location("authref", SCRIPT)
authref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(authref)

SHA = "a" * 40
MARKER = "ci/authorized-" + SHA
REF = "refs/heads/" + MARKER


def payload(ref=REF, sha=SHA, obj_type="commit"):
    return (
        '{"ref":"' + ref + '","object":{"type":"' + obj_type +
        '","sha":"' + sha + '"}}'
    ).encode()


class AuthorizationRefVerifierTest(unittest.TestCase):
    def test_valid_exact_ref_passes(self):
        self.assertTrue(authref.verify(payload(), MARKER, SHA))

    def test_duplicate_top_level_key_fails(self):
        raw = (
            '{"ref":"' + REF + '","ref":"forged",'
            '"object":{"type":"commit","sha":"' + SHA + '"}}'
        ).encode()
        with self.assertRaisesRegex(authref.Denied, "duplicate authorization key"):
            authref.verify(raw, MARKER, SHA)

    def test_duplicate_nested_key_fails(self):
        raw = (
            '{"ref":"' + REF + '","object":{"type":"commit","type":"tag",'
            '"sha":"' + SHA + '"}}'
        ).encode()
        with self.assertRaisesRegex(authref.Denied, "duplicate authorization key"):
            authref.verify(raw, MARKER, SHA)

    def test_nonfinite_json_fails(self):
        raw = (
            '{"ref":"' + REF + '","object":{"type":NaN,"sha":"' + SHA + '"}}'
        ).encode()
        with self.assertRaisesRegex(authref.Denied, "nonfinite JSON"):
            authref.verify(raw, MARKER, SHA)

    def test_bom_fails(self):
        with self.assertRaisesRegex(authref.Denied, "invalid authorization response"):
            authref.verify(b"\xef\xbb\xbf" + payload(), MARKER, SHA)

    def test_non_object_root_fails(self):
        with self.assertRaisesRegex(authref.Denied, "invalid authorization document"):
            authref.verify(b"[]", MARKER, SHA)

    def test_mismatched_sha_fails(self):
        with self.assertRaisesRegex(authref.Denied, "revoked or changed"):
            authref.verify(payload(sha="b" * 40), MARKER, SHA)

    def test_tag_object_fails(self):
        with self.assertRaisesRegex(authref.Denied, "revoked or changed"):
            authref.verify(payload(obj_type="tag"), MARKER, SHA)

    def test_marker_must_bind_expected_sha(self):
        with self.assertRaisesRegex(authref.Denied, "invalid authorization marker"):
            authref.verify(payload(), "ci/authorized-" + ("b" * 40), SHA)

    def test_uppercase_expected_sha_fails(self):
        with self.assertRaisesRegex(authref.Denied, "invalid expected SHA"):
            authref.verify(payload(), MARKER, SHA.upper())

    def test_oversized_response_fails(self):
        with self.assertRaisesRegex(authref.Denied, "invalid authorization response"):
            authref.verify(b"x" * (authref.MAX_RESPONSE_BYTES + 1), MARKER, SHA)


if __name__ == "__main__":
    unittest.main(verbosity=2)
