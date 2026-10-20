# Cross-backend static-analysis verification obligations

Status: prototype analysis and bounded unit tests, **not** a compiler or hard-real-time
certificate. Applies to Java/GraalVM, C++/LLVM and the independent Rust compiler.

## Soundness contracts

1. **Source-to-IR:** every reachable program operation with allocation, copy,
   FFI, blocking, suspension, actor transfer, indirect call or exception effects
   must be represented by a verified producer. A supplied JSON operation is
   not evidence that the actual machine operation has that effect or byte cost.
2. **Call closure:** for every resolved call `f -> g`, summarized effects of `g`
   propagate to `f`; unresolved calls and recursive cycles stay unknown unless
   separately justified by authoritative bounds and target-specific evidence.
3. **Bounds:** a known `allocations_upper_bound` or `copies_upper_bound` must
   upper-bound **every** concrete trace of the function on the chosen target,
   including nested calls and loops. Overflow always yields unknown, never zero.
4. **Actor isolation:** writable storage does not escape actor ownership without
   a separately validated capability, lifetime, alias and transfer proof.
5. **Hard real time:** abstract effect-node counts are **not** instructions,
   stack limits or worst-case execution time. Target WCET, interrupts, scheduler,
   allocator, FFI, cache effects and GC policy remain independent proof obligations.
6. **Admission/provenance:** schema version, compiler identity, source commit/tree
   hash, profile, target triple, linked dependencies and trusted effect declarations
   must be authenticated before treating analysis as a release gate.

## Executable checks in this branch

- Fail-closed limits on JSON bytes, function count, operation count, nested IR
  depth, symbol length and recursive analysis call depth.
- Zero-trip loop has no body effects, including no phantom missing callee.
- Finite exhaustive 6x6 matrix of loop iteration and copy byte bounds.
- Unknown allocations, unresolved calls, recursive cycles, actor mutable
  escape and numeric overflow remain negative regression tests.

## Remaining independent evidence

- Per-backend lossless, source-span-aware IR exporters with stable symbol IDs.
- Golden source programs with equivalent Java/LLVM/Rust effects and diagnostics.
- Bounded model checks of ownership transfer, Task cancellation and suspension.
- Runtime trace-to-IR refinement, sanitizer/regression coverage and target WCET.
- Reproducible CI with exact source Git trees and nonempty job logs.

Do not describe a passing `hard` profile as certification of physical hard RT.

## Current audited scalar producer contract

The restricted `oreslang-source.rs` prototype now exports this schema from its
validated MIR via `oresc --emit-effect-ir`: every parsed zero-argument static
function is enumerated, and literal/integer-arithmetic bodies have no modeled
guest alloc/copy/IO/FFI/await/actor effects. `examples/rust-scalar-expected.json`
is a cross-repo *fixture*, not an authenticated source or machine proof. The
analyzer always reports `assurance_level=untrusted_effect_ir_model_only`, even
when the hard profile returns exit zero. An independently verified bound and
signed/provenanced producer attestation remain prerequisites for certification.
