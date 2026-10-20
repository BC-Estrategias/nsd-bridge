#ifndef NSD_TRANSLATIONS_H
#define NSD_TRANSLATIONS_H
#ifndef AGENT_TEXT_UI
/* Translations: reads a catalog of Luma3DS LayeredFS translation packs from the owner's own Nextcloud public link and
 * installs / removes them under /luma/titles/<ID>/. The folder layout, the safety rules and the saved records are described in
 * docs/translations.md. Everything that touches the network or writes files runs on one worker thread. */
#include <stdbool.h>
#include <stdint.h>

#include "trpack.h"

typedef struct {
  tr_ref ref;
  char path[320];        /* on the share, e.g. "traducoes/ptbr/0004000000125500 - Name/team/v1.0" */
  int mark;              /* 0 none, 1 a folder exists in /luma/titles/<ID> put there by hand, 2 installed by NSD Bridge */
  char inst_version[16]; /* mark 2: the installed version */
  bool update;           /* mark 2: this pack is newer than what is installed */
} tr_pack;

typedef enum { TRS_IDLE, TRS_LISTING, TRS_READY, TRS_PREPARING, TRS_CONFIRM, TRS_WORKING, TRS_DONE, TRS_ERROR } trs_state;
typedef enum { TRE_NONE, TRE_NET, TRE_NOFILES, TRE_BADPATH, TRE_TOOBIG, TRE_SPACE, TRE_IO, TRE_CANCELLED, TRE_SIZE } tr_error;

typedef struct {
  trs_state state;
  unsigned revision;      /* bumps on every change */
  int job;                /* 0 install, 1 uninstall */
  int pack;               /* index in the catalog */
  int list_requests;      /* while listing: requests made so far */
  char team[64], region[24], source[200];   /* from pacote.json, may be empty */
  int title_known;        /* 0 unknown, 1 installed on this console, 2 not installed */
  unsigned files_total, files_done;
  uint64_t bytes_total, bytes_done;
  unsigned bps, eta_s;
  bool has_record;        /* uninstall: the pack left its own record, so exactly its files are removed */
  unsigned removed;       /* uninstall: files removed */
  tr_error error;
  int net_code;
  long http_code;
  char detail[120];       /* e.g. the path that was refused */
} tr_status;

void tr_init(void);
void tr_shutdown(void);

bool tr_configured(void);
const char *tr_link(void);
bool tr_set_link(const char *url);          /* validates, saves in config/translations-url, forgets the saved catalog */

void tr_list(bool force);                   /* reads the catalog (the saved copy unless force or there is none) */
int tr_count(void);
const tr_pack *tr_at(int i);
void tr_refresh_marks(void);                /* recomputes the stars from /luma/titles (quick, main thread) */

void tr_get(tr_status *out);
void tr_prepare(int idx, bool uninstall);   /* gathers the details and the file list, then waits in TRS_CONFIRM */
void tr_confirm(bool yes);                  /* answer to TRS_CONFIRM */
void tr_cancel(void);
void tr_dismiss(void);                      /* leave TRS_DONE / TRS_ERROR / TRS_CONFIRM: back to the list */
bool tr_busy(void);
#endif
#endif
