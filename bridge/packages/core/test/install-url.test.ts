// INSTALL_URL end to end against the C host agent pretending to be a 3DS: the URL reaches the platform hook, bad URLs are
// refused before that, the page's (open) WebSocket can send it, and other platforms answer UNSUPPORTED_COMMAND.
import { strict as assert } from "node:assert";
import { execFile } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
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
});
