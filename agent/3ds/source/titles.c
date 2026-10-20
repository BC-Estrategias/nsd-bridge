#ifndef AGENT_TEXT_UI
#include "titles.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alog.h"
#include "utf16.h"

#define OWN_TITLE 0x000400000BD00100ull
#define MAX_TITLES 640
#define MAX_PENDING 128
#define ICON_UNITS (48 * 48)

typedef struct { char magic[4]; uint16_t version, reserved; struct { uint16_t shortd[0x40], longd[0x80], publisher[0x40]; } titles[16]; uint8_t settings[0x30]; uint8_t reserved2[8]; uint8_t small_icon[0x480]; uint8_t large_icon[0x1200]; } smdh_t;

static LightLock g_lock;
static Thread g_worker;
static bool g_init;
static volatile int g_state = TS_IDLE;
static unsigned g_rev;
static title_entry *g_t;
static int g_nt;
static pending_entry *g_p;
static int g_np;
static char g_msg[96];
static bool g_msg_ok;
static enum { J_NONE, J_LOAD, J_PENDING, J_DELETE, J_PDELETE } g_job;
static int g_job_idx;
static bool g_job_ticket;
static int g_lang = CFG_LANGUAGE_EN;

static void bump(void) { LightLock_Lock(&g_lock); g_rev++; LightLock_Unlock(&g_lock); }
static void set_state(int s) { __atomic_store_n(&g_state, s, __ATOMIC_RELEASE); bump(); }

void titles_init(void) {
  u8 lang = 0;
  if (g_init) return;
  LightLock_Init(&g_lock);
  g_t = calloc(MAX_TITLES, sizeof *g_t);
  g_p = calloc(MAX_PENDING, sizeof *g_p);
  if (R_SUCCEEDED(cfguInit())) {
    if (R_SUCCEEDED(CFGU_GetSystemLanguage(&lang)) && lang < 12) g_lang = lang;
    cfguExit();
  }
  g_init = g_t && g_p;
}

static void free_icons(void) {
  int i;
  for (i = 0; i < g_nt; i++) { free(g_t[i].icon); g_t[i].icon = NULL; }
}

void titles_shutdown(void) {
  if (!g_init) return;
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  free_icons();
  free(g_t);
  free(g_p);
  g_t = NULL;
  g_p = NULL;
  g_nt = g_np = 0;
  g_init = false;
}

int titles_state(void) { return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE); }
unsigned titles_revision(void) { unsigned r; LightLock_Lock(&g_lock); r = g_rev; LightLock_Unlock(&g_lock); return r; }
int titles_count(void) { return g_nt; }
const title_entry *titles_at(int i) { return i >= 0 && i < g_nt ? &g_t[i] : NULL; }
int pending_count(void) { return g_np; }
const pending_entry *pending_at(int i) { return i >= 0 && i < g_np ? &g_p[i] : NULL; }
const char *titles_message(void) { return g_msg; }
bool titles_message_ok(void) { return g_msg_ok; }
bool titles_is_self(const title_entry *t) { return t && t->id == OWN_TITLE; }
/* What can be deleted from here: titles on the SD card, and DSiWare (system memory). The rest of system memory is read-only. */
bool titles_can_delete(const title_entry *t) { return t && !titles_is_self(t) && (t->media == MEDIATYPE_SD || (t->media == MEDIATYPE_NAND && t->twl)); }

static title_kind classify(uint64_t id, bool *twl) {
  uint32_t hi = (uint32_t)(id >> 32);
  *twl = (hi & 0x8000) != 0 && (hi >> 16) == 0x0004;
  if (*twl) return KIND_DSIWARE;
  switch (hi) {
    case 0x00040000: return KIND_APP;
    case 0x0004000E: return KIND_UPDATE;
    case 0x0004008C: return KIND_DLC;
    default: return KIND_SYSTEM;
  }
}

/* Name, publisher and icon from the title's SMDH, read from its own archive (the way FBI does). */
static void read_smdh(title_entry *t) {
  static const u32 file_path[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000};
  u32 archive_path[4] = {(u32)(t->id & 0xFFFFFFFF), (u32)(t->id >> 32), t->media, 0};
  FS_Path ap = {PATH_BINARY, sizeof archive_path, archive_path}, fp = {PATH_BINARY, sizeof file_path, file_path};
  Handle h;
  smdh_t *s;
  u32 got = 0;
  if (R_FAILED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, ap, fp, FS_OPEN_READ, 0))) return;
  s = malloc(sizeof *s);
  if (s && R_SUCCEEDED(FSFILE_Read(h, &got, 0, s, sizeof *s)) && got == sizeof *s && !memcmp(s->magic, "SMDH", 4)) {
    int l = g_lang;
    if (!s->titles[l].shortd[0]) l = CFG_LANGUAGE_EN;
    nsd_utf16_to_utf8(s->titles[l].shortd, 0x40, t->name, sizeof t->name);
    nsd_utf16_to_utf8(s->titles[l].publisher, 0x40, t->publisher, sizeof t->publisher);
    t->icon = malloc(ICON_UNITS * 2);
    if (t->icon) memcpy(t->icon, s->large_icon, ICON_UNITS * 2);
  }
  free(s);
  FSFILE_Close(h);
}

/* DSiWare has no SMDH: the name is in the DS ROM header. */
static void read_twl(title_entry *t) {
  u8 header[0x3B4];
  memset(header, 0, sizeof header);
  if (R_FAILED(FSUSER_GetLegacyRomHeader(t->media, t->id, header))) return;
  {
    char nm[13];
    memcpy(nm, header, 12);
    nm[12] = '\0';
    for (int i = 11; i >= 0 && (nm[i] == ' ' || nm[i] == '\0'); i--) nm[i] = '\0';
    snprintf(t->name, sizeof t->name, "%s", nm);
    snprintf(t->code, sizeof t->code, "TWL-%.4s", (const char *)header + 0x0C);
  }
}

static void load_media(FS_MediaType mt) {
  u32 count = 0, got = 0, i;
  u64 *ids;
  AM_TitleInfo infos[32];
  if (R_FAILED(AM_GetTitleCount(mt, &count)) || count == 0) return;
  if (count > MAX_TITLES) count = MAX_TITLES;
  ids = malloc((size_t)count * sizeof *ids);
  if (!ids) return;
  if (R_SUCCEEDED(AM_GetTitleList(&got, mt, count, ids))) {
    for (i = 0; i < got && g_nt < MAX_TITLES; i += 32) {
      u32 n = got - i < 32 ? got - i : 32, k;
      if (R_FAILED(AM_GetTitleInfo(mt, n, ids + i, infos))) continue;
      for (k = 0; k < n && g_nt < MAX_TITLES; k++) {
        title_entry *t = &g_t[g_nt];
        memset(t, 0, sizeof *t);
        t->id = infos[k].titleID;
        t->size = infos[k].size;
        t->version = infos[k].version;
        t->media = (uint8_t)mt;
        t->kind = (uint8_t)classify(t->id, &t->twl);
        if (mt != MEDIATYPE_GAME_CARD || !t->twl) AM_GetTitleProductCode(mt, t->id, t->code);
        if (t->twl) read_twl(t);
        else if (t->kind == KIND_APP || t->kind == KIND_SYSTEM) read_smdh(t);
        g_nt++;
      }
    }
  }
  free(ids);
}

/* Updates and DLC have no icon of their own: they borrow the name of the base game (same low half of the ID). */
static void name_the_rest(void) {
  int i, j;
  for (i = 0; i < g_nt; i++) {
    title_entry *t = &g_t[i];
    if (t->name[0]) continue;
    if (t->kind == KIND_UPDATE || t->kind == KIND_DLC) {
      for (j = 0; j < g_nt; j++) {
        const title_entry *b = &g_t[j];
        if (b->kind == KIND_APP && b->name[0] && (uint32_t)b->id == (uint32_t)t->id) {
          snprintf(t->name, sizeof t->name, "%.50s (%s)", b->name, t->kind == KIND_UPDATE ? "update" : "DLC");
          snprintf(t->publisher, sizeof t->publisher, "%s", b->publisher);
          break;
        }
      }
    }
    if (!t->name[0]) snprintf(t->name, sizeof t->name, "%016llX", (unsigned long long)t->id);
  }
}

static void job_load(void) {
  free_icons();
  g_nt = 0;
  load_media(MEDIATYPE_SD);
  load_media(MEDIATYPE_NAND);
  load_media(MEDIATYPE_GAME_CARD);
  name_the_rest();
  alog("Titles: %d listed", g_nt);
}

static void pending_media(FS_MediaType mt) {
  u32 n = 0, got = 0, i;
  u64 *ids = malloc(MAX_PENDING * sizeof *ids);
  AM_PendingTitleInfo *info = malloc(MAX_PENDING * sizeof *info);
  if (!ids || !info) goto out;
  if (R_FAILED(AM_GetNumPendingTitles(&n, mt)) || n == 0) goto out;
  if (n > MAX_PENDING) n = MAX_PENDING;
  if (R_FAILED(AM_GetPendingTitleList(&got, ids, n, mt)) || got == 0) goto out;
  if (R_FAILED(AM_GetPendingTitleInfo(got, mt, ids, info))) goto out;
  for (i = 0; i < got && g_np < MAX_PENDING; i++) {
    pending_entry *p = &g_p[g_np++];
    p->id = info[i].titleId;
    p->size = info[i].titleSize;
    p->version = info[i].version;
    p->media = (uint8_t)mt;
    p->status = info[i].status;
  }
out:
  free(ids);
  free(info);
}

static void job_pending(void) {
  g_np = 0;
  pending_media(MEDIATYPE_SD);
  pending_media(MEDIATYPE_NAND);
  alog("Titles: %d pending", g_np);
}

static void job_delete(void) {
  title_entry t;
  Result r;
  if (g_job_idx < 0 || g_job_idx >= g_nt) return;
  t = g_t[g_job_idx];
  if (!titles_can_delete(&t)) { snprintf(g_msg, sizeof g_msg, "%s", "not allowed"); g_msg_ok = false; return; }
  r = AM_DeleteTitle((FS_MediaType)t.media, t.id);
  if (R_SUCCEEDED(r) && g_job_ticket) r = AM_DeleteTicket(t.id);
  if (t.media == MEDIATYPE_SD) AM_QueryAvailableExternalTitleDatabase(NULL);
  if (R_FAILED(r)) {
    snprintf(g_msg, sizeof g_msg, "0x%08lX", (unsigned long)r);
    g_msg_ok = false;
    alog("Titles: delete %016llX failed 0x%08lX", (unsigned long long)t.id, (unsigned long)r);
    return;
  }
  alog("Titles: deleted %016llX (%s)%s", (unsigned long long)t.id, t.media == MEDIATYPE_SD ? "SD" : "system memory", g_job_ticket ? " and its ticket" : "");
  free(g_t[g_job_idx].icon);
  memmove(&g_t[g_job_idx], &g_t[g_job_idx + 1], (size_t)(g_nt - g_job_idx - 1) * sizeof *g_t);
  g_nt--;
  g_msg[0] = '\0';
  g_msg_ok = true;
}

static void job_pdelete(void) {
  Result r = 0;
  if (g_job_idx < 0) {
    r = AM_DeleteAllPendingTitles(MEDIATYPE_SD);
    if (R_SUCCEEDED(r)) r = AM_DeleteAllPendingTitles(MEDIATYPE_NAND);
    alog("Titles: all pending titles deleted: 0x%08lX", (unsigned long)r);
  } else if (g_job_idx < g_np) {
    pending_entry p = g_p[g_job_idx];
    r = AM_DeletePendingTitle((FS_MediaType)p.media, p.id);
    alog("Titles: pending %016llX deleted: 0x%08lX", (unsigned long long)p.id, (unsigned long)r);
  }
  if (R_FAILED(r)) { snprintf(g_msg, sizeof g_msg, "0x%08lX", (unsigned long)r); g_msg_ok = false; }
  else { g_msg[0] = '\0'; g_msg_ok = true; }
  job_pending();
}

static void run(void *arg) {
  bool home, sleep_ok;
  (void)arg;
  if (R_FAILED(amInit())) { snprintf(g_msg, sizeof g_msg, "%s", "AM unavailable"); g_msg_ok = false; set_state(TS_READY); return; }
  /* like the installer: the HOME menu waits for the title manager, so it stays off while AM is working */
  home = aptIsHomeAllowed();
  sleep_ok = aptIsSleepAllowed();
  aptSetHomeAllowed(false);
  aptSetSleepAllowed(false);
  switch (g_job) {
    case J_LOAD: job_load(); break;
    case J_PENDING: job_pending(); break;
    case J_DELETE: job_delete(); break;
    case J_PDELETE: job_pdelete(); break;
    default: break;
  }
  aptSetSleepAllowed(sleep_ok);
  aptSetHomeAllowed(home);
  amExit();
  set_state(TS_READY);
}

static void start(int job, int idx, bool ticket, int state) {
  s32 prio = 0x30;
  if (!g_init || titles_state() == TS_LOADING || titles_state() == TS_BUSY) return;
  if (g_worker) { threadJoin(g_worker, UINT64_MAX); threadFree(g_worker); g_worker = NULL; }
  g_job = job;
  g_job_idx = idx;
  g_job_ticket = ticket;
  set_state(state);
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  g_worker = threadCreate(run, NULL, 64 * 1024, prio > 0x18 ? prio - 1 : prio, 0, false);
  if (!g_worker) { snprintf(g_msg, sizeof g_msg, "%s", "no thread"); g_msg_ok = false; set_state(TS_READY); }
}

void titles_load(void) { start(J_LOAD, 0, false, TS_LOADING); }
void titles_pending_load(void) { start(J_PENDING, 0, false, TS_LOADING); }
void titles_delete(int idx, bool with_ticket) { start(J_DELETE, idx, with_ticket, TS_BUSY); }
void titles_pending_delete(int idx) { start(J_PDELETE, idx, false, TS_BUSY); }

Result titles_launch(int idx) {
  const title_entry *t = titles_at(idx);
  Result r;
  u8 param[0x300], hmac[0x20];
  if (!t) return -1;
  alog("Titles: launching %016llX", (unsigned long long)t->id);
  alog_flush();
  r = APT_PrepareToDoApplicationJump(0, t->id, (FS_MediaType)t->media);
  if (R_FAILED(r)) return r;
  memset(param, 0, sizeof param);
  memset(hmac, 0, sizeof hmac);
  return APT_DoApplicationJump(param, sizeof param, hmac);
}
#endif
