@echo off
setlocal
cd /d "%~dp0"
where em++ >nul 2>nul
if errorlevel 1 (
 echo.
 echo ERROR: Emscripten is not activated.
 echo Install and activate emsdk once: https://emscripten.org/docs/getting_started/downloads.html
 echo In an activated emsdk Command Prompt, run BUILD_WEB.bat again.
 echo.
 pause
 exit /b 1
)
if not exist "site\nets\full_best.nnue" (
 echo ERROR: Missing site\nets\full_best.nnue
 pause
 exit /b 2
)
echo Compiling ORIGINAL Lantern 16.4 C++ search and bundled NNUE into WebAssembly...
echo This standard build needs no special hosting headers, but runs with one engine thread.
em++ "src\lantern_web.cpp" -DLANTERN_BROWSER -std=c++17 -O3 -flto -msimd128 -s WASM=1 -s MODULARIZE=1 -s EXPORT_NAME=LanternWasm -s ENVIRONMENT=worker -s FILESYSTEM=1 -s FORCE_FILESYSTEM=1 -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=268435456 -s MAXIMUM_MEMORY=2147483648 -s STACK_SIZE=8388608 -s EXPORTED_FUNCTIONS=[_lantern_web_init,_lantern_web_state,_lantern_web_move,_lantern_web_search,_malloc,_free] -s EXPORTED_RUNTIME_METHODS=[ccall] --preload-file "site\nets\full_best.nnue@/nets/full_best.nnue" -o "site\lantern.js"
if errorlevel 1 (
 echo.
 echo BUILD FAILED. Nothing should be published until the error is fixed.
 pause
 exit /b 3
)
if not exist "site\lantern.wasm" goto :bad
if not exist "site\lantern.data" goto :bad
echo.
echo SUCCESS: site\lantern.js, site\lantern.wasm, site\lantern.data generated.
echo Upload the WHOLE site folder to static HTTPS hosting, e.g. Cloudflare Pages.
echo No visitor uploads, no chess backend server, and no API keys needed.
pause
exit /b 0
:bad
echo ERROR: Compilation ended but one or more engine output files are missing.
pause
exit /b 4
