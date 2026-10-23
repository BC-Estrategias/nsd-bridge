#ifndef AGENT_TEXT_UI
/* The Fontes screen: browse an arbitrary folder on the owner's own Nextcloud share (sources.c) and download one chosen file to
 * a folder opened for writing on the console. Unlike Translations/Mods there is no catalog: this is a plain two-pane file
 * picker (source folder, then destination folder), one file at a time. */

/* Copies into fixed buffers are bounded on purpose: a too-long name is cut, never overrun. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "sources.h"
#include "ui_internal.h"

enum { USRC_BROWSE, USRC_DEST, USRC_CONFIRM };
#define DST_MAX 256

static int ust = USRC_BROWSE;
static int sel, top;                 /* the source listing */
static char browse_path[400] = "";   /* what is shown / was last asked for */
static char link_msg[120];
static int chosen_idx;
static char chosen_name[200];
static uint64_t chosen_size;

static struct {
  char path[400]; /* "/roms/gba", no "sdmc:" */
  struct { char name[160]; } e[DST_MAX];
  int n, sel, top;
  bool truncated;
} dst;

static bool tap_in(const touchPosition *t, int x0, int y0, int x1, int y1) { return t->px >= x0 && t->px < x1 && t->py >= y0 && t->py < y1; }

void uisrc_set_message(const char *m) { snprintf(link_msg, sizeof link_msg, "%s", m ? m : ""); }

static void fmt_size(char *out, size_t cap, uint64_t b) {
  if (b >= 1024ull * 1024ull * 1024ull) snprintf(out, cap, "%.2f GiB", (double)b / (1024.0 * 1024.0 * 1024.0));
  else if (b >= 1024ull * 1024ull) snprintf(out, cap, "%.1f MiB", (double)b / (1024.0 * 1024.0));
  else snprintf(out, cap, "%llu KiB", (unsigned long long)((b + 1023) / 1024));
}

static void button(float x, float y, uint32_t col, const char *label) {
  ui_rect(x, y, 145, 50, col);
  ui_text(x + 72, y + 14, 0.55f, COL_WHITE, UI_CENTER, "%s", label);
}

static void top_header(const ui_model *m) {
  ui_scene_top(COL_TOP_BG);
  ui_rect(0, 0, 400, 24, COL_TOP_HDR);
  ui_text(8, 3, 0.6f, COL_CYAN, 0, "NSD Bridge");
  ui_text(392, 5, 0.45f, m->build_tag[0] ? COL_YELLOW : COL_DIM, UI_RIGHT, "%s  v%s%s%s", UI_T("Sources", "Fontes"), m->version, m->build_tag[0] ? " " : "", m->build_tag);
}

/* ------------------------------------------------------------------------------------------------ the source browser */
static void sel_clamp(int n) {
  if (sel >= n) sel = n ? n - 1 : 0;
  if (sel < 0) sel = 0;
  if (sel < top) top = sel;
  if (sel >= top + UI_LIST_ROWS) top = sel - UI_LIST_ROWS + 1;
  if (top < 0) top = 0;
}

void uisrc_open(void) {
  ust = USRC_BROWSE;
  sel = top = 0;
  browse_path[0] = '\0';
  link_msg[0] = '\0';
  if (src_configured()) src_list("");
}

/* ------------------------------------------------------------------------------------------------ the destination browser */
static void dst_load(const char *select) {
  char fs[432];
  DIR *d;
  struct dirent *de;
  int i;
  dst.n = 0;
  dst.truncated = false;
  if (strcmp(dst.path, "/") != 0) {
    snprintf(dst.e[0].name, sizeof dst.e[0].name, "..");
    dst.n = 1;
  }
  snprintf(fs, sizeof fs, "sdmc:%s", dst.path);
  d = opendir(fs);
  if (d) {
    int first = dst.n;
    while ((de = readdir(d)) != NULL) {
      if (de->d_type != DT_DIR || de->d_name[0] == '.') continue;
      if (dst.n >= DST_MAX) { dst.truncated = true; break; }
      snprintf(dst.e[dst.n].name, sizeof dst.e[dst.n].name, "%s", de->d_name);
      dst.n++;
    }
    closedir(d);
    for (i = first + 1; i < dst.n; i++) {
      int j = i;
      while (j > first && strcasecmp(dst.e[j - 1].name, dst.e[j].name) > 0) {
        char t[160];
        snprintf(t, sizeof t, "%s", dst.e[j - 1].name);
        snprintf(dst.e[j - 1].name, sizeof dst.e[j - 1].name, "%s", dst.e[j].name);
        snprintf(dst.e[j].name, sizeof dst.e[j].name, "%s", t);
        j--;
      }
    }
  }
  dst.sel = 0;
  if (select) for (i = 0; i < dst.n; i++) if (!strcasecmp(dst.e[i].name, select)) { dst.sel = i; break; }
  dst.top = dst.sel >= UI_LIST_ROWS ? dst.sel - UI_LIST_ROWS + 1 : 0;
}

static void dst_up(void) {
  char from[160];
  char *slash = strrchr(dst.path, '/');
  snprintf(from, sizeof from, "%s", slash ? slash + 1 : "");
  if (slash == dst.path) strcpy(dst.path, "/");
  else if (slash) *slash = '\0';
  dst_load(from);
}

static void dst_enter(void) {
  char name[160];
  if (dst.n == 0) return;
  if (!strcmp(dst.e[dst.sel].name, "..")) { dst_up(); return; }
  snprintf(name, sizeof name, "%s", dst.e[dst.sel].name);
  if (strlen(dst.path) + strlen(name) + 2 < sizeof dst.path) {
    size_t l = strlen(dst.path);
    snprintf(dst.path + l, sizeof dst.path - l, "%s%s", l > 1 ? "/" : "", name);
    dst_load(NULL);
  }
}

/* Called from main.c after a new folder was created (UI_SRC_NEWFOLDER) so the browser shows it. */
void uisrc_dest_refresh(const char *select) { dst_load(select); }
const char *uisrc_dest_path(void) { return dst.path; } /* where main.c's "new folder" prompt creates the folder */

/* Row `sel` of the source listing: a folder descends into it, a file moves on to picking a destination. */
static void browse_activate(void) {
  const src_entry *e = src_at(sel);
  if (!e) return;
  if (e->is_dir) {
    char next[400];
    snprintf(next, sizeof next, "%s%s%s", browse_path, browse_path[0] ? "/" : "", e->name);
    snprintf(browse_path, sizeof browse_path, "%s", next);
    src_list(browse_path);
  } else {
    chosen_idx = sel;
    snprintf(chosen_name, sizeof chosen_name, "%s", e->name);
    chosen_size = e->size;
    snprintf(dst.path, sizeof dst.path, "/");
    dst_load(NULL);
    ust = USRC_DEST;
  }
}

/* ------------------------------------------------------------------------------------------------ input */
ui_action uisrc_handle(u32 down, u32 held, const touchPosition *touch, bool *redraw, bool *leave) {
  src_status s;
  *leave = false;
  src_get(&s);

  if (s.state == SRS_DOWNLOADING) {
    if (down & KEY_B) { src_cancel(); *redraw = true; }
    *redraw = true;
    return UI_NONE;
  }
  if (s.state == SRS_DONE || s.state == SRS_ERROR) {
    if (down & (KEY_A | KEY_B) || (down & KEY_TOUCH)) { src_dismiss(); ust = USRC_BROWSE; *redraw = true; }
    return UI_NONE;
  }

  if (!src_configured()) {
    if ((down & KEY_A) || ((down & KEY_TOUCH) && tap_in(touch, 50, 70, 320, 130))) return UI_SRC_LINK;
    if ((down & KEY_B) || ((down & KEY_TOUCH) && touch->py >= 20 && touch->py < 40)) { *leave = true; *redraw = true; }
    return UI_NONE;
  }

  if (ust == USRC_CONFIRM) {
    if (down & KEY_A) {
      char full[400];
      snprintf(full, sizeof full, "sdmc:%s", dst.path);
      src_download(chosen_idx, full, chosen_name);
      *redraw = true;
    } else if (down & KEY_B) { ust = USRC_DEST; *redraw = true; }
    return UI_NONE;
  }

  if (ust == USRC_DEST) {
    int step = ui_drag_rows(down, held, touch), max = dst.n > UI_LIST_ROWS ? dst.n - UI_LIST_ROWS : 0;
    bool w = nsd_write_allowed(dst.path);
    if (step) {
      dst.top += step;
      if (dst.top < 0) dst.top = 0;
      if (dst.top > max) dst.top = max;
      if (dst.sel < dst.top) dst.sel = dst.top;
      if (dst.sel >= dst.top + UI_LIST_ROWS) dst.sel = dst.top + UI_LIST_ROWS - 1;
      *redraw = true;
    }
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (dst.n) dst.sel = (dst.sel + 1) % dst.n; *redraw = true; }
    else if (down & (KEY_DUP | KEY_CPAD_UP)) { if (dst.n) dst.sel = (dst.sel + dst.n - 1) % dst.n; *redraw = true; }
    else if ((down & KEY_A) && dst.n) { dst_enter(); *redraw = true; }
    else if (down & KEY_X) { if (w) { ust = USRC_CONFIRM; *redraw = true; } }
    else if (down & KEY_Y) { if (w) { *redraw = true; return UI_SRC_NEWFOLDER; } }
    else if (down & KEY_B) {
      if (strcmp(dst.path, "/") == 0) { ust = USRC_BROWSE; } else dst_up();
      *redraw = true;
    } else if (down & KEY_TOUCH) {
      int r = ui_row_at(touch->py);
      if (touch->py >= 20 && touch->py < 40) { if (strcmp(dst.path, "/") == 0) ust = USRC_BROWSE; else dst_up(); *redraw = true; return UI_NONE; }
      if (r >= 0 && dst.top + r < dst.n) { *redraw = true; if (dst.top + r == dst.sel) dst_enter(); else dst.sel = dst.top + r; }
    }
    if (dst.sel < dst.top) dst.top = dst.sel;
    if (dst.sel >= dst.top + UI_LIST_ROWS) dst.top = dst.sel - UI_LIST_ROWS + 1;
    return UI_NONE;
  }

  /* ---- USRC_BROWSE ---- */
  if (s.state == SRS_LISTING) {
    *redraw = true;
    return UI_NONE;
  }
  {
    int n = src_count(), step = ui_drag_rows(down, held, touch), max = n > UI_LIST_ROWS ? n - UI_LIST_ROWS : 0;
    if (step) {
      top += step;
      if (top < 0) top = 0;
      if (top > max) top = max;
      if (sel < top) sel = top;
      if (sel >= top + UI_LIST_ROWS) sel = top + UI_LIST_ROWS - 1;
      *redraw = true;
    }
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (n) sel = (sel + 1) % n; *redraw = true; }
    else if (down & (KEY_DUP | KEY_CPAD_UP)) { if (n) sel = (sel + n - 1) % n; *redraw = true; }
    else if (down & KEY_SELECT) return UI_SRC_LINK;
    else if ((down & KEY_A) && n) { browse_activate(); *redraw = true; }
    else if (down & KEY_B) {
      if (!browse_path[0]) { *leave = true; }
      else {
        char *slash = strrchr(browse_path, '/');
        if (slash) *slash = '\0'; else browse_path[0] = '\0';
        src_list(browse_path);
      }
      *redraw = true;
    } else if (down & KEY_TOUCH) {
      int r = ui_row_at(touch->py);
      if (touch->py >= 20 && touch->py < 40) {
        if (!browse_path[0]) *leave = true;
        else { char *slash = strrchr(browse_path, '/'); if (slash) *slash = '\0'; else browse_path[0] = '\0'; src_list(browse_path); }
        *redraw = true;
        return UI_NONE;
      }
      if (r >= 0 && top + r < n) {
        *redraw = true;
        if (top + r == sel) browse_activate();
        else sel = top + r;
      }
    }
    sel_clamp(n);
  }
  return UI_NONE;
}

/* ------------------------------------------------------------------------------------------------ draw */
void uisrc_draw(const ui_model *m) {
  src_status s;
  char val[48], sz[24];
  int i;
  src_get(&s);
  top_header(m);

  if (s.state == SRS_DOWNLOADING || s.state == SRS_DONE || s.state == SRS_ERROR) {
    if (s.state == SRS_DOWNLOADING) {
      float frac = s.bytes_total ? (float)((double)s.bytes_done / (double)s.bytes_total) : 0.0f;
      char d[24];
      if (frac > 1.0f) frac = 1.0f;
      ui_text(200, 80, 0.5f, COL_WHITE, UI_CENTER, "%.40s", s.name);
      ui_rect(30, 110, 340, 14, COL_BAR);
      ui_rect(30, 110, 340.0f * frac, 14, COL_GREEN);
      fmt_size(d, sizeof d, s.bytes_done);
      fmt_size(sz, sizeof sz, s.bytes_total);
      ui_text(200, 130, 0.42f, COL_WHITE, UI_CENTER, "%u%%   %s / %s   %.2f MiB/s", (unsigned)(frac * 100.0f), d, sz, (double)s.bps / 1048576.0);
      if (s.eta_s > 0) ui_text(200, 150, 0.42f, COL_CYAN, UI_CENTER, "%s ~%u:%02u", UI_T("Time left", "Tempo restante"), s.eta_s / 60, s.eta_s % 60);
    } else if (s.state == SRS_DONE) {
      ui_text(200, 120, 0.7f, COL_GREEN, UI_CENTER, "%s", UI_T("Downloaded", "Baixado"));
      ui_text(200, 150, 0.42f, COL_WHITE, UI_CENTER, "%.40s", s.name);
    } else {
      const char *t;
      switch (s.error) {
        case SRE_EXISTS: t = UI_T("A file with this name is already there.", "Já existe um arquivo com este nome."); break;
        case SRE_SPACE: t = UI_T("Not enough space on the SD card.", "Sem espaço suficiente no cartão SD."); break;
        case SRE_SIZE: t = UI_T("Wrong size: the download did not finish right.", "Tamanho errado: o download não terminou certo."); break;
        case SRE_CANCELLED: t = UI_T("Cancelled.", "Cancelado."); break;
        case SRE_NET: t = s.http_code >= 500 ? UI_T("Server is down.", "Servidor fora do ar.") : UI_T("Could not reach the share.", "Não conseguiu acessar o link."); break;
        default: t = UI_T("Could not write the file.", "Não consegui gravar o arquivo."); break;
      }
      ui_text(200, 130, 0.45f, COL_RED, UI_CENTER, "%s", t);
    }
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Sources", "Fontes"),
                      s.state == SRS_DOWNLOADING ? UI_T("Downloading", "Baixando") : s.state == SRS_DONE ? UI_T("Done", "Concluído") : UI_T("Failed", "Falhou"),
                      s.state == SRS_DOWNLOADING ? UI_T("B: Cancel", "B: Cancelar") : "A / B: OK");
    if (s.state == SRS_DOWNLOADING) button(88, 160, UI_RGB(0xe5, 0x39, 0x35), UI_T("B: Cancel", "B: Cancelar"));
    else button(88, 160, UI_RGB(0x42, 0x7a, 0xb8), "OK");
    return;
  }

  if (!src_configured()) {
    ui_text(12, 40, 0.5f, COL_WHITE, 0, "%s", UI_T("No source yet.", "Ainda sem fonte."));
    ui_text(12, 66, 0.4f, COL_DIM, 0, "%s", UI_T("Paste the public link of a Nextcloud folder", "Cole o link público de uma pasta do Nextcloud"));
    ui_text(12, 82, 0.4f, COL_DIM, 0, "%s", UI_T("(https://.../s/xxxxxxx). You choose what to browse.", "(https://.../s/xxxxxxx). Você escolhe o que navegar."));
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Sources", "Fontes"), UI_T("< Back  (B)", "< Voltar  (B)"), UI_T("A: Type the link", "A: Digitar o link"));
    button(85, 90, UI_RGB(0x4c, 0xaf, 0x50), UI_T("A: Link", "A: Link"));
    if (link_msg[0]) ui_text(160, 160, 0.42f, COL_INK_RED, UI_CENTER, "%s", link_msg);
    return;
  }

  if (ust == USRC_CONFIRM) {
    char full[432];
    snprintf(full, sizeof full, "%s/%s", dst.path, chosen_name);
    ui_text(12, 40, 0.5f, COL_WHITE, 0, "%.40s", chosen_name);
    fmt_size(sz, sizeof sz, chosen_size);
    ui_text(12, 64, 0.42f, COL_DIM, 0, "%s", sz);
    ui_text(12, 86, 0.4f, COL_WHITE, 0, "%s", UI_T("Download to:", "Baixar para:"));
    ui_text(12, 104, 0.4f, COL_CYAN, 0, "%.60s", full);
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Sources", "Fontes"), UI_T("Download this file?", "Baixar este arquivo?"), UI_T("A: Yes   B: Cancel", "A: Sim   B: Cancelar"));
    button(10, 145, UI_RGB(0x4c, 0xaf, 0x50), UI_T("A: Download", "A: Baixar"));
    button(165, 145, UI_RGB(0x42, 0x7a, 0xb8), UI_T("B: Cancel", "B: Cancelar"));
    return;
  }

  if (ust == USRC_DEST) {
    bool w = nsd_write_allowed(dst.path);
    char title[420];
    ui_text(12, 40, 0.5f, COL_WHITE, 0, "%.40s", chosen_name);
    ui_text(12, 64, 0.4f, COL_DIM, 0, "%s", UI_T("Choose a folder to download it into.", "Escolha uma pasta para baixar nele."));
    ui_scene_bottom(COL_BOT_BG);
    snprintf(title, sizeof title, "< %s", dst.path);
    ui_bottom_chrome(w ? UI_T("Writable", "Pode escrever") : UI_T("Read-only", "Só leitura"), title,
                      w ? UI_T("A: Open   X: Download here   Y: New folder", "A: Abrir   X: Baixar aqui   Y: Nova pasta") : UI_T("A: Open   B: Back", "A: Abrir   B: Voltar"));
    if (dst.n == 0) ui_text(160, 100, 0.5f, COL_DIM, UI_CENTER, "%s", UI_T("No sub-folders here.", "Nenhuma subpasta aqui."));
    for (i = dst.top; i < dst.n && i < dst.top + UI_LIST_ROWS; i++)
      ui_list_row(i, dst.sel, dst.top, dst.e[i].name, "", COL_INK);
    return;
  }

  /* ---- USRC_BROWSE ---- */
  if (s.state == SRS_LISTING) {
    ui_text(200, 110, 0.55f, COL_YELLOW, UI_CENTER, "%s", UI_T("Reading the folder...", "Lendo a pasta..."));
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Sources", "Fontes"), UI_T("Please wait", "Aguarde"), "");
    return;
  }
  if (s.state == SRS_ERROR && src_count() == 0 && !browse_path[0]) {
    const char *t = s.http_code >= 500 ? UI_T("Server is down.", "Servidor fora do ar.") : UI_T("Could not reach the share.", "Não conseguiu acessar o link.");
    ui_text(200, 110, 0.5f, COL_RED, UI_CENTER, "%s", t);
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Sources", "Fontes"), UI_T("Failed", "Falhou"), UI_T("B: Back   SELECT: Link", "B: Voltar   SELECT: Link"));
    return;
  }
  {
    int n = src_count();
    char title[420];
    ui_scene_bottom(COL_BOT_BG);
    snprintf(title, sizeof title, "< /%s", browse_path);
    ui_bottom_chrome(UI_T("Sources", "Fontes"), title, UI_T("A: Open   SELECT: Link", "A: Abrir   SELECT: Link"));
    if (n == 0) ui_text(160, 100, 0.5f, COL_DIM, UI_CENTER, "%s", UI_T("Nothing here.", "Nada aqui."));
    for (i = top; i < n && i < top + UI_LIST_ROWS; i++) {
      const src_entry *e = src_at(i);
      char nm[140];
      snprintf(nm, sizeof nm, "%s %.34s", e->is_dir ? "\xF0\x9F\x93\x81" : " ", e->name); /* folder emoji */
      if (e->is_dir) val[0] = '\0'; else fmt_size(val, sizeof val, e->size);
      ui_list_row(i, sel, top, nm, val, COL_INK);
    }
  }
}
#endif
