// Source-free lifecycle model for Oreslang TaskScope/Future-chain invariants.
// Contains no Oreslang source, Git objects, policy files, or private metadata.

class Scope {
  constructor() {
    this.closing = false;
    this.cancelled = false;
    this.active = new Set();
    this.failure = null;
  }
  reserve(parent = null) {
    if (this.cancelled) throw new Error("cancelled scope");
    if (this.closing && (!parent || !parent.maySpawn())) {
      throw new Error("closing scope without live child permit");
    }
    const r = new Registration(this);
    this.active.add(r);
    return r;
  }
  close() {
    this.closing = true;
    return this.drained();
  }
  retired(r, failure) {
    if (!this.active.delete(r)) throw new Error("double retire");
    if (!this.failure && failure) this.failure = failure;
  }
  drained() {
    return this.closing && this.active.size === 0;
  }
}

class Registration {
  constructor(scope) {
    this.scope = scope;
    this.began = false;
    this.terminal = false;
    this.quiescent = false;
    this.retired = false;
    this.failure = null;
  }
  beganExecution() {
    if (this.retired || this.terminal || this.quiescent) throw new Error("retired child began");
    this.began = true;
  }
  maySpawn() {
    return this.began && !this.terminal && !this.quiescent && !this.retired;
  }
  terminalReady(failure = null) {
    this.terminal = true;
    this.failure = failure;
    this.maybeRetire();
  }
  physicallyQuiescent() {
    this.quiescent = true;
    this.maybeRetire();
  }
  failedAdmission(failure) {
    this.terminal = true;
    this.quiescent = true;
    this.failure = failure;
    this.maybeRetire();
  }
  maybeRetire() {
    if (!this.retired && this.terminal && this.quiescent) {
      this.retired = true;
      this.scope.retired(this, this.failure);
    }
  }
}

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

// terminal-only does not drain.
{
  const s = new Scope();
  const r = s.reserve();
  r.beganExecution();
  s.close();
  r.terminalReady();
  assert(!s.drained(), "terminal-only drained scope");
  r.physicallyQuiescent();
  assert(s.drained(), "terminal+quiescence did not drain");
}

// quiescence-only does not drain.
{
  const s = new Scope();
  const r = s.reserve();
  r.beganExecution();
  s.close();
  r.physicallyQuiescent();
  assert(!s.drained(), "quiescence-only drained scope");
  r.terminalReady();
  assert(s.drained(), "quiescence+terminal did not drain");
}

// Future-chain child holds scope open and grants callback descendants a live permit.
{
  const s = new Scope();
  const original = s.reserve();
  original.beganExecution();
  const chain = s.reserve(original);
  chain.beganExecution();

  s.close();

  // The callback must use the chain's dynamic permit, not the stale original permit.
  original.terminalReady();
  original.physicallyQuiescent();
  assert(!original.maySpawn(), "retired original permit remained live");

  let staleRejected = false;
  try { s.reserve(original); } catch { staleRejected = true; }
  assert(staleRejected, "stale captured parent reopened closing scope");

  const callbackChild = s.reserve(chain);
  callbackChild.beganExecution();
  callbackChild.terminalReady(new Error("callback-child-failed"));
  callbackChild.physicallyQuiescent();

  assert(!s.drained(), "chain retired before its own terminal+quiescence");
  chain.terminalReady();
  assert(!s.drained(), "chain terminal alone drained scope");
  chain.physicallyQuiescent();
  assert(s.drained(), "chain + callback descendants did not drain");
  assert(s.failure && s.failure.message === "callback-child-failed",
    "callback child failure did not reach scope");
}

// Failed admission is the only no-Future path: both witnesses arrive together.
{
  const s = new Scope();
  const r = s.reserve();
  s.close();
  r.failedAdmission(new Error("admission-failed"));
  assert(s.drained(), "failed admission leaked registration");
  assert(s.failure && s.failure.message === "admission-failed",
    "failed admission did not preserve failure");
}

console.log(
  "ORESLANG_SOURCE_FREE_TASK_SCOPE_PASS " +
  "terminal_quiescence=conjunctive future_chain=structured " +
  "dynamic_parent=chain stale_parent=rejected private_source_bytes=0"
);
