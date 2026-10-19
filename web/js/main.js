/* Bootstrap: preferences, the page frame (top bar, tabs, banners), the pairing screen, and wiring the session to the
 * three views (Files, Settings, FAQ). */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { h, fill, icon, toast } = NDP.ui;
  const S = NDP.session;
  const t = (...a) => NDP.i18n.t(...a);
  const tp = (key) => NDP.i18n.tp(key, S.info && S.info.platform);

  // ---------------------------------------------------------------------------------------------- preferences
  const PREFS_KEY = "ndev.prefs";
  const coarse = (() => { try { return matchMedia("(pointer: coarse)").matches || innerWidth < 720; } catch (_) { return false; } })();
  const DEFAULTS = { showHidden: false, confirmDelete: true, backupOnSave: true, theme: "auto", openOnClick: coarse, checkUpdates: true };
  const prefs = Object.assign({}, DEFAULTS);
  try { const saved = JSON.parse(localStorage.getItem(PREFS_KEY) || "{}"); for (const k of Object.keys(DEFAULTS)) if (typeof saved[k] === typeof DEFAULTS[k]) prefs[k] = saved[k]; } catch (_) { /* defaults */ }
  Object.defineProperty(prefs, "set", {
    value(k, v) {
      if (!(k in DEFAULTS)) return;
      prefs[k] = v;
      try { localStorage.setItem(PREFS_KEY, JSON.stringify(prefs)); } catch (_) { /* not persisted */ }
      if (k === "showHidden" && NDP.files) NDP.files.render();
    },
  });
  NDP.prefs = prefs;

  function applyTheme() {
    if (prefs.theme === "auto") document.documentElement.removeAttribute("data-theme");
    else document.documentElement.setAttribute("data-theme", prefs.theme);
  }

  // ---------------------------------------------------------------------------------------------- frame
  const TABS = ["files", "settings", "faq"];
  let tab = "files", app, views = {}, tabButtons = {}, statusPill, banner, tabsNav, retryTimer = null, everReady = false, langBtn;
  try { const saved = sessionStorage.getItem("ndev.tab"); if (TABS.includes(saved)) tab = saved; } catch (_) { /* first tab */ }

  function show(name) {
    if (!TABS.includes(name)) return;
    tab = name;
    try { sessionStorage.setItem("ndev.tab", name); } catch (_) { /* fine */ }
    layout();
    if (name === "settings") NDP.settings.render();
  }

  function guessLabel() {
    const ua = navigator.userAgent || "";
    const b = /Edg\//.test(ua) ? "Edge" : /Firefox\//.test(ua) ? "Firefox" : /Chrome\//.test(ua) ? "Chrome" : /Safari\//.test(ua) ? "Safari" : "Browser";
    const os = /Windows/.test(ua) ? "Win" : /Mac OS/.test(ua) ? "Mac" : /Android/.test(ua) ? "Android" : /iPhone|iPad/.test(ua) ? "iOS" : /Linux/.test(ua) ? "Linux" : "";
    return `${b}${os ? " " + os : ""}`.slice(0, 15);
  }

  // ---------------------------------------------------------------------------------------------- pairing view
  const pairUi = { phase: "idle", number: "", error: "", label: guessLabel(), remember: true, ctl: null };

  function pairErrorText(e) {
    if (e && e.refused === "denied") return t("pairDenied");
    if (e && e.refused === "full") return t("pairFull");
    if (e && e.statusName === "UNAUTHORIZED") return t("pairWindowClosed");
    if (e && e.statusName === "BUSY") return t("pairBusy");
    if (e && /timed out/.test(e.message || "")) return t("pairTimeout");
    return (e && e.statusName && NDP.i18n.has("err." + e.statusName) ? t("err." + e.statusName) : (e && e.message) || String(e)) + " " + t("pairFailedHint");
  }

  async function startPairing() {
    pairUi.phase = "waiting";
    pairUi.number = "";
    pairUi.error = "";
    pairUi.ctl = new AbortController();
    layout();
    try {
      await S.pairByNumber(pairUi.label.trim() || guessLabel(), pairUi.remember, (n) => { pairUi.number = n; layout(); }, pairUi.ctl.signal);
      pairUi.phase = "idle";
    } catch (e) {
      const aborted = pairUi.ctl.signal.aborted;
      pairUi.phase = "idle";
      pairUi.error = aborted ? "" : pairErrorText(e);
      if (S.client && (S.client.isClosed || aborted)) S.connect(); // a fresh connection: the console forgets the request of a closed one
      layout();
    }
  }

  function legacyCodeForm() {
    const code = h("input", { type: "text", inputMode: "text", autocomplete: "off", autocapitalize: "characters", spellcheck: false, placeholder: "XXXX-XXXX-XXXX-XXXX", maxLength: 24, "aria-label": t("pairCode") });
    const err = h("div.field-error", { role: "alert" });
    const go = async () => {
      err.textContent = "";
      try { await S.pair(code.value, pairUi.label.trim() || guessLabel(), pairUi.remember); }
      catch (ex) {
        err.textContent = ex.statusName === "BAD_REQUEST" && ex.detail === "code" ? t("pairBadCode") : pairErrorText(ex);
        if (S.client && S.client.isClosed) S.connect();
      }
    };
    code.addEventListener("input", () => { // format as XXXX-XXXX-XXXX-XXXX (paste friendly) and go as soon as it is complete
      const raw = code.value.replace(/[^0-9a-z]/gi, "").toUpperCase().slice(0, 16);
      code.value = raw.replace(/(.{4})(?=.)/g, "$1-");
      if (raw.length === 16) setTimeout(go, 0);
    });
    return h("details.legacy", null, h("summary", { text: t("pairUseCode") }), h("p.muted.small", { text: t("pairUseCodeHint") }), h("label.field", null, h("span", { text: t("pairCode") }), code), err);
  }

  function pairingView() {
    const rejected = S.error === "rejected";
    const waiting = pairUi.phase === "waiting";
    const label = h("input", { type: "text", value: pairUi.label, maxLength: 15, autocomplete: "off", spellcheck: false, "aria-label": t("pairLabel"), oninput: (e) => { pairUi.label = e.target.value; } });
    const remember = h("input", { type: "checkbox", checked: pairUi.remember, onchange: (e) => { pairUi.remember = e.target.checked; } });
    const card = h("div.pair-card", null,
      h("h2", { text: t("pairTitle") }),
      rejected ? h("div.banner.warn", null, icon("warn"), h("div", null, h("p", { text: t("pairRejected") }), S.hasStoredKey() ? h("button", { type: "button", onclick: () => S.connect(), text: t("retryStoredKey") }) : null)) : null,
      S.info && !S.info.pairingOpen && !waiting ? h("div.banner.info", null, icon("help"), h("p", { text: t("pairWindowClosed") })) : null,
      waiting ? h("div.pair-wait", null,
        pairUi.number ? h("div.sas", { "aria-live": "polite", text: pairUi.number }) : h("div.spinner"),
        h("p.pair-compare", { text: pairUi.number ? tp("pairCompare") : t("pairWaiting") }),
        h("div.actions", null, h("button", { type: "button", onclick: () => { pairUi.ctl.abort(); }, text: t("cancel") })))
      : h("div", null,
        h("ol.steps", null, h("li", { text: tp("pairStep1") }), h("li", { text: t("pairStep2") }), h("li", { text: t("pairStep3") })),
        pairUi.error ? h("div.field-error", { role: "alert", text: pairUi.error }) : null,
        h("div.actions", null, h("button.primary", { type: "button", onclick: startPairing, text: t("pair") })),
        h("details", null, h("summary", { text: t("pairOptions") }),
          h("label.field", null, h("span", { text: t("pairLabel") }), label, h("span.muted.small", { text: t("pairLabelHint") })),
          h("label.check", null, remember, h("span", { text: t("pairRemember") }))),
        legacyCodeForm()),
      h("p.muted.small", { text: t("pairWhy") }));
    return card;
  }

  function stateView() {
    if (S.state === "pairing") return pairingView();
    if (S.state === "replaced") {
      return h("div.center", null, icon("warn"), h("h2", { text: t("replacedTitle") }), h("p", { text: t("replacedBody") }),
        h("div.actions", null, h("button.primary", { onclick: () => S.connect(), text: t("useHere") })));
    }
    if (S.state === "connecting") return h("div.center", null, h("div.spinner"), h("p", { text: t("connecting") }));
    if (S.state === "offline" && !everReady) {
      return h("div.center", null, icon("warn"), h("h2", { text: t("cannotReach") }), h("p", { text: tp("cannotReachBody") }),
        h("div.actions", null, h("button.primary", { onclick: () => S.connect(), text: t("retryNow") })));
    }
    return null;
  }

  // ---------------------------------------------------------------------------------------------- layout
  function copyText(text) {
    // plain http has no navigator.clipboard: fall back to a hidden textarea
    try {
      if (navigator.clipboard && globalThis.isSecureContext) { navigator.clipboard.writeText(text); return true; }
      const ta = document.createElement("textarea");
      ta.value = text;
      ta.style.position = "fixed";
      ta.style.opacity = "0";
      document.body.append(ta);
      ta.select();
      const ok = document.execCommand("copy");
      ta.remove();
      return ok;
    } catch (_) { return false; }
  }

  /** "A new version exists": the DSi is updated from a computer (`ndev update`), the 3DS from its own menu. */
  function updateBanner(version) {
    const dsi = S.info.platform === "dsi";
    const cmd = `ndev update ${location.hostname}`;
    const dismiss = h("button", { type: "button", onclick: () => { NDP.update.dismiss(version); layout(); }, text: t("upd.dismiss") });
    if (!dsi) return h("div.banner.info", null, icon("help"), h("p.grow", { text: t("upd.3ds", version, "v" + S.info.agentVersion) }), dismiss);
    return h("div.banner.info", null, icon("help"),
      h("div.grow", null, h("p", { text: t("upd.dsi", version, "v" + S.info.agentVersion) }), h("p", null, h("code", { text: cmd }))),
      h("button", { type: "button", onclick: () => { if (copyText(cmd)) toast(t("upd.copied"), "ok", 2500); }, text: t("upd.copy") }), dismiss);
  }

  function layout() {
    if (!app) return;
    const ready = S.state === "ready";
    const blocking = stateView();
    if (S.state === "ready") everReady = true;

    statusPill.className = "pill " + (ready ? "ok" : S.state === "offline" || S.state === "replaced" ? "bad" : "warn");
    statusPill.textContent = t("state." + S.state);
    for (const n of TABS) { tabButtons[n].classList.toggle("active", n === tab); tabButtons[n].setAttribute("aria-current", n === tab ? "page" : "false"); }
    tabsNav.hidden = !!blocking;

    banner.replaceChildren();
    if (ready && prefs.checkUpdates && S.info) {
      const v = NDP.update.newerThan(S.info.agentVersion);
      if (v) banner.append(updateBanner(v));
    }
    if (S.state === "offline" && everReady) {
      banner.append(h("div.banner.error", null, icon("warn"), h("p.grow", { text: t("connectionLost") }), h("button", { onclick: () => S.connect(), text: t("retryNow") })));
    }

    let gate = views.gate;
    if (blocking) { fill(gate, blocking); }
    gate.hidden = !blocking;
    for (const n of TABS) views[n].hidden = !!blocking || n !== tab;
    document.title = ready ? `${t("appName")} · ${location.hostname}` : t("appName");
  }

  function buildFrame() {
    statusPill = h("button.pill.warn", { onclick: () => show("settings"), title: t("connection") });
    tabButtons = {};
    tabsNav = h("nav.tabs", { "aria-label": "sections" }, TABS.map((n) => (tabButtons[n] = h("button.tab", { onclick: () => show(n) }, icon(n === "files" ? "folder" : n === "settings" ? "gear" : "help"), h("span", { text: t(n) })))));
    langBtn = h("button.icon-btn.lang", { title: t("language"), onclick: () => { NDP.i18n.setLang(NDP.i18n.lang() === "pt-BR" ? "en" : "pt-BR"); rerender(); }, text: NDP.i18n.lang() === "pt-BR" ? "EN" : "PT" });
    banner = h("div#banner");
    views = { gate: h("section#view-gate"), files: h("section#view-files"), settings: h("section#view-settings"), faq: h("section#view-faq") };
    fill(app,
      h("header.topbar", null, h("div.brand", null, icon("console"), h("span", { text: t("appName") })), tabsNav, h("span.grow"), statusPill, langBtn),
      banner, h("main", null, views.gate, views.files, views.settings, views.faq),
      h("footer.footer", null, h("p", null, "Desenvolvido por ", h("strong", { text: "BC Labs" }), " · ", h("a", { href: "https://github.com/BC-Estrategias", target: "_blank", text: "GitHub" }), " · ", h("a", { href: "https://instagram.com/bcestrategias", target: "_blank", text: "Instagram" }))));
    NDP.files.mount(views.files);
    NDP.settings.mount(views.settings);
    NDP.faq.mount(views.faq);
  }

  function rerender() {
    document.documentElement.lang = NDP.i18n.lang();
    buildFrame();
    NDP.files.render();
    layout();
  }

  function boot() {
    app = document.getElementById("app");
    applyTheme();
    document.documentElement.lang = NDP.i18n.lang();
    buildFrame();
    layout();
    let filesStarted = false;
    S.on("state", (state) => {
      if (state === "ready" && prefs.checkUpdates) NDP.update.fetchLatest().then(layout);
      layout();
      if (state === "ready") {
        if (!filesStarted) { filesStarted = true; NDP.files.start(); } else NDP.files.refresh();
      }
    });
    S.on("retry", (ms) => {
      if (retryTimer) clearInterval(retryTimer);
      let left = Math.ceil(ms / 1000);
      const label = () => { const p = banner.querySelector(".banner p"); if (p && S.state === "offline") p.textContent = `${t("connectionLost")} ${t("retryingIn", left)}`; };
      label();
      retryTimer = setInterval(() => { left--; if (left <= 0 || S.state !== "offline") { clearInterval(retryTimer); return; } label(); }, 1000);
    });
    S.on("context", () => { layout(); NDP.files.render(); });
    window.addEventListener("error", (e) => toast(String(e.message || e), "error", 8000));
    window.addEventListener("unhandledrejection", (e) => { console.error(e.reason); });
    S.connect();
  }

  NDP.main = { show, rerender, applyTheme, boot };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot); else boot();
})();
