# Oreslang Semantic Conformance — deeper runtime and diagnostic proofs

**Separate Java and LLVM compiler + ownership checker code.** These cases
extend [the admission suite](../README.md) with stronger oracles.

- Each diagnostic case must return semantic rejection **exit code 1** and
  identify a normalized *class*, not merely any compiler error.
- Each LLVM native case must emit valid module IR, compile using Clang,
  actually execute the binary, and match the declared process exit code.
- Native test values are restricted to 0–125. Process exit status truncation
  makes this unsuitable for arbitrary integer results or exceptions.
- A separate normalized comparator verifies report manifest integrity,
  exact case inventory, diagnostic classes, runtime values and backend readiness.
- A missing compiler, signal, timeout, linker failure, wrong result, or
  infrastructure error **always fails a required case**.
- Pending indicates missing implementation or missing verification; it is
  *not* conformance. Graal runtime-result support remains explicitly pending.
- TypeSpec defines portable interchange models; JSON Schema 2020-12 validates
  constrained cases. TypeSpec and schemas are both compiled/validated in CI.

## Run in LLVM source checkout

```sh
python3 -m pip install 'jsonschema>=4.20,<5'
python3 contracts/v1/semantics/test_runner.py -v
python3 contracts/v1/semantics/run.py --validate
./contracts/v1/node_modules/.bin/tsp compile contracts/v1/semantics
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
python3 contracts/v1/semantics/run.py \
  --backend llvm --command './build/oreslang-llvmc {source}' \
  --clang clang --revision "$(git rev-parse HEAD)" \
  --report build/semantics-llvm.json
```

## Run in Graal source checkout

```sh
mvn -B -ntp -DskipTests package
mvn -B -ntp -q dependency:build-classpath -Dmdep.outputFile=target/conformance-classpath.txt
python3 contracts/v1/semantics/run.py \
  --backend graal --command 'python3 contracts/v1/java_probe.py {source}' \
  --revision "$(git rev-parse HEAD)" --report target/semantics-graal.json
```

After both reports exist, use:

```sh
python3 contracts/v1/semantics/compare.py \
  --llvm build/semantics-llvm.json --graal target/semantics-graal.json
```

**Current limitations:** no shared runtime value ABI beyond a small native
process exit code; no validated Java native result adapter; no LLVM borrow
checker; no automatic actor/preemption conformance yet. Proposed functionality
is recorded as pending instead of turned into false passing tests.

Follow-up work: integer width/overflow specification, stable diagnostic IDs
rather than regular-expression mappings, canonical Oreslang JSON stdout,
actor message event traces, cycle-safe reference/ownership cases, replay-fenced
cancellation and fair preemption under strict resource budgets.

## Externally admitted actor messages

The actor trace protocol now defines an `admit` event: a trusted producer
successfully enqueues a message into a live actor's native mailbox. It carries
`actor` (the receiver) and `message_id`, without inventing an actor identity
for the external host thread. Subsequent `receive` must have the same ID
and can occur at most once. All existing `send` semantics remain unchanged.

LLVM's `oreslang-actor-trace-test` and `traces/native_probe.py` drive a
real `HungryCarrier` with 24 accepted messages, wait for corresponding
single-threaded handler callbacks, verify `CarrierStats` and lifecycle,
then validate the complete causally reconstructed trace via JSON Schema and
the backend-neutral state machine. The funded workflow repeats this 12 times.

**Limits:** This tests the native continuation carrier, not actors compiled
from `.ores`, not realtime isolation, and not cancellation traces or a
production tracing API. The sequence is reconstructed after join from real
admission and callback evidence, not a timestamped total order. Java/Graal
shares the schema and adversarial validator but does not yet emit native
carrier traces.
