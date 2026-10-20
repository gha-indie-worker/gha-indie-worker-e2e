"""Bounded observed wait-cycle and potential lock-order analysis.

Consumes version-one ORES_RACE JSONL, including optional wait/unwait events.
A lock-order cycle is a *risk*, not proof of deadlock; blocked wait cycles
are observed at trace end, not proof that timeouts/cancellation cannot resolve.
No instrumentation-coverage assertion is inferred from an empty trace.
"""
import json
import re
from collections import deque
from pathlib import Path

from .race import TraceError, _identifier, _unique_keys

MAX_EVENTS = 100_000
MAX_LOCKS = 2_048
MAX_EDGES = 10_000
MAX_FINDINGS = 100


def _require(condition, reason):
    if not condition:
        raise TraceError(reason)


def _cycles(graph):
    """Report one shortest cycle per origin, with bounded iterative search.

    Exhausting the traversal budget is inconclusive, never a clean result.
    """
    found = set()
    witnesses = []
    budget = 250_000
    for origin in sorted(graph):
        frontier = deque([(origin, [origin])])
        seen = {origin}
        while frontier:
            budget -= 1
            if budget < 0:
                raise TraceError("inconclusive: lock graph traversal budget exceeded")
            here, path = frontier.popleft()
            for next_node in sorted(graph.get(here, ())):
                if next_node == origin:
                    canonical = min(tuple(path[i:] + path[:i])
                                    for i in range(len(path)))
                    if canonical not in found:
                        found.add(canonical)
                        witnesses.append(list(canonical) + [canonical[0]])
                    frontier.clear()
                    break
                if next_node not in seen:
                    seen.add(next_node)
                    frontier.append((next_node, path + [next_node]))
            if len(witnesses) == MAX_FINDINGS:
                return witnesses
    return witnesses

def _runtime_cycle_trace(path):
    """Java/Graal experimental checkpoint receipts, NOT a full wait graph."""
    if path.stat().st_size > 8_000_000:
        raise TraceError("runtime checkpoint trace exceeds 8 MB")
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeError as exc:
        raise TraceError("malformed runtime checkpoint trace encoding") from exc
    _require(1 <= len(lines) <= MAX_EVENTS + 1,
             "runtime checkpoint event budget exceeded")
    v1 = r"ORES_CONCURRENCY_TRACE_V1\|seed=-?[0-9]+"
    v2 = (r"ORES_CONCURRENCY_TRACE_V2\|seed=-?[0-9]+"
          r"\|entry-sha256=[0-9a-f]{64}\|backend=[A-Za-z0-9_.-]{1,80}\|probe=1")
    v3 = (r"ORES_CONCURRENCY_TRACE_V3\|seed=-?[0-9]+"
          r"\|entry-sha256=[0-9a-f]{64}\|workspace-sha256=[0-9a-f]{64}"
          r"\|backend=[A-Za-z0-9_.-]{1,80}\|probe=3")
    _require(re.fullmatch(v1, lines[0]) is not None
             or re.fullmatch(v2, lines[0]) is not None
             or re.fullmatch(v3, lines[0]) is not None,
             "invalid runtime checkpoint trace header")
    cycles = []
    for event in lines[1:]:
        parts = event.split("|")
        _require(len(parts) == 2 and re.fullmatch(r"[a-z][a-z0-9-]{0,47}", parts[0])
                 and parts[1] in ("0", "1", "2"),
                 "invalid runtime checkpoint event")
        if parts[0].startswith("deadlock-"):
            _require(parts[0] in ("deadlock-future-cycle", "deadlock-mutex-cycle"),
                     "unsupported runtime deadlock event")
            cycles.append(parts[0])
    return {
        "events": len(lines) - 1,
        "lock_events": 0,
        "potential_lock_order_cycles": [],
        "observed_wait_cycles": [],
        "runtime_cycle_events": cycles,
        "pending_waits": 0,
        "backend": "graal-checkpoints",
    }


def analyze_deadlocks(path, expected_run_id=None):
    """Analyze a complete bounded trace; do not claim whole-program liveness."""
    path = Path(path)
    _require(path.is_file() and not path.is_symlink()
             and path.stat().st_size <= 10_000_000,
             "deadlock trace missing, symlinked, or exceeds 10 MB")
    with path.open("rb") as stream:
        first_line = stream.readline(128)
    if first_line.startswith((b"ORES_CONCURRENCY_TRACE_V1|",
                              b"ORES_CONCURRENCY_TRACE_V2|",
                              b"ORES_CONCURRENCY_TRACE_V3|")):
        _require(expected_run_id is None,
                 "runtime checkpoint trace has no nonce binding; cannot authenticate a fresh capture")
        return _runtime_cycle_trace(path)
    try:
        with path.open(encoding="utf-8") as stream:
            records = [json.loads(line, object_pairs_hook=_unique_keys) for line in stream]
    except (UnicodeError, ValueError) as exc:
        raise TraceError(f"invalid deadlock JSONL trace: {exc}") from exc
    _require(2 <= len(records) <= MAX_EVENTS + 2,
             "deadlock trace requires begin/end and <=100000 events")
    _require(all(type(item) is dict for item in records),
             "deadlock trace records must be objects")
    start, end = records[0], records[-1]
    _require(start.get("op") == "begin" and type(start.get("version")) is int
             and start["version"] == 1, "missing ORES_RACE_V1 begin")
    _require(end.get("op") == "end", "missing ORES_RACE_V1 end")
    run_id = _identifier(start.get("run_id"), "run ID")
    _identifier(start.get("backend"), "backend")
    _require(end.get("run_id") == run_id, "deadlock trace run ID mismatch")
    if expected_run_id is not None:
        _require(run_id == expected_run_id, "deadlock trace is stale or foreign")
    _require(type(end.get("events")) is int
             and end["events"] == len(records) - 2,
             "deadlock trace event count mismatch")

    participants = set()
    finished = set()
    owners = {}      # lock identity -> holding task
    held = {}        # task -> held lock identities
    pending = {}     # task -> awaited lock identity
    ordering = {}    # held lock -> later acquired lock
    edges = 0
    observed = 0

    for index, event in enumerate(records[1:-1], start=2):
        op = event.get("op")
        task = _identifier(event.get("task"), f"event {index} task")
        if op == "task":
            _require(task not in participants and len(participants) < 256,
                     "duplicate participant or more than 256 tasks")
            participants.add(task)
            held[task] = set()
            continue
        _require(task in participants and task not in finished,
                 f"event {index}: unknown or finished participant")

        if op == "fork":
            child = _identifier(event.get("child"), "forked child")
            _require(child not in participants and len(participants) < 256,
                     "duplicate child or more than 256 tasks")
            participants.add(child)
            held[child] = set()
        elif op == "finish":
            _require(not held[task] and task not in pending,
                     "task finished while holding or waiting for a lock")
            finished.add(task)
        elif op == "lock":
            name = _identifier(event.get("name"), "lock identity")
            _require(task not in pending, "lock acquired while a wait is active")
            _require(name not in owners, "lock acquired while already held")
            _require(len(owners) < MAX_LOCKS, "too many simultaneously held locks")
            for earlier in held[task]:
                before = ordering.setdefault(earlier, set())
                if name not in before:
                    _require(edges < MAX_EDGES, "too many lock order edges")
                    before.add(name)
                    edges += 1
            held[task].add(name)
            owners[name] = task
            observed += 1
        elif op == "unlock":
            name = _identifier(event.get("name"), "lock identity")
            _require(owners.get(name) == task, "unlock of unowned lock")
            _require(task not in pending,
                     "blocked task cannot unlock while waiting")
            del owners[name]
            held[task].remove(name)
        elif op == "wait":
            name = _identifier(event.get("name"), "awaited lock identity")
            _require(task not in pending, "task already has pending lock wait")
            _require(name in owners, "lock wait lacks an observed owner")
            pending[task] = name
            observed += 1
        elif op == "unwait":
            name = _identifier(event.get("name"), "awaited lock identity")
            _require(pending.get(task) == name, "unwait does not match pending wait")
            del pending[task]
        elif op in ("join", "send", "recv"):
            # These are supported event kinds in the ORES_RACE_V1 trace.
            # They do not modify mutex lock ownership.
            key = "child" if op == "join" else "message"
            _identifier(event.get(key), f"{op} identity")
        elif op == "access":
            _identifier(event.get("location"), "memory location")
            _require(event.get("mode") in ("R", "W"), "invalid access mode")
        else:
            raise TraceError(f"unsupported deadlock event {op!r} at {index}")

    _require(observed > 0,
             "no lock acquisition/wait events; deadlock analysis inconclusive")

    waits = {}
    for task, name in pending.items():
        owner = owners.get(name)
        if owner is not None:
            waits[task] = {owner}
    wait_cycles = _cycles(waits)
    order_cycles = _cycles(ordering)
    return {
        "events": len(records) - 2,
        "lock_events": observed,
        "potential_lock_order_cycles": order_cycles,
        "observed_wait_cycles": wait_cycles,
        "runtime_cycle_events": [],
        "pending_waits": len(pending),
        "backend": start["backend"],
    }
