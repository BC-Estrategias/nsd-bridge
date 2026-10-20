#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dlmap.h"

int main(void) {
  static dlmap m, r;
  static uint8_t buf[DLMAP_FILE_MAX];
  uint32_t a, b, c, i;
  size_t n;
  uint64_t total = 3u * DL_CHUNK + 12345; /* 4 pieces, the last one short */

  assert(dlmap_init(&m, total, "tag-1"));
  assert(m.nchunks == 4);
  assert(dlmap_chunk_len(&m, 0) == DL_CHUNK && dlmap_chunk_len(&m, 3) == 12345);
  assert(dlmap_chunk_off(&m, 3) == 3u * DL_CHUNK);
  assert(!dlmap_init(&r, 0, "x") && !dlmap_init(&r, (uint64_t)DL_MAX_CHUNKS * DL_CHUNK + 1, "x"));
  assert(dlmap_init(&r, (uint64_t)DL_MAX_CHUNKS * DL_CHUNK, "x") && r.nchunks == DL_MAX_CHUNKS);

  /* claiming hands out each piece once, lowest first */
  assert(dlmap_claim(&m, &a) && a == 0);
  assert(dlmap_claim(&m, &b) && b == 1);
  assert(dlmap_claim(&m, &c) && c == 2);
  dlmap_release(&m, b); /* a fetch failed: it can be claimed again */
  assert(dlmap_claim(&m, &b) && b == 1);
  assert(dlmap_claim(&m, &i) && i == 3);
  assert(!dlmap_claim(&m, &i)); /* nothing left to hand out while the others are busy */
  dlmap_complete(&m, 0);
  dlmap_complete(&m, 0); /* twice is harmless */
  assert(m.ndone == 1 && !dlmap_finished(&m));
  assert(dlmap_done_bytes(&m) == DL_CHUNK);
  dlmap_complete(&m, 3);
  assert(dlmap_done_bytes(&m) == DL_CHUNK + 12345);

  /* save / load: only done pieces survive, busy ones come back as free */
  n = dlmap_save(&m, buf, sizeof buf);
  assert(n > 0 && dlmap_save(&m, buf, 10) == 0);
  assert(dlmap_load(&r, buf, n, total, "tag-1"));
  assert(r.ndone == 2 && dlmap_done_bytes(&r) == DL_CHUNK + 12345);
  assert(dlmap_claim(&r, &i) && i == 1);

  /* a different file (size or tag) or a damaged map is not trusted: a fresh, empty map */
  assert(!dlmap_load(&r, buf, n, total + 1, "tag-1") && r.ndone == 0 && r.total == total + 1);
  assert(!dlmap_load(&r, buf, n, total, "tag-2") && r.ndone == 0);
  assert(!dlmap_load(&r, buf, n - 1, total, "tag-1") && r.ndone == 0);
  buf[0] = 'X';
  assert(!dlmap_load(&r, buf, n, total, "tag-1") && r.ndone == 0);
  buf[0] = 'N';
  buf[n - 1] |= 0x80; /* a bit set past the last piece */
  assert(!dlmap_load(&r, buf, n, total, "tag-1") && r.ndone == 0);

  /* finishing */
  dlmap_complete(&m, 1);
  dlmap_complete(&m, 2);
  assert(dlmap_finished(&m) && dlmap_done_bytes(&m) == total);
  assert(!dlmap_claim(&m, &i));
  printf("dlmap: all ok\n");
  return 0;
}
