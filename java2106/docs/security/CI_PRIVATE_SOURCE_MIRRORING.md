# Private CI mirrors: deny-by-default policy

Never transfer private Oreslang source into a public or internal GitHub
repository, including temporary refs, dangling Git objects, Actions artifacts,
cache entries, and logs.

## Destination approval

The policy file `.github/private-ci-approved-repositories.txt` is **empty by
default**. No cross-repository mirroring is authorized until maintainers and
the independent security owner `@ORESoftware` explicitly review an exact
`owner/repo numeric-repository-id` destination.

An allowlist line is necessary but not sufficient. The reviewed
`.github/private-ci-carrier-contracts.json` independently pins the live
destination repository ID, an exact source-free carrier `main` commit, the
carrier workflow blob, and exact test-plan blob/PR sets. Carrier `main`
advance, repository delete/recreate, workflow change, or test-plan change
fails closed until the source policy is reviewed again.

V3 requires **protected source `main` for every private-source export**.
If GitHub reports source `main protected: true`, V3 delegates to the legacy
protected-origin verifier. It requires CODEOWNER review, stale-review dismissal,
at least one approval, strict non-empty required status checks, administrator
enforcement, no review bypasses, no force pushes, and no branch deletion.

There is deliberately no review-attested fallback for the source repository.
A reviewed governance PR, CODEOWNERS text, or successful source-free carrier
run cannot substitute for live source-main protection. If source `main`
reports `protected: false` or the required protection cannot be verified,
private export is denied.

Carrier `main` has a separate fail-closed choice: the unprotected fallback
is bound by the **carrier contract**, not by an implicit trusted branch. If the
carrier reports `protected: true`, V3 requires CODEOWNER review, stale-review
dismissal, at least one approval, administrator enforcement, no review
bypasses, no force pushes, no deletion, and the exact security CODEOWNERS file.
If those private-repository protections are unavailable, the protected source
contract must include an exact positive `review_pr`. That PR must be a
same-repository PR merged to carrier `main`, authored by an account other than
immutable reviewer ID `11139560` / login `ORESoftware`, and the latest
review from that reviewer must be `APPROVED` on the PR's exact head before
merge. V3 then requires the **entire final carrier main root-tree SHA** to equal
the reviewed PR head root-tree SHA and requires the exact carrier CODEOWNERS
bytes. Thus path, Git mode, or blob drift after review fails closed.

The exact carrier-main SHA is rechecked before every source-bearing destination
write. Full carrier review/tree attestation is repeated at initial admission,
immediately before executable refs, and again while the source-bearing payload
ref is still inert before creating the one-use authorization ref. Handoff ref
cleanup is attempted for ordinary authorization failures, transport ambiguity,
interrupts, and unexpected exceptions; both payload and authorization refs are
removed when they still point to the exact handoff commit, and incomplete
rollback fails closed.

At the 2026-10-20 audit, source `main` still reported `protected: false`,
and GitHub reported private-repository rulesets unavailable on the current
plan. Therefore real V3 export is blocked until source-main protection is
enabled and independently verifiable. Source-free carrier tests remain
permissible while that governance blocker is unresolved.

Do **not** accept a CI destination allowlist from environment variables or
unreviewed workflow inputs: a caller must not be able to grant its own approval.
Run the checked transfer from trusted, reviewed source, not an untrusted PR
checkout. If an approval policy can be modified by the code under test, treat
the result as untrusted.

The legacy publisher also treats checkout policy as **data only**: it does not
execute `scripts/check-private-ci-destination.sh` from the candidate checkout
as part of privileged authorization. The shell checker remains a source-free
operator/CI diagnostic. Publication reads the approval through no-follow
descriptor-safe Python, validates the pinned destination repository identity,
then independently verifies protected-origin governance before reading private
source bytes. A PR-controlled shell script must never be an authorization
primitive for its own export.

## Destination-bound source snapshot publisher

The only supported transfer entry point is **not an arbitrary command
launcher**. The direct CLI in `scripts/publish-private-ci-source.py` is
unconditionally disabled; that module remains importable only for shared
verification helpers and offline adversarial tests. V3 accepts an approved
destination, a bounded local source directory, the exact open source PR head,
and one test plan already pinned by the protected carrier contract:

```bash
scripts/run-in-private-ci-destination.sh opto-sync-test/go-java-validation \
  --source-dir ./verified-source-tree \
  --source-commit FULL_40_CHARACTER_OPEN_PR_HEAD_SHA \
  --source-pr 1482 \
  --test-plan ci/test-plans/java-pr1482.json
```

The caller does not choose the executable carrier branch. After proving the
complete source Git tree, the dispatcher derives exactly
`ci/oreslang-<SOURCE_ROOT_TREE_SHA>`. The exact source commit must also be
the current head of the named open PR against source `main`, with the latest
immutable security-owner review approving that exact head. Protected source and
carrier `main` must require CODEOWNER review, stale-review dismissal,
last-push approval, at least one approval, review-thread/conversation resolution,
administrator enforcement, no force-push/deletion/review bypass, and the exact
issuer-bound required security check. V3 validates that authorization before
reading source and again before **every source-bearing Git object write**. Each
per-write check also reattests the original protected-main authority and the
reviewed control-plane/carrier-contract bytes. Weakening source or carrier
protection, moving source or carrier `main`, changing the reviewed control
plane or carrier contract, closing the PR, moving its head, or
dismissing/revoking the exact-head approval therefore stops the next persistent
destination write. V3
also rechecks once more immediately before executable carrier refs are created.

The checked-in policy file must explicitly list the destination. The
publisher checks the live GitHub identity and `visibility=private` /
`private=true` before sending any private bytes and rechecks visibility
immediately before each write. Every GitHub API write is constructed from
that **same validated owner/repo**, rather than a caller-provided URL, Git
remote, shell command or separate destination argument. It creates blobs,
then a **fresh standalone source tree with no `base_tree`**, followed by a
root tree that replaces the **entire** `private-oreslang-source/` directory
in the destination's default tree. This removes stale files from previous
snapshots while preserving unrelated destination workflows and files.
The v3 dispatcher creates a canonical `ores-private-ci-receipt-v3` that
binds source repository ID, PR number/head SHA/root-tree SHA, manifest SHA-256,
destination repository ID and reviewed base commit, immutable carrier workflow
blob, reviewed test-plan digest, and a 24-hour expiry. The receipt digest is
bound into the carrier commit message. It then creates a **new**
`ci/oreslang-<SOURCE_ROOT_TREE_SHA>` ref and the exact
`ci/authorized-<CARRIER_COMMIT_SHA>` dispatcher authorization ref required
by the carrier. Existing branches, including `main`, are never updated.

The reviewed carrier base must itself be source-free: neither
`private-oreslang-source/` nor `ci/oreslang-receipt.json` may already be
present. The source-bearing commit may change only that source subtree plus
the receipt; carrier workflow and test-plan bytes remain inherited from the
pinned base.

## Carrier checkout and execution isolation

A private carrier repository may contain unrelated private project files. Those
bytes are **not** implicitly authorized for disclosure to Oreslang code under
test. The pinned carrier workflow therefore has to enforce a separate
confidentiality boundary in addition to source-transfer authorization.

The source-executing job must have read-only repository permissions and must
materialize only the exact carrier control files, receipt/test plan, and
`private-oreslang-source/` subtree needed for the run. When the carrier shares
a repository with unrelated private material, use a blob-filtered partial clone
plus sparse checkout; a sparse worktree alone is insufficient because omitted
blobs may still exist in `.git/objects`. Authentication must be command-scoped
for the fetch/checkout only, and the Git remote / credential configuration must
be removed before any source-under-test command runs. The test subprocess gets
a fresh HOME/XDG config domain and no token/secret/authentication environment.

The reviewed workflow must prove before test execution that representative
unrelated carrier paths are absent both from the worktree and from the local Git
object database. The pinned workflow blob is part of the protected carrier
contract, so replacing this isolation logic requires a new source-policy review.

Reference cleanup is deliberately privilege-separated: the source-executing job
does **not** receive write permission. A separate no-checkout cleanup job may
receive narrowly scoped `contents: write` solely to verify and remove the exact
`ci/authorized-<COMMIT>` and `ci/oreslang-<TREE>` refs after the run, including
failed runs. Moving either ref causes cleanup to fail closed rather than delete
an unexpected target.

Deleting refs is hygiene, not erasure. Uploaded Git objects can remain retained
and recoverable after the refs disappear. Destination authorization therefore
authorizes a potential persistent private-source copy in the carrier repository;
administrators must not describe ref cleanup as secure deletion.

### Network and build-tool isolation

The source under test is treated as potentially hostile even after an exact-tree
handoff. It must never receive the carrier token, the runner user's HOME, the
carrier's unrelated private files, or normal outbound Internet access.

For Maven plans, the reviewed test plan must bind the exact `pom.xml` Git blob
and every Maven command must use `-o` / `--offline`. Dependency preparation
runs before source execution in a separate read-only job that materializes only
the receipt, plan, carrier dependency controller, and the reviewed `pom.xml`.
That controller uses a fresh HOME, rejects project-defined Maven repositories,
plugin repositories, distribution management, and build extensions, forces
`mirrorOf=*` to Maven Central, enables strict checksum verification, and invokes
an exact `maven-dependency-plugin` coordinate. The resulting local repository is
the only dependency artifact passed to the source-execution job.

The source-execution job verifies the complete receipt/tree first, then copies
the already-verified source into a dedicated gitless sandbox. The untrusted
process runs as a newly-created non-sudo account under Linux `unshare --net`
with `no_new_privs` and all inheritable, ambient, and bounding capabilities
removed. It receives a fresh HOME/TMPDIR and a narrow environment with no
credential-like variables. The trusted plan runner and receipt/plan copies are
read-only and live outside the writable source directory. The carrier workflow
must fail if the untrusted account can read the original Actions workspace.

This split is intentional: the one networked phase can resolve dependencies but
cannot see the private source tree beyond an exact reviewed build descriptor;
the phase that can execute arbitrary private source has no external network and
no privilege path back to the runner.

An explicitly supplied `--source-commit` now pins the **exact original
private Git commit**. Before uploading, the publisher retrieves that commit's
source-tree hash and its complete, non-truncated recursive tree from the
official origin. It rejects untracked/dirty/missing local files, differing
executable modes, symlinks/submodules, or an incomplete tree. Every local
path/mode/blob hash must match the origin tree exactly. After uploading,
the newly assembled isolated subtree must have the **same Git tree SHA**
as the verified original, before the private destination's root commit/ref
can be created. No transfer is authorized merely because a filesystem
directory has a trusted-looking name.

The publisher bounds the export to 1,024 files / 40 MiB total / 10 MiB
per file, requires the source directory to remain **inside the trusted
checkout**, and walks the source **through directory file descriptors** to
prevent parent-directory symlink swap races. It rejects symbolic links,
hardlinks to outside files, FIFOs, `.git` directories, and other unsafe
filesystem objects. Directory enumeration is bounded to 4,096 entries,
1,024 directories and a maximum depth of 64. It verifies every uploaded
Git blob SHA-1 against the source bytes. It checks that the reviewed
destination policy has not changed
before every API write, in addition to rechecking the destination's live
private visibility. It does not print source contents in API failures.
API failure before ref creation can leave unreferenced Git objects in
the private destination; a follow-up retention policy is still needed.

It does **not** intercept arbitrary connector/API tool writes, cannot
completely prevent visibility changes between API checks, and cannot
guarantee that the source directory itself is free of secrets. Run only from a
checkout whose private-CI governance exactly matches the protected source
`main` accepted by V3, using narrowly scoped credentials. The destination
carrier `main` must also be protected; exact-head review or tree attestation is
evidence, not a substitute for enforced carrier protection.

Private repositories must be hosted on a GitHub plan/account that actually
supports the required protected-branch/ruleset policy. If GitHub reports that
private protection requires a plan upgrade, publication fails closed until that
capability is enabled. There is no permission to make source or carrier public
to work around plan limits or exhausted CI minutes.

Test without network access or sending any source:

```bash
bash scripts/test-private-ci-destination.sh
python3 scripts/test-publish-private-ci-source.py
python3 scripts/test-publish-private-ci-source-v3.py
```

The Bash test uses a temporary policy checkout; Python tests mock all GitHub
API operations. V3 tests additionally prove protected carrier-contract parsing,
source-free base enforcement, exact open-PR binding, receipt construction,
tree-derived branch naming, and dispatcher-authorization ref creation. The
real allowlist remains empty and **no destination is authorized** unless
separately reviewed and both source and carrier protected-main enforcement are
satisfied. Exact-tree review evidence never substitutes for protected carrier
`main`.

## Active disclosure incident

Private issue #832 tracks source-bearing commits in a public CI repository.
Resetting branch references did **not** remove the Git objects: commits were
still retrievable by SHA. Administrators must restrict public access and seek
GitHub Support assistance for retained Git objects, Actions caches/logs, and
other copies before resolving the incident.

