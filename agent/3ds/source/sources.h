#ifndef NSD_SOURCES_H
#define NSD_SOURCES_H
#ifndef AGENT_TEXT_UI
/* Fontes: browse an arbitrary folder tree on the owner's own Nextcloud/ownCloud public share and download one chosen file to a
 * folder opened for writing on the console. Unlike Translations/Mods, this expects no catalog layout: it is a plain file
 * browser over WebDAV. Everything that touches the network or writes files runs on one worker thread. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char name[200]; /* percent-decoded */
  bool is_dir;
  uint64_t size; /* files only */
} src_entry;

typedef struct {
  char rel[280]; /* relative to the folder src_prepare_folder() was given: the same sub-path is recreated under the destination */
  uint64_t size;
} src_batch_ent;

typedef enum { SRS_IDLE, SRS_LISTING, SRS_PREPARING, SRS_READY, SRS_DOWNLOADING, SRS_DONE, SRS_ERROR } src_state;
typedef enum { SRE_NONE, SRE_NET, SRE_IO, SRE_SPACE, SRE_CANCELLED, SRE_EXISTS, SRE_SIZE, SRE_NOFILES, SRE_TOOBIG, SRE_BADPATH } src_error;

typedef struct {
  src_state state;
  unsigned revision;
  src_error error;
  int net_code;
  long http_code;
  char detail[160];
  char name[200]; /* the file being downloaded (a single download), or the folder being gathered/sent (a batch) */
  uint64_t bytes_done, bytes_total;
  unsigned files_done, files_total; /* a batch download only; both 0 for a single file */
  unsigned bps, eta_s;
} src_status;

void src_init(void);
void src_shutdown(void);

bool src_configured(void);
const char *src_link(void);
bool src_set_link(const char *url); /* validates, saves in config/sources-url, forgets the current listing */
void src_forget_link(void); /* removes config/sources-url: back to "no source set" */

void src_list(const char *path); /* path relative to the share's root ("" = root): replaces the current listing */
bool src_entry_url(int idx, char *out, size_t cap); /* the full, authenticated URL of src_at(idx) (a file); false if idx is a folder */
const char *src_cur_path(void);
int src_count(void);
const src_entry *src_at(int i);

void src_get(src_status *out);
void src_cancel(void);
void src_dismiss(void); /* leaves SRS_DONE / SRS_ERROR: back to SRS_READY */
bool src_busy(void);

/* Downloads src_at(idx) (must be a file of the folder src_list() last listed) to dest_dir/dest_name, both plain "sdmc:" paths
 * the caller has already checked are open for writing. Refuses to overwrite an existing file. */
void src_download(int idx, const char *dest_dir, const char *dest_name);

/* A whole folder: src_at(idx) must be a folder of the listing src_list() last showed. Recursively reads it (state goes to
 * SRS_PREPARING, then SRS_READY with the count/size in src_get(), or SRS_ERROR); hidden and junk files are skipped the same
 * way a single folder listing skips them. Limits: 20000 files, 1.5 GiB. */
void src_prepare_folder(int idx);
int src_batch_count(void);
const src_batch_ent *src_batch_at(int i);
uint64_t src_batch_bytes(void);
/* Downloads everything src_prepare_folder() gathered into dest_dir (a plain "sdmc:" path already checked as open for
 * writing), recreating the same sub-folders. Skips a file that is already there with the same size; refuses to overwrite one
 * with a different size (SRE_EXISTS, named in detail) rather than guess which copy is right. */
void src_download_batch(const char *dest_dir);
#endif
#endif
