#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Edward Coleridge Smith
#
# Links the shim and libembedpdf.a into the WebAssembly module. Runs inside the
# build container, with the repository root as the working directory.
set -euo pipefail

export PATH="$PWD/third_party/emsdk/upstream/emscripten:$PWD/third_party/emsdk/upstream/bin:$PATH"
export EM_CONFIG="$PWD/third_party/emsdk/.emscripten"

LIB=$(find out/embedpdf-runtime/wasm32 -name libembedpdf.a | head -1)
[[ -n "$LIB" ]] || { echo "libembedpdf.a not found" >&2; exit 1; }

mkdir -p inkbar/out
LIST=inkbar/out/exports.list
sed 's/^/_/' inkbar/exports.txt > "$LIST"

emcc -O2 -c inkbar/shim.c -Ipublic -o inkbar/out/shim.o
em++ -O2 inkbar/out/shim.o "$LIB" -o inkbar/out/inkbar-pdfium.mjs \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node \
  -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 \
  -sEXPORTED_FUNCTIONS=@"$LIST" \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32,getValue,setValue,stringToUTF8,lengthBytesUTF8

cp inkbar/exports.txt inkbar/out/exports.txt

RAW=$(wc -c < inkbar/out/inkbar-pdfium.wasm)
GZ=$(gzip -9 -c inkbar/out/inkbar-pdfium.wasm | wc -c)
BUDGET=$(tr -d '[:space:]' < inkbar/size-budget.txt)
echo "inkbar-pdfium.wasm: raw ${RAW} bytes, gzipped ${GZ} bytes, budget ${BUDGET}"
if (( GZ > BUDGET )); then
  echo "gzipped size ${GZ} exceeds the budget ${BUDGET}" >&2
  exit 1
fi
