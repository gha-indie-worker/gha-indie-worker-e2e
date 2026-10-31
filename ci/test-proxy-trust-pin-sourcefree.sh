#!/usr/bin/env bash
# PUBLIC, source-free synthetic test: do not copy private Oreslang Git trees.
set -euo pipefail
export PATH=/usr/bin:/bin
hash -r
expected_repo='gha-indie-worker/gha-indie-worker-e2e'
expected_ref='refs/heads/ci/oreslang-proxy-sourcefree-trust-pin-20261031'
test "${GITHUB_REPOSITORY:-}" = "$expected_repo"
test "${GITHUB_REF:-}" = "$expected_ref"
test "${GITHUB_SHA:-}" = "$(/usr/bin/git rev-parse HEAD)"
test -z "$(/usr/bin/git status --porcelain --untracked-files=all)"
scratch="$(/usr/bin/mktemp -d)"
trap '/usr/bin/rm -rf -- "$scratch"' EXIT
/bin/printf '#!/bin/sh\nexit 0\n' > "$scratch/approved-verifier"
/bin/chmod 755 "$scratch/approved-verifier"
approved_blob="$(/usr/bin/git hash-object --no-filters -- "$scratch/approved-verifier")"
actual_blob="$(/usr/bin/git hash-object --no-filters -- "$scratch/approved-verifier")"
test "$actual_blob" = "$approved_blob"
echo 'PASS: reviewed synthetic blob matches expected'
test "$actual_blob" != 0000000000000000000000000000000000000000
echo 'PASS: stale placeholder pin cannot admit reviewed blob'
/bin/printf 'exit 99\n' >> "$scratch/approved-verifier"
tampered_blob="$(/usr/bin/git hash-object --no-filters -- "$scratch/approved-verifier")"
test "$tampered_blob" != "$approved_blob"
echo 'PASS: modified verifier rejected by immutable Git blob pin'
/usr/bin/env -i PATH=/usr/bin:/bin HOME=/nonexistent /bin/sh -c '
  test -z "${GITHUB_TOKEN:-}"
  test -z "${GH_TOKEN:-}"
  test -z "${ACTIONS_RUNTIME_TOKEN:-}"
  test -z "${JAVA_TOOL_OPTIONS:-}"
  test -z "${LD_PRELOAD:-}"
  test "${HOME:-}" = /nonexistent
'
echo 'PASS: synthetic guest env has no inherited CI tokens or hooks'
test -z "$(/usr/bin/git config --global --get credential.helper 2>/dev/null || true)" || exit 1
echo 'PASS: no global credential helper required'
echo 'PASS: synthetic source-free controls executed; this is not native Oreslang'
