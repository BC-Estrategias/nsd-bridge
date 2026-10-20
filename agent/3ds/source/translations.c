#ifndef AGENT_TEXT_UI
#include "translations.h"

/* Copies into fixed buffers are bounded on purpose: a too-long name is cut, never overrun. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <3ds.h>
#include <curl/curl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alog.h"
#include "ndp/ndp_defs.h"

#define CONFIG_PATH "sdmc:/3ds/nintendo-dev-agent/config/translations-url"
#define CACHE_DIR "sdmc:/3ds/nintendo-dev-agent/cache"
#define CACHE_PATH CACHE_DIR "/translations.txt"
#define MAX_PACKS 512
#define MAX_FILES 20000u
#define MAX_BYTES (1536ull * 1024ull * 1024ull)
#define SPACE_MARGIN (16ull * 1024ull * 1024ull)
#define ROOT "traducoes"
#define IO_SIZE (128u * 1024u)

extern const char nsd_ca_pem[], nsd_ca_pem_end[]; /* the Mozilla CA bundle, linked in by updater.c */

static LightLock g_lock;
static Thread g_worker;
#ifndef NSD_DEFAULT_TRANSLATIONS_URL
#define NSD_DEFAULT_TRANSLATIONS_URL ""
#endif
static bool g_init, g_busy, g_link_default;
/* The share either contains a traducoes/ folder or IS that folder: g_root is "traducoes" or "" (paths are always from the share's root). */
static char g_root[16];
static tr_status g_st;
static tr_pack *g_packs;
static int g_np;
static char g_link[300], g_base[200], g_token[80];
static bool g_cfg;
static volatile bool g_cancel, g_answered, g_confirmed;

/* what tr_prepare gathered: the files of the pack, as paths relative to arquivos/ */
typedef struct { uint32_t off; uint64_t size; } fent;
static char *g_pool;
static size_t g_pool_n, g_pool_cap;
static fent *g_files;
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
  if (NSD_DEFAULT_TRANSLATIONS_URL[0] && tr_parse_share(NSD_DEFAULT_TRANSLATIONS_URL, g_base, sizeof g_base, g_token, sizeof g_token)) {
    snprintf(g_link, sizeof g_link, "%s", NSD_DEFAULT_TRANSLATIONS_URL);
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
}

/* One request on the share. True when curl succeeded and the server answered below 400. All requests go through the one
 * multi handle of the job (g_mh), so the HTTPS connection stays open between them: a new handshake per file is what made the
 * first version take seconds for each tiny file. */
static CURLM *g_mh;
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

static void save_cache(void) {
  FILE *f;
  int i;
  mkdir("sdmc:/3ds/nintendo-dev-agent/cache", 0777);
  f = fopen(CACHE_PATH, "wb");
  if (!f) return;
  fprintf(f, "# %s\n@ %s\n", g_link, g_root);
  for (i = 0; i < g_np; i++) fprintf(f, "%s\t%s - %s\t%s\t%s\n", g_packs[i].ref.lang, g_packs[i].ref.id, g_packs[i].ref.game, g_packs[i].ref.team, g_packs[i].ref.version);
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
  while (ok && fgets(line, sizeof line, f)) {
    char *lang = line, *idn, *team, *ver, *e;
    tr_ref r;
    static char path[320];
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
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
    if (e) *e = '\0';
    if (!tr_parse_ref(lang, idn, team, ver, &r)) continue;
    if (g_root[0]) snprintf(path, sizeof path, "%s/%s/%s/%s/%s", g_root, lang, idn, team, ver);
    else snprintf(path, sizeof path, "%s/%s/%s/%s", lang, idn, team, ver);
    add_pack(&r, path);
  }
  fclose(f);
  if (!ok) g_np = 0;
  return ok && g_np > 0;
}

static void job_list(void) {
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
  save_cache();
  alog("Translations: %d packs listed (%d requests)", g_np, g_st.list_requests);
  set_state(TRS_READY);
}

/* ------------------------------------------------------------------------------------------------ marks */
#define LUMA_TITLES "sdmc:/luma/titles"

static bool read_record(const char *id, char *ver, size_t vcap, char *team, size_t tcap) {
  static char path[96], head[1024];
  snprintf(path, sizeof path, LUMA_TITLES "/%s/.nsd-pack.json", id);
  if (!slurp(path, head, sizeof head, NULL)) return false;
  if (ver && !tr_json_get(head, "versao", ver, vcap)) ver[0] = '\0';
  if (team && !tr_json_get(head, "equipe_pasta", team, tcap)) team[0] = '\0';
  return true;
}

static bool dir_has_patch(const char *id) {
  static char path[110];
  struct stat sb;
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
  for (i = 0; i < g_np; i++) {
    tr_pack *p = &g_packs[i];
    p->mark = 0;
    p->update = false;
    p->inst_version[0] = '\0';
    if (read_record(p->ref.id, ver, sizeof ver, team, sizeof team)) {
      p->mark = 2;
      snprintf(p->inst_version, sizeof p->inst_version, "%s", ver);
      /* exactly this pack when the team and version match; otherwise this one only counts when it is newer */
      if (!(team[0] ? !strcmp(team, p->ref.team) : true) || strcmp(ver, p->ref.version) != 0) {
        if (tr_version_cmp(p->ref.version, ver) > 0) p->update = true;
        else p->mark = 0;
      }
    } else if (dir_has_patch(p->ref.id)) {
      p->mark = 1;
    }
  }
  /* only the newest pack of an ID offers the update */
  for (i = 0; i < g_np; i++)
    if (g_packs[i].update)
      for (j = 0; j < g_np; j++)
        if (j != i && g_packs[j].update && !strcmp(g_packs[j].ref.id, g_packs[i].ref.id) && tr_version_cmp(g_packs[j].ref.version, g_packs[i].ref.version) > 0) { g_packs[i].update = false; g_packs[i].mark = 0; break; }
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
static bool load_record_files(const char *id) {
  static char path[96];
  char *txt;
  const char *p;
  static char rel[220];
  FILE *f;
  long size;
  snprintf(path, sizeof path, LUMA_TITLES "/%s/.nsd-pack.json", id);
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
    if (tr_json_get(b.p, "equipe", tmp, sizeof tmp)) snprintf(g_st.team, sizeof g_st.team, "%s", tmp);
    if (tr_json_get(b.p, "regiao", tmp, sizeof tmp)) snprintf(g_st.region, sizeof g_st.region, "%s", tmp);
    if (tr_json_get(b.p, "fonte", tmp, sizeof tmp)) snprintf(g_st.source, sizeof g_st.source, "%s", tmp);
    touch();
    LightLock_Unlock(&g_lock);
  }
  free(b.p);
  if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
  if (g_prep_uninstall) {
    bool rec = load_record_files(pk.ref.id);
    if (!rec && !collect_files(pk.path, &c)) { if (c.err != TRE_NONE) fail(c.err, c.bad); else net_fail("listing the files"); return; }
    LightLock_Lock(&g_lock);
    g_st.has_record = rec;
    g_st.files_total = g_nf;
    touch();
    LightLock_Unlock(&g_lock);
    set_state(TRS_CONFIRM);
    return;
  }
  if (!collect_files(pk.path, &c)) { if (c.err != TRE_NONE) fail(c.err, c.bad); else net_fail("listing the files"); return; }
  if (g_nf == 0) { fail(TRE_NOFILES, pk.path); return; }
  for (i = 0; i < g_nf; i++) total += g_files[i].size;
  if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) && (uint64_t)r.freeClusters * r.clusterSize < total + SPACE_MARGIN) { fail(TRE_SPACE, ""); return; }
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

static bool write_record(const tr_pack *pk, const char *base) {
  static char path[320];
  FILE *f;
  unsigned i;
  static char esc[512];
  snprintf(path, sizeof path, "%s/.nsd-pack.json", base);
  f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "{\"formato\": 1,\n \"id\": \"%s\",\n", pk->ref.id);
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

static int cmp_ptr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* An update: the files the previous version wrote that this one no longer has are removed (from the old record, which is
 * still there until write_record replaces it). */
static void remove_stale(const char *base) {
  static char path[330], rel[220];
  char *txt, **sorted;
  const char *p;
  FILE *f;
  long size;
  unsigned i, removed = 0;
  snprintf(path, sizeof path, "%s/.nsd-pack.json", base);
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

#define PAR 6
static uint64_t g_inst_start;
static uint64_t g_ms_sd, g_ms_net, g_ms_open, g_ms_close, g_ms_ren;     /* diagnostics: time in file operations / in the network calls */
static long g_connects;                 /* diagnostics: new HTTPS connections opened by an install, and the time spent waiting */
static double g_sum_tls, g_sum_first;
typedef struct { CURL *c; FILE *f; unsigned idx; uint64_t got; bool active; char tmp[340], dst[330]; } slot;
static size_t sink_slot(char *ptr, size_t a, size_t b, void *ud) {
  slot *x = ud;
  size_t n = a * b;
  if (cancelled()) return 0;
  if (fwrite(ptr, 1, n, x->f) != n) return 0;
  x->got += n;
  return n;
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
  alog("Translations: installing %s v %s (%u files)", pk.ref.id, pk.ref.version, g_nf);
  mkdir_p(base, "");
  g_bytes_base = 0;
  {
    /* PAR files at once: the share takes a second or more to answer each request, so one at a time was mostly waiting */
    static slot sl[PAR];
    unsigned next = 0, done = 0;
    int active = 0, running = 0, k;
    bool bad = false;
    uint64_t t_a, t_b;
    memset(sl, 0, sizeof sl);
    g_connects = 0;
    g_sum_tls = g_sum_first = 0;
    g_ms_sd = g_ms_net = g_ms_open = g_ms_close = g_ms_ren = 0;
    for (k = 0; k < PAR; k++) { sl[k].c = curl_easy_init(); if (!sl[k].c) { fail(TRE_IO, "memory"); goto out; } }
    while ((next < g_nf || active > 0) && !cancelled() && !bad) {
      CURLMsg *msg;
      int left;
      uint64_t live = 0;
      t_a = now_ms();
      for (k = 0; k < PAR && next < g_nf; k++) {
        slot *s2 = &sl[k];
        const char *rel, *slash;
        if (s2->active) continue;
        rel = file_at(next);
        slash = strrchr(rel, '/');
        if (!tr_path_ok(rel)) { fail(TRE_BADPATH, rel); bad = true; break; }
        if (slash) { snprintf(dirrel, sizeof dirrel, "%.*s", (int)(slash - rel), rel); mkdir_p(base, dirrel); }
        t_b = now_ms();
        snprintf(s2->dst, sizeof s2->dst, "%s/%s", base, rel);
        snprintf(s2->tmp, sizeof s2->tmp, "%s.nsdtmp", s2->dst);
        snprintf(src, sizeof src, "%s/arquivos/%s", pk.path, rel);
        s2->f = fopen(s2->tmp, "wb");
        g_ms_open += now_ms() - t_b;
        if (!s2->f) { fail(TRE_IO, rel); bad = true; break; }
        s2->got = 0;
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
      t_a = now_ms();
      while ((msg = curl_multi_info_read(g_mh, &left)) != NULL) {
        slot *d = NULL;
        long http_code = 0;
        if (msg->msg != CURLMSG_DONE) continue;
        for (k = 0; k < PAR; k++) if (sl[k].active && sl[k].c == msg->easy_handle) d = &sl[k];
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
        if (msg->data.result != CURLE_OK || http_code < 200 || http_code >= 400) {
          g_net_code = (int)msg->data.result;
          g_http_code = http_code;
          remove(d->tmp);
          net_fail(file_at(d->idx));
          bad = true;
          break;
        }
        if (g_files[d->idx].size && d->got != g_files[d->idx].size) { remove(d->tmp); fail(TRE_SIZE, file_at(d->idx)); bad = true; break; }
        t_b = now_ms();
        remove(d->dst);
        if (rename(d->tmp, d->dst) != 0) { remove(d->tmp); fail(TRE_IO, file_at(d->idx)); bad = true; break; }
        g_ms_ren += now_ms() - t_b;
        g_bytes_base += d->got;
        done++;
      }
      g_ms_sd += now_ms() - t_a;
      for (k = 0; k < PAR; k++) if (sl[k].active) live += sl[k].got;
      LightLock_Lock(&g_lock);
      g_st.files_done = done;
      LightLock_Unlock(&g_lock);
      progress_bytes(g_bytes_base + live);
    }
    /* stop whatever is still in flight (an error, or the user cancelled) */
    for (k = 0; k < PAR; k++) {
      if (sl[k].active) { curl_multi_remove_handle(g_mh, sl[k].c); if (sl[k].f) fclose(sl[k].f); remove(sl[k].tmp); sl[k].active = false; }
      if (sl[k].c) curl_easy_cleanup(sl[k].c);
    }
    if (bad) goto out;
    LightLock_Lock(&g_lock);
    g_st.files_done = done;
    g_st.bytes_done = g_bytes_base;
    touch();
    LightLock_Unlock(&g_lock);
  }
  if (cancelled()) { fail(TRE_CANCELLED, ""); goto out; }
  alog("Translations: %u files, %llu KiB in %lu s, %ld new connections", g_nf, (unsigned long long)(g_bytes_base >> 10),
       (unsigned long)((now_ms() - g_inst_start) / 1000), g_connects);
  alog("Waits summed: TLS %.0f s, first byte %.0f s", g_sum_tls, g_sum_first);
  alog("Loop: network %lu s, card %lu s (create %lu, close %lu, rename %lu)", (unsigned long)(g_ms_net / 1000), (unsigned long)(g_ms_sd / 1000),
       (unsigned long)(g_ms_open / 1000), (unsigned long)(g_ms_close / 1000), (unsigned long)(g_ms_ren / 1000));
  remove_stale(base);
  if (!write_record(&pk, base)) { fail(TRE_IO, ".nsd-pack.json"); goto out; }
  alog("Translations: %s installed", pk.ref.id);
  set_state(TRS_DONE);
out:
  aptSetSleepAllowed(sleep_ok);
}

/* ------------------------------------------------------------------------------------------------ uninstall */
static int cmp_len_desc(const void *a, const void *b) { return (int)strlen(*(char *const *)b) - (int)strlen(*(char *const *)a); }

static void job_uninstall(void) {
  tr_pack pk;
  static char base[96], path[330];
  unsigned i, removed = 0;
  char **dirs;
  unsigned nd = 0;
  if (g_prep_pack < 0 || g_prep_pack >= g_np) { fail(TRE_NOFILES, ""); return; }
  pk = g_packs[g_prep_pack];
  snprintf(base, sizeof base, LUMA_TITLES "/%s", pk.ref.id);
  set_state(TRS_WORKING);
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
  /* folders the pack leaves empty, the deepest first (rmdir only succeeds on an empty one) */
  if (dirs) {
    unsigned k;
    qsort(dirs, nd, sizeof *dirs, cmp_len_desc);
    for (k = 0; k < nd; k++) {
      char *d = dirs[k];
      for (;;) {
        char *s;
        snprintf(path, sizeof path, "%s/%s", base, d);
        if (rmdir(path) != 0) break;
        s = strrchr(d, '/');
        if (!s) break;
        *s = '\0';
      }
      free(d);
    }
    free(dirs);
  }
  snprintf(path, sizeof path, "%s/.nsd-pack.json", base);
  remove(path);
  rmdir(base); /* only if nothing else is in it */
  alog("Translations: %s uninstalled (%u files removed)", pk.ref.id, removed);
  if (cancelled()) { fail(TRE_CANCELLED, ""); return; }
  set_state(TRS_DONE);
}

/* ------------------------------------------------------------------------------------------------ the worker */
enum { J_LIST, J_PREPARE, J_RUN };
static int g_job;

static void run(void *arg) {
  (void)arg;
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { fail(TRE_NET, "curl"); goto done; }
  g_c = curl_easy_init();
  g_mh = curl_multi_init();
  g_es = malloc(ES_MAX * sizeof *g_es);
  if (!g_c || !g_mh || !g_es) { fail(TRE_IO, "memory"); goto cleanup; }
  if (g_job == J_LIST) job_list();
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
  if (g_init) return;
  LightLock_Init(&g_lock);
  g_packs = calloc(MAX_PACKS, sizeof *g_packs);
  g_files = calloc(MAX_FILES, sizeof *g_files);
  g_init = g_packs && g_files;
  load_config();
}

void tr_shutdown(void) {
  if (!g_init) return;
  __atomic_store_n(&g_cancel, true, __ATOMIC_RELEASE);
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  free(g_packs);
  free(g_files);
  free(g_pool);
  g_packs = NULL;
  g_files = NULL;
  g_pool = NULL;
  g_np = 0;
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
    return;
  }
  LightLock_Lock(&g_lock);
  g_st.list_requests = 0;
  g_st.error = TRE_NONE;
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
