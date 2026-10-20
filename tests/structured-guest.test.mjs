import test from "node:test";
import assert from "node:assert/strict";
import {readFile,mkdtemp,rm} from "node:fs/promises";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {fileURLToPath} from "node:url";
import {compileOresToWasm} from "../scripts/compile-ores.mjs";
const compiler=process.env.ORESLANG_LLVM_COMPILER;
const examples=fileURLToPath(new URL("../examples/",import.meta.url));
const scenarios=["structured-mutable-borrow.ores","structured-shared-borrow.ores"];
test("actual bounded-class Oreslang source compiles to verified Wasm32",{
 skip:!compiler
},async()=>{
 const dir=await mkdtemp(join(tmpdir(),"oreslang-structured-wasm-"));
 try{
  for(const file of scenarios){
   const source=join(examples,file),output=join(dir,file+".wasm");
   const record=await compileOresToWasm({compiler,source,output,environment:"browser",
    compilerSha256:process.env.ORESLANG_LLVM_COMPILER_SHA256});
   const wasm=await readFile(output);
   const manifest=JSON.parse(await readFile(output+".artifact.json","utf8"));
   assert.equal(record.importCount,0);
   assert.equal(manifest.wasmSha256,record.wasmSha256);
   assert.equal(manifest.memory.maximumPages,2);
   assert.deepEqual(manifest.requiredCapabilities,[]);
   assert.equal(new WebAssembly.Instance(new WebAssembly.Module(wasm)).exports.main(),42n);
  }
 }finally{await rm(dir,{recursive:true,force:true})}
});
test("invalid overlapping mutable/shared class loans cannot publish Wasm",{
 skip:!compiler
},async()=>{
 const dir=await mkdtemp(join(tmpdir(),"oreslang-structured-denied-"));
 try{
  const output=join(dir,"invalid.wasm");
  await assert.rejects(compileOresToWasm({
   compiler,source:join(examples,"structured-overlap-reject.ores"),
   output,environment:"browser"
  }),/command failed/);
  await assert.rejects(readFile(output),/ENOENT/);
 }finally{await rm(dir,{recursive:true,force:true})}
});
