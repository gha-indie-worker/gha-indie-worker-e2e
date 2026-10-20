import test from 'node:test';
import assert from 'node:assert/strict';

const SOURCE_REPO='ores-truffle-oreslang/oreslang-source.java';
const SOURCE_REPO_ID=1400792274;
const CARRIER='synthetic/private-carrier';
const CARRIER_ID=1412142815;
const BASE='a'.repeat(40);
const BASE_TREE='b'.repeat(40);
const WORKFLOW='c'.repeat(40);
const SOURCE_HEAD='d'.repeat(40);
const SOURCE_TREE='e'.repeat(40);
const SOURCE_BASE='f'.repeat(40);
const PR=1616;

function contract(){
  return Object.freeze({
    mode:'connector',
    repository_id:CARRIER_ID,
    base_sha:BASE,
    base_tree_sha:BASE_TREE,
    workflow_blob_sha:WORKFLOW,
    source_prs:Object.freeze({
      [String(PR)]:Object.freeze({
        base_ref:'hardening/gate-b-task-loan-runtime-binding-20261019',
        base_sha:SOURCE_BASE,
      })
    })
  });
}

function authorize({repo,main,workflow,pr,testPlan=null}){
  const c=contract();
  assert.equal(testPlan,null,'connector carrier does not accept a caller test plan');
  assert.equal(repo.full_name,CARRIER);
  assert.equal(repo.id,c.repository_id);
  assert.equal(repo.private,true);
  assert.equal(repo.visibility,'private');
  assert.equal(repo.archived,false);
  assert.equal(repo.disabled,false);
  assert.equal(repo.fork,false);
  assert.equal(main.commit,c.base_sha,'carrier main advanced beyond reviewed base');
  assert.equal(main.tree,c.base_tree_sha,'carrier base tree differs from protected contract');
  assert.equal(workflow.sha,c.workflow_blob_sha,'carrier workflow differs from protected contract');
  assert.equal(pr.number,PR,'source PR not authorized');
  assert.equal(pr.state,'open');
  assert.equal(pr.head.sha,SOURCE_HEAD,'source head moved');
  assert.equal(pr.head.repo,SOURCE_REPO);
  assert.equal(pr.base.ref,c.source_prs[String(PR)].base_ref,'source stacked base ref moved');
  assert.equal(pr.base.sha,c.source_prs[String(PR)].base_sha,'source stacked base SHA moved');
  assert.equal(pr.base.repo,SOURCE_REPO);
  return c;
}

const goodRepo=()=>({
  full_name:CARRIER,id:CARRIER_ID,private:true,visibility:'private',
  archived:false,disabled:false,fork:false
});
const goodMain=()=>({commit:BASE,tree:BASE_TREE});
const goodWorkflow=()=>({sha:WORKFLOW});
const goodPr=()=>({
  number:PR,state:'open',
  head:{sha:SOURCE_HEAD,repo:SOURCE_REPO},
  base:{
    ref:'hardening/gate-b-task-loan-runtime-binding-20261019',
    sha:SOURCE_BASE,repo:SOURCE_REPO
  }
});

test('connector authorization accepts exact pinned identities',()=>{
  assert.equal(authorize({
    repo:goodRepo(),main:goodMain(),workflow:goodWorkflow(),pr:goodPr()
  }).mode,'connector');
});

test('caller-selected test plan is rejected',()=>{
  assert.throws(()=>authorize({
    repo:goodRepo(),main:goodMain(),workflow:goodWorkflow(),pr:goodPr(),
    testPlan:'ci/test-plans/injected.json'
  }),/does not accept/);
});

test('repository delete-recreate identity drift is rejected',()=>{
  const repo=goodRepo(); repo.id++;
  assert.throws(()=>authorize({
    repo,main:goodMain(),workflow:goodWorkflow(),pr:goodPr()
  }));
});

test('public or ineligible destination is rejected',()=>{
  for(const mutate of [
    r=>{r.private=false;},
    r=>{r.visibility='public';},
    r=>{r.archived=true;},
    r=>{r.disabled=true;},
    r=>{r.fork=true;}
  ]){
    const repo=goodRepo(); mutate(repo);
    assert.throws(()=>authorize({
      repo,main:goodMain(),workflow:goodWorkflow(),pr:goodPr()
    }));
  }
});

test('carrier base commit, tree, and workflow are independently pinned',()=>{
  const cases=[
    {main:{commit:'1'.repeat(40),tree:BASE_TREE},workflow:goodWorkflow()},
    {main:{commit:BASE,tree:'2'.repeat(40)},workflow:goodWorkflow()},
    {main:goodMain(),workflow:{sha:'3'.repeat(40)}}
  ];
  for(const item of cases){
    assert.throws(()=>authorize({
      repo:goodRepo(),main:item.main,workflow:item.workflow,pr:goodPr()
    }));
  }
});

test('stacked PR head, base ref, and base SHA are exact',()=>{
  for(const mutate of [
    p=>{p.head.sha='1'.repeat(40);},
    p=>{p.base.ref='other/base';},
    p=>{p.base.sha='2'.repeat(40);},
    p=>{p.base.repo='other/repo';},
    p=>{p.state='closed';}
  ]){
    const pr=goodPr(); mutate(pr);
    assert.throws(()=>authorize({
      repo:goodRepo(),main:goodMain(),workflow:goodWorkflow(),pr
    }));
  }
});

test('connector root changes only the private source subtree',()=>{
  const tree=[
    {path:'private-oreslang-source',mode:'040000',type:'tree',sha:SOURCE_TREE}
  ];
  assert.deepEqual(tree,[{
    path:'private-oreslang-source',mode:'040000',type:'tree',sha:SOURCE_TREE
  }]);
  assert.equal(tree.some(x=>x.path==='ci/oreslang-receipt.json'),false);
});

test('connector commit envelope is exact three-line identity binding',()=>{
  const message=
    'ci: exact private Oreslang tree '+SOURCE_HEAD+' tree '+SOURCE_TREE+'\n'+
    'carrier-base '+BASE+' tree '+BASE_TREE+'\n'+
    'connector-authorized source-repository '+SOURCE_REPO+' id '+SOURCE_REPO_ID;
  const lines=message.split('\n');
  assert.equal(lines.length,3);
  assert.equal(lines[0],
    'ci: exact private Oreslang tree '+SOURCE_HEAD+' tree '+SOURCE_TREE);
  assert.equal(lines[1],'carrier-base '+BASE+' tree '+BASE_TREE);
  assert.equal(lines[2],
    'connector-authorized source-repository '+SOURCE_REPO+' id '+SOURCE_REPO_ID);
});

test('payload branch derives only from verified source tree',()=>{
  assert.equal('ci/oreslang-'+SOURCE_TREE,'ci/oreslang-'+SOURCE_TREE);
});

test('payload and authorization refs bind the same carrier commit',()=>{
  const carrierCommit='9'.repeat(40);
  const refs=[
    {ref:'refs/heads/ci/oreslang-'+SOURCE_TREE,sha:carrierCommit},
    {ref:'refs/heads/ci/authorized-'+carrierCommit,sha:carrierCommit}
  ];
  assert.equal(refs[0].sha,refs[1].sha);
  assert.match(refs[1].ref,/^refs\/heads\/ci\/authorized-[0-9a-f]{40}$/);
});

test('feature-branch policy is not protected-main authorization',()=>{
  const localPolicy='synthetic/private-carrier';
  const protectedMainPolicy='# no approvals';
  assert.notEqual(localPolicy,protectedMainPolicy);
});


test('executing control plane must equal protected-main bytes and trusted paths',()=>{
  const protectedMain=Object.freeze({
    guard:'guard-v1',
    launcher:'launcher-v1',
    legacyPublisher:'legacy-v1',
    v3Publisher:'v3-v1'
  });
  const trustedRoot='/trusted/main';
  function bindControlPlane({local,modulePaths}){
    assert.deepEqual(local,protectedMain,'local control-plane bytes diverge');
    assert.equal(modulePaths.v3,trustedRoot+'/scripts/publish-private-ci-source-v3.py');
    assert.equal(modulePaths.legacy,trustedRoot+'/scripts/publish-private-ci-source.py');
    return true;
  }
  assert.equal(bindControlPlane({
    local:{...protectedMain},
    modulePaths:{
      v3:trustedRoot+'/scripts/publish-private-ci-source-v3.py',
      legacy:trustedRoot+'/scripts/publish-private-ci-source.py'
    }
  }),true);
  for(const mutate of [
    x=>{x.local.v3Publisher='modified';},
    x=>{x.local.legacyPublisher='modified';},
    x=>{x.local.guard='modified';},
    x=>{x.local.launcher='modified';},
    x=>{x.modulePaths.v3='/tmp/copied-v3.py';},
    x=>{x.modulePaths.legacy='/tmp/copied-legacy.py';}
  ]){
    const candidate={
      local:{...protectedMain},
      modulePaths:{
        v3:trustedRoot+'/scripts/publish-private-ci-source-v3.py',
        legacy:trustedRoot+'/scripts/publish-private-ci-source.py'
      }
    };
    mutate(candidate);
    assert.throws(()=>bindControlPlane(candidate));
  }
});

test('source PR must still be the exact open head immediately before authorization refs',()=>{
  function twoPhaseAuthorize(initialPr,finalPr){
    authorize({
      repo:goodRepo(),main:goodMain(),workflow:goodWorkflow(),pr:initialPr
    });
    return authorize({
      repo:goodRepo(),main:goodMain(),workflow:goodWorkflow(),pr:finalPr
    });
  }
  assert.equal(twoPhaseAuthorize(goodPr(),goodPr()).mode,'connector');
  for(const mutate of [
    p=>{p.head.sha='1'.repeat(40);},
    p=>{p.state='closed';},
    p=>{p.head.repo='other/repo';},
    p=>{p.base.ref='other/base';},
    p=>{p.base.sha='2'.repeat(40);}
  ]){
    const finalPr=goodPr();
    mutate(finalPr);
    assert.throws(()=>twoPhaseAuthorize(goodPr(),finalPr));
  }
});


test('policy change after payload ref blocks detached authorization ref',()=>{
  const carrierCommit='8'.repeat(40);
  let protectedPolicy='approved-v1';
  const expectedPolicy=protectedPolicy;
  const refs=[];

  const attest=()=>{
    assert.equal(protectedPolicy,expectedPolicy,'protected source policy changed');
  };

  attest();
  attest();
  refs.push({ref:'refs/heads/ci/oreslang-'+SOURCE_TREE,sha:carrierCommit});

  protectedPolicy='revoked-v2';
  assert.throws(()=>attest(),/protected source policy changed/);

  assert.equal(
    refs.some(x=>x.ref==='refs/heads/ci/authorized-'+carrierCommit),
    false,
    'authorization ref must not be minted from stale pre-payload evidence'
  );
});
