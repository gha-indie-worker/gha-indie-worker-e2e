# oreslang-rpc design

## One contract graph

The library keeps one transport-neutral graph:

- `MessageDef` / `FieldDef` / `TypeRef`
- `RpcMethodDef`
- `HttpHandlerDef`
- `ContractSet`

HTTP is a binding of an RPC contract, not a second source of truth. `rpc_from_http` and `http_from_rpc` are deliberately inverse-shaped and preserve `contract_id` plus `operation_id`.

## HTTP routing interoperability

`oreslang-http-routing` already carries four stable metadata values on every route target:

- `handler_id`
- `pipeline_id`
- `contract_id`
- `operation_id`

`HttpHandlerDef` uses that exact seam. The routing library remains responsible for path matching and dispatch; this package remains responsible for request/response contract identity and schema generation.

The path pattern itself stays with the routing library. RPC stores the canonical path template so generated TypeSpec/front-end clients see the same public route identity.

## Code generation

All emitters are native Oreslang:

- JSON Schema 2020-12
- Protocol Buffers v3 + gRPC service definitions
- MessagePack mpackdef v1
- TypeSpec + HTTP decorators

A `TextDocument` is an array of string fragments/newlines, not a host-language builder. This keeps generation portable and allows streaming output.

## Wire compatibility

Field numbers are authoritative for protobuf and MessagePack. `number_text` carries the canonical decimal spelling until the Oreslang standard library exposes a stable integer formatting API.

The builder rejects duplicate message names, duplicate field names/numbers, duplicate contract IDs/operation IDs, duplicate HTTP method/path pairs, missing request/response message types, and HTTP bindings without a corresponding RPC method.

## Future front-end generation

Language-specific client generators should consume `ContractSet` directly or parse one of the emitted shared formats. They should not scrape HTTP source code. That gives TypeScript, Rust, Go, Dart, Gleam, Swift, Kotlin, etc. the same contract identity and wire numbering.
