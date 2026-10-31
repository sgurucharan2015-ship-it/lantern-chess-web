#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
command -v em++ >/dev/null || { echo 'ERROR: em++ missing. Install and activate Emscripten SDK.' >&2; exit 1; }
THREADS="${1:-1}"
flags=()
initial=268435456
if [[ "$THREADS" == "3" ]]; then
  flags=(-DLANTERN_BROWSER_PTHREADS -pthread '-sUSE_PTHREADS=1' '-sPTHREAD_POOL_SIZE=4')
  initial=805306368
elif [[ "$THREADS" != "1" ]];then echo 'Usage: ./build_web.sh [1|3]';exit 2;fi
em++ src/lantern_web.cpp -DLANTERN_BROWSER "${flags[@]}" -std=c++17 -O3 -flto -msimd128 \
  '-sWASM=1' '-sMODULARIZE=1' '-sEXPORT_NAME=LanternWasm' '-sENVIRONMENT=worker' \
  '-sFILESYSTEM=1' '-sFORCE_FILESYSTEM=1' '-sALLOW_MEMORY_GROWTH=1' "-sINITIAL_MEMORY=$initial" \
  '-sMAXIMUM_MEMORY=2147483648' '-sSTACK_SIZE=8388608' \
  '-sEXPORTED_FUNCTIONS=["_lantern_web_init","_lantern_web_state","_lantern_web_move","_lantern_web_search","_malloc","_free"]' \
  '-sEXPORTED_RUNTIME_METHODS=["ccall"]' \
  --preload-file 'site/nets/full_best.nnue@/nets/full_best.nnue' -o site/lantern.js
for f in site/lantern.js site/lantern.wasm site/lantern.data;do test -s "$f" || { echo "Missing $f";exit 1; };done
echo 'SUCCESS: Publish the entire site/ directory on a static host.'
