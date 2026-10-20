#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "utf16.h"

int main(void) {
  char out[16];
  const uint16_t ascii[] = {'Z', 'e', 'l', 'd', 'a', 0};
  const uint16_t acc[] = {'P', 0x00F3, 'k', 'e', 0};                  /* ó: 2 bytes */
  const uint16_t jp[] = {0x30BC, 0x30EB, 0};                          /* katakana: 3 bytes each */
  const uint16_t emoji[] = {0xD83C, 0xDFAE, 0};                       /* U+1F3AE: 4 bytes */
  const uint16_t lone[] = {'a', 0xD800, 'b', 0};
  const uint16_t nul_mid[] = {'a', 0, 'b', 0};
  assert(nsd_utf16_to_utf8(ascii, 64, out, sizeof out) == 5 && !strcmp(out, "Zelda"));
  assert(nsd_utf16_to_utf8(acc, 64, out, sizeof out) == 5 && !strcmp(out, "P\xC3\xB3ke"));
  assert(nsd_utf16_to_utf8(jp, 64, out, sizeof out) == 6 && !strcmp(out, "\xE3\x82\xBC\xE3\x83\xAB"));
  assert(nsd_utf16_to_utf8(emoji, 64, out, sizeof out) == 4 && !strcmp(out, "\xF0\x9F\x8E\xAE"));
  assert(nsd_utf16_to_utf8(lone, 64, out, sizeof out) == 3 && !strcmp(out, "a?b"));
  assert(nsd_utf16_to_utf8(nul_mid, 64, out, sizeof out) == 1 && !strcmp(out, "a"));
  assert(nsd_utf16_to_utf8(ascii, 3, out, sizeof out) == 3 && !strcmp(out, "Zel")); /* max units */
  /* never cut inside a character: 3-byte characters into a 5-byte buffer leave room for one only */
  assert(nsd_utf16_to_utf8(jp, 64, out, 5) == 3 && out[3] == '\0');
  assert(nsd_utf16_to_utf8(ascii, 64, out, 1) == 0 && out[0] == '\0');
  assert(nsd_utf16_to_utf8(ascii, 64, out, 4) == 3 && !strcmp(out, "Zel"));
  printf("utf16: all ok\n");
  return 0;
}
