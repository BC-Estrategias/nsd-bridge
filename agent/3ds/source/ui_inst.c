#ifndef AGENT_TEXT_UI
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "ui_internal.h"

/* ------------------------------------------------------------------------------------------- SD browser */
#define SD_MAX 256
#define NAME_MAX_UI 96
#define PATH_MAX_UI 400

static struct {
  char path[PATH_MAX_UI];           /* "/cias" (no "sdmc:") */
  struct { char name[NAME_MAX_UI]; bool dir; unsigned long size; } e[SD_MAX + 1];
  int n, sel, top;
  bool truncated;
} sd;

static int cmp_entries(const void *a, const void *b) {
  const typeof(sd.e[0]) *x = a, *y = b;
  if (x->dir != y->dir) return x->dir ? -1 : 1;
  return strcasecmp(x->name, y->name);
}

static bool is_cia(const char *n) {
  size_t l = strlen(n);
  return l > 4 && !strcasecmp(n + l - 4, ".cia");
}

static void sd_load(const char *select) {
  char fs[PATH_MAX_UI + 8];
  DIR *d;
  struct dirent *de;
  int i;
  sd.n = 0;
  sd.truncated = false;
  snprintf(fs, sizeof fs, "sdmc:%s", sd.path);
  if (strcmp(sd.path, "/") != 0) { /* the ".." row */
    strcpy(sd.e[0].name, "..");
    sd.e[0].dir = true;
    sd.e[0].size = 0;
    sd.n = 1;
  }
  d = opendir(fs);
  if (d) {
    int first = sd.n;
    while ((de = readdir(d)) != NULL) {
      bool dir = de->d_type == DT_DIR;
      if (de->d_name[0] == '.' || (!dir && !is_cia(de->d_name)) || strlen(de->d_name) >= NAME_MAX_UI) continue;
      if (sd.n >= SD_MAX) { sd.truncated = true; break; }
      snprintf(sd.e[sd.n].name, NAME_MAX_UI, "%s", de->d_name);
      sd.e[sd.n].dir = dir;
      sd.e[sd.n].size = 0;
      sd.n++;
    }
    closedir(d);
    qsort(&sd.e[first], (size_t)(sd.n - first), sizeof sd.e[0], cmp_entries);
    for (i = first; i < sd.n; i++) {
      if (!sd.e[i].dir) {
        struct stat st;
        char full[PATH_MAX_UI + NAME_MAX_UI + 12];
        snprintf(full, sizeof full, "%s/%s", fs, sd.e[i].name);
        if (stat(full, &st) == 0) sd.e[i].size = (unsigned long)st.st_size;
      }
    }
  }
  sd.sel = 0;
  if (select)
    for (i = 0; i < sd.n; i++)
      if (!strcasecmp(sd.e[i].name, select)) { sd.sel = i; break; }
  sd.top = sd.sel >= UI_LIST_ROWS ? sd.sel - UI_LIST_ROWS + 1 : 0;
}

void uii_sd_open(void) {
  struct stat st;
  snprintf(sd.path, sizeof sd.path, "%s", stat("sdmc:/cias", &st) == 0 ? "/cias" : "/");
  sd_load(NULL);
}

static void sd_up(void) {
  char from[NAME_MAX_UI];
  char *slash = strrchr(sd.path, '/');
  snprintf(from, sizeof from, "%s", slash ? slash + 1 : "");
  if (slash == sd.path) strcpy(sd.path, "/");
  else if (slash) *slash = '\0';
  sd_load(from);
}

static ui_action sd_activate(void) {
  if (sd.n == 0) return UI_NONE;
  if (sd.e[sd.sel].dir) {
    if (!strcmp(sd.e[sd.sel].name, "..")) { sd_up(); return UI_NONE; }
    char name[NAME_MAX_UI];
    snprintf(name, sizeof name, "%s", sd.e[sd.sel].name);
    if (strlen(sd.path) + strlen(name) + 2 < PATH_MAX_UI) {
      size_t l = strlen(sd.path);
      snprintf(sd.path + l, sizeof sd.path - l, "%s%s", l > 1 ? "/" : "", name);
      sd_load(NULL);
    }
    return UI_NONE;
  }
  {
    char full[PATH_MAX_UI + NAME_MAX_UI + 12];
    snprintf(full, sizeof full, "sdmc:%s%s%s", sd.path, strcmp(sd.path, "/") ? "/" : "", sd.e[sd.sel].name);
    ui_set_arg_str(full);
  }
  return UI_INST_SD;
}

ui_action uii_sd_handle(u32 down, u32 held, const touchPosition *touch, bool *redraw, bool *leave) {
  int step = ui_drag_rows(down, held, touch), max = sd.n > UI_LIST_ROWS ? sd.n - UI_LIST_ROWS : 0;
  *leave = false;
  if (step) {
    sd.top += step;
    if (sd.top < 0) sd.top = 0;
    if (sd.top > max) sd.top = max;
    if (sd.sel < sd.top) sd.sel = sd.top;
    if (sd.sel >= sd.top + UI_LIST_ROWS) sd.sel = sd.top + UI_LIST_ROWS - 1;
    *redraw = true;
  }
  if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) { if (sd.n) sd.sel = (sd.sel + 1) % sd.n; *redraw = true; }
  else if (down & (KEY_DUP | KEY_CPAD_UP)) { if (sd.n) sd.sel = (sd.sel + sd.n - 1) % sd.n; *redraw = true; }
  else if (down & KEY_R) { sd.sel = sd.sel + 8 < sd.n ? sd.sel + 8 : sd.n ? sd.n - 1 : 0; *redraw = true; }
  else if (down & KEY_L) { sd.sel = sd.sel > 8 ? sd.sel - 8 : 0; *redraw = true; }
  else if ((down & KEY_A) && sd.n) { *redraw = true; return sd_activate(); }
  else if (down & KEY_B) {
    if (strcmp(sd.path, "/") == 0) *leave = true;
    else sd_up();
    *redraw = true;
    return UI_NONE;
  } else if (down & KEY_TOUCH) {
    int r = ui_row_at(touch->py);
    if (touch->py >= 20 && touch->py < 40) {
      if (strcmp(sd.path, "/") == 0) *leave = true;
      else sd_up();
      *redraw = true;
      return UI_NONE;
    }
    if (r >= 0 && sd.top + r < sd.n) {
      *redraw = true;
      if (sd.top + r == sd.sel) return sd_activate();
      sd.sel = sd.top + r;
    }
  }
  if (sd.sel < sd.top) sd.top = sd.sel;
  if (sd.sel >= sd.top + UI_LIST_ROWS) sd.top = sd.sel - UI_LIST_ROWS + 1;
  return UI_NONE;
}

static void fmt_size(char *out, size_t cap, unsigned long bytes) {
  if (bytes >= 1024ul * 1024ul) snprintf(out, cap, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
  else snprintf(out, cap, "%lu KiB", (bytes + 1023) / 1024);
}

void uii_sd_draw(void) {
  int i;
  char title[PATH_MAX_UI + 16], val[24];
  ui_scene_bottom(COL_BOT_BG);
  snprintf(title, sizeof title, "< %s", sd.path);
  ui_bottom_chrome(UI_T("Install from the SD card", "Instalar do cartão SD"), title, UI_T("A: Open / install   B: Back", "A: Abrir / instalar   B: Voltar"));
  if (sd.n == 0) { ui_text(160, 100, 0.5f, COL_DIM, UI_CENTER, "%s", UI_T("No CIA files here.", "Nenhum arquivo CIA aqui.")); return; }
  for (i = sd.top; i < sd.n && i < sd.top + UI_LIST_ROWS; i++) {
    char name[NAME_MAX_UI + 4];
    val[0] = '\0';
    if (sd.e[i].dir) snprintf(name, sizeof name, "%s%s", sd.e[i].name, strcmp(sd.e[i].name, "..") ? "/" : "");
    else { snprintf(name, sizeof name, "%s", sd.e[i].name); fmt_size(val, sizeof val, sd.e[i].size); }
    ui_list_row(i, sd.sel, sd.top, name, val, COL_INK);
  }
  if (sd.truncated) ui_text(160, 205, 0.36f, COL_INK_RED, UI_CENTER, "%s", UI_T("List cut at 256 entries.", "Lista cortada em 256 itens."));
}

/* ----------------------------------------------------------------------------------- receive from the browser */
void uii_recv_draw(const ui_model *m) {
  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("Receive from the browser", "Receber do navegador"), UI_T("< Waiting...  (B: back)", "< Aguardando...  (B: voltar)"), UI_T("B: Back", "B: Voltar"));
  ui_text(160, 50, 0.5f, COL_INK, UI_CENTER, "%s", UI_T("Send a CIA address from the page.", "Envie o endereço de um CIA pela página."));
  ui_text(160, 74, 0.42f, COL_INK, UI_CENTER, "%s", UI_T("Open the page, tab Install, paste the URL.", "Abra a página, aba Instalar, cole a URL."));
  ui_text(160, 98, 0.42f, COL_INK, UI_CENTER, "%s", UI_T("Or on the computer run:", "Ou, no computador, rode:"));
  ui_text(160, 118, 0.42f, COL_INK_GREEN, UI_CENTER, "ndev install %.20s game.cia", m->net == 3 ? m->ip : "<ip>");
  ui_text(160, 160, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("Nothing is downloaded before you confirm here.", "Nada é baixado antes de você confirmar aqui."));
}

/* ---------------------------------------------------------------------------------------------- install flow */
bool uii_flow_active(const ui_model *m) { return m->ins.state != INS_IDLE; }

static const char *err_text(const ins_status *s) {
  static char buf[96];
  switch (s->error) {
    case INS_E_CANCELLED: return UI_T("Cancelled.", "Cancelado.");
    case INS_E_READ: return UI_T("Could not read the file.", "Não foi possível ler o arquivo.");
    case INS_E_NOT_CIA: return UI_T("This is not a valid CIA file.", "Este não é um arquivo CIA válido.");
    case INS_E_FIRM: return UI_T("Firmware titles are not supported.", "Títulos de firmware não são suportados.");
    case INS_E_SELF: return UI_T("This is NSD Bridge itself: use Updates.", "Este é o próprio NSD Bridge: use Atualizações.");
    case INS_E_SPACE: return UI_T("Not enough space on the SD card.", "Sem espaço suficiente no cartão SD.");
    case INS_E_SIZE: return UI_T("The size does not match the CIA header: nothing was installed.", "O tamanho não bate com o cabeçalho: nada foi instalado.");
    case INS_E_HASH: return UI_T("The SHA-256 does not match: nothing was installed.", "O SHA-256 não bate: nada foi instalado.");
    case INS_E_NET:
      if (s->http_code >= 400) snprintf(buf, sizeof buf, "%s %ld.", UI_T("The server answered HTTP", "O servidor respondeu HTTP"), s->http_code);
      else if (s->net_code == 28) snprintf(buf, sizeof buf, "%s", UI_T("The server did not answer in time (curl 28).", "O servidor não respondeu a tempo (curl 28)."));
      else if (s->net_code == 6) snprintf(buf, sizeof buf, "%s", UI_T("Could not find the server (curl 6).", "Não encontrou o servidor (curl 6)."));
      else if (s->net_code == 7) snprintf(buf, sizeof buf, "%s", UI_T("Could not connect to the server (curl 7).", "Não conseguiu conectar ao servidor (curl 7)."));
      else if (s->net_code == 35 || s->net_code == 51 || s->net_code == 58 || s->net_code == 60) snprintf(buf, sizeof buf, "%s (curl %d).", UI_T("HTTPS / certificate problem", "Problema de HTTPS / certificado"), s->net_code);
      else snprintf(buf, sizeof buf, "%s (curl %d).", UI_T("The download failed", "O download falhou"), s->net_code);
      return buf;
    case INS_E_AM: snprintf(buf, sizeof buf, "%s 0x%08lX", UI_T("The system refused the install:", "O sistema recusou a instalação:"), (unsigned long)s->am_result); return buf;
    default: return "";
  }
}

static void version_text(char *out, size_t cap, unsigned v) { snprintf(out, cap, "%u.%u.%u", v >> 10, (v >> 4) & 0x3F, v & 0xF); }

ui_action uii_flow_handle(u32 down, const touchPosition *touch, const ui_model *m, bool *redraw) {
  ins_state st = m->ins.state;
  bool tap = (down & KEY_TOUCH) != 0;
  if (st == INS_PROBING) {
    if (down & KEY_B) { *redraw = true; return UI_INST_CANCEL; }
  } else if (st == INS_CONFIRM) {
    if (down & KEY_A) { *redraw = true; return UI_INST_YES; }
    if (down & KEY_B) { *redraw = true; return UI_INST_NO; }
    if (tap && touch->py >= 160 && touch->py < 210) { *redraw = true; return touch->px < 160 ? UI_INST_YES : UI_INST_NO; }
  } else if (st == INS_INSTALLING) {
    if (down & KEY_B) { *redraw = true; return UI_INST_CANCEL; }
    if (tap && touch->py >= 160 && touch->py < 210) { *redraw = true; return UI_INST_CANCEL; }
  } else if ((down & (KEY_A | KEY_B)) || (tap && touch->py >= 160 && touch->py < 210)) {
    *redraw = true;
    return UI_INST_DISMISS;
  }
  return UI_NONE;
}

static void button(float x, float w, uint32_t color, const char *label) {
  ui_rect(x, 160, w, 48, color);
  ui_text(x + w / 2, 173, 0.6f, COL_WHITE, UI_CENTER, "%s", label);
}

void uii_flow_draw(const ui_model *m) {
  const ins_status *s = &m->ins;
  char ver[24], size[24], line[120];
  float y = 34;
  ui_scene_top(COL_TOP_BG);
  ui_rect(0, 0, 400, 24, COL_TOP_HDR);
  ui_text(8, 3, 0.6f, COL_CYAN, 0, "%s", UI_T("Installer", "Instalador"));
  ui_text(392, 5, 0.45f, COL_DIM, UI_RIGHT, "%s", s->is_url ? UI_T("from a URL", "de uma URL") : UI_T("from the SD card", "do cartão SD"));
  ui_battery(276, 5, m->battery, m->charging, m->adapter);
  if (s->is_url) ui_text(12, y, 0.5f, COL_WHITE, 0, "%.60s", s->host);
  else ui_text(12, y, 0.42f, COL_WHITE, 0, "%.64s", s->source);
  y += 22;
  if (s->state == INS_PROBING) {
    ui_text(200, 110, 0.6f, COL_YELLOW, UI_CENTER, "%s", UI_T("Checking the file...", "Verificando o arquivo..."));
  } else if (s->has_info) {
    const cia_info *ci = &s->info;
    version_text(ver, sizeof ver, ci->title_version);
    fmt_size(size, sizeof size, (unsigned long)ci->total_size);
    ui_text(12, y, 0.45f, COL_DIM, 0, "%s", UI_T("Title ID", "ID do título"));
    ui_text(110, y, 0.45f, COL_WHITE, 0, "%016llX", (unsigned long long)ci->title_id);
    y += 18;
    ui_text(12, y, 0.45f, COL_DIM, 0, "%s", UI_T("Version", "Versão"));
    ui_text(110, y, 0.45f, COL_WHITE, 0, "%s     %s", ver, size);
    y += 18;
    ui_text(12, y, 0.45f, COL_DIM, 0, "%s", UI_T("Goes to", "Vai para"));
    if (ci->cls == CIA_CLASS_SYSTEM) ui_text(110, y, 0.45f, COL_RED, 0, "%s", UI_T("SYSTEM MEMORY (system title)", "MEMÓRIA DO SISTEMA (título de sistema)"));
    else if (ci->cls == CIA_CLASS_DSIWARE) ui_text(110, y, 0.45f, COL_YELLOW, 0, "%s", UI_T("system memory (DSiWare)", "memória do sistema (DSiWare)"));
    else ui_text(110, y, 0.45f, COL_GREEN, 0, "%s", UI_T("SD card", "cartão SD"));
    y += 22;
    if (ci->cls == CIA_CLASS_SYSTEM && s->state == INS_CONFIRM) {
      ui_text(12, y, 0.42f, COL_RED, 0, "%s", UI_T("A wrong system title can stop the console from booting.", "Um título de sistema errado pode impedir o console de ligar."));
      y += 16;
    }
    if (ci->new3ds_only && !m->is_new3ds && s->state == INS_CONFIRM) {
      ui_text(12, y, 0.42f, COL_RED, 0, "%s", UI_T("Made for New 3DS; this console is not one.", "Feito para o New 3DS; este console não é."));
      y += 16;
    }
    if (s->replaces && s->state == INS_CONFIRM) {
      ui_text(12, y, 0.42f, COL_YELLOW, 0, "%s", UI_T("Replaces the installed copy (removed first).", "Substitui a cópia instalada (removida antes)."));
      y += 16;
    }
    if (s->state == INS_CONFIRM) {
      if (s->hash_expected) ui_text(12, y, 0.42f, COL_GREEN, 0, "%s", UI_T("The SHA-256 will be checked before it is finished.", "O SHA-256 será conferido antes de concluir."));
      else if (s->is_url) ui_text(12, y, 0.42f, COL_DIM, 0, "%s", UI_T("No SHA-256 given: only size and the system's checks.", "Sem SHA-256: só o tamanho e as checagens do sistema."));
      y += 16;
      /* the seed was looked up before the download: a title that needs one cannot be opened without it */
      if (s->seed_pre == 1) ui_text(12, y, 0.42f, COL_GREEN, 0, "%s %s", UI_T("Seed available", "Seed disponível"), s->seed_src == 1 ? UI_T("(from the request)", "(do pedido)") : s->seed_src == 2 ? "(/fbi/seed)" : UI_T("(Nintendo's server)", "(servidor da Nintendo)"));
      else if (s->seed_pre == 2) ui_text(12, y, 0.42f, COL_DIM, 0, "%s", UI_T("The server has no seed: this title uses none.", "O servidor não tem seed: este título não usa."));
      else if (s->seed_pre == 3) ui_text(12, y, 0.42f, COL_YELLOW, 0, "%s", UI_T("Could not check the seed now: the game may not open.", "Não consegui checar a seed agora: o jogo pode não abrir."));
      else if (s->seed_pre == 0) ui_text(12, y, 0.42f, COL_DIM, 0, "%s", UI_T("Seed lookup is turned off.", "A busca de seed está desligada."));
      if (s->resume_bytes) {
        y += 16;
        ui_text(12, y, 0.42f, COL_CYAN, 0, "%s %lu MiB", UI_T("Resumes: already on the card:", "Retoma: já está no cartão:"), (unsigned long)(s->resume_bytes >> 20));
      }
    }
  }
  if (s->state == INS_INSTALLING) {
    float frac = s->total ? (float)((double)s->done / (double)s->total) : 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (s->phase == 1) ui_text(200, 132, 0.45f, COL_CYAN, UI_CENTER, "%s (%d %s)", UI_T("Downloading", "Baixando"), s->conns, UI_T("connections", "conexões"));
    else if (s->phase == 2) ui_text(200, 132, 0.45f, COL_CYAN, UI_CENTER, "%s", UI_T("Installing from the card", "Instalando a partir do cartão"));
    ui_rect(30, 150, 340, 14, COL_BAR);
    ui_rect(30, 150, 340.0f * frac, 14, COL_GREEN);
    snprintf(line, sizeof line, "%u%%   %.1f / %.1f MiB   %.2f MiB/s", (unsigned)(frac * 100.0f), (double)s->done / 1048576.0, (double)s->total / 1048576.0, (double)s->bps / 1048576.0);
    ui_text(200, 168, 0.42f, COL_WHITE, UI_CENTER, "%s", line);
    if (s->eta_s > 0) {
      char t[40];
      unsigned e = s->eta_s;
      if (e >= 3600) snprintf(t, sizeof t, "%u h %02u min", e / 3600, (e / 60) % 60);
      else if (e >= 60) snprintf(t, sizeof t, "%u min %02u s", e / 60, e % 60);
      else snprintf(t, sizeof t, "%u s", e);
      ui_text(200, 184, 0.45f, COL_CYAN, UI_CENTER, "%s ~%s", UI_T("Time left", "Tempo restante"), t);
    } else {
      ui_text(200, 184, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("Estimating the time left...", "Estimando o tempo restante..."));
    }
    ui_text(200, 200, 0.38f, COL_YELLOW, UI_CENTER, "%s", UI_T("HOME and sleep are off until it finishes.", "HOME e repouso ficam desligados até terminar."));
    ui_text(200, 218, 0.45f, COL_WHITE, UI_CENTER, "%s", UI_T("B: Cancel   (screens dim to save battery)", "B: Cancelar   (telas apagam p/ poupar bateria)"));
  } else if (s->state == INS_DONE && s->is_bench) {
    static const char *const names[4] = {"easy", "2 connections", "4 connections", "8 connections"};
    int i;
    ui_text(200, 60, 0.55f, COL_CYAN, UI_CENTER, "%s", UI_T("Speed test", "Teste de velocidade"));
    for (i = 0; i < 4; i++) ui_text(60, 90 + (float)i * 22, 0.48f, COL_WHITE, 0, "%-20s %5u KiB/s", names[i], s->bench_kib[i]);
  } else if (s->state == INS_DONE) {
    ui_text(200, 150, 0.7f, COL_GREEN, UI_CENTER, "%s", UI_T("Installed", "Instalado"));
    ui_text(200, 168, 0.34f, COL_DIM, UI_CENTER, "SHA-256 %.32s", s->sha_hex);
    ui_text(200, 179, 0.34f, COL_DIM, UI_CENTER, "%.32s", s->sha_hex + 32);
    if (s->seed == 1) ui_text(200, 192, 0.4f, COL_GREEN, UI_CENTER, "%s", UI_T("Game key (seed) registered.", "Chave do jogo (seed) registrada."));
    else if (s->seed == 2) ui_text(200, 192, 0.4f, COL_DIM, UI_CENTER, "%s", UI_T("This title needs no seed.", "Este título não precisa de seed."));
    else if (s->seed == 3) ui_text(200, 192, 0.4f, COL_YELLOW, UI_CENTER, "%s", UI_T("Could not get the seed: the game may not open.", "Não consegui a seed: o jogo pode não abrir."));
  } else if (s->state == INS_ERROR) {
    ui_text(200, 150, 0.5f, COL_RED, UI_CENTER, "%.70s", err_text(s));
  }

  ui_scene_bottom(COL_BOT_BG);
  ui_bottom_chrome(UI_T("Installer", "Instalador"), s->state == INS_CONFIRM ? UI_T("Install this title?", "Instalar este título?") : s->state == INS_INSTALLING ? (s->phase == 1 ? UI_T("Downloading", "Baixando") : UI_T("Installing", "Instalando")) : s->state == INS_PROBING ? UI_T("Checking", "Verificando") : s->state == INS_DONE ? UI_T("Done", "Concluído") : UI_T("Failed", "Falhou"),
                   s->state == INS_CONFIRM ? UI_T("A: Install   B: Cancel", "A: Instalar   B: Cancelar") : s->state == INS_INSTALLING || s->state == INS_PROBING ? UI_T("B: Cancel", "B: Cancelar") : UI_T("A / B: OK", "A / B: OK"));
  if (s->state == INS_CONFIRM) {
    button(10, 145, UI_RGB(0x4c, 0xaf, 0x50), UI_T("A: Install", "A: Instalar"));
    button(165, 145, UI_RGB(0xe5, 0x39, 0x35), UI_T("B: Cancel", "B: Cancelar"));
  } else if (s->state == INS_INSTALLING) {
    button(60, 200, UI_RGB(0xe5, 0x39, 0x35), UI_T("B: Cancel", "B: Cancelar"));
  } else if (s->state == INS_DONE || s->state == INS_ERROR) {
    button(60, 200, UI_RGB(0x42, 0x7a, 0xb8), "OK");
  }
}
#endif /* !AGENT_TEXT_UI */
