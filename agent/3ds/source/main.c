/* NSD Bridge — 3DS entry point.
 * Wi-Fi/soc bring-up, the UI, and the main loop. Protocol handling and connection management live in
 * the shared code (agent/common, agent/posix). Network start-up follows the sequence proven by ftpd:
 * acInit -> ACU_GetWifiStatus -> memalign(0x1000, 1 MiB) -> socInit -> NDM exclusive+lock. */
#include <3ds.h>

#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access_ui.h"
#include "alog.h"
#include "installer.h"
#include "titles.h"
#include "translations.h"
#include "qr.h"
#include "sources.h"
#include "ui.h"
#include "ui_internal.h"
#include "updater.h"
#include "ndp/ndp_access_file.h"
#include "ndp/ndp_keystore_file.h"
#include "ndp/ndp_posix_fs.h"
#include "ndp/ndp_server.h"
#include "ndp_web_assets_data.h"

#define AGENT_PORT NDP_DEFAULT_PORT
/* The web page (file manager for the browser) is served on its own port by this same app. It is on when the app
 * starts and R turns it off/on; it needs the same pairing as the CLI, and the console stays READ_ONLY until X. */
#define AGENT_WEB_PORT 8080
#ifndef AGENT_WEB_DEFAULT
#define AGENT_WEB_DEFAULT 1
#endif

/* The agent starts READ_ONLY: nothing can be written until the person at the console presses X (DEVELOPMENT mode).
 * Even then writes only reach the folders opened in the on-console folder editor (A), never the protected system
 * zones. A developer build that starts with writes already on is made with `NDEV_DEV=1 ./scripts/build-3ds.sh`.
 * Every computer must be paired first (AGENT_AUTH_REQUIRED, spec §4). */
#ifndef AGENT_START_MODE
#define AGENT_START_MODE NDP_MODE_READ_ONLY
#endif
/* 1 = pairing + HMAC required (the default and the only setting fit for distribution); 0 = no auth (tests). */
#ifndef AGENT_AUTH_REQUIRED
#define AGENT_AUTH_REQUIRED 1
#endif
#define AGENT_DIR "sdmc:/3ds/nintendo-dev-agent"
#define CONFIG_DIR AGENT_DIR "/config"
#define KEYS_FILE CONFIG_DIR "/pairing.bin"
#define ACCESS_FILE CONFIG_DIR "/access.bin"
#define PAIRING_WINDOW_MS 120000u
#define FORGET_CONFIRM_MS 5000u
#define SOC_ALIGN 0x1000
#define SOC_BUFSIZE 0x400000 /* 4 MiB: with the larger receive windows (nettune.h) 8 connections need more than the old 1 MiB */
#define SOC_BUFSIZE_MIN 0x100000
static u32 g_soc_size = SOC_BUFSIZE;

#define C_RESET "\x1b[0m"
#define C_GREEN "\x1b[32;1m"
#define C_YELLOW "\x1b[33;1m"
#define C_RED "\x1b[31;1m"
#define C_CYAN "\x1b[36;1m"

/* Drawing blocks on vblank (~16-20 ms) and would delay reading the next request, so the UI is
 * redrawn at most every DRAW_MIN_MS (measured on hardware: ~20 ms added per back-to-back request
 * when redrawing on every request). */
#define DRAW_MIN_MS 250
#define DRAW_MAX_MS 1000
#define BOT_COLS 39
#define BOT_ROWS 27

#ifdef AGENT_TEXT_UI
static PrintConsole g_top, g_bot;
#endif
static ndp_server g_srv; /* ~135 KB of buffers inside: static, not on the stack */

/* Filesystem: the SD card ("sdmc:"), read-only. Requests that touch the agent's own folder flush the
 * buffered log first, so `ndev cat .../agent.log` shows the current log and not the last flush. */
static ndp_posix_fs_ctx g_fs_ctx;
static ndp_fs_ops g_fs_base, g_fs;

/* Any SD operation slower than this is logged (measured on hardware: mkdir takes ~5.7 s, a small
 * file write ~85 ms; the log shows which call stalls). */
#define SLOW_FS_MS 150
static uint64_t now_ms(void *ctx);
static void slow_check(const char *op, const char *path, uint64_t t0) {
  uint64_t d = now_ms(NULL) - t0;
  if (d >= SLOW_FS_MS) alog("WARN slow %s %lu ms %s", op, (unsigned long)d, path ? path : "");
}

static void flush_if_own_folder(const char *path) {
  static const char own[] = "/3ds/nintendo-dev-agent";
  if (strncmp(path, own, sizeof own - 1) == 0) alog_flush();
}
static int fs_stat(void *ctx, const char *path, ndp_fs_stat *st) {
  uint64_t t0;
  int rc;
  flush_if_own_folder(path);
  t0 = now_ms(NULL);
  rc = g_fs_base.stat(ctx, path, st);
  slow_check("stat", path, t0);
  return rc;
}
static int fs_dir_open(void *ctx, const char *path, void **dir) {
  uint64_t t0;
  int rc;
  flush_if_own_folder(path);
  t0 = now_ms(NULL);
  rc = g_fs_base.dir_open(ctx, path, dir);
  slow_check("dir_open", path, t0);
  return rc;
}
static int fs_file_open(void *ctx, const char *path, void **file) {
  uint64_t t0;
  int rc;
  flush_if_own_folder(path);
  t0 = now_ms(NULL);
  rc = g_fs_base.file_open(ctx, path, file);
  slow_check("file_open", path, t0);
  return rc;
}
static long fs_file_read(void *ctx, void *file, void *buf, size_t n) {
  uint64_t t0 = now_ms(NULL);
  long r = g_fs_base.file_read(ctx, file, buf, n);
  slow_check("file_read", NULL, t0);
  return r;
}
static void fs_file_close(void *ctx, void *file) {
  uint64_t t0 = now_ms(NULL);
  g_fs_base.file_close(ctx, file);
  slow_check("file_close", NULL, t0);
}
static int fs_mkdir(void *ctx, const char *path) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.mkdir(ctx, path);
  slow_check("mkdir", path, t0);
  return rc;
}
static int fs_file_create(void *ctx, const char *path, void **file) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.file_create(ctx, path, file);
  slow_check("file_create", path, t0);
  return rc;
}
static long fs_file_write(void *ctx, void *file, const void *buf, size_t n) {
  uint64_t t0 = now_ms(NULL);
  long r = g_fs_base.file_write(ctx, file, buf, n);
  slow_check("file_write", NULL, t0);
  return r;
}
static int fs_file_sync(void *ctx, void *file) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.file_sync(ctx, file);
  slow_check("file_sync", NULL, t0);
  return rc;
}
static int fs_rename(void *ctx, const char *from, const char *to) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.rename(ctx, from, to);
  slow_check("rename", to, t0);
  return rc;
}
static int fs_remove(void *ctx, const char *path) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.remove_file(ctx, path);
  slow_check("remove", path, t0);
  return rc;
}

static int fs_file_append(void *ctx, const char *path, void **file) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.file_append(ctx, path, file);
  slow_check("file_append", path, t0);
  return rc;
}
static int fs_remove_dir(void *ctx, const char *path) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.remove_dir(ctx, path);
  slow_check("rmdir", path, t0);
  return rc;
}

static u32 *g_soc_buf = NULL;
static bool g_soc_up = false, g_ndm_locked = false, g_ps_ok = false;
static bool g_wifi = false, g_listening = false, g_web_on = AGENT_WEB_DEFAULT, g_web_listening = false;
static ndp_web_assets g_web_assets;
static in_addr_t g_ip = 0;
static char g_err[64] = "";
static uint64_t g_next_soc_try = 0, g_next_listen_try = 0, g_last_check = 0;
static Result g_last_acu = 0;
static bool g_first_check = true;
static uint64_t g_start_ms = 0;
static uint64_t g_last_flush_ms = 0, g_last_req_ms = 0;
static uint32_t g_seen_requests = 0;

static uint64_t now_ms(void *ctx) {
  (void)ctx;
  return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

static void srv_log(void *ctx, const char *line) {
  (void)ctx;
  alog("%s", line);
}

/* Secure random from the console's PS service. There is deliberately no fallback: a weak nonce would
 * undermine authentication, so when PS fails the agent reports an error and refuses to pair/authenticate. */
static int random_bytes(void *ctx, uint8_t *out, size_t n) {
  (void)ctx;
  if (g_ps_ok && R_SUCCEEDED(PS_GenerateRandomBytes(out, n))) return 0;
  alog("ERR: PS random unavailable");
  return -1;
}

static void draw(uint64_t now);
static bool g_cfgu_ok = false;

/* DEVICE_INFO: what this console can report about itself. Fields it cannot read are simply left out. The SD free-space
 * query may take a moment on a big card (the OS reads the FAT), so it is only done when a client asks. */
static int device_info(void *ctx, ndp_device_info *di) {
  static const char *const models[] = {"Nintendo 3DS", "Nintendo 3DS XL", "New Nintendo 3DS", "Nintendo 2DS",
                                       "New Nintendo 3DS XL", "New Nintendo 2DS XL"};
  u8 model = 0xFF;
  bool is_new = false, known = false;
  FS_ArchiveResource res;
  (void)ctx;
  if (g_cfgu_ok && R_SUCCEEDED(CFGU_GetSystemModel(&model)) && model < sizeof models / sizeof models[0]) {
    snprintf(di->model, sizeof di->model, "%s", models[model]);
    di->has |= NDP_DI_MODEL;
    is_new = model == CFG_MODEL_N3DS || model == CFG_MODEL_N3DSXL || model == CFG_MODEL_N2DSXL;
    known = true;
  } else if (R_SUCCEEDED(APT_CheckNew3DS(&is_new))) {
    snprintf(di->model, sizeof di->model, "%s", is_new ? "New 3DS family" : "Old 3DS family");
    di->has |= NDP_DI_MODEL;
    known = true;
  }
  if (known) {
    di->ram_total = (uint64_t)(is_new ? 256u : 128u) << 20;
    di->has |= NDP_DI_RAM_TOTAL;
  }
  if (R_SUCCEEDED(osGetSystemVersionDataString(NULL, NULL, di->firmware, sizeof di->firmware))) {
    di->has |= NDP_DI_FIRMWARE;
  } else {
    u32 k = osGetKernelVersion();
    snprintf(di->firmware, sizeof di->firmware, "kernel %lu.%lu-%lu", (unsigned long)GET_VERSION_MAJOR(k),
             (unsigned long)GET_VERSION_MINOR(k), (unsigned long)GET_VERSION_REVISION(k));
    di->has |= NDP_DI_FIRMWARE;
  }
  /* The application region is deliberately NOT reported: the agent takes all of it at start-up (so "free" would always
   * be 0) and its size is the memory mode of the agent's own CIA (64 MB), not what a game would get (up to 124 MB). */
  di->sys_mem_total = osGetMemRegionSize(MEMREGION_SYSTEM);
  di->sys_mem_free = osGetMemRegionFree(MEMREGION_SYSTEM);
  di->has |= NDP_DI_SYS_MEM;
  if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&res))) {
    di->sd_total = (uint64_t)res.clusterSize * res.totalClusters;
    di->sd_free = (uint64_t)res.clusterSize * res.freeClusters;
    di->has |= NDP_DI_SD;
  }
  return 0;
}

/* The page (or ndev) asked to install from a URL. Only taken while the "Receive from the browser" screen is open; the
 * install flow then probes the file and asks the owner to confirm before anything is downloaded. */
static bool g_is_new3ds = false, g_charging = false, g_adapter = false, g_mcu_ok = false, g_ptm_ok = false;
static int g_battery = -1;
#ifndef AGENT_TEXT_UI
/* The camera must not stay on when the app is suspended (HOME, sleep): switched off there, back on when the app returns. */
static bool g_qr_resume = false;
static aptHookCookie g_apt_cookie;
static void apt_hook(APT_HookType hook, void *param);
static void power_wake(void);
static char g_seed_msg[64] = "", g_pending_msg[40] = "";
static bool g_seeds_dir = false;
static void refresh_pending(void) {
  unsigned n;
  uint64_t b;
  struct stat sb;
  installer_pending(&n, &b);
  g_seeds_dir = stat("sdmc:/fbi/seed", &sb) == 0 && S_ISDIR(sb.st_mode);
  if (n) snprintf(g_pending_msg, sizeof g_pending_msg, "%u (%u MiB)", n, (unsigned)(b >> 20));
  else g_pending_msg[0] = '\0';
}
static bool g_recv_open = false, g_qr_wanted = false, g_qr_restart = false, g_qr_bad = false, g_qr_inner = false;
static void apt_hook(APT_HookType hook, void *param) {
  (void)param;
  if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
    power_wake();
    g_qr_resume = qr_active();
    if (g_qr_resume) { qr_stop(); alog("QR scanner stopped (the app is being suspended)"); }
  } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
    if (g_qr_resume && g_qr_wanted) (void)qr_start(g_qr_inner);
    g_qr_resume = false;
  }
}
#endif


/* Install-time power saving: the bottom backlight goes off after a short idle, the top one after a longer idle.
 * Any button or touch wakes them (that first press is swallowed so it cannot cancel by accident). */
#define PW_BOTTOM_IDLE_MS 15000
#define PW_TOP_IDLE_MS 90000
/* The backlight service is held open only while a screen is off: an app that keeps it open all the time made the HOME button
 * freeze the console. */
static bool g_lcd_open = false, g_pw_bot_off = false, g_pw_top_off = false;
static void power_wake(void) {
  if (!g_lcd_open) return;
  if (g_pw_bot_off) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM); g_pw_bot_off = false; }
  if (g_pw_top_off) { GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_TOP); g_pw_top_off = false; }
  gspLcdExit();
  g_lcd_open = false;
}
static bool lcd_open(void) {
  if (!g_lcd_open) g_lcd_open = R_SUCCEEDED(gspLcdInit());
  return g_lcd_open;
}
/* Returns true when the press only woke the screens (down is cleared then). */
static bool power_step(bool installing, u32 *down, u32 held, uint64_t now) {
  static uint64_t last_input = 0;
  static bool keep = false, was_installing = false;
  bool input = (*down != 0) || (held & KEY_TOUCH);
  bool woke = false;
  if (installing && !was_installing) {
    struct stat kb;
    keep = stat("sdmc:/3ds/nintendo-dev-agent/config/keep-screens-on", &kb) == 0;
    last_input = now;
  }
  was_installing = installing;
  if (input) {
    last_input = now;
    if (g_pw_bot_off || g_pw_top_off) {
      power_wake();
      *down = 0;
      woke = true;
    }
  }
  if (!installing || keep) {
    power_wake();
    return woke;
  }
  if (!g_pw_bot_off && now - last_input >= PW_BOTTOM_IDLE_MS && lcd_open()) { GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM); g_pw_bot_off = true; }
  if (!g_pw_top_off && now - last_input >= PW_TOP_IDLE_MS && lcd_open()) { GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_TOP); g_pw_top_off = true; }
  return woke;
}

/* A link sent by the browser while the Sources screen is open: handed off to the main thread (which owns sources.c)
 * instead of being applied from the network thread. */
#ifndef AGENT_TEXT_UI
static bool g_src_recv_open = false;
static LightLock g_src_link_lock;
static char g_src_link_pending[300];
static volatile bool g_src_link_ready = false;
#endif

static int source_link_cb(void *ctx, const char *url, char *detail, size_t cap) {
  (void)ctx;
#ifndef AGENT_TEXT_UI
  if (!g_src_recv_open) { snprintf(detail, cap, "open Sources on the console first"); return NDP_ST_BUSY; }
  LightLock_Lock(&g_src_link_lock);
  snprintf(g_src_link_pending, sizeof g_src_link_pending, "%s", url);
  LightLock_Unlock(&g_src_link_lock);
  __atomic_store_n(&g_src_link_ready, true, __ATOMIC_RELEASE);
  return NDP_OK;
#else
  (void)url;
  snprintf(detail, cap, "this build has no Sources screen");
  return NDP_ST_UNSUPPORTED_COMMAND;
#endif
}

static int install_url_cb(void *ctx, const char *url, const char *path, const uint8_t *sha256, const uint8_t *seed, char *detail, size_t cap) {
  (void)ctx;
#ifndef AGENT_TEXT_UI
  if (!g_recv_open) { snprintf(detail, cap, "open Installer > Receive from the browser on the console first"); return NDP_ST_BUSY; }
  if (installer_busy()) { snprintf(detail, cap, "an install is already in progress or waiting for confirmation"); return NDP_ST_BUSY; }
  if (path) {
    char full[NDP_PATH_MAX + 8];
    snprintf(full, sizeof full, "sdmc:%s", path);
    if (!installer_start_file(full, sha256, seed)) { snprintf(detail, cap, "the console is busy updating or installing"); return NDP_ST_BUSY; }
    alog("Install: request from the browser (file %.60s)", path);
    return NDP_OK;
  }
  if (!installer_start_url(url, sha256, seed)) { snprintf(detail, cap, "the console is busy updating or installing"); return NDP_ST_BUSY; }
  alog("Install: request from the browser (%.60s)", url);
  return NDP_OK;
#else
  (void)url; (void)path; (void)sha256; (void)seed;
  snprintf(detail, cap, "this build has no installer");
  return NDP_ST_UNSUPPORTED_COMMAND;
#endif
}

static ndp_keystore g_keys;
static ndp_access g_access; /* the folders the owner opened (edited on the console, never over the network) */
static bool g_keys_dirty_warn = false;     /* the last save failed */
static uint64_t g_forget_armed_until = 0;  /* SELECT pressed once: a second press before this time forgets everything */

#ifndef AGENT_TEXT_UI
/* Used by the Sources screen's destination picker: true when `path` ("/roms", no "sdmc:") is open for writing, the same rule
 * the network protocol enforces (a parent folder opened for writing covers its sub-folders; protected zones never qualify). */
bool nsd_write_allowed(const char *path) {
  char norm[NDP_PATH_MAX + 1];
  if (ndp_path_normalize((const uint8_t *)path, strlen(path), norm, sizeof norm) != NDP_OK) return false;
  return ndp_access_level(&g_access, norm, NULL) == NDP_LVL_WRITE;
}
#endif

/* The key store is written by the server after a pairing/forget; it lives in the config folder, which
 * no protocol command can read or write. */
static void keys_changed(void *ctx, const ndp_keystore *keys) {
  int rc;
  (void)ctx;
  rc = ndp_keystore_save_file(keys, KEYS_FILE);
  g_keys_dirty_warn = rc != 0;
  if (rc != 0) alog("ERR: cannot save pairing keys (%d): pairing will be lost when the agent exits", rc);
}

/* Applies the owner's list as the access policy and saves it. Called by the folder editor after each change. */
static int access_changed(const ndp_access *list) {
  static ndp_policy pol; /* ~8 KB: static, the main thread's stack is only 32 KB */
  int rc;
  ndp_access_to_policy(list, &pol);
  ndp_server_set_policy(&g_srv, &pol);
  rc = ndp_access_save_file(list, ACCESS_FILE);
  if (rc != 0) alog("ERR: cannot save the folder list (%d): it will be lost when the agent exits", rc);
  return rc;
}

/* Opens the pairing window: the console generates the code and shows it on the TOP screen only —
 * it is never written to the log (which the protocol can read). */
static void open_pairing(void) {
  uint8_t code[NDP_CODE_BYTES];
  if (!AGENT_AUTH_REQUIRED) return;
  if (ndp_server_pairing_remaining_ms(&g_srv) > 0) {
    ndp_server_close_pairing(&g_srv);
    memset(code, 0, sizeof code);
    return;
  }
  if (random_bytes(NULL, code, sizeof code) != 0) {
    alog("Pairing unavailable: no secure random source");
    return;
  }
  ndp_server_open_pairing(&g_srv, code, PAIRING_WINDOW_MS);
  memset(code, 0, sizeof code);
}

static void set_err(const char *fmt, unsigned long v) { snprintf(g_err, sizeof g_err, fmt, v); }

static bool start_soc(uint64_t now) {
  Result r;
  if (now < g_next_soc_try) return false;
  if (!g_soc_buf) {
    g_soc_size = SOC_BUFSIZE;
    g_soc_buf = (u32 *)memalign(SOC_ALIGN, g_soc_size);
    if (!g_soc_buf) { g_soc_size = SOC_BUFSIZE_MIN; g_soc_buf = (u32 *)memalign(SOC_ALIGN, g_soc_size); } /* not enough memory for 4 MiB: the old size */
  }
  if (!g_soc_buf) {
    alog("ERROR: cannot allocate soc buffer");
    snprintf(g_err, sizeof g_err, "no memory for soc buffer");
    g_next_soc_try = now + 5000;
    return false;
  }
  r = socInit(g_soc_buf, g_soc_size);
  if (R_FAILED(r)) {
    alog("ERROR: socInit 0x%08lX", (unsigned long)r);
    set_err("socInit failed 0x%08lX", (unsigned long)r);
    free(g_soc_buf);
    g_soc_buf = NULL;
    g_next_soc_try = now + 3000;
    return false;
  }
  g_soc_up = true;
  aptSetSleepAllowed(false);
  r = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
  if (R_FAILED(r)) {
    alog("WARN: NDM exclusive state 0x%08lX", (unsigned long)r);
  } else {
    r = NDMU_LockState();
    if (R_FAILED(r)) {
      alog("WARN: NDM lock 0x%08lX", (unsigned long)r);
      NDMU_LeaveExclusiveState();
    } else {
      g_ndm_locked = true;
    }
  }
  alog("Network services ready");
  return true;
}

static void stop_listening(void) {
  ndp_server_close(&g_srv); /* also closes the web listener and the page's clients */
  g_listening = false;
  g_web_listening = false;
}

/* (Re)starts or stops the web listener to match the switch (R) and the state of the NDP listener. */
static void sync_web(void) {
  int rc;
  g_web_listening = false;
  if (!g_web_on || !g_listening) {
    ndp_server_web_close(&g_srv);
    return;
  }
  rc = ndp_server_web_listen(&g_srv, g_ip, AGENT_WEB_PORT);
  if (rc == 0) {
    struct in_addr ia;
    ia.s_addr = g_ip;
    g_web_listening = true;
    alog("Web page: http://%s:%u", inet_ntoa(ia), (unsigned)g_srv.web_port);
  } else {
    alog("WARN: web page not started (%d: %s)", rc, strerror(-rc));
  }
}

static bool ip_usable(in_addr_t ip) { return ip != 0 && ip != (in_addr_t)0xFFFFFFFFu; }

/* Every 500 ms: Wi-Fi state, soc, IP, (re)listen. Returns true when something visible changed. */
static bool service_network(uint64_t now) {
  bool changed = false, up;
  u32 st = 0;
  Result r;
  if (!g_first_check && now - g_last_check < 500) return false;
  g_first_check = false;
  g_last_check = now;

  r = ACU_GetWifiStatus(&st);
  if (r != g_last_acu) { alog("ACU_GetWifiStatus result 0x%08lX", (unsigned long)r); g_last_acu = r; }
  up = R_SUCCEEDED(r) && st != 0;
  if (up != g_wifi) {
    g_wifi = up;
    changed = true;
    if (up) alog("Wi-Fi connected (status %lu)", (unsigned long)st);
    else {
      alog("Wi-Fi lost");
      stop_listening();
      g_ip = 0;
    }
  }
  if (!up) return changed;

  if (!g_soc_up && !start_soc(now)) return true;

  {
    in_addr_t ip = (in_addr_t)gethostid();
    if (!ip_usable(ip)) {
      if (g_listening) { alog("IP address lost"); stop_listening(); changed = true; }
      g_ip = 0;
      snprintf(g_err, sizeof g_err, "waiting for an IP address");
      return changed;
    }
    if (!g_listening || ip != g_ip) {
      int rc;
      if (now < g_next_listen_try) return changed;
      if (g_listening) alog("IP changed, re-listening");
      stop_listening();
      rc = ndp_server_listen(&g_srv, ip, AGENT_PORT);
      changed = true;
      if (rc == 0) {
        struct in_addr ia;
        ia.s_addr = ip;
        g_listening = true;
        g_ip = ip;
        g_err[0] = '\0';
        alog("Listening on %s:%u", inet_ntoa(ia), (unsigned)g_srv.port);
        sync_web();
      } else {
        alog("ERROR: listen failed (%d: %s)", rc, strerror(-rc));
        snprintf(g_err, sizeof g_err, "listen failed: %s", strerror(-rc));
        g_next_listen_try = now + 2000;
      }
    }
  }
  return changed;
}

#ifdef AGENT_TEXT_UI

static void hms(char *out, size_t cap, uint64_t ms) {
  unsigned long s = (unsigned long)(ms / 1000);
  snprintf(out, cap, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

/* Rows a log line occupies on the bottom console: BOT_COLS, then 2-space-indented continuations. */
static int wrapped_rows(const char *s) {
  int len = (int)strlen(s);
  if (len <= BOT_COLS) return 1;
  return 1 + (len - BOT_COLS + (BOT_COLS - 2) - 1) / (BOT_COLS - 2);
}

static void print_wrapped(const char *s) {
  int len = (int)strlen(s), pos = 0, first = 1;
  while (pos < len || first) {
    int width = first ? BOT_COLS : BOT_COLS - 2;
    if (!first) printf("  ");
    printf("%.*s\n", width, s + pos);
    pos += width;
    first = 0;
  }
}

static void draw(uint64_t now) {
  char up[16];
  int n, i, rows;
  const char *color, *status;

  if (g_listening) { color = C_GREEN; status = "ONLINE"; }
  else if (!g_wifi) { color = C_YELLOW; status = "WAITING FOR Wi-Fi"; }
  else if (g_err[0]) { color = C_RED; status = "NETWORK ERROR"; }
  else { color = C_YELLOW; status = "STARTING NETWORK"; }

  consoleSelect(&g_top);
  consoleClear();
  printf(C_CYAN "NSD Bridge" C_RESET "   v%s\n", NDP_AGENT_VERSION);
  printf("Protocol %d\n\n", NDP_PROTOCOL_VERSION);
  printf("Status : %s%s" C_RESET "\n", color, status);
  if (g_listening) {
    struct in_addr ia;
    ia.s_addr = g_ip;
    printf("IP     : %s\n", inet_ntoa(ia));
    printf("Port   : %u\n", (unsigned)g_srv.port);
  } else {
    printf("IP     : -\nPort   : %u\n", (unsigned)AGENT_PORT);
  }
  if (g_web_listening) {
    struct in_addr ia;
    ia.s_addr = g_ip;
    printf("Page   : " C_GREEN "http://%s:%u" C_RESET "\n", inet_ntoa(ia), (unsigned)g_srv.web_port);
    printf("Browser: %s%s\n", g_srv.web_client_fd >= 0 ? C_GREEN "CONNECTED " C_RESET : "not connected", g_srv.web_client_fd >= 0 ? g_srv.web_peer : "");
  } else {
    printf("Page   : %s\n\n", g_web_on ? "starting..." : "off (press R to turn on)");
  }
  if (g_srv.client_fd >= 0) printf("Bridge : " C_GREEN "CONNECTED" C_RESET " %s\n", g_srv.peer);
  else printf("Bridge : not connected\n");
  if (g_srv.agent_cfg.mode == NDP_MODE_READ_ONLY) printf("Mode   : " C_GREEN "READ_ONLY" C_RESET " (no writes)\n");
  else printf("Mode   : " C_YELLOW "%s" C_RESET " (writes on)\n", ndp_mode_name(g_srv.agent_cfg.mode));
  printf("Write  : /3ds/nintendo-dev-agent\n");
  {
    int reads = g_access.count, writes = 0, i;
    bool whole = false;
    for (i = 0; i < g_access.count; i++) {
      if (g_access.e[i].level == NDP_LVL_WRITE) writes++;
      if (strcmp(g_access.e[i].path, "/") == 0) whole = true;
    }
    if (whole) printf("Open   : " C_RED "WHOLE SD CARD" C_RESET " (read%s)\n", writes ? "+write" : "");
    else printf("Open   : agent folder + %d more (%d writable)\n", reads, writes);
  }
  if (!AGENT_AUTH_REQUIRED) printf("Auth   : " C_RED "NONE" C_RESET " (test build)\n");
  else printf("Auth   : required - %d paired\n", g_srv.keys.count);
  hms(up, sizeof up, now - g_start_ms);
  printf("Uptime : %s   Requests: %lu\n", up, (unsigned long)g_srv.requests);
  if (g_err[0]) printf("\n" C_RED "%s" C_RESET "\n", g_err);
  if (g_keys_dirty_warn) printf(C_RED "Pairing not saved (SD error)" C_RESET "\n");
  if (AGENT_AUTH_REQUIRED) {
    uint32_t left = ndp_server_pairing_remaining_ms(&g_srv);
    if (ndp_server_pair2_pending(&g_srv)) {
      /* a page or computer asked to pair: the number is shown here and there; the person compares and answers */
      unsigned sas = (unsigned)g_srv.pairing.p2_sas;
      printf("\n" C_YELLOW "PAIRING REQUEST" C_RESET " from %s\n\"%s\"\nNumber: " C_CYAN "%03u %03u" C_RESET "\n"
             "Same number on the page / computer?\n" C_GREEN "A = YES, pair" C_RESET "   " C_RED "B = NO" C_RESET "\n",
             g_srv.pairing.p2_peer, g_srv.pairing.p2_label, sas / 1000u, sas % 1000u);
      {
        int rep = ndp_keystore_replace_target(&g_srv.keys, g_srv.pairing.p2_label);
        if (rep >= 0) printf("(replaces the earlier pairing \"%s\")\n", g_srv.keys.keys[rep].label);
      }
    } else if (left > 0) {
      char text[NDP_CODE_TEXT];
      ndp_code_encode(g_srv.pairing.code, text);
      printf("\n" C_YELLOW "PAIRING OPEN" C_RESET " (%lu s)\nOn the page: click Pair. Or: ndev pair <ip>\n"
             "Older way, the code: " C_CYAN "%s" C_RESET "\n",
             (unsigned long)((left + 999) / 1000), text);
    } else if (g_forget_armed_until > now) {
      printf("\n" C_RED "Press SELECT again to forget ALL paired computers" C_RESET "\n");
    } else {
      printf("\n\n\n");
    }
  }
  printf("\nA = folders   Y = pair   X = mode   R = web page\nSELECT x2 = forget pairings   START = Exit\n");

  consoleSelect(&g_bot);
  consoleClear();
  printf(C_CYAN "Recent activity" C_RESET "\n\n");
  rows = 0;
  for (n = 0; alog_get(n); n++) {
    int r = wrapped_rows(alog_get(n));
    if (rows + r > BOT_ROWS) break;
    rows += r;
  }
  for (i = n - 1; i >= 0; i--) print_wrapped(alog_get(i));

  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();
}

#else /* graphical UI */

static char g_sd_free[24] = "";

/* Free space on the SD card, as text for the info bar. The OS reads the FAT for this (slow on a big card), so it is
 * done at start-up and when the person comes back from a sub-screen, never while a computer waits. */
static void refresh_sd_free(void) {
  FS_ArchiveResource res;
  if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&res))) {
    double gib = (double)res.clusterSize * res.freeClusters / (1024.0 * 1024.0 * 1024.0);
    snprintf(g_sd_free, sizeof g_sd_free, "%.1f GiB", gib);
  }
}

static void build_model(ui_model *m, uint64_t now) {
  int i;
  memset(m, 0, sizeof *m);
  m->version = NDP_AGENT_VERSION;
#ifdef NDEV_BUILD_TAG
  m->build_tag = NDEV_BUILD_TAG;
#else
  m->build_tag = "";
#endif
  m->net = g_listening ? 3 : !g_wifi ? 1 : g_err[0] ? 2 : 0;
  m->err = g_err;
  if (g_listening) {
    struct in_addr ia;
    ia.s_addr = g_ip;
    snprintf(m->ip, sizeof m->ip, "%s", inet_ntoa(ia));
  }
  m->port = (unsigned)g_srv.port ? (unsigned)g_srv.port : (unsigned)AGENT_PORT;
  m->web_port = (unsigned)g_srv.web_port;
  m->web_on = g_web_on;
  m->web_up = g_web_listening;
  m->web_open = g_srv.web_open;
  m->web_peer = g_srv.web_client_fd >= 0 ? g_srv.web_peer : "";
  m->bridge_peer = g_srv.client_fd >= 0 ? g_srv.peer : "";
  m->read_only = g_srv.agent_cfg.mode == NDP_MODE_READ_ONLY;
  m->mode_name = ndp_mode_name(g_srv.agent_cfg.mode);
  m->open_folders = g_access.count;
  for (i = 0; i < g_access.count; i++) {
    if (g_access.e[i].level == NDP_LVL_WRITE) m->writable_folders++;
    if (strcmp(g_access.e[i].path, "/") == 0) m->whole_sd = true;
  }
  m->auth_required = AGENT_AUTH_REQUIRED;
  m->paired_n = g_srv.keys.count;
  for (i = 0; i < g_srv.keys.count && i < NDP_MAX_KEYS; i++) snprintf(m->paired_label[i], sizeof m->paired_label[i], "%s", g_srv.keys.keys[i].label);
  if (AGENT_AUTH_REQUIRED) {
    uint32_t left = ndp_server_pairing_remaining_ms(&g_srv);
    if (ndp_server_pair2_pending(&g_srv)) {
      int rep;
      m->pairing = 2;
      m->sas = (unsigned)g_srv.pairing.p2_sas;
      m->p2_peer = g_srv.pairing.p2_peer;
      m->p2_label = g_srv.pairing.p2_label;
      rep = ndp_keystore_replace_target(&g_srv.keys, g_srv.pairing.p2_label);
      m->p2_replaces = rep >= 0 ? g_srv.keys.keys[rep].label : "";
    } else if (left > 0) {
      m->pairing = 1;
      m->left_s = (left + 999) / 1000;
      ndp_code_encode(g_srv.pairing.code, m->code);
    }
    m->forget_armed = g_forget_armed_until > now;
  }
  m->keys_warn = g_keys_dirty_warn;
  m->is_new3ds = g_is_new3ds;
  m->recv_open = g_recv_open;
  m->battery = g_battery;
  m->charging = g_charging;
  m->adapter = g_adapter;
  snprintf(m->seed_msg, sizeof m->seed_msg, "%s", g_seed_msg);
  snprintf(m->pending_msg, sizeof m->pending_msg, "%s", g_pending_msg);
  m->seeds_dir = g_seeds_dir;
  m->qr_on = qr_active();
  m->qr_inner = qr_active() ? qr_inner() : g_qr_inner;
  m->qr_bad = g_qr_bad;
  snprintf(m->qr_err, sizeof m->qr_err, "%s", qr_error());
  installer_get(&m->ins);
  snprintf(m->sd_free, sizeof m->sd_free, "%s", g_sd_free);
  m->uptime_s = (unsigned long)((now - g_start_ms) / 1000);
  m->requests = (unsigned long)g_srv.requests;
  {
    upd_status us;
    updater_get(&us);
    m->upd = us.state;
    m->upd_progress = us.progress;
    snprintf(m->upd_version, sizeof m->upd_version, "%s", us.version);
    snprintf(m->upd_message, sizeof m->upd_message, "%s", us.message);
  }
}

static void draw(uint64_t now) {
  ui_model m;
  build_model(&m, now);
  ui_draw(&m);
}
#endif /* AGENT_TEXT_UI */

/* Battery for the top screen: read every 10 seconds. Every change (and every failed read) goes to the log, so a level that
 * looks stuck can be told apart from a reading that stopped working. */
static void read_battery(uint64_t now) {
  static uint64_t next = 0;
  static int last_level = -2, last_charge = -1, last_adapter = -1, fails = 0;
  u8 level = 0, charge = 0;
  Result r1 = -1, r2 = -1, r3 = -1;
  bool plugged = false;
  if (now < next) return;
  next = now + 10000;
  if (g_mcu_ok && R_SUCCEEDED(r1 = MCUHWC_GetBatteryLevel(&level))) {
    /* The reading sometimes jumps (a log showed 78% / 100% / 78% every 10 s while charging): a change of more than 5 points
     * is only believed when the next reading agrees with it, so one wild value never reaches the screen. */
    static int pending = -1;
    int v = level > 100 ? 100 : level;
    if (g_battery < 0 || abs(v - g_battery) <= 5) { g_battery = v; pending = -1; }
    else if (pending >= 0 && abs(v - pending) <= 2) { g_battery = v; pending = -1; }
    else pending = v;
  }
  if (g_ptm_ok && R_SUCCEEDED(r2 = PTMU_GetBatteryChargeState(&charge))) g_charging = charge != 0;
  if (g_ptm_ok && R_SUCCEEDED(r3 = PTMU_GetAdapterState(&plugged))) g_adapter = plugged;
  if (R_FAILED(r1) || R_FAILED(r2) || R_FAILED(r3)) {
    if (fails++ < 3) alog("Battery: read failed (level 0x%08lX, charge 0x%08lX, adapter 0x%08lX)", (unsigned long)r1, (unsigned long)r2, (unsigned long)r3);
  }
  if (g_battery != last_level || (int)g_charging != last_charge || (int)g_adapter != last_adapter) {
    alog("Battery: %d%%%s%s", g_battery, g_charging ? " charging" : "", g_adapter ? " (charger plugged in)" : "");
    last_level = g_battery;
    last_charge = (int)g_charging;
    last_adapter = (int)g_adapter;
  }
}

int main(int argc, char **argv) {
  ndp_agent_config cfg;
  ndp_server_platform plat;
  uint64_t last_draw = 0;
  bool draw_pending = false, draw_now = false, is_new3ds = false;
  Result r;

  osSetSpeedupEnable(true);
  gfxInitDefault();
#ifdef AGENT_TEXT_UI
  consoleInit(GFX_TOP, &g_top);
  consoleInit(GFX_BOTTOM, &g_bot);
#else
  ui_init();
#endif
  alog_init();
#ifndef AGENT_TEXT_UI
  aptHook(&g_apt_cookie, apt_hook, NULL);
#endif
  g_start_ms = now_ms(NULL);

  alog("NSD Bridge v%s, protocol %d", NDP_AGENT_VERSION, NDP_PROTOCOL_VERSION);
  if (R_SUCCEEDED(APT_CheckNew3DS(&is_new3ds))) {
    g_is_new3ds = is_new3ds;
    alog("Console: %s", is_new3ds ? "New 3DS family" : "Old 3DS family");
  }
  r = acInit();
  alog("acInit: 0x%08lX", (unsigned long)r);
  r = psInit();
  g_ps_ok = R_SUCCEEDED(r);
  alog("psInit: 0x%08lX", (unsigned long)r);
  g_cfgu_ok = R_SUCCEEDED(cfguInit());
  alog("cfguInit: %s", g_cfgu_ok ? "ok" : "failed (model falls back to APT)");
  r = ndmuInit();
  alog("ndmuInit: 0x%08lX", (unsigned long)r);
  g_mcu_ok = R_SUCCEEDED(mcuHwcInit());
  g_ptm_ok = R_SUCCEEDED(ptmuInit());

  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "3ds";
  cfg.agent_version = NDP_AGENT_VERSION;
  cfg.mode = AGENT_START_MODE;
  cfg.auth = AGENT_AUTH_REQUIRED ? "required" : "none";
  cfg.max_frame = NDP_DEFAULT_MAX_FRAME;
  cfg.random_bytes = random_bytes;
  cfg.device_info = device_info;
  cfg.install_url = install_url_cb;
  cfg.source_link = source_link_cb;
  if (ndp_posix_fs_init(&g_fs_base, &g_fs_ctx, "sdmc:") == 0) {
    g_fs = g_fs_base;
    g_fs.stat = fs_stat;
    g_fs.dir_open = fs_dir_open;
    g_fs.file_open = fs_file_open;
    g_fs.file_read = fs_file_read;
    g_fs.file_close = fs_file_close;
    g_fs.mkdir = fs_mkdir;
    g_fs.file_create = fs_file_create;
    g_fs.file_write = fs_file_write;
    g_fs.file_sync = fs_file_sync;
    g_fs.rename = fs_rename;
    g_fs.remove_file = fs_remove;
    g_fs.remove_dir = fs_remove_dir;
    g_fs.file_append = fs_file_append;
    cfg.fs = &g_fs; /* default policy: read everything except the agent's config; no writes */
  }
  memset(&plat, 0, sizeof plat);
  plat.now_ms = now_ms;
  plat.log = srv_log;
  plat.keys_changed = keys_changed;
  ndp_server_init(&g_srv, &plat, &cfg);
  g_web_assets.index_gz = ndp_web_index_gz;
  g_web_assets.index_gz_len = ndp_web_index_gz_len;
  g_web_assets.version = ndp_web_version;
  ndp_server_set_web(&g_srv, &g_web_assets);
  {
    /* the page needs no pairing unless this file exists (the CLI and the MCP always pair) */
    struct stat wb;
    ndp_server_set_web_open(&g_srv, stat(CONFIG_DIR "/require-web-pairing", &wb) != 0);
  }

  {
    struct stat sb;
    int ar;
    /* The config folder must exist before anything is saved. Creating a folder on the 3DS SD takes ~6 s
     * (measured), so it is done here, once, and not while a computer waits for a reply. */
    if (stat(CONFIG_DIR, &sb) != 0) {
      alog("Creating %s (one time, ~6 s)...", CONFIG_DIR);
      draw(now_ms(NULL));
      if (mkdir(CONFIG_DIR, 0777) != 0) alog("ERR: cannot create the config folder");
    }
    ar = ndp_access_load_file(&g_access, ACCESS_FILE);
    if (ar < 0) alog("WARN: folder list is corrupt: ignored (only the agent's folder is open)");
    else if (ar == 0) alog("Loaded %d opened folder(s)", g_access.count);
    { static ndp_policy pol; ndp_access_to_policy(&g_access, &pol); ndp_server_set_policy(&g_srv, &pol); }
  }
  if (AGENT_AUTH_REQUIRED) {
    int lr = ndp_keystore_load_file(&g_keys, KEYS_FILE);
    if (lr < 0) alog("WARN: pairing file is corrupt: ignored (no computer is paired)");
    else if (lr == 0) alog("Loaded %d paired computer(s)", g_keys.count);
    if (!g_keys.has_device_id && random_bytes(NULL, g_keys.device_id, sizeof g_keys.device_id) == 0) {
      g_keys.has_device_id = 1;
      keys_changed(NULL, &g_keys);
    }
    ndp_server_set_keys(&g_srv, &g_keys);
  }

#ifndef AGENT_TEXT_UI
  refresh_sd_free();
#endif
#ifndef AGENT_TEXT_UI
  updater_init(argc > 0 ? argv[0] : NULL);
  installer_init();
  titles_init();
  tr_init();
  src_init();
  LightLock_Init(&g_src_link_lock);
  refresh_pending();
#else
  (void)argc;
  (void)argv;
#endif
  draw(now_ms(NULL));
  while (aptMainLoop()) {
    uint64_t now;
    bool changed;
#ifdef AGENT_TEXT_UI
    hidScanInput();
    if (!access_ui_active() && ndp_server_pair2_pending(&g_srv) && (hidKeysDown() & (KEY_A | KEY_B))) {
      int yes = (hidKeysDown() & KEY_A) != 0;
      alog(yes ? "Pairing approved" : "Pairing refused");
      (void)ndp_server_pair2_decide(&g_srv, yes);
      draw_pending = true;
    } else if (access_ui_active()) {
      /* the folder editor owns the buttons while it is open (START closes it, it does not quit the agent) */
      if (access_ui_handle(hidKeysDown())) draw_pending = true;
    } else if (hidKeysDown() & KEY_START) break;
    else if (hidKeysDown() & KEY_A) {
      access_ui_open(&g_access, access_changed);
      draw_pending = true;
    }
    if (!access_ui_active() && AGENT_AUTH_REQUIRED && (hidKeysDown() & KEY_Y)) {
      open_pairing();
      draw_pending = true;
    }
    if (!access_ui_active() && AGENT_AUTH_REQUIRED && (hidKeysDown() & KEY_SELECT)) {
      uint64_t t = now_ms(NULL);
      if (g_forget_armed_until > t) {
        ndp_server_clear_keys(&g_srv);
        g_forget_armed_until = 0;
      } else {
        g_forget_armed_until = t + FORGET_CONFIRM_MS;
      }
      draw_pending = true;
    }
    if (!access_ui_active() && (hidKeysDown() & KEY_R)) {
      g_web_on = !g_web_on;
      alog("Web page -> %s", g_web_on ? "on" : "off");
      sync_web();
      draw_pending = true;
    }
    if (!access_ui_active() && (hidKeysDown() & KEY_X)) {
      ndp_mode next = g_srv.agent_cfg.mode == NDP_MODE_READ_ONLY ? NDP_MODE_DEVELOPMENT : NDP_MODE_READ_ONLY;
      ndp_server_set_mode(&g_srv, next);
      alog("Mode -> %s", ndp_mode_name(next));
      draw_pending = true;
    }

#else
    if (__atomic_load_n(&g_src_link_ready, __ATOMIC_ACQUIRE)) {
      char link[300];
      LightLock_Lock(&g_src_link_lock);
      snprintf(link, sizeof link, "%s", g_src_link_pending);
      LightLock_Unlock(&g_src_link_lock);
      __atomic_store_n(&g_src_link_ready, false, __ATOMIC_RELEASE);
      if (src_set_link(link)) {
        alog("Sources: link set from the browser");
        uisrc_set_message("");
        if (uisrc_is_browsing()) uisrc_open_browse(); /* re-reads from the new share, from its root */
      }
      else alog("Sources: the browser sent a link that was not a Nextcloud/ownCloud share link");
      draw_pending = true;
    }
    hidScanInput();
    {
      touchPosition tp;
      u32 down = hidKeysDown(), held = hidKeysHeld();
      bool redraw = false;
      memset(&tp, 0, sizeof tp);
      {
        ins_status ps;
        installer_get(&ps);
        if (power_step(ps.state == INS_INSTALLING, &down, held, now_ms(NULL))) held &= ~KEY_TOUCH;
      }
      if (held & KEY_TOUCH) hidTouchRead(&tp);
      if (access_ui_active()) {
        /* the folder editor owns the buttons and the touch screen while it is open */
        if (access_ui_handle(down) || access_ui_touch(down, held, &tp)) redraw = true;
        if (!access_ui_active()) { refresh_sd_free(); redraw = true; }
      } else {
        ui_model um;
        ui_action act;
        uint64_t t = now_ms(NULL);
        build_model(&um, t);
        act = ui_handle(down, held, &tp, &um, &redraw);
        switch (act) {
          case UI_FOLDERS: access_ui_open(&g_access, access_changed); break;
          case UI_MODE: {
            ndp_mode next = g_srv.agent_cfg.mode == NDP_MODE_READ_ONLY ? NDP_MODE_DEVELOPMENT : NDP_MODE_READ_ONLY;
            ndp_server_set_mode(&g_srv, next);
            alog("Mode -> %s", ndp_mode_name(next));
            break;
          }
          case UI_PAIR: open_pairing(); break;
          case UI_WEB:
            g_web_on = !g_web_on;
            alog("Web page -> %s", g_web_on ? "on" : "off");
            sync_web();
            break;
          case UI_FORGET:
            if (!AGENT_AUTH_REQUIRED) break;
            if (g_forget_armed_until > t) {
              ndp_server_clear_keys(&g_srv);
              g_forget_armed_until = 0;
            } else {
              g_forget_armed_until = t + FORGET_CONFIRM_MS;
            }
            break;
          case UI_UNPAIR_ONE: {
            int idx = ui_arg();
            if (idx >= 0 && idx < g_srv.keys.count) {
              uint8_t kid[4];
              char label[NDP_LABEL_MAX + 1];
              memcpy(kid, g_srv.keys.keys[idx].key_id, 4);
              snprintf(label, sizeof label, "%s", g_srv.keys.keys[idx].label);
              if (ndp_server_forget_key(&g_srv, kid) == 0) alog("Unpaired \"%s\"", label);
            }
            break;
          }
          case UI_INST_SD:
            if (!installer_start_file(ui_arg_str(), NULL, NULL)) alog("Install: busy, not started");
            break;
          case UI_SRC_INSTALL:
            if (!installer_start_url(ui_arg_str(), NULL, NULL)) alog("Sources: busy, the .cia was not sent to the installer");
            break;
          case UI_TR_LINK: {
            /* the link of the translations share; the keyboard takes over the screen */
            static char link[300];
            SwkbdState kb;
            swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)sizeof link - 40);
            swkbdSetHintText(&kb, ui_pt() ? "https://.../s/xxxxxxxx   (- = padrão)" : "https://.../s/xxxxxxxx   (- = default)");
            swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
            swkbdSetFeatures(&kb, SWKBD_FIXED_WIDTH);
            if (tr_configured()) snprintf(link, sizeof link, "%s", tr_link());
            else link[0] = '\0';
            if (swkbdInputText(&kb, link, sizeof link) == SWKBD_BUTTON_CONFIRM && link[0]) {
              if (!strcmp(link, "-")) { tr_reset_link(); alog("Translations: back to the default link"); uitr_set_message(""); if (tr_configured()) tr_list(true); }
              else if (tr_set_link(link)) { alog("Translations: link set"); uitr_set_message(""); tr_list(true); }
              else uitr_set_message(ui_pt() ? "Link inválido (https://.../s/código)." : "Invalid link (https://.../s/code).");
            }
            draw_pending = true;
            break;
          }
          case UI_SRC_LINK: {
            static char link[300];
            SwkbdState kb;
            swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)sizeof link - 40);
            swkbdSetHintText(&kb, "https://.../s/xxxxxxxx");
            swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
            swkbdSetFeatures(&kb, SWKBD_FIXED_WIDTH);
            if (src_configured()) snprintf(link, sizeof link, "%s", src_link());
            else link[0] = '\0';
            if (swkbdInputText(&kb, link, sizeof link) == SWKBD_BUTTON_CONFIRM && link[0]) {
              if (!strcmp(link, "-")) { src_forget_link(); alog("Sources: link removed"); uisrc_set_message(""); }
              else if (src_set_link(link)) { alog("Sources: link set"); uisrc_set_message(""); }
              else uisrc_set_message(ui_pt() ? "Link inválido (https://.../s/código)." : "Invalid link (https://.../s/code).");
            }
            draw_pending = true;
            break;
          }
          case UI_SRC_RECV_ON: g_src_recv_open = true; break;
          case UI_SRC_RECV_OFF: g_src_recv_open = false; break;
          case UI_SRC_NEWFOLDER: {
            /* a sub-folder under the destination path the Sources screen is showing, created locally (no network command) */
            static char name[64], full[432];
            SwkbdState kb;
            const char *base = uisrc_dest_path();
            swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)sizeof name - 1);
            swkbdSetHintText(&kb, ui_pt() ? "nome da pasta" : "folder name");
            swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_BACKSLASH, 0);
            swkbdSetFeatures(&kb, SWKBD_FIXED_WIDTH);
            name[0] = '\0';
            if (swkbdInputText(&kb, name, sizeof name) == SWKBD_BUTTON_CONFIRM && name[0] && !strchr(name, '/') && strcmp(name, ".") &&
                strcmp(name, "..") && nsd_write_allowed(base)) {
              snprintf(full, sizeof full, "sdmc:%s%s%s", base, strcmp(base, "/") ? "/" : "", name);
              if (mkdir(full, 0777) == 0 || errno == EEXIST) uisrc_dest_refresh(name);
              else uisrc_set_message(ui_pt() ? "Não consegui criar a pasta." : "Could not create the folder.");
            }
            draw_pending = true;
            break;
          }
          case UI_INST_TYPE_URL: {
            /* the system keyboard takes over the screen; on return the next frame redraws */
            static char url[1100];
            SwkbdState kb;
            swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, (int)sizeof url - 80);
            swkbdSetHintText(&kb, "https://...  .cia");
            swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
            swkbdSetFeatures(&kb, SWKBD_FIXED_WIDTH);
            if (swkbdInputText(&kb, url, sizeof url) == SWKBD_BUTTON_CONFIRM && url[0]) {
              char full[1130];
              /* like FBI: a bare address is taken as http:// */
              if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) snprintf(full, sizeof full, "http://%s", url);
              else snprintf(full, sizeof full, "%s", url);
              if (!installer_start_url(full, NULL, NULL)) alog("Install: busy, not started");
            }
            draw_pending = true;
            break;
          }
          case UI_INST_RECV_ON: g_recv_open = true; alog("Install: receiving from the browser"); break;
          case UI_INST_RECV_OFF: g_recv_open = false; alog("Install: left Receive from the browser"); break;
          case UI_INST_QR_ON: g_qr_wanted = true; g_qr_bad = false; g_qr_restart = false; (void)qr_start(g_qr_inner); break;
          case UI_INST_QR_OFF: g_qr_wanted = false; qr_stop(); break;
          case UI_INST_QR_SWITCH: g_qr_inner = !g_qr_inner; qr_stop(); (void)qr_start(g_qr_inner); break;
          case UI_INST_CLEAN: installer_pending_clear(); refresh_pending(); alog("Cleared the partial downloads"); break;
          case UI_INST_SEEDS: installer_import_seeds(g_seed_msg, sizeof g_seed_msg); alog("Seeds: %s", g_seed_msg); break;
          case UI_INST_YES: installer_confirm(true); break;
          case UI_INST_NO: installer_confirm(false); break;
          case UI_INST_CANCEL: installer_cancel(); break;
          case UI_INST_DISMISS: installer_dismiss(); refresh_sd_free(); break;
          case UI_PAIR_YES:
          case UI_PAIR_NO:
            alog(act == UI_PAIR_YES ? "Pairing approved" : "Pairing refused");
            (void)ndp_server_pair2_decide(&g_srv, act == UI_PAIR_YES);
            break;
          case UI_UPDATE: {
            upd_status us;
            updater_get(&us);
            if (us.state == UPD_AVAILABLE) { alog("Update: installing %s", us.version); updater_download(); }
            else if (us.state != UPD_DONE) updater_check();
            break;
          }
          case UI_EXIT: goto quit;
          default: break;
        }
        if (act != UI_NONE) redraw = true;
      }
      if (redraw) { draw_pending = true; draw_now = true; }
    }
#endif
    now = now_ms(NULL);
    changed = service_network(now);
    read_battery(now);
#ifndef AGENT_TEXT_UI
    {
      /* once per run, a few seconds after the network is up (a file named no-update-check in the config folder turns
       * the automatic check off; the Updates menu entry still works) */
      static uint64_t listen_since = 0;
      static bool auto_checked = false;
      upd_status us;
      static unsigned last_rev = 0;
      if (!g_listening) listen_since = 0;
      else if (!listen_since) listen_since = now;
      if (!auto_checked && g_listening && now - listen_since >= 4000) {
        struct stat nb;
        auto_checked = true;
        if (stat(CONFIG_DIR "/no-update-check", &nb) != 0) updater_check();
      }
      updater_get(&us);
      if (us.revision != last_rev) { last_rev = us.revision; changed = true; }
      if (g_qr_wanted) {
        char code[1100];
        ins_status qi;
        installer_get(&qi);
        if (qr_take(code, sizeof code)) {
          qr_stop();
          if (!strncmp(code, "http://", 7) || !strncmp(code, "https://", 8)) {
            g_qr_bad = false;
            alog("QR code read: %.80s", code);
            if (!installer_start_url(code, NULL, NULL)) alog("Install: busy, not started");
            /* done with the camera: back to the Installer menu, it is not switched on again by itself (scan again from there) */
            g_qr_wanted = false;
            ui_show_installer_menu();
          } else {
            g_qr_bad = true;
            alog("QR code is not an http(s) address");
            g_qr_restart = true; /* not an address: keep scanning */
          }
          changed = true;
          draw_now = true;
        } else if (qr_active()) {
          static unsigned last_ser = 0;
          unsigned ser = qr_frame_serial();
          if (ser != last_ser && now - last_draw >= 100) { last_ser = ser; changed = true; draw_now = true; }
        } else if (g_qr_restart && qi.state == INS_IDLE) {
          g_qr_restart = false;
          (void)qr_start(g_qr_inner);
          changed = true;
        }
      }
      {
        static unsigned last_ins_rev = 0, last_titles_rev = 0, last_tr_rev = 0;
        { tr_status ts; tr_get(&ts); if (ts.revision != last_tr_rev) { last_tr_rev = ts.revision; changed = true; draw_now = true; } }
        if (titles_revision() != last_titles_rev) { last_titles_rev = titles_revision(); changed = true; draw_now = true; }
        ins_status is;
        installer_get(&is);
        /* while installing, progress redraws follow the normal throttle: every redraw takes CPU from the download */
        if (is.revision != last_ins_rev) { last_ins_rev = is.revision; changed = true; if (is.state != INS_INSTALLING) { draw_now = true; refresh_pending(); } }
      }
    }
#endif
    if (g_listening) {
      int s = ndp_server_step(&g_srv, 16);
      if (s < 0) {
        alog("Listener lost; will re-listen");
        stop_listening();
        g_ip = 0;
        g_next_listen_try = now + 1000;
        changed = true;
      } else if (s > 0) {
        changed = true;
      }
    } else {
      svcSleepThread(16 * 1000000LL);
    }

    now = now_ms(NULL);
    /* Flush the buffered SD log only when the link is quiet (or at least every 5 s). */
    if (g_srv.requests != g_seen_requests) { g_seen_requests = g_srv.requests; g_last_req_ms = now; }
    if (alog_unflushed() &&
        ((now - g_last_req_ms >= 250 && now - g_last_flush_ms >= 500) || now - g_last_flush_ms >= 5000)) {
      alog_flush();
      g_last_flush_ms = now;
    }
    if (changed || alog_dirty()) draw_pending = true;
    if ((draw_pending && (draw_now || now - last_draw >= DRAW_MIN_MS)) || now - last_draw >= DRAW_MAX_MS) {
      alog_clear_dirty();
      if (access_ui_active()) access_ui_draw(NULL, NULL);
      else draw(now);
      draw_pending = false;
      draw_now = false;
      last_draw = now;
    }
  }

#ifndef AGENT_TEXT_UI
quit:
#endif
  alog("Exiting");
#ifndef AGENT_TEXT_UI
  power_wake();
  qr_stop();
  aptUnhook(&g_apt_cookie);
  tr_shutdown();
  titles_shutdown();
  src_shutdown();
  installer_shutdown();
  updater_shutdown();
#endif
  ndp_server_close(&g_srv);
  if (g_ndm_locked) {
    NDMU_UnlockState();
    NDMU_LeaveExclusiveState();
  }
  aptSetSleepAllowed(true);
  if (g_soc_up) socExit();
  free(g_soc_buf);
  if (g_ps_ok) psExit();
  if (g_cfgu_ok) cfguExit();
  if (g_mcu_ok) mcuHwcExit();
  if (g_ptm_ok) ptmuExit();
  ndmuExit();
  acExit();
  alog_exit();
#ifndef AGENT_TEXT_UI
  ui_exit();
#endif
  gfxExit();
  return 0;
}
