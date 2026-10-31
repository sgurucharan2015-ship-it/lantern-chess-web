"""Fast source/static release preflight. Used in GitHub Actions and locally."""
from pathlib import Path
import subprocess
import re

root = Path(__file__).resolve().parents[1]
site = root / 'site'
for f in ['index.html','app.js','style.css','engine-worker.js',
          'coi-boot.js','coi-service-worker.js','_headers',
          'nets/full_best.nnue']:
    assert (site / f).is_file(), f'Missing: {f}'
for f in ['app.js','engine-worker.js','coi-boot.js','coi-service-worker.js']:
    subprocess.run(['node','--check',str(site/f)],check=True)
html = (site/'index.html').read_text()
app = (site/'app.js').read_text()
worker = (site/'engine-worker.js').read_text()
for item in ['coi-boot.js', 'engineText','board','fen','mode', 'thinkTime','promotionDialog']:
    assert item in html, f'Missing HTML feature: {item}'
for feature in ['pointerdown','pointermove','pointerup','possible','capture',
                'mode===\'both\'', 'function gameOver()', 'animationRunning']:
    assert feature in app, f'Missing board feature: {feature}'
assert 'LanternWasm' in worker and 'lantern_web_search' in worker
assert 'crossOriginIsolated' in app
assert 'Cross-Origin-Opener-Policy' in (site/'coi-service-worker.js').read_text()
for f in ['lantern.js', 'lantern.wasm', 'lantern.data']:
    if (site/f).exists():
        assert (site/f).stat().st_size > 0
print('PASS: web files, node syntax, 3-thread isolation shim and GUI features')
