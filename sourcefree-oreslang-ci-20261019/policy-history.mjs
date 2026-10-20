/**
 * Synthetic, metadata-only policy-history regression checker.
 * Can flag an unreviewed activation even when CURRENT allowlist is empty.
 * Does not query GitHub or certify that all historical commits were supplied.
 */
const SHA=/^[a-f0-9]{40}$/;
const printable=bytes=>bytes.length<=65536&&
 [...bytes].every(char=>char==='\n'||(char.charCodeAt(0)>=32&&char.charCodeAt(0)<=126));
export function syntheticHistoryAudit({complete,commits}={}) {
 const failures=[], windows=[];
 if(complete!==true||!Array.isArray(commits)||commits.length<2||commits.length>10000)
   return {status:'UNVERIFIABLE_HISTORY',failures:['incomplete commit history'],windows,authorizesTransfer:false};
 for(let i=0;i<commits.length;i++){
   const x=commits[i];
   if(!x||typeof x.sha!=='string'||!SHA.test(x.sha)||
      (i>0&&x.parent!==commits[i-1].sha)||
      typeof x.policyText!=='string'||!printable(x.policyText)){
     failures.push('malformed or disconnected commit history');break;
   }
   const lines=x.policyText.split('\n').filter(l=>l.length>0&&!l.startsWith('#'));
   for(const l of lines){
     // Every potentially active line is relevant, including malformed rows.
     const match=/^([A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+) ([1-9][0-9]{0,15})$/.exec(l);
     const canonical=match&&Number.isSafeInteger(Number(match[2]));
     if(!canonical||x.protectedMain!==true||x.independentReview!==true){
       windows.push({index:i,commitSha:x.sha,reason:!canonical?'MALFORMED_ACTIVE_DESTINATION':'UNREVIEWED_DESTINATION'});
     }else{
       // Caller booleans are synthetic; independent approval remains unverified.
       windows.push({index:i,commitSha:x.sha,reason:'ACTIVE_DESTINATION_REQUIRES_EXTERNAL_PROOF'});
     }
   }
 }
 return {
   status:failures.length?'UNVERIFIABLE_HISTORY':windows.length?'HISTORICAL_POLICY_REVIEW_REQUIRED':'NO_ACTIVE_ROW_IN_PROVIDED_HISTORY',
   failures,windows,authorizesTransfer:false,completeRealGitHistoryVerified:false
 };
}
