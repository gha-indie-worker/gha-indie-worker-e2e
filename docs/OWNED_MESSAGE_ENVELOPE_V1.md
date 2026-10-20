# Experimental owned actor message envelope v1

This is a **host-side reference binary codec**, not compiler lowering, a Wasm guest ABI, an actor scheduler, or production browser parity.

## Binary structure

Each message has a fixed 44-byte header: ASCII ORM1 (4), version 1 (u8), flags 0 (u8), top-level variant ID (u16 LE), payload length (u32 LE), and SHA-256 fingerprint of the canonical contract (32 bytes). The hash input is UTF-8 `Oreslang owned-message contract v1\n` followed by normalized JSON. Variants and enum alternatives sort by ID, while ordered fields remain order-significant.

Payload is schema-directed: signed full-width i64 BigInt, canonical boolean 0/1, length-prefixed strict UTF-8 strings, copied binary bytes, positional tuples, fixed-length homogeneous arrays, bounded homogeneous lists, and nested tagged enums with required alternatives.

Example contract:

```json
{ "schemaVersion":1, "ownership":"copy", "variants":[{"id":1,"name":"Compute","fields":[{"name":"text","type":"string"},{"name":"n","type":"i64"}]}] }
```

JavaScript usage:

```js
const wire = await encodeMessage(contract, {variant:"Compute",fields:{text:"hello",n:41n}});
const message = await decodeMessage(contract, wire);
```

Descriptors: `string`, `i64`, `bool`, `bytes`, `{tuple:["string","i64"]}`, `{array:{element:"i64",length:3}}`, `{list:{element:"string",maxLength:32}}`, and `{enum:[{id:1,name:"None",payload:null},{id:2,name:"Some",payload:"i64"}]}`.

## Ownership and bounds

- Maximum message size: 1 MiB including header; callers may tighten it.
- Nesting depth at most 12; maximum list/array bound 4096; at most 256 contract variants and 64 fields per variant.
- Total normalized schema complexity is limited to 4096 type descriptor nodes and 64 KiB of canonical UTF-8 JSON. This is separate from the 4096 visited-value node limit that rejects zero-byte nested-container CPU amplification.
- Mandatory ownership policy is copy. Encoder snapshots typed bytes, and decoder snapshots its entire input before asynchronous WebCrypto hashing.
- Every array/list element must be an own, present slot; sparse JavaScript arrays are rejected, including for zero-byte tuple elements, so encode/decode length and value-node budget accounting remain consistent.
- SharedArrayBuffer views and implicit type coercions are rejected. No shared linear memory, borrowed pointers, implicit host imports, or zero-copy transfer in v1.
- Unknown variants, alternative IDs, malformed/trailing payloads, incompatible schemas, unsupported flags and versions, invalid UTF-8, noncanonical boolean and over-budget messages fail closed.
- Hashing guarantees matching schema identity, not publisher authenticity or authorization. Consumers must separately enforce host capability grants.

## Integration gates

1. Compiler-declared serializable Message contract and send/reply checking.
2. Versioned guest Wasm memory API with owned host/guest buffers.
3. Browser Worker supervisor with cancellation, single-flight turn admission and bounded queues.
4. Identical executable fixtures through LLVM, Wasm, browser and GraalVM where supported.
5. SHA-exact source, compiler, artifact and schema evidence in funded CI. JS reference tests alone cannot prove actor parity.
