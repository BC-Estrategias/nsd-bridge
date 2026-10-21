/* Shared by ui.c and ui_inst.c (the installer screens): colors and the list row helper. */
#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "ui.h"

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
#define COL_INK_GREEN UI_RGB(0x1b, 0x7a, 0x2b)

/* One list row on the bottom screen: highlight when i == sel, name left, value right. */
void ui_list_row(int i, int sel, int top, const char *name, const char *val, uint32_t ink);

/* ---- installer screens (ui_inst.c) ---- */
void uii_sd_open(void);                                   /* start the SD browser at /cias (or the root) */
/* The SD browser: returns UI_INST_SD with ui_arg_str() = the chosen path; *leave = true when B left the browser. */
ui_action uii_sd_handle(u32 down, u32 held, const touchPosition *touch, bool *redraw, bool *leave);
void uii_sd_draw(void);
void uii_recv_draw(const ui_model *m);                    /* "waiting for a URL" (bottom screen) */
/* The install flow (probe / confirm / progress / result) takes over both screens while ins.state != INS_IDLE. */
bool uii_flow_active(const ui_model *m);
ui_action uii_flow_handle(u32 down, const touchPosition *touch, const ui_model *m, bool *redraw);
void uii_flow_draw(const ui_model *m);

/* ---- titles screens (ui_titles.c) ---- */
void uit_open(void);
void uit_shutdown(void);
void uit_handle(u32 down, u32 held, const touchPosition *touch, bool *redraw, bool *leave);
void uit_draw(const ui_model *m);   /* draws both screens */

/* ---- translations screens (ui_trans.c) ---- */
void uitr_open(int kind);   /* 0 translations, 1 mods */
bool uitr_flow_active(void);
ui_action uitr_handle(u32 down, u32 held, const touchPosition *touch, bool *redraw, bool *leave);
void uitr_draw(const ui_model *m);   /* draws both screens */
void uitr_set_message(const char *m);

#endif
