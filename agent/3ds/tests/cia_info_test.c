/* Host test: cc -I source tests/cia_info_test.c source/cia_info.c  (optionally pass a real .cia to check) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cia_info.h"

static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_le64(uint8_t *p, uint64_t v) { put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t al(uint64_t v) { return (v + 0x3F) & ~(uint64_t)0x3F; }

/* a synthetic CIA: header, cert, ticket, TMD (signature type `sig`), then nothing (only the prefix is parsed) */
static size_t make(uint8_t *buf, uint32_t sig, uint64_t id, uint16_t ver, uint16_t contents, uint64_t content_size) {
  static const uint32_t sig_total[6] = {0x240, 0x140, 0x80, 0x240, 0x140, 0x80};
  uint32_t cert = 0xA00, ticket = 0x350, tmd = sig_total[sig] + 0xB04 + 0x30 * contents, meta = 0x3AC0;
  size_t off, tmd_off;
  uint8_t *t;
  int i;
  memset(buf, 0, CIA_PROBE_BYTES);
  put_le32(buf, 0x2020);
  put_le32(buf + 8, cert);
  put_le32(buf + 12, ticket);
  put_le32(buf + 16, tmd);
  put_le32(buf + 20, meta);
  put_le64(buf + 24, content_size);
  tmd_off = al(0x2020) + al(cert) + al(ticket);
  t = buf + tmd_off;
  t[0] = 0; t[1] = 1; t[2] = 0; t[3] = (uint8_t)sig;
  off = sig_total[sig];
  for (i = 0; i < 8; i++) t[off + 0x4C + i] = (uint8_t)(id >> (56 - 8 * i));
  t[off + 0x9C] = ver >> 8; t[off + 0x9D] = (uint8_t)ver;
  t[off + 0x9E] = contents >> 8; t[off + 0x9F] = (uint8_t)contents;
  return tmd_off + off + 0xA0;
}

int main(int argc, char **argv) {
  static uint8_t buf[CIA_PROBE_BYTES];
  cia_info ci;
  size_t need;
  int sig;

  /* every signature type finds the same fields */
  for (sig = 0; sig < 6; sig++) {
    need = make(buf, (uint32_t)sig, 0x000400000BD00100ull, 0x4401, 1, 0x123456);
    assert(cia_parse(buf, need, &ci) == 0);
    assert(ci.title_id == 0x000400000BD00100ull && ci.title_version == 0x4401 && ci.content_count == 1 && ci.content_size == 0x123456);
    assert(ci.cls == CIA_CLASS_APP && !ci.on_nand && !ci.new3ds_only);
    assert(ci.total_size > ci.content_size);
  }
  /* too short to tell: ask for more, never guess */
  need = make(buf, 4, 0x000400000BD00100ull, 1, 1, 100);
  assert(cia_parse(buf, 10, &ci) == -2);
  assert(cia_parse(buf, need - 1, &ci) == -2);
  assert(cia_parse(buf, need, &ci) == 0);
  /* not a CIA */
  buf[0] = 0x21;
  assert(cia_parse(buf, need, &ci) == -1);
  memset(buf, 'A', sizeof buf);
  assert(cia_parse(buf, sizeof buf, &ci) == -1);
  need = make(buf, 4, 1, 1, 1, 100);
  buf[al(0x2020) + al(0xA00) + al(0x350) + 3] = 9; /* an unknown signature type */
  assert(cia_parse(buf, sizeof buf, &ci) == -1);
  need = make(buf, 4, 1, 1, 1, 100);
  put_le32(buf + 8, 0x40000000u); /* absurd certificate size */
  assert(cia_parse(buf, sizeof buf, &ci) == -1);

  /* classification */
  {
    int nand;
    assert(cia_classify(0x0004000000123400ull, &nand) == CIA_CLASS_APP && !nand);             /* application */
    assert(cia_classify(0x0004000E00123400ull, &nand) == CIA_CLASS_APP && !nand);             /* update patch */
    assert(cia_classify(0x0004008C00123400ull, &nand) == CIA_CLASS_APP && !nand);             /* DLC */
    assert(cia_classify(0x0004000200123400ull, &nand) == CIA_CLASS_APP && !nand);             /* demo */
    assert(cia_classify(0x0004800400123400ull, &nand) == CIA_CLASS_DSIWARE && nand);          /* TWL application */
    assert(cia_classify(0x0003000400123400ull, &nand) == CIA_CLASS_DSIWARE && nand);          /* DSiWare */
    assert(cia_classify(0x0004001000020000ull, &nand) == CIA_CLASS_SYSTEM && nand);           /* system application (Home Menu) */
    assert(cia_classify(0x0004003000008F02ull, &nand) == CIA_CLASS_SYSTEM && nand);           /* system applet */
    assert(cia_classify(0x0004013000001702ull, &nand) == CIA_CLASS_SYSTEM && nand);           /* system module */
    assert(cia_classify(0x0004013800000002ull, &nand) == CIA_CLASS_FIRM && nand);             /* NATIVE_FIRM */
    assert(cia_classify(0x0004013820000002ull, &nand) == CIA_CLASS_FIRM && nand);             /* N3DS NATIVE_FIRM */
    /* "needs a New 3DS" */
    need = make(buf, 4, 0x0004000020000100ull, 1, 1, 100);
    assert(cia_parse(buf, need, &ci) == 0 && ci.new3ds_only);
  }

  /* a real CIA given on the command line: the file size must match what the header describes */
  if (argc > 1) {
    FILE *f = fopen(argv[1], "rb");
    long size;
    size_t n;
    assert(f);
    n = fread(buf, 1, sizeof buf, f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    assert(cia_parse(buf, n, &ci) == 0);
    printf("%s: title %016llX version 0x%04X, %s, %llu bytes (header says %llu)\n", argv[1], (unsigned long long)ci.title_id, ci.title_version,
           ci.on_nand ? "NAND" : "SD", (unsigned long long)size, (unsigned long long)ci.total_size);
    assert((uint64_t)size == ci.total_size);
  }
  puts("cia_info: all ok");
  return 0;
}
