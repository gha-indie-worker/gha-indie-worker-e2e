"""Finite reactive-event contract checker (host model, NOT guest instrumentation).

Models demand, terminal cause, disposal, and single-consumer source ownership.
This does not prove scheduler safety or absence of races.
"""
from __future__ import annotations

from collections.abc import Iterable, Mapping


class ReactiveTraceError(ValueError):
    """Malformed or protocol-violating reactive trace."""


def analyze_reactive_events(events: Iterable[Mapping[str, object]]) -> dict[str, int]:
    sources: dict[str, set[str]] = {}
    subscribers: dict[str, dict[str, object]] = {}
    count = 0
    next_count = 0
    terminal_count = 0

    for position, event in enumerate(events):
        count += 1
        if count > 100_000:
            raise ReactiveTraceError("reactive trace exceeds 100000 events")
        if not isinstance(event, Mapping):
            raise ReactiveTraceError(f"event {position}: expected record")
        if any(not isinstance(key, str) for key in event):
            raise ReactiveTraceError(f"event {position}: non-string field name")
        op = event.get("op")
        allowed = {
            "subscribe": {"op", "subscription", "source", "mode"},
            "request": {"op", "subscription", "count"},
            "next": {"op", "subscription", "payload"},
            "complete": {"op", "subscription"},
            "cancel": {"op", "subscription"},
            "error": {"op", "subscription", "reason"},
            "dispose": {"op", "subscription"},
        }
        if not isinstance(op, str) or op not in allowed:
            raise ReactiveTraceError(f"event {position}: unknown operation {op!r}")
        extra = set(event) - allowed[op]
        if extra:
            raise ReactiveTraceError(f"event {position}: unexpected fields {sorted(extra)!r}")
        sub = event.get("subscription")
        if not isinstance(sub, str) or not sub or len(sub) > 256:
            raise ReactiveTraceError(f"event {position}: invalid subscription")
        if op == "subscribe":
            source = event.get("source")
            mode = event.get("mode")
            if not isinstance(source, str) or not source or len(source) > 256:
                raise ReactiveTraceError("subscription requires source identity")
            if mode not in ("hot_single", "cold"):
                raise ReactiveTraceError("subscription must declare hot_single or cold mode")
            if sub in subscribers:
                raise ReactiveTraceError("duplicate subscription identity")
            assigned = sources.setdefault(source, set())
            if mode == "hot_single" and assigned:
                raise ReactiveTraceError("single-consumer channel source cannot multicast")
            if mode == "cold" and any(subscribers[s]["mode"] == "hot_single" for s in assigned):
                raise ReactiveTraceError("mixed source cardinality modes")
            assigned.add(sub)
            subscribers[sub] = {"mode": mode, "source": source, "requested": 0,
                                "terminal": None, "disposed": False}
            continue
        state = subscribers.get(sub)
        if state is None:
            raise ReactiveTraceError(f"event {position}: event before subscribe")
        if op == "dispose":
            if state["disposed"]:
                raise ReactiveTraceError("duplicate disposal")
            if state["terminal"] is None:
                raise ReactiveTraceError("disposal without terminal cause")
            state["disposed"] = True
            continue
        if state["terminal"] is not None:
            raise ReactiveTraceError("event after complete/cancel/error")
        if op == "request":
            amount = event.get("count")
            if isinstance(amount, bool) or not isinstance(amount, int) or not 1 <= amount <= 4096:
                raise ReactiveTraceError("request count must be integer in 1..4096")
            state["requested"] += amount
            if state["requested"] > 4096:
                raise ReactiveTraceError("outstanding demand exceeds 4096")
        elif op == "next":
            if state["requested"] == 0:
                raise ReactiveTraceError("next without downstream demand")
            state["requested"] -= 1
            next_count += 1
        elif op in ("complete", "cancel", "error"):
            if op == "error" and not (isinstance(event.get("reason"), str) and 0 < len(event["reason"]) <= 4096):
                raise ReactiveTraceError("error terminal needs nonempty reason")
            state["terminal"] = op
            terminal_count += 1
        else:
            raise ReactiveTraceError(f"event {position}: unknown operation {op!r}")

    for sub, state in subscribers.items():
        if state["terminal"] is not None and not state["disposed"]:
            raise ReactiveTraceError(f"{sub}: terminal without one disposal")
    return {"events": count, "subscriptions": len(subscribers),
            "next": next_count, "terminals": terminal_count,
            "active": len(subscribers) - terminal_count}
