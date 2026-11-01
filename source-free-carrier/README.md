# Source-free Oreslang hardening probe

This directory validates generic CI security primitives only: exact-head checkout,
immutable action pins, fail-closed destination approval parsing, CODEOWNERS coverage,
and the rule that an alternate-organization capacity probe must not contain a private
Oreslang source snapshot.

It intentionally contains no Oreslang source files, private tree hashes, repository
IDs, proprietary fixtures, or cross-organization credentials.

The carrier inventory rejects symlinks (including the carrier root), special files,
and case-variant source-code extensions. Synthetic positive/negative fixtures only
exercise that policy; real checkout identity, exact-head CI steps, and authorization
must be verified separately. A source-free pass never certifies private guest code.

