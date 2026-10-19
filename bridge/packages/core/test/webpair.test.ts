// The page's own pairing by number comparison (web/js/x25519.js + auth.js + client.js) in Node: vectors, then the whole thing
// against the C host agent over a WebSocket, and cross-checked with the TypeScript client (node:crypto) so the two clients derive
// exactly what the console derives.
import { strict as assert } from "node:assert";
import { readFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, describe, test } from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";
import { type ChildProcess } from "node:child_process";
import { NdpClient, x25519Public, x25519Shared } from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const WEB = fileURLToPath(new URL("../../../../web/js/", import.meta.url));
for (const f of ["sha256.js", "codec.js", "x25519.js", "auth.js", "client.js"]) await import(pathToFileURL(WEB + f).href);
const NDP = (globalThis as any).NDP;
const V = JSON.parse(readFileSync(fileURLToPath(new URL("../../../../docs/protocol/test-vectors/vectors.json", import.meta.url)), "utf8"));
const fromHex = (h: string) => new Uint8Array(Buffer.from(h, "hex"));
const hex = (b: Uint8Array) => Buffer.from(b).toString("hex");
const suite = SKIP ? describe.skip : describe;

test("web/js X25519 and pairing derivations agree with the RFC 7748 vectors, the Python reference and node:crypto", () => {
  const X = NDP.x25519, A = NDP.auth;
  for (const e of V.pair2.x25519) assert.equal(hex(X.shared(fromHex(e.scalar_hex), fromHex(e.point_hex))), e.out_hex);
  for (const lo of V.pair2.x25519_low_order) assert.equal(X.shared(fromHex(V.pair2.x25519[0].scalar_hex), fromHex(lo)), null, "low-order points are refused");
  // random keys: the page and node:crypto agree on public keys and shared secrets
  for (let i = 0; i < 6; i++) {
    const a = new Uint8Array(32), b = new Uint8Array(32);
    crypto.getRandomValues(a);
    crypto.getRandomValues(b);
    assert.equal(hex(X.publicKey(a)), hex(x25519Public(a)));
    assert.equal(hex(X.shared(a, X.publicKey(b))), hex(x25519Shared(a, x25519Public(b))!));
  }
  for (const p of V.pair2.pair2) {
    const sb = fromHex(p.secret_b_hex), sc = fromHex(p.secret_c_hex), label = new TextEncoder().encode(p.label);
    const pubB = X.publicKey(sb), pubC = X.publicKey(sc);
    assert.equal(hex(pubB), p.pub_b_hex);
    assert.equal(hex(pubC), p.pub_c_hex);
    assert.equal(hex(A.pair2Commit(pubB, fromHex(p.nonce_b_hex))), p.commit_hex);
    const sh = X.shared(sb, pubC);
    assert.equal(hex(sh), p.shared_hex);
    const k = A.pair2Derive(sh, pubB, pubC, fromHex(p.nonce_b_hex), fromHex(p.nonce_c_hex), label);
    assert.equal(k.sas, p.sas);
    assert.equal(A.formatSas(k.sas).replace(" ", ""), p.sas_text);
    assert.equal(hex(k.psk), p.psk_hex);
    assert.equal(hex(k.keyId), p.key_id_hex);
    assert.equal(hex(A.pair2OkProof(k.psk, pubB, pubC)), p.ok_proof_hex);
  }
});

suite("the page pairs by number comparison over a WebSocket", () => {
  let dir: string, proc: ChildProcess | undefined;
  after(() => { proc?.kill(); if (dir) rmSync(dir, { recursive: true, force: true }); });

  test("the number matches the console's, the key logs in over sealed frames, and a refusal is reported", async () => {
    dir = mkdtempSync(join(tmpdir(), "ndp-webpair-"));
    const a = await startAgent(["--root", dir, "--mode", "DEVELOPMENT", "--auth", "required", "--keys-file", join(dir, "keys.bin"), "--web-port", "0", "--pair-open", "--pair-approve", "auto"]);
    proc = a.proc;
    const open = async () => { const c = new NDP.NdpWsClient(`ws://127.0.0.1:${a.webPort}/ws`); await c.open(); return c; };
    const c = await open();
    const info = await c.hello();
    assert.equal(info.pairingOpen, true);
    let shown = "";
    const { psk } = await c.pairByNumber("web-test", (s: string) => (shown = s));
    assert.equal(shown.replace(" ", ""), /number=(\d{6})/.exec(a.stdout())![1]);
    c.close();
    const c2 = await open();
    await c2.hello();
    await c2.authenticate(psk);
    assert.equal(await c2.ping() > 0, true);
    assert.ok(Array.isArray(await c2.list("/")), "a sealed request works with the key from the number pairing");
    c2.close();
    // no second pairing: the window closed with the first
    const c3 = await open();
    await c3.hello();
    await assert.rejects(c3.pairByNumber("again", () => undefined), (e: any) => e.statusName === "UNAUTHORIZED");
    c3.close();
  });

  test("a denial reaches the page as .refused = denied", async () => {
    const dir2 = mkdtempSync(join(tmpdir(), "ndp-webpair2-"));
    const a = await startAgent(["--root", dir2, "--auth", "required", "--keys-file", join(dir2, "keys.bin"), "--web-port", "0", "--pair-open", "--pair-approve", "deny"]);
    try {
      const c = new NDP.NdpWsClient(`ws://127.0.0.1:${a.webPort}/ws`);
      await c.open();
      await c.hello();
      await assert.rejects(c.pairByNumber("nope", () => undefined), (e: any) => e.refused === "denied");
      c.close();
    } finally {
      a.proc.kill();
      rmSync(dir2, { recursive: true, force: true });
    }
  });
});

suite("an open page: the browser needs no pairing, the TCP port (CLI/MCP) still does", () => {
  test("the WebSocket is served as auth none, cannot see or forget pairings, and the TCP port refuses an unpaired client", async () => {
    const dir = mkdtempSync(join(tmpdir(), "ndp-webopen-"));
    const a = await startAgent(["--root", dir, "--mode", "DEVELOPMENT", "--auth", "required", "--keys-file", join(dir, "keys.bin"), "--web-port", "0", "--web-open"]);
    try {
      // the page's connection: no AUTH step, and the file commands work
      const w = new NDP.NdpWsClient(`ws://127.0.0.1:${a.webPort}/ws`);
      await w.open();
      const info = await w.hello();
      assert.equal(info.auth, "none");
      assert.equal(Array.isArray(await w.list("/")), true);
      // it cannot read or prune the list of paired computers
      await assert.rejects(w.pairList(), (e: any) => e.statusName === "UNSUPPORTED_COMMAND");
      await assert.rejects(w.forgetPairing(new Uint8Array(4)), (e: any) => e.statusName === "UNSUPPORTED_COMMAND");
      w.close();

      // the raw TCP port used by the CLI and the MCP server is unchanged: pairing is required there
      const c = await NdpClient.connect({ host: "127.0.0.1", port: a.port });
      const tcpInfo = await c.hello();
      assert.equal(tcpInfo.auth, "required");
      await assert.rejects(c.list("/"), (e: any) => e.statusName === "UNAUTHORIZED");
      c.close();
    } finally {
      a.proc.kill();
      rmSync(dir, { recursive: true, force: true });
    }
  });

  test("without --web-open the page still has to pair", async () => {
    const dir = mkdtempSync(join(tmpdir(), "ndp-webclosed-"));
    const a = await startAgent(["--root", dir, "--auth", "required", "--keys-file", join(dir, "keys.bin"), "--web-port", "0"]);
    try {
      const w = new NDP.NdpWsClient(`ws://127.0.0.1:${a.webPort}/ws`);
      await w.open();
      assert.equal((await w.hello()).auth, "required");
      await assert.rejects(w.list("/"), (e: any) => e.statusName === "UNAUTHORIZED");
      w.close();
    } finally {
      a.proc.kill();
      rmSync(dir, { recursive: true, force: true });
    }
  });
});
