"""Host-model tests. Real Oreslang runtime instrumentation is separate."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
from oreslang_test.reactive import analyze_reactive_events, ReactiveTraceError


def sub(name="s", source="pipe", mode="hot_single"):
    return {"op": "subscribe", "subscription": name, "source": source, "mode": mode}


def event(op, sub_name="s", **kw):
    return {"op": op, "subscription": sub_name, **kw}


class ReactiveContractTests(unittest.TestCase):
    def test_demand_and_terminal_cleanup(self):
        trace = [sub(), event("request", count=2), event("next"), event("next"),
                 event("complete"), event("dispose")]
        self.assertEqual(analyze_reactive_events(trace),
                         {"events": 6, "subscriptions": 1, "next": 2,
                          "terminals": 1, "active": 0})

    def test_cold_subscriptions_can_be_independent(self):
        trace = [sub("a", "values", "cold"), sub("b", "values", "cold"),
                 event("cancel", "a"), event("dispose", "a"),
                 event("complete", "b"), event("dispose", "b")]
        self.assertEqual(analyze_reactive_events(trace)["subscriptions"], 2)

    def test_hot_channel_cannot_silently_multicast(self):
        with self.assertRaisesRegex(ReactiveTraceError, "cannot multicast"):
            analyze_reactive_events([sub(), sub("second")])

    def test_next_without_demand_is_rejected(self):
        with self.assertRaisesRegex(ReactiveTraceError, "without downstream demand"):
            analyze_reactive_events([sub(), event("next")])

    def test_error_is_distinct_terminal_cause(self):
        good = [sub(), event("error", reason="upstream failed"), event("dispose")]
        self.assertEqual(analyze_reactive_events(good)["terminals"], 1)
        with self.assertRaises(ReactiveTraceError):
            analyze_reactive_events(good + [event("complete")])

    def test_cancellation_rejects_late_values(self):
        with self.assertRaises(ReactiveTraceError):
            analyze_reactive_events([sub(), event("cancel"), event("dispose"),
                                     event("next")])

    def test_duplicate_disposal_and_missing_disposal_rejected(self):
        with self.assertRaisesRegex(ReactiveTraceError, "duplicate disposal"):
            analyze_reactive_events([sub(), event("complete"), event("dispose"),
                                     event("dispose")])
        with self.assertRaisesRegex(ReactiveTraceError, "without one disposal"):
            analyze_reactive_events([sub(), event("complete")])

    def test_bool_and_unbounded_demand_fail_closed(self):
        for bad in (True, 0, -1, 4097):
            with self.subTest(bad=bad), self.assertRaises(ReactiveTraceError):
                analyze_reactive_events([sub(), event("request", count=bad)])

    def test_no_terminal_required_for_live_subscription(self):
        result = analyze_reactive_events([sub(), event("request", count=1)])
        self.assertEqual(result["active"], 1)


    def test_throw_raise_and_panic_never_masquerade_as_normal_completion(self):
        for op in ("throw", "raise", "panic"):
            with self.subTest(op=op), self.assertRaisesRegex(
                    ReactiveTraceError, "unknown operation"):
                analyze_reactive_events([sub(), event(op)])

    def test_error_identity_must_not_be_hidden_in_completion_metadata(self):
        with self.assertRaisesRegex(ReactiveTraceError, "unexpected fields"):
            analyze_reactive_events([
                sub(), event("complete", reason="panic")])
        with self.assertRaises(ReactiveTraceError):
            analyze_reactive_events([
                sub(), event("error", reason="x" * 4097), event("dispose")])


    def test_malformed_operation_and_event_keys_reject_without_type_error(self):
        for bad in ([1, 2], {"unhashable": []}):
            with self.subTest(bad=bad), self.assertRaises(ReactiveTraceError):
                analyze_reactive_events([{"op": bad, "subscription": "s"}])
        with self.assertRaisesRegex(ReactiveTraceError, "non-string field name"):
            analyze_reactive_events([{"op": "subscribe", "subscription": "s",
                                      "source": "pipe", "mode": "hot_single", 5: "bad"}])

    def test_result_err_payload_is_data_not_automatic_stream_error(self):
        trace = [sub(), event("request", count=1),
                 event("next", payload={"Err": "bad"}),
                 event("complete"), event("dispose")]
        outcome = analyze_reactive_events(trace)
        self.assertEqual(outcome["next"], 1)
        self.assertEqual(outcome["terminals"], 1)


if __name__ == "__main__":
    unittest.main()
