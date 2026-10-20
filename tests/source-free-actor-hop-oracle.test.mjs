// Independently authored public test oracle: synthetic actor labels only.
// Does not fetch, include, execute, or attest any private Oreslang source.
import test from 'node:test';
import assert from 'node:assert/strict';

function expectedHistory(events, maximum) {
  const visited = events.filter(e => e.type === 'actor').map(e => e.name);
  return {tail: visited.slice(-maximum), omitted: Math.max(0, visited.length - maximum)};
}

function exercise(events, maximum, tile, layout) {
  const slots = Array(maximum).fill(null);
  let next = 0, visits = 0;
  for (const event of events) {
    if (event.type !== 'actor') continue;
    slots[next] = {name:event.name, sequence:visits};
    next = (next + 1) % maximum;
    visits++;
  }
  const count = Math.min(maximum, visits);
  const rows = Array.from({length:count},(_,i)=>slots[(next - count + i + maximum)%maximum]);
  let projected;
  switch(layout) {
    case 'aos': projected = rows.map(r=>r.name); break;
    case 'soa': {
      const columns = {name:rows.map(r=>r.name), sequence:rows.map(r=>r.sequence)};
      projected = columns.name;
      break;
    }
    case 'aosoa': {
      const tiles=[];
      for(let pos=0;pos<rows.length;pos+=tile) {
        const block=rows.slice(pos,pos+tile);
        tiles.push({name:block.map(r=>r.name),sequence:block.map(r=>r.sequence)});
      }
      projected=tiles.flatMap(b=>b.name);
      break;
    }
    default: throw Error('unknown layout');
  }
  return {tail:projected,omitted:visits-count};
}

for(const layout of ['aos','soa','aosoa']) {
  test(`source-free ${layout}: bounded visits, repetitions, and async flood`,()=>{
    for(const count of [0,1,4,5,6,10,16,17,64,100]) {
      const events=Array.from({length:count},(_,i)=>({type:'actor',name:`A${i%7}`}));
      for(const cap of [5,16,64]) {
        for(const tile of [1,3,8]) {
          assert.deepEqual(exercise(events,cap,tile,layout),expectedHistory(events,cap));
        }
      }
    }
    const chain=[{type:'actor',name:'A'},{type:'actor',name:'B'},{type:'actor',name:'A'},
      {type:'actor',name:'B'},{type:'actor',name:'C'},{type:'actor',name:'D'}];
    const flood=Array.from({length:100000},(_,i)=>({type:'await',name:`unused${i%3}`}));
    const events=[...chain.slice(0,3),...flood,...chain.slice(3)];
    assert.deepEqual(exercise(events,5,4,layout),expectedHistory(events,5));
    assert.deepEqual(exercise(events,16,4,layout),expectedHistory(events,16));
  });
}

test('source-free: deterministic randomized histories agree with array-slice oracle',()=>{
  let state=0x45ac7183;
  const rng=()=>{state^=state<<13;state^=state>>>17;state^=state<<5;return state>>>0;};
  for(let trial=0;trial<400;trial++) {
    const cap=5+(rng()%60), len=rng()%130, tile=1+(rng()%9);
    const events=Array.from({length:len},(_,i)=>({type:i%5===0?'await':'actor',name:`A${rng()%11}`}));
    const expected=expectedHistory(events,cap);
    for(const layout of ['aos','soa','aosoa'])
      assert.deepEqual(exercise(events,cap,tile,layout),expected);
  }
});
