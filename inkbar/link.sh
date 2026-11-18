#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Edward Coleridge Smith
#
# Links the shim and libembedpdf.a into the WebAssembly module. Runs inside the
# build container, with the repository root as the working directory.
set -euo pipefail

export PATH="$PWD/third_party/emsdk/upstream/emscripten:$PWD/third_party/emsdk/upstream/bin:$PATH"
export EM_CONFIG="$PWD/third_party/emsdk/.emscripten"

LIB=$(find out/embedpdf-runtime/wasm32 -name libembedpdf.a -print -quit)
[[ -n "$LIB" ]] || { echo "libembedpdf.a not found" >&2; exit 1; }

mkdir -p inkbar/out
LIST=inkbar/out/exports.list
sed 's/^/_/' inkbar/exports.txt > "$LIST"

emcc -O2 -c inkbar/shim.c -Ipublic -o inkbar/out/shim.o

# Inkbar's C++ reaches PDFium's internal classes, so it is compiled as PDFium's
# own fpdfsdk sources are: the same defines, include paths and flags, read from
# the Ninja file GN wrote, from the build directory the paths are relative to.
BUILD="$(dirname "$(dirname "$LIB")")"
NINJA_FILE="$BUILD/obj/fpdfsdk/fpdfsdk.ninja"
[[ -f "$NINJA_FILE" ]] || { echo "$NINJA_FILE not found" >&2; exit 1; }
FLAGS=""
for var in defines include_dirs cflags cflags_cc; do
  value="$(sed -n "s/^$var = //p" "$NINJA_FILE")"
  [[ -n "$value" ]] || { echo "no $var in $NINJA_FILE" >&2; exit 1; }
  # A $ is a Ninja variable or escape, which eval below would read as shell.
  if [[ "$value" == *'$'* ]]; then
    echo "$var in $NINJA_FILE contains a \$, which link.sh cannot expand like Ninja: $value" >&2
    exit 1
  fi
  FLAGS+=" $value"
done
ROOT="$PWD"
# eval: Ninja runs the command through a shell, which strips the quoting in the defines.
OBJECTS=()
for source in appearance redact sanitize inspect; do
  (cd "$BUILD" && eval "em++ $FLAGS -Wno-unknown-warning-option -c '$ROOT/inkbar/$source.cc' -o '$ROOT/inkbar/out/$source.o'")
  OBJECTS+=("inkbar/out/$source.o")
done

em++ -O2 inkbar/out/shim.o "${OBJECTS[@]}" "$LIB" -o inkbar/out/inkbar-pdfium.mjs \
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
