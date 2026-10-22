#ifndef NSD_TZIP_H
#define NSD_TZIP_H
/* A small zip reader for pack files (no console services, so a host test can run it): finds the central directory and walks its
 * entries. Only what packs need: stored or deflate, one disk, no encryption, no zip64. The bytes are never trusted: every length is
 * checked against the buffer it points into. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *name;       /* points into the central directory buffer, NOT NUL-terminated */
  uint16_t name_len;
  uint32_t lho;           /* offset of the local header */
  uint32_t csize, usize, crc;
  uint16_t method;        /* 0 stored, 8 deflate */
} tzip_ent;

enum { TZIP_END = 0, TZIP_ENTRY = 1, TZIP_BAD = -1, TZIP_UNSUPPORTED = -2 };

/* tail: the last bytes of the file (at least the last 22, at most 65557 are looked at). Returns 0 and the central directory's place,
 * or TZIP_BAD / TZIP_UNSUPPORTED. */
int tzip_eocd(const uint8_t *tail, size_t n, uint64_t file_size, uint32_t *cd_off, uint32_t *cd_size, uint32_t *count);
/* The next entry of the central directory (pos starts at 0). TZIP_ENTRY, TZIP_END, or an error. */
int tzip_next(const uint8_t *cd, size_t cd_size, size_t *pos, tzip_ent *e);
/* The 30 bytes of a local header (or more): the length of the header with its name and extra field, so the data starts after it. */
int tzip_local_len(const uint8_t *lh, size_t n, size_t *hdr_len);
#endif
