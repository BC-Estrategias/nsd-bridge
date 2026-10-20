#ifndef NSD_TITLES_H
#define NSD_TITLES_H
#ifndef AGENT_TEXT_UI
/* The installed titles (SD card, system memory, game card) and the interrupted installs ("pending titles"), read and
 * managed through the system's title manager (AM). Everything that talks to AM runs on one worker thread, so the screen and
 * the bridge keep answering; the UI reads the lists only while titles_state() == TS_READY. */
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { KIND_APP, KIND_UPDATE, KIND_DLC, KIND_DSIWARE, KIND_SYSTEM } title_kind;

typedef struct {
  uint64_t id, size;
  uint16_t version;
  uint8_t media;          /* FS_MediaType: 0 system memory (NAND), 1 SD card, 2 game card */
  uint8_t kind;           /* title_kind */
  bool twl;               /* DSiWare */
  char name[64];
  char publisher[48];
  char code[16];          /* product code, e.g. CTR-P-ABCD */
  uint16_t *icon;         /* 48x48 RGB565 in the console's tiled order (from the SMDH), or NULL */
} title_entry;

typedef struct {
  uint64_t id, size;
  uint16_t version;
  uint8_t media, status;  /* status: AM_TitleInstallStatus */
} pending_entry;

typedef enum { TS_IDLE, TS_LOADING, TS_READY, TS_BUSY } titles_state_t;

void titles_init(void);
void titles_shutdown(void);
void titles_load(void);                         /* (re)read the title list */
void titles_pending_load(void);                 /* (re)read the pending titles */
void titles_delete(int idx, bool with_ticket);  /* delete titles_at(idx) */
void titles_pending_delete(int idx);            /* idx < 0: all of them */

int titles_state(void);
unsigned titles_revision(void);                 /* bumps on every change */
int titles_count(void);
const title_entry *titles_at(int i);
int pending_count(void);
const pending_entry *pending_at(int i);
const char *titles_message(void);               /* outcome of the last delete: "" while none */
bool titles_message_ok(void);

bool titles_is_self(const title_entry *t);      /* NSD Bridge itself: never deleted from here */
bool titles_can_delete(const title_entry *t);   /* SD card titles and DSiWare; system titles are read-only */
Result titles_launch(int idx);                  /* main thread; on success the console jumps to the title */
#endif
#endif
