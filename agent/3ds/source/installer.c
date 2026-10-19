#ifndef AGENT_TEXT_UI
#include "installer.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alog.h"
#include "ndp/ndp_defs.h"
#include "ndp/ndp_sha256.h"
#include "updater.h"

#define OWN_TITLE 0x000400000BD00100ull /* this app: it updates itself through the Updates menu, not through here */
#define IO_SIZE (128u * 1024u)
#define SPACE_MARGIN (8u * 1024u * 1024u)

extern const char nsd_ca_pem[], nsd_ca_pem_end[]; /* the Mozilla CA bundle, linked in by updater.c */

static LightLock g_lock;
static ins_status g_st;
static Thread g_worker;
static bool g_init, g_busy;
static volatile bool g_cancel, g_answered, g_confirmed;
static char g_src[1100];
static bool g_is_url, g_have_expect;
static uint8_t g_expect[32];

static bool cancelled(void) { return __atomic_load_n(&g_cancel, __ATOMIC_ACQUIRE) || aptShouldClose(); }

static void touch(void) { g_st.revision++; }
static void set_state(ins_state s) { LightLock_Lock(&g_lock); g_st.state = s; touch(); LightLock_Unlock(&g_lock); }
static void fail(ins_error e, uint32_t am) {
  LightLock_Lock(&g_lock);
  g_st.state = INS_ERROR;
  g_st.error = e;
  g_st.am_result = am;
  touch();
  LightLock_Unlock(&g_lock);
}
static void progress(uint64_t done, uint64_t total, unsigned bps) {
  LightLock_Lock(&g_lock);
  g_st.done = done;
  g_st.total = total;
  g_st.bps = bps;
  touch();
  LightLock_Unlock(&g_lock);
}

void installer_get(ins_status *out) {
  if (!g_init) { memset(out, 0, sizeof *out); return; }
  LightLock_Lock(&g_lock);
  *out = g_st;
  LightLock_Unlock(&g_lock);
}
bool installer_busy(void) { return __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE); }
void installer_init(void) {
  if (g_init) return;
  LightLock_Init(&g_lock);
  g_init = true;
}

/* ------------------------------------------------------------------------------------------------ the sink: AM */
typedef struct {
  Handle am;
  bool opened;
  uint64_t off;
  FS_MediaType dest;
  ndp_sha256_ctx sha;
  uint64_t last_ms, last_off;
  unsigned bps;
  uint32_t res;
} sink;

static uint64_t now_ms(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000); }

static bool sink_open(sink *k, const cia_info *ci, int replaces) {
  Result rc;
  k->dest = ci->on_nand ? MEDIATYPE_NAND : MEDIATYPE_SD;
  ndp_sha256_init(&k->sha);
  if (replaces) { /* the same way FBI reinstalls: the installed copy goes first, so the same or an older version can be installed */
    AM_DeleteTitle(k->dest, ci->title_id);
    AM_DeleteTicket(ci->title_id);
    if (k->dest == MEDIATYPE_SD) AM_QueryAvailableExternalTitleDatabase(NULL);
  }
  rc = AM_StartCiaInstall(k->dest, &k->am);
  if (R_FAILED(rc)) { k->res = (uint32_t)rc; return false; }
  k->opened = true;
  k->last_ms = now_ms();
  return true;
}

static bool sink_write(sink *k, const uint8_t *buf, size_t len, uint64_t total) {
  u32 wrote = 0;
  uint64_t t;
  Result rc = FSFILE_Write(k->am, &wrote, k->off, buf, (u32)len, 0);
  if (R_FAILED(rc) || wrote != len) { k->res = (uint32_t)rc; return false; }
  ndp_sha256_update(&k->sha, buf, len);
  k->off += len;
  t = now_ms();
  if (t - k->last_ms >= 1000) {
    k->bps = (unsigned)((k->off - k->last_off) * 1000 / (t - k->last_ms));
    k->last_ms = t;
    k->last_off = k->off;
  }
  progress(k->off, total, k->bps);
  return true;
}

static void sink_abort(sink *k) {
  if (k->opened) AM_CancelCIAInstall(k->am);
  k->opened = false;
}

static void hex32(const uint8_t d[32], char out[65]) {
  int i;
  for (i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* Everything received: checks first, and only then commits. A mismatch installs nothing. */
static bool sink_finish(sink *k, const cia_info *ci) {
  uint8_t digest[32];
  Result rc;
  ndp_sha256_final(&k->sha, digest);
  LightLock_Lock(&g_lock);
  hex32(digest, g_st.sha_hex);
  LightLock_Unlock(&g_lock);
  if (k->off != ci->total_size) { sink_abort(k); fail(INS_E_SIZE, 0); return false; }
  if (g_have_expect && memcmp(digest, g_expect, 32) != 0) { sink_abort(k); fail(INS_E_HASH, 0); return false; }
  rc = AM_FinishCiaInstall(k->am);
  k->opened = false;
  if (R_FAILED(rc)) { fail(INS_E_AM, (uint32_t)rc); return false; }
  if (k->dest == MEDIATYPE_SD) AM_QueryAvailableExternalTitleDatabase(NULL);
  return true;
}

/* ---------------------------------------------------------------------------------------------------- probing */
static bool enough_space(const cia_info *ci) {
  FS_ArchiveResource r;
  if (ci->on_nand) return true; /* AM reports the NAND case itself */
  return R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) && (uint64_t)r.freeClusters * r.clusterSize >= ci->total_size + SPACE_MARGIN;
}

/* Looks at what was read and either moves to CONFIRM or fails. `known_total` is the size the source reports (0 = unknown). */
static bool judge(const uint8_t *buf, size_t n, uint64_t known_total) {
  cia_info ci;
  AM_TitleInfo ti;
  u64 id;
  int rc = cia_parse(buf, n, &ci);
  if (rc != 0) { fail(INS_E_NOT_CIA, 0); return false; }
  if (ci.cls == CIA_CLASS_FIRM) { fail(INS_E_FIRM, 0); return false; }
  if (ci.title_id == OWN_TITLE) { fail(INS_E_SELF, 0); return false; }
  if (known_total && known_total != ci.total_size) { fail(INS_E_SIZE, 0); return false; }
  if (!enough_space(&ci)) { fail(INS_E_SPACE, 0); return false; }
  id = ci.title_id;
  LightLock_Lock(&g_lock);
  g_st.info = ci;
  g_st.has_info = 1;
  g_st.total = ci.total_size;
  g_st.replaces = R_SUCCEEDED(AM_GetTitleInfo(ci.on_nand ? MEDIATYPE_NAND : MEDIATYPE_SD, 1, &id, &ti));
  g_st.hash_expected = g_have_expect;
  g_st.state = INS_CONFIRM;
  touch();
  LightLock_Unlock(&g_lock);
  return true;
}

/* ------------------------------------------------------------------------------------------------------ curl */
typedef struct {
  uint8_t *probe;       /* probe mode: collects the first CIA_PROBE_BYTES */
  size_t probe_len;
  uint64_t total_hint;  /* from Content-Range / Content-Length */
  sink *k;              /* install mode */
  const cia_info *ci;
  bool failed;
} xfer;

static size_t on_header(char *p, size_t a, size_t b, void *ud) {
  xfer *x = ud;
  size_t n = a * b;
  char line[160];
  if (n >= sizeof line) return n;
  memcpy(line, p, n);
  line[n] = '\0';
  if (!strncasecmp(line, "Content-Range:", 14)) {
    const char *slash = strchr(line, '/');
    if (slash && slash[1] >= '0' && slash[1] <= '9') x->total_hint = strtoull(slash + 1, NULL, 10);
  } else if (!strncasecmp(line, "Content-Length:", 15) && x->total_hint == 0) {
    x->total_hint = strtoull(line + 15, NULL, 10);
  }
  return n;
}

static size_t on_body(void *ptr, size_t a, size_t b, void *ud) {
  xfer *x = ud;
  size_t n = a * b;
  if (cancelled()) return 0;
  if (x->probe) {
    size_t take = CIA_PROBE_BYTES - x->probe_len;
    if (take > n) take = n;
    memcpy(x->probe + x->probe_len, ptr, take);
    x->probe_len += take;
    return x->probe_len >= CIA_PROBE_BYTES ? 0 : n; /* enough: stop the transfer (a write error is the expected end) */
  }
  if (!x->k->opened && !sink_open(x->k, x->ci, g_st.replaces)) { x->failed = true; return 0; }
  if (!sink_write(x->k, ptr, n, x->ci->total_size)) { x->failed = true; return 0; }
  return n;
}

static int on_xfer(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) { (void)p; (void)a; (void)b; (void)c; (void)d; return cancelled() ? 1 : 0; }

static CURLcode run_curl(const char *url, xfer *x, bool range) {
  CURL *c = curl_easy_init();
  struct curl_blob ca;
  CURLcode code;
  if (!c) return CURLE_FAILED_INIT;
  ca.data = (void *)nsd_ca_pem;
  ca.len = (size_t)(nsd_ca_pem_end - nsd_ca_pem);
  ca.flags = CURL_BLOB_NOCOPY;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "NSD-Bridge/" NDP_AGENT_VERSION);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 128L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)IO_SIZE);
  if (range) curl_easy_setopt(c, CURLOPT_RANGE, "0-16383");
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, x);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, x);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_xfer);
  code = curl_easy_perform(c);
  curl_easy_cleanup(c);
  return code;
}

/* ---------------------------------------------------------------------------------------------------- the job */
static void wait_answer(void) {
  while (!g_answered && !cancelled()) svcSleepThread(50 * 1000000LL);
}

static void install_from_file(void) {
  sink k;
  FILE *f;
  uint8_t *buf = malloc(IO_SIZE);
  ins_status st;
  size_t n;
  memset(&k, 0, sizeof k);
  installer_get(&st);
  f = buf ? fopen(g_src, "rb") : NULL;
  if (!f) { free(buf); fail(INS_E_READ, 0); return; }
  set_state(INS_INSTALLING);
  if (!sink_open(&k, &st.info, st.replaces)) { fclose(f); free(buf); fail(INS_E_AM, k.res); return; }
  while ((n = fread(buf, 1, IO_SIZE, f)) > 0) {
    if (cancelled()) { sink_abort(&k); fclose(f); free(buf); fail(INS_E_CANCELLED, 0); return; }
    if (!sink_write(&k, buf, n, st.info.total_size)) { sink_abort(&k); fclose(f); free(buf); fail(INS_E_AM, k.res); return; }
  }
  fclose(f);
  free(buf);
  if (sink_finish(&k, &st.info)) set_state(INS_DONE);
}

static void install_from_url(void) {
  sink k;
  xfer x;
  ins_status st;
  CURLcode code;
  memset(&k, 0, sizeof k);
  memset(&x, 0, sizeof x);
  installer_get(&st);
  x.k = &k;
  x.ci = &st.info;
  set_state(INS_INSTALLING);
  code = run_curl(g_src, &x, false);
  if (x.failed) { sink_abort(&k); fail(INS_E_AM, k.res); return; }
  if (code != CURLE_OK) {
    sink_abort(&k);
    if (cancelled()) fail(INS_E_CANCELLED, 0);
    else { alog("Install: download failed (curl %d: %s)", (int)code, curl_easy_strerror(code)); fail(INS_E_NET, 0); }
    return;
  }
  if (!k.opened) { fail(INS_E_NET, 0); return; }
  if (sink_finish(&k, &st.info)) set_state(INS_DONE);
}

static void run_job(void *arg) {
  uint8_t *probe = malloc(CIA_PROBE_BYTES);
  bool curl_ready = false;
  (void)arg;
  if (!probe || R_FAILED(amInit())) { free(probe); fail(INS_E_AM, 0); __atomic_store_n(&g_busy, false, __ATOMIC_RELEASE); return; }
  if (g_is_url) {
    xfer x;
    CURLcode code;
    memset(&x, 0, sizeof x);
    x.probe = probe;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) { fail(INS_E_NET, 0); goto done; }
    curl_ready = true;
    code = run_curl(g_src, &x, true);
    if (cancelled()) { set_state(INS_IDLE); goto done; }
    if (x.probe_len < CIA_PROBE_BYTES && code != CURLE_OK) { alog("Install: probe failed (curl %d: %s)", (int)code, curl_easy_strerror(code)); fail(INS_E_NET, 0); goto done; }
    if (!judge(probe, x.probe_len, x.total_hint)) goto done;
  } else {
    FILE *f = fopen(g_src, "rb");
    size_t n;
    long size;
    if (!f) { fail(INS_E_READ, 0); goto done; }
    n = fread(probe, 1, CIA_PROBE_BYTES, f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    if (!judge(probe, n, size > 0 ? (uint64_t)size : 0)) goto done;
  }
  wait_answer();
  if (cancelled() || !g_confirmed) { set_state(INS_IDLE); goto done; }
  if (g_is_url) install_from_url();
  else install_from_file();
done:
  if (curl_ready) curl_global_cleanup();
  free(probe);
  amExit();
  __atomic_store_n(&g_busy, false, __ATOMIC_RELEASE);
}

static bool begin(const char *src, bool is_url, const uint8_t *sha256) {
  const char *h;
  size_t i;
  s32 prio = 0x30;
  if (!g_init || installer_busy() || updater_busy()) return false;
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  snprintf(g_src, sizeof g_src, "%s", src);
  g_is_url = is_url;
  g_have_expect = sha256 != NULL;
  if (sha256) memcpy(g_expect, sha256, 32);
  __atomic_store_n(&g_cancel, false, __ATOMIC_RELEASE);
  g_answered = false;
  g_confirmed = false;
  LightLock_Lock(&g_lock);
  memset(&g_st, 0, sizeof g_st);
  g_st.state = INS_PROBING;
  g_st.is_url = is_url;
  snprintf(g_st.source, sizeof g_st.source, "%s", src);
  h = is_url ? strstr(src, "://") : NULL;
  if (h) {
    h += 3;
    for (i = 0; i + 1 < sizeof g_st.host && h[i] && h[i] != '/' && h[i] != '?'; i++) g_st.host[i] = h[i];
    g_st.host[i] = '\0';
  }
  touch();
  LightLock_Unlock(&g_lock);
  __atomic_store_n(&g_busy, true, __ATOMIC_RELEASE);
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  g_worker = threadCreate(run_job, NULL, 96 * 1024, prio > 0x18 ? prio - 1 : prio, 0, false);
  if (!g_worker) { __atomic_store_n(&g_busy, false, __ATOMIC_RELEASE); fail(INS_E_NET, 0); return false; }
  return true;
}

bool installer_start_file(const char *sd_path) { return begin(sd_path, false, NULL); }
bool installer_start_url(const char *url, const uint8_t *sha256) { return begin(url, true, sha256); }

void installer_confirm(bool yes) {
  ins_status st;
  installer_get(&st);
  if (st.state != INS_CONFIRM) return;
  g_confirmed = yes;
  g_answered = true;
}
void installer_cancel(void) { __atomic_store_n(&g_cancel, true, __ATOMIC_RELEASE); g_answered = true; }
void installer_dismiss(void) {
  ins_status st;
  installer_get(&st);
  if (st.state == INS_DONE || st.state == INS_ERROR) set_state(INS_IDLE);
}
void installer_shutdown(void) {
  if (!g_init) return;
  installer_cancel();
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  g_init = false;
}
#endif /* !AGENT_TEXT_UI */
