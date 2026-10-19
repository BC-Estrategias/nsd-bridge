// INSTALL_URL end to end against the C host agent pretending to be a 3DS: the URL reaches the platform hook, bad URLs are
// refused before that, the page's (open) WebSocket can send it, and other platforms answer UNSUPPORTED_COMMAND.
import { strict as assert } from "node:assert";
import { execFile, spawn } from "node:child_process";
import { createHash, randomBytes } from "node:crypto";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, test } from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";
import { promisify } from "node:util";
import { NdpClient } from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const WEB = fileURLToPath(new URL("../../../../web/js/", import.meta.url));
for (const f of ["sha256.js", "codec.js", "x25519.js", "auth.js", "client.js"]) await import(pathToFileURL(WEB + f).href);
const NDP = (globalThis as any).NDP;
const run = promisify(execFile);
const CLI = fileURLToPath(new URL("../../cli/src/main.ts", import.meta.url));
const suite = SKIP ? describe.skip : describe;
const code = async (p: Promise<unknown>) => { try { await p; return "OK"; } catch (e: any) { return e.statusName as string; } };

suite("INSTALL_URL", () => {
  test("a 3DS takes valid http(s) URLs, refuses bad ones, says BUSY when its screen is closed", async () => {
    const dir = mkdtempSync(join(tmpdir(), "ndp-inst-"));
    const open = await startAgent(["--root", dir, "--platform", "3ds", "--accept-installs"]);
    const closed = await startAgent(["--root", dir, "--platform", "3ds"]);
    const dsi = await startAgent(["--root", dir, "--platform", "dsi"]);
    try {
      const c = await NdpClient.connect({ host: "127.0.0.1", port: open.port });
      await c.hello();
      await c.installUrl("http://192.168.1.10:8000/game.cia");
      await c.installUrl("https://example.com/a/b.cia?x=1");
      assert.match(open.stdout(), /INSTALL_URL http:\/\/192\.168\.1\.10:8000\/game\.cia/);
      assert.match(open.stdout(), /INSTALL_URL https:\/\/example\.com\/a\/b\.cia\?x=1/);
      for (const bad of ["ftp://x/y.cia", "javascript:alert(1)", "http://a b/c.cia", "http://x/\ny", "game.cia", "", "https://x/é.cia"]) {
        const before = open.stdout();
        assert.equal(await code(c.installUrl(bad)), bad === "" ? "BAD_REQUEST" : "BAD_REQUEST", JSON.stringify(bad));
        assert.equal(open.stdout(), before, "a refused URL never reaches the platform");
      }
      assert.equal(await code(c.installUrl("http://x/" + "a".repeat(1100))), "BAD_REQUEST", "too long");
      // a .cia already on the console's card: accepted with its hash; anything else is refused before the platform sees it
      const sha = new Uint8Array(32).fill(7);
      await c.installFile("/cias/My Game.CIA", sha);
      assert.match(open.stdout(), /INSTALL_PATH \/cias\/My Game\.CIA sha256=0707/);
      for (const bad of ["/cias/readme.txt", "/cias/../../etc/x.cia", "relative/x.cia", "/x.cia.bak", "/.cia"]) {
        const before = open.stdout();
        const r = await code(c.installFile(bad));
        assert.notEqual(r, "OK", `${bad} must be refused (here or on the console)`);
        assert.equal(open.stdout(), before, "a refused path never reaches the platform");
      }
      c.close();

      const c2 = await NdpClient.connect({ host: "127.0.0.1", port: closed.port });
      await c2.hello();
      await assert.rejects(c2.installUrl("http://x/y.cia"), (e: any) => e.statusName === "BUSY" && /install screen/.test(String(e.detail)));
      c2.close();

      const c3 = await NdpClient.connect({ host: "127.0.0.1", port: dsi.port });
      await c3.hello();
      assert.equal(await code(c3.installUrl("http://x/y.cia")), "UNSUPPORTED_COMMAND");
      c3.close();

      // the CLI sends it too
      const ok = await run(process.execPath, [CLI, "install", `127.0.0.1:${open.port}`, "http://10.0.0.2/z.cia"], { encoding: "utf8" });
      assert.match(ok.stdout, /Sent\. Confirm on the 3DS/);
      assert.match(open.stdout(), /INSTALL_URL http:\/\/10\.0\.0\.2\/z\.cia/);
      await assert.rejects(run(process.execPath, [CLI, "install", `127.0.0.1:${dsi.port}`, "http://x/y.cia"], { encoding: "utf8" }), (e: { stderr: string }) => /only the 3DS has an installer/.test(e.stderr));
    } finally {
      for (const a of [open, closed, dsi]) a.proc.kill();
      rmSync(dir, { recursive: true, force: true });
    }
  });

  test("the page's open WebSocket can send a URL without pairing (the screen and the confirmation on the console are the gate)", async () => {
    const dir = mkdtempSync(join(tmpdir(), "ndp-inst2-"));
    const a = await startAgent(["--root", dir, "--platform", "3ds", "--accept-installs", "--auth", "required", "--keys-file", join(dir, "keys.bin"), "--web-port", "0", "--web-open"]);
    try {
      const w = new NDP.NdpWsClient(`ws://127.0.0.1:${a.webPort}/ws`);
      await w.open();
      assert.equal((await w.hello()).auth, "none");
      await w.installUrl("https://github.com/x/y/releases/download/v1/app.cia");
      assert.match(a.stdout(), /INSTALL_URL https:\/\/github\.com\/x\/y\/releases\/download\/v1\/app\.cia/);
      await w.installPath("/3ds/nintendo-dev-agent/game.cia", new Uint8Array(32).fill(1));
      assert.match(a.stdout(), /INSTALL_PATH \/3ds\/nintendo-dev-agent\/game\.cia sha256=0101/);
      // the console's own validation (the page client does not pre-check)
      const before = a.stdout();
      await assert.rejects(w.installPath("/cias/readme.txt"), (e: any) => e.statusName === "BAD_REQUEST");
      await assert.rejects(w.installPath("cias/x.cia"), (e: any) => e.statusName === "PATH_INVALID");
      await assert.rejects(w.installPath("/cias/../../x.cia"), (e: any) => e.statusName === "PATH_INVALID");
      assert.equal(a.stdout(), before);
      w.close();
      // over the raw TCP port a client that is not paired still gets nothing
      const c = await NdpClient.connect({ host: "127.0.0.1", port: a.port });
      await c.hello();
      assert.equal(await code(c.installUrl("http://x/y.cia")), "UNAUTHORIZED");
      c.close();
    } finally {
      a.proc.kill();
      rmSync(dir, { recursive: true, force: true });
    }
  });

  test("ndev install <file>: serves the file (with Range, like the console's probe), sends URL + SHA-256, exits when downloaded", async () => {
    const dir = mkdtempSync(join(tmpdir(), "ndp-inst3-"));
    const a = await startAgent(["--root", dir, "--platform", "3ds", "--accept-installs"]);
    const payload = randomBytes(200_000);
    const file = join(dir, "My Game.cia");
    writeFileSync(file, payload);
    const cli = spawn(process.execPath, [CLI, "install", `127.0.0.1:${a.port}`, file], { stdio: ["ignore", "pipe", "pipe"] });
    let out = "", err = "";
    cli.stdout.on("data", (d) => (out += d));
    cli.stderr.on("data", (d) => (err += d));
    const exited = new Promise<number>((r) => cli.on("close", (c) => r(c ?? -1)));
    try {
      let m: RegExpExecArray | null = null;
      for (let i = 0; i < 100 && !m; i++) { await new Promise((r) => setTimeout(r, 50)); m = /INSTALL_URL (http:\/\/\S+) sha256=([0-9a-f]{64})/.exec(a.stdout()); }
      assert.ok(m, "the agent got the URL and the hash: " + a.stdout() + err);
      const [, url, sha] = m as RegExpExecArray;
      assert.equal(sha, createHash("sha256").update(payload).digest("hex"));
      assert.match(url as string, /My%20Game\.cia$/);
      // the console first probes the start of the file with a Range request...
      const probe = await fetch(url as string, { headers: { Range: "bytes=0-16383" } });
      assert.equal(probe.status, 206);
      assert.equal(probe.headers.get("content-range"), "bytes 0-16383/200000");
      assert.deepEqual(Buffer.from(await probe.arrayBuffer()), payload.subarray(0, 16384));
      assert.equal(cli.exitCode, null, "one partial request is not the end");
      // ...then downloads everything
      const full = await fetch(url as string);
      assert.equal(full.status, 200);
      assert.deepEqual(Buffer.from(await full.arrayBuffer()), payload);
      assert.equal(await exited, 0, err);
      assert.match(out, /downloaded the whole file/);
    } finally {
      cli.kill();
      a.proc.kill();
      rmSync(dir, { recursive: true, force: true });
    }
  });

  test("ndev install: a bad --sha256 is refused, a URL is sent as given with the hash", async () => {
    const dir = mkdtempSync(join(tmpdir(), "ndp-inst4-"));
    const a = await startAgent(["--root", dir, "--platform", "3ds", "--accept-installs"]);
    try {
      await assert.rejects(run(process.execPath, [CLI, "install", `127.0.0.1:${a.port}`, "http://x/y.cia", "--sha256", "abc"], { encoding: "utf8" }), (e: { stderr: string }) => /64 hex digits/.test(e.stderr));
      const h = "ab".repeat(32);
      await run(process.execPath, [CLI, "install", `127.0.0.1:${a.port}`, "https://example.com/y.cia", "--sha256", h], { encoding: "utf8" });
      assert.match(a.stdout(), new RegExp(`INSTALL_URL https://example\\.com/y\\.cia sha256=${h}`));
    } finally {
      a.proc.kill();
      rmSync(dir, { recursive: true, force: true });
    }
  });
});
