import test from "node:test";
import assert from "node:assert/strict";
import {createSerialOwnedActor} from "../transport/serial-actor-reference.mjs";
import {encodeMessage,decodeMessage} from "../transport/owned-envelope.mjs";

const schema={schemaVersion:1,ownership:"copy",variants:[
 {id:1,name:"Compute",fields:[{name:"text",type:"string"},{name:"n",type:"i64"}]},
 {id:2,name:"Result",fields:[{name:"text",type:"string"},{name:"n",type:"i64"}]}
]};
const compute=(text,n)=>({variant:"Compute",fields:{text,n:BigInt(n)}});
const result=(text,n)=>({variant:"Result",fields:{text,n:BigInt(n)}});
const deferred=()=>{let resolve;const promise=new Promise(r=>resolve=r);return{promise,resolve}};
const process=async(mail,self)=>{
 assert.equal(mail.variant,"Compute");
 await self.respond(result(mail.fields.text,mail.fields.n+1n));
 self.done();
 return result("ignored",999);
};

test("a value returned from receive never acts as an implicit actor response",async()=>{
 const silent=createSerialOwnedActor({schema,receive:async()=>result("implicit",42)});
 await assert.rejects(silent.request(compute("hi",41)),/self.done/);
 const doneWithoutReply=createSerialOwnedActor({
  schema,receive:async(mail,self)=>{self.done();return result("implicit",42)}
 });
 await assert.rejects(doneWithoutReply.request(compute("hi",41)),/without respond or forward/);
});
test("explicit respond plus done returns typed decoded Result, exactly once",async()=>{
 const actor=createSerialOwnedActor({schema,receive:process});
 const reply=await actor.request(compute("hello",41));
 assert.deepEqual(reply,result("hello",42));
 assert.equal(actor.pending,0);
 assert.equal(actor.idle,true);
});
test("double respond, forward-after-respond and send after done fail closed",async()=>{
 const actor=createSerialOwnedActor({
  schema,receive:async(mail,self)=>{
   await self.respond(result("one",1));
   assert.throws(()=>self.respond(result("two",2)),/already claimed/);
   assert.throws(()=>self.forward("other",compute("x",1)),/forward route unavailable/);
   self.done();
   assert.throws(()=>self.respond(result("three",3)),/already completed/);
  }
 });
 assert.deepEqual(await actor.request(compute("x",9)),result("one",1));
});
test("send is independent of request reply and does not finish current delivery",async()=>{
 const delivered=[];
 const actor=createSerialOwnedActor({
  schema,routeSend:async(target,bytes)=>{
   delivered.push({target,message:await decodeMessage(schema,bytes)});
  },receive:async(mail,self)=>{
   await self.send("audit",result(mail.fields.text,10));
   self.done();
  }
 });
 assert.equal(await actor.tell(compute("hi",9)),undefined);
 assert.deepEqual(delivered,[{target:"audit",message:result("hi",10)}]);
 await assert.rejects(actor.request(compute("no-reply",1)),/without respond or forward/);
});
test("forward moves response responsibility and checks the returned owned envelope",async()=>{
 const targets=[];
 const actor=createSerialOwnedActor({
  schema,routeForward:async(target,bytes)=>{
   targets.push({target,mail:await decodeMessage(schema,bytes)});
   return encodeMessage(schema,result("forwarded",43));
  },receive:async(mail,self)=>{
   await self.forward("downstream",mail);
   assert.throws(()=>self.respond(result("duplicate",99)),/already claimed/);
   self.done();
  }
 });
 assert.deepEqual(await actor.request(compute("hello",42)),result("forwarded",43));
 assert.deepEqual(targets,[{target:"downstream",mail:compute("hello",42)}]);
});
test("a forwarded malformed reply cannot masquerade as a typed result",async()=>{
 const actor=createSerialOwnedActor({
  schema,routeForward:async()=>Uint8Array.of(1,2,3),
  receive:async(mail,self)=>{await self.forward("bad",mail);self.done();}
 });
 await assert.rejects(actor.request(compute("x",1)),/invalid message size/);
});
test("FIFO single flight and aggregate mailbox capacity are enforced",async()=>{
 const gate=deferred(),entered=deferred(),seen=[];
 const actor=createSerialOwnedActor({schema,maxPending:2,receive:async(mail,self)=>{
  seen.push(mail.fields.n);
  if(mail.fields.n===1n){entered.resolve();await gate.promise;}
  await self.respond(result(mail.fields.text,mail.fields.n+1n));
  self.done();
 }});
 const one=actor.request(compute("one",1));
 const two=actor.request(compute("two",2));
 await assert.rejects(actor.request(compute("overflow",3)),/capacity exceeded/);
 await entered.promise;
 assert.deepEqual(seen,[1n]);
 gate.resolve();
 assert.deepEqual(await Promise.all([one,two]),[result("one",2),result("two",3)]);
 assert.deepEqual(seen,[1n,2n]);
});
test("queued message uses original owned payload despite caller mutation",async()=>{
 const gate=deferred();
 const actor=createSerialOwnedActor({schema,maxPending:2,receive:async(mail,self)=>{
  if(mail.fields.text==="block")await gate.promise;
  await self.respond(result(mail.fields.text,mail.fields.n));
  self.done();
 }});
 const first=actor.request(compute("block",0));
 const mutable=compute("snapshot",13);
 const second=actor.request(mutable);
 mutable.fields.text="tampered";mutable.fields.n=999n;
 gate.resolve();
 await first;
 assert.deepEqual(await second,result("snapshot",13));
});
test("stale self capability is sealed even if receive forgets done",async()=>{
 let stolen;
 const actor=createSerialOwnedActor({schema,receive:async(mail,self)=>{stolen=self;}});
 await assert.rejects(actor.request(compute("x",1)),/self.done/);
 assert.throws(()=>stolen.respond(result("late",2)),/already completed/);
 assert.throws(()=>stolen.done(),/already completed/);
});
test("failed handler does not strand the serial execution lane",async()=>{
 let n=0;
 const actor=createSerialOwnedActor({schema,receive:async(mail,self)=>{
  n++;
  if(n===1)throw Error("handler failed");
  await self.respond(result("recover",42));self.done();
 }});
 await assert.rejects(actor.request(compute("first",1)),/handler failed/);
 assert.deepEqual(await actor.request(compute("second",2)),result("recover",42));
});
test("closing rejects queued messages and prevents further admissions",async()=>{
 const gate=deferred();let opened=0;
 const actor=createSerialOwnedActor({schema,maxPending:2,receive:async(mail,self)=>{
  opened++;await gate.promise;await self.respond(result("done",1));self.done();
 }});
 const first=actor.request(compute("first",1)),queued=actor.request(compute("queued",2));
 actor.close();gate.resolve();
 await assert.rejects(queued,/actor closed/);
 await assert.rejects(first,/actor closed/);
 await assert.rejects(actor.request(compute("later",3)),/actor closed/);
 assert.ok(opened<=1);
});


test("request reply type must match the declared Compute -> Result contract",async()=>{
 const actor=createSerialOwnedActor({
  schema,replyVariants:{Compute:["Result"]},
  receive:async(mail,self)=>{
   assert.throws(()=>self.respond(compute("wrong",42)),/reply variant not admitted/);
   await self.respond(result(mail.fields.text,mail.fields.n+1n));self.done();
  }
 });
 assert.deepEqual(await actor.request(compute("typed",41)),result("typed",42));
});
test("forward must validate downstream response variant and cannot silently change contract",async()=>{
 const actor=createSerialOwnedActor({
  schema,replyVariants:{Compute:["Result"]},
  routeForward:async()=>encodeMessage(schema,compute("wrong",999)),
  receive:async(mail,self)=>{await self.forward("another",mail);self.done();}
 });
 await assert.rejects(actor.request(compute("typed",41)),/reply variant not admitted/);
});
test("one-way delivery cannot claim reply or forward even transiently",async()=>{
 const actor=createSerialOwnedActor({schema,
  routeForward:async()=>encodeMessage(schema,result("not-called",0)),
  receive:async(mail,self)=>{
   assert.throws(()=>self.respond(result("bad",1)),/one-way delivery cannot claim a reply/);
   assert.throws(()=>self.forward("other",mail),/one-way delivery cannot claim a reply/);
   self.done();
  }
 });
 assert.equal(await actor.tell(compute("oneway",41)),undefined);
});
