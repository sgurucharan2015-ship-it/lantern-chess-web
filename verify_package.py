from pathlib import Path
import struct, hashlib, subprocess, sys
root=Path(__file__).resolve().parent
n=root/'site/nets/full_best.nnue'
assert n.is_file()
magic,vers,inputs,buckets,hidden,qa,qb,scale=struct.unpack('<8I',n.read_bytes()[:32])
assert magic==0x314E5350 and vers==3 and inputs==768 and buckets==8 and hidden==1024 and (qa,qb,scale)==(255,64,400)
assert n.stat().st_size==12617776
assert (root/'site/index.html').is_file() and (root/'site/app.js').is_file()
assert 'LANTERN_BROWSER_PTHREADS' in (root/'src/lantern_web.cpp').read_text()
for file in ['site/app.js','site/engine-worker.js','site/coi-boot.js','site/coi-service-worker.js','gen_pieces.js']:
 subprocess.run(['node','--check',str(root/file)],check=True)
required='--require-wasm' in sys.argv
for f in ['site/lantern.js','site/lantern.wasm','site/lantern.data']:
 p=root/f
 if required: assert p.stat().st_size>1000, 'Missing compiled file '+f
 if p.exists(): print('Compiled file:',f,p.stat().st_size)
print('VERIFY: PASS source, frontend syntax, NNUE header and size'+(', compiled wasm assets' if required else ''))
print('SHA256 NNUE:',hashlib.sha256(n.read_bytes()).hexdigest())
