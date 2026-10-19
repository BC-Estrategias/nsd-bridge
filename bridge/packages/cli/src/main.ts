#!/usr/bin/env node
import { createWriteStream } from "node:fs";
import { once } from "node:events";
import { hostname } from "node:os";
import { createInterface } from "node:readline/promises";
import {
  DEFAULT_PORT, KeyStore, LABEL_MAX, NdpClient, NdpRemoteError, NdpTransportError, PathInvalidError, codeDecode, PairingRefusedError,
  discover, downloadAsset, fetchLatestRelease, isNewerVersion, pickDsiAsset,
} from "@ndev/core";

const USAGE = `ndev — Nintendo Dev Bridge (CLI)

Usage:
  ndev find [port]                                scan this computer's subnets for a console running the agent
                                                  (the console's IP changes: DHCP)
  ndev pair  <host[:port]> [--label NAME] [--code | --code-stdin]
                                                  pair this computer with the console: press Y on the console, run this,
                                                  and press A on the console when it shows the same number as here.
                                                  --code: the older way (type the code the console shows)
  ndev unpair <host[:port]>                       forget this computer's key for that console
                                                  (to make the console forget ALL computers: its SELECT button)
  ndev pairings                                   list the consoles this computer is paired with
  ndev info  <host[:port]>                        model, firmware, memory and SD card space of the console
  ndev access <host[:port]>                       the folders the console's owner opened (read / write);
                                                  they are chosen on the console itself (A button)
  ndev hello <host[:port]>                        connect and print the agent's HELLO
  ndev ping  <host[:port]> [-c N] [-i MS]         HELLO + N pings (default 4), MS ms between pings
  ndev ls    <host[:port]> <path>                 list a directory on the device's SD card
  ndev stat  <host[:port]> <path>                 type, size and mtime
  ndev cat   <host[:port]> <path> [--tail N] [--max N] [--offset N]
                                                  write the file (or a range) to stdout
  ndev get   <host[:port]> <remote> <local> [--no-verify] [--chunk BYTES]
                                                  download a file; verifies SHA-256 unless --no-verify
  ndev put   <host[:port]> <local> <remote> [--replace] [--backup] [--chunk BYTES]
  ndev put   <host[:port]> --text "content" <remote> [--replace] [--backup]
                                                  upload (temp file + verify + rename on the device);
                                                  refuses to overwrite unless --replace
  ndev update <host[:port]> [--force] [--file nsd-bridge-dsi-vX.Y.Z.nds]
                                                  DSi: download the newest DSi build from GitHub (SHA-256 checked) and send it
                                                  to /nsd-bridge/ (needs DEVELOPMENT mode: press X on the DSi). The 3DS updates
                                                  itself from its Updates menu.
  ndev install <host[:port]> <url>                3DS: offer installing a title from an http(s) URL; on the console open
                                                  Installer > Receive from the browser, and confirm there with A
  ndev mkdir <host[:port]> <path>                 create a directory (the parent must exist)
  ndev mv    <host[:port]> <from> <to>            move/rename a file or folder (never overwrites; also restores from the trash)
  ndev purge <host[:port]> <path>...              PERMANENTLY delete items inside a trash folder (<root>/.ndp-trash/...), or
                                                  empty a trash by giving the folder itself; refuses anything else
  ndev cp    <host[:port]> <from> <to>            copy ONE file on the card (done by the console; never overwrites)
  ndev rm    <host[:port]> <path>...              move files/folders to the device's trash
                                                  (<write root>/.ndp-trash/); nothing is destroyed

Paths are absolute on the SD card ("/3ds/nintendo-dev-agent/agent.log"). The default port is ${DEFAULT_PORT}.`;

function parseTarget(s: string): { host: string; port: number } {
  const i = s.lastIndexOf(":");
  if (i > 0 && !s.includes("]")) {
    const port = Number(s.slice(i + 1));
    if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error(`invalid port in "${s}"`);
    return { host: s.slice(0, i), port };
  }
  return { host: s, port: DEFAULT_PORT };
}

function human(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${(n / 1024 / 1024).toFixed(2)} MiB`;
}

/** Parses `--flag value` / `--switch` pairs; unknown flags are an error. */
function parseFlags(
  args: string[], valued: string[], switches: string[] = [], strings: string[] = [],
): { flags: Map<string, number>; on: Set<string>; rest: string[]; text: Map<string, string> } {
  const flags = new Map<string, number>();
  const text = new Map<string, string>();
  const on = new Set<string>();
  const rest: string[] = [];
  for (let i = 0; i < args.length; i++) {
    const a = args[i] as string;
    if (strings.includes(a)) {
      const v = args[++i];
      if (v === undefined) throw new Error(`${a} expects a value`);
      text.set(a, v);
    } else if (valued.includes(a)) {
      const n = Number(args[++i]);
      if (!Number.isInteger(n) || n < 0) throw new Error(`${a} expects a non-negative integer`);
      flags.set(a, n);
    } else if (switches.includes(a)) on.add(a);
    else if (a.startsWith("-") && a.length > 1) throw new Error(`unknown option ${a}`);
    else rest.push(a);
  }
  return { flags, on, rest, text };
}

async function withClient<T>(target: string, fn: (c: NdpClient, host: string, port: number) => Promise<T>): Promise<T> {
  const { host, port } = parseTarget(target);
  const { client } = await NdpClient.open({ host, port }, new KeyStore());
  try {
    return await fn(client, host, port);
  } finally {
    client.close();
  }
}

async function readCode(fromStdin: boolean): Promise<string> {
  if (!fromStdin && !process.stdin.isTTY)
    throw new Error("the pairing code must be typed by a person: run this in a terminal (or use --code-stdin)");
  const rl = createInterface({ input: process.stdin, output: process.stderr });
  try {
    return (await rl.question("Code shown on the console (XXXX-XXXX-XXXX-XXXX): ")).trim();
  } finally {
    rl.close();
  }
}

async function pairCommand(target: string, rest: string[]): Promise<number> {
  const { text, on } = parseFlags(rest, [], ["--code-stdin", "--code"], ["--label"]);
  const { host, port } = parseTarget(target);
  const label = text.get("--label") ?? hostname().split(".")[0]!.slice(0, LABEL_MAX);
  if (new TextEncoder().encode(label).length > LABEL_MAX) throw new Error(`--label must be at most ${LABEL_MAX} bytes`);
  const keys = new KeyStore();
  const client = await NdpClient.connect({ host, port });
  try {
    const info = await client.hello();
    if (info.auth !== "required") {
      console.log("This agent does not use authentication (auth none: a development build). Nothing to pair.");
      return 0;
    }
    const id = Buffer.from(info.deviceId!).toString("hex");
    if (keys.find(id)) console.error("Note: this computer is already paired with this console; pairing again replaces the key.");
    if (!info.pairingOpen) {
      console.error("The console's pairing window is closed. Press Y on the console (it opens for 2 minutes), then run this again.");
      return 1;
    }
    if (!on.has("--code") && !on.has("--code-stdin")) {
      // by number comparison: nothing to type. The console shows the same number; the person presses A there.
      try {
        const { psk, keyId } = await client.pairByNumber(label, (sas) => {
          console.log(`Number: ${sas}`);
          console.log("Press A on the console if it shows the same number (B if not).");
        });
        keys.save({ deviceId: id, psk: Buffer.from(psk).toString("hex"), keyId: Buffer.from(keyId).toString("hex"), label, lastHost: host, pairedAt: new Date().toISOString() });
        console.log(`Paired with console ${id.slice(0, 8)}… as "${label}". Key saved in ${keys.path} (private to your user).`);
        return 0;
      } catch (e) {
        if (e instanceof PairingRefusedError) { console.error(e.message); return 1; }
        if (e instanceof NdpRemoteError && e.statusName === "UNAUTHORIZED" && /authentication required/.test(e.detail ?? "")) {
          console.error("This console is older than v1.3.0 and only pairs with the code: run again with --code.");
          return 1;
        }
        throw e;
      }
    }
    const code = codeDecode(await readCode(on.has("--code-stdin")));
    if (!code) throw new Error("that is not a valid code (16 characters: 0-9 and A-Z without I, L, O, U)");
    const { psk, keyId } = await client.pair(code, label);
    keys.save({
      deviceId: id, psk: Buffer.from(psk).toString("hex"), keyId: Buffer.from(keyId).toString("hex"), label,
      lastHost: host, pairedAt: new Date().toISOString(),
    });
    console.log(`Paired with console ${id.slice(0, 8)}… as "${label}". Key saved in ${keys.path} (private to your user).`);
    return 0;
  } finally {
    client.close();
  }
}

async function main(argv: string[]): Promise<number> {
  const [cmd, target, ...rest] = argv;
  if (cmd === "find") {
    const found = await discover(target ? { port: Number(target) } : {});
    if (found.length === 0) {
      console.error("no agent found. Open the NSD Bridge app on the console (same Wi-Fi as this computer).");
      return 1;
    }
    for (const f of found) console.log(`${f.host}:${f.port}  ${f.agent.platform} agent v${f.agent.agentVersion}  mode ${f.agent.mode}`);
    return 0;
  }
  if (cmd === "pairings") {
    const list = new KeyStore().list();
    if (list.length === 0) console.log("not paired with any console");
    for (const c of list) console.log(`${c.deviceId.slice(0, 8)}…  "${c.label}"  last seen ${c.lastHost ?? "?"}  paired ${c.pairedAt}`);
    return 0;
  }
  if (!cmd || cmd === "-h" || cmd === "--help") {
    console.log(USAGE);
    return cmd ? 0 : 2;
  }
  if ((cmd === "pair" || cmd === "unpair") && !target) {
    console.error(USAGE);
    return 2;
  }
  if (cmd === "pair") return pairCommand(target as string, rest);
  if (cmd === "unpair") {
    const { host, port } = parseTarget(target as string);
    const client = await NdpClient.connect({ host, port });
    try {
      const info = await client.hello();
      const keys = new KeyStore();
      const id = info.deviceId ? Buffer.from(info.deviceId).toString("hex") : "";
      console.log(id && keys.remove(id) ? "This computer's key for that console was deleted." : "This computer had no key for that console.");
      return 0;
    } finally {
      client.close();
    }
  }
  if (!["hello", "ping", "info", "access", "ls", "stat", "cat", "get", "put", "mkdir", "mv", "cp", "rm", "purge", "update", "install"].includes(cmd) || !target) {
    console.error(USAGE);
    return 2;
  }

  if (cmd === "info") {
    return withClient(target, async (client, host) => {
      const h = client.info!;
      const d = await client.deviceInfo();
      const gib = (n: number) => `${(n / 1024 ** 3).toFixed(2)} GiB`;
      const mib = (n: number) => `${(n / 1024 ** 2).toFixed(1)} MiB`;
      console.log(`${host}  agent v${h.agentVersion}  mode ${h.mode}`);
      console.log(`model     ${d.model ?? "unknown"}`);
      console.log(`firmware  ${d.firmware ?? "unknown"}`);
      if (d.ramTotal !== undefined) console.log(`RAM       ${mib(d.ramTotal)}`);
      if (d.appMemory) console.log(`app mem   ${mib(d.appMemory.free)} free of ${mib(d.appMemory.total)} (region of the foreground app)`);
      if (d.systemMemory) console.log(`sys mem   ${mib(d.systemMemory.free)} free of ${mib(d.systemMemory.total)}`);
      if (d.sd) console.log(`SD card   ${gib(d.sd.free)} free of ${gib(d.sd.total)} (${((100 * d.sd.free) / d.sd.total).toFixed(0)}% free)`);
      return 0;
    });
  }

  if (cmd === "access") {
    return withClient(target, async (client) => {
      const a = await client.accessInfo();
      console.log(`mode ${a.mode}`);
      console.log("readable folders (recursively):");
      for (const r of a.readRoots) console.log(`  ${r}${a.writeRoots.includes(r) ? "   (also writable)" : ""}`);
      console.log(a.mode === "READ_ONLY" ? "writable folders (writes are OFF: press X on the console):" : "writable folders:");
      for (const r of a.writeRoots) console.log(`  ${r}`);
      return 0;
    });
  }

  if (cmd === "install") {
    // 3DS: ask the console to offer installing a title from a URL. The owner opens Installer > Receive from the browser on
    // the console and confirms there; nothing is downloaded before that.
    const [url] = parseFlags(rest, []).rest;
    if (!url) throw new Error("install needs <url>  (e.g. ndev install 192.168.1.20 http://192.168.1.10:8000/game.cia)");
    return withClient(target, async (client, host) => {
      if (client.info!.platform !== "3ds") throw new Error(`${host} is a ${client.info!.platform} console: only the 3DS has an installer`);
      await client.installUrl(url);
      console.log("Sent. Confirm on the 3DS (A) to download and install.");
      return 0;
    });
  }

  if (cmd === "update") {
    // The DSi cannot update itself (its Wi-Fi has no HTTPS, which GitHub requires), so this computer downloads the newest
    // DSi build, checks it, and sends it over the bridge. The 3DS updates itself from its own Updates menu.
    const { on, text } = parseFlags(rest, [], ["--force"], ["--file"]);
    return withClient(target, async (client, host) => {
      const info = client.info!;
      if (info.platform !== "dsi") {
        console.log(`${host} is a ${info.platform} console. The 3DS updates itself: open "Updates" in its menu (a few seconds after the app opens it offers the new version).`);
        return 0;
      }
      let bytes: Uint8Array, version: string;
      const file = text.get("--file");
      if (file) {
        const { readFileSync } = await import("node:fs");
        bytes = new Uint8Array(readFileSync(file));
        const m = /nsd-bridge-dsi-(v\d+\.\d+\.\d+)\.nds$/.exec(file);
        if (!m) throw new Error("--file must be named nsd-bridge-dsi-vX.Y.Z.nds");
        version = m[1] as string;
      } else {
        const asset = pickDsiAsset(await fetchLatestRelease());
        if (!asset) throw new Error("no usable DSi build in the latest release");
        if (!on.has("--force") && !isNewerVersion(asset.version, info.agentVersion)) {
          console.log(`${host} runs v${info.agentVersion}; the latest release is ${asset.version}. Already up to date (use --force to send it anyway).`);
          return 0;
        }
        console.log(`${host} runs v${info.agentVersion}; downloading ${asset.version} (${(asset.size / 1024).toFixed(0)} KiB)...`);
        bytes = await downloadAsset(asset);
        version = asset.version;
      }
      if (info.mode === "READ_ONLY") throw new Error("the DSi is READ-ONLY: press X on its screen first, then run this again");
      const remote = `/nsd-bridge/nsd-bridge-dsi-${version}.nds`;
      const r = await client.writeBytes(remote, bytes, { overwrite: false, backup: false });
      console.log(`sent to ${remote}: ${r.written} bytes in ${(r.ms / 1000).toFixed(1)} s (checked by the console)`);
      console.log(`On the DSi: close the app, open ${remote} from TWiLight Menu++, and the new version runs. The old file can stay or be deleted from the page.`);
      return 0;
    });
  }

  if (cmd === "hello" || cmd === "ping") {
    const { flags } = parseFlags(rest, ["-c", "-i"]);
    const count = flags.get("-c") ?? 4;
    const intervalMs = flags.get("-i") ?? 0;
    if (count < 1 || count > 1000) throw new Error("-c expects an integer between 1 and 1000");
    return withClient(target, async (client, host, port) => {
      const info = client.info!;
      console.log(`Connected to ${host}:${port}`);
      console.log(
        `Agent: ${info.platform}  v${info.agentVersion}  protocol ${info.protocol}  mode ${info.mode}  ` +
          `auth ${info.auth}  max_frame ${info.maxFrame}`,
      );
      if (cmd === "ping") {
        const rtts: number[] = [];
        for (let seq = 1; seq <= count; seq++) {
          const rtt = await client.ping();
          rtts.push(rtt);
          console.log(`PONG seq=${seq} time=${rtt.toFixed(1)} ms`);
          if (intervalMs && seq < count) await new Promise((r) => setTimeout(r, intervalMs));
        }
        const avg = rtts.reduce((a, b) => a + b, 0) / rtts.length;
        console.log(`min/avg/max = ${Math.min(...rtts).toFixed(1)}/${avg.toFixed(1)}/${Math.max(...rtts).toFixed(1)} ms`);
      }
      return 0;
    });
  }

  if (cmd === "ls" || cmd === "stat") {
    const { rest: pos } = parseFlags(rest, []);
    const path = pos[0];
    if (!path) throw new Error(`${cmd} needs a <path>`);
    return withClient(target, async (client) => {
      if (cmd === "stat") {
        const st = await client.stat(path);
        console.log(`${path}\n  type  ${st.type}\n  size  ${st.size} bytes (${human(st.size)})\n  mtime ${st.mtime === null ? "unknown" : new Date(st.mtime * 1000).toISOString()}`);
        return 0;
      }
      const entries = await client.list(path);
      entries.sort((a, b) => (a.type === b.type ? a.name.localeCompare(b.name) : a.type === "dir" ? -1 : 1));
      for (const e of entries) console.log(`${e.type === "dir" ? "d" : "-"} ${String(e.size).padStart(10)}  ${e.name}${e.type === "dir" ? "/" : ""}`);
      console.error(`${entries.length} entries`);
      return 0;
    });
  }

  if (cmd === "cat") {
    const { flags, rest: pos } = parseFlags(rest, ["--tail", "--max", "--offset"]);
    const path = pos[0];
    if (!path) throw new Error("cat needs a <path>");
    return withClient(target, async (client) => {
      let offset = flags.get("--offset") ?? 0;
      const tail = flags.get("--tail");
      if (tail !== undefined) {
        const size = (await client.stat(path)).size;
        offset = Math.max(0, size - tail);
      }
      const r = await client.readBytes(path, { offset, maxBytes: flags.get("--max") ?? 8 * 1024 * 1024 });
      process.stdout.write(r.data);
      if (r.truncated) console.error(`\n[truncated: read ${r.bytes} of ${r.totalSize} bytes; use --max/--offset/--tail or 'get']`);
      return 0;
    });
  }

  if (cmd === "mkdir") {
    const path = parseFlags(rest, []).rest[0];
    if (!path) throw new Error("mkdir needs a <path>");
    return withClient(target, async (client) => {
      await client.mkdir(path);
      console.log(`created ${path}`);
      return 0;
    });
  }

  if (cmd === "mv") {
    const [from, to] = parseFlags(rest, []).rest;
    if (!from || !to) throw new Error("mv needs <from> and <to>");
    return withClient(target, async (client) => {
      console.log(`${from} -> ${await client.rename(from, to)}`);
      return 0;
    });
  }

  if (cmd === "cp") {
    const [from, to] = parseFlags(rest, []).rest;
    if (!from || !to) throw new Error("cp needs <from> and <to>");
    return withClient(target, async (client) => {
      const r = await client.copy(from, to);
      console.log(`${from} -> ${to} (${r.bytes} bytes)`);
      return 0;
    });
  }

  if (cmd === "purge") {
    const paths = parseFlags(rest, []).rest;
    if (paths.length === 0) throw new Error("purge needs at least one <path> inside a .ndp-trash folder");
    return withClient(target, async (client) => {
      let failed = 0;
      for (const p of paths) {
        try {
          const r = await client.purge(p);
          console.log(`${p}: ${r.removed} item(s) deleted for good`);
        } catch (e) {
          failed++;
          console.error(`${p}: ${e instanceof NdpRemoteError ? `${e.statusName}${e.detail ? ` — ${e.detail}` : ""}` : (e as Error).message}`);
        }
      }
      return failed ? 1 : 0;
    });
  }

  if (cmd === "rm") {
    const paths = parseFlags(rest, []).rest;
    if (paths.length === 0) throw new Error("rm needs at least one <path>");
    return withClient(target, async (client) => {
      let failed = 0;
      for (const p of paths) {
        try {
          const r = await client.delete(p);
          console.log(`${p} -> ${r.trashPath}`);
        } catch (e) {
          failed++;
          console.error(`${p}: ${e instanceof NdpRemoteError ? `${e.statusName}${e.detail ? ` — ${e.detail}` : ""}` : (e as Error).message}`);
        }
      }
      return failed ? 1 : 0;
    });
  }

  if (cmd === "put") {
    const { flags, on, rest: pos, text } = parseFlags(rest, ["--chunk"], ["--replace", "--backup"], ["--text"]);
    const inline = text.get("--text");
    const [local, remote] = inline !== undefined ? [undefined, pos[0]] : [pos[0], pos[1]];
    if (!remote || (inline === undefined && !local)) throw new Error("put needs <local> <remote> (or --text \"...\" <remote>)");
    const chunk = flags.get("--chunk");
    const opts = { overwrite: on.has("--replace"), backup: on.has("--backup"), ...(chunk ? { chunk } : {}) };
    return withClient(target, async (client) => {
      const r = inline !== undefined
        ? await client.writeBytes(remote, new TextEncoder().encode(inline), opts)
        : await client.writeFile(remote, local as string, opts);
      const kib = r.ms > 0 ? (r.written / 1024 / (r.ms / 1000)).toFixed(0) : "-";
      console.log(`${inline !== undefined ? "(text)" : local} -> ${remote}: ${r.written} bytes in ${r.ms.toFixed(0)} ms (${kib} KiB/s)${r.replaced ? " [replaced existing file]" : ""}`);
      console.log(`sha256 ${Buffer.from(r.sha256).toString("hex")} (verified by the device)`);
      return 0;
    });
  }

  // get
  const { flags, on, rest: pos } = parseFlags(rest, ["--chunk"], ["--no-verify"]);
  const [remote, local] = pos;
  if (!remote || !local) throw new Error("get needs <remote> <local>");
  return withClient(target, async (client) => {
    const out = createWriteStream(local);
    let failed: Error | undefined;
    out.on("error", (e) => (failed = e));
    try {
      const chunk = flags.get("--chunk");
      const r = await client.read(remote, { verify: !on.has("--no-verify"), ...(chunk ? { chunk } : {}) }, async (chunk) => {
        if (failed) throw failed;
        if (!out.write(chunk)) await once(out, "drain");
      });
      out.end();
      await once(out, "close");
      const kib = r.ms > 0 ? (r.bytes / 1024 / (r.ms / 1000)).toFixed(0) : "-";
      console.log(`${remote} -> ${local}: ${r.bytes} bytes in ${r.ms.toFixed(0)} ms (${kib} KiB/s)`);
      console.log(r.verified && r.sha256 ? `sha256 ${Buffer.from(r.sha256).toString("hex")} (verified against the agent)` : "not verified");
      return 0;
    } catch (e) {
      out.destroy();
      throw e;
    }
  });
}

main(process.argv.slice(2)).then(
  (code) => process.exit(code),
  (e: unknown) => {
    if (e instanceof NdpRemoteError) console.error(`error: ${e.statusName}${e.detail ? ` — ${e.detail}` : ""}`);
    else if (e instanceof NdpTransportError || e instanceof PathInvalidError || e instanceof Error) console.error(`error: ${e.message}`);
    else console.error(`error: ${String(e)}`);
    process.exit(1);
  },
);
