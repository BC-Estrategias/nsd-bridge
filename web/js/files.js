/* The Files tab: browse the SD card, upload by drag and drop, download, rename/move, delete to the console's trash,
 * restore from it, and open text/images in the viewer. Every operation goes through the single NDP session, one at a
 * time (the console serves one request at a time). */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { h, fill, icon, toast, dialog, confirmBox, promptBox, contextMenu, copyText, saveBlob } = NDP.ui;
  const U = NDP.util, S = NDP.session;
  const t = (...a) => NDP.i18n.t(...a);
  const tp = (key, ...args) => NDP.i18n.tp(key, S.info && S.info.platform, ...args);

  const st = { cwd: "/", entries: [], selected: new Set(), sort: { key: "name", dir: "asc" }, filter: "", loading: false, error: null, last: null };
  let root, uploads = [], upBusy = false, upPanel = null, clip = null; // clip: {mode: "cut"|"copy", dir, entries}
  const prefs = () => NDP.prefs;

  const hashPath = () => "#" + st.cwd;
  const pathFromHash = () => { const p = decodeURIComponent(location.hash.replace(/^#/, "") || "/"); return p.startsWith("/") ? p : "/"; };
  const inTrash = () => !!U.trashRootOf(st.cwd);
  const canCreate = () => canWrite() && !inTrash();
  const canWrite = () => U.accessOf(st.cwd, S.access, S.info && S.info.mode, S.info && S.info.platform) === "write";
  const isReadOnlyMode = () => S.info && S.info.mode === "READ_ONLY";

  function errText(e) {
    if (!e) return "";
    if (e.statusName) {
      const key = "err." + e.statusName;
      const known = NDP.i18n.has(key) ? t(key) : e.statusName;
      return e.detail && !/^(path not|writes are|already|no such)/i.test(e.detail) ? `${known} (${e.detail})` : known;
    }
    return e.message || String(e);
  }
  const fail = (e, what) => toast((what ? what + ": " : "") + errText(e), "error", 8000);

  // ---------------------------------------------------------------------------------------------- loading
  async function load(path, opts = {}) {
    if (path !== undefined) st.cwd = path;
    st.loading = true;
    st.error = null;
    if (!opts.keepSelection) st.selected.clear();
    render();
    try {
      st.entries = await S.run((c) => c.list(st.cwd), { retry: true });
    } catch (e) {
      st.entries = [];
      st.error = e;
      if (e.statusName === "NOT_FOUND" && U.trashRootOf(st.cwd) === st.cwd) st.error = null; // the trash is created by the first deletion: until then it is simply empty
      else if (e.statusName === "NOT_FOUND" && st.cwd !== "/") { st.loading = false; return load(U.parent(st.cwd)); }
    }
    st.loading = false;
    if (opts.keepSelection) st.selected = new Set([...st.selected].filter((n) => st.entries.some((e) => e.name === n)));
    if (location.hash !== hashPath()) history.replaceState(null, "", hashPath());
    render();
  }
  const refresh = () => load(undefined, { keepSelection: true });
  const go = (path) => { history.pushState(null, "", "#" + path); return load(path); };

  // ---------------------------------------------------------------------------------------------- selection helpers
  const visible = () => {
    let list = st.entries;
    if (!prefs().showHidden) list = list.filter((e) => !e.name.startsWith(".") || e.name === ".ndp-trash");
    if (st.filter) { const f = st.filter.toLowerCase(); list = list.filter((e) => e.name.toLowerCase().includes(f)); }
    return U.sortEntries(list, st.sort.key, st.sort.dir);
  };
  const selectedEntries = () => st.entries.filter((e) => st.selected.has(e.name));
  const full = (name) => U.join(st.cwd, name);

  // ---------------------------------------------------------------------------------------------- operations
  async function download(entry) {
    if (entry.type === "dir") return;
    const id = "dl:" + entry.name;
    const kill = toast(t("downloading", entry.name), "info", 0);
    try {
      const col = U.blobCollector();
      await S.run((c) => c.read(full(entry.name), { onProgress: (n, total) => kill.set(t("downloadingPct", entry.name, total ? Math.round((100 * n) / total) : 0)) }, (b) => col.add(b)));
      saveBlob(col.blob(), entry.name);
      toast(t("downloaded", entry.name), "ok", 2500);
    } catch (e) { fail(e, entry.name); if (S.client.isClosed) S.connect(); }
    kill();
    void id;
  }

  async function remove(list) {
    if (!list.length) return;
    if (prefs().confirmDelete) {
      const msg = list.length === 1 ? t("confirmTrash1", list[0].name) : t("confirmTrashN", list.length);
      if (!(await confirmBox(t("moveToTrash"), msg + " " + t("trashNote"), t("moveToTrash"), true))) return;
    }
    let ok = 0;
    for (const e of list) {
      try { await S.run((c) => c.delete(full(e.name))); ok++; } catch (err) { fail(err, e.name); }
    }
    if (ok) toast(t("movedToTrash", ok), "ok", 3000);
    await refresh();
  }

  /** Permanent deletion, only inside a trash (the console refuses it anywhere else). `targets` are full paths: items in a
   * trash, or the trash folder itself (which empties it). The console works in small batches; a dialog shows the count. */
  async function purge(targets, empty, label) {
    if (!targets.length) return;
    const title = empty ? t("emptyTrash") : t("deleteForever");
    const msg = empty ? t("confirmEmpty") : targets.length === 1 ? t("confirmPurge1", label) : t("confirmPurgeN", targets.length);
    if (!(await confirmBox(title, msg + " " + t("cannotUndo"), title, true))) return;
    let cancelled = false, total = 0, failed = false;
    const status = h("p", { text: t("purging", 0) });
    dialog({ title, body: status, buttons: [{ label: t("cancel"), value: "cancel" }] }).then(() => { cancelled = true; });
    try {
      for (const p of targets) {
        const r = await S.run((c) => c.purge(p, (n) => { status.textContent = t("purging", total + n); return !cancelled; }));
        total += r.removed;
        if (!r.complete) break;
      }
    } catch (e) { failed = true; fail(e, title); if (S.client && S.client.isClosed) S.connect(); }
    const dlgs = document.querySelectorAll(".backdrop .dialog");
    if (dlgs.length) dlgs[dlgs.length - 1].close(undefined);
    if (!failed) toast(cancelled ? t("purgeStopped", total) : t("purged", total), cancelled ? "info" : "ok", 4000);
    await refresh();
  }

  async function rename(entry) {
    const name = await promptBox(t("rename"), t("newName"), entry.name, (v) => (U.nameProblem(v) ? t(U.nameProblem(v)) : null));
    if (!name || name === entry.name) return;
    let ok = false;
    try { await S.run((c) => c.rename(full(entry.name), full(name))); toast(t("renamed"), "ok", 2000); ok = true; } catch (e) { fail(e, entry.name); }
    if (ok) st.selected = new Set([name]);
    await refresh();
  }

  async function moveInto(names, targetDir) {
    const entries = st.entries.filter((e) => names.includes(e.name)).map((e) => ({ name: e.name, type: e.type, size: e.size }));
    if (!entries.length) return;
    await NDP.transfer.run({ mode: "move", entries, srcDir: st.cwd, destDir: targetDir });
    st.selected.clear();
    await refresh();
  }

  // ---------------------------------------------------------------------------------------------- clipboard, move/copy to..., trash shortcut
  const canPickDest = (p) => U.accessOf(p, S.access, S.info && S.info.mode, S.info && S.info.platform) === "write" && !U.trashRootOf(p);
  const sameNames = (list) => list.map((e) => ({ name: e.name, type: e.type, size: e.size }));

  function setClip(mode) {
    const sel = selectedEntries();
    if (!sel.length) return;
    clip = { mode, dir: st.cwd, entries: sameNames(sel) };
    toast(t(mode === "cut" ? "cutN" : "copiedToClipboard", sel.length), "info", 2500);
    syncSelection();
  }

  async function paste() {
    if (!clip || !canCreate()) return;
    const c = clip;
    const r = await NDP.transfer.run({ mode: c.mode === "cut" ? "move" : "copy", entries: c.entries, srcDir: c.dir, destDir: st.cwd });
    if (c.mode === "cut" && !r.cancelled && !r.failed) clip = null;
    st.selected.clear();
    await refresh();
  }

  async function transferTo(mode) {
    const sel = selectedEntries();
    if (!sel.length) return;
    const dest = await NDP.transfer.pickFolder({ title: t(mode === "move" ? "moveTo" : "copyTo"), okLabel: t(mode === "move" ? "moveHere" : "copyHere"), start: st.cwd, canPick: canPickDest });
    if (!dest) return;
    await NDP.transfer.run({ mode, entries: sameNames(sel), srcDir: st.cwd, destDir: dest });
    st.selected.clear();
    await refresh();
  }

  /** The trash that FS_DELETE uses for what is in this folder: "<most specific writable root>/.ndp-trash". */
  function trashTargets() {
    const roots = (S.access && S.access.writeRoots) || [];
    let best = null;
    for (const r of roots) if (U.inside(st.cwd, r) && (best === null || r.length > best.length)) best = r;
    return (best !== null ? [best] : roots).map((r) => (r === "/" ? "/.ndp-trash" : r + "/.ndp-trash"));
  }
  function openTrash(ev) {
    const inside = U.trashRootOf(st.cwd);
    if (inside) return go(U.parent(inside));
    const list = trashTargets();
    if (!list.length) { toast(t("noTrashYet"), "info", 4000); return; }
    if (list.length === 1) return go(list[0]);
    const r = ev.currentTarget.getBoundingClientRect();
    contextMenu(r.left, r.bottom + 4, list.map((p) => ({ label: p, icon: "trash", run: () => go(p) })));
  }

  async function restore(entry) {
    const root = U.trashRootOf(full(entry.name));
    if (!root) return;
    const base = U.parent(root);
    const taken = new Set((await S.run((c) => c.list(base)).catch(() => [])).map((e) => e.name.toLowerCase()));
    const name = U.uniqueName(entry.name.replace(/\.\d+$/, ""), taken);
    try { await S.run((c) => c.rename(full(entry.name), U.join(base, name))); toast(t("restoredTo", U.join(base, name)), "ok", 4000); } catch (e) { fail(e, entry.name); }
    await refresh();
  }

  async function newFolder() {
    const taken = new Set(st.entries.map((e) => e.name.toLowerCase()));
    const name = await promptBox(t("newFolder"), t("folderName"), U.uniqueName(t("newFolderDefault"), taken), (v) => (U.nameProblem(v) ? t(U.nameProblem(v)) : null));
    if (!name) return;
    const kill = toast(t("creatingFolder"), "info", 0);
    try { await S.run((c) => c.mkdir(full(name))); } catch (e) { fail(e, name); }
    kill();
    await refresh();
  }

  async function newFile() {
    const taken = new Set(st.entries.map((e) => e.name.toLowerCase()));
    const name = await promptBox(t("newFile"), t("fileName"), U.uniqueName("new.txt", taken), (v) => (U.nameProblem(v) ? t(U.nameProblem(v)) : null));
    if (!name) return;
    try { await S.run((c) => c.write(full(name), new Uint8Array(0))); } catch (e) { fail(e, name); await refresh(); return; }
    await refresh();
    NDP.viewer.open(full(name), { size: 0 });
  }

  // ---------------------------------------------------------------------------------------------- uploads
  function upStats(u) {
    const dt = (performance.now() - u.t0) / 1000;
    u.rate = dt > 0.5 ? u.sent / dt : 0;
  }

  function renderUploads() {
    if (!upPanel) { upPanel = h("div#uploads"); document.body.append(upPanel); }
    upPanel.replaceChildren();
    if (!uploads.length) { upPanel.classList.remove("show"); return; }
    upPanel.classList.add("show");
    const active = uploads.filter((u) => u.state === "queued" || u.state === "hashing" || u.state === "uploading").length;
    upPanel.append(h("div.up-head", null, h("strong", { text: active ? t("uploadingN", active) : t("uploadsDone") }),
      h("span.grow"), h("button.icon-btn", { title: t("clearFinished"), onclick: () => { uploads = uploads.filter((u) => ["queued", "hashing", "uploading"].includes(u.state)); renderUploads(); } }, icon("check")),
      h("button.icon-btn", { title: t("close"), onclick: () => { for (const u of uploads) u.ctl.abort(); uploads = []; renderUploads(); } }, icon("close"))));
    const list = h("div.up-list");
    for (const u of uploads.slice(-40)) {
      const pct = u.size ? Math.min(100, Math.round((100 * u.sent) / u.size)) : u.state === "done" ? 100 : 0;
      const status = u.state === "hashing" ? t("hashing") : u.state === "uploading" ? `${pct}% · ${U.formatRate(u.rate)}${u.rate ? " · " + U.formatEta((u.size - u.sent) / u.rate) : ""}` :
        u.state === "queued" ? t("queued") : u.state === "done" ? t("uploaded") : u.state === "skipped" ? t("skipped") : u.state === "canceled" ? t("canceled") : u.errorText || t("failed");
      list.append(h("div.up-item." + u.state, null,
        h("div.up-name", { title: u.dest }, u.name),
        h("div.up-bar", null, h("div.up-fill", { style: `width:${u.state === "hashing" ? Math.round((100 * u.hashed) / (u.size || 1)) : pct}%` })),
        h("div.up-status", null, h("span", { text: `${U.formatSize(u.size)} · ${status}` }),
          (u.state === "queued" || u.state === "hashing" || u.state === "uploading") ? h("button.link", { onclick: () => u.ctl.abort(), text: t("cancel") }) : null)));
    }
    upPanel.append(list);
  }

  /** files: [{file, dir (target folder), rel (path shown), mkdirs: [paths to create first]}] */
  function enqueue(items) {
    for (const it of items) {
      uploads.push({ ...it, name: it.file.name, dest: U.join(it.dir, it.file.name), size: it.file.size, sent: 0, hashed: 0, rate: 0, state: "queued", ctl: new AbortController(), conflict: null });
    }
    renderUploads();
    if (!upBusy) pump();
  }

  const conflictMemo = { choice: null };
  async function askConflict(u) {
    if (conflictMemo.choice) return conflictMemo.choice;
    const all = h("input", { type: "checkbox" });
    const r = await dialog({
      title: t("alreadyExists"), body: h("div", null, h("p", { text: t("existsBody", u.name) }), h("label.check", null, all, h("span", { text: t("applyToAll") }))),
      buttons: [{ label: t("skip"), value: "skip" }, { label: t("keepBoth"), value: "both" }, { label: t("replaceKeepBackup"), value: "backup", primary: true }, { label: t("replace"), value: "replace", danger: true }],
    });
    const choice = r || "skip";
    if (all.checked) conflictMemo.choice = choice;
    return choice;
  }

  async function ensureDirs(u) {
    for (const d of u.mkdirs || []) {
      if (U.inside(d, "/") && !(await dirExists(d))) {
        try { await S.run((c) => c.mkdir(d)); } catch (e) { if (e.statusName !== "EXISTS") throw e; }
        dirCache.add(d.toLowerCase());
      }
    }
  }
  const dirCache = new Set();
  async function dirExists(d) {
    if (dirCache.has(d.toLowerCase())) return true;
    try { const s = await S.run((c) => c.stat(d)); if (s.type === "dir") { dirCache.add(d.toLowerCase()); return true; } } catch (_) { /* absent */ }
    return false;
  }

  async function pump() {
    upBusy = true;
    conflictMemo.choice = null;
    while (true) {
      const u = uploads.find((x) => x.state === "queued");
      if (!u) break;
      try {
        if (u.ctl.signal.aborted) { u.state = "canceled"; continue; }
        u.state = "hashing"; u.t0 = performance.now(); renderUploads();
        await ensureDirs(u);
        // name clash?
        let taken = null;
        try { taken = new Set((await S.run((c) => c.list(u.dir))).map((e) => e.name.toLowerCase())); } catch (_) { taken = new Set(); }
        let name = u.name, opts = {};
        if (taken.has(name.toLowerCase())) {
          const choice = await askConflict(u);
          if (choice === "skip") { u.state = "skipped"; continue; }
          if (choice === "both") name = U.uniqueName(name, taken);
          else opts = { overwrite: true, backup: choice === "backup" };
        }
        u.name = name; u.dest = U.join(u.dir, name);
        u.state = "hashing";
        await S.run((c) => c.write(u.dest, u.file, {
          ...opts, signal: u.ctl.signal,
          onHashProgress: (n) => { u.hashed = n; renderUploadsSoon(); },
          onProgress: (n) => { if (u.state !== "uploading") { u.state = "uploading"; u.t0 = performance.now(); } u.sent = n; upStats(u); renderUploadsSoon(); },
        }));
        u.sent = u.size; u.state = "done";
      } catch (e) {
        if (u.ctl.signal.aborted) u.state = "canceled";
        else { u.state = "error"; u.errorText = errText(e); }
        if (S.client.isClosed) await S.connect();
      }
      renderUploads();
      if (st.cwd === u.dir) refreshSoon();
    }
    upBusy = false;
    renderUploads();
    await refresh();
    const done = uploads.filter((u) => u.state === "done").length, bad = uploads.filter((u) => u.state === "error").length;
    if (done || bad) toast(bad ? t("uploadFinishedErrors", done, bad) : t("uploadFinished", done), bad ? "error" : "ok", 5000);
  }
  let rafUp = 0, rafRefresh = 0;
  const renderUploadsSoon = () => { if (!rafUp) rafUp = setTimeout(() => { rafUp = 0; renderUploads(); }, 250); };
  const refreshSoon = () => { if (!rafRefresh) rafRefresh = setTimeout(() => { rafRefresh = 0; refresh(); }, 600); };

  // File / folder pickers and drops --------------------------------------------------------------
  function readEntries(reader) { return new Promise((res, rej) => reader.readEntries(res, rej)); }
  async function walk(entry, base, out) {
    if (entry.isFile) { const file = await new Promise((res, rej) => entry.file(res, rej)); out.push({ file, rel: base }); return; }
    if (entry.isDirectory) {
      const reader = entry.createReader();
      out.dirs.push(base + "/" + entry.name);
      for (;;) {
        const batch = await readEntries(reader);
        if (!batch.length) break;
        for (const child of batch) await walk(child, base + "/" + entry.name, out);
      }
    }
  }

  async function itemsFromDrop(dt, targetDir) {
    const out = Object.assign([], { dirs: [] });
    const entries = [];
    if (dt.items && dt.items.length && dt.items[0].webkitGetAsEntry) {
      for (const it of dt.items) { if (it.kind === "file") { const e = it.webkitGetAsEntry(); if (e) entries.push(e); else { const f = it.getAsFile(); if (f) out.push({ file: f, rel: "" }); } } }
      for (const e of entries) await walk(e, "", out);
    } else {
      for (const f of dt.files) out.push({ file: f, rel: "" });
    }
    return out.map((x) => {
      const parts = x.rel.split("/").filter(Boolean);
      const dir = parts.length ? U.join(targetDir, parts.join("/")) : targetDir;
      const mk = [];
      let cur = targetDir;
      for (const p of parts) { cur = U.join(cur, p); mk.push(cur); }
      return { file: x.file, dir, mkdirs: mk };
    });
  }

  async function startUpload(items, targetDir) {
    if (!items.length) return;
    if (isReadOnlyMode()) { toast(t("readOnlyBanner"), "error", 6000); return; }
    const access = U.accessOf(targetDir, S.access, S.info.mode, S.info.platform);
    if (access !== "write" || U.trashRootOf(targetDir)) { toast(t("notWritableHere"), "error", 7000); return; }
    // the card is FAT32: no file of 4 GiB or more; and it must fit (checked against the free space the console reports)
    const tooBig = items.filter((i) => i.file.size >= 4 * 1024 * 1024 * 1024);
    if (tooBig.length) { toast(tp("fat32Limit", tooBig[0].file.name), "error", 9000); items = items.filter((i) => !tooBig.includes(i)); if (!items.length) return; }
    const need = items.reduce((n, i) => n + i.file.size, 0);
    if (need > 8 * 1024 * 1024) { try { await S.refreshContext(); } catch (_) { /* use what we have */ } }
    const free = S.device && S.device.sd && S.device.sd.free;
    if (free !== undefined && need > free) { toast(t("noRoom", U.formatSize(need), U.formatSize(free)), "error", 10000); return; }
    const folders = new Set(items.flatMap((i) => i.mkdirs));
    if (folders.size && !(await confirmBox(t("uploadFolders"), t("uploadFoldersNote", folders.size), t("ok")))) return;
    enqueue(items);
  }

  // ---------------------------------------------------------------------------------------------- opening things
  function open(entry) {
    if (entry.type === "dir") return go(full(entry.name));
    NDP.viewer.open(full(entry.name), entry);
  }

  function rowMenu(e, entry) {
    e.preventDefault();
    if (!st.selected.has(entry.name)) { st.selected = new Set([entry.name]); syncSelection(); }
    const sel = selectedEntries();
    const one = sel.length === 1 ? sel[0] : null;
    const writable = canWrite();
    const inTrash = !!U.trashRootOf(st.cwd) || (one && one.name === ".ndp-trash");
    contextMenu(e.clientX, e.clientY, [
      one && { label: one.type === "dir" ? t("open") : t("view"), icon: "file", run: () => open(one) },
      one && one.type === "file" && { label: t("download"), icon: "download", run: () => download(one) },
      "-",
      one && { label: t("rename"), icon: "edit", disabled: !writable, run: () => rename(one) },
      one && U.trashRootOf(full(one.name)) && { label: t("restore"), icon: "restore", run: () => restore(one) },
      inTrash && !(one && one.name === ".ndp-trash") ? { label: t("deleteForever"), icon: "trash", danger: true, disabled: !writable, run: () => purge(sel.map((e) => full(e.name)), false, sel[0].name) } : { label: t("moveToTrash"), icon: "trash", danger: true, disabled: !writable || inTrash, run: () => remove(sel) },
      one && one.name === ".ndp-trash" && one.type === "dir" && !U.trashRootOf(st.cwd) ? { label: t("emptyTrash"), icon: "trash", danger: true, disabled: !writable, run: () => purge([full(one.name)], true) } : null,
      "-",
      { label: t("cut"), icon: "cut", disabled: !writable || (inTrash && !U.trashRootOf(st.cwd)), run: () => setClip("cut") },
      { label: t("copy"), icon: "copy", run: () => setClip("copy") },
      clip && canCreate() ? { label: t("pasteN", clip.entries.length), icon: "paste", run: paste } : null,
      { label: t("moveTo"), icon: "move", disabled: !writable, run: () => transferTo("move") },
      { label: t("copyTo"), icon: "copy", run: () => transferTo("copy") },
      "-",
      one && { label: t("copyPath"), icon: "copy", run: () => copyText(full(one.name)) },
    ].filter(Boolean));
  }

  // ---------------------------------------------------------------------------------------------- rendering
  function banner() {
    const acc = U.accessOf(st.cwd, S.access, S.info && S.info.mode, S.info && S.info.platform);
    if (st.error) {
      if (st.error.statusName === "PROTECTED_PATH") return h("div.banner.warn", null, icon("lock"), h("div", null, h("strong", { text: t("notOpenedTitle") }), h("p", { text: t("notOpenedBody") })));
      return h("div.banner.error", null, icon("warn"), h("div", null, h("strong", { text: t("cannotList") }), h("p", { text: errText(st.error) })));
    }
    if (U.trashRootOf(st.cwd)) return h("div.banner.info", null, icon("trash"), h("div", null, h("p", { text: t("trashBanner") })));
    if (isReadOnlyMode()) return h("div.banner.warn", null, icon("lock"), h("div", null, h("strong", { text: t("readOnlyBanner") }), h("p", { text: t("readOnlyHow") })));
    if (acc === "read" && S.access) return h("div.banner.info", null, icon("lock"), h("div", null, h("p", { text: t("folderReadOnly") })));
    return null;
  }

  function sortHeader(key, label) {
    const active = st.sort.key === key;
    return h("th" + (active ? ".sorted" : ""), { onclick: () => { st.sort = { key, dir: active && st.sort.dir === "asc" ? "desc" : "asc" }; render(); }, "aria-sort": active ? (st.sort.dir === "asc" ? "ascending" : "descending") : "none" },
      label, active ? h("span.arrow", { text: st.sort.dir === "asc" ? " ▲" : " ▼" }) : null);
  }

  let actionsEl = null;
  /** The action bar depends on the selection: redrawn on its own so a click does not rebuild the rows (a double click needs the same row twice). */
  function renderActions() {
    if (!actionsEl) return;
    const writable = canWrite(), list = visible(), sel = selectedEntries();
    const one = sel.length === 1 ? sel[0] : null;
    const trashRoot = U.trashRootOf(st.cwd);
    fill(actionsEl,
      h("button", { disabled: !canCreate(), onclick: () => filePicker.click() }, icon("upload"), h("span", { text: t("upload") })),
      S.info && S.info.platform === "3ds" ? h("button", { onclick: () => NDP.install.open(one && one.type === "file" && NDP.install.isCia(one.name) ? full(one.name) : null), title: t("inst.hint") }, icon("download"), h("span", { text: t("inst.button") })) : null,
      S.info && S.info.platform === "3ds" ? h("button", { onclick: () => NDP.sources.open(), title: t("src.hint") }, icon("download"), h("span", { text: t("src.button") })) : null,
      h("button", { disabled: !canCreate(), onclick: newFolder }, icon("plus"), h("span", { text: t("newFolder") })),
      h("button", { disabled: !canCreate(), onclick: newFile }, icon("newfile"), h("span", { text: t("newFile") })),
      h("button" + (trashRoot ? "" : ".trashbtn"), { onclick: openTrash, title: t("trashShortcutHint") }, icon("trash"), h("span", { text: trashRoot ? t("leaveTrash") : t("trash") })),
      h("span.sep-v"),
      h("button", { disabled: !(one && one.type === "file"), onclick: () => download(one) }, icon("download"), h("span", { text: t("download") })),
      h("button", { disabled: !(one && writable), onclick: () => rename(one) }, icon("edit"), h("span", { text: t("rename") })),
      h("button", { disabled: !(sel.length && writable), onclick: () => setClip("cut") }, icon("cut"), h("span", { text: t("cut") })),
      h("button", { disabled: !sel.length, onclick: () => setClip("copy") }, icon("copy"), h("span", { text: t("copy") })),
      h("button", { disabled: !(clip && canCreate()), onclick: paste, title: clip ? t("pasteHint", clip.entries.length, clip.dir) : "" }, icon("paste"), h("span", { text: clip ? t("pasteN", clip.entries.length) : t("paste") })),
      h("button", { disabled: !(sel.length && writable), onclick: () => transferTo("move") }, icon("move"), h("span", { text: t("moveTo") })),
      h("button", { disabled: !sel.length, onclick: () => transferTo("copy") }, icon("copy"), h("span", { text: t("copyTo") })),
      h("span.sep-v"),
      inTrash() ? null : h("button.danger", { disabled: !(sel.length && writable), onclick: () => remove(sel) }, icon("trash"), h("span", { text: t("moveToTrash") })),
      inTrash() ? h("button.danger", { disabled: !(sel.length && writable), onclick: () => purge(sel.map((e) => full(e.name)), false, sel[0].name) }, icon("trash"), h("span", { text: t("deleteForever") })) : null,
      inTrash() ? h("button.danger", { disabled: !(writable && list.length), onclick: () => purge([trashRoot], true) }, icon("trash"), h("span", { text: t("emptyTrash") })) : null,
      h("span.grow"),
      h("span.count", { text: t("itemsCount", list.length) + (sel.length ? " · " + t("selectedN", sel.length) : "") }));
  }

  function syncSelection() {
    for (const tr of root.querySelectorAll("tbody tr")) {
      const on = st.selected.has(tr.dataset.name);
      tr.classList.toggle("selected", on);
      tr.classList.toggle("cut", !!clip && clip.mode === "cut" && clip.dir === st.cwd && clip.entries.some((x) => x.name === tr.dataset.name));
      const cb = tr.querySelector("input.cb");
      if (cb) cb.checked = on;
    }
    const head = root.querySelector("input.cball");
    if (head) {
      const names = visible().map((x) => x.name), n = names.filter((x) => st.selected.has(x)).length;
      head.checked = n > 0 && n === names.length;
      head.indeterminate = n > 0 && n < names.length;
    }
    renderActions();
  }

  function render() {
    if (!root) return;
    const tb = h("div.toolbar", null,
      h("button.icon-btn", { title: t("up"), disabled: st.cwd === "/", onclick: () => go(U.parent(st.cwd)) }, icon("up")),
      h("nav.crumbs", { "aria-label": "path" }, U.crumbs(st.cwd).map((c, i, a) => h("span.crumb-wrap", null, i ? h("span.sep", { text: "/" }) : null, h("button.crumb" + (i === a.length - 1 ? ".current" : ""), { onclick: () => go(c.path), text: c.name })))),
      h("span.grow"),
      h("input.filter", { type: "search", placeholder: t("filter"), value: st.filter, oninput: (e) => { st.filter = e.target.value; renderTable(); renderActions(); } }),
      h("button.icon-btn", { title: t("refresh"), onclick: refresh }, icon("refresh")));
    actionsEl = h("div.actions");
    renderActions();
    fill(root, tb, actionsEl, banner(), h("div#tablewrap"), dropOverlay);
    renderTable();
  }

  function renderTable() {
    const wrap = root.querySelector("#tablewrap");
    if (!wrap) return;
    const list = visible();
    if (st.loading && !st.entries.length) { fill(wrap, h("div.empty", { text: t("loading") })); return; }
    if (st.error) { wrap.replaceChildren(); return; }
    if (!list.length) { fill(wrap, h("div.empty", null, icon("folder"), h("p", { text: st.filter ? t("noMatches") : t("emptyFolder") }), canCreate() ? h("p.hint", { text: t("dropHint") }) : null)); return; }
    const body = h("tbody");
    const single = () => !!prefs().openOnClick;
    const toggle = (name, on) => { if (on === undefined ? st.selected.has(name) : !on) st.selected.delete(name); else st.selected.add(name); };
    for (const e of list) {
      const isSel = st.selected.has(e.name);
      const isCut = !!clip && clip.mode === "cut" && clip.dir === st.cwd && clip.entries.some((x) => x.name === e.name);
      const tr = h("tr" + (isSel ? ".selected" : "") + (isCut ? ".cut" : "") + (e.type === "dir" ? ".dir" : ""), { draggable: canWrite() ? "true" : undefined, dataset: { name: e.name },
        onclick: (ev) => {
          if (ev.target.closest(".cbcell")) return;
          if (ev.ctrlKey || ev.metaKey) { toggle(e.name); }
          else if (ev.shiftKey && st.last) { const names = list.map((x) => x.name), a = names.indexOf(st.last), b = names.indexOf(e.name); st.selected = new Set(names.slice(Math.min(a, b), Math.max(a, b) + 1)); }
          else if (single()) { open(e); return; } // one tap opens (phones): selecting is what the checkbox is for
          else st.selected = new Set([e.name]);
          st.last = e.name;
          syncSelection();
        },
        ondblclick: (ev) => { if (!single() && !ev.target.closest(".cbcell")) open(e); }, oncontextmenu: (ev) => rowMenu(ev, e),
        ondragstart: (ev) => { if (!st.selected.has(e.name)) st.selected = new Set([e.name]); ev.dataTransfer.setData("application/x-ndev-names", JSON.stringify([...st.selected])); ev.dataTransfer.effectAllowed = "move"; },
        ondragover: (ev) => { if (e.type === "dir" && canWrite()) { ev.preventDefault(); tr.classList.add("droptarget"); } },
        ondragleave: () => tr.classList.remove("droptarget"),
        ondrop: async (ev) => {
          tr.classList.remove("droptarget");
          if (e.type !== "dir" || !canWrite()) return;
          ev.preventDefault(); ev.stopPropagation();
          dropOverlay.classList.remove("show");
          const internal = ev.dataTransfer.getData("application/x-ndev-names");
          if (internal) { await moveInto(JSON.parse(internal).filter((n) => n !== e.name), full(e.name)); return; }
          await startUpload(await itemsFromDrop(ev.dataTransfer, full(e.name)), full(e.name));
        } },
        h("td.cbcell", { onclick: (ev) => ev.stopPropagation() }, h("input.cb", { type: "checkbox", checked: isSel, "aria-label": e.name, onchange: (ev) => { toggle(e.name, ev.target.checked); st.last = e.name; syncSelection(); } })),
        h("td.name", null, icon(e.type === "dir" ? "folder" : U.isImage(e.name) ? "image" : "file", "ico " + e.type), h("span.fname", { text: e.name })),
        h("td.size", { text: e.type === "dir" ? "" : U.formatSize(e.size) }),
        h("td.kind", { text: e.type === "dir" ? t("folder") : (U.ext(e.name) || t("file")).toUpperCase() }));
      body.append(tr);
    }
    const names = list.map((x) => x.name), nsel = names.filter((x) => st.selected.has(x)).length;
    const headCb = h("input.cball", { type: "checkbox", checked: nsel > 0 && nsel === names.length, "aria-label": t("selectAll"), onchange: (ev) => { if (ev.target.checked) names.forEach((n) => st.selected.add(n)); else names.forEach((n) => st.selected.delete(n)); syncSelection(); } });
    headCb.indeterminate = nsel > 0 && nsel < names.length;
    fill(wrap, h("table.files", null, h("thead", null, h("tr", null, h("th.cbcell", null, headCb), sortHeader("name", t("name")), sortHeader("size", t("size")), h("th", { text: t("type") }))), body));
  }

  const dropOverlay = h("div#dropoverlay", null, icon("upload"), h("p", { text: "" }));
  const filePicker = h("input", { type: "file", multiple: true, style: "display:none", onchange: (e) => { const items = [...e.target.files].map((file) => ({ file, dir: st.cwd, mkdirs: [] })); e.target.value = ""; startUpload(items, st.cwd); } });

  function bindGlobal() {
    let depth = 0;
    const hasFiles = (e) => e.dataTransfer && [...(e.dataTransfer.types || [])].includes("Files");
    window.addEventListener("dragenter", (e) => { if (!hasFiles(e) || !root.isConnected || !root.offsetParent) return; depth++; dropOverlay.querySelector("p").textContent = canCreate() ? t("dropTo", st.cwd) : t("notWritableHere"); dropOverlay.classList.toggle("bad", !canCreate()); dropOverlay.classList.add("show"); });
    window.addEventListener("dragleave", (e) => { if (!hasFiles(e)) return; depth = Math.max(0, depth - 1); if (!depth) dropOverlay.classList.remove("show"); });
    window.addEventListener("dragover", (e) => { if (hasFiles(e)) e.preventDefault(); });
    window.addEventListener("drop", async (e) => {
      if (!hasFiles(e)) return;
      e.preventDefault(); depth = 0; dropOverlay.classList.remove("show");
      if (!root.isConnected || !root.offsetParent) return;
      await startUpload(await itemsFromDrop(e.dataTransfer, st.cwd), st.cwd);
    });
    window.addEventListener("keydown", (e) => {
      if (!root.isConnected || !root.offsetParent || document.querySelector(".backdrop") || /INPUT|TEXTAREA/.test(document.activeElement && document.activeElement.tagName)) return;
      const sel = selectedEntries();
      if (e.key === "F2" && sel.length === 1 && canWrite()) { e.preventDefault(); rename(sel[0]); }
      else if (e.key === "Delete" && sel.length && canWrite()) { e.preventDefault(); if (U.trashRootOf(st.cwd)) purge(sel.map((x) => full(x.name)), false, sel[0].name); else remove(sel); }
      else if (e.key === "Enter" && sel.length === 1) { e.preventDefault(); open(sel[0]); }
      else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "c" && sel.length) { e.preventDefault(); setClip("copy"); }
      else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "x" && sel.length && canWrite()) { e.preventDefault(); setClip("cut"); }
      else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "v" && clip && canCreate()) { e.preventDefault(); paste(); }
      else if (e.key === "Escape" && (sel.length || clip)) { st.selected.clear(); clip = null; syncSelection(); }
      else if (e.key === "Backspace" && st.cwd !== "/") { e.preventDefault(); go(U.parent(st.cwd)); }
      else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "a") { e.preventDefault(); st.selected = new Set(visible().map((x) => x.name)); syncSelection(); }
      else if (e.key === "F5") { e.preventDefault(); refresh(); }
    });
    window.addEventListener("popstate", () => { if (st.cwd !== pathFromHash()) load(pathFromHash()); });
  }

  function mount(el) {
    root = el;
    root.classList.add("files-view");
    bindGlobal();
    document.body.append(filePicker);
    render();
  }

  NDP.files = { mount, load, refresh, go, render, state: st, start: () => load(pathFromHash()) };
})();
