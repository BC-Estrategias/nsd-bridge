#include "tzip.h"

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int tzip_eocd(const uint8_t *tail, size_t n, uint64_t file_size, uint32_t *cd_off, uint32_t *cd_size, uint32_t *count) {
  size_t i;
  if (!tail || n < 22) return TZIP_BAD;
  for (i = n - 22 + 1; i-- > 0;) {
    const uint8_t *p = tail + i;
    uint16_t entries, total, comment;
    uint32_t size, off;
    uint64_t eocd_pos;
    if (u32(p) != 0x06054b50u) continue;
    comment = u16(p + 20);
    if (i + 22 + comment != n) continue; /* not at the very end: a signature inside a comment or data */
    if (u16(p + 4) != 0 || u16(p + 6) != 0) return TZIP_UNSUPPORTED; /* several disks */
    entries = u16(p + 8);
    total = u16(p + 10);
    size = u32(p + 12);
    off = u32(p + 16);
    if (entries != total) return TZIP_BAD;
    if (total == 0xFFFFu || size == 0xFFFFFFFFu || off == 0xFFFFFFFFu) return TZIP_UNSUPPORTED; /* zip64 */
    eocd_pos = file_size - 22 - comment;
    if (file_size < n || (uint64_t)off + size > eocd_pos) return TZIP_BAD;
    *cd_off = off;
    *cd_size = size;
    *count = total;
    return 0;
  }
  return TZIP_BAD;
}

int tzip_next(const uint8_t *cd, size_t cd_size, size_t *pos, tzip_ent *e) {
  const uint8_t *p;
  size_t nlen, elen, clen;
  uint16_t flags;
  if (*pos == cd_size) return TZIP_END;
  if (*pos > cd_size || cd_size - *pos < 46) return TZIP_BAD;
  p = cd + *pos;
  if (u32(p) != 0x02014b50u) return TZIP_BAD;
  flags = u16(p + 8);
  e->method = u16(p + 10);
  e->crc = u32(p + 16);
  e->csize = u32(p + 20);
  e->usize = u32(p + 24);
  nlen = u16(p + 28);
  elen = u16(p + 30);
  clen = u16(p + 32);
  e->lho = u32(p + 42);
  if (cd_size - *pos - 46 < nlen + elen + clen) return TZIP_BAD;
  if ((flags & 0x41) || (e->method != 0 && e->method != 8)) return TZIP_UNSUPPORTED; /* encrypted, or a method other than stored / deflate */
  if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || e->lho == 0xFFFFFFFFu) return TZIP_UNSUPPORTED; /* zip64 */
  if (e->method == 0 && e->csize != e->usize) return TZIP_BAD;
  e->name = (const char *)(p + 46);
  e->name_len = (uint16_t)nlen;
  *pos += 46 + nlen + elen + clen;
  return TZIP_ENTRY;
}

int tzip_local_len(const uint8_t *lh, size_t n, size_t *hdr_len) {
  if (n < 30 || u32(lh) != 0x04034b50u) return TZIP_BAD;
  *hdr_len = 30u + u16(lh + 26) + u16(lh + 28);
  return 0;
}
