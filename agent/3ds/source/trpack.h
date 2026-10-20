#ifndef NSD_TRPACK_H
#define NSD_TRPACK_H
/* The pure logic of the Translations feature (no console services, so a host test can run it): reading a WebDAV listing,
 * naming and validating packs, validating the paths a pack may write, comparing versions, a tiny JSON field reader for
 * pacote.json and the JSON the installer leaves in /luma/titles/<ID>/. See docs/translations.md. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- WebDAV PROPFIND (Depth: 1) answer ---- */
typedef struct {
  char name[160];   /* last path segment, percent-decoded */
  bool is_dir;
  uint64_t size;    /* files only */
} dav_entry;

/* Parses a multistatus answer. The first <response> is the folder asked about and is skipped. Returns the entries stored. */
int dav_parse(const char *xml, size_t n, dav_entry *out, int max);
void url_decode(const char *in, size_t n, char *out, size_t cap);
/* Percent-encodes a path (keeps '/'), for building request URLs. */
void url_encode_path(const char *in, char *out, size_t cap);

/* ---- a pack is traducoes/<idioma>/<ID> - <Nome>/<equipe>/<versao>/ ---- */
typedef struct {
  char lang[12];      /* "ptbr", "en" */
  char id[17];        /* 16 hex digits, upper case */
  char game[96];
  char team[64];
  char version[16];   /* "v1.2" */
} tr_ref;

bool tr_parse_ref(const char *lang, const char *id_and_name, const char *team, const char *version, tr_ref *out);
int tr_version_cmp(const char *a, const char *b);   /* v1.10 > v1.9; -1, 0, 1 */

/* ---- what a pack may write ---- */
bool tr_path_ok(const char *rel);          /* a relative path under /luma/titles/<ID>/: no "..", no absolute, no odd characters */
bool tr_skip_name(const char *name);       /* macOS and Windows leftovers: ._x, .DS_Store, Thumbs.db, desktop.ini */

/* ---- pacote.json (optional) and the installed-pack record ---- */
bool tr_json_get(const char *json, const char *key, char *out, size_t cap);   /* a top-level string value; false if absent */
size_t tr_json_escape(const char *in, char *out, size_t cap);                 /* JSON string body (no quotes); returns length */
#endif
