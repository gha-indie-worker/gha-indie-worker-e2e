import {spawnSync} from "node:child_process";
import {mkdir,stat} from "node:fs/promises";
import {fileURLToPath} from "node:url";
import {resolve,dirname} from "node:path";
const root=resolve(dirname(fileURLToPath(import.meta.url)),"..");
await mkdir(resolve(root,"build"),{recursive:true});
const clang=process.env.CLANG||"clang";
const args=["--target=wasm32","-std=c11","-Oz","-ffreestanding","-fno-builtin","-nostdlib",
 "-Wl,--no-entry","-Wl,--export-memory","-Wl,--initial-memory=131072","-Wl,--max-memory=131072",
 "-Wl,--export=ores_rt_abi_version","-Wl,--export=ores_rt_initialize",
 "-Wl,--export=ores_rt_add","-Wl,--export=ores_rt_enqueue",
 "-Wl,--export=ores_rt_dequeue","-Wl,--export=ores_rt_last_message",
 "-Wl,--export=ores_rt_clock_ms",
 resolve(root,"runtime/oreslang_rt.c"),"-o",resolve(root,"build/oreslang_rt.wasm")];
const result=spawnSync(clang,args,{encoding:"utf8",stdio:"inherit"});
if(result.error){console.error(result.error.message);process.exit(1)}
if(result.status!==0)process.exit(result.status||1);
const size=(await stat(resolve(root,"build/oreslang_rt.wasm"))).size;
console.log("built real wasm32 module ("+size+" bytes)");
