# ORES Chat embed — gha-indie-worker

This directory makes **gha-indie-worker** an external consumer of the ORES Chat public
component bundle published by [ores-chat](https://github.com/ores-chat) at
`https://ores-chat.github.io/components/v1/`.

Nothing here imports ORES Chat source. The bundle is a framework-neutral custom
element delivered over HTTPS, and this suite treats it the way any third party
would: fetch it, verify it, and hold it to its published contract.

## Surfaces

| Surface | Purpose | Context id | API base |
| --- | --- | --- | --- |
| `visitor` | Marketing-site sales and visitor chat | `gha-indie-worker-visitor` | `https://api.indiebuild.dev/` |
| `support` | Signed-in customer support chat | `gha-indie-worker-support` | `https://api.indiebuild.dev/` |
| `admin` | Owner and operator admin chat | `gha-indie-worker-admin` | `https://admin-api.indiebuild.dev/` |
| `internal-search` | Internal knowledge search chat | `gha-indie-worker-internal-search` | `https://admin-api.indiebuild.dev/` |

`context_id` is what routes a conversation inside ORES Chat. The suite asserts
every id survives `normalizeContextId` unchanged — a typo would otherwise
collapse silently into the shared `public` context and misroute this org's
conversations.

`api_base` values are this organization's public chat origins on `indiebuild.dev`.
Confirm them against the live DNS before the embed is switched on; the suite
validates their **shape** (HTTPS, and that they resolve to the bounded
`v1/public/chat` endpoint), not their reachability, so it stays deterministic.

## What the suite guarantees

1. **Integrity.** The artifact's SHA-256 matches its published `.sha256`,
   `manifest.json`, and `provenance.json`.
2. **A supply-chain pin.** The digest, source repository, source commit, and
   source pull request must equal the values recorded in
   `ores-chat.embed.json`. If ores-chat republishes the bundle, this repository
   goes red until someone reviews the new build and bumps the pin. Recomputing
   the upstream checksums does not defeat this — the pin is held here.
3. **A bounded, credential-free contract.** The bundle still sends
   `credentials: "omit"`, keeps its 4,000-character message and 20-second
   request bounds, carries no React/JSX coupling, and touches no cookie or
   web-storage API.
4. **Per-surface correctness.** Every declared surface produces exactly the
   `ores.chat/v1` request shape, over HTTPS, with no authorization, token,
   secret, cookie, provider, or audience field, and rejects a reply whose
   request id does not match.

## Running it

```bash
cd ores-chat-embed
npm test
```

`ORES_CHAT_COMPONENT_BASE` points the suite at another reviewed distribution
origin. It does **not** relax the pin; waiving that needs
`ORES_CHAT_ALLOW_UNPINNED=1` as a separate, deliberate opt-out.

## Updating the pin

1. Read the upstream change in `ores-chat/ores-chat-external-components`.
2. Copy the new `sha256`, `source_commit`, and `source_pull_request` from the
   published `provenance.json` into `ores-chat.embed.json`.
3. Land it as its own reviewed pull request, never bundled into an unrelated
   change.
