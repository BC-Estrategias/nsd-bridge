// Which release of NSD Bridge is newer and which file of it to send to a console (used by `ndev update`, for the DSi:
// its Wi-Fi has no HTTPS, so a computer downloads the new build and pushes it over the bridge). The release JSON comes
// from the GitHub API and is untrusted: the asset must have the exact name and URL of this project's release, a SHA-256
// digest and a sane size, and the downloaded bytes are checked against both.
import { createHash } from "node:crypto";

export const UPDATE_REPOSITORY = "BC-Estrategias/nsd-bridge";
export const UPDATE_MAX_BYTES = 8 * 1024 * 1024;
const MIN_BYTES = 4096;

export function parseVersion(v: string): [number, number, number] | null {
  const m = /^v?(\d{1,4})\.(\d{1,4})\.(\d{1,4})$/.exec(v);
  return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
}

/** True when `candidate` is a newer x.y.z than `installed` (an unparsable version is never newer). */
export function isNewerVersion(candidate: string, installed: string): boolean {
  const a = parseVersion(candidate), b = parseVersion(installed);
  if (!a || !b) return false;
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return (a[i] as number) > (b[i] as number);
  return false;
}

export interface ReleaseAsset {
  version: string; // the tag, e.g. "v1.7.2"
  name: string;
  url: string;
  sha256: string; // lower-case hex
  size: number;
}

/** The DSi build of a "releases/latest" response, or null when there is none / the response is not trustworthy. */
export function pickDsiAsset(release: unknown): ReleaseAsset | null {
  const r = release as Record<string, unknown> | null;
  if (!r || typeof r !== "object" || r.draft !== false || r.prerelease !== false) return null;
  const tag = r.tag_name;
  if (typeof tag !== "string" || !parseVersion(tag)) return null;
  const ver = tag.startsWith("v") ? tag : `v${tag}`;
  const name = `nsd-bridge-dsi-${ver}.nds`;
  const url = `https://github.com/${UPDATE_REPOSITORY}/releases/download/${tag}/${name}`;
  if (!Array.isArray(r.assets)) return null;
  const hits = (r.assets as Array<Record<string, unknown>>).filter((a) => a && typeof a === "object" && a.name === name);
  if (hits.length !== 1) return null;
  const a = hits[0] as Record<string, unknown>;
  const digest = a.digest;
  if (a.browser_download_url !== url || typeof digest !== "string" || !/^sha256:[0-9a-fA-F]{64}$/.test(digest)) return null;
  const size = a.size;
  if (typeof size !== "number" || !Number.isInteger(size) || size < MIN_BYTES || size > UPDATE_MAX_BYTES) return null;
  return { version: tag, name, url, sha256: digest.slice(7).toLowerCase(), size };
}

type Fetch = (url: string, init?: Record<string, unknown>) => Promise<{ ok: boolean; status: number; json(): Promise<unknown>; arrayBuffer(): Promise<ArrayBuffer> }>;

export async function fetchLatestRelease(fetchFn: Fetch = fetch as unknown as Fetch): Promise<unknown> {
  const res = await fetchFn(`https://api.github.com/repos/${UPDATE_REPOSITORY}/releases/latest`, {
    headers: { Accept: "application/vnd.github+json", "User-Agent": "ndev" },
  });
  if (!res.ok) throw new Error(`GitHub answered ${res.status} when asking for the latest release`);
  return res.json();
}

/** Downloads the asset and checks its size and SHA-256 against what the release declared. */
export async function downloadAsset(asset: ReleaseAsset, fetchFn: Fetch = fetch as unknown as Fetch): Promise<Uint8Array> {
  const prefix = `https://github.com/${UPDATE_REPOSITORY}/releases/download/`;
  if (!asset.url.startsWith(prefix)) throw new Error("refusing to download from outside this project's releases");
  const res = await fetchFn(asset.url, { headers: { "User-Agent": "ndev" } });
  if (!res.ok) throw new Error(`download failed: HTTP ${res.status}`);
  const bytes = new Uint8Array(await res.arrayBuffer());
  if (bytes.length !== asset.size) throw new Error(`download is ${bytes.length} bytes, the release says ${asset.size}`);
  const got = createHash("sha256").update(bytes).digest("hex");
  if (got !== asset.sha256) throw new Error("the downloaded file does not match the SHA-256 GitHub reports: not sending it");
  return bytes;
}
