import test from 'node:test';
import assert from 'node:assert/strict';

// Independent, PUBLIC, source-free semantic reference. Not a native compiler test.
class SchedulerOracle {
 constructor(limit){this.limit=limit;this.cursor=0;this.rows=new Map();}
 spawn(slot){
  assert.ok(Number.isInteger(slot)&&slot>=0&&slot<this.limit);
  assert.ok(!this.rows.has(slot));
  this.rows.set(slot,{queued:0,control:false,active:false,retired:false});
 }
 data(slot){
  const x=this.rows.get(slot);
  if(!x||x.retired)return false;
  x.queued++;return true;
 }
 control(slot){
  const x=this.rows.get(slot);
  if(!x||x.retired||x.control)return false;
  x.control=true;return true;
 }
 next(){
  const ready=[...this.rows].filter(([,x])=>!x.active&&(x.control||x.queued));
  const controls=ready.filter(([,x])=>x.control);
  const candidates=controls.length?controls:ready;
  if(!candidates.length)return null;
  candidates.sort(([a],[b])=>((a-this.cursor+this.limit)%this.limit)-
                               ((b-this.cursor+this.limit)%this.limit));
  const [slot,x]=candidates[0];
  const kind=x.control?'CONTROL':'DATA';
  if(x.control)x.control=false;
  else x.queued--;
  x.active=true;
  this.cursor=(slot+1)%this.limit;
  return {slot,kind};
 }
 finish(slot){
  const x=this.rows.get(slot);
  assert.ok(x&&x.active,'only active owner can finish');
  x.active=false;
 }
 stop(slot){
  const x=this.rows.get(slot);
  assert.ok(x);
  x.retired=true;
 }
}
test('CONTROL precedes DATA across 64 and 4096 actor boundaries',()=>{
 const s=new SchedulerOracle(65536);
 for(const id of [0,63,64,4095,4096,65535])s.spawn(id);
 for(const id of [0,63,64,4095,4096,65535])assert.equal(s.data(id),true);
 assert.equal(s.control(65535),true);
 assert.deepEqual(s.next(),{slot:65535,kind:'CONTROL'});
 assert.notEqual(s.next()?.slot,65535,'busy actor cannot execute twice');
 s.finish(65535);
 let last=false;
 while(true){
  const turn=s.next();
  if(!turn)break;
  assert.equal(turn.kind,'DATA');
  if(turn.slot===65535)last=true;
  s.finish(turn.slot);
 }
 assert.equal(last,true);
});
test('round robin DATA does not starve remote actor under replenishment',()=>{
 const s=new SchedulerOracle(65536);
 for(const id of [1,65535])s.spawn(id);
 assert.ok(s.data(1)&&s.data(65535));
 let remote=false;
 for(let i=0;i<16;i++){
  const turn=s.next();
  assert.ok(turn);
  if(turn.slot===65535)remote=true;
  s.finish(turn.slot);
  if(turn.slot===1)s.data(1);
  if(remote)break;
 }
 assert.equal(remote,true);
});
test('retire rejects new work but drains admitted messages without parallel callbacks',()=>{
 const s=new SchedulerOracle(65);
 s.spawn(64);
 assert.ok(s.data(64)&&s.data(64));
 s.stop(64);
 assert.equal(s.data(64),false);
 assert.equal(s.control(64),false);
 for(let i=0;i<2;i++){
  assert.deepEqual(s.next(),{slot:64,kind:'DATA'});
  assert.equal(s.next(),null);
  s.finish(64);
 }
 assert.equal(s.next(),null);
});
