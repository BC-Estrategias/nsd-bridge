#ifndef NSD_UTF16_H
#define NSD_UTF16_H
/* UTF-16 (what a title's SMDH stores) to UTF-8. Pure C. */
#include <stddef.h>
#include <stdint.h>

/* Converts up to `max_units` UTF-16 units (stopping at a NUL) into `out` (always NUL-terminated, never cut inside a character).
 * Returns the bytes written, not counting the NUL. Unpaired surrogates become '?'. */
size_t nsd_utf16_to_utf8(const uint16_t *in, size_t max_units, char *out, size_t cap);
#endif
