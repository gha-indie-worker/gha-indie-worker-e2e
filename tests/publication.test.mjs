import test from "node:test";
import assert from "node:assert/strict";
import {mkdtemp,mkdir,readFile,writeFile,chmod,rm,readdir} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {compileOresToWasm} from "../scripts/compile-ores.mjs";

// A compiler-stdout stand-in, NOT evidence of native Oreslang frontend execution.
async function fixture(dir){
 const compiler=join(dir,"compiler.sh"),source=join(dir,"source.ores");
 await writeFile(compiler,"#!/bin/sh\\nprintf 'target triple = \\\"wasm32-unknown-unknown\\\"\\\\ndefine i64 @main() { ret i64 42 }\\\\n'\\n",{mode:0o700});
 await chmod(compiler,0o700);
 await writeFile(source,"pub fnc main(): int { return 42; }\\n");
 return {compiler,source};
}
test("failed artifact sidecar publication cannot expose new Wasm",async()=>{
 const dir=await mkdtemp(join(tmpdir(),"ores-wasm-publish-"));
 try{
  const {compiler,source}=await fixture(dir);
  const output=join(dir,"guest.wasm");
  await mkdir(output+".artifact.json"); // directory collides with promised metadata path
  await assert.rejects(compileOresToWasm({compiler,source,output,environment:"browser"}));
  await assert.rejects(readFile(output),/ENOENT/);
  await assert.rejects(readFile(output+".provenance.json"),/ENOENT/);
  assert.equal((await readdir(dir)).some(x=>x.startsWith(".oreslang-wasm-")),false);
 }finally{await rm(dir,{recursive:true,force:true})}
});
test("failed provenance sidecar publication cannot expose new Wasm",async()=>{
 const dir=await mkdtemp(join(tmpdir(),"ores-wasm-publish-"));
 try{
  const {compiler,source}=await fixture(dir),output=join(dir,"guest.wasm");
  await mkdir(output+".provenance.json");
  await assert.rejects(compileOresToWasm({compiler,source,output}));
  await assert.rejects(readFile(output),/ENOENT/);
 }finally{await rm(dir,{recursive:true,force:true})}
});
