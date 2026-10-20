#ifndef NSD_DLMAP_H
#define NSD_DLMAP_H
/* Which 1 MiB pieces of a download are already on the card. Pure C (no console services) so a host test can run it.
 * A piece is "done" only after it was written completely; "busy" pieces are being fetched by a connection right now. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DL_CHUNK (1024u * 1024u)
#define DL_MAX_CHUNKS 4096u /* 4 GiB: larger files are not downloaded in pieces */
#define DL_TAG_MAX 96

typedef struct {
  uint64_t total;
  uint32_t nchunks, ndone;
  char tag[DL_TAG_MAX]; /* identifies the exact file on the server (URL hash + validators): a resume only counts if it matches */
  uint8_t done[DL_MAX_CHUNKS / 8];
  uint8_t busy[DL_MAX_CHUNKS / 8];
} dlmap;

bool dlmap_init(dlmap *m, uint64_t total, const char *tag);   /* false if total is 0 or too large */
bool dlmap_claim(dlmap *m, uint32_t *idx);                    /* lowest piece that is neither done nor busy */
void dlmap_release(dlmap *m, uint32_t idx);                   /* a fetch failed: the piece goes back */
void dlmap_complete(dlmap *m, uint32_t idx);
bool dlmap_finished(const dlmap *m);
uint64_t dlmap_done_bytes(const dlmap *m);
uint64_t dlmap_chunk_off(const dlmap *m, uint32_t idx);
uint64_t dlmap_chunk_len(const dlmap *m, uint32_t idx);

#define DLMAP_FILE_MAX (32 + DL_TAG_MAX + DL_MAX_CHUNKS / 8)
size_t dlmap_save(const dlmap *m, uint8_t *out, size_t cap);   /* bytes written, 0 if cap is too small */
/* Loads a saved map; it only counts if total and tag are the ones asked for (else `m` is left as a fresh map). */
bool dlmap_load(dlmap *m, const uint8_t *in, size_t n, uint64_t total, const char *tag);
#endif
