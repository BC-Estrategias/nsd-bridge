#ifndef AGENT_TEXT_UI
#include "ui.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alog.h"

#define COL_TOP_BG UI_RGB(0x2b, 0x2d, 0x33)
#define COL_TOP_HDR UI_RGB(0x1b, 0x1c, 0x20)
#define COL_BAR UI_RGB(0x3f, 0x41, 0x47)
#define COL_BOT_BG UI_RGB(0xe8, 0xe8, 0xe8)
#define COL_TITLE UI_RGB(0xcf, 0xcf, 0xcf)
#define COL_SEL UI_RGB(0xb4, 0xb4, 0xb4)
#define COL_INK UI_RGB(0x22, 0x22, 0x22)
#define COL_WHITE UI_RGB(0xff, 0xff, 0xff)
#define COL_DIM UI_RGB(0x9a, 0xa0, 0xaa)
#define COL_GREEN UI_RGB(0x6b, 0xd0, 0x6b)
#define COL_YELLOW UI_RGB(0xf0, 0xc8, 0x3c)
#define COL_RED UI_RGB(0xf0, 0x5a, 0x50)
#define COL_CYAN UI_RGB(0x6a, 0xc8, 0xf0)
#define COL_INK_RED UI_RGB(0xb0, 0x20, 0x20)

static C3D_RenderTarget *g_top, *g_bot;
static C2D_TextBuf g_tb;
static bool g_ready, g_pt;

enum { SCR_MAIN, SCR_LOG };
static int g_screen = SCR_MAIN, g_log_off = 0;
static int g_sel = 1; /* starts on the row after Updates (IT_FOLDERS): a stray A never starts an install */

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

/* ---- main menu ---- */
enum { IT_UPDATE, IT_FOLDERS, IT_MODE, IT_PAIR, IT_WEB, IT_PAIRED, IT_LOG, IT_EXIT, IT_COUNT };

static const char *item_name(int i) {
  switch (i) {
    case IT_FOLDERS: return UI_T("Open folders", "Pastas liberadas");
    case IT_MODE: return UI_T("Mode", "Modo");
    case IT_PAIR: return UI_T("Pair a device", "Parear dispositivo");
    case IT_WEB: return UI_T("Web page", "Página web");
    case IT_PAIRED: return UI_T("Paired devices", "Dispositivos pareados");
    case IT_UPDATE: return UI_T("Updates", "Atualizações");
    case IT_LOG: return UI_T("Activity log", "Log de atividade");
    default: return UI_T("Exit", "Sair");
  }
}

static ui_action item_action(int i) {
  switch (i) {
    case IT_FOLDERS: return UI_FOLDERS;
    case IT_MODE: return UI_MODE;
    case IT_PAIR: return UI_PAIR;
    case IT_WEB: return UI_WEB;
    case IT_PAIRED: return UI_FORGET;
    case IT_UPDATE: return UI_UPDATE;
    case IT_EXIT: return UI_EXIT;
    default: return UI_NONE;
  }
}

static int log_count(void) {
  int n = 0;
  while (alog_get(n)) n++;
  return n;
}

ui_action ui_handle(u32 down, u32 held, const touchPosition *touch, const ui_model *m, bool *redraw) {
  /* a pairing request owns A/B (and the two touch buttons) until it is answered */
  if (m->pairing == 2) {
    if (down & KEY_A) { *redraw = true; return UI_PAIR_YES; }
    if (down & KEY_B) { *redraw = true; return UI_PAIR_NO; }
    if (down & KEY_TOUCH) {
      if (touch->py >= 150 && touch->py < 200) {
        *redraw = true;
        return touch->px < 160 ? UI_PAIR_YES : UI_PAIR_NO;
      }
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
    if ((down & KEY_B) || (down & KEY_START) || ((down & KEY_TOUCH) && touch->py >= 20 && touch->py < 40)) {
      g_screen = SCR_MAIN;
      *redraw = true;
    }
    return UI_NONE;
  }
  if (down & KEY_START) return UI_EXIT;
  if (down & KEY_X) return UI_MODE;
  if (down & KEY_Y) return UI_PAIR;
  if (down & KEY_R) return UI_WEB;
  if (down & KEY_SELECT) return UI_FORGET;
  if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { g_sel = (g_sel + 1) % IT_COUNT; *redraw = true; }
  else if (down & (KEY_DUP | KEY_CPAD_UP)) { g_sel = (g_sel + IT_COUNT - 1) % IT_COUNT; *redraw = true; }
  else if (down & KEY_A) {
    *redraw = true;
    if (g_sel == IT_LOG) { g_screen = SCR_LOG; g_log_off = 0; return UI_NONE; }
    return item_action(g_sel);
  } else if (down & KEY_TOUCH) {
    int r = ui_row_at(touch->py);
    if (r >= 0 && r < IT_COUNT) {
      /* first tap selects, a tap on the selected row runs it (a stray touch never exits or changes the mode) */
      *redraw = true;
      if (r == g_sel) {
        if (g_sel == IT_LOG) { g_screen = SCR_LOG; g_log_off = 0; return UI_NONE; }
        return item_action(g_sel);
      }
      g_sel = r;
    }
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
  ui_text(392, 5, 0.45f, COL_DIM, UI_RIGHT, "v%s  -  BC Labs", m->version);

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
    ui_text(200, 174, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("On the page click Pair, or: ndev pair <ip>", "Na página, clique em Parear, ou: ndev pair <ip>"));
    ui_text(200, 190, 0.5f, COL_CYAN, UI_CENTER, "%s: %s", UI_T("code", "código"), m->code);
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

static void draw_menu(const ui_model *m) {
  int i;
  char info[80], val[48];
  snprintf(info, sizeof info, "SD: %s %s   %s%s%s", m->sd_free[0] ? m->sd_free : "-", m->sd_free[0] ? UI_T("free", "livres") : "",
           m->net == 3 ? m->ip : "", m->net == 3 ? ":" : "", m->net == 3 ? "6464" : "");
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(info, UI_T("Main menu", "Menu principal"), UI_T("A: Select   X: Mode   Y: Pair   START: Exit", "A: Selecionar  X: Modo  Y: Parear  START: Sair"));
  for (i = 0; i < IT_COUNT; i++) {
    float y = (float)(UI_LIST_Y + i * UI_ROW_H);
    uint32_t ink = COL_INK;
    val[0] = '\0';
    if (i == g_sel) ui_rect(0, y, 320, UI_ROW_H, COL_SEL);
    switch (i) {
      case IT_FOLDERS: if (m->whole_sd) { snprintf(val, sizeof val, "%s", UI_T("WHOLE SD", "SD INTEIRO")); ink = COL_INK_RED; } else snprintf(val, sizeof val, "%d", m->open_folders); break;
      case IT_MODE: snprintf(val, sizeof val, "%s", m->read_only ? UI_T("Read-only", "Leitura") : UI_T("Development", "Desenvolvimento")); break;
      case IT_PAIR: if (m->pairing == 1) snprintf(val, sizeof val, "%s %lus", UI_T("open", "aberto"), (unsigned long)m->left_s); break;
      case IT_WEB: snprintf(val, sizeof val, "%s", m->web_on ? UI_T("on", "ligada") : UI_T("off", "desligada")); break;
      case IT_PAIRED:
        if (m->forget_armed) { snprintf(val, sizeof val, "%s", UI_T("tap again: forget all", "de novo: esquecer todos")); ink = COL_INK_RED; }
        else snprintf(val, sizeof val, "%d", m->paired);
        break;
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
      default: break;
    }
    ui_text(10, y + 3, 0.5f, ink, 0, "%s", item_name(i));
    if (val[0]) ui_text(310, y + 4, 0.45f, ink, UI_RIGHT, "%s", val);
  }
  if (m->pairing == 2) {
    ui_rect(0, 100, 320, 120, COL_BOT_BG);
    ui_rect(0, 100, 320, 2, COL_BAR);
    ui_text(160, 108, 0.5f, COL_INK, UI_CENTER, "%s", UI_T("Same number on the page?", "Mesmo número na página?"));
    ui_text(160, 126, 0.9f, COL_INK, UI_CENTER, "%03u %03u", m->sas / 1000u, m->sas % 1000u);
    ui_rect(10, 160, 140, 40, UI_RGB(0x4c, 0xaf, 0x50));
    ui_text(80, 169, 0.55f, COL_WHITE, UI_CENTER, "A: %s", UI_T("Yes, pair", "Sim, parear"));
    ui_rect(170, 160, 140, 40, UI_RGB(0xe5, 0x39, 0x35));
    ui_text(240, 169, 0.55f, COL_WHITE, UI_CENTER, "B: %s", UI_T("No", "Não"));
    if (m->p2_replaces && m->p2_replaces[0]) ui_text(160, 202, 0.38f, COL_INK, UI_CENTER, "%s \"%.24s\"", UI_T("replaces", "substitui"), m->p2_replaces);
  }
}

static void draw_log(void) {
  int i, n = log_count();
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("Activity log", "Log de atividade"), UI_T("< Back (B)", "< Voltar (B)"), UI_T("D-pad / drag: scroll", "D-pad / arrastar: rolar"));
  for (i = 0; i < UI_LIST_ROWS * 2 && g_log_off + i < n; i++)
    ui_text(4, (float)(UI_LIST_Y + i * 12), 0.36f, COL_INK, 0, "%.68s", alog_get(g_log_off + i));
}

void ui_draw(const ui_model *m) {
  ui_begin();
  draw_top(m);
  if (g_screen == SCR_LOG && m->pairing != 2) draw_log();
  else draw_menu(m);
  ui_end();
}
#endif /* !AGENT_TEXT_UI */
