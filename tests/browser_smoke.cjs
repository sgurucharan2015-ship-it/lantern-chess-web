const { chromium } = require('playwright');
(async () => {
 const browser=await chromium.launch({headless:true,args:['--no-sandbox']});
 try {
  const page=await browser.newPage();
  page.on('console',m=>console.log('BROWSER',m.type(),m.text().slice(0,500)));
  page.on('pageerror',e=>console.error('PAGE ERROR',e.message));
  await page.goto('http://127.0.0.1:8765/',{waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>document.querySelectorAll('.square').length===64,null,{timeout:15000});
  await page.waitForFunction(()=>/ready|unavailable|error/i.test(document.getElementById('engineText').textContent),null,{timeout:90000});
  const status=await page.locator('#engineText').innerText();
  if(!/NNUE ready/i.test(status))throw Error('Engine did not initialize: '+status+' / '+await page.locator('#message').innerText());
  await page.locator('#analyze').click();
  await page.waitForFunction(()=>Number(document.getElementById('depthValue').textContent)>0,null,{timeout:60000});
  const pv=await page.locator('#pvValue').innerText();
  if(/No engine PV|ERROR/i.test(pv) || !/^([a-h][1-8][a-h][1-8])/.test(pv))throw Error('Search failed or no UCI PV: '+pv);
  console.log('PASS: Real Lantern NNUE initialized and searched, PV:',pv);
 }finally {await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
