#!/usr/bin/env bash
set -euo pipefail

fail() { printf '%s\n' "FAIL: $*" >&2; exit 1; }

# This probe intentionally contains no Oreslang source, repository IDs, private
# tree hashes, test vectors, or proprietary workflow bodies. It tests only the
# generic security properties used by a private-source carrier.

is_approved_destination() {
  local file="$1" wanted="$2"
  awk '
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*$/ { next }
    { print $1 }
  ' "$file" | grep -Fxq -- "$wanted"
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/approved.txt" <<'EOF'
# No repositories are approved by default.
# Review-only candidate (NOT approved): example/test-carrier
EOF

if is_approved_destination "$tmp/approved.txt" "example/test-carrier"; then
  fail "commented candidate was treated as approved"
fi

printf '%s\n' "example/approved-carrier 123456" >>"$tmp/approved.txt"
is_approved_destination "$tmp/approved.txt" "example/approved-carrier" ||
  fail "explicit approval was not recognized"

cat >"$tmp/good-workflow.yml" <<'EOF'
- uses: actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683
  with:
    ref: ${{ github.event.pull_request.head.sha || github.sha }}
    persist-credentials: false
EOF

grep -Fq 'actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683' "$tmp/good-workflow.yml"
grep -Fq 'ref: ${{ github.event.pull_request.head.sha || github.sha }}' "$tmp/good-workflow.yml"
grep -Fq 'persist-credentials: false' "$tmp/good-workflow.yml"

cat >"$tmp/bad-workflow.yml" <<'EOF'
- uses: actions/checkout@v4
EOF

if grep -Eq 'uses:[[:space:]]+actions/checkout@v[0-9]+' "$tmp/bad-workflow.yml"; then
  :
else
  fail "moving checkout tag negative fixture was not detected"
fi

cat >"$tmp/CODEOWNERS.good" <<'EOF'
/.github/workflows/ @ORESoftware
/src/ @ORESoftware
/compiler/ @ORESoftware
/runtime/ @ORESoftware
/tests/ @ORESoftware
/contracts/ @ORESoftware
EOF

for path in '/.github/workflows/' '/src/' '/compiler/' '/runtime/' '/tests/' '/contracts/'; do
  grep -Fq "$path @ORESoftware" "$tmp/CODEOWNERS.good" ||
    fail "missing security ownership for $path"
done

# A source-free carrier may carry this probe, but not a private source snapshot.
mkdir -p "$tmp/carrier"
printf '%s\n' 'security-primitives-only' >"$tmp/carrier/manifest.txt"
if find "$tmp/carrier" -type f \( -name '*.ores' -o -name '*.java' -o -name '*.cpp' -o -name '*.cs' -o -name '*.rs' -o -name '*.wasm' \) | grep -q .; then
  fail "source-free carrier contains a private-source-like file"
fi

printf '%s\n' "source-free private-CI security primitive probe: PASS"
