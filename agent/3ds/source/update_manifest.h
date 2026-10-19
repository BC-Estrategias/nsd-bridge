/* Which release is newer, and which file of it to download. Pure logic: no network, no console services, so it is
 * unit-tested on the host (agent/3ds/tests). The release JSON comes from the GitHub API and is untrusted. */
#ifndef UPDATE_MANIFEST_H
#define UPDATE_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_REPOSITORY "BC-Estrategias/nsd-bridge"
#define UPDATE_MAX_FILE (8u * 1024u * 1024u)

typedef struct {
  char version[24];  /* the tag, e.g. "v1.7.0" */
  char url[256];
  char sha256[65];   /* lower-case hex, from the asset's "digest" */
  uint32_t size;
} update_release;

/* "1.6.0" or "v1.6.0" (exactly three numbers). */
bool update_valid_version(const char *v);
bool update_is_newer(const char *candidate, const char *installed);
/* Only files of this project's own releases may be downloaded. */
bool update_allowed_url(const char *url);
/* Parses GET /repos/<repo>/releases/latest. `homebrew` picks the .3dsx asset, otherwise the .cia.
 * 1 = a valid candidate was filled in, 0 = no usable release, -1 = malformed response. */
int update_parse_release(const char *json, size_t len, bool homebrew, update_release *out);

#endif
