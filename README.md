# Oreslang Test CLI

The new test command runner for the Oreslang language.

**Status: working host-side prototype, not yet a fully instrumented language feature.** Python 3.11+ (standard library only). The Java/GraalVM and LLVM runtimes do not yet emit comprehensive memory-access traces, and actual guest schedule control is still to be implemented. Tests here are conformance tests of the tool and synthetic reference models.

## Install and try it

Put the repository's bin directory on PATH, with ORESLANG_BIN pointing to your *existing* Oreslang launcher (not to the shim itself). Non-test commands delegate to that launcher. The alternative executable oreslang-test accepts the test options directly.

    export PATH=/path/to/oreslang-test.cli/bin:$PATH
    export ORESLANG_BIN=/path/to/real/oreslang

    oreslang test
    oreslang test --fuzz --seed 87 --cases 2000
    oreslang test --race
    oreslang test --race --trace tests/traces/recorded.jsonl
    oreslang test --race --logical --model tests/models/lost-update.json

A project must contain oreslang-test.toml. Native tests are automatically discovered as tests/**/test_*.ores and tests/**/*_test.ores, or listed via [native].files. The existing Oreslang stdlib/testing.ores machine protocol (ORES_TEST|...) is parsed and validated. An interpreter failure, absent summary or inconsistent counts makes the suite fail.

Run the included test suite with:

    python3 -m unittest discover -s tests -v
    python3 bin/oreslang test --race --trace tests/traces/racy.jsonl
    python3 bin/oreslang test --race --logical --model tests/models/lost-update.json

The two example invocations deliberately report detected defects.

## Fuzzing

Configure a real executable target that reads a raw test case on standard input and exits nonzero when a property fails:

~~~toml
[fuzz]
command = ["/path/to/oreslang", "tests/fuzz_stdin.ores"]
corpus = "tests/corpus/my-property"
max_bytes = 256
~~~

The runner mutates seed/corpus bytes reproducibly using --seed, runs one isolated process per --cases input, enforces --timeout, stops on failure and attempts delta-debugging shrinkage without changing the failure class. Minimized case bytes are saved under .oreslang-test/failures/fuzz-<hash>.bin. Reproduce by redirecting these bytes into the configured command. This is *mutation fuzzing*, not coverage-guided fuzzing yet; there is no automatic source-level fuzz target discovery or Oreslang-native input generation yet.

## Data races and happens-before

The race analyzer consumes version-1 JSONL records with explicit task and memory location identities. Supported events are task; fork/join with child; lock/unlock with name; send/recv with message; and access with location and mode R/W. It checks unordered conflicting accesses using vector clocks. A minimal intentionally racy record sequence is:

~~~jsonl
{"op":"begin","version":1,"run_id":"test","backend":"graal"}
{"op":"task","task":"a"}
{"op":"task","task":"b"}
{"op":"access","task":"a","location":"heap:alloc-1.field","mode":"W"}
{"op":"access","task":"b","location":"heap:alloc-1.field","mode":"R"}
{"op":"end","run_id":"test","events":4}
~~~

Production runtime integrations must write a *fresh* trace to ORESLANG_RACE_TRACE and echo the invocation's ORESLANG_RACE_RUN_ID in begin/end markers:

~~~toml
[race]
command = ["/path/to/instrumented-oreslang", "tests/race_suite.ores"]
~~~

The tool refuses missing, stale, truncated, malformed and failed traces. Task identifiers mean physical concurrency contexts; all relevant synchronization including actor turn serialization and mailbox handoff must be captured. Memory locations must use stable allocation identities to avoid address reuse false positives. No checker's silence can prove absence of races that instrumentation did not observe. Sample traces here are synthetic.

## Logical races in actor programs

In addition to physical data races, a reentrant actor can suspend after reading a snapshot, then resume and write a stale value. This is a logical race even if native memory accesses are correctly synchronized.

For now --race --logical explores *declarative bounded models*, not compiled Oreslang programs. Each actor contains arrays of atomic turns; boundaries represent suspension. The model supports read, write, atomic_add and final expected integer invariants. The explorer enumerates enabled actor turns and reports a counterexample schedule on invariant failure. If --max-schedules is hit before exploration completes, the result is **inconclusive** (exit code 2). Example files: tests/models/lost-update.json and tests/models/atomic-update.json.

~~~toml
[logical]
models = ["tests/models/atomic-update.json"]
~~~

## Exit codes and integration milestones

- **0:** Tests or observed trace/model run passed. Never interpret this as proof of complete race freedom.
- **1:** Executed test failed, fuzz counterexample, data-race conflict or model invariant violation.
- **2:** Invalid configuration, missing instrumented backend, invalid/incomplete trace, or inconclusive model search.

The native language testing API remains in oreslang-source.java/stdlib/testing.ores. The shared ACTOR_LOGICAL_RACE_V1 contract in Java and LLVM currently certifies the VersionedActorCell *host-side foundation only*, not guest-level enforcement.

Before promoting these flags as general automatic detectors, integrate compiler/runtime memory-access hooks, actor mailbox/lock/suspend happens-before edges, coverage accounting, real guest controlled scheduling, cancellation/continuation generation checks, Java native-image parity, LLVM sanitizer builds, and cross-backend reproducible regression tests. The CLI must continue to fail closed in the absence of such instrumentation.



## Experimental deadlock and schedule diagnostics (October 18, 2026)

The canonical `oreslang test` wrapper now adds **opt-in**, bounded analysis
without pretending that guest instrumentation is complete.

```sh
# Version-one JSONL trace from an instrumented runtime:
oreslang test --deadlock --trace tests/traces/lock-orders.jsonl
oreslang test --race --deadlock --trace tests/traces/instrumented.jsonl

# Systematic bounded exploration of declarative actor-turn reference models:
oreslang test --model-check --model tests/models/atomic-update.json

# Or run a configured instrumented target automatically:
oreslang test --deadlock
```

The same `--deadlock --trace` command also accepts the Java/Graal
`ORES_CONCURRENCY_TRACE_V1` checkpoint file. It reports explicit
`deadlock-future-cycle` and `deadlock-mutex-cycle` receipts. A file containing
only task-turn checkpoints is **inconclusive** (exit 2), because it provides no
mutex/wait-graph evidence. Such checkpoint files contain no invocation nonce
and may be used only for offline inspection, not trusted fresh-run attestation.

A `[deadlock].command` (or existing `[race].command`) must emit a **fresh**
ORES_RACE_V1 JSONL trace at `ORESLANG_RACE_TRACE`, echoing the random
`ORESLANG_RACE_RUN_ID` in the begin/end records. A missing trace or no observed
lock acquisitions/waits is *inconclusive* (status 2), not a PASS. The detector:

- Recognizes potential lock-order inversion (successful nested acquisition
  establishes A-before-B; another task establishes B-before-A).
- Recognizes observed unresolved task wait cycles from `wait` records of the
  form `{"op":"wait","task":"a","name":"lock-b"}`, discharged by
  `{"op":"unwait","task":"a","name":"lock-b"}`. These are optional extensions
  of the version-one trace vocabulary, for this analysis only.
- Refuses inconsistent ownership, forged lock waits, malformed or stale runs,
  and unbounded event/graph work. Lock-order cycles indicate **risk**; wait
  cycles observed at trace end do not establish that cancellation or timeouts
  can never resolve them. The analysis does not cover arbitrary remote RPC
  waits, actor mailbox starvation, or all future program schedules.

The Java/Graal compiler branch also offers experimental scheduler-turn
perturbation and trace replay. To delegate to its new test launcher, configure
the **actual** backend command rather than pretending the wrapper owns the
language scheduler:

```toml
[schedule]
command = ["/path/to/oreslang-compiler", "test", "tests/concurrency.ores"]
```

```sh
oreslang test --schedule-fuzz --seed 42 --iterations 20 --timeout 60
oreslang test --schedule-fuzz --seed 42 --trace-output evidence.trace
oreslang test --replay evidence.trace --timeout 60
```

The wrapper requires native Oreslang `ORES_TEST|SUMMARY` and
`ORES_CONCURRENCY|RUN` receipts for every schedule run. Case-level PASS/FAIL/SKIP
lines must precede each summary, match its counts, and include at least one
actual PASS; fabricated summaries, duplicate metadata fields, and all-skipped
runs fail closed. It checks
per-iteration seeds, well-formed and consistent summary counts, nonzero
checkpoint coverage, and zero observed deadlocks, and passes `--deadlock`
to the Java runtime. Replay traces must be bounded and fully well-formed;
when `--trace-output` is requested, its newly created V3 trace must exist
and its seed and event count must match the backend receipt. Missing or
stale traces cannot produce schedule PASS. `--replay`
requires a V3 trace bound to the seed, entry-file SHA-256, bounded workspace
source snapshot SHA-256, backend mode/platform and probe version. A workspace
snapshot conservatively includes compiler/stdlib sources and schema manifests,
but is not a fully resolved import graph, does not cover imports outside the
workspace, and does not attest the deployed compiler binary. Exact Git-tree
CI remains required for independently verifiable build provenance. It does **not**
synthesize successful receipts, does not claim deterministic physical carrier
ordering, and does not infer data-race freedom from scheduler perturbations.

Cross-backend host analyzers now agree on important observed-trace contracts:
`fork` creates a fresh logical task, `finish` marks its terminal state,
`join` accepts only a finished direct child once, nested critical-section
locks publish ordering only on the final unlock, and message IDs are globally
single-use even after receipt. These are **analyzer contracts**, not guest
compiler/runtime instrumentation or guaranteed replay of carrier threads.

These modes complement the existing **actual** byte-corpus mutation fuzzing
(`--fuzz`) and host-side vector-clock race analyzer (`--race`).
Java/Graal `--race` still refuses an uninstrumented memory-safety verdict;
LLVM guest memory hooks and complete source-level scheduling remain future
work. Analyzed traces are evidence only for the accesses and waits recorded.

## Audit hardening (2026-10-15)

The hardening revision strengthens the host-only tool without claiming language-level instrumentation:

- The vector-clock checker **retains all per-task, per-access-mode histories** (bounded to the last event in each task/mode), rather than silently dropping unordered reads/writes when another task writes. It caps printed conflict examples at 100 while retaining the total count.
- The trace grammar adds `{"op":"finish","task":"..."}`. A `join` now requires a finished, directly forked child that has not already been joined. Any forked child left unfinished at trace termination, post-finish access, duplicate JSON key, or invalid encoding is a hard error.
- Model files are limited to 2 MB, 16 actors, 64 turns, 4096 operations, and 256 state keys. Operation schemas and local references are validated before exploring interleavings.
- Each test subprocess captures no more than 4 MB per stdout/stderr stream in memory; larger outputs fail. Timed-out process groups are terminated on POSIX hosts. A fuzz counterexample is re-executed twice before minimization and the reduced input is also checked twice; flaky first failures become an explicit non-success status.
- `python -m unittest discover -s tests -v` includes race-history, task lifecycle, reentrant-lock, single-use-message, and schedule/replay adversarial cases. The suite's executable test count and outcome must come from an actual runner log, not from source inspection.

The `finish` event is an extension of the v1 JSONL trace vocabulary. Existing traces with independent root tasks remain valid; actual `fork` traces should emit corresponding task completion events. This remains host-side analysis of observed events, and runtime coverage must be separately attested before claiming race freedom.

### Exact-tree concurrency P0 gate

On an authorized private runner, check out the exact PR tree (never a partial
cherry-pick) and set `EXPECTED_SOURCE_TREE_SHA` to its verified Git tree SHA:

```sh
EXPECTED_SOURCE_TREE_SHA=<40-character-tree-sha> bash scripts/ci-verify-concurrency-p0.sh
```

The gate must execute the tests and print `ORES_CONCURRENCY_CI|...|result=PASS`
only after success. Matching Git tree SHA and executable CI logs are required;
failed pre-step GitHub Actions jobs do not count as test evidence. This
script does not grant access to private source across organizations.
