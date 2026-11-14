import test from 'node:test';
import assert from 'node:assert/strict';

// Public independent reference implementation; NO private Oreslang source.
const SLOT_LIMIT = 65536;
const W = 64;
class BitmapIndex {
  constructor(slots = SLOT_LIMIT) {
    if (!Number.isInteger(slots) || slots < 1 || slots > SLOT_LIMIT) throw Error('slots');
    this.slots = slots;
    this.words = Array(Math.ceil(slots / W)).fill(0n);
    this.summary = Array(Math.ceil(this.words.length / W)).fill(0n);
  }
  set(slot, present) {
    if (!Number.isInteger(slot) || slot < 0 || slot >= this.slots) throw Error('index');
    const wi = Math.floor(slot / W), bit = 1n << BigInt(slot % W);
    this.words[wi] = present ? this.words[wi] | bit : this.words[wi] & ~bit;
    const si = Math.floor(wi / W), summaryBit = 1n << BigInt(wi % W);
    this.summary[si] = this.words[wi] ? this.summary[si] | summaryBit : this.summary[si] & ~summaryBit;
  }
  first(start) {
    if (!Number.isInteger(start) || start < 0 || start >= this.slots) throw Error('cursor');
    // Deliberately independent decoding, validating both index levels.
    for (let pass = 0; pass < 2; pass++) {
      const low = pass === 0 ? start : 0, high = pass === 0 ? this.slots : start;
      if (low >= high) continue;
      const firstWord = Math.floor(low / W), lastWord = Math.floor((high - 1) / W);
      for (let wi = firstWord; wi <= lastWord; wi++) {
        const bit = 1n << BigInt(wi % W);
        if (!(this.summary[Math.floor(wi / W)] & bit)) continue;
        let mask = this.words[wi];
        for (let k = wi === firstWord ? low % W : 0; k < (wi === lastWord ? (high - 1) % W + 1 : W); k++) {
          if (mask & (1n << BigInt(k))) return wi * W + k;
        }
      }
    }
    return null;
  }
  validate() {
    this.summary.forEach((word, si) => {
      for (let i = 0; i < W && si * W + i < this.words.length; i++) {
        assert.equal(Boolean(word & (1n << BigInt(i))), this.words[si * W + i] !== 0n);
      }
    });
  }
}
function oracle(present, cursor, slots) {
  for (let offset = 0; offset < slots; offset++) {
    const index = (cursor + offset) % slots;
    if (present.has(index)) return index;
  }
  return null;
}
function prng(seed) {
  let x = seed >>> 0;
  return () => {x ^= x << 13; x ^= x >>> 17; x ^= x << 5; return (x >>> 0);};
}

test('empty, all set, clearing, and every critical word/summary boundary', () => {
  const bits = new BitmapIndex();
  assert.equal(bits.first(0), null);
  const boundaries = [0, 1, 62, 63, 64, 65, 4094, 4095, 4096, 4097, 65534, 65535];
  const present = new Set();
  for (const i of boundaries) {bits.set(i, true); present.add(i);}
  bits.validate();
  for (const cursor of [...boundaries, 2, 66, 4098, 65533])
    assert.equal(bits.first(cursor), oracle(present, cursor, SLOT_LIMIT));
  for (const i of boundaries) {bits.set(i, false); present.delete(i); bits.validate();}
  for (const cursor of boundaries) assert.equal(bits.first(cursor), null);
});

test('deterministic 20000 mixed inserts/removals and round-robin lookups', () => {
  const bits = new BitmapIndex();
  const present = new Set();
  const rand = prng(0x9e3779b9);
  for (let i = 0; i < 20000; i++) {
    const slot = rand() % SLOT_LIMIT, add = (rand() & 1) === 1;
    bits.set(slot, add);
    if (add) present.add(slot); else present.delete(slot);
    if (i % 11 === 0) {
      const cursor = rand() % SLOT_LIMIT;
      assert.equal(bits.first(cursor), oracle(present, cursor, SLOT_LIMIT), `iteration ${i}`);
    }
    if (i % 151 === 0) bits.validate();
  }
  bits.validate();
});

test('CONTROL always precedes DATA and active actor cannot be selected twice', () => {
  const ctrl = new BitmapIndex(), data = new BitmapIndex();
  const busy = new Set();
  const schedule = (start) => {
    const slot = ctrl.first(start);
    if (slot !== null) {ctrl.set(slot, false); busy.add(slot); return [slot, 'CONTROL'];}
    const d = data.first(start);
    if (d !== null) {data.set(d, false); busy.add(d); return [d, 'DATA'];}
    return null;
  };
  for (const i of [0, 63, 64, 4095, 4096, 65535]) data.set(i, true);
  ctrl.set(65535, true);
  assert.deepEqual(schedule(0), [65535, 'CONTROL']);
  assert.equal(busy.has(65535), true);
  // Reconcile both indexes when a lane becomes physically active.
  data.set(65535, false);
  assert.notDeepEqual(schedule(0), [65535, 'DATA']);
  ctrl.set(4096, true);
  assert.deepEqual(schedule(1), [4096, 'CONTROL']);
});

test('invalid slots and cursors reject without aliasing neighboring bits', () => {
  const bits = new BitmapIndex();
  for (const slot of [-1, SLOT_LIMIT, SLOT_LIMIT + 1, 1.5, NaN, Infinity])
    assert.throws(() => bits.set(slot, true));
  for (const cursor of [-1, SLOT_LIMIT, 1.5, NaN])
    assert.throws(() => bits.first(cursor));
  bits.validate();
  assert.equal(bits.first(0), null);
});
