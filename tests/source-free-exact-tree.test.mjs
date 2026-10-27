import test from 'node:test';
import assert from 'node:assert/strict';

// Source-free synthetic gate. This does not authorize copying any private source.
const sha = s => typeof s === 'string' && /^[0-9a-f]{40}$/.test(s);
const denied = m => { throw new Error(m); };
function verify(input) {
  if (input.privateSource !== false) denied('private source transfer prohibited in source-free canary');
  if (input.crossOrgCheckout !== false) denied('cross-org checkout prohibited');
  if (input.secretForwarding !== false) denied('secret forwarding prohibited');
  if (!sha(input.sourceHead) || !sha(input.sourceTree) || !sha(input.mirrorTree))
    denied('invalid canonical SHA');
  if (input.sourceTree !== input.mirrorTree) denied('root tree mismatch');
  if (input.verifiedHead !== input.sourceHead) denied('head mismatch');
  if (input.actualCheckoutTree !== input.mirrorTree) denied('checkout tree mismatch');
  if (input.executableSteps !== true) denied('zero-step result is NOT_EXECUTED');
  if (input.runnerId <= 0 || !Number.isSafeInteger(input.runnerId))
    denied('missing allocated runner');
  if (input.sourceProtected !== true || input.destProtected !== true)
    denied('private source would require protected source and destination');
  if (input.independentApproval !== true) denied('independent review missing');
  if (input.requireIssuerBoundCheck !== true) denied('required status check issuer missing');
  return true;
}
const s='a'.repeat(40), t='b'.repeat(40);
const good=()=>({
  privateSource:false, crossOrgCheckout:false, secretForwarding:false,
  sourceHead:s, verifiedHead:s, sourceTree:t, mirrorTree:t, actualCheckoutTree:t,
  executableSteps:true, runnerId:1234,
  sourceProtected:true, destProtected:true, independentApproval:true,
  requireIssuerBoundCheck:true
});
test('synthetic exact root and execution receipt admitted only under all gates',()=>{
  assert.equal(verify(good()),true);
});
for (const [name,patch] of [
 ['private code disclosure',{privateSource:true}],
 ['cross-org token checkout',{crossOrgCheckout:true}],
 ['secret forwarding',{secretForwarding:true}],
 ['invalid SHA',{sourceTree:'not-a-sha'}],
 ['tree drift',{mirrorTree:s}],
 ['stale source head',{verifiedHead:t}],
 ['different checkout tree',{actualCheckoutTree:s}],
 ['unexecuted steps',{executableSteps:false}],
 ['runner not allocated',{runnerId:0}],
 ['source protection absent',{sourceProtected:false}],
 ['destination protection absent',{destProtected:false}],
 ['approval absent',{independentApproval:false}],
 ['issuer unbound',{requireIssuerBoundCheck:false}]
]) test('fail closed: '+name,()=>{
  assert.throws(()=>verify({...good(),...patch}));
});
test('invalid runner counter also rejected',()=>{
  assert.throws(()=>verify({...good(),runnerId:1.5}));
});
test('source-free gate never treats tests as private build qualification',()=>{
  assert.equal(good().privateSource,false);
  assert.equal(process.env.PRIVATE_ORESLANG_SOURCE ?? '', '');
});
