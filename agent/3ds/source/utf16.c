#include "utf16.h"

size_t nsd_utf16_to_utf8(const uint16_t *in, size_t max_units, char *out, size_t cap) {
  size_t i = 0, o = 0;
  if (!cap) return 0;
  while (i < max_units && in[i]) {
    uint32_t cp = in[i++];
    uint8_t b[4];
    size_t n;
    if (cp >= 0xD800 && cp <= 0xDBFF && i < max_units && in[i] >= 0xDC00 && in[i] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i++] - 0xDC00);
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = '?';
    }
    if (cp < 0x80) { b[0] = (uint8_t)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (uint8_t)(0xC0 | (cp >> 6)); b[1] = (uint8_t)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (uint8_t)(0xE0 | (cp >> 12)); b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (uint8_t)(0x80 | (cp & 0x3F)); n = 3; }
    else { b[0] = (uint8_t)(0xF0 | (cp >> 18)); b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (uint8_t)(0x80 | (cp & 0x3F)); n = 4; }
    if (o + n >= cap) break;
    for (size_t k = 0; k < n; k++) out[o++] = (char)b[k];
  }
  out[o] = '\0';
  return o;
}
