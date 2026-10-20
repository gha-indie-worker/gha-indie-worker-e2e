# Exact-source portability verification — October 10, 2026 (Lima time)

A private GitHub Actions runner executed the exact upstream Git blobs mirrored from the four new Oreslang portability repositories and a pinned native LLVM compiler source snapshot. No tokens or secrets were used for cross-organization source checkout: code was copied into a temporary **private** test branch and inspected by Git object hash.

**Verified Actions run:** https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/38102971891 — completed successfully.

## Verified compilation slice

- Actual native compiler source: `ores-truffle-oreslang/oreslang-source.llvm` at commit `5eba84948cb2836308c0919fad5a74cfd2a5ec01`.
- Built actual `oreslang-llvmc` with the pinned CMake/C++/LLVM source.
- Compiled `examples/native-int.ores` (`pub fnc main(): int { return 40 + 2; }`) to LLVM IR, then real wasm32 code using Clang 18.1.3.
- The linked core Wasm module was 157 bytes, imported **zero** host functions, and exported `main`.
- Instantiated and invoked the resulting Wasm module, obtaining `42n` (Wasm i64).
- Exercised the compiled guest via the `oreslang-browser` loader with `requireRuntimeABI:false` while enforcing raw-byte Wasm admission. Separately loaded the actual freestanding `oreslang-wasm` C runtime through that loader, including mailbox and clock capability tests.

## Pinned evidence

| Field | SHA-256 |
| --- | --- |
| Native compiler executable | `aa182fd030af58391b9f18a06aac851ac047ea317275e0152525be099a47f4be` |
| Input Oreslang source | `662d8610184b7a1f17e12809a78ed2114a4ccf4a924ad30b38b5d9f6a4f54f78` |
| Emitted LLVM IR | `08c5a0260a1fa0a9ec7a4b9a0d3031084b4a5d889b5d986c7ce7b496c7ddde72` |
| Linked Wasm | `3b636c123cf518a75ce53c91beaa208d40ec697c3e201aa310f20ae04402bd5c` |

Four repo suites produced **48 passing test executions, zero failed** in this workflow. The initial normal Wasm test invocation skipped its opt-in native-compiler case; the same case was separately executed successfully after building the real compiler.

## Important boundaries

This verifies only the supported `int` / source-to-LLVM / core Wasm subset. It does **not** prove GraalVM parity, actors, async/await, memory ownership transfer, dynamic imports, WIT components, WASI, iOS, Android, embedded safety, or end-to-end compiler effect inference. The source repository revision is pinned to the exact mirrored source Git blobs; the binary/IR/Wasm hashes provide reproducibility evidence for this particular run.

The four original repositories' own Actions were failing before job steps were allocated, so their hosted checks remain unverified. Do not substitute their run statuses for this private exact-source test evidence.
