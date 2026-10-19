/* Host test of the update manifest logic: cc -I source tests/update_manifest_test.c source/update_manifest.c -ljansson */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "update_manifest.h"

#define URL "https://github.com/" UPDATE_REPOSITORY "/releases/download/"
#define D64 "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef"

static const char *good(char *buf, size_t cap, const char *tag, const char *asset, const char *url, const char *digest, int size, const char *flags) {
  snprintf(buf, cap, "{\"tag_name\":\"%s\",%s\"assets\":[{\"name\":\"%s\",\"browser_download_url\":\"%s\",\"digest\":\"%s\",\"size\":%d}]}",
           tag, flags, asset, url, digest, size);
  return buf;
}

int main(void) {
  char j[1024], url[300], dg[100];
  update_release r;
  const char *ok = "\"draft\":false,\"prerelease\":false,";

  assert(update_is_newer("v1.7.0", "1.6.0") && update_is_newer("1.6.1", "1.6.0") && update_is_newer("v2.0.0", "1.99.99"));
  assert(!update_is_newer("1.6.0", "1.6.0") && !update_is_newer("v1.5.9", "1.6.0") && update_is_newer("1.10.0", "1.9.0"));
  assert(!update_is_newer("garbage", "1.6.0") && !update_is_newer("1.6", "1.6.0") && !update_is_newer("1.6.0-E1", "1.5.0"));
  assert(update_valid_version("v1.0.0") && !update_valid_version("1.0.0.1") && !update_valid_version("") && !update_valid_version("v1..0"));

  assert(update_allowed_url(URL "v1.7.0/x.cia") && !update_allowed_url("http://github.com/" UPDATE_REPOSITORY "/releases/download/x") &&
         !update_allowed_url("https://evil.example/" UPDATE_REPOSITORY "/releases/download/x") && !update_allowed_url(URL "../../x") && !update_allowed_url(NULL));

  snprintf(url, sizeof url, URL "v1.7.0/nsd-bridge-v1.7.0.cia");
  snprintf(dg, sizeof dg, "sha256:%s", D64);
  good(j, sizeof j, "v1.7.0", "nsd-bridge-v1.7.0.cia", url, dg, 400000, ok);
  assert(update_parse_release(j, strlen(j), false, &r) == 1);
  assert(!strcmp(r.version, "v1.7.0") && r.size == 400000 && !strcmp(r.url, url) && r.sha256[0] == '0' && r.sha256[26] == 'a' && strlen(r.sha256) == 64); /* upper-case digits are lower-cased */
  assert(update_parse_release(j, strlen(j), true, &r) == -1 || update_parse_release(j, strlen(j), true, &r) == 0); /* no .3dsx asset */

  good(j, sizeof j, "v1.7.0", "nsd-bridge-v1.7.0.cia", url, dg, 400000, "\"draft\":false,\"prerelease\":true,");
  assert(update_parse_release(j, strlen(j), false, &r) == 0);
  good(j, sizeof j, "v1.7.0", "nsd-bridge-v1.7.0.cia", url, dg, 400000, "\"draft\":true,\"prerelease\":false,");
  assert(update_parse_release(j, strlen(j), false, &r) == 0);
  good(j, sizeof j, "v1.7.0", "nsd-bridge-v1.7.0.cia", "https://evil.example/nsd-bridge-v1.7.0.cia", dg, 400000, ok);
  assert(update_parse_release(j, strlen(j), false, &r) == -1);
  good(j, sizeof j, "v1.7.0", "nsd-bridge-v1.7.0.cia", URL "v1.8.0/nsd-bridge-v1.7.0.cia", dg, 400000, ok); /* tag/url mismatch */
  assert(update_parse_release(j, strlen(j), false, &r) == -1);
  good(j, sizeof j, "v1.7.0", "nsd-bridge-v1.7.0.cia", url, "sha256:zz", 400000, ok);
  assert(update_parse_release(j, strlen(j), false, &r) == -1);
  good(j, sizeof j, "v1.7.0", "nsd-bridge-v1.7.0.cia", url, dg, 999999999, ok); /* absurd size */
  assert(update_parse_release(j, strlen(j), false, &r) == -1);
  good(j, sizeof j, "v1.7.0", "nsd-bridge-v1.7.0.cia", url, dg, 10, ok);
  assert(update_parse_release(j, strlen(j), false, &r) == -1);
  good(j, sizeof j, "latest", "nsd-bridge-vlatest.cia", url, dg, 400000, ok);
  assert(update_parse_release(j, strlen(j), false, &r) == -1);
  assert(update_parse_release("not json", 8, false, &r) == -1 && update_parse_release("[]", 2, false, &r) == -1);
  assert(update_parse_release("{\"tag_name\":\"v1.7.0\",\"tag_name\":\"v9.9.9\"}", 40, false, &r) == -1); /* duplicate keys */
  puts("update_manifest: all ok");
  return 0;
}
