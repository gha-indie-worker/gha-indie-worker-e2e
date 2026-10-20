# Actor reply ownership and procedural match contract (draft v1)

**Scope:** experimental host reference dispatcher at
`transport/serial-actor-reference.mjs`. It exercises the v1 owned binary
envelope, but is **not** a compiler-emitted Oreslang guest ABI, browser Worker,
supervisor or production multi-tenant actor runtime. No backend is claimed to
accept every Oreslang example below.

## Two separate result channels

A callable return is not a message reply. **The preferred form inside
`receive()` is `do match`**, because the handler dispatches messages for
side effects rather than computing a match expression value. Plain `match`
is value-producing and belongs where its result is consumed (for example,
`return match`). This is a style/canonical-form distinction, not a license
to reinterpret a discarded match value as a reply.

```oreslang
define actor WorkerActor as
  @Implements
  receive(Message m): void {
    do match m over {
      on Message.Compute as (text, n) {
        self.respond(Message.Result(text, n + 1));
        self.done();
      }
    }
  }
end
```

`self.respond(reply)` records exactly one reply on the **currently correlated
request**. `self.done()` is an explicit terminal delivery action. Responding
does not itself return from the method or complete the mailbox lane. A one-way
send has no reply context and cannot respond or forward. A request completed
with `done()` but without `respond()` or `forward()` is a protocol failure,
not an implicit nil reply. A handler exit without `done()` is an error.

For a callable whose declared result is a value, both syntactic forms have
different but legitimate control-flow meaning:

```oreslang
// Value-producing match: selected arm produces the expression's result.
fnc result_for(Message m): Message {
  return match m over {
    on Message.Compute as (text, n) {
      return Message.Result(text, n + 1);
    }
  };
}

// Procedural dispatch: arm-level return exits the enclosing callable.
fnc result_do(Message m): Message {
  do match m over {
    on Message.Compute as (text, n) {
      return Message.Result(text, n + 1);
    }
  }
}
```

In the procedural `do match` form, the `return` exits the enclosing callable;
its value is unrelated to the actor reply channel. In the preferred
`receive(...): void` form, arms use `self.respond()` / `self.forward()`
and `self.done()`, **not** a value-return statement.

These are ordinary callable-return values. They MUST NOT invoke the actor reply
channel as a side effect. A `receive(...): void` method MUST NOT return a
`Message.Result` by either form. If the language eventually permits a
non-void `receive(...): Message`, its return type is still not automatically
wired to a remote sender; only explicit response operations do that.

## Typed CONTROL / data operations

* `self.respond(reply)`: once per correlated request; result variant must
  belong to that request's allowed reply set; not available on one-way sends.
* `self.forward(target, message)`: transfer the outstanding reply
  responsibility exactly once to a target, with an owned encoded envelope.
  A forwarded response must satisfy the original typed reply contract.
  Cannot be combined with `respond()`.
* `self.send(target, message)`: independent one-way outbound message. It
  cannot complete or inherit the current reply correlation.
* `self.done()`: terminal completion for **this message delivery**. It is
  **not** `self.end()` (actor lifecycle shutdown), and it does not manufacture
  a reply. A serial actor admits the next delivery only after the current
  handler is fully settled, including queued outbound sends.

The compiler/runtime must reject reply capability use outside its enclosing
actor execution-context, after `done()`, or after the originating delivery
exits. Failure of the callback, a malformed forwarded response or an
over-budget envelope fails the delivery without inventing a response.

## Reference acceptance test

`createSerialOwnedActor({schema, receive, replyVariants, ...})` uses owned
`encodeMessage`/`decodeMessage` on input and response. The JavaScript
`receive` callback is a reference model **only**; its returned JS value is
discarded. Tests verify `Compute("hello",41)` to
`Result("hello",42)`, double-response rejection, no automatic reply, reply
variant enforcement, forwarding, bounded FIFO single flight, snapshot
immutability and stale-capability invalidation.

**Explicit exclusions:** no Java/GraalVM or LLVM source-language
`self.respond()` lowering yet; no Wasm linear-memory pointer/length ABI; no
actual isolated browser actor Worker; no restart supervision, execution-context
resumption or resource reclamation beyond the host test harness. Backends must
fail closed on unsupported syntax or effects until individually verified.
