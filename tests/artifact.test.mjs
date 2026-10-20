import test from "node:test";
import assert from "node:assert/strict";
import {mkdtemp,writeFile,chmod,readFile,rm} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {createHash} from "node:crypto";
import {compileOresToWasm} from "../scripts/compile-ores.mjs";
const sha=b=>createHash("sha256").update(b).digest("hex");
test("wasm32 target-marked compiler IR produces pinned browser artifact",async()=>{
 const dir=await mkdtemp(join(tmpdir(),"ores-wasm-manifest-"));
 try{
  const compiler=join(dir,"compiler.sh"),source=join(dir,"main.ores"),output=join(dir,"guest.wasm");
  // Simulates compiler stdout ONLY, not evidence of native Oreslang compilation.
  await writeFile(compiler,"#!/bin/sh\nprintf 'target triple = \"wasm32-unknown-unknown\"\\ndefine i64 @main() { ret i64 42 }\\n'\n",{mode:0o700});
  await chmod(compiler,0o700);
  await writeFile(source,"pub fnc main(): int { return 42; }\n");
  const r=await compileOresToWasm({compiler,source,output,environment:"browser"});
  const artifact=JSON.parse(await readFile(output+".artifact.json","utf8"));
  const wasm=await readFile(output);
  assert.equal(artifact.artifactType,"oreslang.core-wasm.i64-v1");
  assert.equal(artifact.environment,"browser");
  assert.equal(artifact.target,"wasm32");
  assert.equal(artifact.wasmSha256,sha(wasm));
  assert.equal(artifact.wasmBytes,wasm.length);
  assert.deepEqual(artifact.requiredCapabilities,[]);
  assert.deepEqual(artifact.imports,[]);
  assert.equal(artifact.memory.maximumPages,2);
  assert.equal(artifact.producer.compilerSha256,r.compilerSha256);
  assert.equal(new WebAssembly.Instance(new WebAssembly.Module(wasm)).exports.main(),42n);
  await assert.rejects(compileOresToWasm({compiler,source,output:join(dir,"bad.wasm"),environment:"ios"}),/unsupported portable/);
 }finally{await rm(dir,{recursive:true,force:true})}
});
test("non-wasm32-marked compiler output cannot publish portable artifact",async()=>{
 const dir=await mkdtemp(join(tmpdir(),"ores-bad-triple-"));
 try{
  const compiler=join(dir,"compiler.sh"),source=join(dir,"main.ores"),output=join(dir,"not-published.wasm");
  await writeFile(compiler,"#!/bin/sh\nprintf 'define i64 @main() { ret i64 42 }\\n'\n",{mode:0o700});
  await chmod(compiler,0o700);await writeFile(source,"pub fnc main(): int { return 42; }\n");
  await assert.rejects(compileOresToWasm({compiler,source,output,environment:"browser"}),/target-marked IR/);
  await assert.rejects(readFile(output),/ENOENT/);
 }finally{await rm(dir,{recursive:true,force:true})}
});
