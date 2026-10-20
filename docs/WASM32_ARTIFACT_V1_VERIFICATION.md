# Wasm32 target and signed-identity artifact v1 verification

Verified against exact Git blobs of five coordinated pull requests on a **private** GitHub Actions runner.

- [Successful exact-source CI run](https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/38111809636)
- Runtime: Ubuntu 24.04, Node 22, native LLVM compiler built with CMake, Clang wasm32 toolchain.
- Native compiler source was pinned to LLVM PR #131 head `e08da6c73d307dd4aff9b79c23497e73f79e9029`; the explicit `--target=wasm32` test passed.
- Compiled a real Oreslang `main(): int` returning 42 through target-marked LLVM IR, linked to core wasm32, and created the `oreslang.core-wasm.i64-v1` artifact manifest.
- Browser admission checked exact SHA-256, bounded memory, entry export and zero imports, then executed the guest.
- The conformance subprocess observed the Wasm i64 result `42`.
- Test suites: 17 target-profile passes, 10 Wasm passes (one optional native-compiler suite skipped in the default npm test), 17 browser passes, 14 conformance passes = **58 passing executions, 0 failures**. The live compiler+artifact+browser end-to-end gate passed separately.

## Exact artifact byte identities

| Item | SHA-256 |
|---|---|
| Native compiler executable | `98e01f96c3872d6fa4aa402e8c6deb5c1245fe4d3c4092e0194d7e5630f52e42` |
| Input Oreslang source | `662d8610184b7a1f17e12809a78ed2114a4ccf4a924ad30b38b5d9f6a4f54f78` |
| Target-marked LLVM IR | `30f79798a7eed4bc6dcb01ba9421304b90e17fcb38f1113c8dc76fa3c90db6a6` |
| Core Wasm binary | `3b636c123cf518a75ce53c91beaa208d40ec697c3e201aa310f20ae04402bd5c` |

## Merged feature PRs

- [Target-profile artifact contract #1](https://github.com/ores-truffle-oreslang/oreslang-target-profiles/pull/1)
- [LLVM wasm32 target #131](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/131)
- [Wasm artifact generation #1](https://github.com/ores-truffle-oreslang/oreslang-wasm/pull/1)
- [Browser digest/ABI gate #1](https://github.com/ores-truffle-oreslang/oreslang-browser/pull/1)
- [Conformance E2E gate #3](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/3)

## Limitations

This is an experimental source-to-LLVM-IR-to-Clang/Wasm path for a narrow
scalar subset. Neither native linking inside `oreslang-llvmc` nor a stable
`oreslang build --target wasm32` public CLI exists yet. No compiler-inferred
effects, mandatory cross-file capability admission, structured values, actors,
async/suspends/await, WIT/WASI integration, or GraalVM/LLVM feature parity
is established by this smoke run.

The SHA-256 hashes prove byte identity, not signatures, authorized publisher
identity, or reproducible mapping from a git commit to a compiler binary. The
browser's WebCrypto validation is integrity checking only and does not
replace host trust/permissions, CPU limits or sandboxing.
