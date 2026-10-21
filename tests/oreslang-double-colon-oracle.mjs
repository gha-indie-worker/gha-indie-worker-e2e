import test from "node:test";
import assert from "node:assert/strict";

function visible(source){
  let out="", quote=null, escape=false, block=0;
  for(let i=0;i<source.length;i++){
    const c=source[i], n=source[i+1];
    if(block){
      if(c==="/"&&n==="*"){block++;out+="  ";i++;continue;}
      if(c==="*"&&n==="/"){block--;out+="  ";i++;continue;}
      out+=c==="\n"?"\n":" ";
      continue;
    }
    if(quote!==null){
      if(escape){escape=false;out+=" ";continue;}
      if(c==="\\"){escape=true;out+=" ";continue;}
      if(c===quote){quote=null;out+=" ";continue;}
      out+=c==="\n"?"\n":" ";
      continue;
    }
    if(c==="/"&&n==="/"){
      while(i<source.length&&source[i]!=="\n"){out+=" ";i++;}
      if(i<source.length) out+="\n";
      continue;
    }
    if(c==="/"&&n==="*"){block=1;out+="  ";i++;continue;}
    if(c==="\""){quote=c;out+=" ";continue;}
    out+=c;
  }
  return out;
}

function accepts(source){
  const lines=visible(source).split(/\r?\n/);
  const hi=lines.findIndex(x=>x.trim());
  if(hi<0) return false;
  const header=lines[hi];
  if(!/\b(fnc|routine)\b/.test(header)) return false;
  const close=header.indexOf(")");
  if(close<0) return false;
  let tail=header.slice(close+1).trimStart();
  if(tail.startsWith(":")) return false;
  if(!tail.startsWith("->")) return false;
  tail=tail.slice(2);

  const as=/\bas\b/.exec(tail);
  const dc=tail.indexOf("::");
  const seps=[];
  if(as) seps.push({start:as.index,end:as.index+2});
  if(dc>=0) seps.push({start:dc,end:dc+2});
  seps.sort((a,b)=>a.start-b.start);

  if(seps.length){
    const s=seps[0];
    if(!tail.slice(0,s.start).trim()) return false;
    const body=tail.slice(s.end).trimStart();
    if(/^as\b/.test(body)||body.startsWith("::")) return false;
    return true;
  }

  if(/\b(return|if|do|select|match|switch|let|const|val|await|spawn|throw|raise|panic)\b/.test(tail)) return false;
  const next=lines.slice(hi+1).find(x=>x.trim())?.trimStart()??"";
  if(/^as\b/.test(next)||next.startsWith("::")) return false;
  return true;
}

const vectors=[
  ["separator-free-next-line","fnc f() -> int\n  return 1\nend",true],
  ["same-line-as","fnc f() -> int as return 1; end",true],
  ["same-line-double-colon","fnc f() -> int :: return 1; end",true],
  ["double-colon-static-access","fnc f() -> int :: return Math::answer(); end",true],
  ["higher-order-double-colon","fnc f() -> () => void ::\n  return || -> { return; };\nend",true],
  ["string-double-colon","fnc f() -> string :: return \"::\"; end",true],
  ["comment-double-colon-before-as","fnc f() -> int /* :: */ as return 1; end",true],
  ["next-line-as","fnc f() -> int\nas\n  return 1\nend",false],
  ["next-line-double-colon","fnc f() -> int\n::\n  return 1\nend",false],
  ["same-line-no-separator","fnc f() -> int return 1; end",false],
  ["colon-return-double-colon","fnc f(): int :: return 1; end",false],
  ["as-double-colon","fnc f() -> int as :: return 1; end",false],
  ["double-colon-as","fnc f() -> int :: as return 1; end",false],
  ["comment-double-colon-not-separator","fnc f() -> int /* :: */ return 1; end",false]
];

test("Oreslang :: callable separator v2 source-free oracle",()=>{
  let yes=0,no=0;
  for(const [name,source,expected] of vectors){
    const actual=accepts(source);
    assert.equal(actual,expected,name);
    if(actual) yes++; else no++;
  }
  assert.equal(yes,7);
  assert.equal(no,7);
  console.log("ORESLANG_DOUBLE_COLON_V2_SOURCE_FREE_PASS accepted=7 rejected=7 private_source_bytes=0");
});
