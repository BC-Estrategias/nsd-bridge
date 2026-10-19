#ifndef AGENT_TEXT_UI
#include "updater.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alog.h"
#include "ndp/ndp_defs.h"
#include "ndp/ndp_sha256.h"
#include "update_manifest.h"
#include "installer.h"

#define PART_FILE "sdmc:/3ds/nintendo-dev-agent/config/update.part"
#define PART_PATH_ASCII "/3ds/nintendo-dev-agent/config/update.part"
#define IO_SIZE (64u * 1024u)
#define JSON_LIMIT (256u * 1024u)
#define TITLE_ID 0x000400000BD00100ull

/* The Mozilla CA bundle (curl.se, pinned in agent/3ds/data), linked into the binary: the console's own store is too old
 * for GitHub's certificate chain. */
__asm__(".section .rodata\n.balign 4\n.global nsd_ca_pem\nnsd_ca_pem:\n.incbin \"" CA_PEM_PATH "\"\n"
        ".byte 0\n.global nsd_ca_pem_end\nnsd_ca_pem_end:\n.previous\n");
extern const char nsd_ca_pem[], nsd_ca_pem_end[];

static LightLock g_lock;
static upd_status g_st;
static update_release g_rel;
static Thread g_worker;
static bool g_init, g_busy, g_cancel, g_download, g_homebrew;
static char g_launch[768];

static bool cancelled(void) { return __atomic_load_n(&g_cancel, __ATOMIC_ACQUIRE) || aptShouldClose(); }

static void publish(upd_state s, const char *msg) {
  LightLock_Lock(&g_lock);
  g_st.state = s;
  g_st.progress = 0;
  g_st.revision++;
  snprintf(g_st.message, sizeof g_st.message, "%s", msg ? msg : "");
  LightLock_Unlock(&g_lock);
}
static void progress(unsigned pct) {
  if (pct > 100) pct = 100;
  LightLock_Lock(&g_lock);
  if (g_st.progress != pct) { g_st.progress = pct; g_st.revision++; }
  LightLock_Unlock(&g_lock);
}

void updater_get(upd_status *out) {
  if (!g_init) { memset(out, 0, sizeof *out); return; }
  LightLock_Lock(&g_lock);
  *out = g_st;
  LightLock_Unlock(&g_lock);
}
bool updater_busy(void) { return __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE); }

typedef struct {
  char *buf;           /* JSON mode */
  size_t size, cap;
  FILE *file;          /* download mode */
  uint32_t expected;
  unsigned char *io;   /* batches small TLS records into bigger SD writes */
  size_t pending;
  bool io_error;
} transfer;

static bool flush_io(transfer *t) {
  if (!t->pending) return true;
  if (fwrite(t->io, 1, t->pending, t->file) != t->pending) { t->io_error = true; return false; }
  t->pending = 0;
  return true;
}

static size_t on_data(void *ptr, size_t a, size_t b, void *ud) {
  transfer *t = ud;
  size_t n = a * b;
  size_t limit = t->file ? t->expected : JSON_LIMIT;
  if (a && b > SIZE_MAX / a) return 0;
  if (cancelled() || t->size > limit || n > limit - t->size) return 0;
  if (t->file) {
    const unsigned char *src = ptr;
    size_t left = n;
    while (left) {
      size_t take = IO_SIZE - t->pending;
      if (take > left) take = left;
      memcpy(t->io + t->pending, src, take);
      t->pending += take;
      src += take;
      left -= take;
      if (t->pending == IO_SIZE && !flush_io(t)) return 0;
    }
    progress(t->expected ? (unsigned)((uint64_t)(t->size + n) * 100u / t->expected) : 0);
  } else {
    if (t->size + n + 1 > t->cap) {
      size_t cap = t->size + n + 4096;
      char *p = realloc(t->buf, cap);
      if (!p) return 0;
      t->buf = p;
      t->cap = cap;
    }
    memcpy(t->buf + t->size, ptr, n);
    t->buf[t->size + n] = '\0';
  }
  t->size += n;
  return n;
}

static int on_progress(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
  (void)p; (void)a; (void)b; (void)c; (void)d;
  return cancelled() ? 1 : 0;
}

static bool fetch(const char *url, transfer *t) {
  CURL *c = curl_easy_init();
  struct curl_blob ca;
  CURLcode code;
  long http = 0;
  char err[CURL_ERROR_SIZE] = {0};
  if (!c) return false;
  ca.data = (void *)nsd_ca_pem;
  ca.len = (size_t)(nsd_ca_pem_end - nsd_ca_pem);
  ca.flags = CURL_BLOB_NOCOPY;
  curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "NSD-Bridge/" NDP_AGENT_VERSION);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, t->file ? 600L : 60L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 128L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_BUFFERSIZE, (long)IO_SIZE);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, t);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
  code = curl_easy_perform(c);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
  curl_easy_cleanup(c);
  if (code != CURLE_OK || http != 200) {
    alog("Update: transfer failed (curl %d: %s, http %ld) %s", (int)code, curl_easy_strerror(code), http, err);
    return false;
  }
  return true;
}

static bool enough_space(uint64_t need) {
  FS_ArchiveResource r;
  return R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&r)) && (uint64_t)r.freeClusters * r.clusterSize >= need;
}

static bool verify_part(void) {
  FILE *f = fopen(PART_FILE, "rb");
  unsigned char *buf = malloc(IO_SIZE);
  uint8_t digest[32];
  ndp_sha256_ctx sha;
  char hex[65];
  size_t n, total = 0;
  int i;
  if (!f || !buf) { if (f) fclose(f); free(buf); return false; }
  ndp_sha256_init(&sha);
  while ((n = fread(buf, 1, IO_SIZE, f)) > 0) {
    if (cancelled()) { fclose(f); free(buf); return false; }
    ndp_sha256_update(&sha, buf, n);
    total += n;
  }
  fclose(f);
  free(buf);
  ndp_sha256_final(&sha, digest);
  for (i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return total == g_rel.size && strcmp(hex, g_rel.sha256) == 0;
}

static bool install_cia(void) {
  Handle in = 0, out = 0;
  bool started = false, ok = false;
  AM_TitleInfo info;
  u64 required = 0, offset = 0;
  unsigned char *buf = malloc(IO_SIZE);
  Result rc = amInit();
  if (R_FAILED(rc)) { free(buf); return false; }
  if (!buf) goto done;
  rc = FSUSER_OpenFileDirectly(&in, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, PART_PATH_ASCII), FS_OPEN_READ, 0);
  if (R_FAILED(rc)) goto done;
  rc = AM_GetCiaFileInfo(MEDIATYPE_SD, &info, in);
  if (R_FAILED(rc) || info.titleID != TITLE_ID) { alog("Update: the CIA is not this app's (title %016llX)", (unsigned long long)info.titleID); goto done; }
  rc = AM_GetCiaRequiredSpace(&required, MEDIATYPE_SD, in);
  if (R_FAILED(rc) || !enough_space(required + 4u * 1024u * 1024u) || cancelled()) goto done;
  /* install over the existing title: the installed one is never deleted first */
  rc = AM_StartCiaInstallOverwrite(&out, MEDIATYPE_SD);
  if (R_FAILED(rc)) goto done;
  started = true;
  while (offset < g_rel.size) {
    u32 got = 0, put = 0, want = g_rel.size - (u32)offset;
    if (want > IO_SIZE) want = IO_SIZE;
    if (cancelled()) goto done;
    rc = FSFILE_Read(in, &got, offset, buf, want);
    if (R_FAILED(rc) || got != want) goto done;
    rc = FSFILE_Write(out, &put, offset, buf, got, 0);
    if (R_FAILED(rc) || put != got) goto done;
    offset += got;
    progress((unsigned)(offset * 100 / g_rel.size));
  }
  rc = AM_FinishCiaInstall(out);
  started = false;
  ok = R_SUCCEEDED(rc);
done:
  if (started) AM_CancelCIAInstall(out);
  if (in) FSFILE_Close(in);
  free(buf);
  alog("Update: CIA install result 0x%08lX (%s)", (unsigned long)rc, ok ? "ok" : "failed");
  amExit();
  return ok;
}

static bool install_3dsx(void) {
  unsigned char magic[4];
  char backup[800];
  struct stat st;
  FILE *f;
  bool ok;
  if (!g_launch[0]) return false;
  f = fopen(PART_FILE, "rb");
  if (!f) return false;
  ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "3DSX", 4) == 0;
  fclose(f);
  if (!ok || cancelled()) return false;
  snprintf(backup, sizeof backup, "%s.update-backup", g_launch);
  if (stat(backup, &st) == 0 || rename(g_launch, backup) != 0) return false;
  if (rename(PART_FILE, g_launch) != 0) { rename(backup, g_launch); return false; }
  remove(backup);
  return true;
}

static void run_job(void *arg) {
  transfer t;
  bool ok = false, curl_ready = false;
  (void)arg;
  memset(&t, 0, sizeof t);
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) goto done;
  curl_ready = true;

  if (!g_download) {
    update_release cand;
    int pr;
    if (!fetch("https://api.github.com/repos/" UPDATE_REPOSITORY "/releases/latest", &t)) goto done;
    pr = update_parse_release(t.buf ? t.buf : "", t.size, g_homebrew, &cand);
    if (pr < 0) { publish(UPD_ERROR, "invalid release data"); goto done; }
    if (pr == 0) { publish(UPD_CURRENT, ""); ok = true; goto done; }
    LightLock_Lock(&g_lock);
    g_rel = cand;
    snprintf(g_st.version, sizeof g_st.version, "%s", cand.version);
    LightLock_Unlock(&g_lock);
    if (update_is_newer(cand.version, NDP_AGENT_VERSION)) { alog("Update: %s is available", cand.version); publish(UPD_AVAILABLE, ""); }
    else publish(UPD_CURRENT, "");
    ok = true;
  } else {
    bool fetched;
    if (!update_allowed_url(g_rel.url) || !enough_space((uint64_t)g_rel.size * 2u + 16u * 1024u * 1024u)) { publish(UPD_ERROR, "not enough SD space"); goto done; }
    if (g_homebrew && !g_launch[0]) { publish(UPD_ERROR, "3dsx path unknown"); goto done; }
    t.expected = g_rel.size;
    t.io = malloc(IO_SIZE);
    t.file = t.io ? fopen(PART_FILE, "wb") : NULL;
    if (!t.file) { publish(UPD_ERROR, "cannot write to the SD card"); goto done; }
    fetched = fetch(g_rel.url, &t);
    if (fetched) fetched = flush_io(&t) && t.size == t.expected;
    if (fclose(t.file) != 0) fetched = false;
    t.file = NULL;
    if (!fetched) goto done;
    publish(UPD_VERIFYING, "");
    if (!verify_part()) { publish(UPD_ERROR, "download check failed"); goto done; }
    if (cancelled() || !aptIsActive()) goto done;
    {
      bool home = aptIsHomeAllowed(), sleep_ok = aptIsSleepAllowed();
      aptSetHomeAllowed(false);
      aptSetSleepAllowed(false);
      publish(UPD_INSTALLING, "");
      ok = g_homebrew ? install_3dsx() : install_cia();
      aptSetSleepAllowed(sleep_ok);
      aptSetHomeAllowed(home);
    }
    publish(ok ? UPD_DONE : UPD_ERROR, ok ? "" : "installation failed");
    if (ok) alog("Update: %s installed", g_rel.version);
  }
done:
  if (t.file) fclose(t.file);
  free(t.io);
  free(t.buf);
  if (g_download) remove(PART_FILE);
  if (curl_ready) curl_global_cleanup();
  {
    upd_status s;
    updater_get(&s);
    if (!ok && s.state != UPD_ERROR) publish(UPD_ERROR, cancelled() ? "cancelled" : "connection failed");
  }
  __atomic_store_n(&g_busy, false, __ATOMIC_RELEASE);
}

static void start(bool download) {
  s32 prio = 0x30;
  int net_prio;
  if (!g_init || updater_busy() || installer_busy()) return;
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  if (download) {
    upd_status s;
    updater_get(&s);
    if (s.state != UPD_AVAILABLE) return;
  }
  g_download = download;
  __atomic_store_n(&g_cancel, false, __ATOMIC_RELEASE);
  __atomic_store_n(&g_busy, true, __ATOMIC_RELEASE);
  publish(download ? UPD_DOWNLOADING : UPD_CHECKING, "");
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  net_prio = prio > 0x18 ? prio - 1 : prio;
  g_worker = threadCreate(run_job, NULL, 96 * 1024, net_prio, 0, false);
  if (!g_worker) { __atomic_store_n(&g_busy, false, __ATOMIC_RELEASE); publish(UPD_ERROR, "cannot start the update"); }
}

void updater_check(void) { start(false); }
void updater_download(void) { start(true); }

void updater_init(const char *path) {
  if (g_init) return;
  LightLock_Init(&g_lock);
  g_homebrew = envIsHomebrew();
  if (g_homebrew && path && !strncmp(path, "sdmc:/", 6) && strlen(path) < sizeof g_launch && strlen(path) > 5 &&
      !strcmp(path + strlen(path) - 5, ".3dsx"))
    strcpy(g_launch, path);
  g_init = true;
}

void updater_shutdown(void) {
  if (!g_init) return;
  __atomic_store_n(&g_cancel, true, __ATOMIC_RELEASE);
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  g_init = false;
}
#endif /* !AGENT_TEXT_UI */
