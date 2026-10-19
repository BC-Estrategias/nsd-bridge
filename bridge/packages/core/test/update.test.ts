import { strict as assert } from "node:assert";
import { createHash } from "node:crypto";
import { test } from "node:test";
import { downloadAsset, isNewerVersion, parseVersion, pickDsiAsset, UPDATE_REPOSITORY } from "../src/index.ts";

const BASE = `https://github.com/${UPDATE_REPOSITORY}/releases/download/`;
const D = "0123456789abcdef".repeat(4);
function release(over: Record<string, unknown> = {}, asset: Record<string, unknown> = {}) {
  return {
    tag_name: "v1.7.2", draft: false, prerelease: false,
    assets: [{ name: "nsd-bridge-dsi-v1.7.2.nds", browser_download_url: `${BASE}v1.7.2/nsd-bridge-dsi-v1.7.2.nds`, digest: `sha256:${D}`, size: 388608, ...asset }],
    ...over,
  };
}

test("versions compare as three numbers; anything else is never newer", () => {
  assert.deepEqual(parseVersion("v1.7.2"), [1, 7, 2]);
  assert.equal(parseVersion("1.7"), null);
  assert.equal(isNewerVersion("v1.7.2", "1.7.1"), true);
  assert.equal(isNewerVersion("1.10.0", "1.9.9"), true);
  assert.equal(isNewerVersion("1.7.1", "1.7.1"), false);
  assert.equal(isNewerVersion("1.6.9", "1.7.0"), false);
  assert.equal(isNewerVersion("nope", "1.7.0"), false);
  assert.equal(isNewerVersion("1.7.0-E1", "1.6.0"), false);
});

test("a good release yields the DSi asset, with the digest lower-cased", () => {
  const a = pickDsiAsset(release({}, { digest: `sha256:${D.toUpperCase()}` }));
  assert.ok(a);
  assert.equal(a.version, "v1.7.2");
  assert.equal(a.sha256, D);
  assert.equal(a.size, 388608);
});

test("a release that is not trustworthy yields nothing", () => {
  assert.equal(pickDsiAsset(release({ prerelease: true })), null);
  assert.equal(pickDsiAsset(release({ draft: true })), null);
  assert.equal(pickDsiAsset(release({ tag_name: "latest" })), null);
  assert.equal(pickDsiAsset(release({}, { browser_download_url: "https://evil.example/nsd-bridge-dsi-v1.7.2.nds" })), null);
  assert.equal(pickDsiAsset(release({}, { browser_download_url: `${BASE}v1.8.0/nsd-bridge-dsi-v1.7.2.nds` })), null);
  assert.equal(pickDsiAsset(release({}, { digest: "sha256:zz" })), null);
  assert.equal(pickDsiAsset(release({}, { digest: undefined })), null);
  assert.equal(pickDsiAsset(release({}, { size: 10 })), null);
  assert.equal(pickDsiAsset(release({}, { size: 999999999 })), null);
  assert.equal(pickDsiAsset(release({ assets: [] })), null);
  const twice = release();
  twice.assets.push({ ...twice.assets[0]! });
  assert.equal(pickDsiAsset(twice), null, "two assets with the same name are ambiguous");
  assert.equal(pickDsiAsset(null), null);
  assert.equal(pickDsiAsset("x"), null);
});

function asset(bytes: Uint8Array) {
  return { version: "v1.7.2", name: "nsd-bridge-dsi-v1.7.2.nds", url: `${BASE}v1.7.2/nsd-bridge-dsi-v1.7.2.nds`, sha256: createHash("sha256").update(bytes).digest("hex"), size: bytes.length };
}
const ok = (bytes: Uint8Array) => async () => ({ ok: true, status: 200, json: async () => ({}), arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.length) as ArrayBuffer });

test("a download is accepted only with the declared size and SHA-256", async () => {
  const bytes = new Uint8Array(5000).map((_, i) => i % 251);
  assert.deepEqual(await downloadAsset(asset(bytes), ok(bytes)), bytes);
  const tampered = bytes.slice();
  tampered[10] = (tampered[10] as number) ^ 1;
  await assert.rejects(downloadAsset(asset(bytes), ok(tampered)), /SHA-256/);
  await assert.rejects(downloadAsset(asset(bytes), ok(bytes.slice(0, 4999))), /bytes/);
  await assert.rejects(downloadAsset({ ...asset(bytes), url: "https://evil.example/x.nds" }, ok(bytes)), /outside/);
  await assert.rejects(downloadAsset(asset(bytes), async () => ({ ok: false, status: 404, json: async () => ({}), arrayBuffer: async () => new ArrayBuffer(0) })), /404/);
});
