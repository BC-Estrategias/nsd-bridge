/* Self-update from this project's GitHub releases (3DS only). Everything slow runs on a worker thread so the
 * bridge keeps answering and the screen keeps drawing. Nothing is ever installed without the person pressing the
 * update row on the console; downloads are checked against the SHA-256 and size GitHub reports for the asset. */
#ifndef UPDATER_H
#define UPDATER_H

#include <stdbool.h>

typedef enum {
  UPD_IDLE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE,
  UPD_DOWNLOADING, UPD_VERIFYING, UPD_INSTALLING, UPD_DONE, UPD_ERROR
} upd_state;

typedef struct {
  upd_state state;
  unsigned progress;  /* 0..100 while downloading/installing */
  unsigned revision;  /* bumps on every change: the UI redraws when it moves */
  char version[24];   /* the newest release found, e.g. "v1.7.0" */
  char message[64];   /* the reason for UPD_ERROR */
} upd_status;

/* `launch_path` is argv[0] (used to replace the .3dsx when the app was started from the Homebrew Launcher). */
void updater_init(const char *launch_path);
void updater_check(void);     /* looks for a newer release (no-op while busy) */
void updater_download(void);  /* downloads, verifies and installs the release found by the last check */
void updater_get(upd_status *out);
bool updater_busy(void);
void updater_shutdown(void);

#endif
