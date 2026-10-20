/* CIA installer (3DS): installs a title from a file on the SD card or from an http(s) URL, straight into AM (the system's
 * title manager). Everything slow runs on a worker thread. The flow is always: probe (read the start of the CIA) ->
 * CONFIRM (the console's owner sees what it is and where it goes, and presses A) -> install. What it checks that a plain
 * installer does not: the size the CIA header describes, an optional SHA-256 (before the install is finished, so a
 * mismatch installs nothing), and the HTTPS certificate. Design notes and the comparison with FBI: see CHANGELOG 1.8.0. */
#ifndef INSTALLER_H
#define INSTALLER_H

#include <stdbool.h>
#include <stdint.h>

#include "cia_info.h"

typedef enum { INS_IDLE, INS_PROBING, INS_CONFIRM, INS_INSTALLING, INS_DONE, INS_ERROR } ins_state;

typedef enum {
  INS_E_NONE, INS_E_CANCELLED, INS_E_READ,    /* cannot open/read the file */
  INS_E_NOT_CIA, INS_E_FIRM, INS_E_SELF,      /* not a CIA; firmware titles unsupported; this app itself (use Updates) */
  INS_E_SPACE, INS_E_SIZE, INS_E_HASH,        /* not enough room; size differs from the header; SHA-256 differs */
  INS_E_NET, INS_E_AM                         /* download failed; AM refused (see `am_result`) */
} ins_error;

typedef struct {
  ins_state state;
  unsigned revision;      /* bumps on every change: the UI redraws when it moves */
  int is_url;
  char source[200];       /* the file path, or the URL */
  char host[64];          /* URL: the domain to show the owner */
  cia_info info;
  int has_info;
  int replaces;           /* the title is already installed: it is removed first, then installed again */
  int hash_expected;      /* a SHA-256 came with the request and will be checked */
  uint64_t done, total;   /* bytes so far / expected (0 = unknown) */
  unsigned bps;
  unsigned eta_s;         /* estimated seconds left (smoothed), 0 = not known yet */
  int is_bench;           /* a speed test, not an install */
  int phase;              /* 0 streamed straight into the installer, 1 downloading to a file on the card, 2 installing from that file */
  uint64_t resume_bytes;  /* confirm screen: how much of this download is already on the card from before */
  int conns;              /* connections used by the download (phase 1) */
  unsigned bench_kib[4];  /* KiB/s of: easy, multi (50 ms), easy + big TCP window, multi + big TCP window; 0 = failed */
  int seed_pre;           /* checked before the download: -1 not applicable, 0 off, 1 available, 2 the title uses none, 3 could not be checked */
  int seed_src;           /* with seed_pre == 1: 1 the request, 2 /fbi/seed, 3 Nintendo's server */
  int seed;               /* after an install: 0 not tried, 1 registered, 2 the title needs none, 3 could not get it */
  char sha_hex[65];       /* the SHA-256 of what was installed, once it finished */
  ins_error error;
  uint32_t am_result;
  int net_code;           /* INS_E_NET: the curl result code (28 = timed out...) */
  long http_code;         /* INS_E_NET: the HTTP status when the server answered with an error */
} ins_status;

void installer_init(void);
void installer_shutdown(void);
bool installer_busy(void);          /* probing, waiting for confirmation, or installing */
void installer_get(ins_status *out);

/* Both return false if another install (or an update) is in progress. `sha256` may be NULL. */
bool installer_start_file(const char *sd_path, const uint8_t *sha256, const uint8_t *seed);  /* e.g. "sdmc:/cias/game.cia"; sha256 may be NULL */
bool installer_start_url(const char *url, const uint8_t *sha256, const uint8_t *seed);
void installer_confirm(bool yes);   /* answer to INS_CONFIRM */
void installer_cancel(void);        /* stop an install in progress */
void installer_dismiss(void);       /* leave INS_DONE / INS_ERROR: back to INS_IDLE */
void installer_import_seeds(char *msg, size_t cap);
void installer_pending(unsigned *count, uint64_t *bytes);  /* partial downloads kept on the card (for resuming) */
void installer_pending_clear(void);                       /* delete them all */  /* register the .dat files of /fbi/seed; msg gets "N registered, M failed" */

#endif
