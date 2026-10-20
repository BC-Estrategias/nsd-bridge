/* "Install CIA" (3DS only): sends the console something to install. Three sources: a .cia already on the console's card
 * (selected in the file list), a .cia from this computer (uploaded first, with its SHA-256 checked by the console, then
 * installed from the card), or a URL. The console only takes the request while its Installer > "Receive from the
 * browser" screen is open, and its owner confirms there before anything is installed. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { h, dialog, confirmBox, toast } = NDP.ui;
  const U = NDP.util, S = NDP.session;
  const t = (...a) => NDP.i18n.t(...a);
  const DIR = "/3ds/nintendo-dev-agent";

  /** An http(s) address the console can fetch: printable ASCII, no spaces. */
  function validUrl(v) {
    return /^https?:\/\/[\x21-\x7e]+$/.test(v) && v.length <= 1023;
  }
  const isCia = (name) => /\.cia$/i.test(name) && name.length > 4;

  /** The optional seed field: "" -> null (nothing to send), 32 hex digits (spaces and dashes allowed) -> 16 bytes, else false. */
  function parseSeed(v) {
    const t = String(v || "").replace(/[\s-]/g, "");
    if (!t) return null;
    if (!/^[0-9a-fA-F]{32}$/.test(t)) return false;
    const out = new Uint8Array(16);
    for (let i = 0; i < 16; i++) out[i] = parseInt(t.slice(2 * i, 2 * i + 2), 16);
    return out;
  }

  function errText(e) {
    if (e && e.statusName && NDP.i18n.has("err." + e.statusName)) return t("err." + e.statusName);
    return (e && (e.detail || e.message)) || String(e);
  }

  /** Sends the request; while the console's receive screen is closed it explains and offers to try again. */
  async function send(req) {
    for (;;) {
      try {
        await S.run((c) => (req.path ? c.installPath(req.path, req.sha256, req.seed) : c.installUrl(req.url, req.sha256, req.seed)));
        toast(t("inst.sent"), "ok", 10000);
        return true;
      } catch (e) {
        if (e && e.statusName === "BUSY") {
          if (await confirmBox(t("inst.busyTitle"), t("inst.busyBody"), t("inst.retry"))) continue;
          return false;
        }
        toast(t("inst.failed") + ": " + errText(e), "error", 9000);
        return false;
      }
    }
  }

  async function uploadAndSend(file, seed) {
    if (S.info && S.info.mode === "READ_ONLY") { toast(t("readOnlyBanner"), "error", 7000); return; }
    if (U.accessOf(DIR + "/x", S.access, S.info && S.info.mode, S.info && S.info.platform) !== "write") { toast(t("notWritableHere"), "error", 7000); return; }
    if (file.size >= 4 * 1024 * 1024 * 1024) { toast(t("inst.tooBig"), "error", 7000); return; }
    const dest = U.join(DIR, file.name);
    const kill = toast(t("inst.hashing", file.name), "info", 0);
    let res;
    try {
      res = await S.run((c) => c.write(dest, file, {
        overwrite: true, backup: false,
        onHashProgress: (n, total) => kill.set(t("inst.hashingPct", file.name, total ? Math.round((100 * n) / total) : 0)),
        onProgress: (n) => kill.set(t("inst.uploadingPct", file.name, file.size ? Math.round((100 * n) / file.size) : 0)),
      }));
    } catch (e) {
      kill();
      toast(t("saveFailed") + ": " + errText(e), "error", 9000);
      if (S.client && S.client.isClosed) S.connect();
      return;
    }
    kill();
    if (NDP.files) NDP.files.refresh();
    await send({ path: dest, sha256: res.sha256, seed });
  }

  /** `selected`: the full path of one .cia selected in the file list, or null. */
  async function open(selected) {
    const file = h("input", { type: "file", accept: ".cia" });
    const url = h("input", { type: "text", placeholder: "https://…/game.cia", spellcheck: false, autocapitalize: "off", autocomplete: "off" });
    const seedIn = h("input", { type: "text", placeholder: t("inst.seedPlaceholder"), spellcheck: false, autocapitalize: "off", autocomplete: "off" });
    const err = h("div.field-error", { role: "alert" });
    for (const el of [file, url, seedIn]) el.addEventListener("input", () => { err.textContent = ""; });
    const body = h("div", null,
      h("p", { text: t("inst.intro") }),
      selected
        ? h("p", null, h("strong", { text: t("inst.selected") }), " ", h("code", { text: selected }))
        : h("div", null, h("label.field", null, h("span", { text: t("inst.pickFile") }), file), h("label.field", null, h("span", { text: t("inst.orUrl") }), url)),
      h("label.field", null, h("span", { text: t("inst.seed") }), seedIn, h("span.muted.small", { text: t("inst.seedHint") })),
      err);
    for (;;) {
      const ok = await dialog({ title: t("inst.title"), body, buttons: [{ label: t("cancel"), value: false }, { label: t("inst.send"), value: true, primary: true }] });
      if (!ok) return;
      const seed = parseSeed(seedIn.value);
      if (seed === false) { err.textContent = t("inst.badSeed"); continue; }
      if (selected) { await send({ path: selected, seed: seed || undefined }); return; }
      const f = file.files && file.files[0], u = url.value.trim();
      if (f) {
        if (!isCia(f.name)) { err.textContent = t("inst.notCia"); continue; }
        await uploadAndSend(f, seed || undefined);
        return;
      }
      if (u) {
        if (!validUrl(u)) { err.textContent = t("inst.badUrl"); continue; }
        await send({ url: u, seed: seed || undefined });
        return;
      }
      err.textContent = t("inst.nothing");
    }
  }

  NDP.install = { open, validUrl, isCia, parseSeed };
})();
