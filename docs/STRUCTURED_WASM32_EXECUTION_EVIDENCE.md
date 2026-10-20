# Structured Wasm32 source execution evidence

Validated in a private, token-free exact-source GitHub Actions run:

- [Initial successful run](https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/38112987480).
- [Reconciled LLVM codegen Git-blob verification and successful retest](https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/38113193431).

The real C++ LLVM compiler produced target-marked IR, Clang linked core Wasm32, and Node executed four Oreslang source fixtures: bounded multi-field shared read, independent mutation, exclusive mutable borrow and a guarded return. Four outputs matched 42n. Five invalid source cases were rejected before any LLVM IR, including overlapping borrows and native heap-region admission.

The independent Wasm bridge compiled two real structured examples into zero-import core Wasm modules with exact SHA-256 artifact manifests and verified they returned 42n. An invalid overlapping mutable/shared borrow was rejected before artifact publication.

Codegen was retested with the exact Git blob from the reconciled LLVM PR at the time of the test. Subsequently, LLVM main gained further actor-order and exception-policy/parser changes; therefore LLVM PR #140 needs refreshed current-head aggregate verification before merge. The Wasm fixture PR #2 is independently merged.

**Not established:** stable struct ABI across the host boundary, owned heap objects inside Wasm actors, source-level Wasm actor lowering, Java/Graal parity, or production-ready sandboxing.
