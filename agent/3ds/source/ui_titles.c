#ifndef AGENT_TEXT_UI
/* The Titles screens: the list of installed titles (details and icon on the top screen), the actions for one title, the
 * confirmations, and the pending titles (interrupted installs). All AM work is in titles.c; this only draws and reads buttons. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "alog.h"
#include "titles.h"
#include "ui_internal.h"

enum { TU_LIST, TU_ACTIONS, TU_CONFIRM, TU_MSG, TU_PENDING, TU_PCONFIRM };
enum { A_LAUNCH, A_DELETE, A_DELETE_TICKET };

static int st = TU_LIST, view[640], nview, sel, top, filter, sortk, act_sel, act_rows[3], n_act, confirm_act, psel, ptop;
static unsigned built_rev = ~0u;
static bool after_job, pending_all, msg_is_pending;
static C3D_Tex icon_tex;
static bool icon_inited, icon_have;
static const void *icon_key;

static const char *filter_name(void) {
  switch (filter) {
    case 0: return UI_T("Apps and games", "Apps e jogos");
    case 1: return UI_T("Updates and DLC", "Atualizações e DLC");
    case 2: return UI_T("System", "Sistema");
    default: return UI_T("All", "Todos");
  }
}
static const char *sort_name(void) { return sortk == 0 ? UI_T("by name", "por nome") : sortk == 1 ? UI_T("by size", "por tamanho") : UI_T("by ID", "por ID"); }

static bool in_filter(const title_entry *t) {
  switch (filter) {
    case 0: return t->kind == KIND_APP || t->kind == KIND_DSIWARE;
    case 1: return t->kind == KIND_UPDATE || t->kind == KIND_DLC;
    case 2: return t->kind == KIND_SYSTEM;
    default: return true;
  }
}

static int cmp_view(const void *a, const void *b) {
  const title_entry *x = titles_at(*(const int *)a), *y = titles_at(*(const int *)b);
  if (sortk == 1) { if (x->size != y->size) return x->size < y->size ? 1 : -1; }
  else if (sortk == 2) { if (x->id != y->id) return x->id < y->id ? -1 : 1; }
  return strcasecmp(x->name, y->name);
}

static void view_rebuild(void) {
  const title_entry *keep = sel < nview ? titles_at(view[sel]) : NULL;
  uint64_t keep_id = keep ? keep->id : 0;
  uint8_t keep_media = keep ? keep->media : 0;
  int i, n = titles_count();
  nview = 0;
  for (i = 0; i < n && nview < 640; i++) if (in_filter(titles_at(i))) view[nview++] = i;
  qsort(view, (size_t)nview, sizeof view[0], cmp_view);
  if (keep) for (i = 0; i < nview; i++) if (titles_at(view[i])->id == keep_id && titles_at(view[i])->media == keep_media) { sel = i; break; }
  if (sel >= nview) sel = nview ? nview - 1 : 0;
  if (sel < top) top = sel;
  if (sel >= top + UI_LIST_ROWS) top = sel - UI_LIST_ROWS + 1;
  if (top < 0) top = 0;
  icon_key = (const void *)1; /* the icon buffers may have been freed and reused */
}

void uit_open(void) {
  st = TU_LIST;
  sel = top = 0;
  nview = 0;
  built_rev = ~0u;
  after_job = false;
  titles_load();
}

void uit_shutdown(void) {
  if (icon_inited) { C3D_TexDelete(&icon_tex); icon_inited = false; }
}

static bool busy(void) { int s = titles_state(); return s == TS_LOADING || s == TS_BUSY; }
static const title_entry *cur(void) { return nview ? titles_at(view[sel]) : NULL; }
static int cur_idx(void) { return nview ? view[sel] : -1; }

static void build_actions(const title_entry *t) {
  n_act = 0;
  act_sel = 0;
  if (!t || titles_is_self(t)) return;
  if (t->kind == KIND_APP || t->kind == KIND_DSIWARE) act_rows[n_act++] = A_LAUNCH;
  if (titles_can_delete(t)) { act_rows[n_act++] = A_DELETE; act_rows[n_act++] = A_DELETE_TICKET; }
}

/* ------------------------------------------------------------------------------------------------ input */
static bool tap_in(const touchPosition *t, int x0, int y0, int x1, int y1) { return t->px >= x0 && t->px < x1 && t->py >= y0 && t->py < y1; }

static void list_move(int *s, int *tp, int n, u32 down, u32 held, const touchPosition *touch, bool *redraw, int *tapped) {
  int step = ui_drag_rows(down, held, touch), max = n > UI_LIST_ROWS ? n - UI_LIST_ROWS : 0;
  *tapped = -1;
  if (step) {
    *tp += step;
    if (*tp < 0) *tp = 0;
    if (*tp > max) *tp = max;
    if (*s < *tp) *s = *tp;
    if (*s >= *tp + UI_LIST_ROWS) *s = *tp + UI_LIST_ROWS - 1;
    *redraw = true;
  }
  if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (n) *s = (*s + 1) % n; *redraw = true; }
  else if (down & (KEY_DUP | KEY_CPAD_UP)) { if (n) *s = (*s + n - 1) % n; *redraw = true; }
  else if (down & KEY_R) { *s = *s + 8 < n ? *s + 8 : n ? n - 1 : 0; *redraw = true; }
  else if (down & KEY_L) { *s = *s > 8 ? *s - 8 : 0; *redraw = true; }
  else if ((down & KEY_TOUCH) && touch->py >= UI_LIST_Y) {
    int r = ui_row_at(touch->py);
    if (r >= 0 && *tp + r < n) { *redraw = true; if (*tp + r == *s) *tapped = *s; else *s = *tp + r; }
  }
  if (*s < *tp) *tp = *s;
  if (*s >= *tp + UI_LIST_ROWS) *tp = *s - UI_LIST_ROWS + 1;
}

void uit_handle(u32 down, u32 held, const touchPosition *touch, bool *redraw, bool *leave) {
  int tapped;
  *leave = false;
  /* a job finished since the last frame: rebuild the list; a failed delete says why */
  if (!busy()) {
    unsigned rev = titles_revision();
    if (rev != built_rev && st != TU_PENDING && st != TU_PCONFIRM) { built_rev = rev; view_rebuild(); *redraw = true; }
    if (after_job) {
      after_job = false;
      *redraw = true;
      if (titles_message()[0]) { st = TU_MSG; return; }
      if (st == TU_PCONFIRM) st = TU_PENDING;
      else if (st == TU_CONFIRM) st = TU_LIST;
    }
  }
  if (busy()) { *redraw = true; return; } /* keep the "working" screen fresh; no input while AM works */
  if (st == TU_LIST) {
    if ((down & (KEY_B | KEY_START)) || ((down & KEY_TOUCH) && tap_in(touch, 0, 20, 320, 40))) { *leave = true; *redraw = true; return; }
    if (down & KEY_X) { filter = (filter + 1) % 4; sel = top = 0; view_rebuild(); *redraw = true; return; }
    if (down & KEY_Y) { sortk = (sortk + 1) % 3; view_rebuild(); *redraw = true; return; }
    if (down & KEY_SELECT) { st = TU_PENDING; psel = ptop = 0; msg_is_pending = true; titles_pending_load(); *redraw = true; return; }
    list_move(&sel, &top, nview, down, held, touch, redraw, &tapped);
    if (((down & KEY_A) && nview) || tapped >= 0) { build_actions(cur()); st = TU_ACTIONS; *redraw = true; }
  } else if (st == TU_ACTIONS) {
    if ((down & KEY_B) || ((down & KEY_TOUCH) && tap_in(touch, 0, 20, 320, 40))) { st = TU_LIST; *redraw = true; return; }
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (n_act) act_sel = (act_sel + 1) % n_act; *redraw = true; }
    else if (down & (KEY_DUP | KEY_CPAD_UP)) { if (n_act) act_sel = (act_sel + n_act - 1) % n_act; *redraw = true; }
    else if ((down & KEY_A) && n_act) { confirm_act = act_rows[act_sel]; st = TU_CONFIRM; *redraw = true; }
    else if ((down & KEY_TOUCH) && touch->py >= UI_LIST_Y) {
      int r = ui_row_at(touch->py);
      if (r >= 0 && r < n_act) { *redraw = true; if (r == act_sel) { confirm_act = act_rows[act_sel]; st = TU_CONFIRM; } else act_sel = r; }
    }
  } else if (st == TU_CONFIRM) {
    bool yes = (down & KEY_A) || ((down & KEY_TOUCH) && tap_in(touch, 10, 145, 155, 195));
    bool no = (down & KEY_B) || ((down & KEY_TOUCH) && tap_in(touch, 165, 145, 310, 195));
    if (no) { st = TU_ACTIONS; *redraw = true; return; }
    if (yes) {
      const title_entry *t = cur();
      *redraw = true;
      if (!t) { st = TU_LIST; return; }
      if (confirm_act == A_LAUNCH) {
        Result r = titles_launch(cur_idx());
        if (R_FAILED(r)) { alog("Titles: launch failed 0x%08lX", (unsigned long)r); st = TU_LIST; }
      } else {
        after_job = true;
        msg_is_pending = false;
        titles_delete(cur_idx(), confirm_act == A_DELETE_TICKET);
      }
    }
  } else if (st == TU_MSG) {
    if ((down & (KEY_A | KEY_B)) || (down & KEY_TOUCH)) { st = msg_is_pending ? TU_PENDING : TU_LIST; *redraw = true; }
  } else if (st == TU_PENDING) {
    int n = pending_count();
    if ((down & (KEY_B | KEY_START)) || ((down & KEY_TOUCH) && tap_in(touch, 0, 20, 320, 40))) { st = TU_LIST; built_rev = ~0u; *redraw = true; return; }
    if ((down & KEY_Y) && n) { pending_all = true; st = TU_PCONFIRM; *redraw = true; return; }
    list_move(&psel, &ptop, n, down, held, touch, redraw, &tapped);
    if (((down & KEY_A) && n) || tapped >= 0) { pending_all = false; st = TU_PCONFIRM; *redraw = true; }
  } else if (st == TU_PCONFIRM) {
    bool yes = (down & KEY_A) || ((down & KEY_TOUCH) && tap_in(touch, 10, 145, 155, 195));
    bool no = (down & KEY_B) || ((down & KEY_TOUCH) && tap_in(touch, 165, 145, 310, 195));
    if (no) { st = TU_PENDING; *redraw = true; return; }
    if (yes) { after_job = true; msg_is_pending = true; titles_pending_delete(pending_all ? -1 : psel); *redraw = true; }
  }
}

/* ------------------------------------------------------------------------------------------------ drawing */
static void fmt_size(char *out, size_t cap, uint64_t b) {
  if (b >= 1024ull * 1024ull * 1024ull) snprintf(out, cap, "%.2f GiB", (double)b / (1024.0 * 1024.0 * 1024.0));
  else if (b >= 1024ull * 1024ull) snprintf(out, cap, "%.1f MiB", (double)b / (1024.0 * 1024.0));
  else snprintf(out, cap, "%llu KiB", (unsigned long long)((b + 1023) / 1024));
}
static const char *media_name(uint8_t m) { return m == MEDIATYPE_SD ? "SD" : m == MEDIATYPE_NAND ? UI_T("System", "Sistema") : UI_T("Card", "Cartão"); }
static const char *kind_name(const title_entry *t) {
  switch (t->kind) {
    case KIND_APP: return UI_T("Application", "Aplicativo");
    case KIND_UPDATE: return UI_T("Update", "Atualização");
    case KIND_DLC: return "DLC";
    case KIND_DSIWARE: return "DSiWare";
    default: return UI_T("System title", "Título de sistema");
  }
}

static void icon_use(const uint16_t *src) {
  int ty;
  if (!icon_inited) icon_inited = C3D_TexInit(&icon_tex, 64, 64, GPU_RGB565);
  if (!icon_inited) { icon_have = false; return; }
  if (src == icon_key) return;
  icon_key = src;
  if (!src) { icon_have = false; return; }
  memset(icon_tex.data, 0, 64 * 64 * 2);
  /* the SMDH stores 6 tiles per row of tiles; the 64-wide texture has 8 */
  for (ty = 0; ty < 6; ty++) memcpy((uint8_t *)icon_tex.data + ty * 1024, (const uint8_t *)src + ty * 768, 768);
  GSPGPU_FlushDataCache(icon_tex.data, 64 * 64 * 2);
  icon_have = true;
}

static void draw_top_header(const ui_model *m) {
  ui_scene_top(COL_TOP_BG);
  ui_rect(0, 0, 400, 24, COL_TOP_HDR);
  ui_text(8, 3, 0.6f, COL_CYAN, 0, "NSD Bridge");
  ui_text(392, 5, 0.45f, m->build_tag[0] ? COL_YELLOW : COL_DIM, UI_RIGHT, "%s  v%s%s%s", UI_T("Titles", "Títulos"), m->version, m->build_tag[0] ? " " : "", m->build_tag);
}

static void draw_details(const title_entry *t) {
  static const Tex3DS_SubTexture sub = {48, 48, 0.0f, 1.0f, 0.75f, 0.25f};
  char size[24], ver[24], id[20];
  float y = 36;
  C2D_Image img;
  fmt_size(size, sizeof size, t->size);
  snprintf(ver, sizeof ver, "%u.%u.%u", t->version >> 10, (t->version >> 4) & 0x3F, t->version & 0xF);
  snprintf(id, sizeof id, "%016llX", (unsigned long long)t->id);
  icon_use(t->icon);
  if (icon_have) {
    img.tex = &icon_tex;
    img.subtex = &sub;
    C2D_DrawImageAt(img, 12, 40, 0.5f, NULL, 2.0f, 2.0f);
  } else {
    ui_rect(12, 40, 96, 96, COL_BAR);
    ui_text(60, 70, 0.9f, COL_DIM, UI_CENTER, "%s", t->twl ? "DSi" : t->kind == KIND_UPDATE ? "UPD" : t->kind == KIND_DLC ? "DLC" : t->kind == KIND_SYSTEM ? "SYS" : "3DS");
  }
  ui_text(120, y, 0.55f, COL_WHITE, 0, "%.34s", t->name);
  y += 24;
  if (t->publisher[0]) { ui_text(120, y, 0.42f, COL_DIM, 0, "%.40s", t->publisher); y += 18; }
  ui_text(120, y, 0.42f, COL_WHITE, 0, "ID  %s", id);
  y += 16;
  if (t->code[0]) { ui_text(120, y, 0.42f, COL_WHITE, 0, "%s  %s", UI_T("Code", "Código"), t->code); y += 16; }
  if (t->twl) ui_text(120, y, 0.42f, COL_WHITE, 0, "%s  %s", UI_T("Size", "Tamanho"), size); /* DSiWare has no usable version */
  else ui_text(120, y, 0.42f, COL_WHITE, 0, "%s  %s    %s  %s", UI_T("Version", "Versão"), ver, UI_T("Size", "Tamanho"), size);
  y += 16;
  ui_text(120, y, 0.42f, t->media == MEDIATYPE_NAND ? COL_YELLOW : COL_GREEN, 0, "%s  -  %s", media_name(t->media), kind_name(t));
  if (t->twl && t->size < 512u * 1024u) ui_text(120, y + 16, 0.4f, COL_DIM, 0, "%s", UI_T("Likely a forwarder: the ROM is on the SD.", "Provável atalho: a ROM está no SD."));
  if (titles_is_self(t)) ui_text(12, 150, 0.42f, COL_CYAN, 0, "%s", UI_T("This is NSD Bridge itself.", "Este é o próprio NSD Bridge."));
  else if (!titles_can_delete(t) && t->media == MEDIATYPE_NAND) ui_text(12, 150, 0.42f, COL_YELLOW, 0, "%s", UI_T("System memory: read-only here.", "Memória do sistema: só leitura aqui."));
}

static void button(float x, float y, uint32_t col, const char *label) {
  ui_rect(x, y, 145, 50, col);
  ui_text(x + 72, y + 14, 0.55f, COL_WHITE, UI_CENTER, "%s", label);
}

static const char *action_label(int a) {
  return a == A_LAUNCH ? UI_T("Launch", "Abrir") : a == A_DELETE ? UI_T("Delete title", "Apagar título") : UI_T("Delete title and ticket", "Apagar título e ticket");
}

void uit_draw(const ui_model *m) {
  char val[40], title[96], size[24];
  int i;
  draw_top_header(m);
  if (busy()) {
    ui_text(200, 110, 0.6f, COL_YELLOW, UI_CENTER, "%s", titles_state() == TS_BUSY ? UI_T("Working...", "Trabalhando...") : UI_T("Reading titles...", "Lendo títulos..."));
    if (titles_state() == TS_LOADING) {
      int d = 0, t = 0;
      titles_progress(&d, &t);
      if (t > 0) ui_text(200, 140, 0.5f, COL_WHITE, UI_CENTER, "%d / %d", d, t);
      ui_text(200, 170, 0.38f, COL_DIM, UI_CENTER, "%s", UI_T("The first time is slow; the next ones use a saved copy.", "A primeira vez demora; as próximas usam uma cópia salva."));
    }
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Titles", "Títulos"), UI_T("Please wait", "Aguarde"), "");
    return;
  }
  if (st == TU_PENDING || st == TU_PCONFIRM || (st == TU_MSG && msg_is_pending)) {
    int n = pending_count();
    const pending_entry *p = n ? pending_at(psel) : NULL;
    if (p) {
      fmt_size(size, sizeof size, p->size);
      ui_text(12, 40, 0.5f, COL_WHITE, 0, "%s", UI_T("Interrupted install", "Instalação interrompida"));
      ui_text(12, 62, 0.42f, COL_WHITE, 0, "ID  %016llX", (unsigned long long)p->id);
      ui_text(12, 78, 0.42f, COL_WHITE, 0, "%s  %u.%u.%u    %s  %s", UI_T("Version", "Versão"), p->version >> 10, (p->version >> 4) & 0x3F, p->version & 0xF, UI_T("Size", "Tamanho"), size);
      ui_text(12, 94, 0.42f, COL_YELLOW, 0, "%s  -  %s", media_name(p->media), p->status == 2 ? UI_T("can be resumed", "pode continuar") : p->status == 6 ? UI_T("needs cleanup", "precisa de limpeza") : UI_T("not finished", "não terminada"));
    } else {
      ui_text(200, 110, 0.5f, COL_GREEN, UI_CENTER, "%s", UI_T("No pending titles.", "Nenhum título pendente."));
    }
    ui_scene_bottom(COL_BOT_BG);
    if (st == TU_MSG) {
      ui_bottom_chrome(UI_T("Pending titles", "Títulos pendentes"), UI_T("Could not finish", "Não foi possível"), "A / B: OK");
      ui_text(160, 80, 0.5f, COL_INK_RED, UI_CENTER, "%s", UI_T("The system refused:", "O sistema recusou:"));
      ui_text(160, 104, 0.5f, COL_INK, UI_CENTER, "%.40s", titles_message());
      return;
    }
    if (st == TU_PCONFIRM) {
      ui_bottom_chrome(UI_T("Pending titles", "Títulos pendentes"), pending_all ? UI_T("Delete all of them?", "Apagar todos?") : UI_T("Delete this one?", "Apagar este?"), "");
      ui_text(160, 70, 0.5f, COL_INK_RED, UI_CENTER, "%s", UI_T("This cannot be undone.", "Isto não pode ser desfeito."));
      ui_text(160, 92, 0.4f, COL_INK, UI_CENTER, "%s", UI_T("Only the unfinished install data is removed.", "Só os dados da instalação não terminada."));
      button(10, 145, UI_RGB(0xe5, 0x39, 0x35), UI_T("A: Delete", "A: Apagar"));
      button(165, 145, UI_RGB(0x42, 0x7a, 0xb8), UI_T("B: Cancel", "B: Cancelar"));
      return;
    }
    ui_bottom_chrome(UI_T("Pending titles", "Títulos pendentes"), UI_T("< Titles  (B)", "< Títulos  (B)"), UI_T("A: Delete  Y: Delete all", "A: Apagar  Y: Apagar todos"));
    for (i = ptop; i < n && i < ptop + UI_LIST_ROWS; i++) {
      const pending_entry *q = pending_at(i);
      char nm[24];
      snprintf(nm, sizeof nm, "%016llX", (unsigned long long)q->id);
      fmt_size(val, sizeof val, q->size);
      ui_list_row(i, psel, ptop, nm, val, COL_INK);
    }
    return;
  }
  {
    const title_entry *t = cur();
    if (t) draw_details(t);
    else ui_text(200, 110, 0.5f, COL_DIM, UI_CENTER, "%s", UI_T("Nothing in this view.", "Nada nesta visão."));
    ui_scene_bottom(COL_BOT_BG);
    if (st == TU_MSG) {
      ui_bottom_chrome(UI_T("Titles", "Títulos"), UI_T("Could not finish", "Não foi possível"), "A / B: OK");
      ui_text(160, 80, 0.5f, COL_INK_RED, UI_CENTER, "%s", UI_T("The system refused:", "O sistema recusou:"));
      ui_text(160, 104, 0.5f, COL_INK, UI_CENTER, "%.40s", titles_message());
      return;
    }
    if (st == TU_CONFIRM && t) {
      bool del = confirm_act != A_LAUNCH;
      ui_bottom_chrome(UI_T("Titles", "Títulos"), del ? UI_T("Delete this title?", "Apagar este título?") : UI_T("Launch this title?", "Abrir este título?"), "");
      ui_text(160, 56, 0.5f, COL_INK, UI_CENTER, "%.30s", t->name);
      if (del) {
        ui_text(160, 82, 0.42f, COL_INK_RED, UI_CENTER, "%s", UI_T("This cannot be undone.", "Isto não pode ser desfeito."));
        ui_text(160, 100, 0.38f, COL_INK, UI_CENTER, "%s", UI_T("Its save data may go with it: back it up first.", "O save pode ir junto: faça backup antes."));
        if (confirm_act == A_DELETE_TICKET) ui_text(160, 116, 0.38f, COL_INK_RED, UI_CENTER, "%s", UI_T("The ticket goes too: you may not be able to redownload it.", "O ticket também: pode não dar para baixar de novo."));
      } else {
        ui_text(160, 90, 0.4f, COL_INK, UI_CENTER, "%s", UI_T("NSD Bridge will close.", "O NSD Bridge será fechado."));
      }
      button(10, 145, del ? UI_RGB(0xe5, 0x39, 0x35) : UI_RGB(0x4c, 0xaf, 0x50), del ? UI_T("A: Delete", "A: Apagar") : UI_T("A: Launch", "A: Abrir"));
      button(165, 145, UI_RGB(0x42, 0x7a, 0xb8), UI_T("B: Cancel", "B: Cancelar"));
      return;
    }
    if (st == TU_ACTIONS && t) {
      ui_bottom_chrome(UI_T("Titles", "Títulos"), t->name, UI_T("A: Select   B: Back", "A: Selecionar   B: Voltar"));
      if (n_act == 0) ui_text(160, 100, 0.42f, COL_DIM, UI_CENTER, "%s", titles_is_self(t) ? UI_T("Update NSD Bridge from the main menu.", "Atualize o NSD Bridge pelo menu principal.") : UI_T("No actions for this title.", "Nenhuma ação para este título."));
      for (i = 0; i < n_act; i++) ui_list_row(i, act_sel, 0, action_label(act_rows[i]), "", act_rows[i] == A_LAUNCH ? COL_INK : COL_INK_RED);
      return;
    }
    snprintf(title, sizeof title, "< %s - %s (%d)", filter_name(), sort_name(), nview);
    ui_bottom_chrome(UI_T("Titles", "Títulos"), title, UI_T("A:Open X:Filter Y:Sort SEL:Pending", "A:Abrir X:Filtro Y:Ordem SEL:Pendentes"));
    if (nview == 0) { ui_text(160, 100, 0.45f, COL_DIM, UI_CENTER, "%s", UI_T("Nothing in this view (X: filter).", "Nada nesta visão (X: filtro).")); return; }
    for (i = top; i < nview && i < top + UI_LIST_ROWS; i++) {
      const title_entry *q = titles_at(view[i]);
      fmt_size(size, sizeof size, q->size);
      snprintf(val, sizeof val, "%s %s", media_name(q->media), size);
      ui_list_row(i, sel, top, q->name, val, q->media == MEDIATYPE_NAND && !q->twl ? COL_DIM : COL_INK);
    }
  }
}
#endif /* !AGENT_TEXT_UI */
