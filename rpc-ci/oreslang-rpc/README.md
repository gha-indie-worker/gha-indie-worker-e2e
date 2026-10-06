# oreslang-rpc

Pure Oreslang RPC contracts, HTTP bindings, and interface-description generation.

There is no Java implementation layer in this package. The contract graph, validation, HTTP/RPC conversion, and schema emitters are all written in native Oreslang.

## What it does

`oreslang-rpc` keeps one transport-neutral contract graph and can project it in both directions:

- HTTP handler metadata -> RPC method definition
- RPC method definition -> HTTP handler metadata

The same graph emits:

- JSON Schema 2020-12
- Protocol Buffers v3 message definitions
- gRPC service definitions, including client/server/bidirectional streaming flags
- MessagePack shared definitions (`mpackdef v1`)
- TypeSpec models and HTTP route decorators

Those outputs are intended to become stable inputs for later TypeScript, Rust, Go, Dart, Gleam, Swift, Kotlin, and other client generators.

## HTTP routing interoperability

The package deliberately uses the same stable route metadata already carried by
[`oreslang-http-routing`](https://github.com/ores-truffle-oreslang/oreslang-http-routing):

- `handler_id`
- `pipeline_id`
- `contract_id`
- `operation_id`

The routing library owns path matching and dispatch. The RPC library owns wire-contract identity, request/response types, RPC service/method identity, and generated schemas.

A route generated from an RPC definition therefore preserves the exact IDs expected by `RouterBuilder.route(...)`. The reverse direction accepts those same HTTP handler fields and raises them into an RPC method.

See `examples/http_routing_interop.ores`.

## Contract model

Core definitions live in `src/rpc.ores`:

- `TypeRef`
- `FieldDef`
- `MessageDef`
- `RpcMethodDef`
- `HttpHandlerDef`
- `ContractSet`
- `ContractBuilder`

The builder fails closed on duplicate message names, duplicate wire field names/numbers, duplicate contract IDs, duplicate operation IDs, duplicate HTTP method/path pairs, unresolved request/response message types, and HTTP bindings without a matching RPC method.

Field numbers are shared by protobuf and MessagePack so one compatibility policy governs both representations.

## Example

```ores
import module rpc from "./src/rpc.ores";

pub routine main(): void {
  val request = rpc.message("GetUserRequest", arr[
    rpc.field("id", 1, "1", rpc.string_type(), true, false)
  ]);

  val response = rpc.message("User", arr[
    rpc.field("id", 1, "1", rpc.string_type(), true, false),
    rpc.field("display_name", 2, "2", rpc.string_type(), true, false)
  ]);

  let contracts = rpc.builder();
  contracts.add_message(request).unwrap();
  contracts.add_message(response).unwrap();

  val method = rpc.rpc_method(
    42,
    "Users",
    "GetUser",
    "users.get",
    "GetUserRequest",
    "User"
  );

  contracts.add_rpc_as_http(
    method,
    "GET",
    "/users/{id}",
    1001,
    7
  ).unwrap();

  val graph = contracts.build().unwrap();

  val outputs = rpc.generate_all(
    &graph,
    "https://example.test/schema/rpc",
    "example.rpc",
    "Example.Rpc"
  );

  // TextDocument is a sequence of native Oreslang String fragments.
  // A CLI/file/socket adapter can stream them without a Java StringBuilder.
  stdio.println(outputs.protobuf.part_count());
}
```

## Native text emission

The current Oreslang core intentionally has a small string surface. Generators therefore return a `TextDocument`: an ordered sequence of immutable string fragments and newlines.

That has two advantages:

1. schema generation requires no Java/host string builder;
2. callers can stream fragments directly to stdout, files, sockets, channels, or future build-tool APIs without first constructing one giant host string.

## MessagePack

MessagePack has no universal schema language equivalent to `.proto`. This package emits a deliberately small `mpackdef 1` shared definition. Maps use stable positive integer field keys matching protobuf field numbers.

See `docs/MESSAGEPACK.md` for compatibility rules.

## Validation

`tests/rpc_semantics.ores` exercises:

- HTTP -> RPC conversion
- RPC -> HTTP conversion
- contract/operation ID preservation
- request/response registration
- duplicate wire-field rejection
- missing-message rejection
- JSON Schema generation
- protobuf/gRPC generation
- MessagePack definition generation
- TypeSpec generation

The repository pins the compiler revision in `SOURCE_REF`.
