#ifndef AGENT_TEXT_UI
#include "installer.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>

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
static bool g_is_url, g_have_expect, g_have_seed;
static uint8_t g_expect[32], g_seed[16]; /* g_seed: given with the request (the page's optional field) */

static bool cancelled(void) { return __atomic_load_n(&g_cancel, __ATOMIC_ACQUIRE) || aptShouldClose(); }

static void touch(void) { g_st.revision++; }
static void set_state(ins_state s) { LightLock_Lock(&g_lock); g_st.state = s; touch(); LightLock_Unlock(&g_lock); }
static int g_net_code;
static long g_http_code;
static void fail(ins_error e, uint32_t am) {
  LightLock_Lock(&g_lock);
  g_st.state = INS_ERROR;
  g_st.error = e;
  g_st.am_result = am;
  g_st.net_code = e == INS_E_NET ? g_net_code : 0;
  g_st.http_code = e == INS_E_NET ? g_http_code : 0;
  touch();
  LightLock_Unlock(&g_lock);
}
static uint64_t now_ms(void);
/* Progress changes with every piece received (dozens of times a second); the screen only needs to hear about it a few times a
 * second, because each redraw costs the download CPU time. */
static void progress(uint64_t done, uint64_t total, unsigned bps, unsigned eta) {
  static uint64_t last_ms;
  uint64_t t = now_ms();
  LightLock_Lock(&g_lock);
  g_st.done = done;
  g_st.total = total;
  g_st.bps = bps;
  g_st.eta_s = eta;
  if (t - last_ms >= 250 || (total && done >= total)) { touch(); last_ms = t; }
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
#define BLOCK (128u * 1024u)
#define FLAG_GET(f) __atomic_load_n(&(f), __ATOMIC_ACQUIRE)
#define FLAG_SET(f) __atomic_store_n(&(f), true, __ATOMIC_RELEASE)
#define SLOTS 4 /* 4 x 128 KiB in flight: the network keeps reading while the system's title manager is writing */

/* The install is a small pipeline: whoever has the data (the download callback, or the loop reading a file) fills a slot and
 * hands it over; a writer thread feeds the slots, in order, to AM and to the SHA-256. Before, one thread did both in turn, so
 * the socket was not read during every AM write, the TCP window closed and the sender waited. */
typedef struct {
  Handle am;
  bool opened;
  FS_MediaType dest;
  ndp_sha256_ctx sha;
  uint8_t *slot[SLOTS];
  size_t len[SLOTS];          /* bytes in the slot; 0 on the last "slot" = end marker */
  unsigned head, tail;        /* producer slot / writer slot */
  size_t fill;                /* bytes in the producer's current slot */
  LightSemaphore free_slots, full_slots;
  Thread writer;
  bool failed, stop; /* only through the macros below: one thread writes, the other reads */
  uint64_t in;                /* bytes handed over (producer) */
  uint64_t off;               /* bytes written to AM (writer) */
  uint64_t wait_prod_ms, wait_writer_ms; /* diagnostics: who waited for whom */
  uint64_t last_ms, last_off;
  unsigned bps;
  double ema;                 /* smoothed bytes per second: the estimate must not jump with every sample */
  unsigned eta;
  uint64_t total;
  uint32_t res;
} sink;

static uint64_t now_ms(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000); }
static uint64_t g_am_ms, g_start_ms; /* time spent inside AM writes / since the install started (diagnostics) */

static void sink_release(sink *k);
static int seed_register(u64 title_id); /* defined below, with the other network code */
static int seed_preflight(u64 title_id);

static void writer_thread(void *arg) {
  sink *k = arg;
  for (;;) {
    uint64_t t0 = now_ms();
    size_t n;
    LightSemaphore_Acquire(&k->full_slots, 1);
    k->wait_writer_ms += now_ms() - t0;
    n = k->len[k->tail];
    if (n == 0 || FLAG_GET(k->stop)) break;
    if (!FLAG_GET(k->failed)) {
      u32 wrote = 0;
      uint64_t w0 = now_ms(), t;
      Result rc = FSFILE_Write(k->am, &wrote, k->off, k->slot[k->tail], (u32)n, 0);
      g_am_ms += now_ms() - w0;
      if (R_FAILED(rc) || wrote != n) {
        k->res = (uint32_t)rc;
        FLAG_SET(k->failed);
        alog("Install: AM write failed at byte %llu: 0x%08lX (wrote %lu of %lu)", (unsigned long long)k->off, (unsigned long)rc, (unsigned long)wrote, (unsigned long)n);
      } else {
        ndp_sha256_update(&k->sha, k->slot[k->tail], n);
        k->off += n;
        t = now_ms();
        if (t - k->last_ms >= 1000) {
          double inst = (double)(k->off - k->last_off) * 1000.0 / (double)(t - k->last_ms);
          k->bps = (unsigned)inst;
          k->ema = k->ema > 0 ? 0.8 * k->ema + 0.2 * inst : inst;
          k->eta = k->ema > 1.0 && k->total > k->off ? (unsigned)((double)(k->total - k->off) / k->ema) : 0;
          k->last_ms = t;
          k->last_off = k->off;
        }
        progress(k->off, k->total, k->bps, k->eta);
      }
    }
    k->tail = (k->tail + 1) % SLOTS;
    LightSemaphore_Release(&k->free_slots, 1); /* after a failure the slots are still freed, so the producer never blocks forever */
  }
}

static bool sink_open(sink *k, const cia_info *ci, int replaces) {
  Result rc;
  int i;
  s32 prio = 0x30;
  k->dest = ci->on_nand ? MEDIATYPE_NAND : MEDIATYPE_SD;
  k->total = ci->total_size;
  ndp_sha256_init(&k->sha);
  for (i = 0; i < SLOTS; i++) {
    k->slot[i] = malloc(BLOCK);
    if (!k->slot[i]) { sink_release(k); k->res = 0xE0E01BF8u; alog("Install: no memory for the write buffers"); return false; }
  }
  if (replaces) { /* the same way FBI reinstalls: the installed copy goes first, so the same or an older version can be installed */
    AM_DeleteTitle(k->dest, ci->title_id);
    AM_DeleteTicket(ci->title_id);
    if (k->dest == MEDIATYPE_SD) AM_QueryAvailableExternalTitleDatabase(NULL);
  }
  rc = AM_StartCiaInstall(k->dest, &k->am);
  if (R_FAILED(rc)) {
    k->res = (uint32_t)rc;
    alog("Install: AM_StartCiaInstall failed 0x%08lX (%s)", (unsigned long)rc, k->dest == MEDIATYPE_NAND ? "NAND" : "SD");
    sink_release(k);
    return false;
  }
  g_am_ms = 0;
  g_start_ms = now_ms();
  LightSemaphore_Init(&k->free_slots, SLOTS - 1, SLOTS); /* slot 0 is the producer's from the start */
  LightSemaphore_Init(&k->full_slots, 0, SLOTS + 1);
  k->head = k->tail = 0;
  k->fill = 0;
  k->last_ms = now_ms();
  k->opened = true;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  k->writer = threadCreate(writer_thread, k, 32 * 1024, prio, -1, false);
  if (!k->writer) {
    AM_CancelCIAInstall(k->am);
    k->opened = false;
    k->res = 0xE0E01BF8u;
    sink_release(k);
    alog("Install: cannot start the writer thread");
    return false;
  }
  return true;
}

/* Hands the producer's slot to the writer and takes the next free one (waits if the writer is behind). */
static bool slot_handoff(sink *k, size_t n) {
  uint64_t t0;
  k->len[k->head] = n;
  k->head = (k->head + 1) % SLOTS;
  LightSemaphore_Release(&k->full_slots, 1);
  t0 = now_ms();
  LightSemaphore_Acquire(&k->free_slots, 1);
  k->wait_prod_ms += now_ms() - t0;
  return !FLAG_GET(k->failed);
}

static bool sink_write(sink *k, const uint8_t *buf, size_t len, uint64_t total) {
  (void)total;
  k->in += len;
  while (len) {
    size_t take = BLOCK - k->fill;
    if (FLAG_GET(k->failed)) return false;
    if (take > len) take = len;
    memcpy(k->slot[k->head] + k->fill, buf, take);
    k->fill += take;
    buf += take;
    len -= take;
    if (k->fill == BLOCK) {
      k->fill = 0;
      if (!slot_handoff(k, BLOCK)) return false;
    }
  }
  return !FLAG_GET(k->failed);
}

static void sink_release(sink *k) {
  int i;
  for (i = 0; i < SLOTS; i++) { free(k->slot[i]); k->slot[i] = NULL; }
}

/* Stops the writer (after it drains what was handed over, unless `drop`), and waits for it. */
static void writer_end(sink *k, bool drop) {
  if (!k->writer) return;
  if (drop) FLAG_SET(k->stop);
  if (!drop && k->fill) { (void)slot_handoff(k, k->fill); k->fill = 0; }
  k->len[k->head] = 0; /* the end marker goes in the slot the producer holds */
  LightSemaphore_Release(&k->full_slots, 1);
  threadJoin(k->writer, UINT64_MAX);
  threadFree(k->writer);
  k->writer = NULL;
}

static void sink_abort(sink *k) {
  writer_end(k, true);
  if (k->opened) AM_CancelCIAInstall(k->am);
  k->opened = false;
  sink_release(k);
}

static void hex32(const uint8_t d[32], char out[65]) {
  int i;
  for (i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* Everything received: checks first, and only then commits. A mismatch installs nothing. */
static bool sink_finish(sink *k, const cia_info *ci) {
  uint8_t digest[32];
  Result rc;
  if (FLAG_GET(k->failed)) { sink_abort(k); fail(INS_E_AM, k->res); return false; }
  if (k->in != ci->total_size) { alog("Install: received %llu bytes, the header says %llu", (unsigned long long)k->in, (unsigned long long)ci->total_size); sink_abort(k); fail(INS_E_SIZE, 0); return false; }
  writer_end(k, false);
  if (FLAG_GET(k->failed)) { sink_abort(k); fail(INS_E_AM, k->res); return false; }
  ndp_sha256_final(&k->sha, digest);
  LightLock_Lock(&g_lock);
  hex32(digest, g_st.sha_hex);
  LightLock_Unlock(&g_lock);
  if (g_have_expect && memcmp(digest, g_expect, 32) != 0) { sink_abort(k); fail(INS_E_HASH, 0); return false; }
  rc = AM_FinishCiaInstall(k->am);
  k->opened = false;
  {
    uint64_t el = now_ms() - g_start_ms;
    /* each log line holds 96 characters: short lines, or the end of a long one is lost */
    alog("Install: finish 0x%08lX, %llu bytes in %lu s (%.2f MiB/s)", (unsigned long)rc, (unsigned long long)k->in, (unsigned long)(el / 1000),
         el ? (double)k->in / 1048576.0 / ((double)el / 1000.0) : 0.0);
    alog("AM writes %lu s; network waited %lu s for the writer; writer waited %lu s", (unsigned long)(g_am_ms / 1000),
         (unsigned long)(k->wait_prod_ms / 1000), (unsigned long)(k->wait_writer_ms / 1000));
    alog("sha256 %s", g_st.sha_hex);
  }
  sink_release(k);
  if (R_FAILED(rc)) { fail(INS_E_AM, (uint32_t)rc); return false; }
  if (k->dest == MEDIATYPE_SD) AM_QueryAvailableExternalTitleDatabase(NULL);
  /* applications and updates of the retail line may need a seed; DSiWare and system titles never do */
  if (ci->cls == CIA_CLASS_APP) {
    int sd = seed_register(ci->title_id);
    LightLock_Lock(&g_lock);
    g_st.seed = sd;
    touch();
    LightLock_Unlock(&g_lock);
  }
  return true;
}

/* ---------------------------------------------------------------------------------------------------- probing */
/* ------------------------------------------------------------------------------------------------------- seeds */
/* Titles released since 2015 are encrypted with a per-title "seed" that Nintendo's seed server hands out; without it the
 * system's loader cannot open the game (it crashes in `loader`). FBI fetches and registers it after every install; so do we.
 * The request goes through the system's own HTTP service (which trusts Nintendo's certificates, unlike our bundle) and only
 * carries the title ID and the console's region. A title that needs no seed makes the server answer 404: that is fine. */
static Result fs_add_seed(u64 title_id, const uint8_t seed[16]) {
  u32 *cmd = getThreadCommandBuffer();
  Result ret;
  cmd[0] = 0x087A0180; /* FS:USER AddSeed */
  cmd[1] = (u32)(title_id & 0xFFFFFFFF);
  cmd[2] = (u32)(title_id >> 32);
  memcpy(&cmd[3], seed, 16);
  if (R_FAILED(ret = svcSendSyncRequest(*fsGetSessionHandle()))) return ret;
  return (Result)cmd[1];
}

/* Where the seed of a title comes from, in this order: the request itself, a file in FBI's place (it works with no internet,
 * or when the console talks to a replacement network instead of Nintendo's), Nintendo's seed server.
 * Result: 1 = found (`seed` filled, `*src` says where), 2 = the server has none (the title does not use one),
 * 3 = could not find out (no connection, error), 0 = turned off by config/no-seed-download. */
static int seed_obtain(u64 title_id, uint8_t seed[16], int *src) {
  static const char *const country[] = {"JP", "US", "GB", "GB", "HK", "KR", "TW"};
  char url[128], path[96];
  u8 region = CFG_REGION_USA;
  u32 status = 0, got = 0;
  httpcContext ctx;
  Result rc;
  int result = 3;
  struct stat nb;
  FILE *f;
  *src = 0;
  if (g_have_seed) { memcpy(seed, g_seed, 16); *src = 1; return 1; }
  snprintf(path, sizeof path, "sdmc:/fbi/seed/%016llX.dat", (unsigned long long)title_id);
  f = fopen(path, "rb");
  if (f) {
    size_t n = fread(seed, 1, 16, f);
    fclose(f);
    if (n == 16) { *src = 2; return 1; }
  }
  if (stat("sdmc:/3ds/nintendo-dev-agent/config/no-seed-download", &nb) == 0) return 0;
  CFGU_SecureInfoGetRegion(&region);
  if (region > CFG_REGION_TWN) region = CFG_REGION_USA;
  snprintf(url, sizeof url, "https://kagiya-ctr.cdn.nintendo.net/title/0x%016llX/ext_key?country=%s", (unsigned long long)title_id, country[region]);
  if (R_FAILED(rc = httpcInit(0x4000))) { alog("Seed: httpcInit 0x%08lX", (unsigned long)rc); return 3; }
  {
    /* First with the system's certificate check. Nintendo's seed server uses a certificate from Nintendo's own private
     * authority; if this console's store does not trust it the request fails before any HTTP status, and it is repeated once
     * without the check -- for this one fixed host and path only. The seed is not a secret and the worst a forged answer can do
     * is make a game fail to open; FBI makes the same choice. */
    int attempt;
    for (attempt = 0; attempt < 2 && result == 3; attempt++) {
      status = 0;
      got = 0;
      if (R_SUCCEEDED(rc = httpcOpenContext(&ctx, HTTPC_METHOD_GET, url, 1))) {
        bool ok = (attempt == 0 || R_SUCCEEDED(rc = httpcSetSSLOpt(&ctx, SSLCOPT_DisableVerify))) &&
                  R_SUCCEEDED(rc = httpcAddRequestHeaderField(&ctx, "User-Agent", "NSD-Bridge/" NDP_AGENT_VERSION)) && R_SUCCEEDED(rc = httpcBeginRequest(&ctx)) &&
                  R_SUCCEEDED(rc = httpcGetResponseStatusCodeTimeout(&ctx, &status, 15ull * 1000000000ull));
        if (ok) {
          if (status == 200 && R_SUCCEEDED(rc = httpcDownloadData(&ctx, seed, 16, &got)) && got == 16) {
            result = 1;
            *src = 3;
            if (attempt) alog("Seed: fetched without the certificate check (Nintendo's CA is not trusted)");
          } else if (status == 404 || status == 403) {
            result = 2;
          } else {
            alog("Seed: the server answered HTTP %lu (download 0x%08lX, %lu bytes)", (unsigned long)status, (unsigned long)rc, (unsigned long)got);
            attempt = 2; /* a real answer that is not a seed: no point in repeating */
          }
        } else {
          alog("Seed: request %s failed 0x%08lX", attempt ? "(unverified)" : "(verified)", (unsigned long)rc);
        }
        httpcCloseContext(&ctx);
      } else {
        alog("Seed: httpcOpenContext 0x%08lX", (unsigned long)rc);
        break;
      }
    }
  }
  httpcExit();
  return result;
}

/* The check done before the download (the title ID is known from the first bytes): is there a seed to be had? It is kept, so
 * the install does not ask again. */
static int g_pre_result = -1, g_pre_src;
static uint8_t g_pre_seed[16];
static u64 g_pre_id;
static int seed_preflight(u64 title_id) {
  g_pre_result = seed_obtain(title_id, g_pre_seed, &g_pre_src);
  g_pre_id = title_id;
  alog("Seed check before the download: %s%s", g_pre_result == 1 ? "available" : g_pre_result == 2 ? "the server has none (the title uses none)" : g_pre_result == 0 ? "off" : "could not be checked",
       g_pre_result == 1 ? (g_pre_src == 1 ? " (from the request)" : g_pre_src == 2 ? " (from /fbi/seed)" : " (from Nintendo's server)") : "");
  return g_pre_result;
}

static int seed_register(u64 title_id) {
  uint8_t seed[16];
  int src = 0, result;
  Result rc;
  if (g_pre_result >= 0 && g_pre_id == title_id) { result = g_pre_result; memcpy(seed, g_pre_seed, 16); src = g_pre_src; }
  else result = seed_obtain(title_id, seed, &src);
  g_pre_result = -1;
  if (result != 1) {
    if (result == 2) alog("Install: no seed on the server: this title does not use one");
    else if (result == 3) alog("Install: the seed could not be fetched");
    return result == 2 ? 2 : result == 0 ? 0 : 3;
  }
  rc = fs_add_seed(title_id, seed);
  alog("Install: seed registered (%s), result 0x%08lX", src == 1 ? "from the request" : src == 2 ? "from /fbi/seed" : "from Nintendo's server", (unsigned long)rc);
  return R_SUCCEEDED(rc) ? 1 : 3;
}


static bool enough_space(const cia_info *ci) {
  FS_ArchiveResource r;
  if (ci->on_nand) return true; /* AM reports the NAND case itself */
  return R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) && (uint64_t)r.freeClusters * r.clusterSize >= ci->total_size + SPACE_MARGIN;
}

/* Is this title installed? AM_GetTitleInfo answers for one title; the list is the fallback (and both are logged: this
 * check decides whether the installed copy is removed first). */
static int title_installed(FS_MediaType mt, u64 id, AM_TitleInfo *ti) {
  Result r = AM_GetTitleInfo(mt, 1, &id, ti);
  u32 count = 0, got = 0, i;
  u64 *ids;
  int found = 0;
  if (R_SUCCEEDED(r)) return 1;
  if (R_FAILED(AM_GetTitleCount(mt, &count)) || count == 0) {
    alog("Install: AM_GetTitleInfo 0x%08lX, title count %lu", (unsigned long)r, (unsigned long)count);
    return 0;
  }
  ids = malloc((size_t)count * sizeof *ids);
  if (ids && R_SUCCEEDED(AM_GetTitleList(&got, mt, count, ids)))
    for (i = 0; i < got; i++) if (ids[i] == id) { found = 1; break; }
  free(ids);
  alog("Install: AM_GetTitleInfo 0x%08lX; list of %lu titles says %s", (unsigned long)r, (unsigned long)got, found ? "installed" : "not installed");
  return found;
}

/* Looks at what was read and either moves to CONFIRM or fails. `known_total` is the size the source reports (0 = unknown). */
static bool judge(const uint8_t *buf, size_t n, uint64_t known_total) {
  cia_info ci;
  AM_TitleInfo ti;
  u64 id;
  int replaces;
  int rc = cia_parse(buf, n, &ci);
  if (rc != 0) { fail(INS_E_NOT_CIA, 0); return false; }
  if (ci.cls == CIA_CLASS_FIRM) { fail(INS_E_FIRM, 0); return false; }
  if (ci.title_id == OWN_TITLE) { fail(INS_E_SELF, 0); return false; }
  if (known_total && known_total != ci.total_size) { fail(INS_E_SIZE, 0); return false; }
  if (!enough_space(&ci)) { fail(INS_E_SPACE, 0); return false; }
  id = ci.title_id;
  replaces = title_installed(ci.on_nand ? MEDIATYPE_NAND : MEDIATYPE_SD, id, &ti);
  alog("Install: title %016llX v0x%04X %s %s, %llu bytes", (unsigned long long)ci.title_id, ci.title_version,
       ci.on_nand ? "NAND" : "SD", ci.cls == CIA_CLASS_SYSTEM ? "system" : ci.cls == CIA_CLASS_DSIWARE ? "DSiWare" : "app", (unsigned long long)ci.total_size);
  alog("Install: installed already: %s", replaces ? "yes (it is removed first)" : "no");
  g_pre_result = -1;
  g_st.seed_pre = -1;
  if (ci.cls == CIA_CLASS_APP && !ci.on_nand) {
    int sp = seed_preflight(id); /* before the (long) download: is there a seed to be had? */
    g_st.seed_pre = sp;
    g_st.seed_src = g_pre_src;
  }
  LightLock_Lock(&g_lock);
  g_st.info = ci;
  g_st.has_info = 1;
  g_st.total = ci.total_size;
  g_st.replaces = replaces;
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

/* The transfer is driven with the multi interface and short waits (50 ms): the console's socket service handles one request
 * at a time, so a long wait inside curl (it waits up to a second when a server is slow) also stalls the bridge and the page. */
static CURLcode run_curl(const char *url, xfer *x, bool range) {
  CURL *c = curl_easy_init();
  CURLM *mh = curl_multi_init();
  struct curl_blob ca;
  CURLcode code = CURLE_FAILED_INIT;
  int running = 1;
  long http = 0;
  if (!c || !mh) { if (c) curl_easy_cleanup(c); if (mh) curl_multi_cleanup(mh); return CURLE_FAILED_INIT; }
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
  curl_multi_add_handle(mh, c);
  while (running) {
    if (curl_multi_perform(mh, &running) != CURLM_OK) break;
    if (running) curl_multi_poll(mh, NULL, 0, 50, NULL);
    if (cancelled()) { code = CURLE_ABORTED_BY_CALLBACK; running = -1; break; }
  }
  if (running != -1) {
    CURLMsg *msg;
    int left;
    code = CURLE_RECV_ERROR;
    while ((msg = curl_multi_info_read(mh, &left)) != NULL)
      if (msg->msg == CURLMSG_DONE) code = msg->data.result;
  }
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  g_net_code = (int)code;
  g_http_code = http;
  if (!range && code == CURLE_OK) {
    curl_off_t sp = 0;
    double total = 0;
    curl_easy_getinfo(c, CURLINFO_SPEED_DOWNLOAD_T, &sp);
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &total);
    alog("Install: download %.2f MiB/s (curl, %.0f s)", (double)sp / 1048576.0, total);
  }
  curl_multi_remove_handle(mh, c);
  curl_easy_cleanup(c);
  curl_multi_cleanup(mh);
  return code;
}


/* ------------------------------------------------------------------------------------------------ speed test */
/* A URL ending in "#speedtest" does not install anything: it downloads a few MB with each of four variations of the network
 * code and throws the data away, so the effect of each choice can be measured against a fast server. */
typedef struct { uint64_t bytes, limit, t0; } bench_x;
static size_t on_bench(void *p, size_t a, size_t b, void *ud) {
  bench_x *x = ud;
  (void)p;
  x->bytes += a * b;
  return (cancelled() || x->bytes >= x->limit) ? 0 : a * b;
}
static int on_bench_xfer(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  bench_x *x = ud;
  (void)a; (void)b; (void)c; (void)d;
  return (cancelled() || now_ms() - x->t0 > 25000) ? 1 : 0; /* never longer than 25 s per variation */
}
static int on_bench_sockopt(void *p, curl_socket_t fd, curlsocktype purpose) {
  int size = 64 * 1024;
  (void)p; (void)purpose;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
  return CURL_SOCKOPT_OK;
}
static unsigned bench_one(const char *url, bool use_multi, bool big_window, uint64_t limit) {
  CURL *c = curl_easy_init();
  bench_x x = {0, limit, now_ms()};
  struct curl_blob ca;
  uint64_t el;
  if (!c) return 0;
  ca.data = (void *)nsd_ca_pem;
  ca.len = (size_t)(nsd_ca_pem_end - nsd_ca_pem);
  ca.flags = CURL_BLOB_NOCOPY;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)IO_SIZE);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_bench);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &x);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_bench_xfer);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, &x);
  if (big_window) curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, on_bench_sockopt);
  x.t0 = now_ms();
  if (!use_multi) {
    (void)curl_easy_perform(c);
  } else {
    CURLM *mh = curl_multi_init();
    int running = 1;
    curl_multi_add_handle(mh, c);
    while (running && !cancelled() && x.bytes < limit && now_ms() - x.t0 <= 25000) {
      if (curl_multi_perform(mh, &running) != CURLM_OK) break;
      if (running) curl_multi_poll(mh, NULL, 0, 50, NULL);
    }
    curl_multi_remove_handle(mh, c);
    curl_multi_cleanup(mh);
  }
  el = now_ms() - x.t0;
  curl_easy_cleanup(c);
  return el > 200 ? (unsigned)(x.bytes * 1000 / 1024 / el) : 0;
}
static void run_bench(const char *url_with_fragment) {
  static const char *const names[4] = {"easy", "multi 50 ms", "easy + 64K window", "multi + 64K window"};
  char url[1100];
  unsigned r[4];
  size_t n = strlen(url_with_fragment);
  int i;
  snprintf(url, sizeof url, "%.*s", (int)(n - 10), url_with_fragment); /* without "#speedtest" */
  LightLock_Lock(&g_lock);
  g_st.is_bench = 1;
  LightLock_Unlock(&g_lock);
  set_state(INS_INSTALLING);
  for (i = 0; i < 4 && !cancelled(); i++) {
    r[i] = bench_one(url, (i & 1) != 0, i >= 2, 6u * 1024u * 1024u);
    alog("Speed test: %s: %u KiB/s", names[i], r[i]);
    LightLock_Lock(&g_lock);
    g_st.bench_kib[i] = r[i];
    touch();
    LightLock_Unlock(&g_lock);
  }
  set_state(cancelled() ? INS_IDLE : INS_DONE);
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
  alog("Install: starting from the SD card%s", st.replaces ? " (the installed copy is removed first)" : "");
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
  alog("Install: starting the download%s", st.replaces ? " (the installed copy is removed first)" : "");
  code = run_curl(g_src, &x, false);
  if (x.failed) { sink_abort(&k); fail(INS_E_AM, k.res); return; }
  if (code != CURLE_OK) {
    sink_abort(&k);
    if (cancelled()) fail(INS_E_CANCELLED, 0);
    else { alog("Install: download failed (curl %d: %s, http %ld)", (int)code, curl_easy_strerror(code), g_http_code); fail(INS_E_NET, 0); }
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
    if (strlen(g_src) > 10 && strcmp(g_src + strlen(g_src) - 10, "#speedtest") == 0) { run_bench(g_src); goto done; }
    code = run_curl(g_src, &x, true);
    if (cancelled()) { set_state(INS_IDLE); goto done; }
    if (x.probe_len < CIA_PROBE_BYTES && code != CURLE_OK) { alog("Install: probe failed (curl %d: %s, http %ld)", (int)code, curl_easy_strerror(code), g_http_code); fail(INS_E_NET, 0); goto done; }
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
  {
    /* While the system's title manager holds the install open, the HOME menu waits for it and the whole console hangs: HOME and
     * sleep stay off until the install ends (the updater does the same). */
    bool home = aptIsHomeAllowed(), sleep_ok = aptIsSleepAllowed();
    aptSetHomeAllowed(false);
    aptSetSleepAllowed(false);
    if (g_is_url) install_from_url();
    else install_from_file();
    aptSetSleepAllowed(sleep_ok);
    aptSetHomeAllowed(home);
  }
done:
  if (curl_ready) curl_global_cleanup();
  free(probe);
  amExit();
  __atomic_store_n(&g_busy, false, __ATOMIC_RELEASE);
}

static bool begin(const char *src, bool is_url, const uint8_t *sha256, const uint8_t *seed) {
  const char *h;
  size_t i;
  s32 prio = 0x30;
  if (!g_init || installer_busy() || updater_busy()) return false;
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  snprintf(g_src, sizeof g_src, "%s", src);
  g_is_url = is_url;
  g_have_expect = sha256 != NULL;
  if (sha256) memcpy(g_expect, sha256, 32);
  g_have_seed = seed != NULL;
  if (seed) memcpy(g_seed, seed, 16);
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

bool installer_start_file(const char *sd_path, const uint8_t *sha256, const uint8_t *seed) { return begin(sd_path, false, sha256, seed); }
bool installer_start_url(const char *url, const uint8_t *sha256, const uint8_t *seed) { return begin(url, true, sha256, seed); }

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

/* Registers every seed file in /fbi/seed (<TITLEID>.dat, 16 bytes) with the system, for games installed without one. */
void installer_import_seeds(char *msg, size_t cap) {
  DIR *d = opendir("sdmc:/fbi/seed");
  struct dirent *e;
  int ok = 0, bad = 0;
  if (!d) { snprintf(msg, cap, "%s", "no /fbi/seed folder"); return; }
  while ((e = readdir(d)) != NULL) {
    unsigned long long id = 0;
    char path[128], tail[8];
    FILE *f;
    uint8_t seed[16];
    if (sscanf(e->d_name, "%16llX.%7s", &id, tail) != 2 || strcasecmp(tail, "dat") != 0 || strlen(e->d_name) != 20) continue;
    snprintf(path, sizeof path, "sdmc:/fbi/seed/%s", e->d_name);
    f = fopen(path, "rb");
    if (!f) { bad++; continue; }
    if (fread(seed, 1, sizeof seed, f) == sizeof seed && R_SUCCEEDED(fs_add_seed((u64)id, seed))) { ok++; alog("Seed registered from %s", e->d_name); }
    else { bad++; alog("Seed file %s could not be registered", e->d_name); }
    fclose(f);
  }
  closedir(d);
  snprintf(msg, cap, "%d registered, %d failed", ok, bad);
}
#endif /* !AGENT_TEXT_UI */
