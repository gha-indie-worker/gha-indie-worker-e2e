#!/usr/bin/env python3
"""Synthetic-only private-source admission model.

No Oreslang source, policy files, Git objects, logs, or artifacts are used.
This verifies only the intended fail-closed boolean/state-machine contract.
"""
from itertools import product

GATE_NAMES = (
    "source_protected",
    "incidents_completed",
    "destination_private",
    "destination_protected",
    "independent_review",
    "identity_pinned",
    "retention_approved",
)


class Denied(RuntimeError):
    pass


def authorize(state):
    missing = [name for name in GATE_NAMES if state.get(name) is not True]
    if missing:
        raise Denied(",".join(missing))
    return True


def authorize_write(state):
    # Authorization is not a lease: every source-bearing operation rechecks.
    authorize(state)
    return True


def exhaustive_truth_table():
    accepted = 0
    denied = 0
    for values in product((False, True), repeat=len(GATE_NAMES)):
        state = dict(zip(GATE_NAMES, values))
        expected = all(values)
        try:
            actual = authorize(state)
        except Denied:
            actual = False
        assert actual is expected, (state, expected, actual)
        accepted += int(actual)
        denied += int(not actual)
    assert accepted == 1
    assert denied == (2 ** len(GATE_NAMES)) - 1
    return accepted, denied


def revocation_cases():
    all_ok = {name: True for name in GATE_NAMES}

    # Unprotected source must deny before any private-source read.
    before_read = dict(all_ok, source_protected=False)
    try:
        authorize(before_read)
    except Denied:
        source_reads = 0
    else:
        raise AssertionError("unprotected source was admitted")
    assert source_reads == 0

    # Reopen an incident after preflight: next source-bearing write must stop.
    authorize(all_ok)
    reopened = dict(all_ok, incidents_completed=False)
    try:
        authorize_write(reopened)
    except Denied:
        writes_after_reopen = 0
    else:
        raise AssertionError("reopened incident did not revoke authority")
    assert writes_after_reopen == 0

    # Loss of destination protection/private visibility/review is immediately fatal.
    for key in ("destination_protected", "destination_private", "independent_review"):
        changed = dict(all_ok, **{key: False})
        try:
            authorize_write(changed)
        except Denied:
            pass
        else:
            raise AssertionError(f"{key} revocation did not stop write")


if __name__ == "__main__":
    accepted, denied = exhaustive_truth_table()
    revocation_cases()
    print(
        "ORESLANG_SOURCE_FREE_GATE_PASS "
        f"accepted={accepted} denied={denied} "
        "private_source_bytes=0"
    )
