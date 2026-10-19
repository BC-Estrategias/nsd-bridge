/* Pure helpers for the page (paths, sizes, names, access rules). No DOM here, so they are unit-tested in Node. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});

  const asciiFold = (s) => s.replace(/[A-Z]/g, (c) => c.toLowerCase());
  const comps = (p) => p.split("/").filter(Boolean).map(asciiFold);

  /** Same rule as the console (spec §10): component-wise prefix, ASCII case folded. */
  function inside(path, root) {
    const r = comps(root), p = comps(path);
    if (p.length < r.length) return false;
    for (let i = 0; i < r.length; i++) if (p[i] !== r[i]) return false;
    return true;
  }

  const join = (dir, name) => (dir === "/" ? "/" + name : dir + "/" + name);
  function parent(path) {
    if (path === "/") return "/";
    const i = path.lastIndexOf("/");
    return i <= 0 ? "/" : path.slice(0, i);
  }
  const basename = (path) => path.slice(path.lastIndexOf("/") + 1);
  function ext(name) {
    const i = name.lastIndexOf(".");
    return i > 0 ? name.slice(i + 1).toLowerCase() : "";
  }

  function crumbs(path) {
    const out = [{ name: "/", path: "/" }];
    let cur = "";
    for (const c of path.split("/").filter(Boolean)) { cur += "/" + c; out.push({ name: c, path: cur }); }
    return out;
  }

  function formatSize(n) {
    if (n === undefined || n === null) return "";
    if (n < 1024) return `${n} B`;
    const u = ["KiB", "MiB", "GiB", "TiB"];
    let v = n / 1024, i = 0;
    while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
    return `${v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)} ${u[i]}`;
  }
  function formatRate(bytesPerSec) { return bytesPerSec > 0 ? `${formatSize(Math.round(bytesPerSec))}/s` : ""; }
  function formatEta(seconds) {
    if (!isFinite(seconds) || seconds < 0) return "";
    if (seconds < 60) return `${Math.ceil(seconds)} s`;
    const m = Math.floor(seconds / 60), s = Math.round(seconds % 60);
    return m < 60 ? `${m} min ${s} s` : `${Math.floor(m / 60)} h ${m % 60} min`;
  }

  const TEXT_EXT = new Set(["txt", "log", "md", "json", "ini", "cfg", "conf", "xml", "csv", "tsv", "yml", "yaml", "toml", "c", "h", "cpp", "hpp", "js", "ts", "py", "sh", "lua", "html", "css", "rsf", "plginfo", "ips", "patch", "diff", "bak", "gitignore", "makefile", "mk", "s", "asm", "cmake", "list", "lst", "ver", "info", "readme", "cheats", "pnm"]);
  const IMAGE_EXT = new Set(["png", "jpg", "jpeg", "gif", "webp", "bmp"]);
  const isImage = (name) => IMAGE_EXT.has(ext(name));
  const isTextName = (name) => TEXT_EXT.has(ext(name)) || /^(makefile|readme|license|notice)$/i.test(name);
  const imageMime = (name) => ({ png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", bmp: "image/bmp" })[ext(name)] || "application/octet-stream";

  /** Looks binary if it has NUL bytes or is mostly control characters (checked on a sample). */
  function looksBinary(bytes) {
    const n = Math.min(bytes.length, 4096);
    let bad = 0;
    for (let i = 0; i < n; i++) {
      const b = bytes[i];
      if (b === 0) return true;
      if (b < 9 || (b > 13 && b < 32)) bad++;
    }
    return n > 0 && bad / n > 0.1;
  }

  /** "photo.png" -> "photo (1).png" ... the first name not in `taken` (a Set of lower-cased names). */
  function uniqueName(name, taken) {
    const low = (s) => asciiFold(s);
    if (!taken.has(low(name))) return name;
    const i = name.lastIndexOf(".");
    const stem = i > 0 ? name.slice(0, i) : name, tail = i > 0 ? name.slice(i) : "";
    for (let n = 1; n < 10000; n++) {
      const cand = `${stem} (${n})${tail}`;
      if (!taken.has(low(cand))) return cand;
    }
    return `${stem} (${Date.now()})${tail}`;
  }

  /** FAT-illegal characters and the 255-byte limit (spec §10): returns an error key or null. */
  function nameProblem(name) {
    if (!name || name === "." || name === "..") return "name.empty";
    if (/[\\/:*?"<>|\u0000-\u001f\u007f]/.test(name)) return "name.chars";
    if (/[. ]$/.test(name)) return "name.trailing";
    if (/~\d/.test(name)) return "name.tilde";
    if (new TextEncoder().encode(name).length > 255) return "name.long";
    return null;
  }

  function sortEntries(entries, key, dir) {
    const f = dir === "desc" ? -1 : 1;
    const cmp = (a, b) => {
      if (a.type !== b.type) return a.type === "dir" ? -1 : 1; // folders first, always
      if (key === "size") { const d = (a.size - b.size) * f; if (d) return d; }
      return f * a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: "base" });
    };
    return entries.slice().sort(cmp);
  }

  // The console's fixed protected zones (docs spec §11): never writable, whatever the owner opens.
  // Different per platform (each agent overrides agent/common's 3DS defaults with its own SD layout --
  // see ndp_policy_set_default_zones() -- so the page must match, not assume every console is a 3DS).
  const PROTECTED_ZONES = {
    "3ds": {
      neverWrite: ["/Nintendo 3DS", "/luma", "/boot.firm", "/gm9", "/private", "/3ds/nintendo-dev-agent/config"],
      writeExcept: ["/luma/plugins", "/luma/titles"],
    },
    dsi: { neverWrite: ["/_nds", "/nsd-bridge/config"], writeExcept: [] },
  };
  /** The platform's protected-zone tables, falling back to the 3DS's (also the default when `platform`
   * is unknown/omitted: existing callers -- and existing tests -- assume that console). */
  function protectedZonesFor(platform) { return PROTECTED_ZONES[platform] || PROTECTED_ZONES["3ds"]; }
  function writeProtected(path, platform) {
    const { neverWrite, writeExcept } = protectedZonesFor(platform);
    for (const z of neverWrite) {
      if (!inside(path, z)) continue;
      const lifted = writeExcept.some((e) => inside(path, e) && inside(e, z) && !inside(z, e));
      if (!lifted) return true;
    }
    return false;
  }

  /** What the page can promise about `path` given ACCESS_INFO: "write" | "read" | "none" (from the console's view). */
  function accessOf(path, access, mode, platform) {
    if (!access) return "read";
    const canWrite = mode !== "READ_ONLY" && access.writeRoots.some((r) => inside(path, r)) && !writeProtected(path, platform);
    if (canWrite) return "write";
    return access.readRoots.some((r) => inside(path, r)) ? "read" : "none";
  }

  /** The trash a path lives in: "<root>/.ndp-trash" (returns its path) or null. */
  function trashRootOf(path) {
    const i = path.indexOf("/.ndp-trash");
    if (i < 0) return null;
    const after = path.slice(i + "/.ndp-trash".length);
    return after === "" || after.startsWith("/") ? path.slice(0, i + "/.ndp-trash".length) : null;
  }

  /** Collects downloaded bytes into Blobs of ~8 MiB: the browser can keep big Blobs on disk, so a large download does not
   * sit in JavaScript memory as one huge list of arrays. `add` copies (the caller's buffer is reused). */
  function blobCollector(groupBytes) {
    const limit = groupBytes || 8 * 1024 * 1024;
    const blobs = [];
    let pending = [], pendingBytes = 0, total = 0;
    return {
      add(bytes) {
        pending.push(bytes.slice());
        pendingBytes += bytes.length;
        total += bytes.length;
        if (pendingBytes >= limit) { blobs.push(new Blob(pending)); pending = []; pendingBytes = 0; }
      },
      get size() { return total; },
      blob(type) {
        if (pending.length) { blobs.push(new Blob(pending)); pending = []; pendingBytes = 0; }
        return new Blob(blobs, type ? { type } : undefined);
      },
    };
  }

  /** "v1.7.2" / "1.7.2" -> [1,7,2], or null. */
  function parseVersion(v) {
    const m = /^v?(\d{1,4})\.(\d{1,4})\.(\d{1,4})$/.exec(String(v));
    return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
  }
  /** True when `candidate` is a newer x.y.z than `installed` (an unparsable version is never newer). */
  function versionNewer(candidate, installed) {
    const a = parseVersion(candidate), b = parseVersion(installed);
    if (!a || !b) return false;
    for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i];
    return false;
  }

  NDP.util = { versionNewer, parseVersion, blobCollector, inside, join, parent, basename, ext, crumbs, formatSize, formatRate, formatEta, isImage, isTextName, imageMime, looksBinary, uniqueName, nameProblem, sortEntries, writeProtected, accessOf, trashRootOf, protectedZonesFor };
})();
