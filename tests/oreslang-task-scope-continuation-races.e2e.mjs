// Source-free adversarial continuation race model.
// Contains no Oreslang source, blobs, trees, policies, or private metadata.

class Scope {
  constructor(){ this.closing=false; this.cancelled=false; this.active=new Set(); }
  reserve(parent=null){
    if(this.cancelled) throw new Error("cancelled");
    if(this.closing && (!parent || !parent.maySpawn())) throw new Error("closed");
    const r=new Reg(this); this.active.add(r); return r;
  }
  close(){ this.closing=true; }
  cancel(){ this.closing=true; this.cancelled=true; }
  retire(r){ if(!this.active.delete(r)) throw new Error("double retire"); }
  drained(){ return this.closing && this.active.size===0; }
}
class Reg {
  constructor(scope){ this.scope=scope; this.began=false; this.terminal=false; this.quiescent=false; this.retired=false; }
  beganExecution(){ if(this.retired||this.terminal||this.quiescent) throw new Error("retired"); this.began=true; }
  maySpawn(){ return this.began && !this.terminal && !this.quiescent && !this.retired; }
  terminalReady(){ this.terminal=true; this.finish(); }
  quiesced(){ this.quiescent=true; this.finish(); }
  failedAdmission(){ this.terminal=true; this.quiescent=true; this.finish(); }
  finish(){ if(!this.retired&&this.terminal&&this.quiescent){ this.retired=true; this.scope.retire(this); } }
}
const assert=(c,m)=>{ if(!c) throw new Error(m); };

// Deterministic two-order witness check, repeated to catch model regressions.
for(let i=0;i<2000;i++){
  const s=new Scope(), r=s.reserve(); r.beganExecution(); s.close();
  if((i&1)===0){ r.terminalReady(); assert(!s.drained(),"terminal-only drained"); r.quiesced(); }
  else { r.quiesced(); assert(!s.drained(),"quiescence-only drained"); r.terminalReady(); }
  assert(s.drained(),"conjunctive retirement failed");
}

// Model callback-child admission racing terminal publication: either admission
// occurs while the chain permit is live, or it is rejected after terminal.
// In neither branch can the closing scope leak a child.
for(let i=0;i<2000;i++){
  const s=new Scope(), chain=s.reserve(); chain.beganExecution(); s.close();
  if((i&1)===0){
    const child=s.reserve(chain); child.beganExecution();
    chain.terminalReady(); chain.quiesced();
    assert(!s.drained(),"admitted callback child escaped drain");
    child.terminalReady(); child.quiesced();
  } else {
    chain.terminalReady(); chain.quiesced();
    let rejected=false; try{s.reserve(chain);}catch{rejected=true;}
    assert(rejected,"retired chain reopened scope");
  }
  assert(s.drained(),"scope did not drain after race outcome");
}

// Cancellation seal always dominates descendant admission.
for(let i=0;i<2000;i++){
  const s=new Scope(), parent=s.reserve(); parent.beganExecution(); s.close(); s.cancel();
  let rejected=false; try{s.reserve(parent);}catch{rejected=true;}
  assert(rejected,"cancelled scope admitted descendant");
  parent.terminalReady(); parent.quiesced();
  assert(s.drained(),"cancelled scope failed to drain after physical retirement");
}

// Scoped generator policy is fail-closed until producer lifetime participates.
function activateGenerator(scope){
  if(scope) throw new Error("generator activation inside TaskScope disabled");
  return "generator";
}
for(let i=0;i<1000;i++){
  let denied=false; try{activateGenerator(new Scope());}catch(e){denied=e.message.includes("TaskScope");}
  assert(denied,"scoped generator activation was not denied");
}

console.log("ORESLANG_SOURCE_FREE_CONTINUATION_STRESS_PASS " +
  "rounds=7000 callback_race=sealed cancellation=sealed generator_scope=denied private_source_bytes=0");
