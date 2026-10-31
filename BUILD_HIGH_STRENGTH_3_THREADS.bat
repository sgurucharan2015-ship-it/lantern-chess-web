@echo off
setlocal
cd /d "%~dp0"
where em++ >nul 2>nul
if errorlevel 1 (
 echo ERROR: em++ missing. Activate Emscripten first.
 pause
 exit /b 1
)
echo.
echo LAN TERN WEB HIGH-STRENGTH BUILD - 3 THREADS / 512 MB HASH
 echo NOTE: This build REQUIRES static hosting with COOP/COEP headers.
 echo Cloudflare Pages supports the included site\_headers file; GitHub Pages may not.
em++ "src\lantern_web.cpp" -DLANTERN_BROWSER -DLANTERN_BROWSER_PTHREADS -std=c++17 -O3 -flto -msimd128 -pthread -s USE_PTHREADS=1 -s PTHREAD_POOL_SIZE=4 -s WASM=1 -s MODULARIZE=1 -s EXPORT_NAME=LanternWasm -s ENVIRONMENT=worker -s FILESYSTEM=1 -s FORCE_FILESYSTEM=1 -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=805306368 -s MAXIMUM_MEMORY=2147483648 -s STACK_SIZE=8388608 -s EXPORTED_FUNCTIONS=[_lantern_web_init,_lantern_web_state,_lantern_web_move,_lantern_web_search,_malloc,_free] -s EXPORTED_RUNTIME_METHODS=[ccall] --preload-file "site\nets\full_best.nnue@/nets/full_best.nnue" -o "site\lantern.js"
if errorlevel 1 (
 echo HIGH-STRENGTH WASM BUILD FAILED. Do not upload incomplete output.
 pause
 exit /b 2
)
if not exist "site\lantern.wasm" goto :bad
if not exist "site\lantern.data" goto :bad
echo.
echo 3-thread build generated. Deploy site\ to static host supporting cross-origin isolation.
pause
exit /b 0
:bad
echo Required output missing.
pause
exit /b 3
