# Static-analysis architecture

* **Frontend-neutral input:** Java, LLVM/C++, and Cranelift/Rust producers export the exact same versioned effect IR. A source-language frontend must resolve scopes, types, overloads, method receiver conventions, ownership and calls **before** export.
* **Trust boundary:** An IR imported from another compiler is untrusted; validate the schema and IDs, defend against pathological size/depth, and never execute code as part of an analysis.
* **Static passes (near term):** ownership/borrow legality, actor-heap escape, zero-copy and allocation count, stack and recursion bounds, blocking I/O, Task/await cancellation/suspension, effect inference, module ABI evolution, hard/soft real-time policy.
* **Static passes (later):** abstract interpretation, interval/range analysis, path-sensitive deallocation, fixed-point termination proofs, timing/stack certificates, SARIF and LSP diagnostics.
* **Real-time distinction:** bounded abstract operation counts are **not** cycle counts or worst-case execution-time (WCET) proofs. Platform memory models, interrupts, scheduler, dynamic allocators, GC, FFI and all runtime behavior must be covered separately.
* **Hard real-time:** require a complete call graph, finite loop/recursion bounds, known allocation bounds, no unverified potentially blocking calls, validated FFI effects, and a target-specific WCET analysis before claiming a hard-RT guarantee. Zero allocation in a function is not sufficient by itself.
* **Self-hosting:** the JSON contract is a temporary transport. Specify its semantics and test fixtures independently, then replace analyzer/frontend components in Oreslang without changing required behavior.
* **Crate boundaries:** keep this project free of Cranelift, LLVM, GraalVM, and source frontend-specific crates. Binary parsing and LSP/SARIF adapters can be added separately.

This is a prototype. In particular, callers currently supply operation costs; accepting them is not independent verification of full compiler/runtime semantics.
