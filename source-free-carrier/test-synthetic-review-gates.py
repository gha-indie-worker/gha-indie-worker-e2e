#!/usr/bin/env python3
"""Entirely synthetic PUBLIC test corpus. No Oreslang private source or contracts."""
from dataclasses import dataclass, replace
import unittest

@dataclass(frozen=True)
class State:
    protected_main: bool = True
    review_count: int = 2
    admins_enforced: bool = True
    stale_reviews_dismissed: bool = True
    codeowners_required: bool = True
    incident_one_closed: bool = True
    incident_two_closed: bool = True
    destination_private: bool = True
    destination_id: int = 123456
    source_commit: str = "a" * 40
    source_tree: str = "b" * 40
    reviewer_a: str = "reviewer-a"
    reviewer_b: str = "reviewer-b"
    requester: str = "requester"
    approval_expires: int = 2000
    evaluated_at: int = 1500
    retention_days: int = 1
    ledger_count: int = 1

def synthetic_admission(s):
    return (
        s.protected_main is True
        and type(s.review_count) is int and s.review_count >= 2
        and s.admins_enforced is True
        and s.stale_reviews_dismissed is True
        and s.codeowners_required is True
        and s.incident_one_closed is True and s.incident_two_closed is True
        and s.destination_private is True
        and type(s.destination_id) is int and s.destination_id == 123456
        and s.source_commit == "a" * 40 and s.source_tree == "b" * 40
        and s.reviewer_a != s.reviewer_b
        and s.requester not in (s.reviewer_a, s.reviewer_b)
        and s.approval_expires >= s.evaluated_at
        and s.retention_days == 1 and s.ledger_count == 1
    )

class SyntheticGovernanceTests(unittest.TestCase):
    def test_fully_consistent_synthetic_control_set(self):
        self.assertTrue(synthetic_admission(State()))

    def test_unprotected_and_weak_review_rules_denied(self):
        for field, value in (
            ("protected_main", False), ("review_count", 1),
            ("admins_enforced", False), ("stale_reviews_dismissed", False),
            ("codeowners_required", False)
        ):
            with self.subTest(field=field):
                self.assertFalse(synthetic_admission(replace(State(), **{field:value})))

    def test_each_open_incident_denied(self):
        for field in ("incident_one_closed", "incident_two_closed"):
            self.assertFalse(synthetic_admission(replace(State(), **{field:False})))

    def test_public_carrier_denied_even_with_budget(self):
        self.assertFalse(synthetic_admission(replace(State(), destination_private=False)))

    def test_immutable_destination_id_required(self):
        for value in (123457, 0, True, "123456"):
            self.assertFalse(synthetic_admission(replace(State(), destination_id=value)))

    def test_exact_source_commit_and_tree_required(self):
        for field in ("source_commit", "source_tree"):
            self.assertFalse(synthetic_admission(replace(State(), **{field:"c"*40})))

    def test_reviewers_independent_from_requester(self):
        for changes in (
            {"reviewer_a":"requester"},
            {"reviewer_b":"requester"},
            {"reviewer_b":"reviewer-a"}
        ):
            self.assertFalse(synthetic_admission(replace(State(), **changes)))

    def test_expiration_and_retention_fail_closed(self):
        for changes in (
            {"approval_expires":1499},
            {"retention_days":2},
            {"retention_days":0}
        ):
            self.assertFalse(synthetic_admission(replace(State(), **changes)))

    def test_missing_and_ambiguous_ledger_denied(self):
        for count in (0, 2):
            self.assertFalse(synthetic_admission(replace(State(), ledger_count=count)))

if __name__ == "__main__":
    unittest.main(verbosity=2)
