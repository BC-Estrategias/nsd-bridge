/* NSD Bridge — DSi entry point. Real NDP protocol over wifi+SD, pairing (code + number comparison),
 * the embedded web page, and the on-console folder editor -- full feature parity with the 3DS agent
 * now: everything here -- the two-screen colored status UI, the activity log (alog), pairing,
 * access_ui -- is ported as closely to agent/3ds/source/main.c as the platforms' real differences
 * allow (DSi has no ACU/soc, no system Wi-Fi profile to reuse, so network bring-up is this
 * platform's own proven scan/connect flow instead; see spike/dsi-wifi-sd). Network bring-up (scan,
 * on-screen password entry, WIFI_ATTEMPT_DSI_MODE, cothread_yield_irq while waiting -- never
 * swiWaitForVBlank, or the lwIP cothread never gets scheduled and DHCP hangs forever) is that same
 * proven code.
 *
 * Starts READ_ONLY and closed except its own folder, exactly like the 3DS agent: nothing here
 * loosens that model, only the platform underneath it changes. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <fat.h>
#include <nds.h>
#include <dswifi9.h>

#include "access_ui.h"
#include "alog.h"
#include "entropy.h"
#include "ndp/ndp_defs.h"
#include "ndp/ndp_access_file.h"
#include "ndp/ndp_keystore_file.h"
#include "ndp/ndp_posix_fs.h"
#include "ndp/ndp_server.h"
#include "ndp_web_assets_data.h"

#define AGENT_PORT NDP_DEFAULT_PORT
/* The web page (file manager for the browser) is served on its own port by this same app -- same as
 * the 3DS agent. It is on when the app starts and R turns it off/on; it needs the same pairing as
 * the CLI, and the console stays READ_ONLY until X. */
#define AGENT_WEB_PORT 8080
#ifndef AGENT_WEB_DEFAULT
#define AGENT_WEB_DEFAULT 1
#endif
#ifndef AGENT_START_MODE
#define AGENT_START_MODE NDP_MODE_READ_ONLY
#endif
#ifndef AGENT_AUTH_REQUIRED
#define AGENT_AUTH_REQUIRED 1
#endif
#define AGENT_DIR "sd:/nsd-bridge"
#define CONFIG_DIR AGENT_DIR "/config"
#define KEYS_FILE CONFIG_DIR "/pairing.bin"
#define ACCESS_FILE CONFIG_DIR "/access.bin"
#define WIFI_CFG_PATH CONFIG_DIR "/wifi.cfg"
#define SCAN_SLOTS 32
#define PAIRING_WINDOW_MS 120000u
#define FORGET_CONFIRM_MS 5000u

#define C_RESET "\x1b[0m"
#define C_GREEN "\x1b[32;1m"
#define C_YELLOW "\x1b[33;1m"
#define C_RED "\x1b[31;1m"
#define C_CYAN "\x1b[36;1m"

/* The DSi's screens are 256x192 (32x24 chars in the default 8x8 font), smaller than the 3DS's
 * bottom screen (BOT_COLS 39/BOT_ROWS 27 there): sized down with the same one-row/one-column
 * margin. */
#define BOT_COLS 30
#define BOT_ROWS 20

/* Protocol-level paths (no "sd:" prefix: that is only for this file's own direct fopen/mkdir calls, added
 * internally by ndp_posix_fs_init for everything the protocol touches). Overrides agent/common's 3DS
 * defaults: this SD card has no /3ds or /luma, and has its own critical folder to protect instead
 * (/_nds, TWiLight Menu++ / nds-bootstrap's own files -- corrupting it is this platform's /luma). No
 * write exceptions inside it yet: unlike /luma/plugins on the 3DS, no subfolder there is known-safe to
 * open by default. */
#define PROTOCOL_WORKSPACE "/nsd-bridge"
static const char *const DSI_NEVER_READ[] = { "/nsd-bridge/config" };
static const char *const DSI_NEVER_WRITE[] = { "/_nds", "/nsd-bridge/config" };

static PrintConsole g_top, g_bot;
static ndp_server g_srv;
static ndp_access g_access;
static ndp_keystore g_keys;
static ndp_fs_ops g_fs;
static ndp_posix_fs_ctx g_fs_ctx;
static ndp_web_assets g_web_assets;
static uint64_t g_ms;
static uint64_t g_start_ms;
static uint64_t g_forget_armed_until = 0; /* SELECT pressed once: a second press before this time forgets everything */
static bool g_web_on = AGENT_WEB_DEFAULT, g_web_listening = false;
static uint32_t g_ip; /* s_addr, kept for sync_web() to (re)listen after R toggles it */
static bool g_listening = false; /* the NDP listener is currently up (net_check() tears it down/rebuilds it) */
static bool g_wifi_up = false, g_reconnecting = false;
static uint64_t g_next_reconnect_try = 0;
static Wifi_AccessPoint g_last_ap; /* the AP connect_wifi() last associated with: net_check() reuses it silently */
static char g_last_key[64];

/* ---- platform callbacks for ndp_server ---- */

static uint64_t now_ms(void *ctx) {
  (void)ctx;
  return g_ms;
}

static void srv_log(void *ctx, const char *line) {
  (void)ctx;
  alog("%s", line);
}

static void keys_changed(void *ctx, const ndp_keystore *keys) {
  (void)ctx;
  if (ndp_keystore_save_file(keys, KEYS_FILE) != 0) alog("WARN: pairing not saved (SD error)");
}

static int device_info(void *ctx, ndp_device_info *di) {
  struct statvfs sv;
  (void)ctx;
  memset(di, 0, sizeof *di);
  bool dsi = isDSiMode();
  snprintf(di->model, sizeof di->model, "%s", dsi ? "Nintendo DSi" : "Nintendo DS (DS mode)");
  di->has |= NDP_DI_MODEL;
  di->ram_total = (uint64_t)(dsi ? 16u : 4u) << 20; /* fixed by the hardware/mode, not a runtime query */
  di->has |= NDP_DI_RAM_TOTAL;
  /* Firmware version and system/app memory have no equivalent here: this is bare-metal homebrew, with
   * no OS layer to ask (unlike the 3DS's osGetSystemVersionDataString()/osGetMemRegionSize()) -- the
   * agent reports what it can measure and nothing else (spec: DEVICE_INFO never invents a value). */
  if (statvfs("sd:/", &sv) == 0) {
    di->sd_total = (uint64_t)sv.f_frsize * sv.f_blocks;
    di->sd_free = (uint64_t)sv.f_frsize * sv.f_bavail;
    di->has |= NDP_DI_SD;
  }
  return 0;
}

/* ---- access_ui (folder editor, A button): applies + persists every change ---- */

static int access_changed(const ndp_access *list) {
  static ndp_policy pol;
  int rc;
  ndp_access_to_policy(list, &pol);
  ndp_server_set_policy(&g_srv, &pol);
  rc = ndp_access_save_file(list, ACCESS_FILE);
  if (rc != 0) alog("ERR: cannot save the folder list (%d): it will be lost when the agent exits", rc);
  return rc;
}

/* ---- SD ---- */

static bool sd_init(void) {
  if (!fatInitDefault()) {
    printf("fatInitDefault() FAILED\n");
    return false;
  }
  if (mkdir(AGENT_DIR, 0777) != 0 && errno != EEXIST) printf("mkdir %s FAILED (%d)\n", AGENT_DIR, errno);
  if (mkdir(CONFIG_DIR, 0777) != 0 && errno != EEXIST) printf("mkdir %s FAILED (%d)\n", CONFIG_DIR, errno);
  return true;
}

/* ---- wifi: scan + on-screen password entry, saved so it is only typed once ----
 * (unchanged from spike/dsi-wifi-sd, which proved this connects reliably on real hardware) */

/* Several networks can be remembered at once (L opens the picker to add/switch to one without ever
 * forgetting the others): wifi.cfg is just ssid+key pairs, one network per two lines. */
#define MAX_SAVED_NETS 5
struct saved_net { char ssid[33]; char key[64]; };
static struct saved_net g_saved_nets[MAX_SAVED_NETS];
static int g_saved_count = 0;

static void load_saved_nets(void) {
  FILE *f = fopen(WIFI_CFG_PATH, "rb");
  g_saved_count = 0;
  if (!f) return;
  while (g_saved_count < MAX_SAVED_NETS) {
    struct saved_net n;
    memset(&n, 0, sizeof n);
    if (!fgets(n.ssid, sizeof n.ssid, f) || !fgets(n.key, sizeof n.key, f)) break;
    n.ssid[strcspn(n.ssid, "\r\n")] = '\0';
    n.key[strcspn(n.key, "\r\n")] = '\0';
    if (n.ssid[0] == '\0') break;
    g_saved_nets[g_saved_count++] = n;
  }
  fclose(f);
}

static void save_nets_file(void) {
  FILE *f = fopen(WIFI_CFG_PATH, "wb");
  int i;
  if (!f) { alog("WARN: save wifi.cfg FAILED (%d)", errno); return; }
  for (i = 0; i < g_saved_count; i++) fprintf(f, "%s\n%s\n", g_saved_nets[i].ssid, g_saved_nets[i].key);
  fclose(f);
}

/* Remembers ssid/key (updating the key if this ssid is already known), then persists the whole list.
 * Past MAX_SAVED_NETS, the oldest entry makes room -- the one just used is always kept. */
static void remember_net(const char *ssid, const char *key) {
  int i;
  for (i = 0; i < g_saved_count; i++) {
    if (strcmp(g_saved_nets[i].ssid, ssid) == 0) {
      snprintf(g_saved_nets[i].key, sizeof g_saved_nets[i].key, "%s", key);
      save_nets_file();
      return;
    }
  }
  if (g_saved_count >= MAX_SAVED_NETS) {
    memmove(&g_saved_nets[0], &g_saved_nets[1], (size_t)(MAX_SAVED_NETS - 1) * sizeof g_saved_nets[0]);
    g_saved_count = MAX_SAVED_NETS - 1;
  }
  snprintf(g_saved_nets[g_saved_count].ssid, sizeof g_saved_nets[g_saved_count].ssid, "%s", ssid);
  snprintf(g_saved_nets[g_saved_count].key, sizeof g_saved_nets[g_saved_count].key, "%s", key);
  g_saved_count++;
  save_nets_file();
}

static bool find_saved_key(const char *ssid, char *key_out, size_t cap) {
  int i;
  for (i = 0; i < g_saved_count; i++)
    if (strcmp(g_saved_nets[i].ssid, ssid) == 0) { snprintf(key_out, cap, "%s", g_saved_nets[i].key); return true; }
  return false;
}

static void read_line(const char *prompt, char *buf, size_t buf_size, bool mask) {
  printf("%s\n", prompt);
  size_t len = 0;
  buf[0] = '\0';
  keyboardDemoInit();
  keyboardShow();
  while (1) {
    cothread_yield_irq(IRQ_VBLANK);
    scanKeys();
    int key = keyboardUpdate();
    if (key == DVK_ENTER) break;
    if (key == DVK_BACKSPACE) {
      if (len > 0) { len--; printf("\b \b"); }
      continue;
    }
    if (key > 0 && key < 128 && len < buf_size - 1) {
      buf[len++] = (char)key;
      buf[len] = '\0';
      printf(mask ? "*" : "%c", key);
    }
  }
  printf("\n");
  keyboardHide();
}

/* Returns 0 with `*chosen` set, or -1 (no networks found, or B/START cancels -- only when `cancelable`,
 * since the very first boot has nothing sensible to cancel back to). */
static int scan_and_pick(Wifi_AccessPoint *chosen, bool cancelable) {
  printf("Scanning...\n");
  Wifi_ScanMode();
  for (int i = 0; i < 2 * 60; i++) cothread_yield_irq(IRQ_VBLANK);

  static Wifi_AccessPoint list[SCAN_SLOTS];
  int total = Wifi_GetNumAP(), n = 0;
  for (int i = 0; i < total && n < SCAN_SLOTS; i++) {
    Wifi_AccessPoint ap;
    if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK || ap.ssid_len == 0) continue;
    list[n++] = ap;
  }
  if (n == 0) { printf("No networks found.\n"); return -1; }

  int sel = 0;
  while (1) {
    consoleClear();
    printf(cancelable ? "NSD Bridge -- pick a network\n(UP/DOWN, A to choose, B to cancel)\n\n"
                       : "NSD Bridge -- pick a network\n(UP/DOWN, A to choose)\n\n");
    for (int i = 0; i < n; i++) {
      char ssid[33], key[64];
      int l = list[i].ssid_len < 32 ? list[i].ssid_len : 32;
      memcpy(ssid, list[i].ssid, l);
      ssid[l] = '\0';
      printf("%s%2d. %-24s %s%s\n", i == sel ? "> " : "  ", i, ssid,
             (list[i].flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA)) ? "[locked]" : "[open]",
             find_saved_key(ssid, key, sizeof key) ? " [saved]" : "");
    }
    cothread_yield_irq(IRQ_VBLANK);
    scanKeys();
    int down = keysDown();
    if (cancelable && (down & (KEY_B | KEY_START))) return -1;
    if (down & KEY_UP) sel = (sel - 1 + n) % n;
    if (down & KEY_DOWN) sel = (sel + 1) % n;
    if (down & KEY_A) { *chosen = list[sel]; consoleClear(); return 0; }
  }
}

static bool wait_for_association(int seconds) {
  int deadline = seconds * 60;
  for (int frame = 0; frame < deadline; frame++) {
    cothread_yield_irq(IRQ_VBLANK);
    entropy_tick();
    int status = Wifi_AssocStatus();
    if (frame % 60 == 0) printf(".");
    if (status == ASSOCSTATUS_ASSOCIATED) return true;
    if (status == ASSOCSTATUS_CANNOTCONNECT) return false;
  }
  return false;
}

/* Connects (with the interactive scan/password UI when no saved network is in range). Also records
 * g_last_ap/g_last_key on success, so a later silent drop can be recovered by net_check() without
 * ever showing this UI again mid-session. */
static bool connect_wifi(struct in_addr *out_ip) {
  if (!Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE)) { printf("Wifi_InitDefault FAILED\n"); return false; }
  load_saved_nets();

  bool have_ap = false;
  g_last_key[0] = '\0';

  if (g_saved_count > 0) {
    printf("Scanning for saved networks...\n");
    Wifi_ScanMode();
    for (int i = 0; i < 2 * 60; i++) cothread_yield_irq(IRQ_VBLANK);
    int total = Wifi_GetNumAP();
    for (int i = 0; i < total && !have_ap; i++) {
      Wifi_AccessPoint a;
      char ssid[33] = { 0 }, key[64];
      int l;
      if (Wifi_GetAPData(i, &a) != WIFI_RETURN_OK) continue;
      l = a.ssid_len < 32 ? a.ssid_len : 32;
      memcpy(ssid, a.ssid, l);
      if (find_saved_key(ssid, key, sizeof key)) {
        g_last_ap = a;
        snprintf(g_last_key, sizeof g_last_key, "%s", key);
        have_ap = true;
        printf("Found: %s\n", ssid);
      }
    }
    if (!have_ap) printf("None of the saved networks are in range.\n");
  }

  if (!have_ap) {
    if (scan_and_pick(&g_last_ap, false) != 0) return false;
    if (g_last_ap.flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA)) read_line("Password:", g_last_key, sizeof(g_last_key), true);
  }

  printf("Connecting");
  int rc = (g_last_ap.flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA))
             ? Wifi_ConnectSecureAP(&g_last_ap, g_last_key, strlen(g_last_key))
             : Wifi_ConnectOpenAP(&g_last_ap);
  if (rc != 0) { printf("\nWifi_Connect...AP() FAILED (%d)\n", rc); return false; }
  if (!wait_for_association(20)) { printf("\nAssociation FAILED/timed out\n"); return false; }

  struct in_addr gw = { 0 }, mask = { 0 }, dns1 = { 0 }, dns2 = { 0 };
  *out_ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
  printf("\nConnected. IP: %s\n", inet_ntoa(*out_ip));

  {
    char ssid[33] = { 0 };
    int l = g_last_ap.ssid_len < 32 ? g_last_ap.ssid_len : 32;
    memcpy(ssid, g_last_ap.ssid, l);
    remember_net(ssid, g_last_key);
  }
  g_wifi_up = true;
  return true;
}

/* ---- wifi health check + silent reconnect ----
 * Unlike the 3DS (system-managed Wi-Fi via ACU, which stays associated on its own), the DSi's own
 * wifi chip can drop association after a while with no warning -- connect_wifi() above only ever
 * ran once, at boot, so nothing noticed or recovered from that, and the console just went
 * unreachable until the app was relaunched. Ported the shape of the fix, not the code, from the
 * 3DS agent's service_network(): poll Wi-Fi status every ~500ms, and on a drop, tear down the NDP
 * listener and silently reconnect with the saved credentials (g_last_ap/g_last_key -- never the
 * interactive scan/password UI again mid-session), then re-listen once associated. */
static void sync_web(void); /* defined below, alongside R's other web-page handling */
#define NET_CHECK_MS 500u
#define RECONNECT_RETRY_MS 5000u

static uint64_t g_last_net_check = 0;

static void net_check(void) {
  int status;
  bool up;
  if (g_ms - g_last_net_check < NET_CHECK_MS) return;
  g_last_net_check = g_ms;

  status = Wifi_AssocStatus();
  up = status == ASSOCSTATUS_ASSOCIATED;
  if (up != g_wifi_up) {
    g_wifi_up = up;
    if (up) {
      struct in_addr ip, gw = { 0 }, mask = { 0 }, dns1 = { 0 }, dns2 = { 0 };
      int lr;
      ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
      g_ip = ip.s_addr;
      alog("Wi-Fi reconnected: %s", inet_ntoa(ip));
      lr = ndp_server_listen(&g_srv, ip.s_addr, AGENT_PORT);
      if (lr == 0) { g_listening = true; alog("Listening on %s:%u", inet_ntoa(ip), AGENT_PORT); sync_web(); }
      else alog("ERR: re-listen failed (%d): %s", lr, strerror(-lr));
      g_reconnecting = false;
    } else {
      alog("Wi-Fi lost");
      ndp_server_close(&g_srv);
      g_listening = false;
      g_web_listening = false;
      g_reconnecting = false; /* try again below, right away */
    }
  }
  if (!g_wifi_up) {
    if (g_reconnecting && status == ASSOCSTATUS_CANNOTCONNECT) g_reconnecting = false; /* that attempt failed: retry */
    if (!g_reconnecting && g_ms >= g_next_reconnect_try) {
      (void)((g_last_ap.flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA))
                 ? Wifi_ConnectSecureAP(&g_last_ap, g_last_key, strlen(g_last_key))
                 : Wifi_ConnectOpenAP(&g_last_ap));
      g_reconnecting = true;
      g_next_reconnect_try = g_ms + RECONNECT_RETRY_MS;
    }
  }
}

/* Manual network menu (L button). Two jobs in one: the escape hatch for a "soft" wifi failure that
 * net_check() alone would never notice (the AP silently forgot this console, dswifi's own state
 * never updating to reflect it, so Wifi_AssocStatus() keeps reporting ASSOCIATED forever with nothing
 * actually getting through -- this does not trust that status at all, it tears down and reconnects
 * unconditionally), and the way to switch to a different network on purpose. Never forgets a
 * previously saved network, even the one just left: remember_net() only ever adds or updates. */
static void open_wifi_menu(void) {
  Wifi_AccessPoint ap;
  char ssid[33] = { 0 }, key[64] = { 0 };
  int l, rc;

  alog("Wi-Fi menu opened (L)");
  ndp_server_close(&g_srv);
  g_listening = false;
  g_web_listening = false;
  Wifi_DisconnectAP();
  g_wifi_up = false;

  consoleSelect(&g_top);
  if (scan_and_pick(&ap, true) != 0) {
    alog("Wi-Fi menu: cancelled, retrying the previous network");
    g_reconnecting = false;
    g_next_reconnect_try = 0; /* net_check() picks this up on its very next tick */
    return;
  }

  l = ap.ssid_len < 32 ? ap.ssid_len : 32;
  memcpy(ssid, ap.ssid, l);
  if (!find_saved_key(ssid, key, sizeof key) && (ap.flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA)))
    read_line("Password:", key, sizeof key, true);

  printf("Connecting");
  rc = (ap.flags & (WFLAG_APDATA_WEP | WFLAG_APDATA_WPA)) ? Wifi_ConnectSecureAP(&ap, key, strlen(key)) : Wifi_ConnectOpenAP(&ap);
  if (rc != 0 || !wait_for_association(20)) {
    printf(rc != 0 ? "\nWifi_Connect...AP() FAILED (%d)\n" : "\nAssociation FAILED/timed out\n", rc);
    alog("Wi-Fi menu: could not connect to %s", ssid);
    printf("\nPress any button to continue\n(keeps retrying the previous network)");
    while (1) { cothread_yield_irq(IRQ_VBLANK); scanKeys(); if (keysDown()) break; }
    g_reconnecting = false;
    g_next_reconnect_try = 0;
    return;
  }

  g_last_ap = ap;
  snprintf(g_last_key, sizeof g_last_key, "%s", key);
  remember_net(ssid, key);
  g_wifi_up = true;

  {
    struct in_addr ip, gw = { 0 }, mask = { 0 }, dns1 = { 0 }, dns2 = { 0 };
    int lr;
    ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
    g_ip = ip.s_addr;
    alog("Connected to %s: %s", ssid, inet_ntoa(ip));
    lr = ndp_server_listen(&g_srv, ip.s_addr, AGENT_PORT);
    if (lr == 0) { g_listening = true; alog("Listening on %s:%u", inet_ntoa(ip), AGENT_PORT); sync_web(); }
    else alog("ERR: re-listen failed (%d): %s", lr, strerror(-lr));
  }
}

/* ---- pairing window (Y toggles it; both the code-based flow and number-comparison pairing use it) ---- */

static void open_pairing(void) {
  uint8_t code[NDP_CODE_BYTES];
  if (!AGENT_AUTH_REQUIRED) return;
  if (ndp_server_pairing_remaining_ms(&g_srv) > 0) {
    ndp_server_close_pairing(&g_srv);
    return;
  }
  if (entropy_random_bytes(NULL, code, sizeof code) != 0) return;
  ndp_server_open_pairing(&g_srv, code, PAIRING_WINDOW_MS);
  memset(code, 0, sizeof code);
}

/* ---- web page (R toggles it; on by default, exactly like the 3DS agent) ---- */

static void sync_web(void) {
  int rc;
  g_web_listening = false;
  if (!g_web_on) { ndp_server_web_close(&g_srv); return; }
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

static void hms(char *out, size_t cap, uint64_t ms) {
  unsigned long s = (unsigned long)(ms / 1000);
  snprintf(out, cap, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

/* Rows a log line occupies on the bottom console: BOT_COLS, then 2-space-indented continuations.
 * (Ported verbatim from agent/3ds/source/main.c.) */
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

/* Top screen: status panel (mirrors the 3DS agent's draw()). Bottom screen: the activity log
 * (alog), exactly like the 3DS. Reads g_ip (not a parameter): net_check() updates it on a silent
 * reconnect, and a stale IP passed in from main()'s boot-time local would otherwise linger on
 * screen after the address actually changed. */
static void draw(void) {
  struct in_addr ip;
  bool dev_mode = g_srv.agent_cfg.mode != NDP_MODE_READ_ONLY;
  char up[16];
  int n, i, rows;

  ip.s_addr = g_ip;

  /* The DSi's console is 32 columns (a hardware tile-grid limit of the 256px screen in text mode --
   * no smaller font can change that, unlike the 3DS's much wider screen this layout was ported
   * from). Every line here is kept at or under ~28 columns, with %.Ns truncation on anything of
   * unbounded length (a pairing label, a peer address), so nothing here ever wraps or pushes the
   * title off the top of a 24-row screen. */
  consoleSelect(&g_top);
  consoleClear();
  printf(C_CYAN "NSD Bridge" C_RESET " (DSi) v%s\n", NDP_AGENT_VERSION);
  if (g_listening) printf("Status : " C_GREEN "ONLINE" C_RESET "\n");
  else printf("Status : " C_YELLOW "RECONNECTING" C_RESET "\n");
  printf("IP     : %s:%u\n", inet_ntoa(ip), (unsigned)g_srv.port);
  if (g_web_listening) {
    printf("Web    : :%u %s%s\n", (unsigned)g_srv.web_port,
           g_srv.web_client_fd >= 0 ? C_GREEN "ON" C_RESET : "idle", g_srv.web_open ? " (open)" : "");
  } else {
    printf("Web    : %s\n", g_web_on ? "starting" : "off (R)");
  }
  if (g_srv.client_fd >= 0) printf("Bridge : " C_GREEN "ON" C_RESET " %.18s\n", g_srv.peer);
  else printf("Bridge : off\n");
  if (dev_mode) printf("Mode   : " C_YELLOW "%.20s" C_RESET "\n", ndp_mode_name(g_srv.agent_cfg.mode));
  else printf("Mode   : " C_GREEN "READ_ONLY" C_RESET "\n");
  printf("Write  : %s\n", PROTOCOL_WORKSPACE);
  {
    int reads = g_access.count, writes = 0, i2;
    bool whole = false;
    for (i2 = 0; i2 < g_access.count; i2++) {
      if (g_access.e[i2].level == NDP_LVL_WRITE) writes++;
      if (strcmp(g_access.e[i2].path, "/") == 0) whole = true;
    }
    if (whole) printf("Open   : " C_RED "WHOLE SD" C_RESET "%s\n", writes ? "+w" : "");
    else if (reads == 0) printf("Open   : agent only\n");
    else printf("Open   : +%d (%dw)\n", reads, writes);
  }
  if (!AGENT_AUTH_REQUIRED) printf("Auth   : " C_RED "NONE" C_RESET "\n");
  else printf("Auth   : %d paired\n", g_srv.keys.count);
  hms(up, sizeof up, g_ms - g_start_ms);
  printf("Up: %s  Req: %lu\n", up, (unsigned long)g_srv.requests);

  if (AGENT_AUTH_REQUIRED) {
    uint32_t left = ndp_server_pairing_remaining_ms(&g_srv);
    if (ndp_server_pair2_pending(&g_srv)) {
      /* a page or computer asked to pair: the number is shown here and there; the person compares and answers */
      unsigned sas = (unsigned)g_srv.pairing.p2_sas;
      printf("\n" C_YELLOW "PAIRING REQUEST" C_RESET "\n%.14s \"%.10s\"\n" C_CYAN "%03u %03u" C_RESET " same #?\n"
             C_GREEN "A" C_RESET "=yes " C_RED "B" C_RESET "=no\n",
             g_srv.pairing.p2_peer, g_srv.pairing.p2_label, sas / 1000u, sas % 1000u);
    } else if (left > 0) {
      char text[NDP_CODE_TEXT];
      ndp_code_encode(g_srv.pairing.code, text);
      printf("\n" C_YELLOW "PAIRING OPEN" C_RESET " %lus\n" C_CYAN "%s" C_RESET "\n",
             (unsigned long)((left + 999) / 1000), text);
    } else if (g_forget_armed_until > g_ms) {
      printf("\n" C_RED "SELECT again:" C_RESET "\nforget ALL pairings\n");
    } else {
      printf("\nY: open pairing\n");
    }
  }
  printf("\nA=folders Y=pair X=mode\nR=web  L=wifi menu\nSELECT x2=forget START=exit\n");

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

  consoleSelect(&g_top);
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
  videoSetMode(MODE_0_2D);
  videoSetModeSub(MODE_0_2D);
  vramSetBankA(VRAM_A_MAIN_BG);
  vramSetBankC(VRAM_C_SUB_BG);
  consoleInit(&g_top, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
  consoleInit(&g_bot, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
  consoleSelect(&g_top);

  printf("NSD Bridge (DSi) v" NDP_AGENT_VERSION "\n\n");

  /* Before anything that could build a policy or check a path (loading access.bin, pairing, listening). */
  ndp_workspace_set(PROTOCOL_WORKSPACE);
  ndp_policy_set_default_zones(DSI_NEVER_READ, 1, DSI_NEVER_WRITE, 2, NULL, 0);

  entropy_init();
  if (!sd_init()) { printf("\nPress START to exit"); while (1) { cothread_yield_irq(IRQ_VBLANK); scanKeys(); if (keysHeld() & KEY_START) return 0; } }

  alog_init(); /* the SD card is up: the activity log's file can now be opened */
  g_start_ms = g_ms;
  alog("NSD Bridge v" NDP_AGENT_VERSION ", protocol %d", NDP_PROTOCOL_VERSION);

  struct in_addr ip;
  if (!connect_wifi(&ip)) { printf("\nPress START to exit"); while (1) { cothread_yield_irq(IRQ_VBLANK); scanKeys(); if (keysHeld() & KEY_START) return 0; } }
  g_ip = ip.s_addr;

  int ar = ndp_access_load_file(&g_access, ACCESS_FILE);
  if (ar < 0) alog("WARN: folder list is corrupt: ignored");
  else if (ar == 0) alog("Loaded %d opened folder(s)", g_access.count);

  if (AGENT_AUTH_REQUIRED) {
    int lr = ndp_keystore_load_file(&g_keys, KEYS_FILE);
    if (lr < 0) alog("WARN: pairing file is corrupt: ignored");
    else if (lr == 0) alog("Loaded %d paired computer(s)", g_keys.count);
    if (!g_keys.has_device_id && entropy_random_bytes(NULL, g_keys.device_id, sizeof g_keys.device_id) == 0)
      g_keys.has_device_id = 1;
  }

  ndp_agent_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "dsi";
  cfg.agent_version = NDP_AGENT_VERSION;
  cfg.mode = AGENT_START_MODE;
  cfg.auth = AGENT_AUTH_REQUIRED ? "required" : "none";
  cfg.max_frame = NDP_DEFAULT_MAX_FRAME;
  cfg.random_bytes = entropy_random_bytes;
  cfg.device_info = device_info;
  if (ndp_posix_fs_init(&g_fs, &g_fs_ctx, "sd:") == 0) cfg.fs = &g_fs;

  ndp_server_platform plat;
  memset(&plat, 0, sizeof plat);
  plat.now_ms = now_ms;
  plat.log = srv_log;
  plat.keys_changed = keys_changed;

  ndp_server_init(&g_srv, &plat, &cfg);
  { static ndp_policy pol; ndp_access_to_policy(&g_access, &pol); ndp_server_set_policy(&g_srv, &pol); }
  if (AGENT_AUTH_REQUIRED) ndp_server_set_keys(&g_srv, &g_keys);
  g_web_assets.index_gz = ndp_web_index_gz;
  g_web_assets.index_gz_len = ndp_web_index_gz_len;
  g_web_assets.version = ndp_web_version;
  ndp_server_set_web(&g_srv, &g_web_assets);
  {
    /* the page needs no pairing unless this file exists (the CLI and the MCP always pair) */
    struct stat wb;
    ndp_server_set_web_open(&g_srv, stat(CONFIG_DIR "/require-web-pairing", &wb) != 0);
  }

  int lr = ndp_server_listen(&g_srv, ip.s_addr, AGENT_PORT);
  if (lr != 0) {
    printf("listen failed (%d): %s\n", lr, strerror(-lr));
    printf("\nPress START to exit");
    while (1) { cothread_yield_irq(IRQ_VBLANK); scanKeys(); if (keysHeld() & KEY_START) return 0; }
  }
  g_listening = true;
  alog("Listening on %s:%u", inet_ntoa(ip), AGENT_PORT);
  sync_web();

  draw();

  int redraw_countdown = 0;
  while (1) {
    cothread_yield_irq(IRQ_VBLANK);
    entropy_tick();
    g_ms += 16; /* ~59.8 Hz; +-0.3% is fine for idle timeouts and the pairing window, not for crypto */
    scanKeys();
    if (!access_ui_active() && (keysHeld() & KEY_START)) break; /* while the folder editor is open, START closes it instead (below) */

    int down = keysDown();
    bool force_redraw = false;
    if (AGENT_AUTH_REQUIRED && !access_ui_active() && ndp_server_pair2_pending(&g_srv) && (down & (KEY_A | KEY_B))) {
      /* a page or computer asked to pair by number comparison: the person compares and answers */
      int yes = (down & KEY_A) != 0;
      alog(yes ? "Pairing approved" : "Pairing refused");
      (void)ndp_server_pair2_decide(&g_srv, yes);
      force_redraw = true;
    } else if (access_ui_active()) {
      /* the folder editor owns the buttons while it is open (START closes it, it does not quit the agent) */
      if (access_ui_handle((u32)down)) force_redraw = true;
    } else if (down & KEY_A) {
      access_ui_open(&g_access, access_changed);
      force_redraw = true;
    } else if (AGENT_AUTH_REQUIRED && (down & KEY_Y)) {
      open_pairing();
      force_redraw = true;
    }
    if (!access_ui_active() && (down & KEY_X)) {
      ndp_mode next = g_srv.agent_cfg.mode == NDP_MODE_READ_ONLY ? NDP_MODE_DEVELOPMENT : NDP_MODE_READ_ONLY;
      ndp_server_set_mode(&g_srv, next);
      alog("Mode -> %s", ndp_mode_name(next));
      force_redraw = true;
    }
    if (!access_ui_active() && AGENT_AUTH_REQUIRED && (down & KEY_SELECT)) {
      if (g_forget_armed_until > g_ms) {
        ndp_server_clear_keys(&g_srv);
        g_forget_armed_until = 0;
      } else {
        g_forget_armed_until = g_ms + FORGET_CONFIRM_MS;
      }
      force_redraw = true;
    }
    if (!access_ui_active() && (down & KEY_R)) {
      g_web_on = !g_web_on;
      alog("Web page -> %s", g_web_on ? "on" : "off");
      sync_web();
      force_redraw = true;
    }
    if (!access_ui_active() && (down & KEY_L)) {
      open_wifi_menu();
      force_redraw = true;
    }

    net_check();
    if (g_listening) {
      int s = ndp_server_step(&g_srv, 0);
      if (s > 0) force_redraw = true; /* a pairing/auth/request happened: e.g. the paired count changed */
    }

    /* The pairing countdown needs to visibly tick even with nothing else happening. */
    if (force_redraw || --redraw_countdown <= 0) {
      if (access_ui_active()) access_ui_draw(&g_top, &g_bot);
      else draw();
      redraw_countdown = 30; /* ~0.5 s at ~60 Hz */
    }
  }

  alog_exit();
  ndp_server_close(&g_srv);
  Wifi_DisconnectAP();
  Wifi_DisableWifi();
  return 0;
}
