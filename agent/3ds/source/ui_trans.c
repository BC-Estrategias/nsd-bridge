#ifndef AGENT_TEXT_UI
/* The Translations screens: the catalog list (a star marks what is installed), the actions for one pack, the confirmation,
 * the progress and the result. The work is in translations.c; this only draws and reads buttons. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "translations.h"
#include "ui_internal.h"

enum { TX_LIST, TX_ACTIONS };
enum { ACT_INSTALL, ACT_UNINSTALL };

static int st = TX_LIST, view[512], nview, sel, top, act_sel, act_rows[2], n_act, lang_filter; /* lang_filter: 0 all, else the n-th language */
static char langs[8][12];
static int n_langs;
static unsigned built_rev = ~0u;
static char link_msg[64];

void uitr_set_message(const char *m) { snprintf(link_msg, sizeof link_msg, "%s", m ? m : ""); }

static const char *lang_label(const char *l) {
  if (!strcmp(l, "ptbr")) return "PT-BR";
  if (!strcmp(l, "en")) return "EN";
  if (!strcmp(l, "es")) return "ES";
  return l;
}

static void view_rebuild(void) {
  int i, j, n = tr_count();
  const tr_pack *keep = (sel < nview && nview > 0) ? tr_at(view[sel]) : NULL;
  char keep_id[17] = "", keep_team[64] = "";
  if (keep) { snprintf(keep_id, sizeof keep_id, "%s", keep->ref.id); snprintf(keep_team, sizeof keep_team, "%s", keep->ref.team); }
  n_langs = 0;
  for (i = 0; i < n; i++) {
    const tr_pack *p = tr_at(i);
    bool seen = false;
    for (j = 0; j < n_langs; j++) if (!strcmp(langs[j], p->ref.lang)) seen = true;
    if (!seen && n_langs < 8) snprintf(langs[n_langs++], sizeof langs[0], "%s", p->ref.lang);
  }
  if (lang_filter > n_langs) lang_filter = 0;
  nview = 0;
  for (i = 0; i < n && nview < 512; i++) {
    const tr_pack *p = tr_at(i);
    /* the catalog is sorted newest version first inside (ID, team): only that one is a row */
    if (i > 0) {
      const tr_pack *q = tr_at(i - 1);
      if (!strcmp(p->ref.lang, q->ref.lang) && !strcmp(p->ref.id, q->ref.id) && !strcmp(p->ref.team, q->ref.team)) continue;
    }
    if (lang_filter > 0 && strcmp(p->ref.lang, langs[lang_filter - 1]) != 0) continue;
    view[nview++] = i;
  }
  if (keep) for (i = 0; i < nview; i++) { const tr_pack *p = tr_at(view[i]); if (!strcmp(p->ref.id, keep_id) && !strcmp(p->ref.team, keep_team)) { sel = i; break; } }
  if (sel >= nview) sel = nview ? nview - 1 : 0;
  if (sel < top) top = sel;
  if (sel >= top + UI_LIST_ROWS) top = sel - UI_LIST_ROWS + 1;
  if (top < 0) top = 0;
}

void uitr_open(void) {
  st = TX_LIST;
  sel = top = 0;
  nview = 0;
  built_rev = ~0u;
  link_msg[0] = '\0';
  if (tr_configured()) tr_list(false);
}

static const tr_pack *cur(void) { return nview ? tr_at(view[sel]) : NULL; }

bool uitr_flow_active(void) {
  tr_status s;
  tr_get(&s);
  return s.state == TRS_PREPARING || s.state == TRS_CONFIRM || s.state == TRS_WORKING || s.state == TRS_DONE || s.state == TRS_ERROR;
}

static bool tap_in(const touchPosition *t, int x0, int y0, int x1, int y1) { return t->px >= x0 && t->px < x1 && t->py >= y0 && t->py < y1; }

/* ------------------------------------------------------------------------------------------------ input */
ui_action uitr_handle(u32 down, u32 held, const touchPosition *touch, bool *redraw, bool *leave) {
  tr_status s;
  int step, max;
  *leave = false;
  tr_get(&s);
  if (s.revision != built_rev && (s.state == TRS_READY || s.state == TRS_IDLE)) { built_rev = s.revision; tr_refresh_marks(); view_rebuild(); *redraw = true; }
  /* the flow (prepare / confirm / progress / result) takes both screens over */
  if (s.state == TRS_PREPARING || s.state == TRS_LISTING) {
    if (down & KEY_B) { tr_cancel(); *redraw = true; }
    *redraw = true;
    return UI_NONE;
  }
  if (s.state == TRS_CONFIRM) {
    if (down & KEY_A) { tr_confirm(true); *redraw = true; }
    else if (down & KEY_B) { tr_confirm(false); *redraw = true; }
    else if ((down & KEY_TOUCH) && tap_in(touch, 10, 145, 155, 195)) { tr_confirm(true); *redraw = true; }
    else if ((down & KEY_TOUCH) && tap_in(touch, 165, 145, 310, 195)) { tr_confirm(false); *redraw = true; }
    return UI_NONE;
  }
  if (s.state == TRS_WORKING) {
    if ((down & KEY_B) || ((down & KEY_TOUCH) && tap_in(touch, 60, 195, 260, 235))) { tr_cancel(); *redraw = true; }
    *redraw = true;
    return UI_NONE;
  }
  if (s.state == TRS_DONE || s.state == TRS_ERROR) {
    if ((down & (KEY_A | KEY_B)) || ((down & KEY_TOUCH) && tap_in(touch, 0, 160, 320, 240))) { tr_dismiss(); *redraw = true; }
    return UI_NONE;
  }
  /* not configured: A asks for the link */
  if (!tr_configured()) {
    if (down & KEY_A) { *redraw = true; return UI_TR_LINK; }
    if ((down & (KEY_B | KEY_START)) || ((down & KEY_TOUCH) && tap_in(touch, 0, 20, 320, 40))) { *leave = true; *redraw = true; }
    return UI_NONE;
  }
  if (st == TX_ACTIONS) {
    if ((down & KEY_B) || ((down & KEY_TOUCH) && tap_in(touch, 0, 20, 320, 40))) { st = TX_LIST; *redraw = true; return UI_NONE; }
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (n_act) act_sel = (act_sel + 1) % n_act; *redraw = true; }
    else if (down & (KEY_DUP | KEY_CPAD_UP)) { if (n_act) act_sel = (act_sel + n_act - 1) % n_act; *redraw = true; }
    else if ((down & KEY_A) || ((down & KEY_TOUCH) && touch->py >= UI_LIST_Y && ui_row_at(touch->py) == act_sel)) {
      if (n_act && cur()) { tr_prepare(view[sel], act_rows[act_sel] == ACT_UNINSTALL); st = TX_LIST; }
      *redraw = true;
    } else if ((down & KEY_TOUCH) && touch->py >= UI_LIST_Y) {
      int r = ui_row_at(touch->py);
      if (r >= 0 && r < n_act) { act_sel = r; *redraw = true; }
    }
    return UI_NONE;
  }
  /* the list */
  if ((down & (KEY_B | KEY_START)) || ((down & KEY_TOUCH) && tap_in(touch, 0, 20, 320, 40))) { *leave = true; *redraw = true; return UI_NONE; }
  if (down & KEY_X) { lang_filter = (lang_filter + 1) % (n_langs + 1); sel = top = 0; view_rebuild(); *redraw = true; return UI_NONE; }
  if (down & KEY_Y) { tr_list(true); *redraw = true; return UI_NONE; }
  if (down & KEY_SELECT) { *redraw = true; return UI_TR_LINK; }
  max = nview > UI_LIST_ROWS ? nview - UI_LIST_ROWS : 0;
  step = ui_drag_rows(down, held, touch);
  if (step) { top += step; if (top < 0) top = 0; if (top > max) top = max; if (sel < top) sel = top; if (sel >= top + UI_LIST_ROWS) sel = top + UI_LIST_ROWS - 1; *redraw = true; }
  if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (nview) sel = (sel + 1) % nview; *redraw = true; }
  else if (down & (KEY_DUP | KEY_CPAD_UP)) { if (nview) sel = (sel + nview - 1) % nview; *redraw = true; }
  else if (down & KEY_R) { sel = sel + 8 < nview ? sel + 8 : nview ? nview - 1 : 0; *redraw = true; }
  else if (down & KEY_L) { sel = sel > 8 ? sel - 8 : 0; *redraw = true; }
  else if ((down & KEY_TOUCH) && touch->py >= UI_LIST_Y) {
    int r = ui_row_at(touch->py);
    if (r >= 0 && top + r < nview) {
      *redraw = true;
      if (top + r == sel) down |= KEY_A;
      else sel = top + r;
    }
  }
  if (sel < top) top = sel;
  if (sel >= top + UI_LIST_ROWS) top = sel - UI_LIST_ROWS + 1;
  if ((down & KEY_A) && nview) {
    const tr_pack *p = cur();
    n_act = 0;
    act_sel = 0;
    act_rows[n_act++] = ACT_INSTALL;
    if (p && p->mark > 0) act_rows[n_act++] = ACT_UNINSTALL;
    st = TX_ACTIONS;
    *redraw = true;
  }
  return UI_NONE;
}

/* ------------------------------------------------------------------------------------------------ drawing */
static void fmt_size(char *out, size_t cap, uint64_t b) {
  if (b >= 1024ull * 1024ull * 1024ull) snprintf(out, cap, "%.2f GiB", (double)b / (1024.0 * 1024.0 * 1024.0));
  else if (b >= 1024ull * 1024ull) snprintf(out, cap, "%.1f MiB", (double)b / (1024.0 * 1024.0));
  else snprintf(out, cap, "%llu KiB", (unsigned long long)((b + 1023) / 1024));
}

static void top_header(const ui_model *m) {
  ui_scene_top(COL_TOP_BG);
  ui_rect(0, 0, 400, 24, COL_TOP_HDR);
  ui_text(8, 3, 0.6f, COL_CYAN, 0, "NSD Bridge");
  ui_text(392, 5, 0.45f, m->build_tag[0] ? COL_YELLOW : COL_DIM, UI_RIGHT, "%s  v%s%s%s", UI_T("Translations", "Traduções"), m->version, m->build_tag[0] ? " " : "", m->build_tag);
}

static void button(float x, float y, uint32_t col, const char *label) {
  ui_rect(x, y, 145, 50, col);
  ui_text(x + 72, y + 14, 0.55f, COL_WHITE, UI_CENTER, "%s", label);
}

static const char *mark_text(const tr_pack *p) {
  return p->update ? "\xE2\x86\x91" : p->mark == 2 ? "\xE2\x98\x85" : p->mark == 1 ? "\xE2\x98\x86" : " "; /* up arrow, filled star, empty star */
}

static void draw_pack_details(const tr_pack *p) {
  float y = 36;
  ui_text(12, y, 0.55f, COL_WHITE, 0, "%.38s", p->ref.game);
  y += 24;
  ui_text(12, y, 0.42f, COL_WHITE, 0, "ID  %s", p->ref.id);
  y += 16;
  ui_text(12, y, 0.42f, COL_WHITE, 0, "%s  %s    %s  %s", UI_T("Language", "Idioma"), lang_label(p->ref.lang), UI_T("Version", "Versão"), p->ref.version);
  y += 16;
  ui_text(12, y, 0.42f, COL_WHITE, 0, "%s  %.40s", UI_T("Team", "Equipe"), p->ref.team);
  y += 22;
  if (p->update) ui_text(12, y, 0.45f, COL_YELLOW, 0, "%s %s -> %s", UI_T("Update:", "Atualização:"), p->inst_version, p->ref.version);
  else if (p->mark == 2) ui_text(12, y, 0.45f, COL_GREEN, 0, "%s %s", UI_T("Installed:", "Instalada:"), p->inst_version);
  else if (p->mark == 1) ui_text(12, y, 0.45f, COL_CYAN, 0, "%s", UI_T("A folder for this game is already there (by hand).", "Já existe uma pasta deste jogo (feita à mão)."));
  else ui_text(12, y, 0.45f, COL_DIM, 0, "%s", UI_T("Not installed.", "Não instalada."));
}

static const char *err_text(const tr_status *s) {
  static char buf[140];
  switch (s->error) {
    case TRE_NET:
      if (s->http_code == 401 || s->http_code == 403) snprintf(buf, sizeof buf, "%s (HTTP %ld)", UI_T("The share refused access", "O link recusou o acesso"), s->http_code);
      else if (s->http_code == 404) snprintf(buf, sizeof buf, "%s", UI_T("Not found on the share (404).", "Não encontrado no link (404)."));
      else snprintf(buf, sizeof buf, "%s (curl %d)", UI_T("Could not reach the share", "Não conseguiu acessar o link"), s->net_code);
      return buf;
    case TRE_NOFILES: return UI_T("This pack has no files in arquivos/.", "Este pacote não tem arquivos em arquivos/.");
    case TRE_BADPATH: snprintf(buf, sizeof buf, "%s %.60s", UI_T("Refused an unsafe path:", "Recusou um caminho inseguro:"), s->detail); return buf;
    case TRE_TOOBIG: return UI_T("Too many files or too big (limit 20000 / 1.5 GiB).", "Arquivos demais ou grande demais (limite 20000 / 1,5 GiB).");
    case TRE_SPACE: return UI_T("Not enough space on the SD card.", "Sem espaço suficiente no cartão SD.");
    case TRE_IO: snprintf(buf, sizeof buf, "%s %.60s", UI_T("Could not write:", "Não consegui gravar:"), s->detail); return buf;
    case TRE_CANCELLED: return UI_T("Cancelled.", "Cancelado.");
    case TRE_SIZE: snprintf(buf, sizeof buf, "%s %.60s", UI_T("Wrong size:", "Tamanho errado:"), s->detail); return buf;
    default: return "";
  }
}

void uitr_draw(const ui_model *m) {
  tr_status s;
  char val[48], line[120], sz[24];
  int i;
  tr_get(&s);
  top_header(m);
  /* ---- flow ---- */
  if (s.state == TRS_PREPARING || s.state == TRS_LISTING) {
    ui_text(200, 110, 0.6f, COL_YELLOW, UI_CENTER, "%s", s.state == TRS_LISTING ? UI_T("Reading the catalog...", "Lendo o catálogo...") : UI_T("Checking the pack...", "Verificando o pacote..."));
    if (s.state == TRS_LISTING) ui_text(200, 140, 0.45f, COL_WHITE, UI_CENTER, "%d %s", s.list_requests, UI_T("requests", "pedidos"));
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Translations", "Traduções"), UI_T("Please wait", "Aguarde"), UI_T("B: Cancel", "B: Cancelar"));
    return;
  }
  if (s.state == TRS_CONFIRM || s.state == TRS_WORKING || s.state == TRS_DONE || (s.state == TRS_ERROR && s.pack >= 0)) {
    const tr_pack *p = tr_at(s.pack);
    bool un = s.job == 1;
    if (p) {
      float y = 36;
      ui_text(12, y, 0.5f, COL_WHITE, 0, "%.40s", p->ref.game);
      y += 20;
      ui_text(12, y, 0.4f, COL_DIM, 0, "%s  %s  %s", p->ref.id, lang_label(p->ref.lang), p->ref.version);
      y += 18;
      if (s.state == TRS_CONFIRM) {
        fmt_size(sz, sizeof sz, s.bytes_total);
        if (!un) ui_text(12, y, 0.45f, COL_WHITE, 0, "%u %s, %s", s.files_total, UI_T("files", "arquivos"), sz);
        else ui_text(12, y, 0.45f, COL_WHITE, 0, "%u %s", s.files_total, UI_T("files to remove", "arquivos a remover"));
        y += 18;
        ui_text(12, y, 0.4f, COL_DIM, 0, "%s: %.30s", UI_T("Team", "Equipe"), s.team[0] ? s.team : p->ref.team);
        y += 15;
        if (s.region[0]) { ui_text(12, y, 0.4f, COL_DIM, 0, "%s: %.20s", UI_T("Region", "Região"), s.region); y += 15; }
        if (s.source[0]) { ui_text(12, y, 0.36f, COL_DIM, 0, "%.62s", s.source); y += 15; }
        if (!un && s.title_known == 2) { ui_text(12, y, 0.4f, COL_YELLOW, 0, "%s", UI_T("This game is not installed on this console.", "Este jogo não está instalado neste console.")); y += 15; }
        if (!un) { ui_text(12, y, 0.38f, COL_CYAN, 0, "%s", UI_T("Luma: turn on \"Enable game patching\" (SELECT at boot).", "Luma: ligue \"Enable game patching\" (SELECT ao ligar).")); y += 15; }
        if (un && !s.has_record) ui_text(12, y, 0.38f, COL_YELLOW, 0, "%s", UI_T("Made by hand: only files that are in the pack are removed.", "Feita à mão: só os arquivos que estão no pacote são removidos."));
      } else if (s.state == TRS_WORKING) {
        float frac = (!un && s.bytes_total) ? (float)((double)s.bytes_done / (double)s.bytes_total) : (s.files_total ? (float)s.files_done / (float)s.files_total : 0.0f);
        if (frac > 1.0f) frac = 1.0f;
        ui_rect(30, 150, 340, 14, COL_BAR);
        ui_rect(30, 150, 340.0f * frac, 14, COL_GREEN);
        if (un) snprintf(line, sizeof line, "%u / %u %s", s.files_done, s.files_total, UI_T("files", "arquivos"));
        else {
          char d[24];
          fmt_size(d, sizeof d, s.bytes_done);
          fmt_size(sz, sizeof sz, s.bytes_total);
          snprintf(line, sizeof line, "%u%%   %s / %s   %.2f MiB/s", (unsigned)(frac * 100.0f), d, sz, (double)s.bps / 1048576.0);
        }
        ui_text(200, 168, 0.42f, COL_WHITE, UI_CENTER, "%s", line);
        if (!un) {
          ui_text(200, 184, 0.4f, COL_DIM, UI_CENTER, "%u / %u %s", s.files_done, s.files_total, UI_T("files", "arquivos"));
          if (s.eta_s > 0) ui_text(200, 200, 0.42f, COL_CYAN, UI_CENTER, "%s ~%u:%02u", UI_T("Time left", "Tempo restante"), s.eta_s / 60, s.eta_s % 60);
        }
      } else if (s.state == TRS_DONE) {
        ui_text(200, 130, 0.7f, COL_GREEN, UI_CENTER, "%s", un ? UI_T("Removed", "Removida") : UI_T("Installed", "Instalada"));
        if (un) ui_text(200, 156, 0.42f, COL_WHITE, UI_CENTER, "%u %s", s.removed, UI_T("files removed", "arquivos removidos"));
        else ui_text(200, 156, 0.4f, COL_CYAN, UI_CENTER, "%s", UI_T("Open the game with \"Enable game patching\" on in Luma.", "Abra o jogo com \"Enable game patching\" ligado no Luma."));
      }
    }
    if (s.state == TRS_ERROR) ui_text(200, 150, 0.45f, COL_RED, UI_CENTER, "%.70s", err_text(&s));
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Translations", "Traduções"),
                     s.state == TRS_CONFIRM ? (un ? UI_T("Remove this translation?", "Remover esta tradução?") : UI_T("Install this translation?", "Instalar esta tradução?")) :
                     s.state == TRS_WORKING ? (un ? UI_T("Removing", "Removendo") : UI_T("Installing", "Instalando")) : s.state == TRS_DONE ? UI_T("Done", "Concluído") : UI_T("Failed", "Falhou"),
                     s.state == TRS_CONFIRM ? UI_T("A: Yes   B: Cancel", "A: Sim   B: Cancelar") : s.state == TRS_WORKING ? UI_T("B: Cancel", "B: Cancelar") : "A / B: OK");
    if (s.state == TRS_CONFIRM) {
      button(10, 145, un ? UI_RGB(0xe5, 0x39, 0x35) : UI_RGB(0x4c, 0xaf, 0x50), un ? UI_T("A: Remove", "A: Remover") : UI_T("A: Install", "A: Instalar"));
      button(165, 145, UI_RGB(0x42, 0x7a, 0xb8), UI_T("B: Cancel", "B: Cancelar"));
    } else if (s.state == TRS_WORKING) {
      button(60, 185, UI_RGB(0xe5, 0x39, 0x35), UI_T("B: Cancel", "B: Cancelar"));
    } else {
      button(60, 185, UI_RGB(0x42, 0x7a, 0xb8), "OK");
    }
    return;
  }
  if (s.state == TRS_ERROR) { /* a listing failure */
    ui_text(200, 110, 0.5f, COL_RED, UI_CENTER, "%.70s", err_text(&s));
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Translations", "Traduções"), UI_T("Failed", "Falhou"), "A / B: OK");
    button(60, 185, UI_RGB(0x42, 0x7a, 0xb8), "OK");
    return;
  }
  /* ---- not configured ---- */
  if (!tr_configured()) {
    ui_text(12, 40, 0.5f, COL_WHITE, 0, "%s", UI_T("No translation source yet.", "Ainda sem fonte de traduções."));
    ui_text(12, 66, 0.4f, COL_DIM, 0, "%s", UI_T("Paste the public link of your Nextcloud folder", "Cole o link público da sua pasta do Nextcloud"));
    ui_text(12, 82, 0.4f, COL_DIM, 0, "%s", UI_T("(https://.../s/xxxxxxx) with a traducoes/ folder.", "(https://.../s/xxxxxxx) com uma pasta traducoes/."));
    ui_scene_bottom(COL_BOT_BG);
    ui_bottom_chrome(UI_T("Translations", "Traduções"), UI_T("< Back  (B)", "< Voltar  (B)"), UI_T("A: Type the link", "A: Digitar o link"));
    button(85, 90, UI_RGB(0x4c, 0xaf, 0x50), UI_T("A: Link", "A: Link"));
    if (link_msg[0]) ui_text(160, 160, 0.42f, COL_INK_RED, UI_CENTER, "%s", link_msg);
    return;
  }
  /* ---- the list ---- */
  {
    const tr_pack *p = cur();
    if (p) draw_pack_details(p);
    else ui_text(200, 110, 0.5f, COL_DIM, UI_CENTER, "%s", UI_T("No translations here (Y: refresh).", "Nenhuma tradução aqui (Y: atualizar)."));
    ui_scene_bottom(COL_BOT_BG);
    if (st == TX_ACTIONS && p) {
      ui_bottom_chrome(UI_T("Translations", "Traduções"), p->ref.game, UI_T("A: Select   B: Back", "A: Selecionar   B: Voltar"));
      for (i = 0; i < n_act; i++) {
        const char *lab = act_rows[i] == ACT_UNINSTALL ? UI_T("Uninstall", "Desinstalar") : p->update ? UI_T("Update", "Atualizar") : p->mark == 2 ? UI_T("Reinstall", "Reinstalar") : UI_T("Install", "Instalar");
        ui_list_row(i, act_sel, 0, lab, "", act_rows[i] == ACT_UNINSTALL ? COL_INK_RED : COL_INK);
      }
      return;
    }
    snprintf(line, sizeof line, "< %s (%d)", lang_filter == 0 ? UI_T("All languages", "Todos os idiomas") : lang_label(langs[lang_filter - 1]), nview);
    ui_bottom_chrome(UI_T("Translations", "Traduções"), line, UI_T("A:Open X:Lang Y:Refresh SEL:Link", "A:Abrir X:Idioma Y:Atualizar SEL:Link"));
    for (i = top; i < nview && i < top + UI_LIST_ROWS; i++) {
      const tr_pack *q = tr_at(view[i]);
      char nm[140];
      snprintf(nm, sizeof nm, "%s %s", mark_text(q), q->ref.game);
      snprintf(val, sizeof val, "%.14s %s", q->ref.team, q->ref.version);
      ui_list_row(i, sel, top, nm, val, q->mark == 0 ? COL_INK : COL_INK_GREEN);
    }
  }
}
#endif /* !AGENT_TEXT_UI */
