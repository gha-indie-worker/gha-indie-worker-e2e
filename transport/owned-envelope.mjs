/**
 * Experimental Oreslang owned message wire v1. Browser-compatible host codec.
 * This is NOT yet connected to compiler lowering, Wasm memory or actor lanes.
 *
 * Header: "ORM1" (4 bytes), version (u8), flags (u8),
 * variant ID (u16 LE), payload byte count (u32 LE), SHA256(contract) (32 bytes).
 * All values are schema-directed. All binary buffers are copied.
 */
const HEADER=44, LIMIT=1048576, DEPTH=12, ITEMS=4096;
const SCHEMA_NODES=4096, SCHEMA_BYTES=65536;
const magic=Uint8Array.of(79,82,77,49);
const enc=new TextEncoder(),dec=new TextDecoder("utf-8",{fatal:true});
const prim=new Set(["string","i64","bool","bytes"]);
const plain=x=>x!==null&&typeof x==="object"&&[Object.prototype,null].includes(Object.getPrototypeOf(x));
const fail=reason=>{throw new TypeError("Oreslang envelope: "+reason)};
function shape(x,keys,name){
 if(!plain(x)||Object.keys(x).some(k=>!keys.includes(k)))fail("invalid "+name+" shape");
}
function unique(a,label){if(new Set(a).size!==a.length)fail("duplicate "+label);}
function name(x){if(typeof x!=="string"||!/^[A-Za-z_][A-Za-z0-9_]{0,127}$/.test(x))fail("invalid name");return x;}
function id(x){if(!Number.isInteger(x)||x<1||x>65535)fail("invalid ID");return x;}
function bound(x){if(!Number.isInteger(x)||x<0||x>ITEMS)fail("invalid element bound");return x;}
function type(t,d=0,ctx){
 if(d>DEPTH)fail("type nesting too deep");
 if(++ctx.nodes>SCHEMA_NODES)fail("schema node budget exceeded");
 if(prim.has(t))return t;
 shape(t,["tuple","array","list","enum"],"type");
 const ks=Object.keys(t);if(ks.length!==1)fail("exactly one type required");
 const k=ks[0];
 if(k==="tuple"){
  if(!Array.isArray(t.tuple)||t.tuple.length>64)fail("invalid tuple");
  return {tuple:t.tuple.map(x=>type(x,d+1,ctx))};
 }
 if(k==="array"||k==="list"){
  shape(t[k],k==="array"?["element","length"]:["element","maxLength"],k);
  return {[k]:k==="array"?{element:type(t[k].element,d+1,ctx),length:bound(t[k].length)}
    :{element:type(t[k].element,d+1,ctx),maxLength:bound(t[k].maxLength)}};
 }
 if(!Array.isArray(t.enum)||t.enum.length<1||t.enum.length>256)fail("invalid enum");
 const vs=t.enum.map(v=>{
  shape(v,["id","name","payload"],"enum variant");
  if(!Object.hasOwn(v,"payload"))fail("enum payload declaration required");
  return {id:id(v.id),name:name(v.name),payload:v.payload===null?null:type(v.payload,d+1,ctx)};
 });
 unique(vs.map(v=>v.id),"enum ID");unique(vs.map(v=>v.name),"enum name");
 return {enum:vs.sort((a,b)=>a.id-b.id)};
}
function contract(c){
 shape(c,["schemaVersion","ownership","variants"],"contract");
 if(c.schemaVersion!==1||c.ownership!=="copy")fail("unsupported contract version/ownership");
 if(!Array.isArray(c.variants)||c.variants.length<1||c.variants.length>256)fail("invalid variants");
 const ctx={nodes:0};
 const vs=c.variants.map(v=>{
  shape(v,["id","name","fields"],"variant");
  if(!Array.isArray(v.fields)||v.fields.length>64)fail("invalid fields");
  const fields=v.fields.map(f=>{shape(f,["name","type"],"field");return{name:name(f.name),type:type(f.type,0,ctx)}});
  unique(fields.map(f=>f.name),"field");
  return {id:id(v.id),name:name(v.name),fields};
 });
 unique(vs.map(v=>v.id),"variant ID");unique(vs.map(v=>v.name),"variant name");
 const normalized={schemaVersion:1,ownership:"copy",variants:vs.sort((a,b)=>a.id-b.id)};
 // Reject large contracts before WebCrypto hashes attacker-sized JSON.
 if(enc.encode(JSON.stringify(normalized)).length>SCHEMA_BYTES)fail("schema byte budget exceeded");
 return normalized;
}
async function digest(c){
 if(!globalThis.crypto?.subtle)fail("WebCrypto SHA-256 required");
 return new Uint8Array(await crypto.subtle.digest("SHA-256",
  enc.encode("Oreslang owned-message contract v1\n"+JSON.stringify(c))));
}
function stringBytes(s){
 if(typeof s!=="string")fail("expected string");
 for(let i=0;i<s.length;i++){
  const n=s.charCodeAt(i);
  if(n>=0xd800&&n<=0xdbff){
   if(++i>=s.length||s.charCodeAt(i)<0xdc00||s.charCodeAt(i)>0xdfff)fail("unpaired surrogate");
  }else if(n>=0xdc00&&n<=0xdfff)fail("unpaired surrogate");
 }
 return enc.encode(s);
}
class Writer{
 constructor(limit){this.parts=[];this.size=0;this.limit=limit;this.visits=0;}
 visit(){if(++this.visits>ITEMS)fail("value node budget exceeded");}
 put(b){if(!(b instanceof Uint8Array)||b.length>this.limit-this.size)fail("byte budget exceeded");
  if(typeof SharedArrayBuffer!=="undefined"&&b.buffer instanceof SharedArrayBuffer)fail("shared memory not admitted");
  this.parts.push(Uint8Array.from(b));this.size+=b.length;}
 u8(n){this.put(Uint8Array.of(n));}
 u16(n){const b=new Uint8Array(2);new DataView(b.buffer).setUint16(0,n,true);this.put(b);}
 u32(n){const b=new Uint8Array(4);new DataView(b.buffer).setUint32(0,n,true);this.put(b);}
 i64(n){if(typeof n!=="bigint"||n<-(1n<<63n)||n>(1n<<63n)-1n)fail("signed i64 BigInt required");
  const b=new Uint8Array(8);new DataView(b.buffer).setBigInt64(0,n,true);this.put(b);}
 finish(){const b=new Uint8Array(this.size);let p=0;for(const x of this.parts){b.set(x,p);p+=x.length;}return b;}
}
class Reader{
 constructor(b){this.b=b;this.at=0;this.dv=new DataView(b.buffer,b.byteOffset,b.byteLength);this.visits=0;}
 visit(){if(++this.visits>ITEMS)fail("value node budget exceeded");}
 take(n){if(!Number.isSafeInteger(n)||n<0||n>this.b.length-this.at)fail("truncated payload");
  const x=this.b.subarray(this.at,this.at+n);this.at+=n;return x;}
 u8(){this.take(1);return this.dv.getUint8(this.at-1);}
 u16(){this.take(2);return this.dv.getUint16(this.at-2,true);}
 u32(){this.take(4);return this.dv.getUint32(this.at-4,true);}
 i64(){this.take(8);return this.dv.getBigInt64(this.at-8,true);}
}
function encode(w,t,x,d=0){
 if(d>DEPTH)fail("value nesting too deep");
 w.visit();
 if(t==="i64")return w.i64(x);
 if(t==="bool"){if(typeof x!=="boolean")fail("expected bool");return w.u8(x?1:0);}
 if(t==="string"||t==="bytes"){
  const b=t==="string"?stringBytes(x):x;
  if(!(b instanceof Uint8Array))fail("expected Uint8Array");
  w.u32(b.length);return w.put(b);
 }
 if(Object.hasOwn(t,"tuple")){
  if(!Array.isArray(x)||x.length!==t.tuple.length)fail("tuple arity mismatch");
  t.tuple.forEach((a,i)=>encode(w,a,x[i],d+1));return;
 }
 if(Object.hasOwn(t,"array")||Object.hasOwn(t,"list")){
  const fixed=Object.hasOwn(t,"array"),spec=fixed?t.array:t.list;
  if(!Array.isArray(x)||(fixed?x.length!==spec.length:x.length>spec.maxLength))fail("array/list length");
  if(!fixed)w.u32(x.length);
  // Array.forEach skips sparse holes. With a zero-byte element type this
  // would silently encode fewer values than the declared length, defeating
  // node budgets and canonical typed-message round trips.
  for(let i=0;i<x.length;i++){
   if(!Object.hasOwn(x,i))fail("sparse array/list element");
   encode(w,spec.element,x[i],d+1);
  }
  return;
 }
 shape(x,["variant","value"],"enum value");
 const v=t.enum.find(a=>a.name===x.variant);if(!v)fail("unknown enum alternative");
 w.u16(v.id);
 if(v.payload===null){if(x.value!==null)fail("unit enum must be null");}
 else encode(w,v.payload,x.value,d+1);
}
function decode(r,t,d=0){
 if(d>DEPTH)fail("value nesting too deep");
 r.visit();
 if(t==="i64")return r.i64();
 if(t==="bool"){const x=r.u8();if(x>1)fail("noncanonical bool");return !!x;}
 if(t==="string"||t==="bytes"){const b=r.take(r.u32());
  if(t==="bytes")return Uint8Array.from(b);
  try{return dec.decode(b);}catch{fail("invalid UTF-8");}}
 if(Object.hasOwn(t,"tuple"))return Object.freeze(t.tuple.map(a=>decode(r,a,d+1)));
 if(Object.hasOwn(t,"array")||Object.hasOwn(t,"list")){
  const fixed=Object.hasOwn(t,"array"),spec=fixed?t.array:t.list,n=fixed?spec.length:r.u32();
  if(n>ITEMS||(!fixed&&n>spec.maxLength))fail("array/list length");
  const a=[];for(let i=0;i<n;i++)a.push(decode(r,spec.element,d+1));return Object.freeze(a);
 }
 const variantId=r.u16();const v=t.enum.find(a=>a.id===variantId);if(!v)fail("unknown enum ID");
 return Object.freeze({variant:v.name,value:v.payload===null?null:decode(r,v.payload,d+1)});
}
function snapshot(input){
 if(input instanceof ArrayBuffer)return Uint8Array.from(new Uint8Array(input));
 if(ArrayBuffer.isView(input)){
  if(typeof SharedArrayBuffer!=="undefined"&&input.buffer instanceof SharedArrayBuffer)fail("shared memory not admitted");
  return Uint8Array.from(new Uint8Array(input.buffer,input.byteOffset,input.byteLength));
 }
 fail("expected message bytes");
}
function budget(n){if(!Number.isSafeInteger(n)||n<HEADER||n>LIMIT)fail("invalid maxBytes");}
export async function encodeMessage(schema,message,{maxBytes=LIMIT}={}){
 budget(maxBytes);
 const c=contract(schema);
 shape(message,["variant","fields"],"message");
 const v=c.variants.find(x=>x.name===message.variant);if(!v)fail("unknown message variant");
 shape(message.fields,v.fields.map(x=>x.name),"message fields");
 if(Object.keys(message.fields).length!==v.fields.length)fail("missing message field");
 const w=new Writer(maxBytes-HEADER);for(const f of v.fields)encode(w,f.type,message.fields[f.name]);
 // Payload bytes and normalized schema are snapshotted before any await.
 const payload=w.finish(),hash=await digest(c);
 const result=new Uint8Array(HEADER+payload.length),dv=new DataView(result.buffer);
 result.set(magic);dv.setUint8(4,1);dv.setUint8(5,0);dv.setUint16(6,v.id,true);
 dv.setUint32(8,payload.length,true);result.set(hash,12);result.set(payload,HEADER);return result;
}
export async function decodeMessage(schema,input,{maxBytes=LIMIT}={}){
 budget(maxBytes);const c=contract(schema),bytes=snapshot(input);
 if(bytes.length<HEADER||bytes.length>maxBytes)fail("invalid message size");
 const r=new Reader(bytes);for(const m of magic)if(r.u8()!==m)fail("bad magic");
 if(r.u8()!==1||r.u8()!==0)fail("unknown version/flags");
 const variantId=r.u16(),n=r.u32(),received=r.take(32);
 if(n!==bytes.length-HEADER)fail("payload length mismatch");
 const hash=await digest(c);
 if(!hash.every((x,i)=>x===received[i]))fail("contract SHA-256 mismatch");
 const v=c.variants.find(x=>x.id===variantId);if(!v)fail("unknown message variant");
 const fields={};for(const f of v.fields){
  const value=decode(r,f.type);
  // Define an OWN property, including for "__proto__", without calling
  // Object.prototype setters or replacing the resulting object prototype.
  Object.defineProperty(fields,f.name,{value,enumerable:true,writable:false,configurable:false});
 }
 if(r.at!==bytes.length)fail("unexpected trailing payload");
 return Object.freeze({variant:v.name,fields:Object.freeze(fields)});
}
export const OWNED_ENVELOPE_V1=Object.freeze({version:1,headerBytes:HEADER,maxBytes:LIMIT,ownership:"copy"});
