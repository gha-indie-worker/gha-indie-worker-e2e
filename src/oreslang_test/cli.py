"""Oreslang test CLI. Python >=3.11, standard library only.

Runtime-backed race capture is opt-in and fail-closed pending Java/LLVM hooks.
"""
import argparse
import hashlib
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import sys
import tempfile
import threading
import tomllib
import uuid

from .race import TraceError, analyze_trace
from .logical import ModelError, explore
from .deadlock import analyze_deadlocks


class UsageError(ValueError):
    pass


def _relative(root, name):
    if not isinstance(name, str) or not name:
        raise UsageError("expected nonempty relative path")
    path = Path(name)
    if path.is_absolute() or ".." in path.parts:
        raise UsageError(f"path must be project-relative: {name}")
    target = (root / path).resolve()
    if not target.is_relative_to(root):
        raise UsageError(f"path escapes project root: {name}")
    return target


def _command(value, label):
    if not isinstance(value, list) or not value or not all(isinstance(x, str) and x for x in value):
        raise UsageError(f"{label}.command must be a nonempty TOML array of argv strings")
    return value


def _run(command, root, timeout, data=None, env=None):
    # Drain each output pipe into bounded memory while the guest is running.
    # Post-exit file-size checks permit unlimited disk growth before admission.
    limit = 4_000_000

    def terminate(process):
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            elif process.poll() is None:
                process.kill()
        except ProcessLookupError:
            pass
        except PermissionError:
            # Darwin may return EPERM once wait() has reaped the sole group
            # member. A live process still requires successful termination.
            if process.poll() is None:
                raise

    exceeded = threading.Event()
    reader_errors = []

    def drain(stream, output, process):
        try:
            while chunk := stream.read(65536):
                if len(output) + len(chunk) > limit:
                    exceeded.set()
                    terminate(process)
                    break
                output.extend(chunk)
        except (OSError, ValueError) as error:
            reader_errors.append(error)
            terminate(process)
        finally:
            stream.close()

    # A guest that never reads stdin cannot block an input-writer thread.
    with tempfile.TemporaryFile() as stdin:
        if data is not None:
            stdin.write(data)
            stdin.seek(0)
        try:
            process = subprocess.Popen(command, cwd=root, env=env, stdin=stdin,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       start_new_session=(os.name == "posix"))
        except OSError as exc:
            raise UsageError(f"cannot start executable {command[0]}: {exc}") from exc
        stdout, stderr = bytearray(), bytearray()
        readers = [threading.Thread(target=drain, args=(stream, output, process), daemon=True)
                   for stream, output in ((process.stdout, stdout), (process.stderr, stderr))]
        for reader in readers:
            reader.start()
        timed_out = False
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            terminate(process)
            process.wait()
        finally:
            for reader in readers:
                reader.join(timeout=1)
            leaked_output = any(reader.is_alive() for reader in readers)
            if leaked_output:
                terminate(process)
                for reader in readers:
                    reader.join(timeout=1)
        if any(reader.is_alive() for reader in readers):
            raise UsageError("test process output never closed")
        if reader_errors:
            raise UsageError(f"test process output reader failed: {reader_errors[0]}")
        if exceeded.is_set():
            raise UsageError("test process emitted more than 4 MB stdout or stderr")
        if timed_out:
            return None
        if leaked_output:
            raise UsageError("test process descendants outlived the runner")
        return subprocess.CompletedProcess(command, process.returncode, bytes(stdout), bytes(stderr))


def _decode(output):
    return output.decode("utf-8", errors="replace")[:8000]


def run_native(root, config, timeout):
    native = config.get("native", {})
    files = native.get("files")
    if files is None:
        files = [str(p.relative_to(root)) for p in sorted((root / "tests").rglob("*.ores"))
                 if p.name.startswith("test_") or p.name.endswith("_test.ores")]
    if not isinstance(files, list) or not all(isinstance(x, str) for x in files) or not files:
        raise UsageError("no native test files; configure [native].files or tests/test_*.ores")
    executable = os.environ.get("ORESLANG_BIN", "oreslang-core")
    if Path(executable).resolve() == Path(sys.argv[0]).resolve():
        raise UsageError("ORESLANG_BIN points to the test wrapper (would recurse)")
    failed = 0
    for name in files:
        path = _relative(root, name)
        if not path.is_file() or path.suffix != ".ores":
            raise UsageError(f"not an Oreslang test file: {name}")
        result = _run([executable, str(path)], root, timeout)
        if result is None:
            print(f"TIMEOUT {name}")
            failed += 1
            continue
        output = result.stdout.decode("utf-8", errors="replace")
        summaries = [line for line in output.splitlines() if line.startswith("ORES_TEST|SUMMARY|")]
        status_lines = [line for line in output.splitlines()
                        if line.startswith(("ORES_TEST|PASS|", "ORES_TEST|FAIL|", "ORES_TEST|SKIP|"))]
        valid = False
        if len(summaries) == 1:
            try:
                pairs = [item.split("=", 1) for item in summaries[0].split("|")[2:]]
                counts = {k: int(v) for k, v in pairs}
                valid = (set(counts) == {"total", "passed", "failed", "skipped", "filtered"}
                         and len(pairs) == 5
                         and all(x >= 0 for x in counts.values())
                         and counts["passed"] > 0
                         and counts["total"] == counts["passed"] + counts["failed"] + counts["skipped"]
                         and len(status_lines) == counts["total"])
                for status in ("PASS", "FAIL", "SKIP"):
                    valid &= sum(line.startswith(f"ORES_TEST|{status}|") for line in status_lines) == counts[{"PASS": "passed", "FAIL": "failed", "SKIP": "skipped"}[status]]
                valid &= counts["failed"] == 0
            except (ValueError, KeyError):
                valid = False
        if result.returncode != 0 or not valid:
            failed += 1
            print(f"FAIL {name}: exit={result.returncode}, valid_summary={valid}")
            print(output[:8000] or _decode(result.stderr))
        else:
            print(f"PASS {name}: {summaries[0]}")
    return 1 if failed else 0


def _failure(result):
    if result is None:
        return "timeout"
    return f"exit:{result.returncode}" if result.returncode else None


def fuzz(root, config, seed, cases, timeout):
    params = config.get("fuzz", {})
    cmd = _command(params.get("command"), "fuzz")
    if not 1 <= cases <= 1_000_000:
        raise UsageError("--cases must be between 1 and 1000000")
    max_bytes = params.get("max_bytes", 256)
    if type(max_bytes) is not int or not 1 <= max_bytes <= 65536:
        raise UsageError("fuzz.max_bytes must be between 1 and 65536")
    rng = random.Random(seed)
    corpus_path = _relative(root, params.get("corpus", "tests/corpus"))
    corpus = [b"", b"\x00", b"\xff"]
    if corpus_path.exists():
        if not corpus_path.is_dir():
            raise UsageError("fuzz corpus is not a directory")
        for p in sorted(corpus_path.iterdir()):
            if p.is_symlink():
                raise UsageError("fuzz corpus cannot contain symlinks")
            if p.is_file() and p.stat().st_size <= max_bytes:
                corpus.append(p.read_bytes())
    else:
        print("No corpus directory; using built-in seed cases")
    unique = list(dict.fromkeys(corpus))
    for i in range(cases):
        if i < len(unique):
            case = unique[i]
        else:
            base = bytearray(rng.choice(unique))
            if not base or rng.randrange(4) == 0:
                case = rng.randbytes(rng.randrange(max_bytes + 1))
            else:
                for _ in range(1 + rng.randrange(4)):
                    idx = rng.randrange(len(base))
                    base[idx] ^= rng.randrange(256)
                case = bytes(base[:max_bytes])
        result = _run(cmd, root, timeout, data=case)
        failure = _failure(result)
        if failure:
            # A reproducible counterexample must fail more than once. Do not
            # minimize a flaky invocation or silently treat it as success.
            if any(_failure(_run(cmd, root, timeout, data=case)) != failure for _ in range(2)):
                print(f"FUZZ FLAKY seed={seed} iteration={i} reason={failure}; cannot minimize")
                return 2
            print(f"FUZZ FAILURE seed={seed} iteration={i} reason={failure} size={len(case)}")
            # Deterministic delta debugging. Never accept a shrink with a different failure class.
            minimized, budget, granularity = case, 200, 2
            while len(minimized) > 0 and budget > 0:
                size = max(1, (len(minimized) + granularity - 1) // granularity)
                candidates = [minimized[:start] + minimized[start + size:]
                              for start in range(0, len(minimized), size)]
                shorter = False
                for candidate in candidates:
                    budget -= 1
                    if _failure(_run(cmd, root, timeout, data=candidate)) == failure:
                        minimized, granularity, shorter = candidate, 2, True
                        break
                    if budget == 0:
                        break
                if not shorter:
                    if granularity >= len(minimized):
                        break
                    granularity = min(len(minimized), granularity * 2)
            if any(_failure(_run(cmd, root, timeout, data=minimized)) != failure for _ in range(2)):
                print("FUZZ FLAKY minimized case changed behavior; retaining original counterexample")
                minimized = case
            dest_dir = _relative(root, ".oreslang-test/failures")
            dest_dir.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256(minimized).hexdigest()[:20]
            saved = dest_dir / f"fuzz-{digest}.bin"
            if saved.is_symlink() or (saved.exists() and not saved.is_file()):
                raise UsageError("unsafe failure artifact destination")
            if saved.exists() and saved.read_bytes() != minimized:
                raise UsageError("failure artifact hash collision")
            saved.write_bytes(minimized)
            print(f"Minimized to {len(minimized)} bytes: {saved.relative_to(root)}")
            if result is not None:
                print(_decode(result.stderr))
            return 1
        if len(unique) < 4096:
            unique.append(case)
    print(f"FUZZ PASS cases={cases} seed={seed} (tested inputs only)")
    return 0


def race(root, config, trace, logical, model, timeout, max_schedules):
    params = config.get("race", {})
    status = 0
    analyses = 0
    if trace:
        report = analyze_trace(_relative(root, trace))
        analyses += 1
        status |= _show_races(report)
    elif params.get("command"):
        cmd = _command(params["command"], "race")
        run_id = uuid.uuid4().hex
        with tempfile.TemporaryDirectory(prefix="oreslang-race-") as d:
            path = Path(d) / "events.jsonl"
            env = dict(os.environ, ORESLANG_RACE_TRACE=str(path), ORESLANG_RACE_RUN_ID=run_id)
            result = _run(cmd, root, timeout, env=env)
            if result is None or result.returncode != 0:
                raise UsageError("instrumented race test failed or timed out; cannot certify trace")
            report = analyze_trace(path, expected_run_id=run_id)
            analyses += 1
            status |= _show_races(report)
    if logical or model:
        paths = [model] if model else config.get("logical", {}).get("models", [])
        if not isinstance(paths, list) or not paths:
            raise UsageError("--logical needs --model or [logical].models")
        for name in paths:
            report = explore(_relative(root, name), max_schedules=max_schedules)
            analyses += 1
            if report["violation"]:
                print(f"LOGICAL RACE {name}: {report['violation']}")
                status = 1
            else:
                print(f"LOGICAL PASS {name}: explored {report['schedules']} schedules (reference model only)")
    if analyses > 0 and (logical or model) and not (trace or params.get("command")):
        print("No instrumented memory-access trace analyzed; result is logical-model-only")
    if analyses == 0:
        raise UsageError("--race needs --trace, [race].command instrumentation, or --logical model; refusing a false PASS")
    return status


def deadlock(root, config, trace, timeout):
    """Analyze lock order risk and unresolved wait cycles in actual trace events."""
    if trace:
        report = analyze_deadlocks(_relative(root, trace))
    else:
        params = config.get("deadlock", {})
        command = params.get("command") or config.get("race", {}).get("command")
        if command is None:
            raise UsageError("--deadlock requires --trace or [deadlock].command "
                             "instrumentation; refusing false PASS")
        cmd = _command(command, "deadlock")
        run_id = uuid.uuid4().hex
        with tempfile.TemporaryDirectory(prefix="oreslang-deadlock-") as d:
            path = Path(d) / "events.jsonl"
            env = dict(os.environ, ORESLANG_RACE_TRACE=str(path),
                       ORESLANG_RACE_RUN_ID=run_id)
            result = _run(cmd, root, timeout, env=env)
            if result is None or result.returncode != 0:
                raise UsageError("instrumented deadlock test failed or timed out")
            report = analyze_deadlocks(path, expected_run_id=run_id)
    for kind in report["runtime_cycle_events"][:20]:
        print("RUNTIME CYCLE DETECTED: " + kind)
    if report["backend"] == "graal-checkpoints" and not report["runtime_cycle_events"]:
        raise UsageError("runtime checkpoint trace lacks lock graph evidence; "
                         "no detected cycles is inconclusive")
    for cycle in report["observed_wait_cycles"][:20]:
        print("OBSERVED WAIT CYCLE: " + " -> ".join(cycle))
    for cycle in report["potential_lock_order_cycles"][:20]:
        print("POTENTIAL LOCK-ORDER CYCLE: " + " -> ".join(cycle))
    if report["observed_wait_cycles"] or report["potential_lock_order_cycles"] or report["runtime_cycle_events"]:
        print("DEADLOCK FINDINGS: "
              f"{len(report['observed_wait_cycles'])} observed wait cycles, "
              f"{len(report['potential_lock_order_cycles'])} potential lock-order cycles, "
              f"{len(report['runtime_cycle_events'])} runtime cycle receipts")
        return 1
    print(f"DEADLOCK TRACE PASS: {report['lock_events']} observed lock events "
          "(no cycles in observed lock graph; no whole-program liveness proof)")
    return 0


def _schedule_trace(path):
    """Validate a bounded V3 checkpoint trace; no claim of deterministic carriers."""
    if not path.is_file() or path.is_symlink() or path.stat().st_size > 8_000_000:
        raise UsageError("schedule trace missing, symlinked or too large")
    # Enforce the byte budget on the opened stream, not only on a mutable
    # stat() result. O_NOFOLLOW also closes the last-component symlink race
    # on platforms where the flag is supported.
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        raw = stream.read(8_000_001)
    if len(raw) > 8_000_000:
        raise UsageError("schedule trace exceeds the 8 MB read budget")
    try:
        lines = raw.decode("utf-8", errors="strict").splitlines()
    except UnicodeError as exc:
        raise UsageError("schedule trace is not valid UTF-8") from exc
    if not 2 <= len(lines) <= 100_001:
        raise UsageError("schedule trace has no checkpoints or exceeds event budget")
    match = re.fullmatch(
        r"ORES_CONCURRENCY_TRACE_V3\|seed=(-?[0-9]+)\|entry-sha256=[0-9a-f]{64}"
        r"\|workspace-sha256=[0-9a-f]{64}"
        r"\|backend=[A-Za-z0-9_.-]{1,80}\|probe=3", lines[0])
    if match is None:
        raise UsageError("schedule replay requires workspace-bound V3 trace provenance")
    for index, line in enumerate(lines[1:], 1):
        if re.fullmatch(r"[a-z][a-z0-9-]{0,47}\|[012]", line) is None:
            raise UsageError(f"schedule trace malformed at event {index}")
    return int(match.group(1)), len(lines) - 1


def schedule(root, config, seed, iterations, timeout, replay, trace_output):
    """Delegate schedule perturbation/replay to the explicitly configured runtime."""
    if not 1 <= iterations <= 1_000:
        raise UsageError("--iterations must be between 1 and 1000")
    if replay is not None and iterations != 1:
        raise UsageError("--replay requires --iterations=1")
    if trace_output is not None and iterations != 1:
        raise UsageError("--trace-output requires --iterations=1")
    params = config.get("schedule", {})
    cmd = _command(params.get("command"), "schedule")
    replay_events = None
    if replay is not None:
        if (root / replay).is_symlink():
            raise UsageError("schedule replay input cannot be a symbolic link")
        trace_path = _relative(root, replay)
        expected_seed, replay_events = _schedule_trace(trace_path)
        args = ["--replay=" + str(trace_path)]
    else:
        if seed > (2 ** 63 - 1) - (iterations - 1) or seed < -(2 ** 63):
            raise UsageError("schedule seed sequence exceeds signed 64-bit range")
        expected_seed = seed
        args = ["--schedule-fuzz", "--seed=" + str(seed),
                "--iterations=" + str(iterations)]
    # The backend must count even guest-caught deadlocks as test failures.
    args.append("--deadlock")
    output_trace = None
    if trace_output is not None:
        if (root / trace_output).is_symlink():
            raise UsageError("schedule trace output cannot be a symbolic link")
        output_trace = _relative(root, trace_output)
        if output_trace.exists():
            raise UsageError("schedule trace output must be new, not stale")
        if replay is not None and output_trace == trace_path:
            raise UsageError("schedule trace output cannot overwrite replay input")
        args += ["--trace-output=" + str(output_trace)]
    result = _run(cmd + args, root, timeout)
    if result is None:
        print("SCHEDULE TIMEOUT")
        return 1
    stdout = result.stdout.decode("utf-8", errors="replace")
    stderr = result.stderr.decode("utf-8", errors="replace")
    # Every report must contain independently checkable case records,
    # its summary, and its receipt, in this order. A fabricated SUMMARY
    # alone is not sufficient evidence that guest cases executed.
    runs = []
    case_counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    summary_line = None
    valid = result.returncode == 0
    for line in stdout.splitlines():
        kind = next((status for status in case_counts
                     if line.startswith(f"ORES_TEST|{status}|")), None)
        if kind is not None:
            if summary_line is not None:
                valid = False
                break
            case_counts[kind] += 1
        elif line.startswith("ORES_TEST|SUMMARY|"):
            if summary_line is not None:
                valid = False
                break
            summary_line = line
        elif line.startswith("ORES_CONCURRENCY|RUN|"):
            if summary_line is None:
                valid = False
                break
            runs.append((summary_line, line, dict(case_counts)))
            summary_line = None
            case_counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
    if summary_line is not None or any(case_counts.values()):
        valid = False
    summary_pattern = re.compile(
        r"ORES_TEST\|SUMMARY\|total=([0-9]+)\|passed=([0-9]+)"
        r"\|failed=([0-9]+)\|skipped=([0-9]+)\|filtered=([0-9]+)")
    receipt_pattern = re.compile(
        r"ORES_CONCURRENCY\|RUN\|seed=(-?[0-9]+)\|events=([0-9]+)"
        r"\|deadlocks=([0-9]+)\|schedule=(normal|perturbed)\|race_coverage=none")
    valid &= len(runs) == iterations
    if valid:
        for i, (line, receipt, counted) in enumerate(runs):
            sm = summary_pattern.fullmatch(line)
            rm = receipt_pattern.fullmatch(receipt)
            if sm is None or rm is None:
                valid = False
                break
            total, passed, failed, skipped, filtered = map(int, sm.groups())
            observed_seed, events, deadlocks = map(int, rm.groups()[:3])
            if (passed < 1 or total != passed + failed + skipped or failed != 0
                    or counted["PASS"] != passed or counted["FAIL"] != failed
                    or counted["SKIP"] != skipped
                    or observed_seed != expected_seed + i or events < 1
                    or (replay_events is not None and events != replay_events)
                    or deadlocks != 0):
                valid = False
                break
    if valid and output_trace is not None:
        try:
            trace_seed, trace_events = _schedule_trace(output_trace)
            reported = receipt_pattern.fullmatch(runs[0][1])
            valid &= (reported is not None and trace_seed == expected_seed
                      and trace_events == int(reported.group(2)))
        except (UsageError, OSError):
            valid = False
    if not valid:
        print("SCHEDULE FAIL: backend did not provide complete native suite receipts")
        print((stdout + stderr)[:8000])
        return 1
    print(f"SCHEDULE PASS: {iterations} backend perturbation/replay runs "
          "(carrier interleavings are not deterministic)")
    return 0


def _show_races(report):
    if report["race_count"]:
        for conflict in report["races"][:20]:
            print(f"DATA RACE: {conflict}")
        print(f"{report['race_count']} conflicts in {report['events']} observed events (first 100 shown)")
        return 1
    print(f"TRACE PASS: {report['events']} observed events on {report['backend']} (no conflicts in observed trace; not a full-race-freedom proof)")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="oreslang test")
    parser.add_argument("--root", default=".")
    parser.add_argument("--fuzz", action="store_true")
    parser.add_argument("--race", action="store_true")
    parser.add_argument("--deadlock", action="store_true")
    parser.add_argument("--model-check", action="store_true")
    parser.add_argument("--schedule-fuzz", action="store_true")
    parser.add_argument("--replay", help="project-relative backend schedule replay trace")
    parser.add_argument("--trace-output", help="project-relative schedule trace output")
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--logical", action="store_true")
    parser.add_argument("--trace", help="project-relative ORES_RACE_V1 JSONL or Java runtime cycle trace")
    parser.add_argument("--model", help="bounded actor model JSON (project-relative)")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--cases", type=int, default=100)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--max-schedules", type=int, default=10000)
    args = parser.parse_args(argv)
    try:
        if args.fuzz and (args.race or args.deadlock or args.model_check or args.logical
                          or args.model or args.trace or args.schedule_fuzz):
            raise UsageError("--fuzz cannot combine with other testing modes")
        if (args.logical or args.model) and not (args.race or args.model_check):
            raise UsageError("--logical/--model requires --race or --model-check")
        if args.model_check and (args.race or args.deadlock):
            raise UsageError("--model-check explores reference models; do not combine with instrumented --race/--deadlock")
        if args.trace and not (args.race or args.deadlock):
            raise UsageError("--trace requires --race or --deadlock")
        if args.replay and args.schedule_fuzz:
            raise UsageError("--replay and --schedule-fuzz are mutually exclusive")
        if (args.replay or args.trace_output or args.iterations != 1) and not (args.schedule_fuzz or args.replay):
            raise UsageError("schedule options require --schedule-fuzz or --replay")
        if (args.schedule_fuzz or args.replay) and (args.race or args.deadlock or args.fuzz or args.model_check):
            raise UsageError("schedule runner is a separate backend test mode")
        if not 0.01 <= args.timeout <= 600:
            raise UsageError("--timeout must be between 0.01 and 600 seconds")
        root = Path(args.root).resolve(strict=True)
        if not root.is_dir():
            raise UsageError("project root must be a directory")
        manifest = root / "oreslang-test.toml"
        if manifest.is_file():
            with manifest.open("rb") as fh:
                config = tomllib.load(fh)
        else:
            config = {}
        if args.schedule_fuzz or args.replay:
            return schedule(root, config, args.seed, args.iterations,
                            args.timeout, args.replay, args.trace_output)
        if args.fuzz:
            return fuzz(root, config, args.seed, args.cases, args.timeout)
        if args.model_check:
            # The existing bounded reference-model explorer, not full guest
            # compiler model checking. The output explicitly identifies scope.
            return race(root, config, None, True, args.model,
                        args.timeout, args.max_schedules)
        status = 0
        if args.race:
            status |= race(root, config, args.trace, args.logical, args.model,
                           args.timeout, args.max_schedules)
        if args.deadlock:
            status |= deadlock(root, config, args.trace, args.timeout)
        if args.race or args.deadlock:
            return status
        return run_native(root, config, args.timeout)
    except (UsageError, TraceError, ModelError, OSError, ValueError, tomllib.TOMLDecodeError) as exc:
        print(f"oreslang test: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
