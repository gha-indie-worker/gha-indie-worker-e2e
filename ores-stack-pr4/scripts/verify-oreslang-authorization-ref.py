#!/usr/bin/env python3
"""Strictly validate one exact dispatcher authorization ref response."""
import json
from pathlib import Path
import re
import sys

HEX40 = re.compile(r"^[0-9a-f]{40}$")
MAX_RESPONSE_BYTES = 1024 * 1024


class Denied(RuntimeError):
    pass


def _unique_object(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise Denied("duplicate authorization key")
        out[key] = value
    return out


def verify(raw, marker, expected_sha):
    if not isinstance(raw, (bytes, bytearray)):
        raise Denied("authorization response type")
    if not raw or len(raw) > MAX_RESPONSE_BYTES or raw.startswith(b"\xef\xbb\xbf"):
        raise Denied("invalid authorization response")
    if not isinstance(expected_sha, str) or not HEX40.fullmatch(expected_sha):
        raise Denied("invalid expected SHA")
    if marker != "ci/authorized-" + expected_sha:
        raise Denied("invalid authorization marker")
    try:
        doc = json.loads(
            bytes(raw).decode("utf-8"),
            object_pairs_hook=_unique_object,
            parse_constant=lambda _: (_ for _ in ()).throw(Denied("nonfinite JSON")),
        )
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise Denied("invalid authorization JSON") from exc
    if type(doc) is not dict:
        raise Denied("invalid authorization document")
    obj = doc.get("object")
    if (doc.get("ref") != "refs/heads/" + marker
            or type(obj) is not dict
            or obj.get("type") != "commit"
            or obj.get("sha") != expected_sha):
        raise Denied("dispatcher authorization revoked or changed")
    return True


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) != 3:
        print("usage: verify-oreslang-authorization-ref.py RESPONSE MARKER SHA", file=sys.stderr)
        return 2
    response_path, marker, expected_sha = argv
    try:
        raw = Path(response_path).read_bytes()
        verify(raw, marker, expected_sha)
    except (OSError, Denied) as exc:
        print("AUTHORIZATION_REF_DENIED: " + str(exc), file=sys.stderr)
        return 1
    print("AUTHORIZATION_REF_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
