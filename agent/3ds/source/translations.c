#ifndef AGENT_TEXT_UI
#include "translations.h"

/* Copies into fixed buffers are bounded on purpose: a too-long name is cut, never overrun. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <3ds.h>
#include <curl/curl.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

#include "alog.h"
#include "nettune.h"
#include "tzip.h"
#include "ndp/ndp_defs.h"

#define CACHE_DIR "sdmc:/3ds/nintendo-dev-agent/cache"
#define MAX_PACKS 512
#define MAX_FILES 20000u
#define MAX_BYTES (1536ull * 1024ull * 1024ull)
#define SPACE_MARGIN (16ull * 1024ull * 1024ull)
#define IO_SIZE (128u * 1024u)

extern const char nsd_ca_pem[], nsd_ca_pem_end[]; /* the Mozilla CA bundle, linked in by updater.c */

static LightLock g_lock;
static Thread g_worker;
#ifndef NSD_DEFAULT_TRANSLATIONS_URL
#define NSD_DEFAULT_TRANSLATIONS_URL ""
#endif
#ifndef NSD_DEFAULT_MODS_URL
#define NSD_DEFAULT_MODS_URL ""
#endif
static bool g_init, g_busy;
/* The share either contains a traducoes/ folder or IS that folder: g_root is "traducoes" or "" (paths are always from the share's root). */
static tr_status g_st;
/* Two catalogs share the engine: translations (KIND_TRANSL: <lang>/<ID - game>/<team>/<version>) and mods (KIND_MOD:
 * <ID - game>/<mod>/<version>). Each has its own link, saved copy, packs and records; only one is active at a time. */
typedef struct {
  const char *root_name, *cfg_path, *cache_path, *default_url;
  tr_pack *packs;
  int np;
  char link[300], base[200], token[80];
  bool cfg_ok, is_default;
  char root[16]; /* "" or root_name, whichever the share turned out to use */
} kind_t;
static kind_t K[2];
static int g_kind;
#define g_packs (K[g_kind].packs)
#define g_np (K[g_kind].np)
#define g_link (K[g_kind].link)
#define g_base (K[g_kind].base)
#define g_token (K[g_kind].token)
#define g_cfg (K[g_kind].cfg_ok)
#define g_link_default (K[g_kind].is_default)
#define g_root (K[g_kind].root)
#define CONFIG_PATH (K[g_kind].cfg_path)
#define CACHE_PATH (K[g_kind].cache_path)
#define ROOT (K[g_kind].root_name)
static volatile bool g_cancel, g_answered, g_confirmed;

/* what tr_prepare gathered: the files of the pack, as paths relative to arquivos/ */
typedef struct { uint32_t off; uint64_t size; } fent;
static char *g_pool;
static size_t g_pool_n, g_pool_cap;
static fent *g_files;
static uint8_t *g_fflag;
static unsigned g_nf;
static int g_prep_pack = -1;
static bool g_prep_uninstall;

static bool cancelled(void) { return __atomic_load_n(&g_cancel, __ATOMIC_ACQUIRE) || aptShouldClose(); }
static void touch(void) { g_st.revision++; }
static void set_state(trs_state s) { LightLock_Lock(&g_lock); g_st.state = s; touch(); LightLock_Unlock(&g_lock); }
static void fail(tr_error e, const char *detail) {
  alog("Translations: failed (error %d) %.80s", (int)e, detail ? detail : "");
  LightLock_Lock(&g_lock);
  g_st.state = TRS_ERROR;
  g_st.error = e;
  snprintf(g_st.detail, sizeof g_st.detail, "%s", detail ? detail : "");
  touch();
  LightLock_Unlock(&g_lock);
}
static uint64_t now_ms(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000); }
static const char *file_at(unsigned i) { return g_pool + g_files[i].off; }

/* ------------------------------------------------------------------------------------------------ config */
static bool slurp(const char *path, char *out, size_t cap, size_t *len) {
  FILE *f = fopen(path, "rb");
  size_t n;
  if (!f) return false;
  n = fread(out, 1, cap - 1, f);
  fclose(f);
  out[n] = '\0';
  if (len) *len = n;
  return true;
}

/* The link in config/translations-url wins; without one the link this build ships with (NDEV_TRANSLATIONS_URL) is used. */
static void load_config(void) {
  char line[300];
  size_t i;
  g_cfg = false;
  g_link_default = false;
  g_link[0] = g_base[0] = g_token[0] = '\0';
  if (slurp(CONFIG_PATH, line, sizeof line, NULL)) {
    for (i = 0; line[i]; i++) if (line[i] == '\r' || line[i] == '\n') { line[i] = '\0'; break; }
    if (tr_parse_share(line, g_base, sizeof g_base, g_token, sizeof g_token)) {
      snprintf(g_link, sizeof g_link, "%s", line);
      g_cfg = true;
      return;
    }
  }
  if (K[g_kind].default_url[0] && tr_parse_share(K[g_kind].default_url, g_base, sizeof g_base, g_token, sizeof g_token)) {
    snprintf(g_link, sizeof g_link, "%s", K[g_kind].default_url);
    g_cfg = true;
    g_link_default = true;
  }
}

bool tr_link_is_default(void) { return g_link_default; }

void tr_reset_link(void) {
  remove(CONFIG_PATH);
  remove(CACHE_PATH);
  LightLock_Lock(&g_lock);
  g_np = 0;
  touch();
  LightLock_Unlock(&g_lock);
  load_config();
}

bool tr_configured(void) { return g_cfg; }
const char *tr_link(void) { return g_link; }

bool tr_set_link(const char *url) {
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
  remove(CACHE_PATH); /* the saved catalog belongs to the old link */
  LightLock_Lock(&g_lock);
  g_np = 0;
  touch();
  LightLock_Unlock(&g_lock);
  load_config();
  return g_cfg;
}

/* ------------------------------------------------------------------------------------------------ HTTP */
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

static char g_etag[72]; /* the share root's etag when the saved catalog was made */
static CURL *g_c;
static int g_net_code;
static long g_http_code;

static const char PROPFIND_BODY[] =
    "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/></d:prop></d:propfind>";

/* One request on the share. True when curl succeeded and the server answered below 400. The connection is kept between calls. */
/* The options every request on the share uses; the string arguments are copied by curl, so the buffers can be reused. */
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
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L); /* the credentials never leave the share's server */
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

/* One request on the share. True when curl succeeded and the server answered below 400. All requests go through the one
 * multi handle of the job (g_mh), so the HTTPS connection stays open between them: a new handshake per file is what made the
 * first version take seconds for each tiny file. */
static CURLM *g_mh;
/* A request may carry two extra header lines (Range, If-Range) and may ask for the status line and the ETag of the answer. All are
 * cleared when the request ends. */
static char g_x1[64], g_x2[112], g_resp_etag[96];
static bool g_capture;
static long g_resp_status;
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
  /* short waits: the console's socket service handles one request at a time, and the bridge shares it */
  while (running) {
    if (curl_multi_perform(g_mh, &running) != CURLM_OK) break;
    if (running) curl_multi_poll(g_mh, NULL, 0, 50, NULL);
    if (cancelled()) { code = CURLE_ABORTED_BY_CALLBACK; running = -1; break; }
  }
  if (running != -1) {
    CURLMsg *msg;
    int left;
    code = CURLE_RECV_ERROR;
    while ((msg = curl_multi_info_read(g_mh, &left)) != NULL) if (msg->msg == CURLMSG_DONE) code = msg->data.result;
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
  alog("Translations: %s failed (curl %d, HTTP %ld)", what, g_net_code, g_http_code);
  LightLock_Lock(&g_lock);
  g_st.net_code = g_net_code;
  g_st.http_code = g_http_code;
  LightLock_Unlock(&g_lock);
  if (cancelled()) fail(TRE_CANCELLED, "");
  else fail(TRE_NET, what);
}

/* A folder's content, one level (PROPFIND, Depth: 1). Returns the entries or -1. */
static dav_entry *g_es;
#define ES_MAX 400
static int list_dir(const char *path) {
  static char p[420];
  buf b = {0, 0, 0, 2u * 1024u * 1024u, false};
  int n;
  if (path[0]) snprintf(p, sizeof p, "%s/", path);
  else p[0] = '\0';
  LightLock_Lock(&g_lock);
  g_st.list_requests++;
  touch();
  LightLock_Unlock(&g_lock);
  if (!http("PROPFIND", p, "1", PROPFIND_BODY, sink_buf, &b)) { free(b.p); return -1; }
  n = b.p ? dav_parse(b.p, b.n, g_es, ES_MAX) : 0;
  free(b.p);
  return n;
}

/* ------------------------------------------------------------------------------------------------ catalog */
static void add_pack(const tr_ref *r, const char *path) {
  tr_pack *p;
  if (g_np >= MAX_PACKS) return;
  p = &g_packs[g_np++];
  memset(p, 0, sizeof *p);
  p->ref = *r;
  p->kind = g_kind;
  snprintf(p->path, sizeof p->path, "%s", path);
}

static int cmp_pack(const void *a, const void *b) {
  const tr_pack *x = a, *y = b;
  int c = strcmp(x->ref.lang, y->ref.lang);
  if (c) return c;
  c = strcasecmp(x->ref.game, y->ref.game);
  if (c) return c;
  c = strcmp(x->ref.id, y->ref.id);
  if (c) return c;
  c = strcmp(x->ref.team, y->ref.team);
  if (c) return c;
  return -tr_version_cmp(x->ref.version, y->ref.version);
}

/* The few lines the lists show under each pack come from its pacote.json (one small request each, kept in the saved catalog). */
static void clean_field(char *t) { for (; *t; t++) if (*t == '\t' || *t == '\n' || *t == '\r') *t = ' '; }
static void fetch_meta_all(void) {
  static char path[420], tmp[200];
  int i;
  for (i = 0; i < g_np && !cancelled(); i++) {
    tr_pack *p = &g_packs[i];
    buf b = {0, 0, 0, 8191, false};
    snprintf(path, sizeof path, "%s/pacote.json", p->path);
    LightLock_Lock(&g_lock);
    g_st.list_requests++;
    touch();
    LightLock_Unlock(&g_lock);
    if (http("GET", path, NULL, NULL, sink_buf, &b) && b.p) {
      if (tr_json_get(b.p, "descricao", tmp, sizeof tmp)) { snprintf(p->desc, sizeof p->desc, "%s", tmp); clean_field(p->desc); }
      if (tr_json_get(b.p, "status", tmp, sizeof tmp)) { snprintf(p->status, sizeof p->status, "%s", tmp); clean_field(p->status); }
      if (tr_json_get(b.p, p->kind == 1 ? "autor" : "equipe", tmp, sizeof tmp)) { snprintf(p->author, sizeof p->author, "%s", tmp); clean_field(p->author); }
      if (tr_json_get(b.p, "regiao", tmp, sizeof tmp)) { snprintf(p->region, sizeof p->region, "%s", tmp); clean_field(p->region); }
    }
    free(b.p);
  }
}

static void save_cache(void) {
  FILE *f;
  int i;
  mkdir("sdmc:/3ds/nintendo-dev-agent/cache", 0777);
  f = fopen(CACHE_PATH, "wb");
  if (!f) return;
  fprintf(f, "# %s\n@ %s\n~ %s\n", g_link, g_root, g_etag);
  for (i = 0; i < g_np; i++)
    fprintf(f, "%s\t%s - %s\t%s\t%s\t%s\t%s\t%s\t%s\n", g_packs[i].ref.lang, g_packs[i].ref.id, g_packs[i].ref.game, g_packs[i].ref.team, g_packs[i].ref.version,
            g_packs[i].desc, g_packs[i].status, g_packs[i].author, g_packs[i].region);
  fclose(f);
}

static bool load_cache(void) {
  FILE *f = fopen(CACHE_PATH, "rb");
  static char line[600];
  bool ok = false;
  if (!f) return false;
  if (fgets(line, sizeof line, f) && line[0] == '#' && line[1] == ' ') {
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
    ok = !strcmp(line + 2, g_link);
  }
  if (ok) {
    ok = fgets(line, sizeof line, f) && line[0] == '@' && line[1] == ' ';
    if (ok) {
      size_t l = strlen(line);
      while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
      ok = !strcmp(line + 2, ROOT) || line[2] == '\0';
      snprintf(g_root, sizeof g_root, "%s", line + 2);
    }
  }
  g_np = 0;
  g_etag[0] = '\0';
  while (ok && fgets(line, sizeof line, f)) {
    char *lang = line, *idn, *team, *ver, *e;
    tr_ref r;
    static char path[320];
    char *meta_fields[4] = {NULL, NULL, NULL, NULL};
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
    if (line[0] == '~' && line[1] == ' ') { snprintf(g_etag, sizeof g_etag, "%s", line + 2); continue; }
    idn = strchr(lang, '\t');
    if (!idn) continue;
    *idn++ = '\0';
    team = strchr(idn, '\t');
    if (!team) continue;
    *team++ = '\0';
    ver = strchr(team, '\t');
    if (!ver) continue;
    *ver++ = '\0';
    e = strchr(ver, '\t');
    if (e) *e++ = '\0';
    if (!tr_parse_ref(lang, idn, team, ver, &r)) continue;
    {
      char *fields[4] = {e, NULL, NULL, NULL}; /* description, status, author, region (older saved copies have none) */
      int fi;
      for (fi = 0; fi < 3 && fields[fi]; fi++) { char *t = strchr(fields[fi], '\t'); if (t) *t++ = '\0'; fields[fi + 1] = t; }
      memcpy(meta_fields, fields, sizeof fields);
    }
    if (g_kind == 1) { /* a mod: <ID - game>/<mod>/<version> */
      if (g_root[0]) snprintf(path, sizeof path, "%s/%s/%s/%s", g_root, idn, team, ver);
      else snprintf(path, sizeof path, "%s/%s/%s", idn, team, ver);
    } else if (g_root[0]) snprintf(path, sizeof path, "%s/%s/%s/%s/%s", g_root, lang, idn, team, ver);
    else snprintf(path, sizeof path, "%s/%s/%s/%s", lang, idn, team, ver);
    add_pack(&r, path);
    if (g_np > 0) {
      tr_pack *np = &g_packs[g_np - 1];
      if (meta_fields[0]) snprintf(np->desc, sizeof np->desc, "%s", meta_fields[0]);
      if (meta_fields[1]) snprintf(np->status, sizeof np->status, "%s", meta_fields[1]);
      if (meta_fields[2]) snprintf(np->author, sizeof np->author, "%s", meta_fields[2]);
      if (meta_fields[3]) snprintf(np->region, sizeof np->region, "%s", meta_fields[3]);
    }
  }
  fclose(f);
  if (!ok) g_np = 0;
  return ok && g_np > 0;
}

/* A folder's etag changes whenever anything inside it does, so one small request tells whether the saved catalog is still current. */
static const char ETAG_BODY[] = "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/></d:prop></d:propfind>";
static bool fetch_etag(char *out, size_t cap) {
  buf b = {0, 0, 0, 8191, false};
  const char *e;
  size_t n = 0;
  out[0] = '\0';
  if (!http("PROPFIND", "", "0", ETAG_BODY, sink_buf, &b) || !b.p) { free(b.p); return false; }
  e = strstr(b.p, "getetag>");
  if (e) {
    for (e += 8; *e && *e != '<' && n + 1 < cap; e++)
      if ((*e >= '0' && *e <= '9') || (*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') || *e == '-' || *e == '_') out[n++] = *e;
    out[n] = '\0';
  }
  free(b.p);
  return n > 0;
}

/* The mods catalog: <ID - game>/<mod>/<version>, three levels under the root. */
static void job_list_mods(void) {
  static char p1[420], p2[420], path[420];
  static dav_entry games[ES_MAX], mods[ES_MAX];
  int nr, ng, nm, nv, i, g, m, v;
  g_np = 0;
  nr = list_dir("");
  if (nr < 0) { net_fail("listing"); return; }
  g_root[0] = '\0';
  for (i = 0; i < nr; i++) if (g_es[i].is_dir && !strcmp(g_es[i].name, ROOT)) snprintf(g_root, sizeof g_root, "%s", ROOT);
  if (g_root[0]) {
    ng = list_dir(g_root);
    if (ng < 0) { net_fail("listing"); return; }
  } else {
    ng = nr;
  }
  memcpy(games, g_es, sizeof games[0] * (size_t)(ng < ES_MAX ? ng : ES_MAX));
  for (g = 0; g < ng && !cancelled(); g++) {
    if (!games[g].is_dir) continue;
    if (g_root[0]) snprintf(p1, sizeof p1, "%s/%s", g_root, games[g].name);
    else snprintf(p1, sizeof p1, "%s", games[g].name);
    nm = list_dir(p1);
    if (nm < 0) { net_fail("listing"); return; }
    memcpy(mods, g_es, sizeof mods[0] * (size_t)nm);
    for (m = 0; m < nm && !cancelled(); m++) {
      if (!mods[m].is_dir) continue;
      snprintf(p2, sizeof p2, "%s/%s", p1, mods[m].name);
      nv = list_dir(p2);
      if (nv < 0) { net_fail("listing"); return; }
      for (v = 0; v < nv; v++) {
        tr_ref r;
        if (!g_es[v].is_dir) continue;
        if (!tr_parse_ref("mod", games[g].name, mods[m].name, g_es[v].name, &r)) continue;
        snprintf(path, sizeof path, "%s/%s", p2, g_es[v].name);
        add_pack(&r, path);
      }
    }
  }
  if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
  qsort(g_packs, (size_t)g_np, sizeof *g_packs, cmp_pack);
  fetch_meta_all();
  save_cache();
  alog("Mods: %d packs listed (%d requests)", g_np, g_st.list_requests);
  set_state(TRS_READY);
}

static void job_list(void) {
  if (g_kind == 1) { job_list_mods(); return; }
  int nl, l, ng, g, nt, t, nv, v;
  static char p1[420], p2[420], p3[420];
  static dav_entry langs[24];
  int i, nr;
  g_np = 0;
  /* the share's root: a "traducoes" folder inside it, or the root already is the catalog */
  nr = list_dir("");
  if (nr < 0) { net_fail("listing"); return; }
  g_root[0] = '\0';
  for (i = 0; i < nr; i++) if (g_es[i].is_dir && !strcmp(g_es[i].name, ROOT)) snprintf(g_root, sizeof g_root, "%s", ROOT);
  if (g_root[0]) {
    nl = list_dir(g_root);
    if (nl < 0) { net_fail("listing"); return; }
  } else {
    nl = nr;
  }
  memcpy(langs, g_es, sizeof langs[0] * (size_t)(nl < 24 ? nl : 24));
  for (l = 0; l < nl && l < 24 && !cancelled(); l++) {
    static dav_entry games[ES_MAX];
    if (!langs[l].is_dir) continue;
    if (g_root[0]) snprintf(p1, sizeof p1, "%s/%s", g_root, langs[l].name);
    else snprintf(p1, sizeof p1, "%s", langs[l].name);
    ng = list_dir(p1);
    if (ng < 0) { net_fail("listing"); return; }
    memcpy(games, g_es, sizeof games[0] * (size_t)ng);
    for (g = 0; g < ng && !cancelled(); g++) {
      static dav_entry teams[ES_MAX];
      if (!games[g].is_dir) continue;
      snprintf(p2, sizeof p2, "%s/%s", p1, games[g].name);
      nt = list_dir(p2);
      if (nt < 0) { net_fail("listing"); return; }
      memcpy(teams, g_es, sizeof teams[0] * (size_t)nt);
      for (t = 0; t < nt && !cancelled(); t++) {
        if (!teams[t].is_dir) continue;
        snprintf(p3, sizeof p3, "%s/%s", p2, teams[t].name);
        nv = list_dir(p3);
        if (nv < 0) { net_fail("listing"); return; }
        for (v = 0; v < nv; v++) {
          tr_ref r;
          static char path[420];
          if (!g_es[v].is_dir) continue;
          if (!tr_parse_ref(langs[l].name, games[g].name, teams[t].name, g_es[v].name, &r)) continue;
          snprintf(path, sizeof path, "%s/%s", p3, g_es[v].name);
          add_pack(&r, path);
        }
      }
    }
  }
  if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
  qsort(g_packs, (size_t)g_np, sizeof *g_packs, cmp_pack);
  fetch_meta_all();
  save_cache();
  alog("Translations: %d packs listed (%d requests)", g_np, g_st.list_requests);
  set_state(TRS_READY);
}

/* ------------------------------------------------------------------------------------------------ marks */
#define LUMA_TITLES "sdmc:/luma/titles"

/* A file name made of safe characters from a mod's folder name. */
static void slug_of(const char *s2, char *out, size_t cap) {
  size_t o = 0;
  for (; *s2 && o + 1 < cap && o < 40; s2++) out[o++] = (isalnum((unsigned char)*s2) || *s2 == '-' || *s2 == '_' || *s2 == '.') ? *s2 : '_';
  out[o] = '\0';
}

/* Where a pack's record lives. The translation keeps the original name; every mod has its own file, so a translation and
 * several mods can sit in one title folder, each with its own list of files. */
static void record_path_of(const char *id, int kind, const char *team, char *out, size_t cap) {
  if (kind == 0) snprintf(out, cap, LUMA_TITLES "/%s/.nsd-pack.json", id);
  else {
    char sl[48];
    slug_of(team, sl, sizeof sl);
    snprintf(out, cap, LUMA_TITLES "/%s/.nsd-mod-%s.json", id, sl);
  }
}

static bool read_record(const tr_pack *p, char *ver, size_t vcap, char *team, size_t tcap, bool *complete) {
  static char path[160], head[1024], tmp[8];
  record_path_of(p->ref.id, p->kind, p->ref.team, path, sizeof path);
  if (!slurp(path, head, sizeof head, NULL)) return false;
  if (ver && !tr_json_get(head, "versao", ver, vcap)) ver[0] = '\0';
  if (team && !tr_json_get(head, "equipe_pasta", team, tcap)) team[0] = '\0';
  if (complete) *complete = !(tr_json_get(head, "completo", tmp, sizeof tmp) && !strcmp(tmp, "nao"));
  return true;
}

#define EMPTY_MARK ".nsd-empty" /* left in /luma/titles/<ID> after an uninstall that kept the (empty) folders */
static bool dir_has_patch(const char *id) {
  static char path[110];
  struct stat sb;
  snprintf(path, sizeof path, LUMA_TITLES "/%s/" EMPTY_MARK, id);
  if (stat(path, &sb) == 0) return false; /* only the empty folders of a pack that was uninstalled */
  static const char *const names[] = {"romfs", "exefs", "code.ips", "code.bin"};
  size_t i;
  for (i = 0; i < sizeof names / sizeof *names; i++) {
    snprintf(path, sizeof path, LUMA_TITLES "/%s/%s", id, names[i]);
    if (stat(path, &sb) == 0) return true;
  }
  return false;
}

void tr_refresh_marks(void) {
  int i, j;
  static char ver[16], team[64];
  bool complete;
  for (i = 0; i < g_np; i++) {
    tr_pack *p = &g_packs[i];
    p->mark = 0;
    p->update = false;
    p->inst_version[0] = '\0';
    complete = true;
    if (read_record(p, ver, sizeof ver, team, sizeof team, &complete)) {
      p->mark = complete ? 2 : 3; /* 3: an install that did not finish */
      snprintf(p->inst_version, sizeof p->inst_version, "%s", ver);
      /* exactly this pack when the team and version match; otherwise this one only counts when it is newer */
      if (!(team[0] ? !strcmp(team, p->ref.team) : true) || strcmp(ver, p->ref.version) != 0) {
        if (tr_version_cmp(p->ref.version, ver) > 0) p->update = true;
        else p->mark = 0;
      }
    } else if (p->kind == 0 && dir_has_patch(p->ref.id)) {
      p->mark = 1; /* a translation folder somebody put there by hand (a mod has no way to be recognised by hand) */
    }
  }
  /* only the newest pack of an ID offers the update */
  for (i = 0; i < g_np; i++)
    if (g_packs[i].update)
      for (j = 0; j < g_np; j++)
        if (j != i && g_packs[j].update && !strcmp(g_packs[j].ref.id, g_packs[i].ref.id) && (g_kind == 0 || !strcmp(g_packs[j].ref.team, g_packs[i].ref.team)) && tr_version_cmp(g_packs[j].ref.version, g_packs[i].ref.version) > 0) { g_packs[i].update = false; g_packs[i].mark = 0; break; }
}

/* ------------------------------------------------------------------------------------------------ prepare */
static void pool_reset(void) { g_pool_n = 0; g_nf = 0; }
static bool pool_add(const char *rel, uint64_t size) {
  size_t l = strlen(rel) + 1;
  if (g_nf >= MAX_FILES) return false;
  if (g_pool_n + l > g_pool_cap) {
    size_t nc = g_pool_cap ? g_pool_cap * 2 : 262144;
    char *np = realloc(g_pool, nc);
    if (!np) return false;
    g_pool = np;
    g_pool_cap = nc;
  }
  memcpy(g_pool + g_pool_n, rel, l);
  g_files[g_nf].off = (uint32_t)g_pool_n;
  g_files[g_nf].size = size;
  g_nf++;
  g_pool_n += l;
  return true;
}

static bool skipped(const char *rel) {
  const char *s = rel;
  for (;;) {
    const char *e = strchr(s, '/');
    char seg[80];
    size_t l = e ? (size_t)(e - s) : strlen(s);
    if (l >= sizeof seg) return false;
    memcpy(seg, s, l);
    seg[l] = '\0';
    if (tr_skip_name(seg)) return true;
    if (!e) return false;
    s = e + 1;
  }
}

typedef struct { const char *prefix; size_t plen; tr_error err; char bad[120]; uint64_t bytes; } collect;
static int on_entry(const dav_entry *e, void *ud) {
  collect *c = ud;
  const char *p = strstr(e->href, c->prefix);
  const char *rel;
  if (e->is_dir) return 0;
  if (!p) return 0;
  rel = p + c->plen;
  if (skipped(rel)) return 0;
  if (!tr_path_ok(rel)) { c->err = TRE_BADPATH; snprintf(c->bad, sizeof c->bad, "%.110s", rel); return 1; }
  c->bytes += e->size;
  if (c->bytes > MAX_BYTES) { c->err = TRE_TOOBIG; return 1; }
  if (!pool_add(rel, e->size)) { c->err = TRE_TOOBIG; return 1; }
  return 0;
}

/* Lists arquivos/ of a pack: one request with Depth: infinity when the server allows it, folder by folder otherwise. */
static bool collect_files(const char *packpath, collect *c) {
  static char dir[420], prefix[430];
  buf b = {0, 0, 0, 8u * 1024u * 1024u, false};
  snprintf(dir, sizeof dir, "%s/arquivos/", packpath);
  snprintf(prefix, sizeof prefix, "%s/arquivos/", packpath);
  memset(c, 0, sizeof *c); /* the byte count starts from zero */
  c->prefix = prefix;
  c->plen = strlen(prefix);
  c->err = TRE_NONE;
  pool_reset();
  if (http("PROPFIND", dir, "infinity", PROPFIND_BODY, sink_buf, &b) && b.p) {
    dav_parse_cb(b.p, b.n, on_entry, c);
    free(b.p);
    return c->err == TRE_NONE;
  }
  free(b.p);
  if (cancelled()) return false;
  /* the server refused Depth: infinity (or the answer was too big): walk the folders */
  {
    static char stack[64][330];
    int sp = 0, n, i;
    pool_reset();
    snprintf(stack[sp++], sizeof stack[0], "%s/arquivos", packpath);
    while (sp > 0 && !cancelled()) {
      static char cur[330];
      snprintf(cur, sizeof cur, "%s", stack[--sp]);
      n = list_dir(cur);
      if (n < 0) return false;
      for (i = 0; i < n; i++) {
        if (g_es[i].is_dir) {
          if (sp < 64 && strlen(cur) + strlen(g_es[i].name) + 2 < sizeof stack[0]) snprintf(stack[sp++], sizeof stack[0], "%s/%s", cur, g_es[i].name);
        } else if (on_entry(&g_es[i], c)) {
          return false;
        }
      }
    }
    return !cancelled() && c->err == TRE_NONE;
  }
}

static int title_known(const char *id_hex) {
  u64 id = strtoull(id_hex, NULL, 16);
  AM_TitleInfo ti;
  bool found;
  if (R_FAILED(amInit())) return 0;
  found = R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &id, &ti)) || R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_GAME_CARD, 1, &id, &ti)) ||
          R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_NAND, 1, &id, &ti));
  amExit();
  return found ? 1 : 2;
}

/* The files of an installed pack, from the record it left. */
static bool load_record_files(const tr_pack *pk) {
  static char path[160];
  char *txt;
  const char *p;
  static char rel[220];
  FILE *f;
  long size;
  record_path_of(pk->ref.id, pk->kind, pk->ref.team, path, sizeof path);
  f = fopen(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); return false; }
  txt = malloc((size_t)size + 1);
  if (!txt) { fclose(f); return false; }
  txt[fread(txt, 1, (size_t)size, f)] = '\0';
  fclose(f);
  pool_reset();
  p = tr_json_array(txt, "arquivos");
  while (p && tr_json_array_next(&p, rel, sizeof rel)) if (tr_path_ok(rel)) pool_add(rel, 0);
  free(txt);
  return g_nf > 0;
}

static int cmp_ptr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The file list of one record on the card (a copy of every string, sorted), or NULL. */
static char **read_list(const char *path, unsigned *n) {
  char *txt, **v = NULL;
  const char *p;
  static char rel[220];
  FILE *f = fopen(path, "rb");
  long size;
  unsigned cap = 0;
  *n = 0;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); return NULL; }
  txt = malloc((size_t)size + 1);
  if (!txt) { fclose(f); return NULL; }
  txt[fread(txt, 1, (size_t)size, f)] = '\0';
  fclose(f);
  p = tr_json_array(txt, "arquivos");
  while (p && tr_json_array_next(&p, rel, sizeof rel)) {
    if (*n >= cap) { char **nv; cap = cap ? cap * 2 : 64; nv = realloc(v, cap * sizeof *v); if (!nv) break; v = nv; }
    v[*n] = strdup(rel);
    if (v[*n]) (*n)++;
  }
  free(txt);
  if (v) qsort(v, *n, sizeof *v, cmp_ptr);
  return v;
}
static void free_list(char **v, unsigned n) { unsigned i; for (i = 0; i < n; i++) free(v[i]); free(v); }

/* Before a mod is installed: how many of its files already exist (they will be backed up) and whether another pack that is
 * installed here wrote some of them (a translation, or another mod). */
static void scan_conflicts(const tr_pack *pk) {
  static char base[96], path[330], own[160];
  unsigned i, replaces = 0, shared = 0, n_own = 0;
  char **own_list;
  uint8_t *exists = calloc(g_nf ? g_nf : 1, 1);
  DIR *d;
  struct dirent *e;
  struct stat sb;
  if (!exists) return;
  snprintf(base, sizeof base, LUMA_TITLES "/%s", pk->ref.id);
  record_path_of(pk->ref.id, pk->kind, pk->ref.team, own, sizeof own);
  own_list = read_list(own, &n_own); /* files of this same mod from an earlier install do not count as conflicts */
  for (i = 0; i < g_nf; i++) {
    char *key = (char *)file_at(i);
    snprintf(path, sizeof path, "%s/%s", base, key);
    if (stat(path, &sb) != 0) continue;
    if (own_list && bsearch(&key, own_list, n_own, sizeof *own_list, cmp_ptr)) continue;
    exists[i] = 1;
    replaces++;
  }
  if (own_list) free_list(own_list, n_own);
  d = replaces ? opendir(base) : NULL;
  while (d && (e = readdir(d)) != NULL) {
    size_t l = strlen(e->d_name);
    unsigned n2 = 0;
    char **list;
    if (strncmp(e->d_name, ".nsd-", 5) != 0 || l < 11 || strcmp(e->d_name + l - 5, ".json") != 0) continue;
    snprintf(path, sizeof path, "%s/%s", base, e->d_name);
    if (!strcmp(path, own)) continue;
    list = read_list(path, &n2);
    if (!list) continue;
    for (i = 0; i < g_nf; i++) {
      char *key = (char *)file_at(i);
      if (exists[i] && bsearch(&key, list, n2, sizeof *list, cmp_ptr)) {
        shared++;
        if (!g_st.shared_with[0]) snprintf(g_st.shared_with, sizeof g_st.shared_with, "%s", strcmp(e->d_name, ".nsd-pack.json") == 0 ? "translation" : e->d_name + 9);
      }
    }
    free_list(list, n2);
  }
  if (d) closedir(d);
  free(exists);
  LightLock_Lock(&g_lock);
  g_st.replaces = replaces;
  g_st.shared = shared;
  touch();
  LightLock_Unlock(&g_lock);
}

/* ---- a pack published as one file: <version>/pacote.zip (stored or deflate, paths relative to /luma/titles/<ID>/) ----
 * It is downloaded first (with resume: the part on the card is continued when the share says the file is the same), then its
 * central directory gives the file list for the confirmation, and the install extracts it. A pack without a zip still uses arquivos/. */
#define ZIP_DIR "sdmc:/3ds/nintendo-dev-agent/downloads"
typedef struct { uint32_t lho, csize, usize, crc; uint16_t method; } zent;
static zent *g_zent;
static bool g_zip_mode;
static char g_zip_path[200], g_zip_tag[208];
static uint64_t g_zl_ms, g_zl_bytes;
static double g_zema;

static void zip_names(const char *packpath) {
  uint64_t h = 1469598103934665603ull;
  const char *p;
  for (p = packpath; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
  snprintf(g_zip_path, sizeof g_zip_path, ZIP_DIR "/pk-%08lx%08lx.zip", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
  snprintf(g_zip_tag, sizeof g_zip_tag, ZIP_DIR "/pk-%08lx%08lx.ztag", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
}

static void zip_progress(uint64_t done) {
  uint64_t t = now_ms();
  LightLock_Lock(&g_lock);
  g_st.zip_done = done;
  if (t - g_zl_ms >= 1000) {
    double inst = (double)(done - g_zl_bytes) * 1000.0 / (double)(t - g_zl_ms);
    g_zema = g_zema > 0 ? 0.8 * g_zema + 0.2 * inst : inst;
    g_st.bps = (unsigned)inst;
    g_st.eta_s = g_zema > 1.0 && g_st.zip_total > done ? (unsigned)((double)(g_st.zip_total - done) / g_zema) : 0;
    g_zl_ms = t;
    g_zl_bytes = done;
    touch();
  } else if (t - g_zl_ms >= 250) {
    touch();
  }
  LightLock_Unlock(&g_lock);
}

typedef struct { FILE *f; uint64_t base, got; bool opened; char *wbuf; } zsink;
static size_t sink_zip(char *ptr, size_t a, size_t b, void *ud) {
  zsink *x = ud;
  size_t n = a * b;
  if (cancelled() || g_resp_status >= 300) return 0;
  if (!x->opened) {
    /* 206: the share continues from where the part ends; 200: it sends the whole file, so the part starts over */
    if (x->base > 0 && g_resp_status == 206) { x->f = fopen(g_zip_path, "r+b"); if (x->f) fseek(x->f, (long)x->base, SEEK_SET); }
    else { x->base = 0; x->f = fopen(g_zip_path, "wb"); }
    if (!x->f) return 0;
    setvbuf(x->f, x->wbuf, _IOFBF, IO_SIZE);
    x->opened = true;
    if (g_resp_etag[0]) { FILE *t = fopen(g_zip_tag, "wb"); if (t) { fputs(g_resp_etag, t); fclose(t); } }
  }
  if (fwrite(ptr, 1, n, x->f) != n) return 0;
  x->got += n;
  zip_progress(x->base + x->got);
  return n;
}
static size_t sink_drop(char *ptr, size_t a, size_t b, void *ud) { (void)ptr; (void)ud; return a * b; }

/* The share lists pacote.zip next to pacote.json? Returns its size. */
static bool zip_detect(const tr_pack *pk, uint64_t *zsize) {
  int n = list_dir(pk->path), i;
  for (i = 0; i < n; i++)
    if (!g_es[i].is_dir && !strcmp(g_es[i].name, "pacote.zip")) { *zsize = g_es[i].size; return true; }
  return false;
}

/* Makes g_zip_path hold the whole pacote.zip. A failure is reported (and the part kept for a resume). */
static bool zip_fetch(const tr_pack *pk, uint64_t zsize) {
  static char url[420], etag[96];
  struct stat sb;
  uint64_t base = 0;
  FS_ArchiveResource r;
  zsink zs;
  bool ok;
  zip_names(pk->path);
  mkdir("sdmc:/3ds", 0777);
  mkdir("sdmc:/3ds/nintendo-dev-agent", 0777);
  mkdir(ZIP_DIR, 0777);
  etag[0] = '\0';
  if (stat(g_zip_path, &sb) == 0) {
    size_t n = 0;
    if (!slurp(g_zip_tag, etag, sizeof etag, &n) || n == 0 || (uint64_t)sb.st_size > zsize) { remove(g_zip_path); remove(g_zip_tag); etag[0] = '\0'; }
    else base = (uint64_t)sb.st_size;
  }
  snprintf(url, sizeof url, "%s/pacote.zip", pk->path);
  if (base == zsize) { /* all of it is there: one tiny request tells whether the share still has this same file */
    snprintf(g_x1, sizeof g_x1, "Range: bytes=0-0");
    snprintf(g_x2, sizeof g_x2, "If-Range: %s", etag);
    g_capture = true;
    ok = http("GET", url, NULL, NULL, sink_drop, NULL);
    if (ok && g_resp_status == 206) { LightLock_Lock(&g_lock); g_st.zip_done = zsize; touch(); LightLock_Unlock(&g_lock); return true; }
    if (cancelled()) { fail(TRE_CANCELLED, ""); return false; }
    if (!ok && g_http_code >= 400 && g_http_code != 416) { net_fail("pacote.zip"); return false; }
    remove(g_zip_path);
    remove(g_zip_tag);
    base = 0;
    etag[0] = '\0';
  }
  if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) && (uint64_t)r.freeClusters * r.clusterSize < (zsize - base) + SPACE_MARGIN) { fail(TRE_SPACE, ""); return false; }
  memset(&zs, 0, sizeof zs);
  zs.base = base;
  zs.wbuf = malloc(IO_SIZE);
  if (!zs.wbuf) { fail(TRE_IO, "memory"); return false; }
  g_zema = 0;
  g_zl_ms = now_ms();
  g_zl_bytes = base;
  LightLock_Lock(&g_lock);
  g_st.zip_total = zsize;
  g_st.zip_done = base;
  g_st.bps = g_st.eta_s = 0;
  touch();
  LightLock_Unlock(&g_lock);
  if (base > 0) {
    snprintf(g_x1, sizeof g_x1, "Range: bytes=%llu-", (unsigned long long)base);
    snprintf(g_x2, sizeof g_x2, "If-Range: %s", etag);
  }
  g_capture = true;
  ok = http("GET", url, NULL, NULL, sink_zip, &zs);
  if (zs.f) fclose(zs.f);
  free(zs.wbuf);
  if (!ok) {
    if (cancelled()) fail(TRE_CANCELLED, "");
    else net_fail("pacote.zip");
    return false; /* what arrived stays on the card: the same pack continues from there */
  }
  if (stat(g_zip_path, &sb) != 0 || (uint64_t)sb.st_size != zsize) { remove(g_zip_path); remove(g_zip_tag); fail(TRE_SIZE, "pacote.zip"); return false; }
  return true;
}

/* Reads the central directory of the downloaded zip into the same list arquivos/ would have given. */
static bool zip_open_index(uint64_t zsize, collect *c) {
  static char nm[320];
  FILE *f = fopen(g_zip_path, "rb");
  uint8_t *tail = NULL, *cd = NULL;
  uint32_t off = 0, size = 0, count = 0;
  size_t tn = zsize < 65557u ? (size_t)zsize : 65557u, pos = 0;
  uint64_t total = 0;
  tzip_ent e;
  int r;
  bool ok = false;
  pool_reset();
  c->err = TRE_ZIP;
  snprintf(c->bad, sizeof c->bad, "%s", "pacote.zip");
  if (!f) { c->err = TRE_IO; return false; }
  tail = malloc(tn);
  if (!tail || fseeko(f, (off_t)(zsize - tn), SEEK_SET) != 0 || fread(tail, 1, tn, f) != tn) goto done;
  if (tzip_eocd(tail, tn, zsize, &off, &size, &count) != 0 || size > 8u * 1024u * 1024u || count > 2u * MAX_FILES) goto done;
  cd = malloc(size ? size : 1);
  if (!cd || fseeko(f, (off_t)off, SEEK_SET) != 0 || (size && fread(cd, 1, size, f) != size)) goto done;
  if (!g_zent) g_zent = calloc(MAX_FILES, sizeof *g_zent);
  if (!g_zent) { c->err = TRE_IO; goto done; }
  while ((r = tzip_next(cd, size, &pos, &e)) == TZIP_ENTRY) {
    const char *rel = nm;
    if (e.name_len == 0 || e.name_len >= sizeof nm) { c->err = TRE_BADPATH; snprintf(c->bad, sizeof c->bad, "%s", "(name)"); goto done; }
    memcpy(nm, e.name, e.name_len);
    nm[e.name_len] = '\0';
    if (nm[e.name_len - 1] == '/') continue;               /* a folder entry */
    if (!strncmp(rel, "arquivos/", 9)) rel += 9;            /* a zip made from the pack's own folder */
    if (!strncmp(rel, "__MACOSX/", 9) || !rel[0] || skipped(rel)) continue;
    if (!tr_path_ok(rel)) { c->err = TRE_BADPATH; snprintf(c->bad, sizeof c->bad, "%.110s", rel); goto done; }
    total += e.usize;
    if (total > MAX_BYTES || !pool_add(rel, e.usize)) { c->err = TRE_TOOBIG; goto done; }
    g_zent[g_nf - 1] = (zent){e.lho, e.csize, e.usize, e.crc, e.method};
  }
  if (r != TZIP_END) goto done;
  {
    /* the same path twice would be written twice */
    char **v = malloc((g_nf ? g_nf : 1) * sizeof *v);
    unsigned i;
    if (!v) { c->err = TRE_IO; goto done; }
    for (i = 0; i < g_nf; i++) v[i] = (char *)file_at(i);
    qsort(v, g_nf, sizeof *v, cmp_ptr);
    for (i = 1; i < g_nf; i++) if (!strcmp(v[i - 1], v[i])) { c->err = TRE_BADPATH; snprintf(c->bad, sizeof c->bad, "%.110s", v[i]); free(v); goto done; }
    free(v);
  }
  c->bytes = total;
  ok = true;
done:
  if (f) fclose(f);
  free(tail);
  free(cd);
  return ok;
}

/* How many folders the install will have to create (creating one is by far the slowest thing the card does here: seconds each, where a
 * file is milliseconds). Counts the folders of the pack that are not on the card yet, measures one real folder creation, and gives a
 * rough time for the whole install. Skipped for small packs. */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void estimate_dirs(const tr_pack *pk, unsigned *new_dirs, unsigned *est_s) {
  static char path[330], lastd[220], missing[220];
  char **v = NULL, **u = NULL;
  unsigned nv = 0, cap = 0, i, nu = 0, count = 0;
  struct stat sb;
  *new_dirs = *est_s = 0;
  lastd[0] = '\x01';
  lastd[1] = '\0';
  for (i = 0; i < g_nf; i++) { /* the folder of each file, and every folder above it */
    const char *rel = file_at(i), *slash = strrchr(rel, '/');
    char d[220];
    size_t l, k;
    if (!slash) continue;
    l = (size_t)(slash - rel);
    if (l >= sizeof d) continue;
    memcpy(d, rel, l);
    d[l] = '\0';
    if (!strcmp(d, lastd)) continue;
    memcpy(lastd, d, l + 1);
    for (k = 0; k <= l; k++) {
      if (k != l && d[k] != '/') continue;
      if (nv + 1 > cap) { char **nvv = realloc(v, (cap ? cap * 2 : 1024) * sizeof *v); if (!nvv) goto out; v = nvv; cap = cap ? cap * 2 : 1024; }
      v[nv] = strndup(d, k);
      if (!v[nv]) goto out;
      nv++;
    }
  }
  if (!nv) goto out;
  qsort(v, nv, sizeof *v, cmp_str);
  u = malloc(nv * sizeof *u);
  if (!u) goto out;
  for (i = 0; i < nv; i++) if (i == 0 || strcmp(v[i], v[i - 1])) u[nu++] = v[i];
  if (nu < 20) goto out; /* a few folders are quick whatever happens */
  missing[0] = '\0';
  for (i = 0; i < nu && !cancelled(); i++) {
    size_t ml = strlen(missing);
    if (ml && !strncmp(u[i], missing, ml) && u[i][ml] == '/') { count++; continue; } /* below a folder that is not there */
    snprintf(path, sizeof path, LUMA_TITLES "/%s/%s", pk->ref.id, u[i]);
    if (stat(path, &sb) != 0) { count++; snprintf(missing, sizeof missing, "%s", u[i]); }
  }
  *new_dirs = count;
  if (count) {
    uint64_t t0, ms;
    t0 = now_ms(); /* one real creation, timed, and taken away again */
    mkdir(LUMA_TITLES "/.nsd-probe", 0777);
    ms = now_ms() - t0;
    rmdir(LUMA_TITLES "/.nsd-probe");
    if (ms > 60000) ms = 60000;
    *est_s = (unsigned)(((uint64_t)count * ms + (uint64_t)g_nf * 80u) / 1000u);
    alog("Translations: %u of %u folders to create, one took %lu ms: about %u s", count, nu, (unsigned long)ms, *est_s);
  }
out:
  for (i = 0; i < nv; i++) free(v[i]);
  free(v);
  free(u);
}

static void job_prepare(void) {
  tr_pack pk;
  collect c;
  static char path[420];
  buf b = {0, 0, 0, 8191, false};
  uint64_t total = 0;
  unsigned i;
  FS_ArchiveResource r;
  if (g_prep_pack < 0 || g_prep_pack >= g_np) { fail(TRE_NOFILES, ""); return; }
  pk = g_packs[g_prep_pack];
  LightLock_Lock(&g_lock);
  g_st.team[0] = g_st.region[0] = g_st.source[0] = '\0';
  g_st.files_total = g_st.files_done = 0;
  g_st.bytes_total = g_st.bytes_done = 0;
  g_st.title_known = 0;
  g_st.removed = 0;
  g_st.status[0] = g_st.shared_with[0] = '\0';
  g_st.replaces = g_st.shared = 0;
  g_st.zip_mode = false;
  g_st.zip_done = g_st.zip_total = 0;
  g_st.fps10 = 0;
  g_st.new_dirs = g_st.est_s = g_st.dirs_done = g_st.dirs_total = 0;
  g_zip_mode = false;
  g_st.has_record = false;
  g_st.error = TRE_NONE;
  g_st.detail[0] = '\0';
  touch();
  LightLock_Unlock(&g_lock);
  /* the credits (optional) */
  snprintf(path, sizeof path, "%s/pacote.json", pk.path);
  if (http("GET", path, NULL, NULL, sink_buf, &b) && b.p) {
    char tmp[200];
    LightLock_Lock(&g_lock);
    if (tr_json_get(b.p, g_kind == 1 ? "autor" : "equipe", tmp, sizeof tmp)) snprintf(g_st.team, sizeof g_st.team, "%s", tmp);
    if (tr_json_get(b.p, "status", tmp, sizeof tmp)) snprintf(g_st.status, sizeof g_st.status, "%s", tmp);
    if (tr_json_get(b.p, "regiao", tmp, sizeof tmp)) snprintf(g_st.region, sizeof g_st.region, "%s", tmp);
    if (tr_json_get(b.p, "fonte", tmp, sizeof tmp)) snprintf(g_st.source, sizeof g_st.source, "%s", tmp);
    touch();
    LightLock_Unlock(&g_lock);
  }
  free(b.p);
  if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
  if (g_prep_uninstall) {
    bool rec = load_record_files(&pk);
    if (!rec && !collect_files(pk.path, &c)) { if (c.err != TRE_NONE) fail(c.err, c.bad); else net_fail("listing the files"); return; }
    LightLock_Lock(&g_lock);
    g_st.has_record = rec;
    g_st.files_total = g_nf;
    touch();
    LightLock_Unlock(&g_lock);
    set_state(TRS_CONFIRM);
    return;
  }
  {
    uint64_t zsize = 0;
    if (zip_detect(&pk, &zsize)) { /* one pacote.zip: download it, then read its index */
      memset(&c, 0, sizeof c);
      if (zsize == 0 || zsize > MAX_BYTES) { fail(TRE_TOOBIG, "pacote.zip"); return; }
      LightLock_Lock(&g_lock);
      g_st.zip_mode = true;
      g_st.zip_total = zsize;
      touch();
      LightLock_Unlock(&g_lock);
      if (!zip_fetch(&pk, zsize)) return;
      LightLock_Lock(&g_lock);
      g_st.zip_total = 0; /* the download is over: the screen goes back to "checking" */
      touch();
      LightLock_Unlock(&g_lock);
      if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
      if (!zip_open_index(zsize, &c)) { fail(c.err, c.bad); return; }
      g_zip_mode = true;
      alog("Translations: pacote.zip %llu KiB, %u files", (unsigned long long)(zsize >> 10), g_nf);
    } else if (!collect_files(pk.path, &c)) { if (c.err != TRE_NONE) fail(c.err, c.bad); else net_fail("listing the files"); return; }
  }
  if (g_nf == 0) { fail(TRE_NOFILES, pk.path); return; }
  for (i = 0; i < g_nf; i++) total += g_files[i].size;
  if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) && (uint64_t)r.freeClusters * r.clusterSize < total + SPACE_MARGIN) { fail(TRE_SPACE, ""); return; }
  if (pk.kind == 1) scan_conflicts(&pk);
  {
    unsigned nd = 0, es = 0;
    estimate_dirs(&pk, &nd, &es);
    if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
    LightLock_Lock(&g_lock);
    g_st.new_dirs = nd;
    g_st.est_s = es;
    LightLock_Unlock(&g_lock);
  }
  LightLock_Lock(&g_lock);
  g_st.files_total = g_nf;
  g_st.bytes_total = total;
  g_st.title_known = title_known(pk.ref.id);
  touch();
  LightLock_Unlock(&g_lock);
  set_state(TRS_CONFIRM);
}

/* ------------------------------------------------------------------------------------------------ install */
static uint64_t g_bytes_base, g_last_ms, g_last_bytes;
static double g_ema;

static void progress_bytes(uint64_t done) {
  uint64_t t = now_ms();
  LightLock_Lock(&g_lock);
  g_st.bytes_done = done;
  if (t - g_last_ms >= 1000) {
    double inst = (double)(done - g_last_bytes) * 1000.0 / (double)(t - g_last_ms);
    g_ema = g_ema > 0 ? 0.8 * g_ema + 0.2 * inst : inst;
    g_st.bps = (unsigned)inst;
    g_st.eta_s = g_ema > 1.0 && g_st.bytes_total > done ? (unsigned)((double)(g_st.bytes_total - done) / g_ema) : 0;
    g_last_ms = t;
    g_last_bytes = done;
    touch();
  } else if (t - g_last_ms >= 250) {
    touch();
  }
  LightLock_Unlock(&g_lock);
}

static void mkdir_p(const char *base, const char *rel_dir) {
  static char path[330];
  const char *s = rel_dir;
  mkdir("sdmc:/luma", 0777);
  mkdir(LUMA_TITLES, 0777);
  mkdir(base, 0777);
  while (*s) {
    const char *e = strchr(s, '/');
    size_t l = e ? (size_t)(e - s) : strlen(s);
    snprintf(path, sizeof path, "%s/%.*s", base, (int)(rel_dir + (e ? (size_t)(e - rel_dir) : strlen(rel_dir)) - rel_dir), rel_dir);
    mkdir(path, 0777);
    if (!e) break;
    s = e + 1;
    (void)l;
  }
}

/* complete = false while the install runs: if the power goes, the list shows an unfinished install, not a good one. */
static bool write_record(const tr_pack *pk, const char *base, bool complete) {
  static char path[320];
  FILE *f;
  unsigned i;
  static char esc[512];
  (void)base;
  record_path_of(pk->ref.id, pk->kind, pk->ref.team, path, sizeof path);
  f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "{\"formato\": 1,\n \"tipo\": \"%s\",\n \"completo\": \"%s\",\n \"id\": \"%s\",\n", pk->kind == 1 ? "mod" : "traducao", complete ? "sim" : "nao", pk->ref.id);
  tr_json_escape(pk->ref.game, esc, sizeof esc);
  fprintf(f, " \"jogo\": \"%s\",\n", esc);
  tr_json_escape(pk->ref.team, esc, sizeof esc);
  fprintf(f, " \"equipe_pasta\": \"%s\",\n", esc);
  tr_json_escape(g_st.team, esc, sizeof esc);
  fprintf(f, " \"equipe\": \"%s\",\n", esc);
  fprintf(f, " \"idioma\": \"%s\",\n \"versao\": \"%s\",\n \"arquivos\": [\n", pk->ref.lang, pk->ref.version);
  for (i = 0; i < g_nf; i++) {
    tr_json_escape(file_at(i), esc, sizeof esc);
    fprintf(f, "  \"%s\"%s\n", esc, i + 1 < g_nf ? "," : "");
  }
  fprintf(f, " ]}\n");
  return fclose(f) == 0;
}

/* An update: the files the previous version wrote that this one no longer has are removed (from the old record, which is
 * still there until write_record replaces it). */
static void remove_stale(const tr_pack *pk, const char *base) {
  static char path[330], rel[220];
  char *txt, **sorted;
  const char *p;
  FILE *f;
  long size;
  unsigned i, removed = 0;
  record_path_of(pk->ref.id, pk->kind, pk->ref.team, path, sizeof path);
  f = fopen(path, "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > 4 * 1024 * 1024) { fclose(f); return; }
  txt = malloc((size_t)size + 1);
  sorted = malloc((g_nf ? g_nf : 1) * sizeof *sorted);
  if (!txt || !sorted) { fclose(f); free(txt); free(sorted); return; }
  txt[fread(txt, 1, (size_t)size, f)] = '\0';
  fclose(f);
  for (i = 0; i < g_nf; i++) sorted[i] = (char *)file_at(i);
  qsort(sorted, g_nf, sizeof *sorted, cmp_ptr);
  p = tr_json_array(txt, "arquivos");
  while (p && tr_json_array_next(&p, rel, sizeof rel)) {
    char *key = rel;
    if (!tr_path_ok(rel) || bsearch(&key, sorted, g_nf, sizeof *sorted, cmp_ptr)) continue;
    snprintf(path, sizeof path, "%s/%s", base, rel);
    if (remove(path) == 0) removed++;
  }
  if (removed) alog("Translations: %u files of the previous version removed", removed);
  free(txt);
  free(sorted);
}

#define PAR 8 /* files at once */
static int g_par = PAR;
static uint64_t g_conc_ms_x;             /* diagnostics: sum of (active transfers x ms) */
static uint64_t g_inst_start;
static uint64_t g_ms_sd, g_ms_net, g_ms_open, g_ms_close, g_ms_ren;     /* diagnostics: time in file operations / in the network calls */
static long g_connects;                 /* diagnostics: new HTTPS connections opened by an install, and the time spent waiting */
static double g_sum_tls, g_sum_first;
#define WBUF (128u * 1024u) /* what is handed to the card in one write: it is very slow with the 16 KiB pieces a download delivers */
typedef struct { CURL *c; FILE *f; char *wbuf; unsigned idx; uint64_t got; bool active; int tries; char tmp[340], dst[330]; } slot;
static size_t sink_slot(char *ptr, size_t a, size_t b, void *ud) {
  slot *x = ud;
  size_t n = a * b;
  if (cancelled()) return 0;
  if (fwrite(ptr, 1, n, x->f) != n) return 0;
  x->got += n;
  return n;
}

/* A mod replaces files that may already be there (the owner's own patch, a translation's file): the original goes to
 * .nsd-backup/<mod>/<same path> first, and comes back on uninstall or if the install fails. Returns 1 backed up, 0 nothing
 * there, -1 could not. */
static int backup_existing(const char *base, const char *team, const char *rel, const char *dst) {
  static char bak[400], sub[260];
  char sl[48];
  const char *slash;
  struct stat sb;
  if (stat(dst, &sb) != 0) return 0;
  slug_of(team, sl, sizeof sl);
  slash = strrchr(rel, '/');
  if (slash) snprintf(sub, sizeof sub, ".nsd-backup/%s/%.*s", sl, (int)(slash - rel), rel);
  else snprintf(sub, sizeof sub, ".nsd-backup/%s", sl);
  mkdir_p(base, sub);
  snprintf(bak, sizeof bak, "%s/.nsd-backup/%s/%s", base, sl, rel);
  remove(bak);
  return rename(dst, bak) == 0 ? 1 : -1;
}

/* An install that failed or was cancelled: what was put in place goes away and every original comes back. */
static void rollback(const tr_pack *pk, const char *base) {
  static char dst[330], bak[400], rec[160];
  char sl[48];
  unsigned i;
  slug_of(pk->ref.team, sl, sizeof sl);
  for (i = 0; i < g_nf; i++) {
    const char *rel = file_at(i);
    snprintf(dst, sizeof dst, "%s/%s", base, rel);
    if (g_fflag[i] & 2) remove(dst);
    if (g_fflag[i] & 1) {
      snprintf(bak, sizeof bak, "%s/.nsd-backup/%s/%s", base, sl, rel);
      remove(dst);
      rename(bak, dst);
    }
  }
  record_path_of(pk->ref.id, pk->kind, pk->ref.team, rec, sizeof rec);
  remove(rec);
  alog("Mods: install rolled back (%s)", pk->ref.team);
}

/* Removes what a mod's record lists and puts back the originals it had backed up. Returns the files removed or restored. */
static unsigned uninstall_by_record(const tr_pack *pk, const char *base) {
  static char path[330], bak[400], rec[160], dirp[330];
  char sl[48], **list;
  unsigned n = 0, i, done = 0;
  record_path_of(pk->ref.id, pk->kind, pk->ref.team, rec, sizeof rec);
  list = read_list(rec, &n);
  slug_of(pk->ref.team, sl, sizeof sl);
  for (i = 0; i < n && !cancelled(); i++) {
    struct stat sb;
    if (!tr_path_ok(list[i])) continue;
    snprintf(path, sizeof path, "%s/%s", base, list[i]);
    snprintf(bak, sizeof bak, "%s/.nsd-backup/%s/%s", base, sl, list[i]);
    if (stat(bak, &sb) == 0) { remove(path); if (rename(bak, path) == 0) done++; }
    else if (remove(path) == 0) done++;
    LightLock_Lock(&g_lock);
    g_st.files_done = i + 1;
    g_st.removed = done;
    touch();
    LightLock_Unlock(&g_lock);
  }
  /* folders left empty (rmdir only works on an empty one), the backup folders too */
  for (i = 0; i < n; i++) {
    char rel[220];
    snprintf(rel, sizeof rel, "%s", list[i]);
    for (;;) {
      char *slash = strrchr(rel, '/');
      if (!slash) break;
      *slash = '\0';
      snprintf(dirp, sizeof dirp, "%s/%s", base, rel);
      rmdir(dirp);
      snprintf(dirp, sizeof dirp, "%s/.nsd-backup/%s/%s", base, sl, rel);
      rmdir(dirp);
    }
  }
  snprintf(dirp, sizeof dirp, "%s/.nsd-backup/%s", base, sl);
  rmdir(dirp);
  snprintf(dirp, sizeof dirp, "%s/.nsd-backup", base);
  rmdir(dirp);
  remove(rec);
  if (list) free_list(list, n);
  return done;
}

/* Installs from the downloaded pacote.zip: each file is read from the zip, inflated and written in one pass, the CRC-32 and the size
 * of every file are checked. The same record, backup and rollback rules as the file-by-file install. */
#define ZBUF (32u * 1024u)
static void install_zip(const tr_pack *pk, const char *base) {
  static char dst[330], tmp[340], dirrel[220], last_dir[220], src_dummy[1];
  uint8_t *in = malloc(ZBUF), *out = malloc(ZBUF);
  char *wbuf = malloc(IO_SIZE);
  FILE *zf = fopen(g_zip_path, "rb");
  unsigned i, done = 0;
  uint64_t bytes = 0, t_a, t_b, t_last = now_ms(), ms_read = 0;
  bool bad = false;
  (void)src_dummy;
  unsigned dirs_done = 0, dirs_total = 0;
  g_ms_open = g_ms_close = g_ms_ren = g_ms_sd = g_ms_net = 0;
  last_dir[0] = '\x01';
  last_dir[1] = '\0';
  {
    /* how many times the folder changes while walking the list: each change may mean creating a folder */
    char prev[220];
    prev[0] = '\x01';
    prev[1] = '\0';
    for (i = 0; i < g_nf; i++) {
      const char *rel = file_at(i), *sl2 = strrchr(rel, '/');
      char cur[220];
      snprintf(cur, sizeof cur, "%.*s", sl2 ? (int)(sl2 - rel) : 0, rel);
      if (strcmp(cur, prev)) { dirs_total++; snprintf(prev, sizeof prev, "%s", cur); }
    }
    LightLock_Lock(&g_lock);
    g_st.dirs_total = dirs_total;
    g_st.dirs_done = 0;
    LightLock_Unlock(&g_lock);
  }
  if (!in || !out || !wbuf || !zf) { fail(TRE_IO, "memory"); bad = true; }
  for (i = 0; i < g_nf && !bad && !cancelled(); i++) {
    const zent *e = &g_zent[i];
    const char *rel = file_at(i), *slash;
    FILE *f;
    uint8_t lh[30];
    size_t hl;
    uint32_t crc = crc32(0L, Z_NULL, 0);
    uint64_t produced = 0, remaining = e->csize;
    bool good = true;
    if (!tr_path_ok(rel)) { fail(TRE_BADPATH, rel); bad = true; break; }
    slash = strrchr(rel, '/');
    snprintf(dirrel, sizeof dirrel, "%.*s", slash ? (int)(slash - rel) : 0, rel);
    if (strcmp(dirrel, last_dir)) { t_a = now_ms(); mkdir_p(base, dirrel); g_ms_sd += now_ms() - t_a; snprintf(last_dir, sizeof last_dir, "%s", dirrel); dirs_done++; }
    snprintf(dst, sizeof dst, "%s/%s", base, rel);
    if (pk->kind == 1) {
      int bk = backup_existing(base, pk->ref.team, rel, dst);
      if (bk < 0) { fail(TRE_IO, rel); bad = true; break; }
      if (bk) g_fflag[i] |= 1;
      snprintf(tmp, sizeof tmp, "%s.nsdtmp", dst);
    } else {
      snprintf(tmp, sizeof tmp, "%s", dst); /* straight to its name: the record says "incomplete" until the end */
    }
    t_b = now_ms();
    f = fopen(tmp, "wb");
    g_ms_open += now_ms() - t_b;
    if (!f) { fail(TRE_IO, rel); bad = true; break; }
    setvbuf(f, wbuf, _IOFBF, IO_SIZE);
    t_a = now_ms();
    if (fseeko(zf, (off_t)e->lho, SEEK_SET) != 0 || fread(lh, 1, 30, zf) != 30 || tzip_local_len(lh, 30, &hl) != 0 || fseeko(zf, (off_t)(e->lho + hl), SEEK_SET) != 0) good = false;
    if (good && e->method == 0) {
      while (remaining && good && !cancelled()) {
        size_t n = remaining < ZBUF ? (size_t)remaining : ZBUF;
        if (fread(in, 1, n, zf) != n) { good = false; break; }
        crc = crc32(crc, in, (uInt)n);
        if (fwrite(in, 1, n, f) != n) { good = false; break; }
        remaining -= n;
        produced += n;
      }
    } else if (good) {
      z_stream z;
      int zr = Z_OK;
      memset(&z, 0, sizeof z);
      if (inflateInit2(&z, -15) != Z_OK) good = false;
      while (good && zr != Z_STREAM_END && !cancelled()) {
        size_t got;
        if (z.avail_in == 0) {
          size_t n = remaining < ZBUF ? (size_t)remaining : ZBUF;
          if (n == 0 || fread(in, 1, n, zf) != n) { good = false; break; }
          z.next_in = in;
          z.avail_in = (uInt)n;
          remaining -= n;
        }
        z.next_out = out;
        z.avail_out = ZBUF;
        zr = inflate(&z, Z_NO_FLUSH);
        if (zr != Z_OK && zr != Z_STREAM_END) { good = false; break; }
        got = ZBUF - z.avail_out;
        if (got) {
          produced += got;
          if (produced > e->usize) { good = false; break; } /* more than the index says: refuse */
          crc = crc32(crc, out, (uInt)got);
          if (fwrite(out, 1, got, f) != got) { good = false; break; }
        }
      }
      if (z.total_in || z.total_out || zr == Z_STREAM_END) inflateEnd(&z);
    }
    ms_read += now_ms() - t_a;
    t_b = now_ms();
    if (fclose(f) != 0) good = false;
    g_ms_close += now_ms() - t_b;
    if (cancelled()) { remove(tmp); break; }
    if (!good || produced != e->usize || crc != e->crc) { remove(tmp); fail(TRE_SIZE, rel); bad = true; break; }
    if (pk->kind == 1) {
      t_b = now_ms();
      remove(dst);
      if (rename(tmp, dst) != 0) { remove(tmp); fail(TRE_IO, rel); bad = true; break; }
      g_ms_ren += now_ms() - t_b;
    }
    g_fflag[i] |= 2;
    bytes += produced;
    done++;
    {
      uint64_t t = now_ms();
      if (t - t_last >= 500) {
        double secs = (double)(t - g_inst_start) / 1000.0;
        double rate = secs > 0 ? (double)done / secs : 0.0;
        /* the time left counts folders and files apart: a folder costs seconds and a file milliseconds, so a plain files-per-second
         * estimate is far off while the folders are being built */
        double dir_s = dirs_done ? (double)g_ms_sd / 1000.0 / (double)dirs_done : 0.0;
        double file_s = done ? (secs - (double)g_ms_sd / 1000.0) / (double)done : 0.0;
        double left = (double)(dirs_total - dirs_done) * dir_s + (double)(g_nf - done) * (file_s > 0 ? file_s : 0.0);
        LightLock_Lock(&g_lock);
        g_st.files_done = done;
        g_st.bytes_done = bytes;
        g_st.dirs_done = dirs_done;
        g_st.fps10 = (unsigned)(rate * 10.0);
        g_st.eta_s = done > 20 ? (unsigned)left : 0;
        touch();
        LightLock_Unlock(&g_lock);
        t_last = t;
      }
    }
  }
  if (zf) fclose(zf);
  free(in);
  free(out);
  free(wbuf);
  {
    unsigned long secs = (unsigned long)((now_ms() - g_inst_start) / 1000);
    alog("Translations: zip, %u of %u files, %llu KiB in %lu s (%.1f files/s)%s", done, g_nf, (unsigned long long)(bytes >> 10), secs,
         secs ? (double)done / (double)secs : 0.0, (bad || cancelled()) ? " - stopped" : "");
    alog("Loop: read+inflate %lu s, folders %lu s, create %lu s, close %lu s, rename %lu s", (unsigned long)(ms_read / 1000), (unsigned long)(g_ms_sd / 1000),
         (unsigned long)(g_ms_open / 1000), (unsigned long)(g_ms_close / 1000), (unsigned long)(g_ms_ren / 1000));
  }
  if (bad) { if (pk->kind == 1) rollback(pk, base); return; }
  if (cancelled()) { if (pk->kind == 1) rollback(pk, base); fail(TRE_CANCELLED, ""); return; } /* the zip stays: installing again needs no new download */
  LightLock_Lock(&g_lock);
  g_st.files_done = done;
  g_st.bytes_done = bytes;
  touch();
  LightLock_Unlock(&g_lock);
  if (!write_record(pk, base, true)) { fail(TRE_IO, "record"); return; }
  remove(g_zip_path);
  remove(g_zip_tag);
  alog("Translations: %s installed", pk->ref.id);
  set_state(TRS_DONE);
}

static void job_install(void) {
  tr_pack pk;
  static char base[96], src[480], dirrel[220];
  bool sleep_ok = aptIsSleepAllowed();
  if (g_prep_pack < 0 || g_prep_pack >= g_np) { fail(TRE_NOFILES, ""); return; }
  pk = g_packs[g_prep_pack];
  snprintf(base, sizeof base, LUMA_TITLES "/%s", pk.ref.id);
  aptSetSleepAllowed(false);
  g_ema = 0;
  g_last_ms = now_ms();
  g_last_bytes = 0;
  LightLock_Lock(&g_lock);
  g_st.files_done = 0;
  g_st.bytes_done = 0;
  g_st.bps = g_st.eta_s = 0;
  touch();
  LightLock_Unlock(&g_lock);
  set_state(TRS_WORKING);
  g_inst_start = now_ms();
  alog("Translations: installing %s v %s (%u files%s)", pk.ref.id, pk.ref.version, g_nf, g_zip_mode ? ", from pacote.zip" : "");
  mkdir_p(base, "");
  { static char mark[120]; snprintf(mark, sizeof mark, "%s/" EMPTY_MARK, base); remove(mark); }
  g_bytes_base = 0;
  memset(g_fflag, 0, g_nf);
  if (pk.kind == 1) uninstall_by_record(&pk, base); /* an update first puts back what the old version had replaced */
  else remove_stale(&pk, base);                     /* files the old version had and this one has not */
  if (!write_record(&pk, base, false)) { fail(TRE_IO, "record"); goto out; }
  if (g_zip_mode) { install_zip(&pk, base); goto out; }
  {
    /* PAR files at once: the share takes a second or more to answer each request, so one at a time was mostly waiting */
    static slot sl[PAR];
    static char raw_last[220];
    unsigned next = 0, done = 0;
    int active = 0, running = 0, k;
    bool bad = false;
    uint64_t t_a, t_b, t_last = now_ms();
    g_conc_ms_x = 0;
    raw_last[0] = '\x01';
    raw_last[1] = '\0';
    memset(sl, 0, sizeof sl);
    g_connects = 0;
    g_sum_tls = g_sum_first = 0;
    g_ms_sd = g_ms_net = g_ms_open = g_ms_close = g_ms_ren = 0;
    for (k = 0; k < g_par; k++) { sl[k].c = curl_easy_init(); sl[k].wbuf = malloc(WBUF); if (!sl[k].c) { fail(TRE_IO, "memory"); goto out; } }
    while ((next < g_nf || active > 0) && !cancelled() && !bad) {
      CURLMsg *msg;
      int left;
      uint64_t live = 0;
      t_a = now_ms();
      for (k = 0; k < g_par && next < g_nf; k++) {
        slot *s2 = &sl[k];
        const char *rel, *slash;
        if (s2->active) continue;
        rel = file_at(next);
        slash = strrchr(rel, '/');
        if (!tr_path_ok(rel)) { fail(TRE_BADPATH, rel); bad = true; break; }
        if (slash) {
          snprintf(dirrel, sizeof dirrel, "%.*s", (int)(slash - rel), rel);
          if (strcmp(dirrel, raw_last)) { mkdir_p(base, dirrel); snprintf(raw_last, sizeof raw_last, "%s", dirrel); } /* only when the folder changes */
        }
        t_b = now_ms();
        snprintf(s2->dst, sizeof s2->dst, "%s/%s", base, rel);
        /* a translation is written straight to its name (the record already says "incomplete" until the end), which saves a rename
         * per file on the card; a mod keeps the temporary name because it may be replacing a file that was backed up */
        if (pk.kind == 0) snprintf(s2->tmp, sizeof s2->tmp, "%s", s2->dst);
        else snprintf(s2->tmp, sizeof s2->tmp, "%s.nsdtmp", s2->dst);
        snprintf(src, sizeof src, "%s/arquivos/%s", pk.path, rel);
        if (pk.kind == 1) {
          int bk = backup_existing(base, pk.ref.team, rel, s2->dst);
          if (bk < 0) { fail(TRE_IO, rel); bad = true; break; }
          if (bk) g_fflag[next] |= 1;
        }
        s2->f = fopen(s2->tmp, "wb");
        if (s2->f && s2->wbuf) setvbuf(s2->f, s2->wbuf, _IOFBF, WBUF);
        g_ms_open += now_ms() - t_b;
        if (!s2->f) { fail(TRE_IO, rel); bad = true; break; }
        s2->got = 0;
        s2->tries = 0;
        s2->idx = next++;
        curl_easy_reset(s2->c);
        apply_opts(s2->c, "GET", src);
        curl_easy_setopt(s2->c, CURLOPT_WRITEFUNCTION, sink_slot);
        curl_easy_setopt(s2->c, CURLOPT_WRITEDATA, s2);
        curl_multi_add_handle(g_mh, s2->c);
        s2->active = true;
        active++;
      }
      g_ms_sd += now_ms() - t_a;
      if (bad) break;
      t_a = now_ms();
      if (curl_multi_perform(g_mh, &running) != CURLM_OK) { g_net_code = CURLE_RECV_ERROR; g_http_code = 0; net_fail("transfer"); bad = true; break; }
      if (running) curl_multi_poll(g_mh, NULL, 0, 50, NULL);
      g_ms_net += now_ms() - t_a;
      g_conc_ms_x += (uint64_t)active * (now_ms() - t_last);
      t_last = now_ms();
      t_a = now_ms();
      while ((msg = curl_multi_info_read(g_mh, &left)) != NULL) {
        slot *d = NULL;
        long http_code = 0;
        if (msg->msg != CURLMSG_DONE) continue;
        for (k = 0; k < g_par; k++) if (sl[k].active && sl[k].c == msg->easy_handle) d = &sl[k];
        if (!d) continue;
        curl_easy_getinfo(d->c, CURLINFO_RESPONSE_CODE, &http_code);
        {
          long nc = 0;
          double tls = 0, first = 0;
          curl_easy_getinfo(d->c, CURLINFO_NUM_CONNECTS, &nc);
          curl_easy_getinfo(d->c, CURLINFO_APPCONNECT_TIME, &tls);
          curl_easy_getinfo(d->c, CURLINFO_STARTTRANSFER_TIME, &first);
          g_connects += nc;
          g_sum_tls += tls;
          g_sum_first += first;
        }
        curl_multi_remove_handle(g_mh, d->c);
        t_b = now_ms();
        fclose(d->f);
        g_ms_close += now_ms() - t_b;
        d->f = NULL;
        d->active = false;
        active--;
        if ((msg->data.result != CURLE_OK || http_code >= 500) && d->tries < 3 && !cancelled()) {
          /* the share answers an overloaded 5xx or drops a connection now and then: that file is fetched again (three times at most) */
          d->tries++;
          alog("Translations: %s failed (curl %d, HTTP %ld), try %d", file_at(d->idx), (int)msg->data.result, http_code, d->tries + 1);
          remove(d->tmp);
          d->got = 0;
          d->f = fopen(d->tmp, "wb");
          if (!d->f) { fail(TRE_IO, file_at(d->idx)); bad = true; break; }
          if (d->wbuf) setvbuf(d->f, d->wbuf, _IOFBF, WBUF);
          snprintf(src, sizeof src, "%s/arquivos/%s", pk.path, file_at(d->idx));
          curl_easy_reset(d->c);
          apply_opts(d->c, "GET", src);
          curl_easy_setopt(d->c, CURLOPT_WRITEFUNCTION, sink_slot);
          curl_easy_setopt(d->c, CURLOPT_WRITEDATA, d);
          curl_multi_add_handle(g_mh, d->c);
          d->active = true;
          active++;
          continue;
        }
        if (msg->data.result != CURLE_OK || http_code < 200 || http_code >= 400) {
          g_net_code = (int)msg->data.result;
          g_http_code = http_code;
          remove(d->tmp);
          net_fail(file_at(d->idx));
          bad = true;
          break;
        }
        if (g_files[d->idx].size && d->got != g_files[d->idx].size) { remove(d->tmp); fail(TRE_SIZE, file_at(d->idx)); bad = true; break; }
        if (strcmp(d->tmp, d->dst)) {
          t_b = now_ms();
          remove(d->dst);
          if (rename(d->tmp, d->dst) != 0) { remove(d->tmp); fail(TRE_IO, file_at(d->idx)); bad = true; break; }
          g_ms_ren += now_ms() - t_b;
        }
        g_fflag[d->idx] |= 2;
        g_bytes_base += d->got;
        done++;
      }
      g_ms_sd += now_ms() - t_a;
      for (k = 0; k < g_par; k++) if (sl[k].active) live += sl[k].got;
      LightLock_Lock(&g_lock);
      g_st.files_done = done;
      LightLock_Unlock(&g_lock);
      progress_bytes(g_bytes_base + live);
    }
    /* stop whatever is still in flight (an error, or the user cancelled) */
    for (k = 0; k < g_par; k++) {
      if (sl[k].active) { curl_multi_remove_handle(g_mh, sl[k].c); if (sl[k].f) fclose(sl[k].f); remove(sl[k].tmp); sl[k].active = false; }
      if (sl[k].c) curl_easy_cleanup(sl[k].c);
      free(sl[k].wbuf);
    }
    /* the timings go to the log whether the install finished, failed or was cancelled (a cancelled one says how far it got) */
    {
      unsigned long secs = (unsigned long)((now_ms() - g_inst_start) / 1000);
      alog("Translations: %u of %u files, %llu KiB in %lu s (%.1f files/s)%s, %ld new connections", done, g_nf, (unsigned long long)(g_bytes_base >> 10), secs,
           secs ? (double)done / (double)secs : 0.0, (bad || cancelled()) ? " - stopped" : "", g_connects);
      alog("Waits summed: TLS %.0f s, first byte %.0f s; slots %d, average in flight %.1f", g_sum_tls, g_sum_first, g_par,
           (now_ms() - g_inst_start) ? (double)g_conc_ms_x / (double)(now_ms() - g_inst_start) : 0.0);
      alog("Loop: network %lu s, card %lu s (create %lu, close %lu, rename %lu)", (unsigned long)(g_ms_net / 1000), (unsigned long)(g_ms_sd / 1000),
           (unsigned long)(g_ms_open / 1000), (unsigned long)(g_ms_close / 1000), (unsigned long)(g_ms_ren / 1000));
    }
    if (bad) { if (pk.kind == 1) rollback(&pk, base); goto out; }
    LightLock_Lock(&g_lock);
    g_st.files_done = done;
    g_st.bytes_done = g_bytes_base;
    touch();
    LightLock_Unlock(&g_lock);
  }
  if (cancelled()) { if (pk.kind == 1) rollback(&pk, base); fail(TRE_CANCELLED, ""); goto out; }
  if (!write_record(&pk, base, true)) { fail(TRE_IO, "record"); goto out; }
  alog("Translations: %s installed", pk.ref.id);
  set_state(TRS_DONE);
out:
  aptSetSleepAllowed(sleep_ok);
}

/* ------------------------------------------------------------------------------------------------ uninstall */

static void job_uninstall(void) {
  tr_pack pk;
  static char base[96], path[330];
  unsigned i, removed = 0;
  char **dirs;
  unsigned nd = 0;
  uint64_t t_start = now_ms();
  if (g_prep_pack < 0 || g_prep_pack >= g_np) { fail(TRE_NOFILES, ""); return; }
  pk = g_packs[g_prep_pack];
  snprintf(base, sizeof base, LUMA_TITLES "/%s", pk.ref.id);
  set_state(TRS_WORKING);
  if (pk.kind == 1) { /* a mod: its own files go and the originals it replaced come back */
    removed = uninstall_by_record(&pk, base);
    alog("Mods: %s uninstalled (%u files removed or restored)", pk.ref.team, removed);
    if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
    set_state(TRS_DONE);
    return;
  }
  dirs = calloc(g_nf ? g_nf : 1, sizeof *dirs);
  for (i = 0; i < g_nf && !cancelled(); i++) {
    const char *rel = file_at(i);
    const char *slash;
    if (!tr_path_ok(rel)) continue;
    snprintf(path, sizeof path, "%s/%s", base, rel);
    if (remove(path) == 0) removed++;
    slash = strrchr(rel, '/');
    if (slash && dirs && nd < g_nf) {
      char *d = strndup(rel, (size_t)(slash - rel));
      if (d) dirs[nd++] = d;
    }
    LightLock_Lock(&g_lock);
    g_st.files_done = i + 1;
    g_st.removed = removed;
    touch();
    LightLock_Unlock(&g_lock);
  }
  /* The folders stay: creating a folder takes seconds on this hardware (removing the files takes milliseconds), and an empty folder
   * does nothing for Luma, so a reinstall or an update of the same pack does not have to build the tree again. */
  if (dirs) {
    unsigned k;
    for (k = 0; k < nd; k++) free(dirs[k]);
    free(dirs);
  }
  record_path_of(pk.ref.id, pk.kind, pk.ref.team, path, sizeof path);
  remove(path);
  rmdir(base); /* only works when the folder is empty (a pack with no sub-folders) */
  {
    struct stat sb;
    if (stat(base, &sb) == 0) {
      FILE *m;
      snprintf(path, sizeof path, "%s/" EMPTY_MARK, base);
      m = fopen(path, "wb");
      if (m) { fputs("NSD Bridge: the pack was uninstalled; its empty folders were kept so installing it again is fast.\n", m); fclose(m); }
    }
  }
  alog("Translations: %s uninstalled (%u files removed in %lu s)", pk.ref.id, removed, (unsigned long)((now_ms() - t_start) / 1000));
  if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
  set_state(TRS_DONE);
}

/* ------------------------------------------------------------------------------------------------ the worker */
enum { J_LIST, J_PREPARE, J_RUN, J_CHECK };
static int g_job;

static void run(void *arg) {
  (void)arg;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { fail(TRE_NET, "curl"); goto done; }
  g_c = curl_easy_init();
  g_mh = curl_multi_init();
  g_es = malloc(ES_MAX * sizeof *g_es);
  if (!g_c || !g_mh || !g_es) { fail(TRE_IO, "memory"); goto cleanup; }
  if (g_job == J_CHECK) {
    char now[sizeof g_etag];
    bool ok = fetch_etag(now, sizeof now);
    LightLock_Lock(&g_lock);
    g_st.offline = !ok;
    g_st.net_code = g_net_code;
    g_st.http_code = g_http_code;
    touch();
    LightLock_Unlock(&g_lock);
    if (!ok) alog("Translations: the share did not answer the quick check (curl %d, HTTP %ld)", g_net_code, g_http_code);
    if (!ok || (g_etag[0] && !strcmp(now, g_etag))) goto cleanup; /* unreachable or unchanged: keep the saved list */
    LightLock_Lock(&g_lock);
    g_st.list_requests = 0;
    g_st.error = TRE_NONE;
    g_st.offline = false;
    g_st.state = TRS_LISTING;
    touch();
    LightLock_Unlock(&g_lock);
    snprintf(g_etag, sizeof g_etag, "%s", now);
    job_list();
  } else if (g_job == J_LIST) {
    if (!fetch_etag(g_etag, sizeof g_etag)) g_etag[0] = '\0';
    job_list();
  }
  else if (g_job == J_PREPARE) job_prepare();
  else if (g_prep_uninstall) job_uninstall();
  else job_install();
cleanup:
  if (g_c) curl_easy_cleanup(g_c);
  g_c = NULL;
  if (g_mh) curl_multi_cleanup(g_mh);
  g_mh = NULL;
  free(g_es);
  g_es = NULL;
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

void tr_init(void) {
  int k;
  if (g_init) return;
  LightLock_Init(&g_lock);
  K[0].root_name = "traducoes";
  K[0].cfg_path = "sdmc:/3ds/nintendo-dev-agent/config/translations-url";
  K[0].cache_path = CACHE_DIR "/translations.txt";
  K[0].default_url = NSD_DEFAULT_TRANSLATIONS_URL;
  K[1].root_name = "mods";
  K[1].cfg_path = "sdmc:/3ds/nintendo-dev-agent/config/mods-url";
  K[1].cache_path = CACHE_DIR "/mods.txt";
  K[1].default_url = NSD_DEFAULT_MODS_URL;
  g_files = calloc(MAX_FILES, sizeof *g_files);
  g_fflag = calloc(MAX_FILES, 1);
  g_init = g_files != NULL && g_fflag != NULL;
  for (k = 0; k < 2; k++) {
    g_kind = k;
    K[k].packs = calloc(MAX_PACKS, sizeof *K[k].packs);
    if (!K[k].packs) g_init = false;
    load_config();
  }
  g_kind = 0;
}

/* Which catalog the calls below act on (translations or mods). Not while a job is running. */
void tr_select(int kind) {
  if (kind < 0 || kind > 1 || tr_busy() || kind == g_kind) return;
  g_kind = kind;
  LightLock_Lock(&g_lock);
  g_st.state = K[kind].np > 0 ? TRS_READY : TRS_IDLE;
  g_st.error = TRE_NONE;
  touch();
  LightLock_Unlock(&g_lock);
}
int tr_kind(void) { return g_kind; }

void tr_shutdown(void) {
  if (!g_init) return;
  __atomic_store_n(&g_cancel, true, __ATOMIC_RELEASE);
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  for (g_kind = 0; g_kind < 2; g_kind++) { free(g_packs); g_packs = NULL; g_np = 0; }
  g_kind = 0;
  free(g_zent);
  g_zent = NULL;
  free(g_files);
  free(g_fflag);
  free(g_pool);
  g_fflag = NULL;
  g_files = NULL;
  g_pool = NULL;
  g_init = false;
}

bool tr_busy(void) { return __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE); }
void tr_get(tr_status *out) {
  if (!g_init) { memset(out, 0, sizeof *out); return; }
  LightLock_Lock(&g_lock);
  *out = g_st;
  LightLock_Unlock(&g_lock);
}
int tr_count(void) { return g_np; }
const tr_pack *tr_at(int i) { return i >= 0 && i < g_np ? &g_packs[i] : NULL; }

void tr_list(bool force) {
  if (!g_init || !g_cfg || tr_busy()) return;
  if (!force && load_cache()) {
    tr_refresh_marks();
    set_state(TRS_READY);
    start(J_CHECK);
    return;
  }
  LightLock_Lock(&g_lock);
  g_st.list_requests = 0;
  g_st.error = TRE_NONE;
  g_st.offline = false;
  g_st.state = TRS_LISTING;
  touch();
  LightLock_Unlock(&g_lock);
  if (!start(J_LIST)) fail(TRE_IO, "thread");
}

void tr_prepare(int idx, bool uninstall) {
  if (!g_init || tr_busy() || idx < 0 || idx >= g_np) return;
  g_prep_pack = idx;
  g_prep_uninstall = uninstall;
  LightLock_Lock(&g_lock);
  g_st.job = uninstall ? 1 : 0;
  g_st.pack = idx;
  g_st.state = TRS_PREPARING;
  g_st.error = TRE_NONE;
  touch();
  LightLock_Unlock(&g_lock);
  if (!start(J_PREPARE)) fail(TRE_IO, "thread");
}

void tr_confirm(bool yes) {
  tr_status s;
  tr_get(&s);
  if (s.state != TRS_CONFIRM) return;
  if (!yes) { set_state(TRS_READY); return; }
  g_confirmed = true;
  if (!start(J_RUN)) fail(TRE_IO, "thread");
}

void tr_cancel(void) { __atomic_store_n(&g_cancel, true, __ATOMIC_RELEASE); }

void tr_dismiss(void) {
  tr_status s;
  tr_get(&s);
  if (s.state == TRS_DONE || s.state == TRS_ERROR || s.state == TRS_CONFIRM) {
    if (s.state == TRS_DONE) tr_refresh_marks();
    set_state(g_np > 0 ? TRS_READY : TRS_IDLE);
  }
}
#endif
