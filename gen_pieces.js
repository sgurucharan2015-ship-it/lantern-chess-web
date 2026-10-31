const fs = require('fs'), path = require('path');
const dir=path.join(__dirname,'site','pieces');fs.mkdirSync(dir,{recursive:true});
const dirFA='/opt/nvm/versions/node/v22.16.0/lib/node_modules/@fortawesome/free-solid-svg-icons';
for(const [symbol,n] of Object.entries({p:'Pawn',n:'Knight',b:'Bishop',r:'Rook',q:'Queen',k:'King'})){
 const icon=require(dirFA+'/faChess'+n+'.js')['faChess'+n].icon;
 const [w,h,,,paths]=icon, datum=Array.isArray(paths)?paths.join(' '):paths;
 for(const color of ['w','b']){
  const dark=color==='b';
  for(const check of (symbol==='k'?['','_check']:[''])){
   const kind=dark?'obsidian':'ivory';
   const svg=`<svg xmlns="http://www.w3.org/2000/svg" viewBox="-24 -22 ${w+48} ${h+44}"><defs>
     <linearGradient id="${kind}" x1="0" y1="0" x2="0.72" y2="1">
       <stop offset="0" stop-color="${dark?'#515969':'#ffffff'}"/>
       <stop offset=".44" stop-color="${dark?'#1c2230':'#f2efe7'}"/>
       <stop offset="1" stop-color="${dark?'#070b12':'#bbc4ca'}"/>
     </linearGradient>
     <filter id="shadow" x="-30%" y="-20%" width="160%" height="160%"><feDropShadow dx="3" dy="7" stdDeviation="6" flood-color="#020617" flood-opacity=".43"/></filter>
     <filter id="glow"><feDropShadow dx="0" dy="0" stdDeviation="8" flood-color="#ff1942" flood-opacity=".93"/></filter>
    </defs>
    ${check?`<rect x="0" y="0" width="${w}" height="${h}" fill="#ff314b" rx="20" opacity=".16" filter="url(#glow)"/>`:''}
    <path d="${datum}" fill="url(#${kind})" stroke="${dark?'#9da8c0':'#454a57'}" stroke-width="${dark?'3':'5'}" stroke-linejoin="round" filter="url(#shadow)"/>
    <path d="${datum}" fill="none" stroke="${dark?'#c9d1df':'#ffffff'}" stroke-opacity="${dark?'.15':'.46'}" stroke-width="2"/>
   </svg>`;
   fs.writeFileSync(path.join(dir,color+symbol+check+'.svg'),svg);
  }
 }
}
console.log('generated piece art in',dir);
