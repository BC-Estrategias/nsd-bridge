// `ndev update` against the C host agent pretending to be a DSi (and a 3DS): the file goes to /nsd-bridge/ under the right name.
import { strict as assert } from "node:assert";
import { execFile } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { SKIP, startAgent } from "./helpers.ts";

const run = promisify(execFile);
const CLI = fileURLToPath(new URL("../../cli/src/main.ts", import.meta.url));
const suite = SKIP ? describe.skip : describe;

suite("ndev update", () => {
  test("a DSi gets the build in /nsd-bridge/ (needs DEVELOPMENT), a 3DS is told to use its own menu", async () => {
    const dir = mkdtempSync(join(tmpdir(), "ndp-upd-"));
    mkdirSync(join(dir, "nsd-bridge"));
    const nds = join(dir, "nsd-bridge-dsi-v9.9.9.nds");
    const payload = Buffer.alloc(6000, 7);
    writeFileSync(nds, payload);
    const dsi = await startAgent(["--root", dir, "--platform", "dsi", "--write-root", "/nsd-bridge", "--mode", "READ_ONLY"]);
    const dev = await startAgent(["--root", dir, "--platform", "dsi", "--write-root", "/nsd-bridge", "--mode", "DEVELOPMENT"]);
    const three = await startAgent(["--root", dir, "--platform", "3ds"]);
    try {
      const cli = (port: number, ...a: string[]) => run(process.execPath, [CLI, "update", `127.0.0.1:${port}`, ...a], { encoding: "utf8" });
      await assert.rejects(cli(dsi.port, "--file", nds), (e: { stderr: string }) => /READ-ONLY/.test(e.stderr));
      assert.equal(existsSync(join(dir, "nsd-bridge", "nsd-bridge-dsi-v9.9.9.nds")), false);
      const ok = await cli(dev.port, "--file", nds);
      assert.match(ok.stdout, /sent to \/nsd-bridge\/nsd-bridge-dsi-v9\.9\.9\.nds: 6000 bytes/);
      assert.deepEqual(readFileSync(join(dir, "nsd-bridge", "nsd-bridge-dsi-v9.9.9.nds")), payload);
      // a second run never overwrites what is already there
      await assert.rejects(cli(dev.port, "--file", nds), (e: { stderr: string }) => /ALREADY_EXISTS|exists/i.test(e.stderr));
      // a wrong file name is refused before anything is sent
      writeFileSync(join(dir, "other.nds"), payload);
      await assert.rejects(cli(dev.port, "--file", join(dir, "other.nds")), (e: { stderr: string }) => /must be named/.test(e.stderr));
      const t = await cli(three.port);
      assert.match(t.stdout, /3ds console\. The 3DS updates itself/);
    } finally {
      for (const a of [dsi, dev, three]) a.proc.kill();
      rmSync(dir, { recursive: true, force: true });
    }
  });
});
