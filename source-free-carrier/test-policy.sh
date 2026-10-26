#!/usr/bin/env bash
set -euo pipefail

fail() { printf '%s\n' "FAIL: $*" >&2; exit 1; }

# This probe intentionally contains no Oreslang source, repository IDs, private
# tree hashes, test vectors, or proprietary workflow bodies. It tests only the
# generic security properties used by a private-source carrier.

is_approved_destination() {
  local file="$1" wanted="$2" immutable_id="$3"
  # Fail closed on malformed ACTIVE entries, not merely a matching first field.
  awk -v want="$wanted" -v id="$immutable_id" '
    /^[[:space:]]*(#|$)/ { next }
    NF != 2 || $1 !~ /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/ ||
      $2 !~ /^[1-9][0-9]*$/ { bad = 1; next }
    $1 == want { slug_count++; if ($2 == id) matched++ }
    END { exit (bad || slug_count != 1 || matched != 1) ? 1 : 0 }
  ' "$file"
}

validate_codeowners() {
  local file="$1"
  # Literal escaped newlines join two patterns into one invalid GitHub rule.
  if grep -Fq '\n' "$file"; then return 1; fi
  awk '
    /^[[:space:]]*(#|$)/ { next }
    NF != 2 || $1 !~ /^\// || $2 != "@ORESoftware" { bad = 1; next }
    { seen[$1]++ }
    END {
      n = split("/.github/workflows/ /.github/CODEOWNERS /.github/private-ci-approved-repositories.txt /.github/workflows/private-ci-policy.yml /src/ /compiler/ /runtime/ /tests/ /contracts/", required, " ")
      for (i = 1; i <= n; ++i)
        if (seen[required[i]] != 1) bad = 1
      exit bad ? 1 : 0
    }
  ' "$file"
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/approved.txt" <<'EOF'
# No repositories are approved by default.
# Review-only candidate (NOT approved): example/test-carrier
EOF

if is_approved_destination "$tmp/approved.txt" "example/test-carrier" "123456"; then
  fail "commented candidate was treated as approved"
fi

printf '%s\n' "example/approved-carrier 123456" >>"$tmp/approved.txt"
is_approved_destination "$tmp/approved.txt" "example/approved-carrier" "123456" ||
  fail "exact slug and immutable numeric ID not recognized"
if is_approved_destination "$tmp/approved.txt" "example/approved-carrier" "123457"; then
  fail "numeric repository ID mismatch was accepted"
fi
printf '%s\n' "example/approved-carrier malformed" >>"$tmp/approved.txt"
if is_approved_destination "$tmp/approved.txt" "example/approved-carrier" "123456"; then
  fail "malformed active policy entry was accepted"
fi
sed -i '$d' "$tmp/approved.txt"
printf '%s\n' "example/approved-carrier 123456" >>"$tmp/approved.txt"
if is_approved_destination "$tmp/approved.txt" "example/approved-carrier" "123456"; then
  fail "duplicate destination approval was accepted"
fi
sed -i '$d' "$tmp/approved.txt"
is_approved_destination "$tmp/approved.txt" "example/approved-carrier" "123456" ||
  fail "valid approval unexpectedly disappeared after duplicate test"
printf '%s\n' "example/approved-carrier 999999" >>"$tmp/approved.txt"
if is_approved_destination "$tmp/approved.txt" "example/approved-carrier" "123456"; then
  fail "conflicting repository ID for same slug was accepted"
fi
sed -i '$d' "$tmp/approved.txt"
is_approved_destination "$tmp/approved.txt" "example/approved-carrier" "123456" ||
  fail "conflicting-ID test did not restore the original approval"

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
/.github/CODEOWNERS @ORESoftware
/.github/private-ci-approved-repositories.txt @ORESoftware
/.github/workflows/private-ci-policy.yml @ORESoftware
/src/ @ORESoftware
/compiler/ @ORESoftware
/runtime/ @ORESoftware
/tests/ @ORESoftware
/contracts/ @ORESoftware
EOF

validate_codeowners "$tmp/CODEOWNERS.good" ||
  fail "valid critical ownership policy was rejected"

cp "$tmp/CODEOWNERS.good" "$tmp/CODEOWNERS.bad"
printf '%s\n' '/.github/workflows/ @another-owner' >>"$tmp/CODEOWNERS.bad"
if validate_codeowners "$tmp/CODEOWNERS.bad"; then
  fail "conflicting late ownership override was accepted"
fi

cp "$tmp/CODEOWNERS.good" "$tmp/CODEOWNERS.bad"
printf '%s\n' '/.github/workflows/ @ORESoftware' >>"$tmp/CODEOWNERS.bad"
if validate_codeowners "$tmp/CODEOWNERS.bad"; then
  fail "duplicate critical ownership rule was accepted"
fi

cp "$tmp/CODEOWNERS.good" "$tmp/CODEOWNERS.bad"
printf '%s\n' '/.github/private-ci-approved-repositories.txt @ORESoftware\\n/.github/workflows/private-ci-policy.yml @ORESoftware' >>"$tmp/CODEOWNERS.bad"
if validate_codeowners "$tmp/CODEOWNERS.bad"; then
  fail "literal escaped CODEOWNERS newline was accepted"
fi

grep -vFx '/contracts/ @ORESoftware' "$tmp/CODEOWNERS.good" >"$tmp/CODEOWNERS.bad"
if validate_codeowners "$tmp/CODEOWNERS.bad"; then
  fail "missing critical ownership rule was accepted"
fi

grep -vFx '/.github/private-ci-approved-repositories.txt @ORESoftware' "$tmp/CODEOWNERS.good" >"$tmp/CODEOWNERS.bad"
if validate_codeowners "$tmp/CODEOWNERS.bad"; then
  fail "missing private CI allowlist ownership was accepted"
fi

grep -vFx '/.github/workflows/private-ci-policy.yml @ORESoftware' "$tmp/CODEOWNERS.good" >"$tmp/CODEOWNERS.bad"
if validate_codeowners "$tmp/CODEOWNERS.bad"; then
  fail "missing private CI workflow ownership was accepted"
fi

# A source-free carrier may carry this probe, but not a private source snapshot.
mkdir -p "$tmp/carrier"
printf '%s\n' 'security-primitives-only' >"$tmp/carrier/manifest.txt"
if find "$tmp/carrier" -type f \( -name '*.ores' -o -name '*.java' -o -name '*.cpp' -o -name '*.cs' -o -name '*.rs' -o -name '*.wasm' \) | grep -q .; then
  fail "source-free carrier contains a private-source-like file"
fi

# Check the real checked-out carrier too; the synthetic directory above is
# only a negative fixture and cannot establish source-free provenance by itself.
if find source-free-carrier -type f \( -name '*.ores' -o -name '*.java' -o -name '*.cpp' -o -name '*.cs' -o -name '*.rs' -o -name '*.wasm' \) | grep -q .; then
  fail "checked-out carrier contains source-like files"
fi
if grep -REq 'model_for_(llvm|java)_h[e]ad=[0-9a-f]{40}' source-free-carrier; then
  fail "checked-out carrier embeds an exact private compiler head"
fi

printf '%s\n' "source-free private-CI security primitive probe: PASS"
