#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Edward Coleridge Smith
#
# Packs the build output into inkbar/out/inkbar-pdfium-wasm32-<tag>.tar.gz and prints its SHA-256.
# Usage: inkbar/package.sh <tag>. Run inkbar/write-manifest.sh first.
set -euo pipefail

TAG="${1:?usage: package.sh <tag>}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

OUT=inkbar/out
ARCHIVE="$OUT/inkbar-pdfium-wasm32-$TAG.tar.gz"
for f in inkbar-pdfium.wasm inkbar-pdfium.mjs exports.txt manifest.json; do
  [[ -f "$OUT/$f" ]] || { echo "package.sh: $OUT/$f not found." >&2; exit 1; }
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp "$OUT/inkbar-pdfium.wasm" "$OUT/inkbar-pdfium.mjs" "$OUT/exports.txt" "$OUT/manifest.json" LICENSE NOTICE "$STAGE/"

# Fixed order, owner and mtime keep the archive reproducible. gzip -n drops the timestamp.
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' -C "$STAGE" -cf - \
  LICENSE NOTICE exports.txt inkbar-pdfium.mjs inkbar-pdfium.wasm manifest.json | gzip -9 -n > "$ARCHIVE"

sha256sum "$ARCHIVE" | cut -d' ' -f1
