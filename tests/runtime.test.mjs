import test from "node:test";
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
const binary=await readFile(new URL("../build/oreslang_rt.wasm",import.meta.url));
const module=new WebAssembly.Module(binary);
function load(clock=()=>1234){
 const instance=new WebAssembly.Instance(module,{ores_host:{clock_ms:clock}});
 return instance.exports;
}
test("emits real, import-checked wasm32 binary with exported memory",()=>{
 assert.equal(WebAssembly.validate(binary),true);
 assert.deepEqual(WebAssembly.Module.imports(module).map(x=>x.module+"."+x.name),["ores_host.clock_ms"]);
 const e=load();
 assert.ok(e.memory instanceof WebAssembly.Memory);
 assert.equal(e.memory.buffer.byteLength,131072);
 assert.throws(()=>e.memory.grow(1),RangeError);
 assert.equal(e.ores_rt_abi_version(),1);
 assert.equal(e.ores_rt_add(40,2),42);
});
test("bounded queue admits exactly 8 items, then fails closed",()=>{
 const e=load();
 assert.equal(e.ores_rt_initialize(0),1);
 for(let i=0;i<8;i++)assert.equal(e.ores_rt_enqueue(i+20),1);
 assert.equal(e.ores_rt_enqueue(99),0);
 for(let i=0;i<8;i++){assert.equal(e.ores_rt_dequeue(),1);assert.equal(e.ores_rt_last_message(),i+20)}
 assert.equal(e.ores_rt_dequeue(),0);
});
test("clock import only executes with explicit runtime bit",()=>{
 let calls=0;
 const e=load(()=>{calls++;return 777});
 assert.equal(e.ores_rt_initialize(0),1);
 assert.equal(e.ores_rt_clock_ms(),-1);
 assert.equal(calls,0);
 assert.equal(e.ores_rt_initialize(1),1);
 assert.equal(e.ores_rt_clock_ms(),777);
 assert.equal(calls,1);
 assert.equal(e.ores_rt_initialize(2),0);
 assert.equal(e.ores_rt_clock_ms(),-1);
 assert.equal(e.ores_rt_enqueue(123),0);
 assert.equal(e.ores_rt_initialize(1),1);
 assert.equal(e.ores_rt_clock_ms(),777);
});
test("initialization resets bounded queue state",()=>{
 const e=load();
 e.ores_rt_initialize(0);
 e.ores_rt_enqueue(32);
 e.ores_rt_initialize(0);
 assert.equal(e.ores_rt_dequeue(),0);
});

test("no mailbox admission or host call before first initialization",()=>{
 let calls=0;
 const e=load(()=>{calls++;return 9});
 assert.equal(e.ores_rt_enqueue(11),0);
 assert.equal(e.ores_rt_dequeue(),0);
 assert.equal(e.ores_rt_clock_ms(),-1);
 assert.equal(calls,0);
});
test("failed reinitialization invalidates previous message state and permission",()=>{
 const e=load(()=>21);
 assert.equal(e.ores_rt_initialize(1),1);
 assert.equal(e.ores_rt_enqueue(123),1);
 assert.equal(e.ores_rt_initialize(0xffffffff),0);
 assert.equal(e.ores_rt_dequeue(),0);
 assert.equal(e.ores_rt_clock_ms(),-1);
 assert.equal(e.ores_rt_initialize(1),1);
 assert.equal(e.ores_rt_dequeue(),0);
});
test("mailbox FIFO wraps around and full queue preserves existing items",()=>{
 const e=load();
 assert.equal(e.ores_rt_initialize(0),1);
 for(let i=0;i<8;i++)assert.equal(e.ores_rt_enqueue(i),1);
 for(let i=0;i<4;i++){assert.equal(e.ores_rt_dequeue(),1);assert.equal(e.ores_rt_last_message(),i)}
 for(let i=8;i<12;i++)assert.equal(e.ores_rt_enqueue(i),1);
 assert.equal(e.ores_rt_enqueue(3000),0);
 for(let i=4;i<12;i++){assert.equal(e.ores_rt_dequeue(),1);assert.equal(e.ores_rt_last_message(),i)}
});
