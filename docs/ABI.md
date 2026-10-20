# Core Wasm ABI v1 (experimental)

This is a narrow **freestanding C runtime ABI prototype**, not Oreslang guest-code compilation.

Imports: ores_host.clock_ms : () -> f64.
Exports: memory (2 pages, fixed); ores_rt_abi_version ()->i32;
ores_rt_initialize (capabilityMask:i32)->i32 (1 success, 0 unknown-bit rejection);
ores_rt_add (u32,u32)->u32; ores_rt_enqueue (u32)->i32;
ores_rt_dequeue ()->i32; ores_rt_last_message ()->u32;
ores_rt_clock_ms ()->f64 (-1 if disabled, otherwise host call).

Capability mask bit 0 = clock. Initialization is fail-closed: even an invalid bitmask resets the queue, clears previously requested clock access, and makes subsequent queue operations refuse work until a valid initialization. Requests before initialization are also rejected.

Initialization never increases permissions on its own: **the embedding host must derive the mask from an independently validated manifest and its policy**. A hostile guest can call the export with 1. This prototype does not provide a strong isolation boundary; host imports and policy must be protected by the loader.

Mailbox: capacity eight; FIFO; single global queue; no threads or concurrent execution; enqueue returns 0 on overflow. Zero is a valid message; successful dequeue is indicated separately. No heap allocation, scheduler, interrupts or actor identity. All functions are subject to later ABI versioning.

Memory layout, ABI stability, host capability verification, component model binding, cross-module actors, exception semantics and async support are pending. Do not use as a production security or actor-isolation primitive.


## Pure guest Wasm bridging

`scripts/compile-ores.mjs` builds a **separate**, pure-integer guest module from the native LLVM compiler's emitted IR. It currently exports a zero-argument `main` returning Wasm i64 (JavaScript `BigInt`) and rejects any module imports. It is not linked to the above runtime ABI, which exports `ores_rt_*` and a capability-checked host clock. Joining guest and runtime safely requires a stable ABI for references, memory, async frames and ownership transfer.
