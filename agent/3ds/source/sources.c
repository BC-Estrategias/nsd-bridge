#ifndef AGENT_TEXT_UI
#include "sources.h"

/* Copies into fixed buffers are bounded on purpose: a too-long name is cut, never overrun. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <3ds.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "alog.h"
#include "nettune.h"
#include "ndp/ndp_defs.h"
#include "trpack.h"

#define CONFIG_PATH "sdmc:/3ds/nintendo-dev-agent/config/sources-url"
#define STAGE_DIR "sdmc:/3ds/nintendo-dev-agent/downloads"
#define IO_SIZE (128u * 1024u)
#define SPACE_MARGIN (16ull * 1024ull * 1024ull)
#define ES_MAX 400

extern const char nsd_ca_pem[], nsd_ca_pem_end[]; /* the Mozilla CA bundle, linked in by updater.c */

static LightLock g_lock;
static Thread g_worker;
static bool g_init, g_busy, g_cfg;
static volatile bool g_cancel;
static char g_link[300], g_base[200], g_token[80];
static char g_cur_path[400];  /* what the last finished listing covers */
static char g_list_path[400]; /* what src_list() asked for; the worker reads this once it starts */
static src_entry *g_entries;
static int g_ne;
static src_status g_st;
static bool g_quiet_refresh; /* this run of job_list() is a background revalidation of a folder already on screen: no spinner */

/* The last folders read, kept in memory for the session: entering one again shows it at once while a revalidation (quiet,
 * best-effort) runs behind it. Session-only (lost when the app closes), small, FIFO. */
#define CACHE_MAX 24
typedef struct { char path[400]; src_entry *entries; int n; } cache_ent;
static cache_ent g_cache[CACHE_MAX];
static int g_cache_n, g_cache_next;

static const cache_ent *cache_find(const char *path) {
  int i;
  for (i = 0; i < g_cache_n; i++) if (!strcmp(g_cache[i].path, path)) return &g_cache[i];
  return NULL;
}
static void cache_store(const char *path, const src_entry *entries, int n) {
  int i = -1, k;
  for (k = 0; k < g_cache_n; k++) if (!strcmp(g_cache[k].path, path)) { i = k; break; }
  if (i < 0) {
    if (g_cache_n < CACHE_MAX) i = g_cache_n++;
    else { i = g_cache_next; g_cache_next = (g_cache_next + 1) % CACHE_MAX; }
  }
  free(g_cache[i].entries);
  g_cache[i].entries = NULL;
  g_cache[i].n = 0;
  if (n > 0) {
    g_cache[i].entries = malloc((size_t)n * sizeof *entries);
    if (g_cache[i].entries) { memcpy(g_cache[i].entries, entries, (size_t)n * sizeof *entries); g_cache[i].n = n; }
  }
  snprintf(g_cache[i].path, sizeof g_cache[i].path, "%s", path);
}
static void cache_clear(void) {
  int i;
  for (i = 0; i < g_cache_n; i++) { free(g_cache[i].entries); g_cache[i].entries = NULL; }
  g_cache_n = g_cache_next = 0;
}

static bool cancelled(void) { return __atomic_load_n(&g_cancel, __ATOMIC_ACQUIRE) || aptShouldClose(); }
static void touch(void) { g_st.revision++; }
static void set_state(src_state s) {
  LightLock_Lock(&g_lock);
  g_st.state = s;
  touch();
  LightLock_Unlock(&g_lock);
}
static void fail(src_error e, const char *detail) {
  alog("Sources: failed (error %d) %.100s", (int)e, detail ? detail : "");
  LightLock_Lock(&g_lock);
  g_st.state = SRS_ERROR;
  g_st.error = e;
  snprintf(g_st.detail, sizeof g_st.detail, "%s", detail ? detail : "");
  touch();
  LightLock_Unlock(&g_lock);
}
static uint64_t now_ms(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000); }

/* ------------------------------------------------------------------------------------------------ config */
static bool slurp(const char *path, char *out, size_t cap) {
  FILE *f = fopen(path, "rb");
  size_t n;
  if (!f) return false;
  n = fread(out, 1, cap - 1, f);
  fclose(f);
  out[n] = '\0';
  return true;
}

static void load_config(void) {
  char line[300];
  size_t i;
  g_cfg = false;
  g_link[0] = g_base[0] = g_token[0] = '\0';
  if (!slurp(CONFIG_PATH, line, sizeof line)) return;
  for (i = 0; line[i]; i++)
    if (line[i] == '\r' || line[i] == '\n') { line[i] = '\0'; break; }
  if (tr_parse_share(line, g_base, sizeof g_base, g_token, sizeof g_token)) {
    snprintf(g_link, sizeof g_link, "%s", line);
    g_cfg = true;
  }
}

bool src_configured(void) { return g_cfg; }
const char *src_link(void) { return g_link; }

bool src_set_link(const char *url) {
  char base[200], tok[80];
  FILE *f;
  if (!tr_parse_share(url, base, sizeof base, tok, sizeof tok)) return false;
  mkdir("sdmc:/3ds", 0777);
  mkdir("sdmc:/3ds/nintendo-dev-agent", 0777);
  mkdir("sdmc:/3ds/nintendo-dev-agent/config", 0777);
  f = fopen(CONFIG_PATH, "wb");
  if (!f) return false;
  fprintf(f, "%s\n", url);
  fclose(f);
  LightLock_Lock(&g_lock);
  g_ne = 0;
  g_cur_path[0] = '\0';
  g_st.state = SRS_IDLE;
  touch();
  LightLock_Unlock(&g_lock);
  cache_clear(); /* the cached folders belonged to the old share */
  load_config();
  return g_cfg;
}

void src_forget_link(void) {
  remove(CONFIG_PATH);
  LightLock_Lock(&g_lock);
  g_ne = 0;
  g_cur_path[0] = '\0';
  g_st.state = SRS_IDLE;
  touch();
  LightLock_Unlock(&g_lock);
  cache_clear();
  load_config();
}

/* The full URL of src_at(idx) (a file of the current listing), with the share's token embedded as the Basic-auth user
 * (https://TOKEN:@host/public.php/webdav/path) — for handing straight to the CIA installer, bypassing the generic
 * download-to-a-folder flow: a .cia is installed and no copy of it is kept anywhere. */
bool src_entry_url(int idx, char *out, size_t cap) {
  char enc[700], rel[420];
  const char *host;
  if (!g_cfg || idx < 0 || idx >= g_ne || g_entries[idx].is_dir) return false;
  host = strstr(g_base, "://");
  if (!host) return false;
  host += 3;
  snprintf(rel, sizeof rel, "%s%s%s", g_cur_path, g_cur_path[0] ? "/" : "", g_entries[idx].name);
  url_encode_path(rel, enc, sizeof enc);
  snprintf(out, cap, "https://%s:@%s/public.php/webdav/%s", g_token, host, enc);
  return true;
}

int src_count(void) { return g_ne; }
const src_entry *src_at(int i) { return i >= 0 && i < g_ne ? &g_entries[i] : NULL; }
const char *src_cur_path(void) { return g_cur_path; }

/* ------------------------------------------------------------------------------------------------ HTTP */
static CURL *g_c;
static CURLM *g_mh;
static int g_net_code;
static long g_http_code;
/* A request may carry two extra header lines (Range, If-Range) and may ask for the status line and the ETag of the answer. */
static char g_x1[64], g_x2[112], g_resp_etag[96];
static bool g_capture;
static long g_resp_status;

typedef struct { char *p; size_t n, cap, max; bool over; } buf;
static size_t sink_buf(char *ptr, size_t a, size_t b, void *ud) {
  buf *x = ud;
  size_t n = a * b;
  if (cancelled()) return 0;
  if (x->n + n + 1 > x->cap) {
    size_t nc = x->cap ? x->cap * 2 : 16384;
    char *np;
    while (nc < x->n + n + 1) nc *= 2;
    if (nc > x->max + 1) nc = x->max + 1;
    if (x->n + n + 1 > nc) { x->over = true; return 0; }
    np = realloc(x->p, nc);
    if (!np) { x->over = true; return 0; }
    x->p = np;
    x->cap = nc;
  }
  memcpy(x->p + x->n, ptr, n);
  x->n += n;
  x->p[x->n] = '\0';
  return n;
}
static size_t sink_drop(char *ptr, size_t a, size_t b, void *ud) { (void)ptr; (void)ud; return a * b; }

static size_t on_header(char *p, size_t a, size_t b, void *ud) {
  size_t n = a * b;
  (void)ud;
  if (n > 12 && !strncmp(p, "HTTP/", 5)) g_resp_status = strtol(p + 9, NULL, 10);
  else if (n > 6 && !strncasecmp(p, "ETag:", 5)) {
    const char *v = p + 5;
    size_t l;
    while (*v == ' ') v++;
    l = n - (size_t)(v - p);
    while (l && (v[l - 1] == '\r' || v[l - 1] == '\n' || v[l - 1] == ' ')) l--;
    if (l && l < sizeof g_resp_etag) { memcpy(g_resp_etag, v, l); g_resp_etag[l] = '\0'; }
  }
  return n;
}

static void apply_opts(CURL *c, const char *method, const char *path) {
  static char url[1000], enc[700], userpwd[100];
  static struct curl_blob ca;
  url_encode_path(path, enc, sizeof enc);
  snprintf(url, sizeof url, "%s/public.php/webdav/%s", g_base, enc);
  snprintf(userpwd, sizeof userpwd, "%s:", g_token);
  ca.data = (void *)nsd_ca_pem;
  ca.len = (size_t)(nsd_ca_pem_end - nsd_ca_pem);
  ca.flags = CURL_BLOB_NOCOPY;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "NSD-Bridge/" NDP_AGENT_VERSION);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 128L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)IO_SIZE);
  curl_easy_setopt(c, CURLOPT_USERPWD, userpwd);
  curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
  if (method) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, nsd_sockopt);
}

static bool http(const char *method, const char *path, const char *depth, const char *body, curl_write_callback sink, void *ud) {
  static char dh[24];
  struct curl_slist *hdr = NULL;
  CURLcode code = CURLE_FAILED_INIT;
  int running = 1;
  long http_code = 0;
  curl_easy_reset(g_c);
  apply_opts(g_c, method, path);
  curl_easy_setopt(g_c, CURLOPT_WRITEFUNCTION, sink);
  curl_easy_setopt(g_c, CURLOPT_WRITEDATA, ud);
  if (depth) { snprintf(dh, sizeof dh, "Depth: %s", depth); hdr = curl_slist_append(hdr, dh); }
  if (body) {
    hdr = curl_slist_append(hdr, "Content-Type: application/xml");
    curl_easy_setopt(g_c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(g_c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  }
  if (g_x1[0]) hdr = curl_slist_append(hdr, g_x1);
  if (g_x2[0]) hdr = curl_slist_append(hdr, g_x2);
  if (g_capture) {
    g_resp_status = 0;
    g_resp_etag[0] = '\0';
    curl_easy_setopt(g_c, CURLOPT_HEADERFUNCTION, on_header);
  }
  if (hdr) curl_easy_setopt(g_c, CURLOPT_HTTPHEADER, hdr);
  curl_multi_add_handle(g_mh, g_c);
  while (running) {
    if (curl_multi_perform(g_mh, &running) != CURLM_OK) break;
    if (running) curl_multi_poll(g_mh, NULL, 0, 50, NULL);
    if (cancelled()) { code = CURLE_ABORTED_BY_CALLBACK; running = -1; break; }
  }
  if (running != -1) {
    CURLMsg *msg;
    int left;
    code = CURLE_RECV_ERROR;
    while ((msg = curl_multi_info_read(g_mh, &left)) != NULL)
      if (msg->msg == CURLMSG_DONE) code = msg->data.result;
  }
  curl_easy_getinfo(g_c, CURLINFO_RESPONSE_CODE, &http_code);
  curl_multi_remove_handle(g_mh, g_c);
  curl_slist_free_all(hdr);
  g_net_code = (int)code;
  g_http_code = http_code;
  g_x1[0] = g_x2[0] = '\0';
  g_capture = false;
  return code == CURLE_OK && http_code > 0 && http_code < 400;
}

static void net_fail(const char *what) {
  alog("Sources: %s failed (curl %d, HTTP %ld)", what, g_net_code, g_http_code);
  LightLock_Lock(&g_lock);
  g_st.net_code = g_net_code;
  g_st.http_code = g_http_code;
  LightLock_Unlock(&g_lock);
  if (cancelled()) fail(SRE_CANCELLED, "");
  else fail(SRE_NET, what);
}

static const char PROPFIND_BODY[] =
    "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/></d:prop></d:propfind>";

/* ------------------------------------------------------------------------------------------------ listing */
static char g_job_path[400]; /* the path this job was asked to list, fixed at the start (g_list_path may move on if the owner
                               * navigates while a quiet background refresh is still running) */
static void job_list(void) {
  static char p[420];
  dav_entry *es;
  src_entry *out;
  buf b = {0, 0, 0, 2u * 1024u * 1024u, false};
  int n, i, ni;
  bool quiet = g_quiet_refresh;
  snprintf(g_job_path, sizeof g_job_path, "%s", g_list_path);
  if (!quiet) set_state(SRS_LISTING); /* a cache hit already shows something: no spinner for the refresh behind it */
  es = malloc(ES_MAX * sizeof *es);
  out = malloc(ES_MAX * sizeof *out);
  if (!es || !out) { free(es); free(out); if (!quiet) fail(SRE_IO, "memory"); return; }
  if (g_job_path[0]) snprintf(p, sizeof p, "%s/", g_job_path);
  else p[0] = '\0';
  if (!http("PROPFIND", p, "1", PROPFIND_BODY, sink_buf, &b)) {
    free(b.p);
    free(es);
    free(out);
    if (!quiet) net_fail("listing"); /* a failed quiet revalidation keeps whatever is already on screen, silently */
    else alog("Sources: background refresh of \"%s\" failed (curl %d, HTTP %ld)", g_job_path, g_net_code, g_http_code);
    return;
  }
  n = b.p ? dav_parse(b.p, b.n, es, ES_MAX) : 0;
  free(b.p);
  if (cancelled()) { free(es); free(out); if (!quiet) fail(SRE_CANCELLED, ""); return; }
  n = n < ES_MAX ? n : ES_MAX;
  for (ni = 0, i = 0; i < n; i++) {
    if (es[i].name[0] == '.' || tr_skip_name(es[i].name)) continue; /* hidden (a leading dot) and macOS/Windows leftovers */
    out[ni].is_dir = es[i].is_dir;
    out[ni].size = es[i].size;
    snprintf(out[ni].name, sizeof out[ni].name, "%s", es[i].name);
    ni++;
  }
  free(es);
  /* folders first, then alphabetically (case-insensitive) */
  for (i = 1; i < ni; i++) {
    src_entry key = out[i];
    int j = i - 1;
    while (j >= 0 && (out[j].is_dir < key.is_dir || (out[j].is_dir == key.is_dir && strcasecmp(out[j].name, key.name) > 0))) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = key;
  }
  cache_store(g_job_path, out, ni);
  if (strcmp(g_job_path, g_list_path) != 0) { free(out); return; } /* the owner moved on while this (quiet) job was running */
  memcpy(g_entries, out, (size_t)ni * sizeof *out);
  free(out);
  g_ne = ni;
  snprintf(g_cur_path, sizeof g_cur_path, "%s", g_job_path);
  alog("Sources: %d entries at \"%s\"", g_ne, g_cur_path[0] ? g_cur_path : "/");
  set_state(SRS_READY);
}

/* ------------------------------------------------------------------------------------------------ download */
static int g_dl_idx;
static char g_dl_dir[330], g_dl_name[220];
static char g_stage_path[300], g_stage_tag[308];
static uint64_t g_dl_total;

static void stage_names(const char *dir, const char *name) {
  uint64_t h = 1469598103934665603ull;
  const char *p;
  char key[560];
  snprintf(key, sizeof key, "%s/%s", dir, name);
  for (p = key; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
  snprintf(g_stage_path, sizeof g_stage_path, STAGE_DIR "/src-%08lx%08lx.part", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
  snprintf(g_stage_tag, sizeof g_stage_tag, STAGE_DIR "/src-%08lx%08lx.stag", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
}

static void dl_progress(uint64_t done) {
  static uint64_t last_ms, last_bytes;
  static double ema;
  uint64_t t = now_ms();
  LightLock_Lock(&g_lock);
  g_st.bytes_done = done;
  if (t - last_ms >= 1000) {
    double inst = (double)(done - last_bytes) * 1000.0 / (double)(t - last_ms);
    ema = ema > 0 ? 0.8 * ema + 0.2 * inst : inst;
    g_st.bps = (unsigned)inst;
    g_st.eta_s = ema > 1.0 && g_dl_total > done ? (unsigned)((double)(g_dl_total - done) / ema) : 0;
    last_ms = t;
    last_bytes = done;
    touch();
  } else if (t - last_ms >= 250) {
    touch();
  }
  LightLock_Unlock(&g_lock);
}

typedef struct { FILE *f; uint64_t base, got; bool opened; char *wbuf; } dsink;
static size_t sink_file(char *ptr, size_t a, size_t b, void *ud) {
  dsink *x = ud;
  size_t n = a * b;
  if (cancelled() || g_resp_status >= 300) return 0;
  if (!x->opened) {
    /* 206: the share continues from where the part ends; 200: it sends the whole file, so the part starts over */
    if (x->base > 0 && g_resp_status == 206) { x->f = fopen(g_stage_path, "r+b"); if (x->f) fseeko(x->f, (off_t)x->base, SEEK_SET); }
    else { x->base = 0; x->f = fopen(g_stage_path, "wb"); }
    if (!x->f) return 0;
    setvbuf(x->f, x->wbuf, _IOFBF, IO_SIZE);
    x->opened = true;
    if (g_resp_etag[0]) { FILE *t = fopen(g_stage_tag, "wb"); if (t) { fputs(g_resp_etag, t); fclose(t); } }
  }
  if (fwrite(ptr, 1, n, x->f) != n) return 0;
  x->got += n;
  dl_progress(x->base + x->got);
  return n;
}

static void job_download(void) {
  static char url[460], dst[560], etag[96];
  struct stat sb;
  uint64_t base = 0;
  FS_ArchiveResource r;
  dsink ds;
  bool ok;
  const src_entry *e;
  set_state(SRS_DOWNLOADING);
  if (g_dl_idx < 0 || g_dl_idx >= g_ne || g_entries[g_dl_idx].is_dir) { fail(SRE_IO, "file"); return; }
  e = &g_entries[g_dl_idx];
  g_dl_total = e->size;
  snprintf(dst, sizeof dst, "%s/%s", g_dl_dir, g_dl_name);
  if (stat(dst, &sb) == 0) { fail(SRE_EXISTS, g_dl_name); return; }
  LightLock_Lock(&g_lock);
  g_st.bytes_total = g_dl_total;
  g_st.bytes_done = 0;
  g_st.bps = g_st.eta_s = 0;
  snprintf(g_st.name, sizeof g_st.name, "%s", g_dl_name);
  touch();
  LightLock_Unlock(&g_lock);
  mkdir("sdmc:/3ds", 0777);
  mkdir("sdmc:/3ds/nintendo-dev-agent", 0777);
  mkdir(STAGE_DIR, 0777);
  stage_names(g_dl_dir, g_dl_name);
  etag[0] = '\0';
  if (stat(g_stage_path, &sb) == 0) {
    if (!slurp(g_stage_tag, etag, sizeof etag) || etag[0] == '\0' || (uint64_t)sb.st_size > g_dl_total) { remove(g_stage_path); remove(g_stage_tag); etag[0] = '\0'; }
    else base = (uint64_t)sb.st_size;
  }
  snprintf(url, sizeof url, "%s%s%s", g_cur_path, g_cur_path[0] ? "/" : "", e->name);
  if (base == g_dl_total && g_dl_total > 0) { /* all of it is staged: one tiny request tells whether the share still has this same file */
    snprintf(g_x1, sizeof g_x1, "Range: bytes=0-0");
    snprintf(g_x2, sizeof g_x2, "If-Range: %s", etag);
    g_capture = true;
    ok = http("GET", url, NULL, NULL, sink_drop, NULL);
    if (ok && g_resp_status == 206) base = g_dl_total;
    else if (cancelled()) { fail(SRE_CANCELLED, ""); return; }
    else if (!ok && g_http_code >= 400 && g_http_code != 416) { net_fail(g_dl_name); return; }
    else { remove(g_stage_path); remove(g_stage_tag); base = 0; etag[0] = '\0'; }
  }
  if (base < g_dl_total || g_dl_total == 0) {
    if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) && (uint64_t)r.freeClusters * r.clusterSize < (g_dl_total - base) + SPACE_MARGIN) { fail(SRE_SPACE, ""); return; }
    memset(&ds, 0, sizeof ds);
    ds.base = base;
    ds.wbuf = malloc(IO_SIZE);
    if (!ds.wbuf) { fail(SRE_IO, "memory"); return; }
    if (base > 0) {
      snprintf(g_x1, sizeof g_x1, "Range: bytes=%llu-", (unsigned long long)base);
      snprintf(g_x2, sizeof g_x2, "If-Range: %s", etag);
    }
    g_capture = true;
    dl_progress(base);
    ok = http("GET", url, NULL, NULL, sink_file, &ds);
    if (ds.f) fclose(ds.f);
    free(ds.wbuf);
    if (!ok) {
      if (cancelled()) fail(SRE_CANCELLED, "");
      else net_fail(g_dl_name);
      return; /* what arrived stays staged: the same download continues from there next time */
    }
    if (stat(g_stage_path, &sb) != 0 || (g_dl_total && (uint64_t)sb.st_size != g_dl_total)) { remove(g_stage_path); remove(g_stage_tag); fail(SRE_SIZE, g_dl_name); return; }
  }
  if (rename(g_stage_path, dst) != 0) { fail(SRE_IO, g_dl_name); return; }
  remove(g_stage_tag);
  alog("Sources: %s downloaded to %s", g_dl_name, g_dl_dir);
  LightLock_Lock(&g_lock);
  g_st.bytes_done = g_dl_total;
  touch();
  LightLock_Unlock(&g_lock);
  set_state(SRS_DONE);
}

/* ------------------------------------------------------------------------------------------------ the worker */
enum { J_LIST, J_DOWNLOAD };
static int g_job;

static void run(void *arg) {
  (void)arg;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { fail(SRE_NET, "curl"); goto done; }
  g_c = curl_easy_init();
  g_mh = curl_multi_init();
  if (!g_c || !g_mh) { fail(SRE_IO, "memory"); goto cleanup; }
  if (g_job == J_LIST) job_list();
  else job_download();
cleanup:
  if (g_c) curl_easy_cleanup(g_c);
  g_c = NULL;
  if (g_mh) curl_multi_cleanup(g_mh);
  g_mh = NULL;
  curl_global_cleanup();
done:
  __atomic_store_n(&g_busy, false, __ATOMIC_RELEASE);
}

static bool start(int job) {
  s32 prio = 0x30;
  if (!g_init || !g_cfg || __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE)) return false;
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  g_job = job;
  __atomic_store_n(&g_cancel, false, __ATOMIC_RELEASE);
  __atomic_store_n(&g_busy, true, __ATOMIC_RELEASE);
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  g_worker = threadCreate(run, NULL, 96 * 1024, prio > 0x18 ? prio - 1 : prio, 0, false);
  if (!g_worker) { __atomic_store_n(&g_busy, false, __ATOMIC_RELEASE); return false; }
  return true;
}

/* ------------------------------------------------------------------------------------------------ public API */
bool src_busy(void) { return __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE); }
void src_get(src_status *out) {
  if (!g_init) { memset(out, 0, sizeof *out); return; }
  LightLock_Lock(&g_lock);
  *out = g_st;
  LightLock_Unlock(&g_lock);
}
void src_cancel(void) { __atomic_store_n(&g_cancel, true, __ATOMIC_RELEASE); }
void src_dismiss(void) {
  src_status s;
  src_get(&s);
  if (s.state == SRS_DONE || s.state == SRS_ERROR) set_state(SRS_READY);
}

void src_list(const char *path) {
  const cache_ent *c;
  if (!g_init || !g_cfg) return;
  path = path ? path : "";
  c = cache_find(path);
  if (c) {
    /* shown at once from what was read before; a quiet refresh (best-effort: skipped if the worker is already busy with
     * something else) checks whether the share still agrees, and updates the list if not */
    LightLock_Lock(&g_lock);
    g_ne = c->n;
    if (c->n) memcpy(g_entries, c->entries, (size_t)c->n * sizeof *g_entries);
    snprintf(g_cur_path, sizeof g_cur_path, "%s", path);
    g_st.state = SRS_READY;
    g_st.error = SRE_NONE;
    touch();
    LightLock_Unlock(&g_lock);
    snprintf(g_list_path, sizeof g_list_path, "%s", path);
    if (!src_busy()) { g_quiet_refresh = true; start(J_LIST); }
    return;
  }
  if (src_busy()) return;
  g_quiet_refresh = false;
  snprintf(g_list_path, sizeof g_list_path, "%s", path);
  if (!start(J_LIST)) fail(SRE_IO, "thread");
}

void src_download(int idx, const char *dest_dir, const char *dest_name) {
  if (!g_init || !g_cfg) return;
  if (idx < 0 || idx >= g_ne || g_entries[idx].is_dir) { fail(SRE_IO, "the list changed: open the file again"); return; }
  if (src_busy()) { fail(SRE_IO, "busy"); return; } /* should not happen (the confirm screen is only up when idle), but never silent */
  alog("Sources: downloading \"%s\" (%llu bytes) to %s", g_entries[idx].name, (unsigned long long)g_entries[idx].size, dest_dir);
  g_dl_idx = idx;
  snprintf(g_dl_dir, sizeof g_dl_dir, "%s", dest_dir);
  snprintf(g_dl_name, sizeof g_dl_name, "%s", dest_name);
  if (!start(J_DOWNLOAD)) fail(SRE_IO, "thread");
}

void src_init(void) {
  if (g_init) return;
  LightLock_Init(&g_lock);
  g_entries = calloc(ES_MAX, sizeof *g_entries);
  g_init = g_entries != NULL;
  load_config();
}
void src_shutdown(void) {
  if (!g_init) return;
  __atomic_store_n(&g_cancel, true, __ATOMIC_RELEASE);
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  cache_clear();
  free(g_entries);
  g_entries = NULL;
  g_init = false;
}
#endif
