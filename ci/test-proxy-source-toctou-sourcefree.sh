#!/usr/bin/env bash
# PUBLIC SYNTHETIC SOURCE ONLY. No private proxy source or Oreslang runtime.
set -euo pipefail
export PATH=/usr/bin:/bin
[[ $(id -u) == 0 ]] || { echo 'requires ephemeral CI root for separate-uid probe'; exit 77; }
work="$(mktemp -d /tmp/proxy-source-toctou-XXXXXX)"
trap 'rm -rf -- "$work"' EXIT
cd "$work"
git init -q
git config user.name 'Local Synthetic Boundary Test'
git config user.email synthetic@example.invalid
mkdir src
printf '%s\n' 'printf "APPROVED_SOURCE\n"' > src/guest.sh
git add src/guest.sh
git commit -qm 'immutable synthetic source'
immutable_blob="$(git rev-parse HEAD:src/guest.sh)"
verify_bytes() {
  test "$(git rev-parse HEAD:src/guest.sh)" = "$immutable_blob"
  git show HEAD:src/guest.sh | cmp -s -- src/guest.sh -
}
verify_bytes
# Same-identity source swap between verification and use (deliberately unsafe).
printf '%s\n' 'printf "REPLACED_SOURCE\n"' > src/guest.sh
readback="$(/bin/sh src/guest.sh)"
test "$readback" = REPLACED_SOURCE
if verify_bytes; then echo 'FAIL: post-use recheck missed modification'; exit 1; fi
echo 'PASS: same-identity source swap reached synthetic launched code'
# Root-owned source with a separate guest identity prevents source replacement.
git checkout -- src/guest.sh
verify_bytes
chown root:root "$work" "$work/src" "$work/src/guest.sh"
chmod 0755 "$work"
chmod 0555 "$work/src"
chmod 0444 "$work/src/guest.sh"
if /usr/sbin/runuser -u nobody -- /bin/sh -c 'printf "%s\n" BAD > "$1/src/guest.sh"' -- "$work" 2>/dev/null; then
  echo 'FAIL: unprivileged guest modified root-owned approved source'; exit 1
fi
if /usr/sbin/runuser -u nobody -- /bin/sh -c 'mv "$1/src/guest.sh" "$1/src/replaced.sh"' -- "$work" 2>/dev/null; then
  echo 'FAIL: unprivileged guest renamed root-owned approved source'; exit 1
fi
readback="$(/usr/sbin/runuser -u nobody -- /bin/sh "$work/src/guest.sh")"
test "$readback" = APPROVED_SOURCE
verify_bytes
echo 'PASS: separate-UID write and rename denied; healthy approved placeholder executed'
echo 'EVIDENCE_CLASS=PUBLIC_SYNTHETIC_HOST_ONLY'
echo 'NATIVE_ORESLANG_EXECUTION=false'
