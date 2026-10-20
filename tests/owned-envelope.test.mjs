import test from "node:test";
import assert from "node:assert/strict";
import {encodeMessage,decodeMessage,OWNED_ENVELOPE_V1} from "../transport/owned-envelope.mjs";
const contract={schemaVersion:1,ownership:"copy",variants:[
 {id:2,name:"Result",fields:[{name:"text",type:"string"},{name:"next",type:"i64"}]},
 {id:1,name:"Compute",fields:[{name:"text",type:"string"},{name:"n",type:"i64"}]},
 {id:3,name:"Batch",fields:[
  {name:"pair",type:{tuple:["string","i64"]}},
  {name:"fixed",type:{array:{element:"bool",length:2}}},
  {name:"items",type:{list:{element:"string",maxLength:3}}},
  {name:"state",type:{enum:[
   {id:2,name:"Some",payload:{tuple:["i64","string"]}},
   {id:1,name:"None",payload:null}]}},
  {name:"blob",type:"bytes"}]}
]};
test("Compute/Result preserve full-width signed i64 values through binary wire",async()=>{
 for(const m of [
  {variant:"Compute",fields:{text:"hello 🌎",n:41n}},
  {variant:"Result",fields:{text:"hello",next:42n}},
  {variant:"Result",fields:{text:"negative",next:-(1n<<63n)}},
  {variant:"Result",fields:{text:"large",next:(1n<<63n)-1n}}
 ]){
  const bytes=await encodeMessage(contract,m);
  assert.deepEqual([...bytes.subarray(0,4)],[79,82,77,49]);
  assert.deepEqual(await decodeMessage(contract,bytes),m);
 }
 assert.equal(OWNED_ENVELOPE_V1.ownership,"copy");
});
test("tuple, fixed array, list, nested enum and owned binary are typed",async()=>{
 const fields={pair:["hi",-7n],fixed:[true,false],items:["a","b"],
  state:{variant:"Some",value:[42n,"yes"]},blob:Uint8Array.of(1,2,3)};
 const bytes=await encodeMessage(contract,{variant:"Batch",fields});
 fields.blob[0]=255;
 const result=await decodeMessage(contract,bytes);
 assert.equal(result.fields.blob[0],1);
 assert.deepEqual(result.fields.state,{variant:"Some",value:[42n,"yes"]});
 assert.equal(Object.isFrozen(result.fields.pair),true);
 const noPayload=await encodeMessage(contract,{variant:"Batch",fields:{...fields,state:{variant:"None",value:null}}});
 assert.equal((await decodeMessage(contract,noPayload)).fields.state.variant,"None");
});
test("contract fingerprint is deterministic for reordered variant declarations",async()=>{
 const m={variant:"Compute",fields:{text:"x",n:7n}};
 const a=await encodeMessage(contract,m),b=await encodeMessage({...contract,variants:[...contract.variants].reverse()},m);
 assert.deepEqual(a,b);
});
test("schema agreement uses exact SHA-256, never just a numeric variant tag",async()=>{
 const bytes=await encodeMessage(contract,{variant:"Compute",fields:{text:"x",n:41n}});
 const changed={...contract,variants:contract.variants.map(v=>v.name==="Compute"
  ?{...v,fields:[{name:"n",type:"i64"},{name:"text",type:"string"}]}:v)};
 await assert.rejects(decodeMessage(changed,bytes),/contract SHA-256 mismatch/);
 const corrupted=bytes.slice();corrupted[12]^=1;
 await assert.rejects(decodeMessage(contract,corrupted),/contract SHA-256 mismatch/);
});
test("invalid variants, i64 range, schema smuggling, lengths and enum payloads fail closed",async()=>{
 for(const m of [
  {variant:"Unknown",fields:{}},
  {variant:"Compute",fields:{text:"x",n:41}},
  {variant:"Compute",fields:{text:"x",n:1n<<63n}},
  {variant:"Compute",fields:{text:"x",n:42n,extra:1}},
  {variant:"Compute",fields:{text:"x"}},
  {variant:"Compute",fields:{text:"\ud800",n:42n}}
 ])await assert.rejects(encodeMessage(contract,m));
 const fields={pair:["hi",42n],fixed:[true,false],items:["a","b"],
  state:{variant:"None",value:null},blob:Uint8Array.of(1)};
 for(const bad of [
  {...fields,fixed:[true]},
  {...fields,items:["a","b","c","d"]},
  {...fields,state:{variant:"Wrong",value:null}},
  {...fields,state:{variant:"None",value:4}}
 ])await assert.rejects(encodeMessage(contract,{variant:"Batch",fields:bad}));
 for(const revised of [
  {...contract,ownership:"borrow"},
  {...contract,variants:[contract.variants[0],contract.variants[0]]},
  {...contract,variants:[{id:9,name:"Bad",fields:[{name:"x",type:"evil"}]}]},
  {...contract,variants:[{id:9,name:"Bad",fields:[{name:"x",type:{list:{element:"i64",maxLength:5000}}}]}]}
 ])await assert.rejects(encodeMessage(revised,{variant:"Compute",fields:{text:"x",n:41n}}));
});
test("header tampering, truncation and extraneous payload are rejected",async()=>{
 const bytes=await encodeMessage(contract,{variant:"Compute",fields:{text:"x",n:42n}});
 for(const bad of [bytes.subarray(0,bytes.length-1),Uint8Array.from([...bytes,0]),
  Uint8Array.of(0,1,2)])await assert.rejects(decodeMessage(contract,bad));
 for(const at of [0,4,5,6]){const b=bytes.slice();b[at]^=255;await assert.rejects(decodeMessage(contract,b));}
});
test("untrusted input is snapshotted before async hashing",async()=>{
 const bytes=await encodeMessage(contract,{variant:"Compute",fields:{text:"safe",n:41n}});
 const pending=decodeMessage(contract,bytes);
 bytes.fill(0);
 assert.equal((await pending).fields.text,"safe");
});
test("bounded messages and explicitly nonshared ownership",async()=>{
 const msg={variant:"Compute",fields:{text:"x".repeat(500),n:42n}};
 await assert.rejects(encodeMessage(contract,msg,{maxBytes:80}),/budget/);
 const bytes=await encodeMessage(contract,msg);
 await assert.rejects(decodeMessage(contract,bytes,{maxBytes:80}),/size/);
 if(typeof SharedArrayBuffer!=="undefined"){
  const shared=new Uint8Array(new SharedArrayBuffer(bytes.length));shared.set(bytes);
  await assert.rejects(decodeMessage(contract,shared),/shared memory not admitted/);
  const fields={pair:["hi",1n],fixed:[true,false],items:[],state:{variant:"None",value:null},
   blob:new Uint8Array(new SharedArrayBuffer(4))};
  await assert.rejects(encodeMessage(contract,{variant:"Batch",fields}),/shared memory not admitted/);
 }
});

test("decoder preserves __proto__ as ordinary field without mutating its prototype",async()=>{
 const schema={schemaVersion:1,ownership:"copy",variants:[{id:1,name:"Proto",fields:[
  {name:"__proto__",type:{enum:[{id:1,name:"Some",payload:"string"}]}}
 ]}]};
 const fields=Object.create(null);
 fields.__proto__={variant:"Some",value:"not a prototype"};
 const bytes=await encodeMessage(schema,{variant:"Proto",fields});
 const result=await decodeMessage(schema,bytes);
 assert.equal(Object.getPrototypeOf(result.fields),Object.prototype);
 assert.equal(Object.hasOwn(result.fields,"__proto__"),true);
 assert.deepEqual(Object.getOwnPropertyDescriptor(result.fields,"__proto__").value,
  {variant:"Some",value:"not a prototype"});
 assert.equal(Object.isFrozen(result.fields),true);
});
test("global value node budget prevents CPU amplification by zero-byte nested values",async()=>{
 const schema={schemaVersion:1,ownership:"copy",variants:[{id:1,name:"Zero",fields:[
  {name:"values",type:{list:{element:{tuple:[]},maxLength:4096}}}
 ]}]};
 const many=Array.from({length:4095},()=>[]);
 const valid=await encodeMessage(schema,{variant:"Zero",fields:{values:many}});
 assert.equal((await decodeMessage(schema,valid)).fields.values.length,4095);
 await assert.rejects(
  encodeMessage(schema,{variant:"Zero",fields:{values:[...many,[]]}}),
  /value node budget exceeded/);
 const forged=valid.slice();
 // A list count is the first payload u32; raise it from 4095 to 4096.
 forged[44]=0;forged[45]=16;
 await assert.rejects(decodeMessage(schema,forged),/value node budget exceeded/);
});


test("global schema-node budget prevents adversarial type-descriptor amplification",async()=>{
 const variants=Array.from({length:100},(_,i)=>({
  id:i+1,name:"V"+i,fields:Array.from({length:50},(_,j)=>({name:"f"+j,type:"i64"}))
 }));
 const schema={schemaVersion:1,ownership:"copy",variants};
 await assert.rejects(encodeMessage(schema,{variant:"V0",fields:{}}),/schema node budget exceeded/);
 await assert.rejects(decodeMessage(schema,Uint8Array.of(0)),/schema node budget exceeded/);
});
test("canonical schema-byte budget rejects enormous valid-looking metadata",async()=>{
 const variants=Array.from({length:80},(_,i)=>({
  id:i+1,name:"V"+i,
  fields:Array.from({length:30},(_,j)=>({
   name:"f"+j+"_"+("a".repeat(118)),type:"i64"
  }))
 }));
 const schema={schemaVersion:1,ownership:"copy",variants};
 await assert.rejects(encodeMessage(schema,{variant:"V0",fields:{}}),/schema byte budget exceeded/);
 await assert.rejects(decodeMessage(schema,Uint8Array.of(0)),/schema byte budget exceeded/);
});


test("sparse arrays and lists cannot omit schema-declared zero-byte values",async()=>{
 const schema={schemaVersion:1,ownership:"copy",variants:[
  {id:1,name:"List",fields:[{name:"items",type:{list:{
   element:{tuple:[]},maxLength:5
  }}}]},
  {id:2,name:"Fixed",fields:[{name:"items",type:{array:{
   element:{tuple:[]},length:2
  }}}]}
 ]};
 for(const variant of ["List","Fixed"]){
  const holes=new Array(2);
  const partial=[[],[]];delete partial[1];
  for(const value of [holes,partial]){
   await assert.rejects(encodeMessage(schema,{
    variant,fields:{items:value}
   }),/sparse array\/list element/);
  }
  const dense={variant,fields:{items:[[],[]]}};
  const encoded=await encodeMessage(schema,dense);
  assert.deepEqual(await decodeMessage(schema,encoded),dense);
 }
});
test("a sparse list cannot bypass the global zero-byte value visit budget",async()=>{
 const schema={schemaVersion:1,ownership:"copy",variants:[{
  id:1,name:"EmptyTuples",fields:[{name:"items",
   type:{list:{element:{tuple:[]},maxLength:4096}}}]
 }]};
 const sparse=new Array(4096);sparse[0]=[];
 await assert.rejects(encodeMessage(schema,{
  variant:"EmptyTuples",fields:{items:sparse}
 }),/sparse array\/list element/);
});
