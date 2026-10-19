// The page's pure logic (web/js/util.js) and its dictionaries (web/js/i18n.js), run in Node. The DOM parts are checked
// in a real browser against the host agent (see scripts/check.sh notes); here we guard what can silently rot.
import { strict as assert } from "node:assert";
import { readFileSync, readdirSync } from "node:fs";
import { test } from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

const WEB = fileURLToPath(new URL("../../../../web/js/", import.meta.url));
for (const f of ["util.js", "update.js", "i18n.js"]) await import(pathToFileURL(WEB + f).href);
const NDP = (globalThis as any).NDP;
const U = NDP.util;

test("util: paths follow the console's rules (component-wise, ASCII case folded)", () => {
  assert.ok(U.inside("/3ds/Foo", "/3DS"));
  assert.ok(U.inside("/3ds", "/3ds"));
  assert.ok(!U.inside("/3dsx", "/3ds"));
  assert.ok(!U.inside("/", "/3ds"));
  assert.ok(U.inside("/anything/at/all", "/"));
  assert.equal(U.join("/", "a"), "/a");
  assert.equal(U.join("/a/b", "c"), "/a/b/c");
  assert.equal(U.parent("/a/b"), "/a");
  assert.equal(U.parent("/a"), "/");
  assert.equal(U.parent("/"), "/");
  assert.equal(U.basename("/a/b.txt"), "b.txt");
  assert.equal(U.ext("Photo.PNG"), "png");
  assert.equal(U.ext(".gitignore"), "");
  assert.equal(U.ext("noext"), "");
  assert.deepEqual(U.crumbs("/a/b").map((c: any) => c.path), ["/", "/a", "/a/b"]);
  assert.deepEqual(U.crumbs("/").map((c: any) => c.path), ["/"]);
  assert.equal(U.trashRootOf("/3ds/x/.ndp-trash/a.1"), "/3ds/x/.ndp-trash");
  assert.equal(U.trashRootOf("/3ds/x/.ndp-trash"), "/3ds/x/.ndp-trash");
  assert.equal(U.trashRootOf("/3ds/x/.ndp-trashy/a"), null);
  assert.equal(U.trashRootOf("/3ds/x"), null);
});

test("util: sizes, rates and times are readable", () => {
  const cases: Array<[number, string]> = [[0, "0 B"], [1023, "1023 B"], [1024, "1.00 KiB"], [1536, "1.50 KiB"], [10 * 1024, "10.0 KiB"], [100 * 1024, "100 KiB"], [1 << 20, "1.00 MiB"], [5 * 2 ** 30, "5.00 GiB"]];
  for (const [n, s] of cases) assert.equal(U.formatSize(n), s);
  assert.equal(U.formatSize(undefined), "");
  assert.equal(U.formatRate(0), "");
  assert.equal(U.formatRate(1 << 20), "1.00 MiB/s");
  assert.equal(U.formatEta(5.2), "6 s");
  assert.equal(U.formatEta(125), "2 min 5 s");
  assert.equal(U.formatEta(7300), "2 h 1 min");
  assert.equal(U.formatEta(Infinity), "");
});

test("util: file kinds and binary detection", () => {
  assert.ok(U.isImage("a.PNG") && U.isImage("b.jpeg") && !U.isImage("c.txt"));
  assert.ok(U.isTextName("notes.md") && U.isTextName("Makefile") && U.isTextName("README") && !U.isTextName("game.3dsx"));
  assert.equal(U.imageMime("a.jpg"), "image/jpeg");
  assert.ok(U.looksBinary(new Uint8Array([65, 0, 66])));
  assert.ok(U.looksBinary(new Uint8Array(100).fill(3)));
  assert.ok(!U.looksBinary(new TextEncoder().encode("hello\nworld\t\r\n")));
  assert.ok(!U.looksBinary(new Uint8Array(0)));
});

test("util: unique names and name validation", () => {
  const taken = new Set(["a.txt", "a (1).txt", "noext", ".hidden"]);
  assert.equal(U.uniqueName("b.txt", taken), "b.txt");
  assert.equal(U.uniqueName("A.TXT", taken), "A (2).TXT");
  assert.equal(U.uniqueName("noext", taken), "noext (1)");
  assert.equal(U.uniqueName(".hidden", taken), ".hidden (1)");
  for (const [n, want] of [["", "name.empty"], [".", "name.empty"], ["..", "name.empty"], ["a/b", "name.chars"], ["a:b", "name.chars"], ["a\u0001", "name.chars"], ["bad.", "name.trailing"], ["bad ", "name.trailing"], ["x~1.txt", "name.tilde"], ["é".repeat(128), "name.long"], ["ok name.txt", null], ["é".repeat(127), null]] as Array<[string, string | null]>) {
    assert.equal(U.nameProblem(n), want, JSON.stringify(n));
  }
});

test("util: folders first, natural numeric order, size sort", () => {
  const e = (name: string, type = "file", size = 0) => ({ name, type, size });
  const list = [e("file10"), e("file2"), e("Zed", "dir"), e("alpha", "dir"), e("file1")];
  assert.deepEqual(U.sortEntries(list, "name", "asc").map((x: any) => x.name), ["alpha", "Zed", "file1", "file2", "file10"]);
  assert.deepEqual(U.sortEntries(list, "name", "desc").map((x: any) => x.name), ["Zed", "alpha", "file10", "file2", "file1"]);
  const bySize = [e("a", "file", 5), e("b", "file", 50), e("c", "dir", 0), e("d", "file", 1)];
  assert.deepEqual(U.sortEntries(bySize, "size", "desc").map((x: any) => x.name), ["c", "b", "a", "d"]);
});

test("util: what the page promises about a path matches the console's policy", () => {
  for (const p of ["/luma", "/luma/config.ini", "/Nintendo 3DS/x", "/boot.firm", "/gm9/x", "/private", "/3ds/nintendo-dev-agent/config/pairing.bin"]) assert.ok(U.writeProtected(p), p);
  for (const p of ["/luma/plugins", "/luma/plugins/a.3gx", "/luma/titles/0004/code.bin", "/3ds/app", "/", "/lumafoo"]) assert.ok(!U.writeProtected(p), p);
  const access = { readRoots: ["/3ds", "/DCIM"], writeRoots: ["/3ds/nintendo-dev-agent", "/luma"] };
  assert.equal(U.accessOf("/3ds/nintendo-dev-agent/x", access, "DEVELOPMENT"), "write");
  assert.equal(U.accessOf("/3ds/nintendo-dev-agent/x", access, "READ_ONLY"), "read");
  assert.equal(U.accessOf("/luma/config.ini", access, "DEVELOPMENT"), "none"); // protected, and not readable by the lists
  assert.equal(U.accessOf("/3ds/other", access, "DEVELOPMENT"), "read");
  assert.equal(U.accessOf("/dcim/a.jpg", access, "DEVELOPMENT"), "read");
  assert.equal(U.accessOf("/etc", access, "DEVELOPMENT"), "none");
  assert.equal(U.accessOf("/anything", null, "DEVELOPMENT"), "read");
  // no platform given (or an unrecognized one) falls back to the 3DS's zones, same as every assertion above
  assert.ok(U.writeProtected("/luma", undefined));
  assert.ok(U.writeProtected("/luma", "made-up-platform"));
});

test("util: the DSi agent has its own protected zones, not the 3DS's (each platform overrides the shared defaults)", () => {
  for (const p of ["/_nds", "/_nds/nds-bootstrap.ini", "/nsd-bridge/config", "/nsd-bridge/config/pairing.bin"]) assert.ok(U.writeProtected(p, "dsi"), p);
  // the 3DS's own zones do not apply on a DSi card, which has neither of these folders
  for (const p of ["/luma", "/Nintendo 3DS", "/boot.firm", "/gm9", "/private", "/roms", "/nsd-bridge/hello.txt", "/"]) assert.ok(!U.writeProtected(p, "dsi"), p);
  const access = { readRoots: ["/nsd-bridge"], writeRoots: ["/nsd-bridge"] };
  assert.equal(U.accessOf("/nsd-bridge/x", access, "DEVELOPMENT", "dsi"), "write");
  assert.equal(U.accessOf("/nsd-bridge/config/pairing.bin", access, "DEVELOPMENT", "dsi"), "read"); // protected from writes, but still inside the read root
  assert.deepEqual(U.protectedZonesFor("dsi").neverWrite, ["/_nds", "/nsd-bridge/config"]);
});

test("util: blobCollector groups downloaded bytes into blobs without losing or reordering any", async () => {
  const col = U.blobCollector(1000);
  const parts: Uint8Array[] = [];
  for (let i = 0; i < 37; i++) { const b = new Uint8Array(137).map((_, j) => (i * 31 + j) & 255); parts.push(b); col.add(b); b.fill(0); } // the caller reuses its buffer
  assert.equal(col.size, 37 * 137);
  const blob = col.blob("application/pdf");
  assert.equal(blob.size, 37 * 137);
  assert.equal(blob.type, "application/pdf");
  const got = new Uint8Array(await blob.arrayBuffer());
  for (let i = 0; i < 37; i++) for (let j = 0; j < 137; j++) assert.equal(got[i * 137 + j], (i * 31 + j) & 255);
  assert.equal(U.blobCollector().blob().size, 0);
});

// ------------------------------------------------------------------------------------------------ i18n

const placeholders = (s: string) => [...s.matchAll(/\{(\d+)\}/g)].map((m) => m[1]).sort();

test("i18n: both dictionaries have exactly the same keys and placeholders", () => {
  const { "pt-BR": pt, en } = NDP.i18n.DICT as { "pt-BR": any; en: any };
  assert.deepEqual(Object.keys(pt).sort(), Object.keys(en).sort());
  for (const k of Object.keys(pt)) {
    assert.equal(typeof pt[k], "string", k);
    assert.ok(pt[k].length > 0 && en[k].length > 0, `empty text for ${k}`);
    assert.deepEqual(placeholders(pt[k]), placeholders(en[k]), `placeholders of ${k}`);
  }
});

test("i18n: every key the page uses exists (static t(\"…\") calls and the dynamic families)", () => {
  const dict: any = NDP.i18n.DICT["pt-BR"];
  const used = new Set<string>();
  for (const f of readdirSync(WEB).filter((n) => n.endsWith(".js") && n !== "i18n.js")) {
    const src = readFileSync(WEB + f, "utf8");
    for (const m of src.matchAll(/\bt\(\s*"([^"]+)"/g)) if (!m[1]!.endsWith(".")) used.add(m[1]!);
    for (const m of src.matchAll(/"(name\.[a-z]+)"/g)) used.add(m[1]!);
  }
  // families built at run time
  for (const s of ["connecting", "pairing", "ready", "offline", "replaced"]) used.add(`state.${s}`);
  for (const m of ["READ_ONLY", "DEVELOPMENT"]) used.add(`mode.${m}`);
  const codec = readFileSync(WEB + "codec.js", "utf8");
  const statuses = /const Status = \{([^}]*)\}/s.exec(codec)![1]!.matchAll(/([A-Z_]+):/g);
  for (const s of statuses) if (s[1] !== "OK") used.add(`err.${s[1]!}`);
  const faq = /const SECTIONS = \[([^\]]*)\]/.exec(readFileSync(WEB + "faq.js", "utf8"))![1]!.matchAll(/"([a-z]+)"/g);
  for (const s of faq) { used.add(`faq.${s[1]!}.q`); used.add(`faq.${s[1]!}.a`); }
  for (const tab of ["files", "settings", "faq"]) used.add(tab);
  const missing = [...used].filter((k) => !(k in dict));
  assert.deepEqual(missing, [], "keys used by the page but missing from the dictionaries");
  assert.ok(used.size > 150, "the scan found suspiciously few keys");
});

test("i18n: t() substitutes, falls back to the key, and honours the chosen language", () => {
  const I = NDP.i18n;
  I.setLang("en");
  assert.equal(I.lang(), "en");
  assert.equal(I.t("usedOf", "1 MiB", "4 MiB"), "1 MiB of 4 MiB used");
  assert.equal(I.t("no.such.key"), "no.such.key");
  assert.ok(I.has("close") && !I.has("no.such.key"));
  I.setLang("pt-BR");
  assert.equal(I.t("close"), "Fechar");
  assert.equal(I.t("itemsCount", 3), "3 item(ns)");
  I.setLang("auto");
  assert.equal(I.choice(), "auto");
});

test("i18n: tp() picks the platform override when one exists, and falls back otherwise", () => {
  const I = NDP.i18n;
  I.setLang("en");
  assert.equal(I.tp("faq.what.a", "dsi"), I.t("faq.what.a.dsi"));
  assert.notEqual(I.tp("faq.what.a", "dsi"), I.t("faq.what.a"));
  assert.equal(I.tp("faq.what.a", "3ds"), I.t("faq.what.a")); // no "faq.what.a.3ds" override: falls back
  assert.equal(I.tp("faq.what.a", undefined), I.t("faq.what.a"));
  assert.equal(I.tp("faq.keys.a", "made-up-platform"), I.t("faq.keys.a"));
  I.setLang("auto");
});

test("update: the page compares versions, caches GitHub's answer, and only trusts a real release", async () => {
  assert.equal(U.versionNewer("v1.7.2", "1.7.1"), true);
  assert.equal(U.versionNewer("1.10.0", "1.9.9"), true);
  assert.equal(U.versionNewer("1.7.1", "1.7.1"), false);
  assert.equal(U.versionNewer("1.7.0", "1.7.1"), false);
  assert.equal(U.versionNewer("junk", "1.7.1"), false);

  const store = new Map<string, string>();
  (globalThis as any).localStorage = { getItem: (k: string) => store.get(k) ?? null, setItem: (k: string, v: string) => void store.set(k, v), removeItem: (k: string) => void store.delete(k) };
  let calls = 0, body: any = { tag_name: "v1.7.2", draft: false, prerelease: false };
  (globalThis as any).fetch = async () => { calls++; return { ok: true, json: async () => body }; };
  const up = NDP.update;

  assert.equal(await up.fetchLatest(), "v1.7.2");
  assert.equal(up.newerThan("1.7.1"), "v1.7.2");
  assert.equal(up.newerThan("1.7.2"), "", "an up-to-date console sees nothing");
  assert.equal(await up.fetchLatest(), "v1.7.2");
  assert.equal(calls, 1, "the second ask is served from the cache (GitHub rate limit)");

  up.dismiss("v1.7.2");
  assert.equal(up.newerThan("1.7.1"), "", "a dismissed version stays quiet");

  store.clear();
  body = { tag_name: "v9.9.9", draft: false, prerelease: true };
  assert.equal(await up.fetchLatest(), null, "a pre-release is never offered");
  body = { tag_name: "latest", draft: false, prerelease: false };
  assert.equal(await up.fetchLatest(), null, "an odd tag is ignored");
  (globalThis as any).fetch = async () => { throw new Error("offline"); };
  assert.equal(await up.fetchLatest(), null, "no network: no banner, no error");
});
