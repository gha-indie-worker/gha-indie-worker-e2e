"""Happens-before analyzer for *observed* Oreslang accesses (not a runtime proof).

The producer must serialize events in causal per-task order. An independent
runtime attestation is required to claim complete instrumentation coverage.
"""
import json
import os
from pathlib import Path


class TraceError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise TraceError(message)


def _identifier(value, label):
    _require(type(value) is str and 0 < len(value) <= 512 and
             all(ord(ch) >= 32 for ch in value), f"invalid {label}")
    return value


def _ordered(earlier, later):
    return all(v <= later.get(k, 0) for k, v in earlier.items())


def _merge(target, source):
    for key, value in source.items():
        target[key] = max(target.get(key, 0), value)


def _unique_keys(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, f"duplicate JSON property {key!r}")
        result[key] = value
    return result


def analyze_trace(path, expected_run_id=None):
    path = Path(path)
    _require(path.is_file() and not path.is_symlink() and
             path.stat().st_size <= 10_000_000,
             "trace missing, symlinked, or exceeds 10 MB")
    # Enforce the byte limit on the actual opened stream, not just on a
    # pre-read stat() that another process could race. Use no-follow where
    # supported to reject final-component symlink substitution.
    try:
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(path, flags), "rb") as stream:
            raw = stream.read(10_000_001)
        _require(len(raw) <= 10_000_000, "trace exceeds 10 MB read budget")
        records = [json.loads(line, object_pairs_hook=_unique_keys)
                   for line in raw.decode("utf-8", errors="strict").splitlines()]
    except (ValueError, UnicodeError) as exc:
        if isinstance(exc, TraceError):
            raise
        raise TraceError(f"invalid JSONL trace: {exc}") from exc
    _require(2 <= len(records) <= 100_002, "trace must have begin/end and <=100000 events")
    _require(all(type(record) is dict for record in records), "trace records must be JSON objects")
    begin, end = records[0], records[-1]
    _require(begin.get("op") == "begin" and type(begin.get("version")) is int and
             begin["version"] == 1, "missing ORES_RACE_V1 begin")
    _require(end.get("op") == "end", "missing trace end")
    run_id = _identifier(begin.get("run_id"), "run ID")
    backend = _identifier(begin.get("backend"), "backend")
    _require(end.get("run_id") == run_id, "end run ID differs")
    if expected_run_id is not None:
        _require(run_id == expected_run_id, "trace is stale or foreign to invocation")
    _require(type(end.get("events")) is int and end["events"] == len(records) - 2,
             "incorrect trace event count")

    clocks = {}
    finished = set()
    parents = {}
    joined = set()
    locks = {}      # name => (owner, depth, last_release_vector)
    messages = {}   # outstanding message-id => send_vector
    issued_message_ids = set()  # globally single-use even after receipt
    history = {}    # location => {"R": task=>vector, "W": task=>vector}
    reported = []
    total_races = 0
    reads_writes = 0

    for index, event in enumerate(records[1:-1], start=2):
        op = event.get("op")
        task = _identifier(event.get("task"), f"event {index} task")
        if op == "task":
            _require(task not in clocks and len(clocks) < 256, "duplicate task or >256 tasks")
            clocks[task] = {task: 1}
            continue
        _require(task in clocks and task not in finished,
                 f"event {index} from unregistered or completed task {task}")
        clock = clocks[task]
        if op == "fork":
            child = _identifier(event.get("child"), "fork child")
            _require(child not in clocks and len(clocks) < 256, "duplicate child or >256 tasks")
            clocks[child] = dict(clock)
            clocks[child][child] = 1
            parents[child] = task
        elif op == "finish":
            _require(all(owner != task for owner, _, _ in locks.values()),
                     "cannot finish while holding a lock")
            finished.add(task)
        elif op == "join":
            child = _identifier(event.get("child"), "join child")
            _require(parents.get(child) == task and child in finished and child not in joined,
                     "join requires a finished, unjoined, directly forked child")
            _merge(clock, clocks[child])
            joined.add(child)
        elif op == "lock":
            name = _identifier(event.get("name"), "lock")
            owner, depth, released = locks.get(name, (None, 0, {}))
            _require(owner is None or owner == task,
                     f"lock {name} acquired while owned by a different participant")
            _require(depth < 2 ** 32 - 1, "reentrant lock depth exhausted")
            if owner is None:
                _merge(clock, released)
                locks[name] = (task, 1, released)
            else:
                locks[name] = (task, depth + 1, released)
        elif op == "unlock":
            name = _identifier(event.get("name"), "lock")
            owner, depth, released = locks.get(name, (None, 0, {}))
            _require(owner == task and depth > 0, f"unlock of unowned lock {name}")
            if depth > 1:
                locks[name] = (task, depth - 1, released)
            else:
                locks[name] = (None, 0, dict(clock))
        elif op == "send":
            message = _identifier(event.get("message"), "message")
            _require(message not in issued_message_ids,
                     "message identity cannot be reused even after receipt")
            issued_message_ids.add(message)
            messages[message] = dict(clock)
        elif op == "recv":
            message = _identifier(event.get("message"), "message")
            _require(message in messages, "receive without send")
            _merge(clock, messages.pop(message))
        elif op == "access":
            location = _identifier(event.get("location"), "location")
            mode = event.get("mode")
            _require(mode in ("R", "W"), "access mode must be R or W")
            reads_writes += 1
            past = history.setdefault(location, {"R": {}, "W": {}})
            candidates = [("W", t, vc) for t, vc in past["W"].items()]
            if mode == "W":
                candidates.extend(("R", t, vc) for t, vc in past["R"].items())
            for prior_mode, previous_task, prior_clock in candidates:
                if previous_task != task and not _ordered(prior_clock, clock):
                    total_races += 1
                    if len(reported) < 100:
                        reported.append({"location": location, "first": previous_task,
                                         "second": task, "first_mode": prior_mode,
                                         "second_mode": mode, "event": index})
            # Do not erase other tasks' accesses. An earlier write may race
            # with a third task even when an intervening write is ordered.
            past[mode][task] = dict(clock)
        else:
            raise TraceError(f"unsupported op {op!r} at event {index}")
        clock[task] = clock.get(task, 0) + 1

    _require(reads_writes > 0, "trace contains no memory accesses; refusing false PASS")
    _require(all(owner is None and depth == 0
                 for owner, depth, _ in locks.values()), "trace ends with held locks")
    _require(all(child in finished for child in parents), "trace ends with unfinished forked task")
    return {"events": len(records) - 2, "accesses": reads_writes,
            "race_count": total_races, "races": reported, "backend": backend}
