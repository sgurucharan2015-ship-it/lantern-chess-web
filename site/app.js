/* Lantern Analysis Studio: native C++ search / NNUE in a dedicated WebAssembly Worker. */
(() => {
'use strict';
if (!self.crossOriginIsolated) {
 const s=document.getElementById('engineText');
 if(s) s.textContent='Enabling 3-thread browser engine…';
 const m=document.getElementById('message');
 if(m) m.textContent=window.LANTERN_ISOLATION_ERROR||'Enabling isolated browser mode. This page may reload once automatically.';
 return;
}
sessionStorage.removeItem('lantern-coi-reloads');
const START='rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
const $=id=>document.getElementById(id);
const files='abcdefgh';
const boardEl=$('board');
let fen=START, state=null, mode='both', flipped=false, selected=null, lastMove=null;
let history=[{fen:START,uci:'',san:''}], positionVersion=0, analysisVersion=0;
let ready=false, committing=false, botThinking=false, animationRunning=false, drag=null, pendingPromotion=null;
let msgIndex=0, requests=new Map();
const worker=new Worker('engine-worker.js');
worker.onmessage=evt=>{
 const d=evt.data;
 if(d.type==='ready'){
   ready=true;$('engineStatus').classList.add('ready');$('engineText').textContent='Lantern 16.4 · NNUE loaded';
   state=d.initial;renderAll();setMessage('Engine ready — play a move or choose your mode.');
   scheduleEngineAction();return;
 }
 if(d.type==='fatal'){
   ready=false;$('engineStatus').classList.add('error');$('engineText').textContent='Engine build needed';
   setMessage(d.error+' — '+(d.help||''));
   $('pvValue').textContent='The real Lantern WebAssembly files are missing or failed to initialize. See BUILD_WEB instructions.';
   for(const p of requests.values())p.reject(Error(d.error));requests.clear();return;
 }
 if(d.type==='error'&&d.requestId==null){setMessage(d.error);return;}
 const pending=requests.get(d.requestId);
 if(!pending)return;
 requests.delete(d.requestId);
 if(d.type==='error')pending.reject(Error(d.error));else pending.resolve(d);
};
worker.onerror=e=>{setMessage('Lantern worker could not start: '+e.message);$('engineStatus').classList.add('error');};
function rpc(type,more={}){
 if(!ready)return Promise.reject(Error('Lantern is still loading.'));
 return new Promise((resolve,reject)=>{
   const requestId=++msgIndex;requests.set(requestId,{resolve,reject});
   worker.postMessage({type,requestId,...more});
 });
}
const turn=()=>fen.split(' ')[1];
const toIndex=s=>files.indexOf(s[0])+8*(Number(s[1])-1);
function parseBoard(f){
 const squares=Array(64).fill('');
 const rows=f.split(' ')[0].split('/');
 rows.forEach((row,r)=>{let c=0;for(const p of row){if(/^[1-8]$/.test(p))c+=Number(p);else if(c<8){squares[(7-r)*8+c]=p;c++;}}});
 return squares;
}
const pieceColor=p=>p ? p===p.toUpperCase()?'w':'b' : '';
const imageFor=(p,check=false)=>`pieces/${pieceColor(p)}${p.toLowerCase()}${check&&p.toLowerCase()==='k'?'_check':''}.svg`;
function formatCount(n){return n>=1e6?(n/1e6).toFixed(2)+'M':n>=1e3?(n/1e3).toFixed(1)+'k':String(n??'—');}
function gameOver(){if(!state)return false;return !!(state.mate||state.stalemate||state.draw||repeated());}
function repeated(){const key=f=>f.split(' ').slice(0,4).join(' ');return history.filter(h=>key(h.fen)===key(fen)).length>=5;}
function resultText(){
 if(!state)return 'Loading…';
 if(state.mate)return `${turn()==='w'?'Black':'White'} wins by checkmate`;
 if(state.stalemate)return 'Draw by stalemate';
 if(state.draw)return 'Draw (insufficient material or move limit)';
 if(repeated())return 'Draw by repetition';
 return `${turn()==='w'?'White':'Black'} to move${state.check?' — CHECK!':''}`;
}
function isBotTurn(){return mode!=='both'&&turn()!==mode[0];}
function canHumanMove(){return ready&&state&&!committing&&!botThinking&&!animationRunning&&!gameOver()&&!isBotTurn();}
function selectedTargets(){return selected==null?[]:state.moves.filter(m=>m.slice(0,2)===selected).map(m=>m.slice(2,4));}
function renderBoard(){
 const board=parseBoard(fen), targets=selectedTargets();
 boardEl.innerHTML='';
 for(let vis=0;vis<64;vis++){
  const rank=flipped?Math.floor(vis/8):7-Math.floor(vis/8);
  const file=flipped?7-vis%8:vis%8;
  const s=files[file]+(rank+1),p=board[rank*8+file];
  const dark=(rank+file)%2===0;
  const sq=document.createElement('div');sq.className='square '+(dark?'dark':'light');sq.dataset.square=s;
  sq.setAttribute('role','gridcell');sq.setAttribute('aria-label',s+(p?' '+p:''));
  if(lastMove&&(s===lastMove.slice(0,2)||s===lastMove.slice(2,4)))sq.classList.add('last');
  if(s===selected)sq.classList.add('selected');
  if(targets.includes(s))sq.classList.add(p?'capture':'possible');
  if(p.toLowerCase()==='k'&&pieceColor(p)===turn()&&state?.check)sq.classList.add('king-check');
  if(p){const img=document.createElement('img');img.className='piece';img.src=imageFor(p,sq.classList.contains('king-check'));img.alt=(pieceColor(p)==='w'?'White ':'Black ')+({p:'pawn',r:'rook',n:'knight',b:'bishop',q:'queen',k:'king'})[p.toLowerCase()];img.draggable=false;sq.appendChild(img);}
  if(file===0){const coord=document.createElement('span');coord.className='coord rank';coord.textContent=rank+1;sq.appendChild(coord);}
  if(rank===(flipped?7:0)){const coord=document.createElement('span');coord.className='coord file';coord.textContent=files[file];sq.appendChild(coord);}
  boardEl.appendChild(sq);
 }
}
function renderMoves(){const el=$('moves');el.textContent='';if(history.length===1){const t=document.createElement('span');t.className='empty-moves';t.textContent='Play a move to begin.';el.appendChild(t);return;}
 for(let i=1;i<history.length;i+=2){
  const row=document.createElement('div');row.className='move';
  const no=document.createElement('span');no.textContent=Math.ceil(i/2)+'.';row.appendChild(no);
  for(let j=i;j<Math.min(i+2,history.length);j++){const t=document.createElement('strong');t.textContent=history[j].san;row.appendChild(t);}
  el.appendChild(row);
 }
 el.scrollTop=el.scrollHeight;
}
function renderPlayers(){
 const whiteName=mode==='black'?'Lantern':'White';const blackName=mode==='white'?'Lantern':'Black';
 $('topPlayer').textContent=flipped?whiteName:blackName;
 $('bottomPlayer').textContent=flipped?blackName:whiteName;
 $('topDescriptor').textContent=mode==='both'?'Local play':(flipped?whiteName:blackName)==='Lantern'?'Engine':'You';
 $('bottomDescriptor').textContent=mode==='both'?'Local play':(flipped?blackName:whiteName)==='Lantern'?'Engine':'You';
 $('topToMove').classList.toggle('on',(flipped?'w':'b')===turn());
 $('bottomToMove').classList.toggle('on',(flipped?'b':'w')===turn());
}
function renderAll(){renderBoard();renderMoves();renderPlayers();$('fen').value=fen;setMessage(resultText());$('undo').disabled=history.length<=1||committing||botThinking;}
function setMessage(s){$('message').textContent=s;}
function findSquareAt(x,y){return document.elementFromPoint(x,y)?.closest?.('.square')?.dataset?.square||null;}
function clearGhost(){document.querySelectorAll('.ghost').forEach(e=>e.remove());boardEl.querySelectorAll('.drag-origin').forEach(e=>e.classList.remove('drag-origin'));}
boardEl.addEventListener('pointerdown',e=>{
 if(!canHumanMove()||e.button>0)return;
 const square=e.target.closest('.square');if(!square)return;
 const source=square.dataset.square,p=parseBoard(fen)[toIndex(source)];
 const pieceIsOurs=p&&pieceColor(p)===turn();
 drag={source,x:e.clientX,y:e.clientY,piece:p||'',pieceIsOurs,moved:false,pointerId:e.pointerId};
 if(pieceIsOurs){e.preventDefault();boardEl.setPointerCapture?.(e.pointerId);}
});
boardEl.addEventListener('pointermove',e=>{
 if(!drag||!drag.pieceIsOurs||!canHumanMove())return;
 const delta=Math.hypot(e.clientX-drag.x,e.clientY-drag.y);
 if(delta<7&&!drag.moved)return;
 if(!drag.moved){drag.moved=true;selected=drag.source;renderBoard();const src=boardEl.querySelector(`[data-square="${drag.source}"]`);src?.classList.add('drag-origin');const el=document.createElement('img');el.className='ghost';el.src=imageFor(drag.piece);const size=src?.getBoundingClientRect().width||68;el.style.width=size+'px';el.style.height=size+'px';el.id='dragGhost';document.body.appendChild(el);}
 const el=$('dragGhost');if(el){el.style.left=e.clientX+'px';el.style.top=e.clientY+'px';}
});
boardEl.addEventListener('pointerup',e=>{
 if(!drag)return;
 const source=drag.source,wasDrag=drag.moved,pieceIsOurs=drag.pieceIsOurs;
 const target=findSquareAt(e.clientX,e.clientY)||source;drag=null;clearGhost();
 if(!canHumanMove())return;
 if(wasDrag&&pieceIsOurs){selected=null;const candidates=state.moves.filter(m=>m.slice(0,2)===source&&m.slice(2,4)===target);if(candidates.length)chooseMove(candidates);else renderBoard();}
 else squareClick(target);
});
boardEl.addEventListener('pointercancel',()=>{drag=null;clearGhost();renderBoard();});
function squareClick(s){
 const p=parseBoard(fen)[toIndex(s)];
 if(selected&&s!==selected){const candidates=state.moves.filter(m=>m.slice(0,2)===selected&&m.slice(2,4)===s);if(candidates.length){selected=null;chooseMove(candidates);return;}}
 if(p&&pieceColor(p)===turn())selected=(selected===s?null:s);else selected=null;
 renderBoard();
}
function chooseMove(candidates){
 if(candidates.length===1){void playMove(candidates[0]);return;}
 if(candidates.length>1){pendingPromotion=candidates;const choices=$('promotionChoices');choices.textContent='';for(const promo of ['q','r','b','n']){const m=candidates.find(s=>s.endsWith(promo));if(!m)continue;const button=document.createElement('button');const img=document.createElement('img');img.src='pieces/'+turn()+promo+'.svg';img.alt=promo;button.appendChild(img);button.addEventListener('click',()=>{$('promotionDialog').hidden=true;pendingPromotion=null;void playMove(m);});choices.appendChild(button);}$('promotionDialog').hidden=false;}
}
$('cancelPromotion').onclick=()=>{$('promotionDialog').hidden=true;pendingPromotion=null;renderBoard();};
function sanForMove(beforeFen,beforeState,m,afterState){
 const b=parseBoard(beforeFen),p=b[toIndex(m.slice(0,2))], target=b[toIndex(m.slice(2,4))];
 if(!p)return m;
 const typ=p.toUpperCase(),from=m.slice(0,2),dest=m.slice(2,4);
 if(typ==='K'&&Math.abs(files.indexOf(from[0])-files.indexOf(dest[0]))===2)return (dest[0]==='g'?'O-O':'O-O-O')+(afterState.mate?'#':afterState.check?'+':'');
 let capture=!!target || (typ==='P'&&from[0]!==dest[0]);
 let disambig='';
 if(typ!=='P'){
  const matches=beforeState.moves.filter(x=>x!==m&&x.slice(2,4)===dest&&b[toIndex(x.slice(0,2))]===p);
  if(matches.length){const sameFile=matches.some(x=>x[0]===from[0]);const sameRank=matches.some(x=>x[1]===from[1]);disambig=!sameFile?from[0]:!sameRank?from[1]:from;}
 }else if(capture)disambig=from[0];
 return (typ==='P'?'':typ)+disambig+(capture?'x':'')+dest+(m.length===5?'='+m[4].toUpperCase():'')+(afterState.mate?'#':afterState.check?'+':'');
}
async function animateMove(oldFen,m){
 const src=boardEl.querySelector(`[data-square="${m.slice(0,2)}"]`),dst=boardEl.querySelector(`[data-square="${m.slice(2,4)}"]`);
 if(!src||!dst)return;
 const p=parseBoard(oldFen)[toIndex(m.slice(0,2))];if(!p)return;
 const a=src.getBoundingClientRect(),b=dst.getBoundingClientRect();
 src.classList.add('anim-origin');
 const img=document.createElement('img');img.className='moving-piece';img.src=imageFor(p);img.style.left=a.left+'px';img.style.top=a.top+'px';img.style.width=a.width+'px';img.style.height=a.height+'px';document.body.appendChild(img);
 const dx=b.left-a.left,dy=b.top-a.top;
 // Castling: the rook glides too.
 let rook=null;
 if(p.toLowerCase()==='k'&&Math.abs(files.indexOf(m[0])-files.indexOf(m[2]))===2){
  const rank=m[1],kingSide=m[2]==='g',rookSrc=(kingSide?'h':'a')+rank,rookDest=(kingSide?'f':'d')+rank;
  const rs=boardEl.querySelector(`[data-square="${rookSrc}"]`),rt=boardEl.querySelector(`[data-square="${rookDest}"]`);
  if(rs&&rt){const ra=rs.getBoundingClientRect(),rb=rt.getBoundingClientRect();rs.classList.add('anim-origin');rook=document.createElement('img');rook.className='moving-piece';rook.src=imageFor(pieceColor(p)==='w'?'R':'r');rook.style.left=ra.left+'px';rook.style.top=ra.top+'px';rook.style.width=ra.width+'px';rook.style.height=ra.height+'px';rook.dataset.x=(rb.left-ra.left);rook.dataset.y=(rb.top-ra.top);document.body.appendChild(rook);}
 }
 await new Promise(resolve=>{requestAnimationFrame(()=>requestAnimationFrame(()=>{img.style.transform=`translate(${dx}px,${dy}px)`;if(rook)rook.style.transform=`translate(${rook.dataset.x}px,${rook.dataset.y}px)`;}));setTimeout(resolve,205);});
 img.remove();rook?.remove();
}
async function playMove(move){
 if(!state||gameOver()||committing||animationRunning)return;
 committing=true;positionVersion++;analysisVersion++;
 const oldFen=fen, oldState=state;
 try{
  const res=await rpc('move',{fen:oldFen,move});
  animationRunning=true;await animateMove(oldFen,move);animationRunning=false;
  const san=sanForMove(oldFen,oldState,move,res.data);
  fen=res.fen;state=res.data;lastMove=move;
  history.push({fen,uci:move,san});selected=null;
  renderAll();
 }catch(e){animationRunning=false;setMessage('Move failed: '+e.message);}
 finally{committing=false;}
 if(gameOver())return;
 scheduleEngineAction();
}
async function scheduleEngineAction(){
 if(!ready||!state||gameOver()||committing||botThinking)return;
 if(isBotTurn()){await botMove();return;}
 if($('autoAnalyze').checked){const current=++analysisVersion;setTimeout(()=>{if(current===analysisVersion&&!gameOver()&&!isBotTurn())void requestAnalysis();},120);}
}
async function botMove(){
 if(botThinking||committing||!ready||gameOver()||!isBotTurn())return;
 botThinking=true;const version=positionVersion, focus=fen;setMessage('Lantern is thinking…');
 try{
  const r=await rpc('search',{fen:focus,milliseconds:Number($('thinkTime').value),blend:Number($('blend').value)});
  if(version!==positionVersion||focus!==fen||!isBotTurn()){botThinking=false;return;}
  showAnalysis(r,focus);
  const m=r.bestmove;
  if(!m||m==='0000'||!state.moves.includes(m)){setMessage('Lantern returned no legal move.');botThinking=false;return;}
  botThinking=false;await playMove(m);
 }catch(e){botThinking=false;setMessage('Lantern move error: '+e.message);}
}
function showAnalysis(r,focus){
 const info=r.info;
 if(!info){$('pvValue').textContent=r.log?.slice(-400)||'No search information.';return;}
 $('depthValue').textContent=info.depth;
 $('nodesValue').textContent=formatCount(info.nodes);
 $('npsValue').textContent=formatCount(info.nps);
 const fromWhite=(focus.split(' ')[1]==='w'?1:-1);
 let score=info.scoreType==='mate'?info.score:info.score/100;
 const whiteScore=score*fromWhite;
 $('evalPOV').textContent='(White)';
 $('evalValue').textContent=(info.scoreType==='mate'?'M'+(whiteScore>0?'+':'')+whiteScore: (whiteScore>=0?'+':'')+whiteScore.toFixed(2));
 $('pvValue').textContent=info.pv || '—';
 const percent=info.scoreType==='mate'?(whiteScore>0?96:4):Math.max(4,Math.min(96,50+45*Math.tanh(whiteScore/3)));
 $('evalWhite').style.height=percent+'%';$('evalRailLabel').textContent=info.scoreType==='mate'?'M':whiteScore.toFixed(1);
}
async function requestAnalysis(){
 if(!ready||!state||gameOver()||isBotTurn()||committing)return;
 const focus=fen, v=++analysisVersion;
 $('pvValue').textContent='Analyzing…';
 try{const r=await rpc('search',{fen:focus,milliseconds:Number($('thinkTime').value),blend:Number($('blend').value)});if(v!==analysisVersion||focus!==fen)return;showAnalysis(r,focus);}
 catch(e){if(focus===fen)setMessage('Analysis error: '+e.message);}
}
async function loadNewFen(next,resetHistory=true){
 if(!ready||committing||animationRunning||botThinking)return;
 positionVersion++;analysisVersion++;
 try{
  const r=await rpc('state',{fen:next});
  if(r.data.error)throw Error(r.data.error);
  fen=next;state=r.data;selected=null;lastMove=null;
  if(resetHistory)history=[{fen:next,uci:'',san:''}];
  renderAll();scheduleEngineAction();
 }catch(e){setMessage('Could not load position: '+e.message);}
}
$('newGame').onclick=()=>void loadNewFen(START);
$('loadFen').onclick=()=>void loadNewFen($('fen').value.trim());
$('undo').onclick=()=>{if(!ready||botThinking||committing||history.length<2)return;let n=1;if(mode!=='both'&&history.length>2)n=2;history.splice(-n);void loadNewFen(history.at(-1).fen,false);};
$('flip').onclick=()=>{flipped=!flipped;renderBoard();renderPlayers();};
$('copyFen').onclick=async()=>{await navigator.clipboard.writeText(fen).catch(()=>{});setMessage('FEN copied.');};
$('copyPgn').onclick=async()=>{const text=history.slice(1).map((x,i)=>(i%2===0?(Math.ceil((i+1)/2))+'. ':'')+x.san).join(' ');await navigator.clipboard.writeText(text).catch(()=>{});setMessage('Moves copied.');};
$('analyze').onclick=()=>void requestAnalysis();
$('thinkTime').oninput=()=>{$('timeLabel').textContent=$('thinkTime').value+' ms';};
$('blend').oninput=()=>{$('blendLabel').textContent='H'+$('blend').value;};
document.querySelectorAll('.mode').forEach(btn=>btn.addEventListener('click',()=>{
 if(committing||botThinking)return;
 document.querySelectorAll('.mode').forEach(x=>x.classList.remove('active'));
 btn.classList.add('active');mode=btn.dataset.mode;
 // Don't allow an engine move to continue from stale analysis after a mode switch.
 positionVersion++;analysisVersion++;renderPlayers();scheduleEngineAction();
}));
$('autoAnalyze').onchange=()=>{if($('autoAnalyze').checked)void scheduleEngineAction();};
})();
