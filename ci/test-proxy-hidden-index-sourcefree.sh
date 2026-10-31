#!/usr/bin/env bash
# Public synthetic Git-index admission test; no Oreslang or private source.
set -euo pipefail
export PATH=/usr/bin:/bin
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
cd "$scratch"
git init -q
git config user.name 'Synthetic CI Fixture'
git config user.email 'synthetic@example.invalid'
printf 'original approved bytes\n' > tracked.txt
printf '*.ignored\n' > .gitignore
git add tracked.txt .gitignore
git commit -qm 'public synthetic checkout fixture'
original="$(git rev-parse HEAD:tracked.txt)"
for case in assume-unchanged skip-worktree; do
  git update-index --"$case" tracked.txt
  printf 'concealed tampered bytes\n' > tracked.txt
  test -z "$(git status --porcelain=v1 --untracked-files=all)" || { echo "concealment not reproduced: $case"; exit 1; }
  test "$(git hash-object --no-filters -- tracked.txt)" != "$original" || { echo "tampering not detected: $case"; exit 1; }
  git update-index --"no-$case" tracked.txt
  git checkout -- tracked.txt
  test "$(git hash-object --no-filters -- tracked.txt)" = "$original"
  printf 'PASS: concealed %s mutation detected by full tracked byte hash\n' "$case"
done
printf 'hidden extra\n' > extra.ignored
test -z "$(git status --porcelain=v1 --untracked-files=all)"
test "$(git ls-files --others --ignored --exclude-standard -z | wc -c)" -gt 0
rm extra.ignored
echo 'PASS: ignored untracked addition needs independent enumeration'
git update-index --chmod=+x tracked.txt
test "$(git ls-files --stage -- tracked.txt | cut -d' ' -f1)" != "$(git ls-tree HEAD -- tracked.txt | cut -d' ' -f1)"
git update-index --chmod=-x tracked.txt
echo 'PASS: index mode drift detected separately from file hash'
echo 'PUBLIC_SYNTHETIC_CHECKOUT_ADMISSION_PASS=4'
