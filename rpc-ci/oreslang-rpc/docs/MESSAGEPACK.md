# MessagePack shared definitions

MessagePack intentionally does not define a universal schema/IDL comparable to Protocol Buffers. `oreslang-rpc` therefore emits a small, deterministic **mpackdef v1** description that downstream generators can consume.

The wire representation is a MessagePack map whose keys are positive integer field numbers. Those numbers are the same stable numbers used by the protobuf emitter. This means field order does not matter and one contract can drive protobuf and MessagePack clients without maintaining two numbering schemes.

Example:

```text
mpackdef 1
encoding map-int-keys

message User {
  1 id: str required
  2 display_name: str required
  3 tags: array<str> optional
}

rpc Users.GetUser(GetUserRequest) -> User
```

Compatibility rules:

- never reuse a retired field number for a different meaning;
- adding an optional field is backward compatible;
- removing an optional field is compatible when readers ignore unknown integer keys;
- changing a field's wire type is breaking;
- RPC request/response message names and streaming kind are part of the contract.

The emitter itself is pure Oreslang and returns a `TextDocument` of fragments so callers can stream it to any transport without Java string builders.
