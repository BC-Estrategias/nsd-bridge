#include "access_ui.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "alog.h"
#ifndef AGENT_TEXT_UI
#include "ui.h"
#endif

#define C_RESET "\x1b[0m"
#define C_GREEN "\x1b[32;1m"
#define C_YELLOW "\x1b[33;1m"
#define C_RED "\x1b[31;1m"
#define C_CYAN "\x1b[36;1m"

#define MAX_ROWS 256      /* folders shown per directory (row 0 is the folder itself) */
#define NAME_MAX_UI 128   /* longer names are skipped and counted */
#ifdef AGENT_TEXT_UI
#define VISIBLE_ROWS 24
#else
#define VISIBLE_ROWS UI_LIST_ROWS
#endif

static struct {
  bool active;
  ndp_access *list;
  access_changed_fn changed;
  char path[NDP_PATH_MAX + 1];
  char names[MAX_ROWS][NAME_MAX_UI];
  int n;          /* rows, including row 0 */
  int sel, top;
  int skipped;    /* folders not shown (too many / name too long) */
  char msg[80];
  bool msg_bad;
} g;

static int cmp_names(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void join(char *out, size_t cap, const char *dir, const char *name) {
  size_t dl = strlen(dir), nl = strlen(name), o = 0;
  if (dl + nl + 2 > cap) { out[0] = '\0'; return; } /* absurdly deep: unusable (rejected later) */
  memcpy(out, dir, dl);
  o = dl;
  if (dl > 1) out[o++] = '/';
  memcpy(out + o, name, nl);
  out[o + nl] = '\0';
}

/* Reads the sub-folders of g.path. Row 0 stands for the folder itself. */
static void load_dir(const char *select_name) {
  char fs_path[NDP_PATH_MAX + 8];
  DIR *d;
  struct dirent *e;
  static char tmp[MAX_ROWS][NAME_MAX_UI];
  int count = 0, i;
  g.skipped = 0;
  snprintf(fs_path, sizeof fs_path, "sdmc:%s", g.path);
  d = opendir(fs_path);
  if (d) {
    while ((e = readdir(d)) != NULL) {
      if (e->d_type != DT_DIR || e->d_name[0] == '.') continue;
      if (strlen(e->d_name) >= NAME_MAX_UI || count >= MAX_ROWS - 1) { g.skipped++; continue; }
      strcpy(tmp[count++], e->d_name);
    }
    closedir(d);
  }
  qsort(tmp, (size_t)count, NAME_MAX_UI, cmp_names);
  strcpy(g.names[0], ".");
  for (i = 0; i < count; i++) strcpy(g.names[i + 1], tmp[i]);
  g.n = count + 1;
  g.sel = 0;
  if (select_name)
    for (i = 1; i < g.n; i++)
      if (strcasecmp(g.names[i], select_name) == 0) { g.sel = i; break; }
  g.top = g.sel >= VISIBLE_ROWS ? g.sel - VISIBLE_ROWS + 1 : 0;
}

static void say(bool bad, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(bool bad, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g.msg, sizeof g.msg, fmt, ap);
  va_end(ap);
  g.msg_bad = bad;
}

void access_ui_open(ndp_access *list, access_changed_fn changed) {
  g.active = true;
  g.list = list;
  g.changed = changed;
  strcpy(g.path, "/");
  g.msg[0] = '\0';
  load_dir(NULL);
  alog("Access menu opened");
}

bool access_ui_active(void) { return g.active; }

static void row_path(int row, char *out, size_t cap) {
  if (row == 0) snprintf(out, cap, "%s", g.path);
  else join(out, cap, g.path, g.names[row]);
}

static const char *level_name(ndp_level l) { return l == NDP_LVL_WRITE ? "read+write" : l == NDP_LVL_READ ? "read" : "closed"; }

static void cycle_selected(void) {
  char path[NDP_PATH_MAX + 1];
  ndp_level exp, next, eff;
  int rc;
  row_path(g.sel, path, sizeof path);
  eff = ndp_access_level(g.list, path, &exp);
  next = ndp_access_next(g.list, path);
  if (next == exp) {
    if (!ndp_access_allowed(path, NDP_LVL_READ)) say(true, "Protected: this folder can never be opened.");
    else if (eff >= NDP_LVL_READ && !ndp_access_allowed(path, NDP_LVL_WRITE)) say(true, "System folder: read only, never writable.");
    else if (eff > exp) say(true, "Already open through its parent (%s).", level_name(eff));
    else say(true, "Nothing to change here.");
    return;
  }
  rc = ndp_access_set(g.list, path, next);
  if (rc == NDP_ST_NO_SPACE) { say(true, "List full (%d folders): close one first.", NDP_ACCESS_MAX); return; }
  if (rc != NDP_OK) { say(true, "Cannot use this folder name (error %d).", rc); return; }
  if (g.changed && g.changed(g.list) != 0) say(true, "Changed, but NOT saved to the SD card!");
  else if (strcmp(path, "/") == 0 && next != NDP_LVL_NONE) say(true, "WHOLE SD CARD is now open for %s.", level_name(next));
  else say(false, "%s -> %s", path, level_name(next));
  alog("[ACCESS] %s -> %s", path, level_name(next));
}

bool access_ui_handle(u32 k) {
  if (!g.active) return false;
  if (k & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (g.sel + 1 < g.n) g.sel++; }
  else if (k & (KEY_DUP | KEY_CPAD_UP)) { if (g.sel > 0) g.sel--; }
  else if (k & KEY_R) g.sel = g.sel + 10 < g.n ? g.sel + 10 : g.n - 1;
  else if (k & KEY_L) g.sel = g.sel > 10 ? g.sel - 10 : 0;
  else if (k & (KEY_A | KEY_DRIGHT)) {
    if (g.sel > 0) {
      char next[NDP_PATH_MAX + 1];
      join(next, sizeof next, g.path, g.names[g.sel]);
      strcpy(g.path, next);
      g.msg[0] = '\0';
      load_dir(NULL);
    }
  } else if (k & (KEY_B | KEY_DLEFT)) {
    if (strcmp(g.path, "/") == 0) { g.active = false; alog("Access menu closed"); return true; }
    { /* up one level and put the cursor back on the folder we came from */
      char came_from[NAME_MAX_UI];
      char *slash = strrchr(g.path, '/');
      snprintf(came_from, sizeof came_from, "%s", slash + 1);
      if (slash == g.path) strcpy(g.path, "/");
      else *slash = '\0';
      g.msg[0] = '\0';
      load_dir(came_from);
    }
  } else if (k & KEY_Y) cycle_selected();
  else if (k & KEY_START) { g.active = false; alog("Access menu closed"); return true; }
  else return false;
  if (g.sel < g.top) g.top = g.sel;
  if (g.sel >= g.top + VISIBLE_ROWS) g.top = g.sel - VISIBLE_ROWS + 1;
  return true;
}

#ifdef AGENT_TEXT_UI
void access_ui_draw(PrintConsole *top, PrintConsole *bottom) {
  int i, end;
  consoleSelect(top);
  consoleClear();
  printf(C_CYAN "Access folders" C_RESET "  what the Bridge may use\n");
  printf("%.48s\n", g.path);
  end = g.top + VISIBLE_ROWS < g.n ? g.top + VISIBLE_ROWS : g.n;
  for (i = g.top; i < end; i++) {
    char path[NDP_PATH_MAX + 1];
    ndp_level exp, eff;
    const char *tag;
    row_path(i, path, sizeof path);
    eff = ndp_access_level(g.list, path, &exp);
    if (exp != NDP_LVL_NONE) tag = exp == NDP_LVL_WRITE ? "[RW]" : "[R ]";
    else if (eff != NDP_LVL_NONE) tag = eff == NDP_LVL_WRITE ? "(RW)" : "(R )";
    else tag = "[  ]";
    printf("%s%s%s" C_RESET " %.40s%s\n", i == g.sel ? C_YELLOW ">" C_RESET : " ", exp != NDP_LVL_NONE ? C_GREEN : "", tag,
           i == 0 ? ". (this folder)" : g.names[i], !ndp_access_allowed(path, NDP_LVL_WRITE) ? "  !" : "");
  }
  if (g.skipped) printf(C_RED "%d folder(s) not shown (too many / long name)" C_RESET "\n", g.skipped);

  consoleSelect(bottom);
  consoleClear();
  printf(C_CYAN "Access folders" C_RESET "\n\n");
  printf("D-pad  move      A  open folder\n");
  printf("B  back / close  START  close\n");
  printf(C_YELLOW "Y" C_RESET "  change access of the highlighted\n   folder: closed > read > read+write\n\n");
  printf("[R ] read    [RW] read+write\n(R ) open through a parent folder\n! = system folder: never writable\n\n");
  printf("Row \".\" is the folder you are inside.\nThe agent's own folder is always open.\nWrites also need the mode switch (X).\n\n");
  if (g.msg[0]) printf("%s%s" C_RESET "\n", g.msg_bad ? C_RED : C_GREEN, g.msg);
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();
}
#else
/* Touch: tap a row to select it, tap the selected row's tag to change its access, tap its name to open it, tap
 * the title bar to go up, drag to scroll. */
bool access_ui_touch(u32 down, u32 held, const touchPosition *tp) {
  int step, r, max;
  if (!g.active) return false;
  step = ui_drag_rows(down, held, tp);
  if (step) {
    max = g.n > VISIBLE_ROWS ? g.n - VISIBLE_ROWS : 0;
    g.top += step;
    if (g.top < 0) g.top = 0;
    if (g.top > max) g.top = max;
    if (g.sel < g.top) g.sel = g.top;
    if (g.sel >= g.top + VISIBLE_ROWS) g.sel = g.top + VISIBLE_ROWS - 1;
    return true;
  }
  if (!(down & KEY_TOUCH)) return false;
  if (tp->py >= 20 && tp->py < 40) return access_ui_handle(KEY_B);
  r = ui_row_at(tp->py);
  if (r < 0 || g.top + r >= g.n) return false;
  if (g.top + r != g.sel) { g.sel = g.top + r; return true; }
  if (tp->px < 64) cycle_selected();
  else if (g.sel > 0) return access_ui_handle(KEY_A);
  return true;
}

void access_ui_draw(PrintConsole *top, PrintConsole *bottom) {
  int i, end;
  char info[NDP_PATH_MAX + 8];
  (void)top;
  (void)bottom;
  ui_begin();
  ui_scene_top(UI_RGB(0x2b, 0x2d, 0x33));
  ui_rect(0, 0, 400, 24, UI_RGB(0x1b, 0x1c, 0x20));
  ui_text(8, 3, 0.6f, UI_RGB(0x6a, 0xc8, 0xf0), 0, "%s", UI_T("Open folders", "Pastas liberadas"));
  ui_text(392, 5, 0.45f, UI_RGB(0x9a, 0xa0, 0xaa), UI_RIGHT, "%s", UI_T("what the Bridge may use", "o que o Bridge pode usar"));
  ui_text(12, 36, 0.5f, UI_RGB(0xff, 0xff, 0xff), 0, "%.52s", g.path);
  ui_text(12, 70, 0.42f, UI_RGB(0x9a, 0xa0, 0xaa), 0, "[R] %s    [RW] %s", UI_T("read", "leitura"), UI_T("read + write", "leitura e escrita"));
  ui_text(12, 86, 0.42f, UI_RGB(0x9a, 0xa0, 0xaa), 0, "(R) %s", UI_T("open through a parent folder", "aberta por uma pasta pai"));
  ui_text(12, 102, 0.42f, UI_RGB(0x9a, 0xa0, 0xaa), 0, "! %s", UI_T("system folder: never writable", "pasta do sistema: nunca gravavel"));
  ui_text(12, 128, 0.42f, UI_RGB(0x9a, 0xa0, 0xaa), 0, "%s", UI_T("Writes also need Development mode (X).", "Escrever tambem exige o modo Desenvolvimento (X)."));
  ui_text(12, 144, 0.42f, UI_RGB(0x9a, 0xa0, 0xaa), 0, "%s", UI_T("The agent's own folder is always open.", "A pasta do proprio agente esta sempre aberta."));
  if (g.skipped) ui_text(12, 168, 0.42f, UI_RGB(0xf0, 0x5a, 0x50), 0, "%d %s", g.skipped, UI_T("folder(s) not shown (too many / long name)", "pasta(s) nao exibida(s) (muitas / nome longo)"));
  if (g.msg[0]) ui_text(12, 190, 0.5f, g.msg_bad ? UI_RGB(0xf0, 0x5a, 0x50) : UI_RGB(0x6b, 0xd0, 0x6b), 0, "%.60s", g.msg);
  ui_rect(0, 220, 400, 20, UI_RGB(0x3f, 0x41, 0x47));

  snprintf(info, sizeof info, "%s", g.path);
  ui_scene_bottom(UI_RGB(0xe8, 0xe8, 0xe8));
  ui_bottom_chrome(info, UI_T("< Up (B)    tag: change    name: open", "< Subir (B)    tag: acesso    nome: abrir"),
                   UI_T("A: open  Y: access  B: back  START: close", "A: abrir  Y: acesso  B: voltar  START: fechar"));
  end = g.top + VISIBLE_ROWS < g.n ? g.top + VISIBLE_ROWS : g.n;
  for (i = g.top; i < end; i++) {
    char path[NDP_PATH_MAX + 1];
    ndp_level exp, eff;
    const char *tag;
    float y = (float)(UI_LIST_Y + (i - g.top) * UI_ROW_H);
    const uint32_t ink = UI_RGB(0x22, 0x22, 0x22);
    row_path(i, path, sizeof path);
    eff = ndp_access_level(g.list, path, &exp);
    if (exp != NDP_LVL_NONE) tag = exp == NDP_LVL_WRITE ? "[RW]" : "[R]";
    else if (eff != NDP_LVL_NONE) tag = eff == NDP_LVL_WRITE ? "(RW)" : "(R)";
    else tag = "[ ]";
    if (i == g.sel) ui_rect(0, y, 320, UI_ROW_H, UI_RGB(0xb4, 0xb4, 0xb4));
    ui_text(8, y + 4, 0.5f, exp != NDP_LVL_NONE ? UI_RGB(0x1b, 0x7a, 0x2b) : ink, 0, "%s", tag);
    ui_text(60, y + 4, 0.5f, ink, 0, "%.34s%s", i == 0 ? UI_T(". (this folder)", ". (esta pasta)") : g.names[i],
            !ndp_access_allowed(path, NDP_LVL_WRITE) ? "  !" : "");
  }
  ui_end();
}
#endif
