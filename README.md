# Oreslang Static Analysis — Rust

Cross-backend, structured analysis for Oreslang. Independent of GraalVM and LLVM.

## Working prototype

The first checkable contract is a **versioned JSON effect IR**. Java, C++/LLVM, and Rust can emit the same input without sharing private repository code or linking different compilers.

\`\`\`sh
cargo test --workspace
cargo run -p ores-analyze -- examples/hard-realtime.json hard
cargo run -p ores-analyze -- examples/unsafe.json hard
\`\`\`

The CLI prints machine-readable JSON diagnostics, summaries, and uses a nonzero exit code for errors. Source spans, lints, LSP/SARIF and native Oreslang parsing are next; this milestone does **not** claim a full proof of hard real-time behavior.

See [architecture](docs/ARCHITECTURE.md) and [versioned input contract](docs/IR-V1.md).
