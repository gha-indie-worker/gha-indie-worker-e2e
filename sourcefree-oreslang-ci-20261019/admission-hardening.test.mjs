import test from 'node:test';
import assert from 'node:assert/strict';
import {model, executed} from './admission-model.mjs';

// Independent, public synthetic metadata only. No Oreslang private source.
const A='a'.repeat(40),B='b'.repeat(40),C='c'.repeat(40),D='d'.repeat(40);
const valid=()=>({
 source:{id:101,private:true,visibility:'private',commit:C,tree:D},
 destination:{id:202,private:true,visibility:'private'},
 main:{protected:true,codeowners:true,independentReviews:true,noBypasses:true,sha:A,workflowBlob:B},
 policy:{sourceId:101,destinationId:202,mainSha:A,workflowBlob:B,sourceCommit:C,sourceTree:D,reviewHead:A},
 review:{independent:true,id:303,operatorId:404,revoked:false,head:A},
 incidents:{
  22:{number:22,state:'closed',reason:'completed',receipt:{independent:true,reviewerId:505,revoked:false,sourceCommit:C,sourceTree:D,destinationId:202,reviewHead:A}},
  77:{number:77,state:'closed',reason:'completed',receipt:{independent:true,reviewerId:606,revoked:false,sourceCommit:C,sourceTree:D,destinationId:202,reviewHead:A}},
 },
 beforeWrite:{main:true,destination:true,incident22:true,incident77:true,noDrift:true}
});
test('source and destination must not alias even with self-consistent supplied IDs',()=>{
 const s=valid();
 assert.equal(model(s).classification,'SYNTHETIC_MODEL_CONSISTENT');
 s.destination.id=s.source.id;
 s.policy.destinationId=s.source.id;
 s.incidents[22].receipt.destinationId=s.source.id;
 s.incidents[77].receipt.destinationId=s.source.id;
 const r=model(s);
 assert.equal(r.classification,'DENIED');
 assert.ok(r.denied.includes('destination'));
 assert.equal(r.authorizesTransfer,false);
});
test('malformed or missing metadata denies rather than throwing',()=>{
 const inputs=[null,undefined,false,true,42,'approved',[],
  {source:null,destination:null,main:null,policy:null,review:null,incidents:null,beforeWrite:null},
  {source:[],destination:[],main:[],policy:[],review:[],incidents:[],beforeWrite:[]}];
 for(const x of inputs){
  assert.doesNotThrow(()=>model(x));
  const result=model(x);
  assert.equal(result.classification,'DENIED');
  assert.equal(result.authorizesTransfer,false);
 }
});
test('caller-supplied override bits and tokens cannot authorize source publication',()=>{
 const s=valid();
 s.allowPrivateExport=true;
 s.token='synthetic-not-a-real-token';
 s.policy.operatorOverride=true;
 const r=model(s);
 assert.equal(r.authorizesTransfer,false);
 assert.equal(r.liveAuthorityVerified,false);
});
test('null jobs, absent steps and forged step metadata never certify execution',()=>{
 const run={id:100,attempt:1,head:A,status:'completed',conclusion:'success',jobs:[{
   id:200,runnerId:300,runId:100,attempt:1,head:A,conclusion:'success',
   steps:[{name:'Run synthetic policy tests',status:'completed',conclusion:'success'}]
 }]};
 assert.equal(executed(run,A),'SYNTHETIC_TEST_STEPS_EXECUTED');
 assert.equal(executed({...run,jobs:[null]},A),'JOB_IDENTITY_UNKNOWN');
 assert.equal(executed({...run,jobs:[]},A),'RUN_NOT_SUCCESS');
 assert.equal(executed({...run,jobs:[{...run.jobs[0],steps:[null]}]},A),'NO_TEST_STEPS');
 assert.equal(executed({...run,jobs:[{...run.jobs[0],steps:null}]},A),'STEPS_UNOBSERVED');
 assert.equal(executed({...run,jobs:[{...run.jobs[0],steps:[]}]},A),'OBSERVED_ZERO_STEPS');
 assert.equal(executed({...run,jobs:[{...run.jobs[0],runnerId:null}]},A),'JOB_IDENTITY_UNKNOWN');
});

test('job and step evidence must agree before a synthetic run can be credited',()=>{
 const step={name:'Run synthetic policy tests',status:'completed',conclusion:'success'};
 const job={id:200,runnerId:300,runId:100,attempt:1,head:A,conclusion:'success',
   steps:[step,{name:'Post checkout',status:'completed',conclusion:'success'}]};
 const run={id:100,attempt:1,head:A,status:'completed',conclusion:'success',jobs:[job]};
 assert.equal(executed(run,A),'SYNTHETIC_TEST_STEPS_EXECUTED');
 for(const contradiction of [
   {name:'Run a failing step',status:'completed',conclusion:'failure'},
   {name:'Cancel',status:'completed',conclusion:'cancelled'},
   {name:'Unfinished job step',status:'in_progress',conclusion:null},
   {name:'Forged success',status:null,conclusion:'success'},
   {name:'Malformed',status:'completed',conclusion:'success',unexpected:true,other:1},
   null,
 ]){
   const changed=contradiction?.name==='Malformed'?{...contradiction,name:undefined}:contradiction;
   const result=executed({...run,jobs:[{...job,steps:[step,changed]}]},A);
   assert.equal(result,'JOB_STEP_CONFLICT',JSON.stringify(contradiction));
 }
 assert.equal(executed({...run,jobs:[{...job,steps:[step,{name:'Optional',status:'completed',conclusion:'skipped'}]}]},A),
   'SYNTHETIC_TEST_STEPS_EXECUTED');
 assert.equal(executed({...run,jobs:[{...job,steps:[{name:'Run synthetic policy tests',status:'completed',conclusion:'skipped'}]}]},A),
   'NO_TEST_STEPS');
});
