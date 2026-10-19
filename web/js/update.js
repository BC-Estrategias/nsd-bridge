/* Asks GitHub (from this browser, not from the console) whether a newer NSD Bridge release exists. The answer is cached for
 * a few hours (GitHub allows 60 anonymous requests an hour per address). The page never downloads the new build itself:
 * GitHub's file downloads send no CORS headers, so a browser cannot read them. The DSi gets it from `ndev update`; the 3DS
 * updates itself from its own Updates menu. Turned off by the "check for new versions" preference. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const U = NDP.util;
  const API = "https://api.github.com/repos/BC-Estrategias/nsd-bridge/releases/latest";
  const CACHE_KEY = "ndev.latestRelease", DISMISS_KEY = "ndev.dismissedUpdate", TTL_MS = 6 * 3600 * 1000;
  let latest = null; // { version, at }

  function readCache() {
    try {
      const c = JSON.parse(localStorage.getItem(CACHE_KEY) || "null");
      if (c && typeof c.version === "string" && U.parseVersion(c.version) && Date.now() - c.at < TTL_MS) return c;
    } catch (_) { /* no cache */ }
    return null;
  }

  /** Resolves to the newest release tag (e.g. "v1.7.2") or null when it could not be determined. Never throws. */
  async function fetchLatest() {
    const cached = readCache();
    if (cached) return (latest = cached).version;
    try {
      const ctl = new AbortController();
      const timer = setTimeout(() => ctl.abort(), 8000);
      const res = await fetch(API, { headers: { Accept: "application/vnd.github+json" }, signal: ctl.signal });
      clearTimeout(timer);
      if (!res.ok) return null;
      const j = await res.json();
      if (!j || j.draft !== false || j.prerelease !== false || typeof j.tag_name !== "string" || !U.parseVersion(j.tag_name)) return null;
      latest = { version: j.tag_name, at: Date.now() };
      try { localStorage.setItem(CACHE_KEY, JSON.stringify(latest)); } catch (_) { /* not cached */ }
      return latest.version;
    } catch (_) { return null; }
  }

  const dismissed = () => { try { return localStorage.getItem(DISMISS_KEY) || ""; } catch (_) { return ""; } };
  function dismiss(version) { try { localStorage.setItem(DISMISS_KEY, version); } catch (_) { /* fine */ } }

  /** The newer version for a console that runs `installed`, or "" (also "" when the person dismissed that version). */
  function newerThan(installed) {
    return latest && U.versionNewer(latest.version, installed) && dismissed() !== latest.version ? latest.version : "";
  }

  NDP.update = { fetchLatest, newerThan, dismiss };
})();
