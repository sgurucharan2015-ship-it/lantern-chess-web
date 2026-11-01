/* Lantern's ACTUAL compiled C++ + NNUE engine runs here; no substitute engine. */
let mod=null;
const send=(type,data={})=>self.postMessage({type,...data});
const asset=name=>new URL(name+'?v=repair3',self.location.href).href;
const call=(name,types=[],values=[])=>mod.ccall(name,'string',types,values);
async function boot(){
 try{
  send('bootstatus',{message:'Loading Lantern 16.4 (compatible WASM)…'});
  importScripts(asset('lantern.js'));
  if(typeof LanternWasm!=='function')throw Error('LanternWasm loader not found');
  // CRITICAL: in a Web Worker, document.currentScript is undefined.
  // PThread builds therefore require mainScriptUrlOrBlob, otherwise
  // Emscripten tries new Worker(undefined) and throws [object Event].
  mod=await LanternWasm({
    locateFile: asset,
    mainScriptUrlOrBlob:asset('lantern.js'),
    printErr: msg => console.error('Lantern C++:',msg),
    onAbort: why=>send('bootstatus',{message:'Lantern aborted: '+String(why).slice(0,160)})
  });
  const result=call('lantern_web_init');
  if(!result.startsWith('OK:'))throw Error(result);
  const fen='rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
  send('ready',{message:result,initial:JSON.parse(call('lantern_web_state',['string'],[fen]))});
 }catch(e){
  const text=e?.stack||e?.message||String(e);
  console.error('Lantern engine worker initialization failed:',e);
  send('fatal',{error:text,help:'The board remains usable for two-sided play. The real Lantern engine could not initialize.'});
 }
}
self.onmessage=e=>{
 const r=e.data;
 if(!mod){send('error',{requestId:r.requestId,error:'Lantern engine is not ready'});return;}
 try{
  if(r.type==='search'){
   const log=call('lantern_web_search',['string','number','number'],[r.fen,r.milliseconds,r.blend]);
   if(log.startsWith('ERROR:'))throw Error(log);
   const best=/^bestmove\s+(\S+)/m.exec(log)?.[1]||'0000';
   const info=[...log.matchAll(/^info depth (\d+) .*?score (cp|mate) (-?\d+) .*?nodes (\d+) .*?nps (\d+).*?pv (.+)$/gm)].at(-1);
   send('search',{requestId:r.requestId,bestmove:best,log,
     info:info?{depth:+info[1],scoreType:info[2],score:+info[3],nodes:+info[4],nps:+info[5],pv:info[6].trim()}:null});
  }
 }catch(e){send('error',{requestId:r.requestId,error:e?.message||String(e)});}
};
boot();
