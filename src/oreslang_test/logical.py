"""Bounded reference-model exploration of actor turns (not runtime instrumentation)."""
import json
from pathlib import Path


class ModelError(ValueError):
    pass


def _int(value):
    if type(value) is not int:
        raise ModelError("state values must be integers")
    return value


def _value(spec, local):
    if type(spec) is int:
        return spec
    if isinstance(spec, dict) and set(spec).issubset({"local", "add"}) and "local" in spec:
        name = spec["local"]
        if name not in local:
            raise ModelError(f"unbound local {name!r}")
        return local[name] + _int(spec.get("add", 0))
    raise ModelError(f"invalid value expression {spec!r}")


def explore(path, max_schedules=10000):
    path = Path(path)
    if not path.is_file() or path.is_symlink() or path.stat().st_size > 2_000_000:
        raise ModelError("model missing, symlinked, or exceeds 2 MB")
    model = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(model, dict):
        raise ModelError("model must be an object")
    initial, actors, expected = model.get("initial"), model.get("actors"), model.get("expected")
    if not isinstance(initial, dict) or not isinstance(expected, dict) or not isinstance(actors, list) or len(actors) < 2:
        raise ModelError("model requires initial, expected, and at least two actors")
    if not expected or not set(expected).issubset(initial):
        raise ModelError("expected must constrain at least one initialized key")
    if not all(isinstance(k, str) and k for k in initial):
        raise ModelError("invalid state key")
    for v in list(initial.values()) + list(expected.values()):
        _int(v)
    if len(actors) > 16 or not all(isinstance(a, dict) for a in actors):
        raise ModelError("models support at most 16 well-formed actors")
    if len(initial) > 256 or len(expected) > 256:
        raise ModelError("models support at most 256 state keys")
    names = [a.get("name") for a in actors]
    if not all(type(n) is str and 0 < len(n) <= 128 for n in names):
        raise ModelError("actor names must be nonempty strings <=128 characters")
    if len(names) != len(set(names)) or not all(isinstance(n, str) and n for n in names):
        raise ModelError("actor names must be nonempty and unique")
    for actor in actors:
        if not isinstance(actor.get("turns"), list) or not actor["turns"]:
            raise ModelError("each actor needs nonempty turns")
        if any(not isinstance(t, list) or not t for t in actor["turns"]):
            raise ModelError("turns must be nonempty arrays of operations")
    if sum(sum(len(turn) for turn in actor["turns"]) for actor in actors) > 4096:
        raise ModelError("at most 4096 operations per model")
    for actor in actors:
        declared_locals = set()
        for turn in actor["turns"]:
            for operation in turn:
                if not isinstance(operation, dict):
                    raise ModelError("operations must be objects")
                kind = operation.get("op")
                key = operation.get("key")
                if type(key) is not str or key not in initial:
                    raise ModelError("every operation must reference an existing state key")
                if kind == "read":
                    if set(operation) != {"op", "key", "as"} or type(operation["as"]) is not str or not operation["as"]:
                        raise ModelError("read must specify a local name")
                    declared_locals.add(operation["as"])
                elif kind == "write":
                    if set(operation) != {"op", "key", "value"}:
                        raise ModelError("write must specify exactly op/key/value")
                    value = operation["value"]
                    if type(value) is int:
                        pass
                    elif type(value) is dict and set(value) in ({"local"}, {"local", "add"}) and type(value.get("local")) is str and value["local"] in declared_locals and type(value.get("add", 0)) is int:
                        pass
                    else:
                        raise ModelError("write requires integer or prior local expression")
                elif kind == "atomic_add":
                    if set(operation) != {"op", "key", "value"} or type(operation["value"]) is not int:
                        raise ModelError("atomic_add requires integer value")
                else:
                    raise ModelError("unknown operation")
    if sum(len(a["turns"]) for a in actors) > 64:
        raise ModelError("at most 64 actor turns may be modeled")
    if type(max_schedules) is not int or not 1 <= max_schedules <= 1_000_000:
        raise ModelError("invalid schedule limit")
    result = {"schedules": 0, "violation": None, "truncated": False}

    def apply(turn, state, local):
        for operation in turn:
            if not isinstance(operation, dict):
                raise ModelError("operation must be an object")
            op, key = operation.get("op"), operation.get("key")
            if key not in state:
                raise ModelError(f"unknown state key {key!r}")
            if op == "read":
                dest = operation.get("as")
                if not isinstance(dest, str) or not dest:
                    raise ModelError("read requires local 'as'")
                local[dest] = state[key]
            elif op == "write":
                state[key] = _value(operation.get("value"), local)
            elif op == "atomic_add":
                state[key] += _int(operation.get("value"))
            else:
                raise ModelError(f"unsupported op {op!r}")

    def dfs(indices, state, locals_by_actor, schedule):
        if all(i == len(a["turns"]) for i, a in zip(indices, actors)):
            result["schedules"] += 1
            if any(state[k] != v for k, v in expected.items()) and result["violation"] is None:
                result["violation"] = {"schedule": list(schedule), "final": dict(state), "expected": expected}
            return
        if result["schedules"] >= max_schedules:
            result["truncated"] = True
            return
        if result["violation"] is not None:
            return
        for j, actor in enumerate(actors):
            if indices[j] >= len(actor["turns"]):
                continue
            new_state = dict(state)
            new_locals = [dict(x) for x in locals_by_actor]
            apply(actor["turns"][indices[j]], new_state, new_locals[j])
            new_indices = list(indices)
            new_indices[j] += 1
            dfs(new_indices, new_state, new_locals, schedule + [f"{actor['name']}:{indices[j]}"])
            if result["violation"] is not None:
                return

    dfs([0] * len(actors), dict(initial), [{} for _ in actors], [])
    if result["truncated"] and result["violation"] is None:
        raise ModelError(f"inconclusive: reached schedule limit {max_schedules}")
    return result
