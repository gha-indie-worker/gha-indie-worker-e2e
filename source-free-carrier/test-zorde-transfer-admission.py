#!/usr/bin/env python3
"""Public synthetic model, not Oreslang implementation or transfer authority."""
from dataclasses import dataclass, replace
import re
import unittest

SHA40 = re.compile(r"^[0-9a-f]{40}$")
HEX_A = "a" * 40
HEX_B = "b" * 40

@dataclass(frozen=True)
class Candidate:
    destination_id: int
    destination_private: bool
    source_commit: str
    source_tree: str
    reviewer: str
    requester: str
    incident_one_cleared: bool
    incident_two_cleared: bool
    retention_days: int
    authorization_expires: int

@dataclass(frozen=True)
class Authorization:
    destination_id: int
    source_commit: str
    source_tree: str
    independent_reviewer: str
    expires: int

def authorize(candidate, ledger, current_epoch):
    if len(ledger) != 1:
        return False
    record, = ledger
    return (
        type(candidate.destination_id) is int and candidate.destination_id > 0
        and candidate.destination_private is True
        and bool(SHA40.fullmatch(candidate.source_commit))
        and bool(SHA40.fullmatch(candidate.source_tree))
        and candidate.source_commit != "0" * 40
        and candidate.source_tree != "0" * 40
        and candidate.incident_one_cleared is True
        and candidate.incident_two_cleared is True
        and type(candidate.retention_days) is int
        and 0 < candidate.retention_days <= 1
        and type(current_epoch) is int
        and type(candidate.authorization_expires) is int
        and candidate.authorization_expires >= current_epoch
        and record.destination_id == candidate.destination_id
        and record.source_commit == candidate.source_commit
        and record.source_tree == candidate.source_tree
        and record.expires == candidate.authorization_expires
        and record.independent_reviewer == candidate.reviewer
        and candidate.reviewer != candidate.requester
        and candidate.reviewer != ""
    )

class SyntheticTransferAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.candidate = Candidate(
            destination_id=123456, destination_private=True,
            source_commit=HEX_A, source_tree=HEX_B,
            requester="proposer", reviewer="independent-reviewer",
            incident_one_cleared=True, incident_two_cleared=True,
            retention_days=1, authorization_expires=20
        )
        self.ledger = [Authorization(
            123456, HEX_A, HEX_B, "independent-reviewer", 20
        )]

    def test_fully_authorized_synthetic_case_only(self):
        self.assertTrue(authorize(self.candidate, self.ledger, 19))

    def test_empty_or_duplicated_ledger_denied(self):
        for ledger in ([], self.ledger + self.ledger):
            with self.subTest(ledger_length=len(ledger)):
                self.assertFalse(authorize(self.candidate, ledger, 19))

    def test_public_or_unknown_visibility_denied(self):
        for value in (False, None, "private", 1):
            with self.subTest(value=value):
                self.assertFalse(authorize(replace(self.candidate, destination_private=value), self.ledger, 19))

    def test_destination_identity_must_match(self):
        for value in (0, -1, True, 123457, "123456"):
            with self.subTest(value=value):
                self.assertFalse(authorize(replace(self.candidate, destination_id=value), self.ledger, 19))

    def test_source_commit_and_tree_must_match_exactly(self):
        for field in ("source_commit", "source_tree"):
            for value in ("0" * 40, "c" * 40, "x" * 40, "A" * 40, "a" * 39):
                with self.subTest(field=field, value=value):
                    self.assertFalse(authorize(replace(self.candidate, **{field:value}), self.ledger, 19))

    def test_both_incident_signoffs_required(self):
        for field in ("incident_one_cleared", "incident_two_cleared"):
            for value in (False, None, 1, "yes"):
                with self.subTest(field=field, value=value):
                    self.assertFalse(authorize(replace(self.candidate, **{field:value}), self.ledger, 19))

    def test_independent_reviewer_required(self):
        for reviewer in ("proposer", "", "someone-else"):
            with self.subTest(reviewer=reviewer):
                self.assertFalse(authorize(replace(self.candidate, reviewer=reviewer), self.ledger, 19))

    def test_short_retention_mandatory(self):
        for value in (0, 2, -1, True, "1"):
            with self.subTest(value=value):
                self.assertFalse(authorize(replace(self.candidate, retention_days=value), self.ledger, 19))

    def test_authorization_expiry_checked(self):
        self.assertFalse(authorize(self.candidate, self.ledger, 21))
        self.assertFalse(authorize(replace(self.candidate, authorization_expires=21), self.ledger, 19))

    def test_public_carrier_is_never_a_source_destination(self):
        public = replace(self.candidate, destination_private=False)
        self.assertFalse(authorize(public, self.ledger, 19))

if __name__ == "__main__":
    unittest.main(verbosity=2)
