/* Every calculation happens inside the visitor's browser. No HTTP engine API. */
let mod = null;
const toText = (name, types, values) => mod.ccall(name, 'string', types, values);
const START = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
const send = (type, data = {}) => self.postMessage({type, ...data});
async function boot(){
  try {
        /* LANTERN_ABSOLUTE_ASSET_PATHS_V1 */
    const asset = name => new URL(name, self.location.href).href;
    send('bootstatus',{message:'Loading Lantern engine and NNUEâ€¦'});
    importScripts(asset('lantern.js'));
    mod=await LanternWasm({locateFile: asset, printErr: (msg)=>send('bootstatus',{message:'Engine: '+String(msg).slice(0,180)})});
    const result=toText('lantern_web_init',[],[]);
    if (!result.startsWith('OK:')) throw new Error(result);
    send('ready',{message:result, initial:JSON.parse(toText('lantern_web_state',['string'],[START]))});
  } catch(e){send('fatal',{error:String(e),help:'The cloud build may have failed or required WebAssembly files are missing. Check GitHub Actions logs.'});}
}
self.onmessage = event => {
  const r=event.data;
  if(!mod){send('error',{requestId:r.requestId,error:'Engine not ready'});return;}
  try {
    if(r.type==='state'){
      const data=JSON.parse(toText('lantern_web_state',['string'],[r.fen]));
      send('state',{requestId:r.requestId, data});
    } else if(r.type==='move') {
      const fen=toText('lantern_web_move',['string','string'],[r.fen,r.move]);
      if(fen.startsWith('ERROR:')) throw Error(fen);
      const data=JSON.parse(toText('lantern_web_state',['string'],[fen]));
      send('move',{requestId:r.requestId,fen,move:r.move,data});
    } else if(r.type==='search') {
      const log=toText('lantern_web_search',['string','number','number'],[r.fen,r.milliseconds,r.blend]);
      if(log.startsWith('ERROR:')) throw Error(log);
      const best=/^bestmove\s+(\S+)/m.exec(log)?.[1] || '0000';
      const info=[...log.matchAll(/^info depth (\d+) .*?score (cp|mate) (-?\d+) .*?nodes (\d+) .*?nps (\d+).*?pv (.+)$/gm)];
      const last=info.length?info[info.length-1]:null;
      send('search',{requestId:r.requestId, bestmove:best, log, info:last?{depth:+last[1],scoreType:last[2],score:+last[3],nodes:+last[4],nps:+last[5],pv:last[6].trim()}:null});
    }
  } catch(e){send('error',{requestId:r.requestId,error:String(e)});}
};
boot();
