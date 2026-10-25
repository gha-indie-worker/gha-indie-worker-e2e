// Synthetic-only policy test: no private Oreslang source or metadata.
const gates = [
  "sourceProtected",
  "incidentsCompleted",
  "destinationPrivate",
  "destinationProtected",
  "independentReview",
  "identityPinned",
  "retentionApproved",
];

function authorize(state) {
  const missing = gates.filter((k) => state[k] !== true);
  if (missing.length) throw new Error("DENIED:" + missing.join(","));
  return true;
}

let accepted = 0;
let denied = 0;
for (let mask = 0; mask < (1 << gates.length); mask++) {
  const state = Object.fromEntries(
    gates.map((k, i) => [k, Boolean(mask & (1 << i))])
  );
  const expected = gates.every((k) => state[k] === true);
  let actual = false;
  try { actual = authorize(state); } catch {}
  if (actual !== expected) throw new Error("truth-table mismatch " + mask);
  accepted += actual ? 1 : 0;
  denied += actual ? 0 : 1;
}
if (accepted !== 1 || denied !== 127) throw new Error("bad totals");

const allOk = Object.fromEntries(gates.map((k) => [k, true]));
authorize(allOk);

for (const revoked of [
  "sourceProtected",
  "incidentsCompleted",
  "destinationPrivate",
  "destinationProtected",
  "independentReview",
  "identityPinned",
  "retentionApproved",
]) {
  const state = { ...allOk, [revoked]: false };
  let stopped = false;
  try { authorize(state); } catch { stopped = true; }
  if (!stopped) throw new Error("revocation not enforced: " + revoked);
}

console.log(
  "ORESLANG_SOURCE_FREE_GATE_PASS accepted=1 denied=127 private_source_bytes=0"
);
