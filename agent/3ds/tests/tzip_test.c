#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "tzip.h"

static uint8_t *slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  uint8_t *b;
  assert(f);
  fseek(f, 0, SEEK_END);
  *n = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  b = malloc(*n + 1);
  assert(fread(b, 1, *n, f) == *n);
  fclose(f);
  return b;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "agent/3ds/tests/data/pack_sample.zip";
  size_t n, pos = 0;
  uint8_t *z = slurp(path, &n);
  uint32_t off, size, count;
  tzip_ent e;
  int r, seen = 0, deflated = 0, stored = 0;

  assert(tzip_eocd(z, n, n, &off, &size, &count) == 0);
  assert(count == 7 && off + size < n);
  /* the same answer when only the tail is given */
  {
    uint32_t o2, s2, c2;
    size_t t = n > 40 ? 40 : n;
    assert(tzip_eocd(z + n - t, t, n, &o2, &s2, &c2) == 0 && o2 == off && s2 == size && c2 == count);
  }
  while ((r = tzip_next(z + off, size, &pos, &e)) == TZIP_ENTRY) {
    char name[64];
    snprintf(name, sizeof name, "%.*s", e.name_len, e.name);
    seen++;
    if (e.method == 8) deflated++; else stored++;
    if (!strcmp(name, "romfs/Message/a.msg")) {
      /* inflate it the way the console does and check the CRC and the size */
      uint8_t out[1024], *data;
      size_t hl;
      z_stream s;
      assert(tzip_local_len(z + e.lho, n - e.lho, &hl) == 0);
      data = z + e.lho + hl;
      memset(&s, 0, sizeof s);
      assert(inflateInit2(&s, -15) == Z_OK);
      s.next_in = data;
      s.avail_in = e.csize;
      s.next_out = out;
      s.avail_out = sizeof out;
      assert(inflate(&s, Z_FINISH) == Z_STREAM_END);
      assert(s.total_out == e.usize && e.usize == 600 && crc32(0, out, (uInt)s.total_out) == e.crc);
      inflateEnd(&s);
    }
    if (!strcmp(name, "romfs/Pok\xC3\xA9mon.bin")) assert(e.usize == 9 && e.method == 0);
  }
  assert(r == TZIP_END && seen == 7 && deflated >= 2 && stored >= 3 && pos == size);

  /* damaged inputs never read outside the buffer and are refused */
  assert(tzip_eocd(z, 10, n, &off, &size, &count) == TZIP_BAD);
  assert(tzip_eocd(z, n - 1, n, &off, &size, &count) == TZIP_BAD); /* the last byte (the comment length's) is gone */
  {
    uint8_t *c = malloc(n);
    uint32_t o, s, k;
    size_t p2 = 0;
    memcpy(c, z, n);
    assert(tzip_eocd(c, n, n, &o, &s, &k) == 0);
    /* a name length that points beyond the directory */
    c[o + 28] = 0xFF;
    c[o + 29] = 0xFF;
    assert(tzip_next(c + o, s, &p2, &e) == TZIP_BAD);
    memcpy(c, z, n);
    c[o + 8] |= 1; /* encrypted */
    p2 = 0;
    assert(tzip_next(c + o, s, &p2, &e) == TZIP_UNSUPPORTED);
    memcpy(c, z, n);
    c[o + 10] = 12; /* bzip2 */
    p2 = 0;
    assert(tzip_next(c + o, s, &p2, &e) == TZIP_UNSUPPORTED);
    memcpy(c, z, n);
    c[o + 20] = c[o + 21] = c[o + 22] = c[o + 23] = 0xFF; /* zip64 marker */
    p2 = 0;
    assert(tzip_next(c + o, s, &p2, &e) == TZIP_UNSUPPORTED);
    memcpy(c, z, n);
    c[o] = 'X'; /* not a central directory entry */
    p2 = 0;
    assert(tzip_next(c + o, s, &p2, &e) == TZIP_BAD);
    /* several disks and zip64 in the end record */
    memcpy(c, z, n);
    c[n - 22 + 4] = 1;
    assert(tzip_eocd(c, n, n, &o, &s, &k) == TZIP_UNSUPPORTED);
    memcpy(c, z, n);
    c[n - 22 + 10] = c[n - 22 + 11] = 0xFF;
    c[n - 22 + 8] = c[n - 22 + 9] = 0xFF;
    assert(tzip_eocd(c, n, n, &o, &s, &k) == TZIP_UNSUPPORTED);
    free(c);
  }
  {
    uint8_t lh[30] = {0};
    size_t hl;
    assert(tzip_local_len(lh, 30, &hl) == TZIP_BAD && tzip_local_len(lh, 5, &hl) == TZIP_BAD);
  }
  free(z);
  printf("tzip: all ok\n");
  return 0;
}
