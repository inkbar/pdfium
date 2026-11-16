#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Edward Coleridge Smith
#
# Writes inkbar/out/manifest.json for a built module. Usage: inkbar/write-manifest.sh <tag>
# Run from a full-history clone after inkbar/build.sh. Needs an upstream remote.
set -euo pipefail

TAG="${1:?usage: write-manifest.sh <tag>}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

OUT=inkbar/out
WASM="$OUT/inkbar-pdfium.wasm"
ARGS=out/embedpdf-runtime/wasm32/args.gn
[[ -f "$WASM" ]] || { echo "write-manifest.sh: $WASM not found. Run inkbar/build.sh first." >&2; exit 1; }
[[ -f "$ARGS" ]] || { echo "write-manifest.sh: $ARGS not found." >&2; exit 1; }

git fetch --quiet upstream '+refs/heads/chromium/*:refs/remotes/upstream/chromium/*' main
UPSTREAM_COMMIT="$(git merge-base HEAD upstream/main)"

BRANCH=""
for b in $(git branch -r --list 'upstream/chromium/*' | sort -t/ -k3 -n -r); do
  if git merge-base --is-ancestor "$b" HEAD; then
    BRANCH="${b##*/}"
    break
  fi
done
[[ -n "$BRANCH" ]] || { echo "write-manifest.sh: no upstream chromium branch is an ancestor of HEAD." >&2; exit 1; }
# Tags are v<branch>.<n>. The branch part must name the branch the build is on.
TAG_BRANCH="${TAG#v}"
TAG_BRANCH="${TAG_BRANCH%%.*}"
[[ "$TAG_BRANCH" == "$BRANCH" ]] || {
  echo "write-manifest.sh: tag $TAG names branch $TAG_BRANCH, but HEAD is on chromium branch $BRANCH." >&2
  exit 1
}

EMSDK="$(sed -n 's/^EMSDK_VERSION=//p' inkbar/build.sh | head -n 1)"
GN_ARGS="$(tr '\n' ' ' < "$ARGS" | sed 's/ $//')"
SHA="$(sha256sum "$WASM" | cut -d' ' -f1)"
GZ="$(gzip -9 -c "$WASM" | wc -c | tr -d ' ')"

jq -n \
  --arg version "$TAG" \
  --arg forkCommit "$(git rev-parse HEAD)" \
  --arg upstreamCommit "$UPSTREAM_COMMIT" \
  --argjson chromiumBranch "$BRANCH" \
  --arg emsdk "$EMSDK" \
  --arg gnArgs "$GN_ARGS" \
  --arg wasmSha256 "$SHA" \
  --argjson wasmGzipBytes "$GZ" \
  '$ARGS.named' > "$OUT/manifest.json"
cat "$OUT/manifest.json"
