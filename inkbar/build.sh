#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Edward Coleridge Smith
#
# Builds inkbar/out/inkbar-pdfium.{wasm,mjs} in a pinned Ubuntu container.
# Needs a full-history clone with an origin remote and a clean tree. Run from anywhere. CONTAINER=docker switches the runtime (default podman).
set -euo pipefail

# Toolchain pins. The image is pinned below by digest.
DEPOT_TOOLS_COMMIT=ec77017fb3e090ff02bffb9c6980ebcdd3006ddc
EMSDK_COMMIT=708dcbcc9ee23bc7d2dad44fc90db377e38b771c
EMSDK_VERSION=3.1.72

CONTAINER="${CONTAINER:-podman}"
IMAGE="docker.io/library/ubuntu@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8" # ubuntu:24.04
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# gclient compares this with the checkout's own origin and tries to switch it when they differ.
FORK_URL="$(git -C "$REPO" remote get-url origin 2>/dev/null)" || {
  echo "build.sh: the checkout has no 'origin' remote. Clone inkbar/pdfium with full history." >&2
  exit 1
}

# label=disable: SELinux hosts deny container writes to the bind mount.
"$CONTAINER" run --rm --security-opt label=disable -e "PDF_RUNTIME_FORK_URL=$FORK_URL" -e "HOST_UID=$(id -u)" -e "HOST_GID=$(id -g)" -e "DEPOT_TOOLS_COMMIT=$DEPOT_TOOLS_COMMIT" -e "EMSDK_COMMIT=$EMSDK_COMMIT" -e "EMSDK_VERSION=$EMSDK_VERSION" -v "$REPO:/ws/runtime" "$IMAGE" bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive DEPOT_TOOLS_UPDATE=1 DEPOT_TOOLS_PYTHON_BYPASS=1
apt-get update -qq
apt-get install -y -qq git python3 python-is-python3 curl cmake g++ pkg-config tar xz-utils lsb-release ca-certificates file >/dev/null
# Hand the files the build wrote back to the host user, also on failure.
# Skipped in a user namespace (rootless podman), where root already is the host user
# and chown to the host uid would map to a sub-uid instead.
if [[ "$(cat /proc/self/uid_map)" =~ ^[[:space:]]*0[[:space:]]+0[[:space:]]+4294967295 ]]; then
  trap "chown -R $HOST_UID:$HOST_GID /ws/runtime 2>/dev/null || true" EXIT
fi
git config --global --add safe.directory "*"
# gclient fetches origin, which is private. Point it at the mounted checkout.
git config --global "url./ws/runtime/.git.insteadOf" "$PDF_RUNTIME_FORK_URL"
cd /ws/runtime
# cipd renames packages from /ws/.cipd into the checkout, which fails across the mount.
mkdir -p /ws/runtime/.cipd-site
ln -sfn /ws/runtime/.cipd-site /ws/.cipd
# depot_tools takes file locks, which the mounted checkout does not support. Keep it in the container.
git clone -q https://chromium.googlesource.com/chromium/tools/depot_tools.git /opt/depot_tools
git -C /opt/depot_tools checkout -q "$DEPOT_TOOLS_COMMIT"
export PATH="/opt/depot_tools:$PATH"
# Bootstrap depot_tools Python. gn and ninja wrappers need it, also when no sync runs.
gclient --version >/dev/null
[[ -d third_party/emsdk ]] || git clone -q https://github.com/emscripten-core/emsdk.git third_party/emsdk
git -C third_party/emsdk checkout -q "$EMSDK_COMMIT"
third_party/emsdk/emsdk install "$EMSDK_VERSION" >/dev/null
third_party/emsdk/emsdk activate "$EMSDK_VERSION" >/dev/null
export PATH="$PWD/third_party/emsdk/upstream/emscripten:$PWD/third_party/emsdk/upstream/bin:$PATH"
scripts/embedpdf-runtime/build-target.sh wasm32
inkbar/link.sh
'
