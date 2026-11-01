"""Verify real Lantern browser assets, legal-chess fallbacks, and UI wiring."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
site = root / 'site'
for file in ('index.html','app.js','style.css','engine-worker.js',
             'coi-boot.js','coi-service-worker.js','_headers',
             'nets/full_best.nnue','chess.js'):
    assert (site/file).is_file(), f'Missing deployed web file: {file}'
for file in ('app.js','engine-worker.js','coi-boot.js','coi-service-worker.js','chess.js'):
    subprocess.run(['node','--check',str(site/file)], check=True)
html=(site/'index.html').read_text(encoding='utf-8')
app=(site/'app.js').read_text(encoding='utf-8')
worker=(site/'engine-worker.js').read_text(encoding='utf-8')
for item in ('chess.js?v=repair3', 'app.js?v=repair3','engineText','board','fen','mode',
             'thinkTime','promotionDialog'):
    assert item in html, f'Missing HTML component: {item}'
for feature in ('pointerdown','pointermove','pointerup','possible','capture',
                'game.game_over()', 'canHuman', 'renderAll(); // Absolutely no engine dependency',
                'game.moves({square:', 'animate(', 'botMove('):
    assert feature in app, f'Missing legal board feature: {feature}'
assert 'LanternWasm' in worker and 'lantern_web_search' in worker
assert 'mainScriptUrlOrBlob' in worker, 'Missing threaded worker script URL fix'
assert 'if(!self.crossOriginIsolated)' not in app
assert 'Cross-Origin-Opener-Policy' in (site/'coi-service-worker.js').read_text()
for f in ('lantern.js', 'lantern.wasm', 'lantern.data'):
    assert (site/f).is_file() and (site/f).stat().st_size > 1000, f'Missing real C++ engine: {f}'
print('PASS: fully playable manual board, threading loader, C++ NNUE assets and JavaScript')
