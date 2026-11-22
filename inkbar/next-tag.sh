#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Edward Coleridge Smith
#
# Prints the tag of the next release: v<branch>.<n>. <branch> is the highest Chromium branch that
# is an ancestor of HEAD. <n> is one more than the highest release already tagged on that branch,
# or 1 when there is none. Run from a full-history clone with its tags. Needs an upstream remote.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

git fetch --quiet upstream '+refs/heads/chromium/*:refs/remotes/upstream/chromium/*'

BRANCH=""
for b in $(git branch -r --list 'upstream/chromium/*' | sort -t/ -k3 -n -r); do
  if git merge-base --is-ancestor "$b" HEAD; then
    BRANCH="${b##*/}"
    break
  fi
done
[[ -n "$BRANCH" ]] || { echo "next-tag.sh: no upstream chromium branch is an ancestor of HEAD." >&2; exit 1; }

LAST=0
for t in $(git tag --list "v$BRANCH.*"); do
  n="${t#"v$BRANCH."}"
  [[ "$n" =~ ^[0-9]+$ ]] || continue
  (( n > LAST )) && LAST="$n"
done

echo "v$BRANCH.$((LAST + 1))"
