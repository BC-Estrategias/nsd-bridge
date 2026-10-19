#ifndef AGENT_TEXT_UI
#include "qr.h"

#include <3ds.h>
#include <citro2d.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alog.h"
#include "quirc.h"

#define W 400
#define H 240
#define TEX_W 512
#define TEX_H 256

static LightLock g_lock;
static bool g_lock_init;
static Thread g_thread;
static Handle g_cancel_ev;
static volatile bool g_running, g_found, g_inner;
static char g_text[1100], g_err[64];
static u16 *g_preview;        /* latest frame, RGB565 400x240 */
static unsigned g_serial, g_drawn_serial;
static C3D_Tex g_tex;
static bool g_tex_ok;
static struct quirc_code g_code;
static struct quirc_data g_data;

bool qr_active(void) { return g_running; }
bool qr_inner(void) { return g_inner; }
const char *qr_error(void) { return g_err; }
unsigned qr_frame_serial(void) { return g_serial; }

/* Camera frames are RGB565; quirc wants grey. */
static void to_grey(const u16 *src, uint8_t *dst, int w, int h) {
  int i, n = w * h;
  for (i = 0; i < n; i++) {
    u16 px = src[i];
    dst[i] = (uint8_t)(((((px >> 11) & 0x1F) << 3) + (((px >> 5) & 0x3F) << 2) + ((px & 0x1F) << 3)) / 3);
  }
}

static void scan_thread(void *arg) {
  Handle events[3] = {0, 0, 0}; /* cancel, frame received, buffer error */
  u32 buf_size = W * H * sizeof(u16), transfer = 0;
  u16 *buf = calloc(1, buf_size);
  struct quirc *q = quirc_new();
  Result res = 0;
  bool cam_ok = false;
  (void)arg;
  events[0] = g_cancel_ev;
  if (!buf || !q || quirc_resize(q, W, H) != 0) { snprintf(g_err, sizeof g_err, "no memory"); goto done; }
  if (R_FAILED(res = camInit())) { snprintf(g_err, sizeof g_err, "camera 0x%08lX", (unsigned long)res); goto done; }
  cam_ok = true;
  {
    u32 cam = g_inner ? SELECT_IN1 : SELECT_OUT1;
    if (R_SUCCEEDED(res = CAMU_SetSize(cam, SIZE_CTR_TOP_LCD, CONTEXT_A)) && R_SUCCEEDED(res = CAMU_SetOutputFormat(cam, OUTPUT_RGB_565, CONTEXT_A)) &&
        R_SUCCEEDED(res = CAMU_SetFrameRate(cam, FRAME_RATE_30)) && R_SUCCEEDED(res = CAMU_SetNoiseFilter(cam, true)) &&
        R_SUCCEEDED(res = CAMU_SetAutoExposure(cam, true)) && R_SUCCEEDED(res = CAMU_SetAutoWhiteBalance(cam, true)) && R_SUCCEEDED(res = CAMU_Activate(cam)) &&
        R_SUCCEEDED(res = CAMU_GetBufferErrorInterruptEvent(&events[2], PORT_CAM1)) && R_SUCCEEDED(res = CAMU_SetTrimming(PORT_CAM1, true)) &&
        R_SUCCEEDED(res = CAMU_SetTrimmingParamsCenter(PORT_CAM1, W, H, 400, 240)) && R_SUCCEEDED(res = CAMU_GetMaxBytes(&transfer, W, H)) &&
        R_SUCCEEDED(res = CAMU_SetTransferBytes(PORT_CAM1, transfer, W, H)) && R_SUCCEEDED(res = CAMU_ClearBuffer(PORT_CAM1)) &&
        R_SUCCEEDED(res = CAMU_SetReceiving(&events[1], buf, PORT_CAM1, buf_size, (s16)transfer)) && R_SUCCEEDED(res = CAMU_StartCapture(PORT_CAM1))) {
      while (g_running && R_SUCCEEDED(res)) {
        s32 idx = 0;
        if (R_FAILED(res = svcWaitSynchronizationN(&idx, events, 3, false, U64_MAX))) break;
        if (idx == 0) break;
        svcCloseHandle(events[1]);
        events[1] = 0;
        if (idx == 1) {
          int w = 0, h = 0, n, i;
          uint8_t *grey;
          LightLock_Lock(&g_lock);
          memcpy(g_preview, buf, buf_size);
          g_serial++;
          LightLock_Unlock(&g_lock);
          grey = quirc_begin(q, &w, &h);
          to_grey(buf, grey, w, h);
          quirc_end(q);
          n = quirc_count(q);
          for (i = 0; i < n && !g_found; i++) {
            quirc_extract(q, i, &g_code);
            if (quirc_decode(&g_code, &g_data) == QUIRC_SUCCESS) {
              size_t len = 0;
              while (len < sizeof g_text - 1 && len < (size_t)g_data.payload_len && g_data.payload[len] != '\n' && g_data.payload[len] != '\r') {
                g_text[len] = (char)g_data.payload[len];
                len++;
              }
              g_text[len] = '\0';
              g_found = true;
            }
          }
          if (g_found) break;
          res = CAMU_SetReceiving(&events[1], buf, PORT_CAM1, buf_size, (s16)transfer);
        } else { /* buffer error: restart the transfer */
          if (R_SUCCEEDED(res = CAMU_ClearBuffer(PORT_CAM1)) && R_SUCCEEDED(res = CAMU_SetReceiving(&events[1], buf, PORT_CAM1, buf_size, (s16)transfer)))
            res = CAMU_StartCapture(PORT_CAM1);
        }
      }
      CAMU_StopCapture(PORT_CAM1);
      {
        bool busy = false;
        while (R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy) svcSleepThread(1000000);
      }
      CAMU_ClearBuffer(PORT_CAM1);
    } else {
      snprintf(g_err, sizeof g_err, "camera setup 0x%08lX", (unsigned long)res);
    }
    CAMU_Activate(SELECT_NONE);
  }
done:
  if (cam_ok) camExit();
  if (events[1]) svcCloseHandle(events[1]);
  if (events[2]) svcCloseHandle(events[2]);
  if (q) quirc_destroy(q);
  free(buf);
  g_running = false;
}

bool qr_start(bool inner) {
  if (g_running) return true;
  if (!g_lock_init) { LightLock_Init(&g_lock); g_lock_init = true; }
  if (g_thread) { threadJoin(g_thread, UINT64_MAX); threadFree(g_thread); g_thread = NULL; }
  if (!g_preview) g_preview = calloc(1, W * H * sizeof(u16));
  if (!g_tex_ok) g_tex_ok = C3D_TexInit(&g_tex, TEX_W, TEX_H, GPU_RGB565);
  if (!g_preview || !g_tex_ok) { snprintf(g_err, sizeof g_err, "no memory"); return false; }
  g_err[0] = '\0';
  g_found = false;
  g_text[0] = '\0';
  g_inner = inner;
  if (R_FAILED(svcCreateEvent(&g_cancel_ev, RESET_STICKY))) { snprintf(g_err, sizeof g_err, "event"); return false; }
  g_running = true;
  g_thread = threadCreate(scan_thread, NULL, 0x20000, 0x1A, 0, false);
  if (!g_thread) { g_running = false; svcCloseHandle(g_cancel_ev); snprintf(g_err, sizeof g_err, "thread"); return false; }
  alog("QR scanner started (%s camera)", inner ? "inner" : "outer");
  return true;
}

void qr_stop(void) {
  if (!g_thread) return;
  g_running = false;
  svcSignalEvent(g_cancel_ev);
  threadJoin(g_thread, UINT64_MAX);
  threadFree(g_thread);
  g_thread = NULL;
  svcCloseHandle(g_cancel_ev);
  g_cancel_ev = 0;
}

bool qr_take(char *out, size_t cap) {
  if (!g_found) return false;
  snprintf(out, cap, "%s", g_text);
  g_found = false;
  return true;
}

/* The preview texture is tiled (8x8 Morton blocks) and upside down in GPU space: the top row of the image is the highest row. */
static void tile_frame(void) {
  static uint8_t morton[8][8];
  static bool ready;
  u16 *dst = (u16 *)g_tex.data;
  int x, y;
  if (!ready) {
    for (y = 0; y < 8; y++)
      for (x = 0; x < 8; x++) {
        int m = 0, b;
        for (b = 0; b < 3; b++) m |= (((x >> b) & 1) << (2 * b)) | (((y >> b) & 1) << (2 * b + 1));
        morton[y][x] = (uint8_t)m;
      }
    ready = true;
  }
  for (y = 0; y < H; y++) {
    int ty = TEX_H - 1 - y; /* texture row, 0 = bottom */
    const u16 *row = g_preview + y * W;
    for (x = 0; x < W; x++) {
      int tile = (ty >> 3) * (TEX_W >> 3) + (x >> 3);
      dst[tile * 64 + morton[ty & 7][x & 7]] = row[x];
    }
  }
  GSPGPU_FlushDataCache(g_tex.data, TEX_W * TEX_H * sizeof(u16));
}

void qr_draw_preview(float x, float y) {
  static Tex3DS_SubTexture sub = {W, H, 0.0f, 1.0f, (float)W / TEX_W, 1.0f - (float)H / TEX_H};
  C2D_Image img;
  if (!g_tex_ok || !g_serial) return;
  LightLock_Lock(&g_lock);
  if (g_drawn_serial != g_serial) { tile_frame(); g_drawn_serial = g_serial; }
  LightLock_Unlock(&g_lock);
  img.tex = &g_tex;
  img.subtex = &sub;
  C2D_DrawImageAt(img, x, y, 0.4f, NULL, 1.0f, 1.0f);
}
#endif /* !AGENT_TEXT_UI */
