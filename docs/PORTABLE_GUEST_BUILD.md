# Opt-in portable Wasm guest build

The experimental compiler branch adds `oreslang-llvmc --target=wasm32`. To build a pinned core-i64 guest artifact:

    node scripts/compile-ores.mjs --compiler=/absolute/oreslang-llvmc --source=/absolute/main.ores --output=/absolute/guest.wasm --environment=browser

The compiler stdout must contain a wasm32-unknown-unknown target triple; the bridge links with Clang, verifies the core Wasm module and rejects host imports. It writes guest.wasm.artifact.json (versioned schema in oreslang-target-profiles) with an exact binary digest and bounded memory, plus guest.wasm.provenance.json. This is a pure, scalar-only ABI; it is not a source effect proof, shared MIR, WIT component or public stable compilation command. The compiler option is still a proposed PR until merged.
