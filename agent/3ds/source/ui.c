#ifndef AGENT_TEXT_UI
#include "qr.h"
#include "ui_internal.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alog.h"


static C3D_RenderTarget *g_top, *g_bot;
static C2D_TextBuf g_tb;
static bool g_ready, g_pt;

enum { SCR_MAIN, SCR_MCP, SCR_DEVICES, SCR_LOG, SCR_INST, SCR_SD, SCR_RECV, SCR_QR, SCR_TITLES, SCR_TRANSL, SCR_SETTINGS, SCR_COUNT };
static int g_screen = SCR_MAIN, g_log_off = 0;
static int g_sel[SCR_COUNT] = {1, 0, 0, 0, 0, 0, 0, 0}; /* the main menu starts on the row after Updates: a stray A never starts an install */
static int g_arg;
static char g_arg_str[520];
const char *ui_arg_str(void) { return g_arg_str; }
void ui_show_installer_menu(void) { g_screen = SCR_INST; }
void ui_set_arg_str(const char *s) { snprintf(g_arg_str, sizeof g_arg_str, "%s", s); }
static int cur_screen(void) { return (unsigned)g_screen < SCR_COUNT ? g_screen : SCR_MAIN; }

void ui_init(void) {
  u8 lang = 0;
  if (R_SUCCEEDED(cfguInit())) {
    if (R_SUCCEEDED(CFGU_GetSystemLanguage(&lang))) g_pt = (lang == CFG_LANGUAGE_PT);
    cfguExit();
  }
  C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
  C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
  C2D_Prepare();
  g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
  g_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
  g_tb = C2D_TextBufNew(8192);
  g_ready = true;
}

void ui_exit(void) {
  if (!g_ready) return;
  uit_shutdown();
  C2D_TextBufDelete(g_tb);
  C2D_Fini();
  C3D_Fini();
  g_ready = false;
}

bool ui_pt(void) { return g_pt; }

void ui_begin(void) {
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
  C2D_TextBufClear(g_tb);
}
void ui_end(void) { C3D_FrameEnd(0); }
void ui_scene_top(uint32_t bg) {
  C2D_TargetClear(g_top, bg);
  C2D_SceneBegin(g_top);
}
void ui_scene_bottom(uint32_t bg) {
  C2D_TargetClear(g_bot, bg);
  C2D_SceneBegin(g_bot);
}
void ui_battery(float x, float y, int percent, bool charging, bool adapter) {
  /* green while charging; when the charger is in but it is not charging (full), say so instead of showing nothing */
  const char *state = charging ? UI_T("charging", "carregando") : adapter ? UI_T("plugged in", "na tomada") : "";
  uint32_t col = charging || adapter ? UI_RGB(0x6b, 0xd0, 0x6b) : percent < 15 ? UI_RGB(0xf0, 0x5a, 0x50) : percent < 30 ? UI_RGB(0xf0, 0xc8, 0x3c) : UI_RGB(0xb8, 0xc0, 0xcc);
  if (percent < 0) return;
  ui_text(x, y, 0.45f, col, UI_RIGHT, "%s %d%%%s%s", UI_T("Battery", "Bateria"), percent, state[0] ? "  " : "", state);
}

void ui_rect(float x, float y, float w, float h, uint32_t color) { C2D_DrawRectSolid(x, y, 0.5f, w, h, color); }

void ui_text(float x, float y, float scale, uint32_t color, int flags, const char *fmt, ...) {
  char buf[200];
  C2D_Text t;
  va_list ap;
  u32 f = C2D_WithColor;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  C2D_TextParse(&t, g_tb, buf);
  C2D_TextOptimize(&t);
  if (flags == UI_RIGHT) f |= C2D_AlignRight;
  else if (flags == UI_CENTER) f |= C2D_AlignCenter;
  C2D_DrawText(&t, f, x, y, 0.5f, scale, scale, color);
}

void ui_bottom_chrome(const char *info, const char *title, const char *hint) {
  ui_rect(0, 0, 320, 20, COL_BAR);
  ui_text(6, 2, 0.45f, COL_WHITE, 0, "%s", info);
  ui_rect(0, 20, 320, 20, COL_TITLE);
  ui_text(6, 22, 0.45f, COL_INK, 0, "%s", title);
  ui_rect(0, 220, 320, 20, COL_BAR);
  ui_text(6, 222, 0.45f, COL_WHITE, 0, "%s", hint);
}

int ui_row_at(int y) {
  int r;
  if (y < UI_LIST_Y) return -1;
  r = (y - UI_LIST_Y) / UI_ROW_H;
  return r < UI_LIST_ROWS ? r : -1;
}

int ui_drag_rows(u32 down, u32 held, const touchPosition *touch) {
  static int last_y = -1, acc = 0;
  if (down & KEY_TOUCH) { last_y = touch->py; acc = 0; return 0; }
  if (!(held & KEY_TOUCH)) { last_y = -1; return 0; }
  if (last_y < 0) return 0;
  acc += last_y - touch->py; /* finger up -> content goes towards the end */
  last_y = touch->py;
  if (acc >= UI_ROW_H) { acc -= UI_ROW_H; return 1; }
  if (acc <= -UI_ROW_H) { acc += UI_ROW_H; return -1; }
  return 0;
}

/* ---- the screens: main menu -> Pairing (pair, paired devices, web page) -> paired devices (B goes back one level) ---- */
/* the main menu: START leaves the app, so there is no "Exit" row; the rarely used rows live under Settings */
enum { IT_UPDATE, IT_SETTINGS, IT_TITLES, IT_TRANSL, IT_INSTALLER, IT_COUNT };
enum { SS_FOLDERS, SS_MODE, SS_PAIRING, SS_LOG, SS_COUNT };
enum { SP_PAIR, SP_DEVICES, SP_WEB, SP_COUNT };
enum { SI_SD, SI_URL, SI_RECV, SI_QR, SI_PENDING, SI_SEEDS, SI_COUNT };

int ui_arg(void) { return g_arg; }

static const char *item_name(int i) {
  switch (i) {
    case IT_UPDATE: return UI_T("Updates", "Atualizações");
    case IT_SETTINGS: return UI_T("Settings", "Configurações");
    case IT_TITLES: return UI_T("Titles", "Títulos");
    case IT_TRANSL: return UI_T("Translations", "Traduções");
    default: return UI_T("Installer", "Instalador");
  }
}

static const char *settings_name(int i) {
  switch (i) {
    case SS_FOLDERS: return UI_T("Open folders", "Pastas liberadas");
    case SS_MODE: return UI_T("Mode", "Modo");
    case SS_PAIRING: return UI_T("Pairing", "Pareamento");
    default: return UI_T("Activity log", "Log de atividade");
  }
}

static int log_count(void) {
  int n = 0;
  while (alog_get(n)) n++;
  return n;
}

/* rows on the current screen */
static int rows_here(const ui_model *m) {
  switch (g_screen) {
    case SCR_MAIN: return IT_COUNT;
    case SCR_SETTINGS: return SS_COUNT;
    case SCR_MCP: return SP_COUNT;
    case SCR_INST: return m->seeds_dir ? SI_COUNT : SI_COUNT - 1;
    case SCR_DEVICES: return m->paired_n + (m->paired_n > 0 ? 1 : 0); /* + "forget all" */
    default: return 0;
  }
}

static void go(int screen) { g_screen = screen; }

/* Runs the highlighted row of the current screen. */
static ui_action activate(const ui_model *m) {
  int sel = g_sel[cur_screen()];
  if (g_screen == SCR_MAIN) {
    switch (sel) {
      case IT_UPDATE: return UI_UPDATE;
      case IT_SETTINGS: g_sel[SCR_SETTINGS] = 0; go(SCR_SETTINGS); return UI_NONE;
      case IT_INSTALLER: go(SCR_INST); return UI_NONE;
      case IT_TITLES: uit_open(); go(SCR_TITLES); return UI_NONE;
      case IT_TRANSL: uitr_open(); go(SCR_TRANSL); return UI_NONE;
      default: break;
    }
    return UI_NONE;
  }
  if (g_screen == SCR_SETTINGS) {
    switch (sel) {
      case SS_FOLDERS: return UI_FOLDERS;
      case SS_MODE: return UI_MODE;
      case SS_PAIRING: go(SCR_MCP); return UI_NONE;
      default: g_log_off = 0; go(SCR_LOG); return UI_NONE;
    }
  }
  if (g_screen == SCR_MCP) {
    if (sel == SP_PAIR) return UI_PAIR;
    if (sel == SP_WEB) return UI_WEB;
    g_sel[SCR_DEVICES] = 0;
    go(SCR_DEVICES);
    return UI_NONE;
  }
  if (g_screen == SCR_INST) {
    switch (sel) {
      case SI_SD: uii_sd_open(); go(SCR_SD); return UI_NONE;
      case SI_URL: return UI_INST_TYPE_URL;
      case SI_RECV: go(SCR_RECV); return UI_INST_RECV_ON;
      case SI_QR: go(SCR_QR); return UI_INST_QR_ON;
      case SI_PENDING: return UI_INST_CLEAN;
      default: return UI_INST_SEEDS;
    }
  }
  if (g_screen == SCR_DEVICES) {
    if (sel < m->paired_n) { g_arg = sel; if (g_sel[SCR_DEVICES] > 0 && g_sel[SCR_DEVICES] >= m->paired_n - 1) g_sel[SCR_DEVICES]--; return UI_UNPAIR_ONE; }
    return UI_FORGET; /* the "forget all" row: first press arms it, the second one does it */
  }
  return UI_NONE;
}

ui_action ui_handle(u32 down, u32 held, const touchPosition *touch, const ui_model *m, bool *redraw) {
  /* a pairing request owns A/B (and the two touch buttons) on every screen until it is answered */
  if (m->pairing == 2) {
    if (down & KEY_A) { *redraw = true; return UI_PAIR_YES; }
    if (down & KEY_B) { *redraw = true; return UI_PAIR_NO; }
    if ((down & KEY_TOUCH) && touch->py >= 150 && touch->py < 200) {
      *redraw = true;
      return touch->px < 160 ? UI_PAIR_YES : UI_PAIR_NO;
    }
    return UI_NONE;
  }
  if (uii_flow_active(m)) return uii_flow_handle(down, touch, m, redraw);
  if (g_screen == SCR_TRANSL) {
    bool leave = false;
    ui_action a = uitr_handle(down, held, touch, redraw, &leave);
    if (leave) { go(SCR_MAIN); *redraw = true; }
    return a;
  }
  if (g_screen == SCR_TITLES) {
    bool leave = false;
    uit_handle(down, held, touch, redraw, &leave);
    if (leave) { go(SCR_MAIN); *redraw = true; }
    return UI_NONE;
  }
  if (g_screen == SCR_SD) {
    bool leave = false;
    ui_action a = uii_sd_handle(down, held, touch, redraw, &leave);
    if (leave) { go(SCR_INST); *redraw = true; }
    return a;
  }
  if (g_screen == SCR_RECV || g_screen == SCR_QR) {
    if (g_screen == SCR_QR && (down & KEY_X)) { *redraw = true; return UI_INST_QR_SWITCH; }
    if ((down & (KEY_B | KEY_START)) || ((down & KEY_TOUCH) && touch->py >= 20 && touch->py < 40)) {
      ui_action off = g_screen == SCR_RECV ? UI_INST_RECV_OFF : UI_INST_QR_OFF;
      go(SCR_INST);
      *redraw = true;
      return off;
    }
    return UI_NONE;
  }
  if (g_screen == SCR_LOG) {
    int n = log_count(), step = ui_drag_rows(down, held, touch);
    int max = n > UI_LIST_ROWS * 2 ? n - UI_LIST_ROWS * 2 : 0;
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) step = 1;
    if (down & (KEY_DUP | KEY_CPAD_UP)) step = -1;
    if (down & KEY_R) step = 6;
    if (down & KEY_L) step = -6;
    if (step) {
      g_log_off += step;
      if (g_log_off < 0) g_log_off = 0;
      if (g_log_off > max) g_log_off = max;
      *redraw = true;
    }
  } else {
    int n = rows_here(m), sel = g_sel[cur_screen()];
    if (g_screen == SCR_MAIN) {
      /* the old physical shortcuts, main menu only */
      if (down & KEY_START) return UI_EXIT;
      if (down & KEY_X) return UI_MODE;
      if (down & KEY_Y) return UI_PAIR;
      if (down & KEY_R) return UI_WEB;
      if (down & KEY_SELECT) return UI_FORGET;
    }
    if (n > 0 && sel >= n) { g_sel[cur_screen()] = sel = n - 1; *redraw = true; }
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (n) g_sel[cur_screen()] = (sel + 1) % n; *redraw = true; }
    else if (down & (KEY_DUP | KEY_CPAD_UP)) { if (n) g_sel[cur_screen()] = (sel + n - 1) % n; *redraw = true; }
    else if ((down & KEY_A) && n > 0) { *redraw = true; return activate(m); }
    else if ((down & KEY_TOUCH) && touch->py >= UI_LIST_Y) {
      int r = ui_row_at(touch->py), top = (g_screen == SCR_DEVICES || g_screen == SCR_MAIN) && sel >= UI_LIST_ROWS ? sel - UI_LIST_ROWS + 1 : 0;
      r = r < 0 ? -1 : r + top;
      if (r >= 0 && r < n) {
        /* the first tap selects, a tap on the selected row runs it (a stray touch never exits, changes the mode or unpairs) */
        *redraw = true;
        if (r == sel) return activate(m);
        g_sel[cur_screen()] = r;
      }
    }
  }
  /* B (and START on the sub-screens) goes back one level; a tap on the title bar does too */
  if ((down & KEY_B) || (g_screen != SCR_MAIN && (down & KEY_START)) ||
      ((down & KEY_TOUCH) && touch->py >= 20 && touch->py < 40 && g_screen != SCR_MAIN)) {
    if (g_screen == SCR_DEVICES) go(SCR_MCP);
    else if (g_screen == SCR_MCP || g_screen == SCR_LOG) go(SCR_SETTINGS);
    else if (g_screen != SCR_MAIN) go(SCR_MAIN); /* (SCR_INST -> main; SD/RECV/QR have their own handling above) */
    else return UI_NONE;
    *redraw = true;
  }
  return UI_NONE;
}

static void draw_top(const ui_model *m) {
  static const char *const net_en[] = {"STARTING NETWORK", "WAITING FOR Wi-Fi", "NETWORK ERROR", "ONLINE"};
  static const char *const net_pt[] = {"INICIANDO REDE", "AGUARDANDO Wi-Fi", "ERRO DE REDE", "ONLINE"};
  const uint32_t net_col[] = {COL_YELLOW, COL_YELLOW, COL_RED, COL_GREEN};
  ui_scene_top(COL_TOP_BG);
  ui_rect(0, 0, 400, 24, COL_TOP_HDR);
  ui_text(8, 3, 0.6f, COL_CYAN, 0, "NSD Bridge");
  ui_text(392, 5, 0.45f, m->build_tag[0] ? COL_YELLOW : COL_DIM, UI_RIGHT, "v%s%s%s  -  BC Labs", m->version, m->build_tag[0] ? " " : "", m->build_tag);
  ui_battery(392, 28, m->battery, m->charging, m->adapter);

  ui_text(12, 32, 0.7f, net_col[m->net], 0, "%s", ui_pt() ? net_pt[m->net] : net_en[m->net]);
  if (m->net == 3) {
    ui_text(12, 56, 0.45f, COL_WHITE, 0, "IP  %s:%u", m->ip, m->port);
    if (m->web_up) {
      ui_text(12, 72, 0.45f, COL_GREEN, 0, "http://%s:%u", m->ip, m->web_port);
      ui_text(392, 72, 0.4f, m->web_open ? COL_YELLOW : COL_DIM, UI_RIGHT, "%s", m->web_open ? UI_T("browser: no pairing", "navegador: sem pareamento") : UI_T("browser: pairing required", "navegador: exige pareamento"));
    }
    else ui_text(12, 72, 0.45f, COL_DIM, 0, "%s", m->web_on ? UI_T("web page starting...", "página web iniciando...") : UI_T("web page off", "página web desligada"));
  } else if (m->err && m->err[0]) {
    ui_text(12, 56, 0.45f, COL_RED, 0, "%.60s", m->err);
  }
  ui_text(12, 92, 0.45f, m->read_only ? COL_GREEN : COL_YELLOW, 0, "%s: %s", UI_T("Mode", "Modo"),
          m->read_only ? UI_T("READ-ONLY", "SOMENTE LEITURA") : UI_T("DEVELOPMENT (writes on)", "DESENVOLVIMENTO (escrita ligada)"));
  if (m->whole_sd) ui_text(12, 108, 0.45f, COL_RED, 0, "%s", UI_T("WHOLE SD CARD OPEN", "CARTÃO SD INTEIRO ABERTO"));
  else ui_text(12, 108, 0.45f, COL_DIM, 0, "%s: %d (%d %s)", UI_T("Open folders", "Pastas"), m->open_folders, m->writable_folders, UI_T("writable", "com escrita"));
  if (m->bridge_peer && m->bridge_peer[0]) ui_text(12, 124, 0.45f, COL_GREEN, 0, "Bridge: %s", m->bridge_peer);
  if (m->web_peer && m->web_peer[0]) ui_text(12, 140, 0.45f, COL_GREEN, 0, "%s: %s", UI_T("Browser", "Navegador"), m->web_peer);

  if (m->pairing == 2) {
    ui_text(200, 150, 0.45f, COL_YELLOW, UI_CENTER, "%s", UI_T("PAIRING REQUEST - same number on the page?", "PEDIDO DE PAREAMENTO - mesmo número na página?"));
    ui_text(200, 164, 1.1f, COL_CYAN, UI_CENTER, "%03u %03u", m->sas / 1000u, m->sas % 1000u);
    ui_text(200, 200, 0.4f, COL_DIM, UI_CENTER, "%.30s  (%.20s)", m->p2_label, m->p2_peer);
  } else if (m->pairing == 1) {
    ui_text(200, 156, 0.5f, COL_YELLOW, UI_CENTER, "%s (%lu s)", UI_T("PAIRING OPEN", "PAREAMENTO ABERTO"), (unsigned long)m->left_s);
    if (m->web_open) {
      /* the browser does not pair: this window is for the CLI and the MCP */
      ui_text(200, 174, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("On the computer run: ndev pair <ip>", "No computador, rode: ndev pair <ip>"));
      ui_text(200, 190, 0.45f, COL_CYAN, UI_CENTER, "%s: %s", UI_T("or ndev pair --code", "ou ndev pair --code"), m->code);
    } else {
      ui_text(200, 174, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("On the page click Pair, or: ndev pair <ip>", "Na página, clique em Parear, ou: ndev pair <ip>"));
      ui_text(200, 190, 0.5f, COL_CYAN, UI_CENTER, "%s: %s", UI_T("code", "código"), m->code);
    }
  } else if (m->forget_armed) {
    ui_text(200, 170, 0.5f, COL_RED, UI_CENTER, "%s", UI_T("Select it again to forget ALL paired devices", "Selecione de novo para esquecer TODOS os dispositivos"));
  }

  if (m->pairing == 0 && !m->forget_armed) {
    if (m->upd == UPD_AVAILABLE) {
      ui_text(200, 160, 0.55f, COL_YELLOW, UI_CENTER, "%s %s", UI_T("Update available:", "Atualização disponível:"), m->upd_version);
      ui_text(200, 180, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("Open \"Updates\" in the menu to install it.", "Abra \"Atualizações\" no menu para instalar."));
    } else if (m->upd == UPD_DOWNLOADING || m->upd == UPD_INSTALLING || m->upd == UPD_VERIFYING) {
      ui_text(200, 160, 0.5f, COL_CYAN, UI_CENTER, "%s %s", m->upd == UPD_INSTALLING ? UI_T("Installing", "Instalando") : UI_T("Downloading", "Baixando"), m->upd_version);
      ui_rect(60, 184, 280, 12, COL_BAR);
      ui_rect(60, 184, 280.0f * (float)m->upd_progress / 100.0f, 12, COL_GREEN);
      ui_text(200, 200, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("Do not turn the console off.", "Não desligue o console."));
    } else if (m->upd == UPD_DONE) {
      ui_text(200, 164, 0.55f, COL_GREEN, UI_CENTER, "%s %s", UI_T("Updated to", "Atualizado para"), m->upd_version);
      ui_text(200, 184, 0.45f, COL_WHITE, UI_CENTER, "%s", UI_T("Press START to close, then open the app again.", "Aperte START para fechar e abra o app de novo."));
    } else if (m->upd == UPD_ERROR) {
      ui_text(200, 168, 0.45f, COL_RED, UI_CENTER, "%s: %.40s", UI_T("Update failed", "Falha na atualização"), m->upd_message);
    }
  }
  ui_rect(0, 220, 400, 20, COL_BAR);
  ui_text(8, 223, 0.4f, COL_WHITE, 0, "%s: %lu    %s: %lu", UI_T("Requests", "Requisições"), m->requests,
          UI_T("Uptime", "Ativo (min)"), m->uptime_s / 60);
  ui_text(392, 223, 0.4f, COL_WHITE, UI_RIGHT, "%.60s", alog_get(0) ? alog_get(0) : "");
  if (m->keys_warn) ui_text(200, 204, 0.45f, COL_RED, UI_CENTER, "%s", UI_T("Pairing not saved (SD error)", "Pareamento não salvo (erro no SD)"));
}

/* One list row: highlight, name on the left, value on the right. */
void ui_list_row(int i, int sel, int top, const char *name, const char *val, uint32_t ink) {
  float y = (float)(UI_LIST_Y + (i - top) * UI_ROW_H);
  if (i == sel) ui_rect(0, y, 320, UI_ROW_H, COL_SEL);
  ui_text(10, y + 3, 0.5f, ink, 0, "%.30s", name);
  if (val && val[0]) ui_text(310, y + 4, 0.45f, ink, UI_RIGHT, "%s", val);
}

static void draw_main(const ui_model *m) {
  int i, main_top;
  char info[80], val[48];
  snprintf(info, sizeof info, "SD: %s %s   %s%s%s", m->sd_free[0] ? m->sd_free : "-", m->sd_free[0] ? UI_T("free", "livres") : "",
           m->net == 3 ? m->ip : "", m->net == 3 ? ":" : "", m->net == 3 ? "6464" : "");
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(info, UI_T("Main menu", "Menu principal"), UI_T("A: Select   X: Mode   Y: Pair   START: Exit", "A: Selecionar  X: Modo  Y: Parear  START: Sair"));
  main_top = g_sel[SCR_MAIN] >= UI_LIST_ROWS ? g_sel[SCR_MAIN] - UI_LIST_ROWS + 1 : 0;
  for (i = main_top; i < IT_COUNT && i < main_top + UI_LIST_ROWS; i++) {
    uint32_t ink = COL_INK;
    val[0] = '\0';
    switch (i) {
      case IT_UPDATE:
        switch (m->upd) {
          case UPD_CHECKING: snprintf(val, sizeof val, "%s", UI_T("checking...", "verificando...")); break;
          case UPD_CURRENT: snprintf(val, sizeof val, "%s", UI_T("up to date", "em dia")); break;
          case UPD_AVAILABLE: snprintf(val, sizeof val, "%s %s", m->upd_version, UI_T("- tap to install", "- instalar")); ink = UI_RGB(0x1b, 0x7a, 0x2b); break;
          case UPD_DOWNLOADING: snprintf(val, sizeof val, "%s %u%%", UI_T("downloading", "baixando"), m->upd_progress); break;
          case UPD_VERIFYING: snprintf(val, sizeof val, "%s", UI_T("verifying...", "conferindo...")); break;
          case UPD_INSTALLING: snprintf(val, sizeof val, "%s %u%%", UI_T("installing", "instalando"), m->upd_progress); break;
          case UPD_DONE: snprintf(val, sizeof val, "%s", UI_T("done - restart", "pronto - reabra")); ink = UI_RGB(0x1b, 0x7a, 0x2b); break;
          case UPD_ERROR: snprintf(val, sizeof val, "%s", UI_T("failed - tap to retry", "falhou - tentar de novo")); ink = COL_INK_RED; break;
          default: snprintf(val, sizeof val, "%s", UI_T("tap to check", "verificar")); break;
        }
        break;
      case IT_SETTINGS: if (m->whole_sd) { snprintf(val, sizeof val, "%s", UI_T("WHOLE SD", "SD INTEIRO")); ink = COL_INK_RED; } else snprintf(val, sizeof val, "%s", m->read_only ? UI_T("Read-only", "Leitura") : UI_T("Development", "Desenvolvimento")); break;
      default: break;
    }
    ui_list_row(i, g_sel[SCR_MAIN], main_top, item_name(i), val, ink);
  }
}

static void draw_settings(const ui_model *m) {
  int i;
  char val[48];
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("Settings", "Configurações"), UI_T("< Main menu  (B: back)", "< Menu principal  (B: voltar)"), UI_T("A: Select   B: Back", "A: Selecionar   B: Voltar"));
  for (i = 0; i < SS_COUNT; i++) {
    uint32_t ink = COL_INK;
    val[0] = '\0';
    switch (i) {
      case SS_FOLDERS: if (m->whole_sd) { snprintf(val, sizeof val, "%s", UI_T("WHOLE SD", "SD INTEIRO")); ink = COL_INK_RED; } else snprintf(val, sizeof val, "%d", m->open_folders); break;
      case SS_MODE: snprintf(val, sizeof val, "%s", m->read_only ? UI_T("Read-only", "Leitura") : UI_T("Development", "Desenvolvimento")); break;
      case SS_PAIRING: snprintf(val, sizeof val, "%d", m->paired_n); break;
      default: break;
    }
    ui_list_row(i, g_sel[SCR_SETTINGS], 0, settings_name(i), val, ink);
  }
}

static void draw_mcp(const ui_model *m) {
  char val[32];
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("ndev / MCP computers and the web page", "Computadores ndev / MCP e a página web"), UI_T("< Pairing  (B: back)", "< Pareamento  (B: voltar)"),
                   UI_T("A: Select   B: Back", "A: Selecionar   B: Voltar"));
  val[0] = '\0';
  if (m->pairing == 1) snprintf(val, sizeof val, "%s %lus", UI_T("open", "aberto"), (unsigned long)m->left_s);
  ui_list_row(SP_PAIR, g_sel[SCR_MCP], 0, UI_T("Pair a device", "Parear dispositivo"), val, COL_INK);
  snprintf(val, sizeof val, "%d", m->paired_n);
  ui_list_row(SP_DEVICES, g_sel[SCR_MCP], 0, UI_T("Paired devices", "Dispositivos pareados"), val, COL_INK);
  ui_list_row(SP_WEB, g_sel[SCR_MCP], 0, UI_T("Web page", "Página web"), m->web_on ? UI_T("on", "ligada") : UI_T("off", "desligada"), COL_INK);
  if (!m->auth_required) ui_text(10, 130, 0.4f, COL_INK_RED, 0, "%s", UI_T("This build has pairing turned off.", "Esta versão está com o pareamento desligado."));
}

static void draw_devices(const ui_model *m) {
  int i, n = rows_here(m), sel = g_sel[SCR_DEVICES], top = sel >= UI_LIST_ROWS ? sel - UI_LIST_ROWS + 1 : 0;
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("Computers paired with this console", "Computadores pareados com este console"), UI_T("< Paired devices  (B: back)", "< Dispositivos pareados  (B: voltar)"),
                   UI_T("A / tap again: unpair   B: Back", "A / tocar de novo: desparear   B: Voltar"));
  if (n == 0) { ui_text(160, 100, 0.5f, COL_DIM, UI_CENTER, "%s", UI_T("No paired devices.", "Nenhum dispositivo pareado.")); return; }
  for (i = top; i < n && i < top + UI_LIST_ROWS; i++) {
    if (i < m->paired_n) ui_list_row(i, sel, top, m->paired_label[i][0] ? m->paired_label[i] : UI_T("(unnamed)", "(sem nome)"), i == sel ? UI_T("tap again: unpair", "de novo: desparear") : "", i == sel ? COL_INK_RED : COL_INK);
    else ui_list_row(i, sel, top, UI_T("Forget all", "Esquecer todos"), m->forget_armed ? UI_T("tap again: confirm", "de novo: confirmar") : "", COL_INK_RED);
  }
}

static void draw_installer(const ui_model *m) {
  char val[24];
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("Install CIA files", "Instalar arquivos CIA"), UI_T("< Installer  (B: back)", "< Instalador  (B: voltar)"), UI_T("A: Select   B: Back", "A: Selecionar   B: Voltar"));
  val[0] = '\0';
  ui_list_row(SI_SD, g_sel[SCR_INST], 0, UI_T("Install from the SD card", "Instalar do cartão SD"), val, COL_INK);
  ui_list_row(SI_URL, g_sel[SCR_INST], 0, UI_T("Type a URL", "Digitar URL"), val, COL_INK);
  ui_list_row(SI_RECV, g_sel[SCR_INST], 0, UI_T("Receive from the browser", "Receber do navegador"), val, COL_INK);
  ui_list_row(SI_QR, g_sel[SCR_INST], 0, UI_T("Scan a QR code", "Escanear QR Code"), val, COL_INK);
  ui_list_row(SI_PENDING, g_sel[SCR_INST], 0, UI_T("Clear partial downloads", "Limpar downloads parciais"), m->pending_msg, COL_INK);
  if (m->seeds_dir) ui_list_row(SI_SEEDS, g_sel[SCR_INST], 0, UI_T("Register seeds (/fbi/seed)", "Registrar seeds (/fbi/seed)"), m->seed_msg, COL_INK);
  ui_text(160, 206, 0.36f, COL_DIM, UI_CENTER, "%s", UI_T("Seeds: the key some newer games need to open.", "Seeds: a chave que alguns jogos mais novos exigem para abrir."));
}

/* The scanner: the camera preview fills the top screen; the bottom screen explains. */
static void draw_qr(const ui_model *m) {
  ui_scene_top(COL_TOP_BG);
  qr_draw_preview(0, 0);
  if (!m->qr_on) {
    ui_text(200, 100, 0.55f, COL_RED, UI_CENTER, "%s", UI_T("Camera not available", "Câmera indisponível"));
    if (m->qr_err[0]) ui_text(200, 124, 0.4f, COL_DIM, UI_CENTER, "%.60s", m->qr_err);
  }
  ui_rect(0, 212, 400, 28, COL_TOP_HDR);
  ui_text(200, 218, 0.45f, m->qr_bad ? COL_YELLOW : COL_WHITE, UI_CENTER, "%s", m->qr_bad ? UI_T("That code is not an http(s) address. Try another.", "Esse código não é um endereço http(s). Tente outro.") : UI_T("Point the camera at a QR code with a CIA address", "Aponte a câmera para um QR code com o endereço de um CIA"));
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("Scan a QR code", "Escanear QR Code"), UI_T("< Scanning  (B: back)", "< Escaneando  (B: voltar)"), UI_T("X: Switch camera   B: Back", "X: Trocar câmera   B: Voltar"));
  ui_text(160, 70, 0.5f, COL_INK, UI_CENTER, "%s", m->qr_inner ? UI_T("Camera: inner", "Câmera: interna") : UI_T("Camera: outer", "Câmera: externa"));
  ui_text(160, 100, 0.42f, COL_INK, UI_CENTER, "%s", UI_T("Show the QR code on a screen or on paper.", "Mostre o QR code numa tela ou no papel."));
  ui_text(160, 120, 0.42f, COL_INK, UI_CENTER, "%s", UI_T("It must hold an http:// or https:// address.", "Ele precisa conter um endereço http:// ou https://."));
  ui_text(160, 160, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("Nothing is installed before you confirm.", "Nada é instalado antes de você confirmar."));
}

static void draw_log(void) {
  int i, n = log_count();
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("Activity log", "Log de atividade"), UI_T("< Back (B)", "< Voltar (B)"), UI_T("D-pad / drag: scroll", "D-pad / arrastar: rolar"));
  for (i = 0; i < UI_LIST_ROWS * 2 && g_log_off + i < n; i++)
    ui_text(4, (float)(UI_LIST_Y + i * 12), 0.36f, COL_INK, 0, "%.68s", alog_get(g_log_off + i));
}

static void draw_pair_request(const ui_model *m) {
  ui_rect(0, 100, 320, 120, COL_BOT_BG);
  ui_rect(0, 100, 320, 2, COL_BAR);
  ui_text(160, 108, 0.5f, COL_INK, UI_CENTER, "%s", UI_T("Same number on the computer?", "Mesmo número no computador?"));
  ui_text(160, 126, 0.9f, COL_INK, UI_CENTER, "%03u %03u", m->sas / 1000u, m->sas % 1000u);
  ui_rect(10, 160, 140, 40, UI_RGB(0x4c, 0xaf, 0x50));
  ui_text(80, 169, 0.55f, COL_WHITE, UI_CENTER, "A: %s", UI_T("Yes, pair", "Sim, parear"));
  ui_rect(170, 160, 140, 40, UI_RGB(0xe5, 0x39, 0x35));
  ui_text(240, 169, 0.55f, COL_WHITE, UI_CENTER, "B: %s", UI_T("No", "Não"));
  if (m->p2_replaces && m->p2_replaces[0]) ui_text(160, 202, 0.38f, COL_INK, UI_CENTER, "%s \"%.24s\"", UI_T("replaces", "substitui"), m->p2_replaces);
}

void ui_draw(const ui_model *m) {
  ui_begin();
  if (uii_flow_active(m)) {
    uii_flow_draw(m);
    if (m->pairing == 2) draw_pair_request(m);
    ui_end();
    return;
  }
  if (g_screen == SCR_TRANSL) {
    uitr_draw(m);
    if (m->pairing == 2) draw_pair_request(m);
    ui_end();
    return;
  }
  if (g_screen == SCR_TITLES) {
    uit_draw(m);
    if (m->pairing == 2) draw_pair_request(m);
    ui_end();
    return;
  }
  if (g_screen == SCR_QR) {
    draw_qr(m);
    if (m->pairing == 2) draw_pair_request(m);
    ui_end();
    return;
  }
  draw_top(m);
  switch (g_screen) {
    case SCR_INST: draw_installer(m); break;
    case SCR_SETTINGS: draw_settings(m); break;
    case SCR_SD: uii_sd_draw(); break;
    case SCR_RECV: uii_recv_draw(m); break;
    case SCR_MCP: draw_mcp(m); break;
    case SCR_DEVICES: draw_devices(m); break;
    case SCR_LOG: draw_log(); break;
    default: draw_main(m); break;
  }
  if (m->pairing == 2) draw_pair_request(m);
  ui_end();
}
#endif /* !AGENT_TEXT_UI */
