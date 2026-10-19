/* QR scanner (3DS): the camera on a worker thread, frames decoded with quirc. The preview goes to the top screen. A
 * decoded address is handed to the installer by main.c; nothing is installed without the owner's confirmation. */
#ifndef QR_H
#define QR_H

#include <3ds.h>
#include <stdbool.h>

bool qr_start(bool inner_camera);   /* false when the camera cannot start (see qr_error) */
void qr_stop(void);
bool qr_active(void);
/* True once when a QR code was decoded; `out` gets its text (the first line). */
bool qr_take(char *out, size_t cap);
unsigned qr_frame_serial(void);     /* bumps with each new camera frame: the UI redraws on it */
/* Draws the latest camera frame at (x, y), 400x240. Call inside a citro2d scene. */
void qr_draw_preview(float x, float y);
const char *qr_error(void);         /* "" when fine */
bool qr_inner(void);

#endif
