#!/usr/bin/env bash
# Tests tools/release-notes.sh against a throwaway repository with invented commits.
set -euo pipefail
script="$(cd "$(dirname "$0")/.." && pwd)/release-notes.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
git init -q
commit() { git -c user.name="$1" -c user.email="$1@example.invalid" commit -q --allow-empty -m "$2"; }
mkdir -p docs/release-notes
printf '# Bigtop Player 0.2.0\n\nHand-written part.\n' > docs/release-notes/v0.2.0.md
git add . && commit "Ada" "docs: notes"
commit "Ada" "feat: old feature"
git tag v0.1.0
commit "Ada" "feat(app): show a banner"
commit "Ben" "fix(build): compile on another system"
commit "Ada" "docs: wiki"
git tag v0.2.0-rc1
commit "Ada" "perf: faster import"
git tag v0.2.0-rc2
fail() { echo "FAIL: $1"; echo "$out"; exit 1; }
out=$(bash "$script" v0.2.0-rc2)
grep -q "Hand-written part." <<<"$out" || fail "hand-written part missing"
grep -q "^- show a banner$" <<<"$out" || fail "feat missing"
grep -q "^- compile on another system$" <<<"$out" || fail "fix missing"
grep -q "^- faster import$" <<<"$out" || fail "perf missing"
grep -q "old feature" <<<"$out" && fail "a commit of the previous release is listed"
grep -q "wiki" <<<"$out" && fail "a docs commit is listed"
grep -q "Commits in this release by: Ada, Ben" <<<"$out" || fail "contributors missing"
if bash "$script" v0.3.0 >/dev/null 2>&1; then fail "a version without notes file must fail"; fi
echo "release-notes: all tests passed"
