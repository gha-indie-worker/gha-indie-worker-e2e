"""Source-free exact-tree mirror receipt contract: no private source."""
import re
import unittest


def admitted(receipt, expected_head):
    if not isinstance(expected_head, str) or re.fullmatch(r"[0-9a-f]{40}", expected_head) is None:
        return False
    if not isinstance(receipt, dict) or receipt.get("head_sha") != expected_head:
        return False
    jobs = receipt.get("jobs")
    if not isinstance(jobs, list) or len(jobs) != 1:
        return False
    job = jobs[0]
    if not isinstance(job, dict) or job.get("status") != "completed" or job.get("conclusion") != "success":
        return False
    steps = job.get("steps")
    if not isinstance(steps, list) or not steps:
        return False
    matches = [step for step in steps if isinstance(step, dict)
               and step.get("name") == "source-free-validation"]
    return len(matches) == 1 and matches[0].get("status") == "completed" and matches[0].get("conclusion") == "success"


class AdmissionContractTest(unittest.TestCase):
    head = "a" * 40

    def receipt(self):
        return {
            "head_sha": self.head,
            "jobs": [{
                "status": "completed",
                "conclusion": "success",
                "steps": [{"name": "source-free-validation", "status": "completed", "conclusion": "success"}],
            }],
        }

    def test_positive_stepful_exact_head(self):
        self.assertTrue(admitted(self.receipt(), self.head))

    def test_deny_empty_steps(self):
        payload = self.receipt()
        payload["jobs"][0]["steps"] = []
        self.assertFalse(admitted(payload, self.head))

    def test_deny_ambiguous_duplicate_step_names(self):
        payload = self.receipt()
        payload["jobs"][0]["steps"] *= 2
        self.assertFalse(admitted(payload, self.head))

    def test_deny_mismatched_head(self):
        self.assertFalse(admitted(self.receipt(), "b" * 40))

    def test_deny_incomplete_job(self):
        payload = self.receipt()
        payload["jobs"][0]["status"] = "queued"
        self.assertFalse(admitted(payload, self.head))

    def test_deny_skipped_named_step(self):
        payload = self.receipt()
        payload["jobs"][0]["steps"][0]["conclusion"] = "skipped"
        self.assertFalse(admitted(payload, self.head))

    def test_deny_missing_job(self):
        payload = self.receipt()
        payload["jobs"] = []
        self.assertFalse(admitted(payload, self.head))

    def test_deny_extra_job(self):
        payload = self.receipt()
        payload["jobs"].append(payload["jobs"][0].copy())
        self.assertFalse(admitted(payload, self.head))


if __name__ == "__main__":
    unittest.main()
