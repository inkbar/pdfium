#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Edward Coleridge Smith
#
# Builds inkbar/out/inkbar-pdfium.{wasm,mjs} in a pinned Ubuntu container.
# Run from anywhere. CONTAINER=docker switches the runtime (default podman).
set -euo pipefail

CONTAINER="${CONTAINER:-podman}"
IMAGE="docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8" # ubuntu:24.04
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# gclient compares this with the checkout's own origin and tries to switch it when they differ.
FORK_URL="$(git -C "$REPO" remote get-url origin)"

"$CONTAINER" run --rm --security-opt label=disable -e "PDF_RUNTIME_FORK_URL=$FORK_URL" -v "$REPO:/ws/runtime" "$IMAGE" bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive DEPOT_TOOLS_UPDATE=1 DEPOT_TOOLS_PYTHON_BYPASS=1 EMSDK_VERSION=3.1.72
apt-get update -qq
apt-get install -y -qq git python3 python-is-python3 curl cmake g++ pkg-config tar xz-utils lsb-release ca-certificates file >/dev/null
git config --global --add safe.directory "*"
# gclient fetches origin, which is private. Point it at the mounted checkout.
git config --global "url./ws/runtime/.git.insteadOf" "$PDF_RUNTIME_FORK_URL"
cd /ws/runtime
# cipd renames packages from /ws/.cipd into the checkout, which fails across the mount.
mkdir -p /ws/runtime/.cipd-site
ln -sfn /ws/runtime/.cipd-site /ws/.cipd
# depot_tools takes file locks, which the mounted checkout does not support. Keep it in the container.
git clone -q https://chromium.googlesource.com/chromium/tools/depot_tools.git /opt/depot_tools
export PATH="/opt/depot_tools:$PATH"
[[ -d third_party/emsdk ]] || git clone -q https://github.com/emscripten-core/emsdk.git third_party/emsdk
third_party/emsdk/emsdk install "$EMSDK_VERSION" >/dev/null
third_party/emsdk/emsdk activate "$EMSDK_VERSION" >/dev/null
export PATH="$PWD/third_party/emsdk/upstream/emscripten:$PWD/third_party/emsdk/upstream/bin:$PATH"
scripts/embedpdf-runtime/build-target.sh wasm32
inkbar/link.sh
'
