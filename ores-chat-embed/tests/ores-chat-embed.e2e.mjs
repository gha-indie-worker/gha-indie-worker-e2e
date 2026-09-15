/**
 * ORES Chat embed acceptance test.
 *
 * This repository embeds the ORES Chat public component bundle published by
 * github.com/ores-chat. It is an EXTERNAL consumer: it trusts nothing from the
 * ores-chat source tree, only the published artifact and its provenance.
 *
 * The suite fails closed. If ores-chat republishes the bundle without this
 * repository deliberately bumping `ores-chat.embed.json`, the pinned digest
 * stops matching and CI goes red rather than silently adopting new code.
 *
 * Set ORES_CHAT_COMPONENT_BASE only to point at another reviewed distribution
 * origin (used by the ores-chat-test organization to exercise a staging build).
 */
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import test from "node:test";

const descriptor = JSON.parse(
  await readFile(new URL("../ores-chat.embed.json", import.meta.url), "utf8"),
);

const base = new URL(process.env.ORES_CHAT_COMPONENT_BASE ?? descriptor.bundle.base_url);
if (!base.pathname.endsWith("/")) base.pathname += "/";

assert.equal(
  new URL(descriptor.bundle.base_url).protocol,
  "https:",
  "the pinned distribution origin must use HTTPS",
);

// The digest pin is waived only by an explicit, separate opt-out. Pointing the
// suite at a staging origin is NOT on its own enough to drop the pin, so a
// stray ORES_CHAT_COMPONENT_BASE cannot quietly disarm supply-chain review.
const pinWaived = process.env.ORES_CHAT_ALLOW_UNPINNED === "1";

async function fetchText(path) {
  const response = await fetch(new URL(path, base), {
    redirect: "error",
    signal: AbortSignal.timeout(20_000),
  });
  assert.equal(response.status, 200, `${path} must be publicly available at ${base.href}`);
  return response.text();
}

const artifactName = descriptor.bundle.artifact;

test("published bundle matches the digest this repository pinned", async () => {
  const [artifact, checksumFile, manifestText, provenanceText] = await Promise.all([
    fetchText(artifactName),
    fetchText(`${artifactName}.sha256`),
    fetchText("manifest.json"),
    fetchText("provenance.json"),
  ]);

  const digest = createHash("sha256").update(artifact).digest("hex");
  const checksum = checksumFile.trim().split(/\s+/)[0];
  const manifest = JSON.parse(manifestText);
  const provenance = JSON.parse(provenanceText);

  // The distribution must be internally consistent.
  assert.equal(digest, checksum, "artifact digest must match the published .sha256");
  assert.equal(manifest.schema_version, 1);
  assert.equal(manifest.package, descriptor.bundle.package);
  const entry = manifest.artifacts.find((candidate) => candidate.path === artifactName);
  assert.ok(entry, `manifest must describe ${artifactName}`);
  assert.equal(entry.sha256, digest, "manifest digest must match the artifact");
  assert.deepEqual(entry.surfaces, descriptor.bundle.expected_surfaces);
  assert.equal(provenance.sha256, digest, "provenance digest must match the artifact");
  assert.equal(provenance.artifact, artifactName);

  if (pinWaived) return;

  // The supply-chain pin. Bumping these values is a reviewed decision.
  assert.equal(
    digest,
    descriptor.bundle.sha256,
    "published digest changed; review the new ORES Chat bundle and bump ores-chat.embed.json",
  );
  assert.equal(provenance.source_repository, descriptor.bundle.source_repository);
  assert.equal(provenance.source_commit, descriptor.bundle.source_commit);
  assert.equal(provenance.source_pull_request, descriptor.bundle.source_pull_request);
});

test("bundle keeps the bounded, credential-free public contract", async () => {
  const artifact = await fetchText(artifactName);

  assert.match(artifact, /customElements\.define\("ores-chat-footer-link"/);
  assert.match(artifact, /v1\/public\/chat/);
  assert.match(artifact, /credentials:\s*"omit"/);
  assert.match(artifact, /MAX_MESSAGE_LENGTH\s*=\s*4_000/);
  assert.match(artifact, /REQUEST_TIMEOUT_MS\s*=\s*20_000/);
  // No ambient authority and no framework coupling.
  assert.doesNotMatch(artifact, /from\s+["']react|react-dom|\.tsx|\.jsx/i);
  assert.doesNotMatch(artifact, /document\.cookie|localStorage|sessionStorage/);
});

test("every declared surface resolves to a valid bounded request", async () => {
  const artifact = await fetchText(artifactName);
  const live = await import(
    `data:text/javascript;base64,${Buffer.from(artifact).toString("base64")}`
  );

  assert.ok(descriptor.surfaces.length > 0, "at least one embed surface must be declared");
  const seen = new Set();

  for (const surface of descriptor.surfaces) {
    const label = `surface ${surface.name}`;

    assert.equal(seen.has(surface.name), false, `${label} is declared twice`);
    seen.add(surface.name);

    // A context id that does not survive normalization would silently collapse
    // to the shared "public" context and misroute this org's conversations.
    assert.equal(
      live.normalizeContextId(surface.context_id),
      surface.context_id,
      `${label}: context_id "${surface.context_id}" is not a valid ORES Chat context`,
    );

    const endpoint = live.buildMessageEndpoint(surface.api_base);
    assert.equal(
      endpoint,
      `${surface.api_base.replace(/\/?$/, "/")}v1/public/chat`,
      `${label}: api_base must resolve to the bounded public chat endpoint`,
    );
    assert.ok(endpoint.startsWith("https://"), `${label}: endpoint must be HTTPS`);

    const requestId = `request_${surface.name}`;
    const request = live.buildPublicChatRequest({
      requestId,
      contextId: surface.context_id,
      message: "  How do I get help?  ",
    });
    assert.deepEqual(
      request,
      {
        protocol: "ores.chat/v1",
        request_id: requestId,
        message: "How do I get help?",
        context_refs: [{ id: surface.context_id }],
      },
      `${label}: request shape drifted from ores.chat/v1`,
    );

    // The public surface must never carry authority.
    const serialized = JSON.stringify(request).toLowerCase();
    for (const forbidden of ["authorization", "token", "secret", "cookie", "provider", "audience"]) {
      assert.equal(
        serialized.includes(forbidden),
        false,
        `${label}: public request must not carry "${forbidden}"`,
      );
    }

    // Replies are bound to their request.
    assert.equal(
      live.extractAssistantReply(
        { protocol: "ores.chat/v1", request_id: requestId, answer: " Verified " },
        requestId,
      ),
      "Verified",
    );
    assert.throws(
      () => live.extractAssistantReply(
        { protocol: "ores.chat/v1", request_id: "request_other", answer: "Wrong request" },
        requestId,
      ),
      TypeError,
      `${label}: a mismatched request id must be rejected`,
    );
  }
});

test("message bounds are enforced for this org's surfaces", async () => {
  const artifact = await fetchText(artifactName);
  const live = await import(
    `data:text/javascript;base64,${Buffer.from(artifact).toString("base64")}`
  );
  const [surface] = descriptor.surfaces;

  assert.throws(
    () => live.buildPublicChatRequest({
      requestId: "request_bounds",
      contextId: surface.context_id,
      message: "x".repeat(4_001),
    }),
    RangeError,
  );
  assert.throws(
    () => live.buildPublicChatRequest({
      requestId: "request_bounds",
      contextId: surface.context_id,
      message: "   ",
    }),
    RangeError,
  );
  assert.throws(
    () => live.buildPublicChatRequest({
      requestId: "not a valid id",
      contextId: surface.context_id,
      message: "hello",
    }),
    TypeError,
  );
});
