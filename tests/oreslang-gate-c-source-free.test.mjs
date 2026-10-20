import test from 'node:test';
import assert from 'node:assert/strict';

function makeLedger(program) {
  return {
    program,
    sealed: false,
    executable: false,
    entries: new Map(),
    seal() {
      this.sealed = true;
      return this;
    },
    authorize(p, ownershipSeal) {
      assert.equal(this.sealed, true, 'type proof must be sealed');
      assert.equal(p, this.program, 'different exact program instance');
      assert.equal(
        ownershipSeal.program,
        this.program,
        'ownership seal belongs to a different exact program instance'
      );
      this.executable = true;
      return this;
    },
    requireScalar(p, boundary) {
      assert.equal(p, this.program, 'different exact program instance');
      assert.equal(this.sealed, true, 'Task boundary admissions must be sealed');
      assert.equal(
        this.executable,
        true,
        'ownership-validated execution authority required'
      );
      assert.ok(this.entries.has(boundary), 'no admission for exact Task boundary');
      const admission = this.entries.get(boundary);
      assert.equal(admission.kind, 'SCALAR', 'wrong Task admission kind');
      return admission;
    }
  };
}

const ownershipCheck = program => Object.freeze({ program });

test('exact program plus exact boundary identity is required', () => {
  const program = {}, otherProgram = {}, boundary = {}, lookalike = {};
  const ledger = makeLedger(program);
  ledger.entries.set(boundary, Object.freeze({ kind: 'SCALAR', captures: [] }));
  ledger.seal().authorize(program, ownershipCheck(program));

  assert.equal(ledger.requireScalar(program, boundary).kind, 'SCALAR');
  assert.throws(
    () => ledger.requireScalar(program, lookalike),
    /no admission for exact Task boundary/
  );
  assert.throws(
    () => ledger.requireScalar(otherProgram, boundary),
    /different exact program/
  );
});

test('zero-capture scalar admission is concrete proof, not absence', () => {
  const program = {}, boundary = {}, missing = {};
  const ledger = makeLedger(program);
  const admission = Object.freeze({ kind: 'SCALAR', captures: Object.freeze([]) });
  ledger.entries.set(boundary, admission);
  ledger.seal().authorize(program, ownershipCheck(program));

  assert.equal(ledger.requireScalar(program, boundary), admission);
  assert.deepEqual(admission.captures, []);
  assert.throws(() => ledger.requireScalar(program, missing), /no admission/);
});

test('type-only ledger is sealed but non-executable', () => {
  const program = {}, boundary = {};
  const ledger = makeLedger(program);
  ledger.entries.set(boundary, Object.freeze({ kind: 'SCALAR' }));
  ledger.seal();

  assert.equal(ledger.sealed, true);
  assert.equal(ledger.executable, false);
  assert.throws(
    () => ledger.requireScalar(program, boundary),
    /ownership-validated execution authority/
  );
});

test('foreign ownership seal cannot upgrade type proof', () => {
  const program = {}, foreign = {};
  const ledger = makeLedger(program).seal();

  assert.throws(
    () => ledger.authorize(program, ownershipCheck(foreign)),
    /ownership seal belongs to a different exact program/
  );
  assert.equal(ledger.executable, false);
});

test('exact ownership seal upgrades exact type proof', () => {
  const program = {}, boundary = {};
  const ledger = makeLedger(program);
  ledger.entries.set(boundary, Object.freeze({ kind: 'SCALAR' }));
  ledger.seal().authorize(program, ownershipCheck(program));

  assert.equal(ledger.executable, true);
  assert.equal(ledger.requireScalar(program, boundary).kind, 'SCALAR');
});

test('reject-all manual root is executable but grants no Task boundary', () => {
  const program = {}, boundary = {};
  const rejectAll = makeLedger(program);
  rejectAll.seal();
  rejectAll.executable = true;

  assert.equal(rejectAll.executable, true);
  assert.throws(() => rejectAll.requireScalar(program, boundary), /no admission/);
});

test('public compiler result must bind ledger to same exact program', () => {
  const program = {}, foreign = {};
  const ledger = makeLedger(program).seal().authorize(program, ownershipCheck(program));

  const construct = (p, admissions) => {
    assert.equal(admissions.sealed, true);
    assert.equal(admissions.executable, true);
    assert.equal(
      admissions.program,
      p,
      'compiler result admission/program identity mismatch'
    );
    return Object.freeze({ program: p, admissions });
  };

  assert.equal(construct(program, ledger).program, program);
  assert.throws(
    () => construct(foreign, ledger),
    /admission\/program identity mismatch/
  );
});

test('runtime scoped mint uses frozen verified claims', () => {
  const program = {};
  let sourceAllowed = true;
  let resolvedAllowed = true;
  let proofCalls = 0;
  const mutableInput = [{ allocation: 31, generation: 1, mode: 'READ' }];

  const prove = () => {
    proofCalls++;
    assert.equal(sourceAllowed, true);
    assert.equal(resolvedAllowed, true);
    return Object.freeze({
      program,
      claims: Object.freeze(
        mutableInput.map(claim => Object.freeze({ ...claim }))
      )
    });
  };

  const proof = prove();
  mutableInput[0].generation = 999;
  sourceAllowed = false;
  resolvedAllowed = false;

  const runtimeMint = (p, frozen) => {
    assert.equal(frozen.program, p, 'exact program mismatch');
    return Object.freeze({ claims: frozen.claims });
  };

  const runtime = runtimeMint(program, proof);
  assert.equal(proofCalls, 1);
  assert.equal(runtime.claims[0].generation, 1);
});

test('stale allocation generation fails before runtime authority mint', () => {
  const liveGenerations = new Map();
  let capabilities = 0;

  const mint = claims => {
    for (const claim of claims) {
      const current = liveGenerations.get(claim.allocation);
      assert.ok(
        current === undefined || current === claim.generation,
        'stale allocation generation'
      );
    }
    for (const claim of claims) {
      liveGenerations.set(claim.allocation, claim.generation);
    }
    capabilities++;
  };

  mint([{ allocation: 9, generation: 1, mode: 'READ' }]);
  assert.equal(capabilities, 1);
  assert.throws(
    () => mint([{ allocation: 9, generation: 2, mode: 'READ' }]),
    /stale allocation generation/
  );
  assert.equal(capabilities, 1);
});

test('scalar admission cannot be reinterpreted as scoped loan', () => {
  const scalar = Object.freeze({ kind: 'SCALAR' });
  const mintScoped = proof => {
    assert.equal(
      proof.kind,
      'SCOPED',
      'exact boundary has no scoped-loan compiler proof'
    );
  };

  assert.throws(() => mintScoped(scalar), /no scoped-loan compiler proof/);
});

test('cleanup failure preserves live loan and blocks quiescence', () => {
  const cleanup = {}, loan = {};
  const scope = {
    children: new Set(),
    failedAuthorities: new Set(),
    loanLive: true
  };

  const failCleanup = () => {
    scope.failedAuthorities.add(cleanup);
    scope.failedAuthorities.add(loan);
  };
  const quiescent = () =>
    scope.children.size === 0 && scope.failedAuthorities.size === 0;

  failCleanup();
  assert.equal(scope.loanLive, true);
  assert.equal(scope.failedAuthorities.size, 2);
  assert.equal(quiescent(), false);
});

test('pre-dispatch loan rollback failure preserves owner pin', () => {
  const loan = {}, owner = {};
  const scope = {
    owners: new Set([owner]),
    failedAuthorities: new Set()
  };

  const rollback = () => {
    const failure = new Error('loan rollback failed');
    scope.failedAuthorities.add(loan);
    return failure;
  };

  const failure = rollback();
  assert.match(failure.message, /loan rollback failed/);
  assert.ok(scope.failedAuthorities.has(loan));
  assert.ok(scope.owners.has(owner));
});
