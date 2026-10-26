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

// Structured generator model: activation is lazy. The producer becomes a
// scope child on first pull, normal scope close requests graceful stop, and
// terminal state cannot drain until the producer carrier is physically gone.
class GeneratorModel {
  constructor(scope,parent=null){
    this.scope=scope; this.parent=parent; this.reg=null;
    this.started=false; this.closed=false; this.terminal=false; this.quiescent=false;
  }
  pull(){
    if(this.closed) return "done";
    if(!this.started){
      this.reg=this.scope ? this.scope.reserve(this.parent) : null;
      if(this.reg) this.reg.beganExecution();
      this.started=true;
    }
    return "yield";
  }
  scopeCloseSignal(){
    this.closed=true;
  }
  producerTerminal(){
    this.terminal=true;
    if(this.reg) this.reg.terminalReady();
  }
  producerQuiescent(){
    this.quiescent=true;
    if(this.reg) this.reg.physicallyQuiescent();
  }
}
for(let i=0;i<1000;i++){
  const s=new Scope();
  const g=new GeneratorModel(s);
  assert(g.pull()==="yield","generator did not start on first pull");
  s.close();
  g.scopeCloseSignal();
  g.producerTerminal();
  assert(!s.drained(),"generator terminal forged physical retirement");
  g.producerQuiescent();
  assert(s.drained(),"structured generator did not drain after quiescence");
  assert(g.pull()==="done","closed generator resumed after scope close");
}

// An unused lazy generator is not a child and cannot pin lexical exit.
for(let i=0;i<1000;i++){
  const s=new Scope();
  const g=new GeneratorModel(s);
  s.close();
  assert(s.drained(),"unused lazy generator pinned scope close");
  g.scopeCloseSignal();
}

console.log("ORESLANG_SOURCE_FREE_CONTINUATION_STRESS_PASS " +
  "rounds=8000 callback_race=sealed cancellation=sealed generator_scope=structured " +
  "generator_terminal_ne_quiescence=proven lazy_unused=nonchild private_source_bytes=0");
