#include "dlmap.h"

#include <string.h>

#define BIT(a, i) ((a)[(i) >> 3] & (1u << ((i) & 7)))
#define SETBIT(a, i) ((a)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7)))
#define CLRBIT(a, i) ((a)[(i) >> 3] &= (uint8_t)~(1u << ((i) & 7)))

bool dlmap_init(dlmap *m, uint64_t total, const char *tag) {
  memset(m, 0, sizeof *m);
  if (total == 0 || total > (uint64_t)DL_MAX_CHUNKS * DL_CHUNK) return false;
  m->total = total;
  m->nchunks = (uint32_t)((total + DL_CHUNK - 1) / DL_CHUNK);
  strncpy(m->tag, tag ? tag : "", DL_TAG_MAX - 1);
  return true;
}
bool dlmap_claim(dlmap *m, uint32_t *idx) {
  uint32_t i;
  for (i = 0; i < m->nchunks; i++)
    if (!BIT(m->done, i) && !BIT(m->busy, i)) { SETBIT(m->busy, i); *idx = i; return true; }
  return false;
}
void dlmap_release(dlmap *m, uint32_t idx) { if (idx < m->nchunks) CLRBIT(m->busy, idx); }
void dlmap_complete(dlmap *m, uint32_t idx) {
  if (idx >= m->nchunks || BIT(m->done, idx)) return;
  SETBIT(m->done, idx);
  CLRBIT(m->busy, idx);
  m->ndone++;
}
bool dlmap_finished(const dlmap *m) { return m->ndone == m->nchunks; }
uint64_t dlmap_chunk_off(const dlmap *m, uint32_t idx) { (void)m; return (uint64_t)idx * DL_CHUNK; }
uint64_t dlmap_chunk_len(const dlmap *m, uint32_t idx) {
  uint64_t off = dlmap_chunk_off(m, idx);
  return m->total - off < DL_CHUNK ? m->total - off : DL_CHUNK;
}
uint64_t dlmap_done_bytes(const dlmap *m) {
  uint64_t b = 0;
  uint32_t i;
  for (i = 0; i < m->nchunks; i++) if (BIT(m->done, i)) b += dlmap_chunk_len(m, i);
  return b;
}

/* file: "NDLM1\0\0\0" | total (u64 LE) | nchunks (u32 LE) | taglen (u32 LE) | tag | bitmap */
size_t dlmap_save(const dlmap *m, uint8_t *out, size_t cap) {
  size_t tl = strlen(m->tag), bl = (m->nchunks + 7) / 8, n = 8 + 8 + 4 + 4 + tl + bl, i;
  if (cap < n) return 0;
  memcpy(out, "NDLM1\0\0\0", 8);
  for (i = 0; i < 8; i++) out[8 + i] = (uint8_t)(m->total >> (8 * i));
  for (i = 0; i < 4; i++) out[16 + i] = (uint8_t)(m->nchunks >> (8 * i));
  for (i = 0; i < 4; i++) out[20 + i] = (uint8_t)((uint32_t)tl >> (8 * i));
  memcpy(out + 24, m->tag, tl);
  memcpy(out + 24 + tl, m->done, bl);
  return n;
}
bool dlmap_load(dlmap *m, const uint8_t *in, size_t n, uint64_t total, const char *tag) {
  uint64_t t = 0;
  uint32_t nc = 0, tl = 0, i, done = 0;
  dlmap fresh;
  if (!dlmap_init(&fresh, total, tag)) { memset(m, 0, sizeof *m); return false; }
  *m = fresh;
  if (n < 24 || memcmp(in, "NDLM1\0\0\0", 8) != 0) return false;
  for (i = 0; i < 8; i++) t |= (uint64_t)in[8 + i] << (8 * i);
  for (i = 0; i < 4; i++) nc |= (uint32_t)in[16 + i] << (8 * i);
  for (i = 0; i < 4; i++) tl |= (uint32_t)in[20 + i] << (8 * i);
  if (t != total || nc != fresh.nchunks || tl >= DL_TAG_MAX || n != 24 + (size_t)tl + (nc + 7) / 8) return false;
  if (tl != strlen(fresh.tag) || memcmp(in + 24, fresh.tag, tl) != 0) return false;
  memcpy(m->done, in + 24 + tl, (nc + 7) / 8);
  for (i = 0; i < nc; i++) if (BIT(m->done, i)) done++;
  /* bits past the last piece must be clear */
  for (i = nc; i < ((nc + 7) / 8) * 8; i++) if (BIT(m->done, i)) { *m = fresh; return false; }
  m->ndone = done;
  return true;
}
