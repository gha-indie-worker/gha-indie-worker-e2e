import assert from "node:assert/strict";
import test from "node:test";

const actorIoTypes = new Set([
  "$ActorIO",
  "$ActorIOOut",
  "$ActorIOIn",
  "$ActorIOErr",
  "$ActorIOEvents",
  "$ActorIOMeta",
]);

function resolveName({ actor, name }) {
  if (actor && name === "io") return "$ActorIO";
  if (actor && (name === "stdio" || name === "print")) {
    throw new Error("process-level I/O is unavailable inside actors; use io.*");
  }
  if (!actor && name === "io") throw new Error("unknown name 'io'");
  return name;
}

function actorMember(type, member) {
  if (type === "$ActorIO") {
    const table = {
      out: "$ActorIOOut",
      in: "$ActorIOIn",
      err: "$ActorIOErr",
      events: "$ActorIOEvents",
      meta: "$ActorIOMeta",
    };
    if (!(member in table)) throw new Error("unknown actor io member");
    return table[member];
  }
  if (type === "$ActorIOOut" && member === "write") {
    throw new Error("io.out.write is direct-call-only");
  }
  if (["$ActorIOIn", "$ActorIOErr", "$ActorIOEvents", "$ActorIOMeta"].includes(type)) {
    throw new Error("reserved but not implemented");
  }
  throw new Error("unknown member");
}

function validateActorPayload(type) {
  if (actorIoTypes.has(type)) throw new Error(`cannot use ${type} across an actor boundary`);
  if (["Future", "Channel", "Mutex", "TcpConnection", "ActorRef", "SharedMutex"].includes(type)) {
    throw new Error(`cannot use ${type} across an actor boundary`);
  }
  return true;
}

function ioOutWrite({ actor, args, spread = false, typeArgs = false, payloadType }) {
  if (!actor) throw new Error("unknown name 'io'");
  if (typeArgs || spread || args !== 1) {
    throw new Error("io.out.write expects exactly one non-spread value and no type arguments");
  }
  validateActorPayload(payloadType);
  return "Future<void>";
}

test("actor io is intrinsic", () => {
  assert.equal(resolveName({ actor: true, name: "io" }), "$ActorIO");
});

test("process stdio is rejected in actor execution", () => {
  assert.throws(
    () => resolveName({ actor: true, name: "stdio" }),
    /process-level I\/O/,
  );
});

test("root io is not a process alias", () => {
  assert.throws(() => resolveName({ actor: false, name: "io" }), /unknown name/);
});

test("canonical actor channels are reserved", () => {
  const io = resolveName({ actor: true, name: "io" });
  assert.equal(actorMember(io, "out"), "$ActorIOOut");
  assert.equal(actorMember(io, "in"), "$ActorIOIn");
  assert.equal(actorMember(io, "err"), "$ActorIOErr");
  assert.equal(actorMember(io, "events"), "$ActorIOEvents");
  assert.equal(actorMember(io, "meta"), "$ActorIOMeta");
});

test("unimplemented channels fail closed", () => {
  assert.throws(() => actorMember("$ActorIOErr", "write"), /reserved but not implemented/);
});

test("io.out.write validates arity and spread", () => {
  assert.throws(
    () => ioOutWrite({ actor: true, args: 0, payloadType: "string" }),
    /exactly one/,
  );
  assert.throws(
    () => ioOutWrite({ actor: true, args: 1, spread: true, payloadType: "string" }),
    /non-spread/,
  );
});

test("io.out.write returns Future<void> for data", () => {
  assert.equal(
    ioOutWrite({ actor: true, args: 1, payloadType: "string" }),
    "Future<void>",
  );
});

test("actor io capabilities cannot be emitted as data", () => {
  for (const type of actorIoTypes) {
    assert.throws(
      () => ioOutWrite({ actor: true, args: 1, payloadType: type }),
      /cannot use/,
    );
  }
});

test("live actor transport capabilities are not output data", () => {
  for (const type of ["ActorRef", "SharedMutex"]) {
    assert.throws(
      () => ioOutWrite({ actor: true, args: 1, payloadType: type }),
      /cannot use/,
    );
  }
});

test("capturing io.out.write as a normal function is rejected", () => {
  assert.throws(() => actorMember("$ActorIOOut", "write"), /direct-call-only/);
});

test("process stdout shortcuts cannot become actor aliases", () => {
  for (const spelling of ["stdio.println", "stdio.stdout.log", "stdio.stdout.logList"]) {
    const root = spelling.split(".")[0];
    assert.throws(
      () => resolveName({ actor: true, name: root }),
      /process-level I\/O/,
    );
  }
});
