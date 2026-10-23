/* "Sources" link (3DS only): sets the share the console's Sources screen browses. The console only takes it while that
 * screen is open, and applies it at once (nothing is downloaded or run by this alone, so unlike Install CIA there is no
 * on-console confirmation step — see ARCHITECTURE.md / docs/protocol/ndp-v1.md on SOURCE_LINK). */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { h, dialog, confirmBox, toast } = NDP.ui;
  const S = NDP.session;
  const t = (...a) => NDP.i18n.t(...a);

  function validUrl(v) {
    return /^https?:\/\/[\x21-\x7e]+$/.test(v) && v.length <= 1023;
  }

  function errText(e) {
    if (e && e.statusName && NDP.i18n.has("err." + e.statusName)) return t("err." + e.statusName);
    return (e && (e.detail || e.message)) || String(e);
  }

  async function send(url) {
    for (;;) {
      try {
        await S.run((c) => c.sourceLink(url));
        toast(t("src.sent"), "ok", 8000);
        return true;
      } catch (e) {
        if (e && e.statusName === "BUSY") {
          if (await confirmBox(t("src.busyTitle"), t("src.busyBody"), t("src.retry"))) continue;
          return false;
        }
        toast(t("src.failed") + ": " + errText(e), "error", 9000);
        return false;
      }
    }
  }

  async function open() {
    const url = h("input", { type: "text", placeholder: "https://…/s/xxxxxxxx", spellcheck: false, autocapitalize: "off", autocomplete: "off" });
    const err = h("div.field-error", { role: "alert" });
    url.addEventListener("input", () => { err.textContent = ""; });
    const body = h("div", null,
      h("p", { text: t("src.intro") }),
      h("label.field", null, h("span", { text: t("src.url") }), url),
      err);
    for (;;) {
      const ok = await dialog({ title: t("src.title"), body, buttons: [{ label: t("cancel"), value: false }, { label: t("src.send"), value: true, primary: true }] });
      if (!ok) return;
      const u = url.value.trim();
      if (!validUrl(u)) { err.textContent = t("src.badUrl"); continue; }
      await send(u);
      return;
    }
  }

  NDP.sources = { open, validUrl };
})();
