#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Edward Coleridge Smith
#
# Packs the build output into inkbar/out/inkbar-pdfium-wasm32-<tag>.tar.gz and prints its SHA-256.
# Usage: inkbar/package.sh <tag>. Run inkbar/write-manifest.sh first. Needs GNU tar.
set -euo pipefail

TAG="${1:?usage: package.sh <tag>}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

OUT=inkbar/out
BUILD=out/embedpdf-runtime/wasm32
NINJA="${NINJA:-third_party/ninja/ninja}"
EMSCRIPTEN=third_party/emsdk/upstream/emscripten
ARCHIVE="$OUT/inkbar-pdfium-wasm32-$TAG.tar.gz"
for f in inkbar-pdfium.wasm inkbar-pdfium.mjs exports.txt manifest.json; do
  [[ -f "$OUT/$f" ]] || { echo "package.sh: $OUT/$f not found." >&2; exit 1; }
done

fail() {
  echo "package.sh: $*" >&2
  exit 1
}

# Appends one licence section: a header with the library name and the file's path, then the file verbatim.
section() {
  local name="$1" file="$2"
  [[ -f "$file" ]] || fail "licence file $file for $name not found."
  printf '%s\n%s\n%s\n%s\n\n' \
    '================================================================================' \
    "$name" "$file" \
    '================================================================================'
  cat "$file"
  printf '\n\n'
}

# Prints the value of a "Key:" line in a README.chromium or README.pdfium file.
field() {
  sed -n "/^$1:/{s/^$1:[[:space:]]*//p;q;}" "$2"
}

# The third-party libraries in the binary are the ones whose sources or headers
# went into an object in libembedpdf.a. Ninja's dependency log records both.
mapfile -t NINJA_FILES < <(grep -rl '^build obj/libembedpdf.a:' "$BUILD/obj" --include='*.ninja')
(( ${#NINJA_FILES[@]} == 1 )) || fail "expected one build rule for libembedpdf.a under $BUILD/obj."
NINJA_FILE="${NINJA_FILES[0]}"
[[ -x "$NINJA" ]] || fail "$NINJA not found. Set NINJA to a ninja binary."
mapfile -t OBJECTS < <(grep '^build obj/libembedpdf.a:' "$NINJA_FILE" | tr ' ' '\n' | grep '\.o$')
(( ${#OBJECTS[@]} > 0 )) || fail "libembedpdf.a has no objects in $NINJA_FILE."
mapfile -t LIBS < <(
  "$NINJA" -C "$BUILD" -t deps "${OBJECTS[@]}" |
    sed -n 's#^[[:space:]]\{1,\}\(\.\./\)*\(/ws/runtime/\)\{0,1\}\(third_party/[^/]*\)/.*#\3#p' |
    grep -v '^third_party/emsdk$' |
    sort -u
)
(( ${#LIBS[@]} > 0 )) || fail "found no third-party libraries in the dependency log."

LICENSES="$OUT/THIRD_PARTY_LICENSES"
{
  echo 'Third-party software in inkbar-pdfium.wasm and inkbar-pdfium.mjs.'
  echo 'Each section names a library and the file its licence text comes from.'
  echo
  for lib in "${LIBS[@]}"; do
    readme=""
    for candidate in "$lib/README.chromium" "$lib/README.pdfium"; do
      if [[ -f "$candidate" ]] && grep -q '^License File:' "$candidate"; then
        readme="$candidate"
        break
      fi
    done
    [[ -n "$readme" ]] || fail "$lib has no README.chromium or README.pdfium with a License File line."
    name="$(field Name "$readme")"
    [[ -n "$name" ]] || name="${lib#third_party/}"
    IFS=',' read -ra files <<< "$(field 'License File' "$readme")"
    for file in "${files[@]}"; do
      file="$(echo "$file" | tr -d '[:space:]')"
      [[ -n "$file" ]] || continue
      if [[ "$file" == //* ]]; then
        file="${file#//}"
      else
        file="$lib/$file"
      fi
      section "$name" "$file"
    done
  done
  # Emscripten links its own C library, C++ library and compiler runtime into the module.
  section 'Emscripten' "$EMSCRIPTEN/LICENSE"
  section 'musl libc (Emscripten)' "$EMSCRIPTEN/system/lib/libc/musl/COPYRIGHT"
  section 'libc++ (Emscripten)' "$EMSCRIPTEN/system/lib/libcxx/LICENSE.TXT"
  section 'libc++abi (Emscripten)' "$EMSCRIPTEN/system/lib/libcxxabi/LICENSE.TXT"
  section 'compiler-rt (Emscripten)' "$EMSCRIPTEN/system/lib/compiler-rt/LICENSE.TXT"
} > "$LICENSES"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp "$OUT/inkbar-pdfium.wasm" "$OUT/inkbar-pdfium.mjs" "$OUT/exports.txt" "$OUT/manifest.json" "$LICENSES" LICENSE NOTICE "$STAGE/"

# Fixed order, owner and mtime keep the archive reproducible. gzip -n drops the timestamp.
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' -C "$STAGE" -cf - \
  LICENSE NOTICE THIRD_PARTY_LICENSES exports.txt inkbar-pdfium.mjs inkbar-pdfium.wasm manifest.json |
  gzip -9 -n > "$ARCHIVE"
COUNT="$(tar -tzf "$ARCHIVE" | wc -l)"
(( COUNT == 7 )) || fail "the archive holds $COUNT files, not 7."

sha256sum "$ARCHIVE" | cut -d' ' -f1
