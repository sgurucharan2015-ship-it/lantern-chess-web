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
// Analysis is optional: the board stays playable even if the WASM engine fails.
const reviews=[], evalCache=new Map();
let bestArrow=null, reviewGeneration=0, reviewing=false;
let showSuggestions=true, showLastArrow=true, selectedReview=-1;
const RATING={brilliant:['!!','Brilliant','#27c8db'],great:['!','Great','#58a7f9'],best:['★','Best','#8aca63'],excellent:['✓','Excellent','#65b774'],good:['✓','Good','#98b36b'],inaccuracy:['?!','Inaccuracy','#e2c15a'],mistake:['?','Mistake','#ed9649'],blunder:['??','Blunder','#eb5c65'],forced:['·','Forced','#96a0ac']};
function initAnalyzerUI(){
 const css=document.createElement('style');css.textContent=`
 #board{position:relative!important;isolation:isolate}
 #lanternArrows{position:absolute;inset:0;width:100%;height:100%;z-index:8;pointer-events:none;overflow:hidden}
 #lanternAnalysisPanel{background:rgba(100,120,140,.09);border:1px solid rgba(130,150,170,.25);border-radius:14px;padding:12px;margin-top:12px;color:inherit;font:inherit}
 #lanternAnalysisPanel .la-title{font-weight:800;font-size:1.05rem;margin-bottom:8px}
 #lanternAnalysisPanel .la-row{display:flex;justify-content:space-between;gap:10px;align-items:center;margin:7px 0;flex-wrap:wrap}
 #lanternAnalysisPanel .la-muted{opacity:.7;font-size:.86rem}
 #lanternAnalysisPanel .la-controls{display:flex;gap:12px;flex-wrap:wrap;font-size:.88rem;margin:9px 0}
 #lanternAnalysisPanel label{display:inline-flex;gap:5px;align-items:center;cursor:pointer}
 #lanternAnalysisPanel .la-badge{font-weight:900;border-radius:6px;padding:3px 9px;background:rgba(100,120,140,.14)}
 #lanternAnalysisPanel .la-summary{display:flex;gap:7px;flex-wrap:wrap;margin-top:9px}
 #lanternAnalysisPanel .la-count{padding:4px 7px;border:1px solid rgba(150,150,150,.22);border-radius:7px;font-size:.83rem}
 #moves .lantern-annotated{cursor:pointer;display:inline-flex;align-items:center;gap:4px;border-radius:5px;padding:2px 4px}
 #moves .lantern-annotated:hover,#moves .lantern-review-selected{background:rgba(125,150,200,.18)}
 #moves .lantern-mark{font-weight:900;font-size:.85em}
 #board .lantern-square-badge{position:absolute;right:1px;bottom:1px;z-index:6;min-width:17px;height:17px;line-height:17px;text-align:center;font-size:12px;font-weight:900;border-radius:50%;background:#222e;color:white;pointer-events:none}
 #board .square{position:relative}
 `;document.head.appendChild(css);
 const panel=document.createElement('section');panel.id='lanternAnalysisPanel';panel.setAttribute('aria-label','Lantern analysis');
 panel.innerHTML='<div class="la-title">♟ Lantern Game Review</div><div class="la-controls"><label><input type="checkbox" id="laSuggestions" checked> Best-move arrows</label><label><input type="checkbox" id="laLast" checked> Last-move arrow</label></div><div class="la-row"><span>Suggested move</span><strong id="laBest">Waiting for analysis</strong></div><div class="la-row"><span>Move quality</span><span class="la-badge" id="laRating">Not reviewed</span></div><div class="la-row"><span>Eval loss</span><strong id="laLoss">—</strong></div><div class="la-muted" id="laExplanation">Moves are classified using Lantern evaluations; results are estimates, not Chess.com labels.</div><div class="la-summary" id="laSummary"></div>';
 const moves=$('moves');(moves?.parentElement||squares.parentElement).appendChild(panel);
 $('laSuggestions').onchange=e=>{showSuggestions=e.target.checked;drawArrows();};
 $('laLast').onchange=e=>{showLastArrow=e.target.checked;drawArrows();};
}
function arrowPoint(square){const file=files.indexOf(square[0]),rank=Number(square[1])-1;if(file<0||rank<0||rank>7)return null;return {x:(flipped?7-file:file)*12.5+6.25,y:(flipped?rank:7-rank)*12.5+6.25};}
function drawArrows(){
 squares.querySelector('#lanternArrows')?.remove();
 const arrows=[];
 if(showLastArrow&&lastMove)arrows.push({uci:lastMove.from+lastMove.to,color:'#e8b547',alpha:.47});
 if(showSuggestions&&bestArrow&&bestArrow.fen===game.fen())arrows.push({uci:bestArrow.uci,color:'#24c96b',alpha:.85});
 if(!arrows.length)return;
 const ns='http://www.w3.org/2000/svg',svg=document.createElementNS(ns,'svg');svg.id='lanternArrows';svg.setAttribute('viewBox','0 0 100 100');svg.setAttribute('preserveAspectRatio','none');
 const defs=document.createElementNS(ns,'defs');svg.appendChild(defs);
 arrows.forEach((a,n)=>{
  const p=arrowPoint(a.uci.slice(0,2)),q=arrowPoint(a.uci.slice(2,4));if(!p||!q)return;
  const id='la-head-'+n,marker=document.createElementNS(ns,'marker');marker.setAttribute('id',id);marker.setAttribute('markerWidth','4');marker.setAttribute('markerHeight','4');marker.setAttribute('refX','3');marker.setAttribute('refY','2');marker.setAttribute('orient','auto');marker.setAttribute('markerUnits','strokeWidth');
  const path=document.createElementNS(ns,'path');path.setAttribute('d','M0 0 L4 2 L0 4 Z');path.setAttribute('fill',a.color);marker.appendChild(path);defs.appendChild(marker);
  const line=document.createElementNS(ns,'line');line.setAttribute('x1',p.x);line.setAttribute('y1',p.y);const dx=q.x-p.x,dy=q.y-p.y,l=Math.hypot(dx,dy)||1;line.setAttribute('x2',q.x-dx/l*2.1);line.setAttribute('y2',q.y-dy/l*2.1);line.setAttribute('stroke',a.color);line.setAttribute('stroke-width','1.7');line.setAttribute('stroke-opacity',a.alpha);line.setAttribute('stroke-linecap','round');line.setAttribute('marker-end','url(#'+id+')');svg.appendChild(line);
 });squares.appendChild(svg);
}
function evalFromResult(r,fen){
 const i=r?.info;if(!i||!Number.isFinite(Number(i.score)))return null;
 const n=Number(i.score),white=fen.split(' ')[1]==='w'?1:-1;
 return (i.scoreType==='mate'?Math.sign(n)*Math.max(7,20-Math.abs(n)*.25):n/100)*white;
}
function rankFromLoss(loss,forced){if(forced)return 'forced';if(loss<=.15)return 'best';if(loss<=.35)return 'excellent';if(loss<=.75)return 'good';if(loss<=1.5)return 'inaccuracy';if(loss<=3)return 'mistake';return 'blunder';}
function formatEval(n){return Number.isFinite(n)?(n>=0?'+':'')+n.toFixed(2):'—';}
function recordReview(i,review){reviews[i]=review;selectedReview=i;renderMoves();renderReview();}
function renderReview(){
 const last=selectedReview>=0?selectedReview:reviews.length-1,r=reviews[last];
 const badge=$('laRating'),loss=$('laLoss'),expl=$('laExplanation');if(!badge)return;
 if(!r){badge.textContent=reviewing?'Analyzing…':'Not reviewed';badge.style.color='';loss.textContent='—';expl.textContent='Analysis labels are approximate and are based on engine evaluation loss.';}
 else {const item=RATING[r.rating]||RATING.good;badge.textContent=item[0]+' '+item[1];badge.style.color=item[2];loss.textContent=r.loss.toFixed(2)+' pawns';expl.textContent=`${r.san}: ${r.rating==='forced'?'Only legal move.':r.rating==='best'?'Matches or closely follows Lantern’s best evaluation.':'Best alternative: '+(r.bestSan||r.bestUci||'unknown')+'.'} Before ${formatEval(r.bestEval)}, after ${formatEval(r.playedEval)} (White perspective).`;}
 const c={};for(const x of reviews)if(x)c[x.rating]=(c[x.rating]||0)+1;
 $('laSummary').textContent='';for(const [k,n] of Object.entries(c)){const el=document.createElement('span');el.className='la-count';el.style.color=RATING[k]?.[2]||'';el.textContent=(RATING[k]?.[0]||'')+' '+k+' '+n;$('laSummary').appendChild(el);}
}
function resetReviews(){reviewGeneration++;reviews.length=0;evalCache.clear();selectedReview=-1;bestArrow=null;reviewing=false;$('laBest').textContent='Waiting for analysis';renderReview();drawArrows();}
function moveSanAtFen(fen,uci){const t=new Chess(fen),m=t.moves({verbose:true}).find(x=>x.from+x.to+(x.promotion||'')===uci);return m?.san||uci;}
async function searchPosition(fen){const cached=evalCache.get(fen);if(cached)return cached;const r=await rpc('search',{fen,milliseconds:Number($('thinkTime').value),blend:Number($('blend').value)});if(r?.info)evalCache.set(fen,r);return r;}
async function reviewPly(ply,beforeFen,afterFen,played,gen){
 if(!engineReady)return;reviewing=true;renderReview();
 try{
  const before=await searchPosition(beforeFen);if(gen!==reviewGeneration)return;
  const after=await searchPosition(afterFen);if(gen!==reviewGeneration)return;
  const a=evalFromResult(before,beforeFen),b=evalFromResult(after,afterFen);
  if(a===null||b===null)return;
  const mover=played.color==='w'?1:-1,loss=Math.max(0,Math.min(30,(a-b)*mover));
  const moves=new Chess(beforeFen).moves();
  recordReview(ply,{san:played.san,loss,rating:rankFromLoss(loss,moves.length===1),bestEval:a,playedEval:b,bestUci:before.bestmove,bestSan:moveSanAtFen(beforeFen,before.bestmove||'')});renderBoard();
  if(game.fen()===afterFen){showAnalysis(after,afterFen);bestArrow=after.bestmove?{fen:afterFen,uci:after.bestmove}:null;drawArrows();}
 }catch(e){console.warn('Move review skipped:',e);}finally{if(gen===reviewGeneration){reviewing=false;renderReview();}}
}

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
   const lastIndex=game.history().length-1,rr=reviews[lastIndex];
   if(lastMove?.to===loc&&rr){const badge=document.createElement('span');badge.className='lantern-square-badge';badge.textContent=RATING[rr.rating]?.[0]||'';badge.style.background=RATING[rr.rating]?.[2]||'#555';s.appendChild(badge);}
   squares.appendChild(s);
 }
 drawArrows();
}
function renderMoves(){
 const el=$('moves');el.textContent='';const hist=game.history();
 if(!hist.length){let x=document.createElement('span');x.className='empty-moves';x.textContent='Play a move to begin.';el.appendChild(x);return;}
 for(let i=0;i<hist.length;i+=2){const row=document.createElement('div');row.className='move';const number=document.createElement('span');number.textContent=Math.floor(i/2)+1+'.';row.appendChild(number);
  for(let j=i;j<Math.min(i+2,hist.length);j++){const move=document.createElement('strong');move.textContent=hist[j];move.className='lantern-annotated';const r=reviews[j];if(r){const m=document.createElement('span');m.className='lantern-mark';m.style.color=RATING[r.rating]?.[2]||'';m.textContent=RATING[r.rating]?.[0]||'';move.appendChild(m);}move.insertBefore(document.createTextNode(hist[j]+' '),move.firstChild);if(selectedReview===j)move.classList.add('lantern-review-selected');move.onclick=()=>{selectedReview=j;renderReview();renderMoves();};row.appendChild(move);}el.appendChild(row);}
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
 const origin=game.fen();busy=true;version++;bestArrow=null;
 const played=game.move({from:move.from,to:move.to,promotion:move.promotion||'q'});
 if(!played){busy=false;return;}
 try{await animate(played);}catch(e){console.warn('animation skipped',e);}
 lastMove=played;selected=null;busy=false;renderAll();
 const ply=game.history().length-1, gen=reviewGeneration, after=game.fen();
 // Keep engine searches serialized: reviewing cannot interfere with an opponent turn.
 if(engineReady){await reviewPly(ply,origin,after,played,gen);}
 if(gen===reviewGeneration)queueNext();
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
 if(reviewing)return;
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
 if(focus===game.fen()){$('laBest').textContent=r.bestmove?moveSanAtFen(focus,r.bestmove):'—';bestArrow=r.bestmove?{fen:focus,uci:r.bestmove}:null;drawArrows();}
 const pct=i.scoreType==='mate'?(score>0?96:4):Math.max(4,Math.min(96,50+45*Math.tanh(score/3)));
 $('evalWhite').style.height=pct+'%';$('evalRailLabel').textContent=i.scoreType==='mate'?'M':score.toFixed(1);
}
const compact=n=>n>=1e6?(n/1e6).toFixed(2)+'M':n>=1e3?(n/1e3).toFixed(1)+'k':String(n??'—');
async function analyze(){
 if(!engineReady){setMsg('Lantern engine is not ready. Both-sides chess play remains available.');return;}
 if(game.game_over()){setMsg(endText());return;}
 const focus=game.fen(), v=version;
 $('pvValue').textContent='Analyzing…';
 try{const r=await searchPosition(focus);if(game.fen()===focus&&v===version)showAnalysis(r,focus);}
 catch(e){setMsg('Lantern analysis error: '+e.message);}
}
async function botMove(){
 if(!engineReady||busy||!isBotTurn()||game.game_over())return;
 engineBusy=true;const focus=game.fen(),v=version;setMsg('Lantern thinking…');
 try{const r=await searchPosition(focus);
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
$('newGame').onclick=()=>{version++;resetReviews();game=new Chess();selected=null;lastMove=null;busy=false;renderAll();queueNext();};
$('undo').onclick=()=>{if(busy)return;version++;resetReviews();game.undo();if(mode!=='both'&&game.history().length>0&&isBotTurn())game.undo();selected=null;lastMove=null;renderAll();queueNext();};
$('flip').onclick=()=>{flipped=!flipped;renderBoard();renderPlayers();};
$('copyFen').onclick=()=>{navigator.clipboard?.writeText(game.fen()).catch(()=>{});setMsg('FEN copied.');};
$('copyPgn').onclick=()=>{navigator.clipboard?.writeText(game.pgn()).catch(()=>{});setMsg('Moves copied.');};
$('loadFen').onclick=()=>{const next=$('fen').value.trim(),temp=new Chess();if(!temp.load(next)){setMsg('Invalid FEN — position unchanged.');return;}version++;resetReviews();game=temp;selected=null;lastMove=null;renderAll();queueNext();};
$('analyze').onclick=()=>void analyze();
$('thinkTime').oninput=()=>{$('timeLabel').textContent=$('thinkTime').value+' ms';evalCache.clear();};
$('blend').oninput=()=>{$('blendLabel').textContent='H'+$('blend').value;evalCache.clear();bestArrow=null;drawArrows();queueNext();};
document.querySelectorAll('.mode').forEach(btn=>btn.onclick=()=>{version++;mode=btn.dataset.mode;selected=null;if(mode==='black')flipped=true;else if(mode==='white')flipped=false;
 document.querySelectorAll('.mode').forEach(b=>b.classList.toggle('active',b===btn));renderAll();queueNext();});
$('autoAnalyze').onchange=queueNext;
initAnalyzerUI();renderReview();renderAll(); // Absolutely no engine dependency: users can play immediately.
bootEngine();
})();
