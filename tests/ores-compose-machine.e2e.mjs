import assert from 'node:assert/strict';
import test from 'node:test';

const CONTRACT_SHA = '5dbda2127357b4be87821902d36e4ce9560f6876';
const BASE = `https://raw.githubusercontent.com/ORESoftware/ores-interfaces/${CONTRACT_SHA}/contracts/ores-compose-machine/v1`;

async function read(path) {
  const response = await fetch(`${BASE}/${path}`);
  assert.equal(response.status, 200, `failed to fetch ${path}: ${response.status}`);
  return response.text();
}

const [schemaText, typeSpec] = await Promise.all([
  read('authored.schema.json'),
  read('main.tsp'),
]);
const schema = JSON.parse(schemaText);
const defs = schema.$defs;

test('ensure admission is typed and cannot smuggle process controls', () => {
  const ensure = defs.EnsureRequest;
  assert.deepEqual(ensure.required, ['schema_version', 'project', 'session', 'service', 'revision', 'rebuild']);
  assert.equal(ensure.additionalProperties, false);
  for (const forbidden of ['command', 'argv', 'cwd', 'shell', 'port', 'backend']) {
    assert.equal(ensure.properties[forbidden], undefined, forbidden);
  }
});

test('job identity and switch generation are decimal strings for cross-runtime safety', () => {
  assert.equal(defs.EnqueueResponse.properties.job_id.type, 'string');
  assert.equal(defs.EnqueueResponse.properties.job_id.pattern, '^[1-9][0-9]{0,19}$');
  assert.equal(defs.ActiveSystem.properties.generation.type, 'string');
  assert.equal(defs.ActiveSystem.properties.generation.pattern, '^[1-9][0-9]{0,19}$');
});

test('serialized machine states and fencing errors remain explicit', () => {
  const states = defs.JobStatusResponse.properties.state.anyOf.map((x) => x.const);
  assert.deepEqual(states, ['queued', 'running', 'ready', 'failed']);
  const codes = defs.MachineErrorResponse.properties.code.anyOf.map((x) => x.const);
  for (const required of ['queue_full', 'machine_busy', 'stale_generation', 'activation_failed']) {
    assert.ok(codes.includes(required), required);
  }
});

test('public machine ingress rejects internet-routable backend authorities', () => {
  const pattern = new RegExp(defs.MachineIngress.properties.authority.pattern);
  assert.ok(pattern.test('127.0.0.1:39123'));
  assert.ok(pattern.test('[::1]:39123'));
  assert.ok(pattern.test('/tmp/ores-compose/control.sock'));
  assert.equal(pattern.test('8.8.8.8:53'), false);
  assert.equal(pattern.test('10.0.0.8:8080'), false, 'private replica addresses are not public machine ingress');
});

test('TypeSpec and JSON Schema remain peer authorities with snake_case wire fields', () => {
  for (const model of ['EnsureRequest', 'EnqueueResponse', 'MachineIngress', 'ActiveSystem', 'JobStatusResponse', 'ReadinessResponse', 'MachineErrorResponse']) {
    assert.match(typeSpec, new RegExp(`model\\s+${model}\\b`), model);
    assert.ok(defs[model], model);
  }
  for (const field of ['schema_version', 'job_id']) {
    assert.match(typeSpec, new RegExp(`\\b${field}\\b`));
  }
  assert.doesNotMatch(typeSpec, /schemaVersion|jobId|machineBusy/);
});
