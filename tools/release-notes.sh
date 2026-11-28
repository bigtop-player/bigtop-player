#!/usr/bin/env bash
# release-notes.sh <tag>: prints the release notes for <tag> (run from the repository root).
# First the hand-written docs/release-notes/<version>.md (v0.2.0-rc1 uses v0.2.0.md), then
# "New" (feat) and "Fixed" (fix, perf) from the commit titles since the last final release
# tag, then everyone with a commit in that range. Other commit types are left out.
set -euo pipefail
tag=${1:?usage: release-notes.sh <tag>}
version=${tag%%-*}
file="docs/release-notes/$version.md"
[ -f "$file" ] || { echo "release-notes: $file is missing" >&2; exit 1; }
cat "$file"
# Since the last final release, so every candidate of a version lists the whole round.
prev=$(git describe --tags --abbrev=0 --match 'v*' --exclude 'v*-*' "$tag^" 2>/dev/null || true)
range="${prev:+$prev..}$tag"
titles=$(git log --no-merges --format='%s' "$range")
new=$(printf '%s\n' "$titles" | sed -nE 's/^feat(\([^)]*\))?!?: (.*)$/- \2/p')
fixed=$(printf '%s\n' "$titles" | sed -nE 's/^(fix|perf)(\([^)]*\))?!?: (.*)$/- \3/p')
people=$(git log --no-merges --format='%an' "$range" | sort -u | paste -sd ',' - | sed 's/,/, /g')
[ -z "$new" ] || printf '\n## New\n\n%s\n' "$new"
[ -z "$fixed" ] || printf '\n## Fixed\n\n%s\n' "$fixed"
[ -z "$people" ] || printf '\n## Thanks\n\nCommits in this release by: %s\n' "$people"
