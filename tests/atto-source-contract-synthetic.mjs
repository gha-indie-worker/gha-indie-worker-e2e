import assert from "node:assert/strict";
import {test} from "node:test";
import {mkdtemp, mkdir, writeFile, symlink, lstat, rm} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join, extname} from "node:path";

async function locate(spec) {
  const suffix = extname(spec).toLowerCase();
  if (suffix !== "") {
    assert.ok(suffix === ".atto" || suffix === ".ores", "foreign suffix");
    return spec;
  }
  const atto = spec + ".atto";
  const legacy = spec + ".ores";
  try {
    await lstat(atto);
    return atto;
  } catch (error) {
    if (error.code !== "ENOENT") throw error;
  }
  try {
    await lstat(legacy);
    return legacy;
  } catch (error) {
    if (error.code !== "ENOENT") throw error;
  }
  return atto;
}

async function makeRoot(t) {
  const root = await mkdtemp(join(tmpdir(), "atto-suffix-"));
  t.after(() => rm(root, {recursive: true, force: true}));
  return root;
}

test("AttoLang source suffix contract: canonical, legacy, explicit and invalid paths", async t => {
  const root = await makeRoot(t);
  await writeFile(join(root, "both.atto"), "new");
  await writeFile(join(root, "both.ores"), "old");
  assert.equal(await locate(join(root, "both")), join(root, "both.atto"));
  assert.equal(await locate(join(root, "both.ores")), join(root, "both.ores"));
  assert.equal(await locate(join(root, "missing")), join(root, "missing.atto"));
  await assert.rejects(async () => locate(join(root, "foreign.py")));
});

test("invalid canonical entries cannot reveal a legacy sibling", async t => {
  const root = await makeRoot(t);
  await mkdir(join(root, "directory.atto"));
  await writeFile(join(root, "directory.ores"), "old");
  assert.equal(await locate(join(root, "directory")), join(root, "directory.atto"));

  await symlink(join(root, "no-such-file"), join(root, "dangling.atto"));
  await writeFile(join(root, "dangling.ores"), "old");
  assert.equal(await locate(join(root, "dangling")), join(root, "dangling.atto"));
  assert.equal((await lstat(join(root, "dangling.atto"))).isSymbolicLink(), true);
});
