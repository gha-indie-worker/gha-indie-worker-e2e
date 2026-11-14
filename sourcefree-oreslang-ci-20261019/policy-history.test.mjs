import test from 'node:test';
import assert from 'node:assert/strict';
import {syntheticHistoryAudit} from './policy-history.mjs';
const A='a'.repeat(40),B='b'.repeat(40),C='c'.repeat(40);
const line='synthetic/private-runner 202\n';
const safe={sha:A,parent:C,policyText:'# empty\n',protectedMain:false,independentReview:false};
const active={sha:B,parent:A,policyText:line,protectedMain:false,independentReview:false};
test('empty current allowlist does not erase historical unauthorized activation',()=>{
 const output=syntheticHistoryAudit({complete:true,commits:[safe,active,{...safe,sha:C,parent:B}]});
 assert.equal(output.status,'HISTORICAL_POLICY_REVIEW_REQUIRED');
 assert.deepEqual(output.windows.map(x=>x.index),[1]);
 assert.equal(output.windows[0].reason,'UNREVIEWED_DESTINATION');
 assert.equal(output.authorizesTransfer,false);
});
test('comment-only candidate is not an active destination',()=>{
 const output=syntheticHistoryAudit({complete:true,commits:[safe,{...safe,sha:B,parent:A,policyText:'# '+line}]});
 assert.equal(output.status,'NO_ACTIVE_ROW_IN_PROVIDED_HISTORY');
 assert.equal(output.completeRealGitHistoryVerified,false);
});
test('a slug lacking numeric ID is a malformed active destination',()=>{
 const output=syntheticHistoryAudit({complete:true,commits:[safe,{...active,policyText:'synthetic/private-runner\n'}]});
 assert.equal(output.windows[0].reason,'MALFORMED_ACTIVE_DESTINATION');
});
test('even a hypothetically protected independently reviewed destination is not live approval',()=>{
 const output=syntheticHistoryAudit({complete:true,commits:[safe,{...active,protectedMain:true,independentReview:true}]});
 assert.equal(output.status,'HISTORICAL_POLICY_REVIEW_REQUIRED');
 assert.equal(output.windows[0].reason,'ACTIVE_DESTINATION_REQUIRES_EXTERNAL_PROOF');
 assert.equal(output.authorizesTransfer,false);
});
test('incomplete, disconnected or noncanonical history denies certification',()=>{
 for(const input of [
  {complete:false,commits:[safe,active]},
  {complete:true,commits:[safe]},
  {complete:true,commits:[safe,{...active,parent:C}]},
  {complete:true,commits:[safe,{...active,policyText:line+'\u2028'}]},
  {complete:true,commits:[safe,{...active,policyText:line+'\r'}]}
 ]){
  const output=syntheticHistoryAudit(input);
  assert.notEqual(output.status,'NO_ACTIVE_ROW_IN_PROVIDED_HISTORY');
  assert.equal(output.authorizesTransfer,false);
 }
});
