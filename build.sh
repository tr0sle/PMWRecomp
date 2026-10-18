#!/usr/bin/env bash
# Builds the browser version: gen/*.c (recompiled game code) + runtime/*.c -> dist/pmr.{js,wasm}
set -euo pipefail
cd "$(dirname "$0")"
EMSDK=${EMSDK:-/c/Users/Anon/emsdk}
export PATH="$EMSDK/upstream/emscripten:$EMSDK/upstream/bin:$PATH"
OPT=${OPT:--O2}
JOBS=${JOBS:-8}
CFLAGS="$OPT -g0 -fno-strict-aliasing -fwrapv -Iruntime -Igen -I$EMSDK/upstream/emscripten/system/lib -sSUPPORT_LONGJMP=wasm \
  -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses-equality -Wno-comment \
  -Wno-tautological-constant-out-of-range-compare -Wno-unused-value"
mkdir -p build dist
python tools/gen_stubs.py
compile() {
  local src=$1 obj=build/$(basename "${1%.c}").o
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ runtime/cpu.h -nt "$obj" ] || [ runtime/hle.h -nt "$obj" ]; then
    emcc $CFLAGS -c "$src" -o "$obj" || { echo "FAILED: $src"; return 1; }
    echo "  cc $src"
  fi
}
export -f compile
export CFLAGS
ls gen/*.c runtime/*.c | xargs -P "$JOBS" -I{} bash -c 'compile {}'
emcc $OPT build/*.o -o dist/pmr.js \
  --js-library runtime/lib.js --pre-js web/pre.js \
  -sJSPI -sSUPPORT_LONGJMP=wasm \
  -sGLOBAL_BASE=512MB -sINITIAL_MEMORY=768MB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=16MB \
  -sMALLOC=emmalloc -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sENVIRONMENT=web \
  -sEXPORTED_RUNTIME_METHODS=callMain,HEAPU8,HEAP32 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
  --profiling-funcs ${LDEXTRA:-}
cp web/index.html web/online.js web/gamestore.js web/test2.html dist/
rm -rf dist/overlay && cp -r web/overlay dist/overlay
echo "build ok: $(ls -la dist/pmr.wasm | awk '{print $5}') bytes"
