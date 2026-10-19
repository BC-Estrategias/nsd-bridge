/* The FAQ tab: how the page works, how to install the local MCP (with copy buttons), limits and troubleshooting.
 * The wording lives in the i18n dictionaries ("faq.<id>.q" / "faq.<id>.a"); "a" is plain text, one paragraph per
 * line, lines starting with "- " are list items. A handful of answers differ enough between consoles (a
 * folder path, a button that only exists on one, a whole troubleshooting bullet) to need a "<key>.<platform>"
 * override -- tp() picks it up automatically and falls back to the plain key otherwise (see i18n.js). */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { h, fill, icon, copyText } = NDP.ui;
  const S = NDP.session;
  const t = (...a) => NDP.i18n.t(...a);
  const tp = (key) => NDP.i18n.tp(key, S.info && S.info.platform);

  const SECTIONS = ["what", "pairing", "keys", "modes", "upload", "install", "edit", "organize", "trash", "mcp", "security", "trouble", "about"];
  const REPO = "https://github.com/BC-Estrategias/nsd-bridge";

  function prose(text) {
    const out = [];
    let list = null;
    for (const line of text.split("\n")) {
      if (/^- /.test(line)) { if (!list) { list = h("ul"); out.push(list); } list.append(h("li", { text: line.slice(2) })); continue; }
      list = null;
      if (line.trim()) out.push(h("p", { text: line }));
    }
    return out;
  }

  function code(text) {
    return h("div.code", null, h("pre", null, h("code", { text })), h("button.icon-btn", { title: t("copy"), onclick: () => copyText(text) }, icon("copy")));
  }

  /** The MCP install steps. `where` is where the project folder lives on the person's computer. */
  function mcpBody() {
    const box = h("div.mcp");
    const where = h("input", { type: "text", value: "~/nintendo-dev-mcp", spellcheck: false, autocomplete: "off", "aria-label": t("faq.mcp.where") });
    const draw = () => {
      const dir = where.value.trim().replace(/\/+$/, "") || "~/nintendo-dev-mcp";
      const q = /\s/.test(dir) ? `"${dir}"` : dir;
      const server = `${q}/bridge/packages/mcp/src/main.ts`;
      fill(box, 
        h("ol.steps", null,
          h("li", null, h("strong", { text: t("faq.mcp.s1") }), code("node --version")),
          h("li", null, h("strong", { text: t("faq.mcp.s2") }), code(`git clone ${REPO}.git ${q}\ncd ${q}/bridge && npm install\nmkdir -p ~/nintendo-dev`)),
          h("li", null, h("strong", { text: t("faq.mcp.s3") }), h("label.field", null, h("span", { text: t("faq.mcp.where") }), where), h("p.muted.small", { text: t("faq.mcp.s3b") })),
          h("li", null, h("strong", { text: t("faq.mcp.s4") }), h("p.muted.small", { text: t("faq.mcp.s4b", location.hostname) }), code(`node ${q}/bridge/packages/cli/src/main.ts pair ${location.hostname}`)),
          h("li", null, h("strong", { text: t("faq.mcp.s5") }),
            h("h4", { text: "Claude Code" }), code(`claude mcp add nintendo --scope user -- node ${server} --local-root ~/nintendo-dev`),
            h("h4", { text: "Codex" }), code(`codex mcp add nintendo -- node ${server} --local-root ~/nintendo-dev`),
            h("p.muted.small", { text: t("faq.mcp.s5b") })),
          h("li", null, h("strong", { text: tp("faq.mcp.s6") }), h("p.muted.small", { text: t("faq.mcp.s6b") }))),
        h("h4", { text: t("faq.mcp.remove") }), code("claude mcp remove nintendo\ncodex mcp remove nintendo"),
        h("p.muted.small", null, t("faq.mcp.more") + " ", h("code", { text: "docs/mcp.md" }), " · ", h("a", { href: REPO, target: "_blank", rel: "noopener noreferrer", text: "GitHub" })));
    };
    where.addEventListener("input", () => { const pos = where.selectionStart; draw(); const w = box.querySelector("input"); if (w) { w.focus(); w.setSelectionRange(pos, pos); } });
    where.addEventListener("change", draw);
    draw();
    return box;
  }

  function render(root) {
    const open = new Set();
    try { for (const id of JSON.parse(sessionStorage.getItem("ndev.faq") || "[]")) open.add(id); } catch (_) { /* fine */ }
    const items = SECTIONS.map((id) => {
      const d = h("details", { id: "faq-" + id, open: open.has(id) || id === "what" },
        h("summary", { text: t(`faq.${id}.q`) }),
        h("div.faq-body", null, prose(tp(`faq.${id}.a`)), id === "mcp" ? mcpBody() : null));
      d.addEventListener("toggle", () => {
        d.open ? open.add(id) : open.delete(id);
        try { sessionStorage.setItem("ndev.faq", JSON.stringify([...open])); } catch (_) { /* fine */ }
      });
      return d;
    });
    fill(root, h("div.faq", null, h("h2", { text: t("faq") }), h("p.muted", { text: t("faq.intro") }), items));
  }

  function mount(root) { render(root); return { rerender: () => render(root) }; }

  NDP.faq = { mount, SECTIONS };
})();
