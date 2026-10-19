/* The Settings tab: what this page is connected to, the console's storage and memory, the folders the console opened
 * (shown only: they can be changed on the console, never from here), pairing, and this browser's preferences. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { h, fill, icon, confirmBox, toast } = NDP.ui;
  const U = NDP.util, S = NDP.session, C = NDP.codec, A = NDP.auth;
  const t = (...a) => NDP.i18n.t(...a);
  let root;
  let pairedKeys = null, pairedKeysError = null, pairedKeysLoading = false;

  function errText(e) {
    if (!e) return "";
    if (e.statusName) return NDP.i18n.has("err." + e.statusName) ? t("err." + e.statusName) : e.statusName;
    return e.message || String(e);
  }

  /** The key_id of this browser's own stored pairing for the current console, as a hex string (or null). */
  function myKeyIdHex() {
    const mine = S.deviceHex && S.store.read()[S.deviceHex];
    if (!mine) return null;
    try { return C.hex(A.keyIdOf(C.fromHex(mine.psk))); } catch (_) { return null; }
  }

  async function loadPairedKeys() {
    if (S.state !== "ready" || pairedKeysLoading) return;
    pairedKeysLoading = true;
    try {
      const list = await S.run((c) => c.pairList(), { retry: true });
      pairedKeys = list.map((k) => ({ hex: C.hex(k.keyId), label: k.label }));
      pairedKeysError = null;
    } catch (e) {
      pairedKeys = null;
      pairedKeysError = errText(e);
    } finally {
      pairedKeysLoading = false;
      render();
    }
  }

  const LEVEL = { write: "levelWrite", read: "levelRead" };

  function bar(label, pair) {
    if (!pair || !pair.total) return h("div.meter", null, h("div.meter-top", null, h("span", { text: label }), h("span.muted", { text: "—" })));
    const used = Math.max(0, pair.total - pair.free), pct = Math.min(100, Math.round((100 * used) / pair.total));
    return h("div.meter", null,
      h("div.meter-top", null, h("span", { text: label }), h("span.muted", { text: t("usedOf", U.formatSize(used), U.formatSize(pair.total)) })),
      h("div.meter-bar", { role: "progressbar", "aria-valuenow": String(pct), "aria-valuemin": "0", "aria-valuemax": "100", "aria-label": label }, h("div.meter-fill" + (pct > 90 ? ".hot" : ""), { style: `width:${pct}%` })),
      h("div.muted.small", { text: t("freeN", U.formatSize(pair.free)) }));
  }

  const row = (k, v) => h("div.kv", null, h("dt", { text: k }), h("dd", null, v === undefined || v === null || v === "" ? "—" : v));

  function card(title, ...kids) { return h("section.card", null, h("h3", { text: title }), ...kids); }

  function pref(label, hint, control) {
    return h("label.pref", null, h("span.pref-text", null, h("strong", { text: label }), hint ? h("span.muted.small", { text: hint }) : null), control);
  }

  function toggle(key, onchange) {
    return h("input.switch", { type: "checkbox", checked: NDP.prefs[key], onchange: (e) => { NDP.prefs.set(key, e.target.checked); if (onchange) onchange(); } });
  }

  function rootsList(title, list, kind) {
    return h("div.roots", null, h("h4", { text: title }),
      list.length ? h("ul", null, list.map((p) => h("li", null, h("code", { text: p }), h("button.link", { onclick: () => { NDP.main.show("files"); NDP.files.go(p); }, text: t("open") })))) : h("p.muted", { text: t(kind === "write" ? "noWriteFolders" : "noReadFolders") }));
  }

  function pairedDevicesList() {
    if (pairedKeysError) return h("p.muted", { text: pairedKeysError });
    if (!pairedKeys) return h("p.muted", { text: t("loading") });
    const mine = myKeyIdHex();
    return pairedKeys.length ? h("ul", null, pairedKeys.map(({ hex, label }) => {
      const isCurrent = hex === mine;
      return h("li.device-item", null,
        h("div.device-info", null,
          h("span.device-label", { text: label || "(unnamed)" }),
          isCurrent ? h("span.pill.ok", { text: t("currentDevice") }) : null),
        h("button.link.danger", {
          onclick: async () => {
            if (await confirmBox(t("forgetDevice"), t("forgetDeviceBody", label || "(unnamed)"), t("forget"), true)) {
              try {
                await S.run((c) => c.forgetPairing(C.fromHex(hex)));
                if (isCurrent) S.store.forget(S.deviceHex);
                await loadPairedKeys();
              } catch (e) { toast(errText(e), "error", 6000); }
            }
          },
          text: t("remove")
        }));
    })) : h("p.muted", { text: t("noPairedDevices") });
  }

  function render() {
    if (!root) return;
    const info = S.info, dev = S.device, acc = S.access;
    const ready = S.state === "ready" && info;
    const mode = info && info.mode;

    const connection = card(t("connection"),
      h("dl", null,
        row(t("address"), h("code", { text: location.host })),
        row(t("status"), h("span.pill." + (ready ? "ok" : "bad"), { text: t("state." + S.state) })),
        row(t("agentVersion"), info && info.agentVersion),
        row(t("platform"), info && info.platform),
        row(t("protocol"), info && "NDP v" + info.protocol),
        row(t("mode"), mode ? h("span.pill." + (mode === "READ_ONLY" ? "warn" : "ok"), { text: t("mode." + mode) }) : null),
        row(t("latency"), S.lastPingMs != null ? `${S.lastPingMs.toFixed(0)} ms` : null),
        row(t("security"), info && (info.auth === "required" ? t("authRequired") : t("authOff")))),
      h("div.actions", null, h("button", { onclick: async () => { await S.refreshContext(); render(); }, disabled: !ready }, icon("refresh"), t("refresh"))),
      mode === "READ_ONLY" ? h("p.hint", { text: t("readOnlyHow") }) : null);

    const consoleCard = card(t("console"),
      dev ? h("dl", null, row(t("model"), dev.model), row(t("firmware"), dev.firmware), row(t("ram"), dev.ramTotal ? U.formatSize(dev.ramTotal) : null)) : h("p.muted", { text: ready ? t("noDeviceInfo") : t("notConnected") }),
      dev ? bar(t("sdCard"), dev.sd) : null,
      dev ? bar(t("systemMemory"), dev.systemMemory) : null,
      dev && dev.appMemory && dev.appMemory.total ? bar(t("appMemory"), dev.appMemory) : null);

    const folders = card(t("folders"),
      h("p.muted", { text: t("foldersNote") }),
      acc ? h("div.roots-grid", null, rootsList(t("levelWrite"), acc.writeRoots, "write"), rootsList(t("levelRead"), acc.readRoots.filter((r) => !acc.writeRoots.includes(r)), "read")) : h("p.muted", { text: t("notConnected") }),
      h("p.muted.small", { text: t("protectedNote", U.protectedZonesFor(info && info.platform).neverWrite.join(", ")) }));

    const paired = S.deviceHex && S.store.read()[S.deviceHex];
    const pairing = card(t("pairing"),
      h("p", { text: paired ? t("pairedThis", paired.label || "") : t("notPairedThis") }),
      paired ? h("p.muted.small", { text: t("forgetNote") }) : null,
      h("div.actions", null,
        paired ? h("button.danger", { onclick: async () => { if (await confirmBox(t("forgetThis"), t("forgetThisBody"), t("forgetThis"), true)) { await S.forgetThisBrowser(); NDP.main.show("files"); } } }, icon("trash"), t("forgetThis")) : null));

    const pairedDevicesCard = ready && info.auth === "required"
      ? card(t("pairedDevices"), h("p.muted.small", { text: t("allPairedDevicesNote") }), pairedDevicesList())
      : null;
    if (ready && info.auth === "required" && pairedKeys === null && !pairedKeysError && !pairedKeysLoading) loadPairedKeys();

    const prefs = card(t("preferences"),
      pref(t("language"), null, h("select", { onchange: (e) => { NDP.i18n.setLang(e.target.value); NDP.main.rerender(); } }, [["auto", t("langAuto")], ["pt-BR", "Português (Brasil)"], ["en", "English"]].map(([v, l]) => h("option", { value: v, text: l, selected: NDP.i18n.choice() === v })))),
      pref(t("theme"), null, h("select", { onchange: (e) => { NDP.prefs.set("theme", e.target.value); NDP.main.applyTheme(); } }, [["auto", t("themeAuto")], ["light", t("themeLight")], ["dark", t("themeDark")]].map(([v, l]) => h("option", { value: v, text: l, selected: NDP.prefs.theme === v })))),
      pref(t("openOnClick"), t("openOnClickHint"), toggle("openOnClick")),
      pref(t("showHidden"), t("showHiddenHint"), toggle("showHidden")),
      pref(t("confirmDelete"), t("confirmDeleteHint"), toggle("confirmDelete")),
      pref(t("backupOnSave"), t("backupOnSaveHint"), toggle("backupOnSave")),
      pref(t("upd.pref"), t("upd.prefHint"), toggle("checkUpdates", () => { if (NDP.prefs.checkUpdates) NDP.update.fetchLatest().then(() => NDP.main.rerender()); else NDP.main.rerender(); })));

    fill(root, h("div.cards", null, connection, consoleCard, folders, pairing, pairedDevicesCard, prefs));
  }

  function mount(el) {
    root = el;
    S.on("state", (state) => {
      if (state !== "ready") { pairedKeys = null; pairedKeysError = null; } // a new connection may be a different console
      if (root.offsetParent) render();
    });
    S.on("context", () => { if (root.offsetParent) render(); });
    S.on("ping", () => { if (root.offsetParent) render(); });
    render();
  }

  NDP.settings = { mount, render };
})();
