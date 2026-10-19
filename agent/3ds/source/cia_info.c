#include "cia_info.h"

#include <string.h>

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t be64(const uint8_t *p) {
  uint64_t v = 0;
  int i;
  for (i = 0; i < 8; i++) v = v << 8 | p[i];
  return v;
}
static uint64_t align64(uint64_t v) { return (v + 0x3F) & ~(uint64_t)0x3F; }

cia_class cia_classify(uint64_t id, int *on_nand) {
  unsigned platform = (unsigned)((id >> 48) & 0xFFFF), category = (unsigned)((id >> 32) & 0xFFFF), variation = (unsigned)(id & 0xFF);
  /* the destination rule FBI has used for years: DSi titles, TWL/system/DLP categories and "system title" 0x...02 go to NAND */
  int nand = platform == 0x0003 || (platform == 0x0004 && ((category & 0x8011) != 0 || (category == 0x0000 && variation == 0x02)));
  cia_class c = CIA_CLASS_APP;
  if (on_nand) *on_nand = nand;
  if (!nand) return CIA_CLASS_APP;
  if (platform == 0x0004 && category == 0x0138) return CIA_CLASS_FIRM;      /* NATIVE_FIRM, SAFE_MODE_FIRM, N3DS firmware... */
  if (platform == 0x0003 || (platform == 0x0004 && (category & 0x8000) && !(category & 0x0010))) c = CIA_CLASS_DSIWARE;
  else c = CIA_CLASS_SYSTEM;
  return c;
}

int cia_parse(const uint8_t *b, size_t len, cia_info *out) {
  static const uint32_t sig_total[6] = {0x240, 0x140, 0x80, 0x240, 0x140, 0x80}; /* type word + signature + padding */
  uint64_t header, cert, ticket, tmd, meta, content, tmd_off;
  uint32_t sig_type;
  const uint8_t *t;
  if (len < 0x20) return -2;
  header = le32(b);
  if (header != 0x2020) return -1; /* every CIA the 3DS makes has a 0x2020-byte header */
  cert = le32(b + 8);
  ticket = le32(b + 12);
  tmd = le32(b + 16);
  meta = le32(b + 20);
  content = le64(b + 24);
  if (cert == 0 || cert > 0x4000 || ticket == 0 || ticket > 0x1000 || tmd < 0x100 || tmd > 0x100000 || meta > 0x10000) return -1;
  tmd_off = align64(header) + align64(cert) + align64(ticket);
  if (len < tmd_off + 8) return -2;
  t = b + tmd_off;
  if (t[0] != 0x00 || t[1] != 0x01 || t[2] != 0x00 || t[3] > 5) return -1; /* signature type 0x010000..0x010005 */
  sig_type = t[3];
  if (len < tmd_off + sig_total[sig_type] + 0xA0) return -2;
  t += sig_total[sig_type];
  memset(out, 0, sizeof *out);
  out->title_id = be64(t + 0x4C);
  out->title_version = be16(t + 0x9C);
  out->content_count = be16(t + 0x9E);
  out->content_size = content;
  out->total_size = tmd_off + align64(tmd) + align64(meta) + align64(content);
  out->cls = cia_classify(out->title_id, &out->on_nand);
  out->new3ds_only = ((out->title_id >> 28) & 0xF) == 2;
  return 0;
}
