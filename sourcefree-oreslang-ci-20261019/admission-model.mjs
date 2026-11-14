// Source-free synthetic policy oracle. Never grants Git/source export.
const HEX=/^[a-f0-9]{40}$/;
const pin=(a,b)=>typeof a==='string'&&HEX.test(a)&&a===b;
const pos=x=>Number.isSafeInteger(x)&&x>0;
const object=x=>x&&typeof x==='object'&&!Array.isArray(x)?x:{};
export function model(s){
  const denied=[];
  const require=(ok,k)=>{if(!ok)denied.push(k)};
  s=object(s);
  const src=object(s.source),dst=object(s.destination),m=object(s.main),p=object(s.policy),v=object(s.review),incidents=object(s.incidents);
  require(pos(src.id)&&src.id===p.sourceId&&src.private===true&&src.visibility==='private','source');
  require(pos(dst.id)&&dst.id===p.destinationId&&dst.id!==src.id&&dst.private===true&&dst.visibility==='private','destination');
  require(m.protected===true&&m.codeowners===true&&m.independentReviews===true&&m.noBypasses===true,'main');
  require(pin(m.sha,p.mainSha)&&pin(m.workflowBlob,p.workflowBlob),'immutable policy/workflow');
  require(pin(src.commit,p.sourceCommit)&&pin(src.tree,p.sourceTree),'exact snapshot');
  require(v.independent===true&&pos(v.id)&&v.id!==v.operatorId&&v.revoked===false&&pin(v.head,p.reviewHead),'independent review');
  for(const n of [22,77]){
    const x=object(incidents[n]),r=object(x.receipt);
    require(x.number===n&&x.state==='closed'&&x.reason==='completed'&&
      r.independent===true&&r.revoked===false&&pos(r.reviewerId)&&r.reviewerId!==v.operatorId&&
      pin(r.sourceCommit,src.commit)&&pin(r.sourceTree,src.tree)&&
      r.destinationId===dst.id&&pin(r.reviewHead,v.head),'incident '+n);
  }
  const z=object(s.beforeWrite);
  require(z.main===true&&z.destination===true&&z.incident22===true&&z.incident77===true&&z.noDrift===true,'write recheck');
  return {classification:denied.length?'DENIED':'SYNTHETIC_MODEL_CONSISTENT',denied,
    authorizesTransfer:false,liveAuthorityVerified:false};
}
export function executed(run,head){
  if(!run||!pin(run.head,head)||!pos(run.id)||!pos(run.attempt))return 'RUN_IDENTITY_UNKNOWN';
  if(run.status!=='completed'||run.conclusion!=='success'||!Array.isArray(run.jobs)||!run.jobs.length)return 'RUN_NOT_SUCCESS';
  let missing=false,empty=false,testSteps=0;
  for(const j of run.jobs){
    if(!j||!pos(j.id)||!pos(j.runnerId)||!pin(j.head,head)||j.runId!==run.id||j.attempt!==run.attempt||j.conclusion!=='success')return 'JOB_IDENTITY_UNKNOWN';
    if(!Array.isArray(j.steps)){missing=true;continue;}
    if(!j.steps.length)empty=true;
    // The job-level success flag is insufficient if step evidence contradicts
    // it or is incomplete. Treat failed/cancelled/in-progress/forged steps as
    // untrustworthy rather than counting an earlier successful test step.
    if(j.steps.some(x=>!x||typeof x.name!=='string'||
      !((x.status==='completed'&&x.conclusion==='success')||
        (x.status==='completed'&&x.conclusion==='skipped'))))return 'JOB_STEP_CONFLICT';
    testSteps+=j.steps.filter(x=>x.conclusion==='success'&&/^Run (tests|unit tests|synthetic policy tests)$/i.test(x.name)).length;
  }
  return missing?'STEPS_UNOBSERVED':empty?'OBSERVED_ZERO_STEPS':testSteps?'SYNTHETIC_TEST_STEPS_EXECUTED':'NO_TEST_STEPS';
}
