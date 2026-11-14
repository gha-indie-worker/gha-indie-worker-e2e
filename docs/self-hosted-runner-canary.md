# Self-hosted runner YAML conformance: controlled canary

This is a **runbook**, not evidence that a self-hosted runner has executed.
The diagnostic workflow lives at
`.github/workflows/manual-runner-yaml-conformance.yml` and is manual-only.
It has no checkout, third-party actions, API calls, secrets, pull-request
trigger, or caller-defined shell command. Workflow `permissions: {}`
keeps the token read-only in effect (no explicit repository permissions).

## Preconditions

1. Obtain independent review and merge the source-free diagnostic workflow
   to the repository default branch. GitHub only offers `workflow_dispatch`
   from a workflow present on the default branch.
2. Have an **authorized**, isolated, ephemeral runner registered under the
   repository or its permitted organization group. Run on a reviewed Linux
   runner matching the `self-hosted` and `Linux` labels. The macOS canary
   requires `self-hosted` and `macOS`. No runner installation or registration
   token is provisioned by this probe.
3. Reserve enough runner capacity for **two sequential jobs** (each can run
   on a separate eligible runner), and ensure the runner workspace is cleaned
   between jobs. Do not run this workflow on an untrusted PR branch.
4. Confirm source repository and runner policies allow an explicit
   `workflow_dispatch` event. Do not disable branch protection or leak
   registration tokens to make a canary start.

## Invocation

From the repository's **Actions** tab, open **Manual runner YAML
conformance**, select **Run workflow**, choose the reviewed default branch,
and select `self-hosted-linux`. Repeat with `self-hosted-macos` only after
Linux has completed and a macOS runner is authorized.

The `github-hosted-ubuntu` input is a baseline comparator rather than
evidence of a self-hosted runner. Hosted PR CI additionally exercises
job dependencies, step outputs, conditional execution, and the distinction
between a deliberately failed step's `outcome` and continued `conclusion`.

## Evidence to record

For each run, record the exact default-branch SHA, workflow run URL,
GitHub event name `workflow_dispatch`, selected runner input,
job IDs, runner names/labels shown in GitHub's job details, and terminal
conclusions. Both `probe` and dependent `verify` must finish
`success`. The step summary must say runner YAML conformance passed,
and `verify` must enforce that `needs.probe.outputs.ready == 'true'`
and its conditional step emitted `passed=yes`.

A queued run, skipped job, startup failure, runner-offline event, timed-out
job, or only the successful hosted comparator is **not** self-hosted
conformance evidence. Preserve the GitHub URLs instead of inventing metrics.

## Failure and rollback

- If a run queues indefinitely, confirm eligible runner labels, scope and
  capacity. Do not relax label matching to an unreviewed machine.
- If an isolated runner loses contact, retire the ephemeral runner and
  examine GitHub's actual job verdict and local runner diagnostics.
  A job's transition to a terminal state is separate from proof that
  the worker's application-level job state was durably recovered.
- If outputs or conditions fail, retain the run as a regression and keep
  the workflow draft/disable dispatch until reviewed.
- Worker cancellation, force-kill recovery, lease fencing, webhook replay,
  database durability and retry idempotency are **separate** integration
  scenarios. This source-free YAML probe does not certify them.

No real runner is provisioned, activated, or registered by this document.
