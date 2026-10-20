// Independently written source-free CI canary. Contains NO Oreslang compiler/runtime source.
import test from 'node:test';
import assert from 'node:assert/strict';
function newActor() {
  return {code:1,route:1,freeze:false,stop:false,queued:0,pins:0,epoch:1,delivered:0};
}
function assertSafe(s) {
  assert.equal(s.code,s.route,'code/route split');
  assert.equal(s.code,s.epoch,'generation drift');
  assert.ok(s.pins>=0,'negative generation pin count');
  assert.ok(s.queued>=0,'negative queued count');
  assert.ok(s.delivered>=0,'negative delivery count');
  if(s.stop)assert.equal(s.freeze,true,'terminal actor reopened');
}
function step(s,op) {
  switch(op) {
    case 'send':if(!s.freeze&&!s.stop)s.queued++;break;
    case 'dispatch':if(!s.stop&&s.queued){s.queued--;s.pins++;}break;
    case 'complete':if(s.pins){s.pins--;s.delivered++;}break;
    case 'freeze':if(!s.stop)s.freeze=true;break;
    case 'publish':
      if(!s.stop&&s.freeze&&!s.pins&&!s.queued){
        // Publish the *single* authoritative record, never two separate slots.
        const next=s.epoch+1;
        s.code=next;s.route=next;s.epoch=next;s.freeze=false;
      }break;
    case 'abort':if(!s.stop)s.freeze=false;break;
    case 'stop':s.stop=true;s.freeze=true;break;
  }
  assertSafe(s);
}
test('no Oreslang private source is required for this capacity/transaction canary',()=>{
  assert.throws(()=>assertSafe({...newActor(),code:2}),/code\/route split/);
  const a=newActor();
  step(a,'send');step(a,'dispatch');step(a,'freeze');
  step(a,'publish');assert.equal(a.epoch,1,'old pinned turn must block publication');
  step(a,'complete');step(a,'publish');assert.equal(a.epoch,2);
  assert.equal(a.delivered,1);
});
test('59,049 bounded event schedules preserve atomic route/code authority',()=>{
  const commands=['send','dispatch','complete','freeze','publish','abort','stop','publish','send'];
  let schedules=0;
  function walk(depth,steps) {
    if(depth===0) {
      const a=newActor();
      for(const op of steps)step(a,op);
      schedules++;return;
    }
    for(const op of commands)walk(depth-1,[...steps,op]);
  }
  walk(5,[]);
  assert.equal(schedules,59049);
});
