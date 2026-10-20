import {test} from 'node:test';
import assert from 'node:assert/strict';
import {model,executed} from './admission-model.mjs';
const A='a'.repeat(40),B='b'.repeat(40),C='c'.repeat(40),D='d'.repeat(40),E='e'.repeat(40);
const good=()=>({
 source:{id:101,private:true,visibility:'private',commit:C,tree:D},
 destination:{id:202,private:true,visibility:'private'},
 main:{protected:true,codeowners:true,independentReviews:true,noBypasses:true,sha:A,workflowBlob:B},
 policy:{sourceId:101,destinationId:202,mainSha:A,workflowBlob:B,sourceCommit:C,sourceTree:D,reviewHead:E},
 review:{independent:true,id:303,operatorId:404,revoked:false,head:E},
 incidents:{
  22:{number:22,state:'closed',reason:'completed',receipt:{independent:true,reviewerId:505,revoked:false,sourceCommit:C,sourceTree:D,destinationId:202,reviewHead:E}},
  77:{number:77,state:'closed',reason:'completed',receipt:{independent:true,reviewerId:606,revoked:false,sourceCommit:C,sourceTree:D,destinationId:202,reviewHead:E}},
 },
 beforeWrite:{main:true,destination:true,incident22:true,incident77:true,noDrift:true}
});
const set=(obj,path,value)=>{let x=obj;const names=path.split('.');for(const p of names.slice(0,-1))x=x[p];x[names.at(-1)]=value;return obj};
test('even a fully consistent synthetic model never authorizes source export',()=>{
 const r=model(good());assert.equal(r.classification,'SYNTHETIC_MODEL_CONSISTENT');
 assert.equal(r.authorizesTransfer,false);assert.equal(r.liveAuthorityVerified,false);
});
test('all single-gate failures deny, including both distinct P0 incidents',()=>{
 const changes=[
  ['source.id',999],['source.private',false],['source.visibility','public'],
  ['destination.id',999],['destination.private',false],['destination.visibility','internal'],
  ['main.protected',false],['main.codeowners',false],['main.independentReviews',false],['main.noBypasses',false],
  ['main.sha',B],['main.workflowBlob',A],['source.commit',B],['source.tree',B],
  ['review.independent',false],['review.id',404],['review.revoked',true],['review.head',A],
  ['incidents.22.state','open'],['incidents.22.reason','not_planned'],['incidents.77.state','open'],
  ['incidents.77.receipt.revoked',true],['incidents.77.receipt.sourceTree',A],
  ['incidents.22.receipt.destinationId',999],['incidents.22.receipt.reviewerId',404],
  ['beforeWrite.main',false],['beforeWrite.destination',false],['beforeWrite.incident22',false],
  ['beforeWrite.incident77',false],['beforeWrite.noDrift',false]
 ];
 for(const [path,val] of changes){const r=model(set(good(),path,val));assert.equal(r.classification,'DENIED',path);assert.equal(r.authorizesTransfer,false);}
});
test('exhaustive independent revocation bitmask is deny-by-default',()=>{
 const fields=['main.protected','main.codeowners','main.independentReviews','main.noBypasses',
  'review.independent','incidents.22.receipt.independent','incidents.77.receipt.independent',
  'beforeWrite.main','beforeWrite.destination','beforeWrite.incident22','beforeWrite.incident77','beforeWrite.noDrift'];
 for(let mask=0;mask<2**fields.length;mask++){
  const obj=good();fields.forEach((f,i)=>set(obj,f,!!(mask&(1<<i))));
  const x=model(obj);assert.equal(x.authorizesTransfer,false);
  assert.equal(x.classification,mask===2**fields.length-1?'SYNTHETIC_MODEL_CONSISTENT':'DENIED');
 }
});
test('no metadata and forged caller approval fail closed',()=>{
 for(const x of [{},{approved:true},{incidents:{22:'closed',77:'closed'}},{source:{private:true},destination:{private:true}}])
  assert.equal(model(x).classification,'DENIED');
});
test('source-free tests may execute but never prove private compiler tests',()=>{
 const run={id:100,attempt:1,head:A,status:'completed',conclusion:'success',jobs:[{
  id:200,runnerId:9,head:A,runId:100,attempt:1,conclusion:'success',
  steps:[{name:'Set up job',status:'completed',conclusion:'success'},
   {name:'Run synthetic policy tests',status:'completed',conclusion:'success'}]}]};
 assert.equal(executed(run,A),'SYNTHETIC_TEST_STEPS_EXECUTED');
 assert.equal(executed({...run,jobs:[{...run.jobs[0],steps:null}]},A),'STEPS_UNOBSERVED');
 assert.equal(executed({...run,jobs:[{...run.jobs[0],steps:[]}]},A),'OBSERVED_ZERO_STEPS');
 assert.equal(executed({...run,jobs:[{...run.jobs[0],steps:[run.jobs[0].steps[0]]}]},A),'NO_TEST_STEPS');
 assert.equal(executed(run,B),'RUN_IDENTITY_UNKNOWN');
 assert.equal(executed({...run,jobs:[{...run.jobs[0],runnerId:null}]},A),'JOB_IDENTITY_UNKNOWN');
});
test('failed runs and mismatched job attempts never certify execution',()=>{
 const x={id:100,attempt:1,head:A,status:'completed',conclusion:'failure',jobs:[{
 id:3,runnerId:5,head:A,runId:100,attempt:1,conclusion:'failure',steps:[]}]};
 assert.equal(executed(x,A),'RUN_NOT_SUCCESS');
 x.conclusion='success';x.jobs[0].conclusion='success';x.jobs[0].attempt=2;
 assert.equal(executed(x,A),'JOB_IDENTITY_UNKNOWN');
});
