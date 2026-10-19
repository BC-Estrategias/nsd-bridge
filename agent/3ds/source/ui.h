/* Graphical console UI (citro2d), in the style of FBI: a status screen on top and a selectable menu on the bottom
 * that works with the D-pad and the touch screen. main.c owns the state and fills a ui_model; this module only draws
 * and turns buttons/touches into actions. The old text UI is still built with AGENT_TEXT_UI=1 (NDEV_TEXT_UI=1). */
#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include <stdint.h>

#include "ndp/ndp_auth.h"
#include "installer.h"
#include "updater.h"

/* What the main menu can ask main.c to do. */
typedef enum {
  UI_NONE = 0,
  UI_FOLDERS,   /* open the folder editor */
  UI_MODE,      /* READ_ONLY <-> DEVELOPMENT */
  UI_PAIR,      /* open/close the pairing window */
  UI_WEB,       /* web page on/off */
  UI_FORGET,    /* arm / confirm "forget all pairings" */
  UI_UNPAIR_ONE, /* forget one paired computer: its index is ui_arg() */
  UI_INST_SD,       /* install the CIA at ui_arg_str() */
  UI_INST_TYPE_URL, /* ask for a URL with the system keyboard */
  UI_INST_RECV_ON,  /* the "receive from the browser" screen opened: INSTALL_URL requests are taken */
  UI_INST_RECV_OFF,
  UI_INST_QR_ON,    /* the QR scanner screen opened (camera on) */
  UI_INST_QR_OFF,
  UI_INST_YES,      /* answers to the install confirmation / progress / result screens */
  UI_INST_NO,
  UI_INST_CANCEL,
  UI_INST_DISMISS,
  UI_UPDATE,    /* check for / install an update */
  UI_EXIT,
  UI_PAIR_YES,  /* answer to a pairing request */
  UI_PAIR_NO
} ui_action;

/* A snapshot of the app state, rebuilt by main.c before each draw. */
typedef struct {
  const char *version;
  int net;               /* 0 starting, 1 waiting for Wi-Fi, 2 error, 3 online */
  const char *err;
  char ip[16];
  unsigned port, web_port;
  bool web_on, web_up, web_open;
  const char *web_peer;  /* "" when no browser is connected */
  const char *bridge_peer;
  bool read_only;
  const char *mode_name;
  int open_folders, writable_folders;
  bool whole_sd;
  bool auth_required;
  int paired_n;
  char paired_label[NDP_MAX_KEYS][NDP_LABEL_MAX + 1];
  int pairing;           /* 0 none, 1 window open (code), 2 a request is waiting for A/B */
  unsigned sas;
  const char *p2_peer, *p2_label, *p2_replaces;
  char code[NDP_CODE_TEXT];
  unsigned left_s;
  bool forget_armed, keys_warn;
  char sd_free[24];      /* "13.7 GiB" or "" */
  unsigned long uptime_s, requests;
  bool is_new3ds, recv_open;
  char recv_addr[48];    /* what the page/ndev should reach: "192.168.1.5:8080" */
  ins_status ins;
  upd_state upd;
  unsigned upd_progress;
  char upd_version[24], upd_message[64];
} ui_model;

/* Colors (RGBA8, as citro2d wants them). */
#define UI_RGB(r, g, b) ((uint32_t)C2D_Color32((r), (g), (b), 255))

void ui_init(void);
void ui_exit(void);
const char *ui_arg_str(void);  /* the text that goes with UI_INST_SD */
void ui_set_arg_str(const char *s);
int ui_arg(void);  /* the index that goes with the last UI_UNPAIR_ONE */
bool ui_pt(void);  /* true when the console is set to Portuguese: all strings pick pt-BR */
#define UI_T(en, pt) (ui_pt() ? (pt) : (en))

/* ---- the main menu / log screens ---- */
/* Feeds one frame of input. Returns the action to run (UI_NONE if nothing) and sets *redraw when the screen changed. */
ui_action ui_handle(u32 down, u32 held, const touchPosition *touch, const ui_model *m, bool *redraw);
void ui_draw(const ui_model *m);

/* ---- drawing primitives, shared with the folder editor ---- */
#define UI_ROW_H 22
#define UI_LIST_Y 40
#define UI_LIST_ROWS 8
void ui_begin(void);                    /* starts a frame */
void ui_end(void);                      /* presents it */
void ui_scene_top(uint32_t bg);         /* 400x240 */
void ui_scene_bottom(uint32_t bg);      /* 320x240 */
void ui_rect(float x, float y, float w, float h, uint32_t color);
/* flags: 0 left, UI_RIGHT / UI_CENTER relative to x. */
#define UI_RIGHT 1
#define UI_CENTER 2
void ui_text(float x, float y, float scale, uint32_t color, int flags, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
/* Title bar (y 20..40) and footer (y 220..240) of the bottom screen, FBI style. */
void ui_bottom_chrome(const char *info, const char *title, const char *hint);
/* Converts a touch to a list row (0..UI_LIST_ROWS-1), or -1. */
int ui_row_at(int y);
/* Drag-scroll: returns how many rows the content should move this frame (+ = towards the end). */
int ui_drag_rows(u32 down, u32 held, const touchPosition *touch);

#endif
