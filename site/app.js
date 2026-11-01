/* Lantern Studio: browser-local legal chess play, with the ORIGINAL Lantern C++ WASM
 * engine as an optional opponent/analyzer. A failed engine MUST NOT freeze the board.
 * Chess.js 0.10.3 (MIT) is vendored during GitHub Actions. */
(() => {
'use strict';
const $ = id => document.getElementById(id);
const START='rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
const files='abcdefgh';
const squares=$('board');
if(typeof Chess!=='function') {
  $('engineText').textContent='Chess rules failed to load';
  $('message').textContent='Missing chess.js. The site build must include chess.js.';
  return;
}
let game=new Chess(), mode='both', flipped=false, selected=null, lastMove=null;
let busy=false, drag=null, engineReady=false, engineFailed=false, engineWorker=null;
let requests=new Map(), requestId=0, version=0, timer=null, engineBusy=false;
const imgPath=(p,highlight=false)=>`pieces/${p.color}${p.type}${highlight&&p.type==='k'?'_check':''}.svg`;
const toSquare=e=>e.target?.closest?.('.square')?.dataset?.square||null;
const isBotTurn=()=>mode!=='both' && game.turn()!==mode[0];
const canHuman=()=>!busy && !game.game_over() && !isBotTurn();
const legalFrom=s=>game.moves({square:s,verbose:true});
function setMsg(s){$('message').textContent=s;}
function endText(){
 if(game.in_checkmate())return `Checkmate — ${game.turn()==='w'?'Black':'White'} wins!`;
 if(game.in_stalemate())return 'Draw — stalemate';
 if(game.in_threefold_repetition())return 'Draw — threefold repetition';
 if(game.insufficient_material())return 'Draw — insufficient material';
 if(game.in_draw())return 'Draw';
 return `${game.turn()==='w'?'White':'Black'} to move${game.in_check()?' — CHECK!':''}`;
}
function renderBoard(){
 const targets=selected?legalFrom(selected):[];
 squares.textContent='';
 for(let n=0;n<64;n++) {
   const rank=flipped?Math.floor(n/8):7-Math.floor(n/8);
   const file=flipped?7-n%8:n%8;
   const loc=files[file]+(rank+1), p=game.get(loc);
   const s=document.createElement('div');
   s.className='square '+((rank+file)%2===0?'dark':'light');
   s.dataset.square=loc;s.setAttribute('role','gridcell');
   if(lastMove&&(lastMove.from===loc||lastMove.to===loc))s.classList.add('last');
   if(selected===loc)s.classList.add('selected');
   if(targets.some(m=>m.to===loc))s.classList.add(p?'capture':'possible');
   const inCheck=p?.type==='k'&&p.color===game.turn()&&game.in_check();
   if(inCheck)s.classList.add('king-check');
   if(p){const img=document.createElement('img');img.className='piece';img.src=imgPath(p,inCheck);img.alt=(p.color==='w'?'White ':'Black ')+p.type;img.draggable=false;s.appendChild(img);}
   if(file===0){const a=document.createElement('span');a.className='coord rank';a.textContent=rank+1;s.appendChild(a);}
   if(rank===(flipped?7:0)){const a=document.createElement('span');a.className='coord file';a.textContent=files[file];s.appendChild(a);}
   squares.appendChild(s);
 }
}
function renderMoves(){
 const el=$('moves');el.textContent='';const hist=game.history();
 if(!hist.length){let x=document.createElement('span');x.className='empty-moves';x.textContent='Play a move to begin.';el.appendChild(x);return;}
 for(let i=0;i<hist.length;i+=2){const row=document.createElement('div');row.className='move';const number=document.createElement('span');number.textContent=Math.floor(i/2)+1+'.';row.appendChild(number);
  for(let j=i;j<Math.min(i+2,hist.length);j++){const move=document.createElement('strong');move.textContent=hist[j];row.appendChild(move);}el.appendChild(row);}
 el.scrollTop=el.scrollHeight;
}
function renderPlayers(){
 const white=mode==='black'?'Lantern':'White',black=mode==='white'?'Lantern':'Black';
 $('topPlayer').textContent=flipped?white:black;
 $('bottomPlayer').textContent=flipped?black:white;
 $('topDescriptor').textContent=mode==='both'?'Local play':(flipped?white:black)==='Lantern'?'Engine':'You';
 $('bottomDescriptor').textContent=mode==='both'?'Local play':(flipped?black:white)==='Lantern'?'Engine':'You';
 $('topToMove').classList.toggle('on',game.turn()===(flipped?'w':'b'));
 $('bottomToMove').classList.toggle('on',game.turn()===(flipped?'b':'w'));
}
function renderAll(){renderBoard();renderMoves();renderPlayers();$('fen').value=game.fen();$('undo').disabled=busy||game.history().length===0;setMsg(endText() + (!engineReady&&mode!=='both'?' · Lantern engine is loading.':''));}
function clearGhost(){document.querySelectorAll('.ghost').forEach(e=>e.remove());document.querySelectorAll('.drag-origin').forEach(e=>e.classList.remove('drag-origin'));}
function squareClick(dest){
 if(!canHuman())return;
 if(selected){const candidates=legalFrom(selected).filter(m=>m.to===dest);if(candidates.length){choose(candidates);return;}}
 const p=game.get(dest);selected=p?.color===game.turn()?(selected===dest?null:dest):null;renderBoard();
}
function choose(moves){
 if(moves.length===1){void playMove(moves[0]);return;}
 const dialog=$('promotionDialog'), container=$('promotionChoices');
 container.textContent='';
 for(const t of ['q','r','b','n']) {
  const m=moves.find(x=>x.promotion===t);if(!m)continue;
  const btn=document.createElement('button'),img=document.createElement('img');
  img.src=`pieces/${game.turn()}${t}.svg`;img.alt=t.toUpperCase();btn.appendChild(img);
  btn.onclick=()=>{dialog.hidden=true;void playMove(m);};container.appendChild(btn);
 }
 dialog.hidden=false;
}
$('cancelPromotion').onclick=()=>{$('promotionDialog').hidden=true;renderBoard();};
squares.addEventListener('pointerdown',e=>{
 if(!canHuman()||e.button!==0)return;
 const source=toSquare(e),piece=source?game.get(source):null;
 if(!source)return;
 drag={source,x:e.clientX,y:e.clientY,piece,validPiece:piece?.color===game.turn(),moved:false};
 if(drag.validPiece){try{squares.setPointerCapture(e.pointerId);}catch{}}
});
squares.addEventListener('pointermove',e=>{
 if(!drag?.validPiece||!canHuman())return;
 if(!drag.moved && Math.hypot(e.clientX-drag.x,e.clientY-drag.y)<7)return;
 if(!drag.moved){drag.moved=true;selected=drag.source;renderBoard();
  squares.querySelector(`[data-square="${drag.source}"]`)?.classList.add('drag-origin');
  const ghost=document.createElement('img');ghost.id='lanternDragGhost';ghost.className='ghost';ghost.src=imgPath(drag.piece);
  const w=squares.getBoundingClientRect().width/8;ghost.style.width=w+'px';ghost.style.height=w+'px';document.body.appendChild(ghost);
 }
 const ghost=$('lanternDragGhost');if(ghost){ghost.style.left=e.clientX+'px';ghost.style.top=e.clientY+'px';}
});
squares.addEventListener('pointerup',e=>{
 if(!drag)return;
 const d=drag;drag=null;
 const dest=document.elementFromPoint(e.clientX,e.clientY)?.closest?.('.square')?.dataset.square || (d.moved?null:d.source);
 clearGhost();
 if(!canHuman())return;
 if(d.moved){selected=null;if(dest){const candidates=legalFrom(d.source).filter(m=>m.to===dest);if(candidates.length){choose(candidates);return;}}renderBoard();}
 else if(dest)squareClick(dest);
});
squares.addEventListener('pointercancel',()=>{drag=null;clearGhost();renderBoard();});
async function animate(oldMove){
 const a=squares.querySelector(`[data-square="${oldMove.from}"]`), b=squares.querySelector(`[data-square="${oldMove.to}"]`);
 if(!a||!b)return;
 const from=a.getBoundingClientRect(),to=b.getBoundingClientRect();
 const p={color:oldMove.color,type:oldMove.piece};
 const img=document.createElement('img');img.className='moving-piece';img.src=imgPath(p);img.style.cssText+=`;left:${from.left}px;top:${from.top}px;width:${from.width}px;height:${from.height}px`;
 a.classList.add('anim-origin');document.body.appendChild(img);
 let rook=null;
 if(oldMove.flags.includes('k')||oldMove.flags.includes('q')){
  const rank=oldMove.color==='w'?'1':'8',rs=(oldMove.flags.includes('k')?'h':'a')+rank,rt=(oldMove.flags.includes('k')?'f':'d')+rank;
  const ra=squares.querySelector(`[data-square="${rs}"]`),rb=squares.querySelector(`[data-square="${rt}"]`);
  if(ra&&rb){const q=ra.getBoundingClientRect(),w=rb.getBoundingClientRect();ra.classList.add('anim-origin');rook=document.createElement('img');rook.className='moving-piece';rook.src=imgPath({color:oldMove.color,type:'r'});rook.style.cssText+=`;left:${q.left}px;top:${q.top}px;width:${q.width}px;height:${q.height}px`;document.body.appendChild(rook);rook.dataset.dx=String(w.left-q.left);rook.dataset.dy=String(w.top-q.top);}
 }
 await new Promise(resolve=>{requestAnimationFrame(()=>requestAnimationFrame(()=>{
  img.style.transform=`translate(${to.left-from.left}px,${to.top-from.top}px)`;
  if(rook)rook.style.transform=`translate(${rook.dataset.dx}px,${rook.dataset.dy}px)`;
 }));setTimeout(resolve,205);});
 img.remove();rook?.remove();
}
async function playMove(move){
 if(busy||game.game_over())return;
 const origin=game.fen();busy=true;version++;
 const played=game.move({from:move.from,to:move.to,promotion:move.promotion||'q'});
 if(!played){busy=false;return;}
 try{await animate(played);}catch(e){console.warn('animation skipped',e);}
 lastMove=played;selected=null;busy=false;renderAll();queueNext();
}
function rpc(type,data={}){
 if(!engineReady)return Promise.reject(Error('Lantern engine is not ready'));
 return new Promise((resolve,reject)=>{const id=++requestId;requests.set(id,{resolve,reject});
  engineWorker.postMessage({type,requestId:id,...data});
 });
}
function queueNext(){
 clearTimeout(timer);timer=null;
 if(game.game_over())return;
 if(isBotTurn()){
  if(engineReady)timer=setTimeout(()=>void botMove(),200);
  else setMsg('Lantern is loading. Your board is still playable in Both sides mode.');
 }else if($('autoAnalyze').checked&&engineReady)timer=setTimeout(()=>void analyze(),250);
}
function showAnalysis(r,focus){
 const i=r.info;if(!i){$('pvValue').textContent=r.log?.slice(-400)||'No engine PV';return;}
 $('depthValue').textContent=i.depth;$('nodesValue').textContent=compact(i.nodes);$('npsValue').textContent=compact(i.nps);
 const white=(focus.split(' ')[1]==='w'?1:-1), score=(i.scoreType==='mate'?i.score:i.score/100)*white;
 $('evalValue').textContent=i.scoreType==='mate'?'M'+(score>0?'+':'')+score:(score>=0?'+':'')+score.toFixed(2);
 $('pvValue').textContent=i.pv||'—';
 const pct=i.scoreType==='mate'?(score>0?96:4):Math.max(4,Math.min(96,50+45*Math.tanh(score/3)));
 $('evalWhite').style.height=pct+'%';$('evalRailLabel').textContent=i.scoreType==='mate'?'M':score.toFixed(1);
}
const compact=n=>n>=1e6?(n/1e6).toFixed(2)+'M':n>=1e3?(n/1e3).toFixed(1)+'k':String(n??'—');
async function analyze(){
 if(!engineReady){setMsg('Lantern engine is not ready. Both-sides chess play remains available.');return;}
 if(game.game_over()){setMsg(endText());return;}
 const focus=game.fen(), v=version;
 $('pvValue').textContent='Analyzing…';
 try{const r=await rpc('search',{fen:focus,milliseconds:Number($('thinkTime').value),blend:Number($('blend').value)});if(game.fen()===focus&&v===version)showAnalysis(r,focus);}
 catch(e){setMsg('Lantern analysis error: '+e.message);}
}
async function botMove(){
 if(!engineReady||busy||!isBotTurn()||game.game_over())return;
 engineBusy=true;const focus=game.fen(),v=version;setMsg('Lantern thinking…');
 try{const r=await rpc('search',{fen:focus,milliseconds:Number($('thinkTime').value),blend:Number($('blend').value)});
  if(focus!==game.fen()||v!==version||!isBotTurn())return;
  showAnalysis(r,focus);const found=game.moves({verbose:true}).find(m=>m.from+m.to+(m.promotion||'')===r.bestmove);
  if(!found){setMsg('Lantern did not return a legal move: '+String(r.bestmove));return;}
  await playMove(found);
 }catch(e){setMsg('Lantern move failed: '+e.message);}finally{engineBusy=false;}
}
function bootEngine(){
 try{engineWorker=new Worker('engine-worker.js?v=repair3');}
 catch(e){engineFailed=true;$('engineText').textContent='Lantern unavailable';setMsg('Engine worker creation failed: '+e.message+'. You can still play both sides.');return;}
 engineWorker.onmessage=event=>{
  const d=event.data;
  if(d.type==='bootstatus'){$('engineText').textContent=d.message;return;}
  if(d.type==='ready'){engineReady=true;engineFailed=false;$('engineStatus').classList.add('ready');$('engineText').textContent='Lantern 16.4 · H50 NNUE ready';setMsg(endText());queueNext();return;}
  if(d.type==='fatal'){engineFailed=true;$('engineStatus').classList.add('error');$('engineText').textContent='Lantern engine unavailable';setMsg('Lantern startup error: '+d.error+' — Both sides still works.');return;}
  const pending=requests.get(d.requestId);if(!pending)return;requests.delete(d.requestId);
  if(d.type==='error')pending.reject(Error(d.error));else pending.resolve(d);
 };
 engineWorker.onerror=e=>{engineReady=false;engineFailed=true;$('engineStatus').classList.add('error');$('engineText').textContent='Lantern worker error';setMsg('Lantern worker error: '+(e.message||e.filename+':'+e.lineno||'unknown')+'. Two-player mode remains available.');console.error('Lantern worker error',e);};
}
$('newGame').onclick=()=>{version++;game=new Chess();selected=null;lastMove=null;busy=false;renderAll();queueNext();};
$('undo').onclick=()=>{if(busy)return;version++;game.undo();if(mode!=='both'&&game.history().length>0&&isBotTurn())game.undo();selected=null;lastMove=null;renderAll();queueNext();};
$('flip').onclick=()=>{flipped=!flipped;renderBoard();renderPlayers();};
$('copyFen').onclick=()=>{navigator.clipboard?.writeText(game.fen()).catch(()=>{});setMsg('FEN copied.');};
$('copyPgn').onclick=()=>{navigator.clipboard?.writeText(game.pgn()).catch(()=>{});setMsg('Moves copied.');};
$('loadFen').onclick=()=>{const next=$('fen').value.trim(),temp=new Chess();if(!temp.load(next)){setMsg('Invalid FEN — position unchanged.');return;}version++;game=temp;selected=null;lastMove=null;renderAll();queueNext();};
$('analyze').onclick=()=>void analyze();
$('thinkTime').oninput=()=>{$('timeLabel').textContent=$('thinkTime').value+' ms';};
$('blend').oninput=()=>{$('blendLabel').textContent='H'+$('blend').value;};
document.querySelectorAll('.mode').forEach(btn=>btn.onclick=()=>{version++;mode=btn.dataset.mode;selected=null;if(mode==='black')flipped=true;else if(mode==='white')flipped=false;
 document.querySelectorAll('.mode').forEach(b=>b.classList.toggle('active',b===btn));renderAll();queueNext();});
$('autoAnalyze').onchange=queueNext;
renderAll(); // Absolutely no engine dependency: users can play immediately.
bootEngine();
})();
