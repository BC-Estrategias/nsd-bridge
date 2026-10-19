#include "update_manifest.h"

#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <string.h>

static bool parse_version(const char *s, unsigned part[3]) {
  int i;
  if (!s) return false;
  if (*s == 'v') s++;
  for (i = 0; i < 3; i++) {
    unsigned n = 0;
    int digits = 0;
    if (!isdigit((unsigned char)*s)) return false;
    while (isdigit((unsigned char)*s)) {
      n = n * 10 + (unsigned)(*s++ - '0');
      if (++digits > 4) return false;
    }
    part[i] = n;
    if (i < 2) {
      if (*s++ != '.') return false;
    }
  }
  return *s == '\0';
}

bool update_valid_version(const char *v) {
  unsigned p[3];
  return parse_version(v, p);
}

bool update_is_newer(const char *candidate, const char *installed) {
  unsigned a[3], b[3];
  int i;
  if (!parse_version(candidate, a) || !parse_version(installed, b)) return false;
  for (i = 0; i < 3; i++)
    if (a[i] != b[i]) return a[i] > b[i];
  return false;
}

bool update_allowed_url(const char *url) {
  static const char prefix[] = "https://github.com/" UPDATE_REPOSITORY "/releases/download/";
  return url && strncmp(url, prefix, sizeof prefix - 1) == 0 && !strstr(url, "..");
}

static bool text(json_t *o, const char *key, char *dst, size_t cap) {
  json_t *v = json_object_get(o, key);
  if (!json_is_string(v) || json_string_length(v) >= cap || strlen(json_string_value(v)) != json_string_length(v)) return false;
  memcpy(dst, json_string_value(v), json_string_length(v) + 1);
  return true;
}

int update_parse_release(const char *data, size_t len, bool homebrew, update_release *out) {
  json_error_t err;
  json_t *root, *assets, *a;
  char tag[24], name[64], expected[256];
  size_t i;
  int result = 0;

  memset(out, 0, sizeof *out);
  root = json_loadb(data, len, JSON_REJECT_DUPLICATES, &err);
  if (!root) return -1;
  if (!json_is_object(root)) { json_decref(root); return -1; }
  /* a draft or a pre-release is never offered */
  if (!json_is_false(json_object_get(root, "draft")) || !json_is_false(json_object_get(root, "prerelease"))) { json_decref(root); return 0; }
  if (!text(root, "tag_name", tag, sizeof tag) || !update_valid_version(tag)) { json_decref(root); return -1; }
  snprintf(name, sizeof name, "nsd-bridge-%s%s.%s", tag[0] == 'v' ? "" : "v", tag, homebrew ? "3dsx" : "cia");
  snprintf(expected, sizeof expected, "https://github.com/" UPDATE_REPOSITORY "/releases/download/%s/%s", tag, name);
  assets = json_object_get(root, "assets");
  if (!json_is_array(assets)) { json_decref(root); return -1; }
  json_array_foreach(assets, i, a) {
    char n[64], url[256], digest[80];
    json_t *size;
    int j;
    if (!json_is_object(a) || !text(a, "name", n, sizeof n) || strcmp(n, name) != 0) continue;
    if (result || !text(a, "browser_download_url", url, sizeof url) || strcmp(url, expected) != 0 ||
        !text(a, "digest", digest, sizeof digest) || strlen(digest) != 71 || strncmp(digest, "sha256:", 7) != 0) { result = -1; break; }
    size = json_object_get(a, "size");
    if (!json_is_integer(size) || json_integer_value(size) < 4096 || json_integer_value(size) > (json_int_t)UPDATE_MAX_FILE) { result = -1; break; }
    for (j = 7; j < 71; j++)
      if (!isxdigit((unsigned char)digest[j])) break;
    if (j != 71) { result = -1; break; }
    for (j = 0; j < 64; j++) out->sha256[j] = (char)tolower((unsigned char)digest[j + 7]);
    snprintf(out->version, sizeof out->version, "%s", tag);
    snprintf(out->url, sizeof out->url, "%s", url);
    out->size = (uint32_t)json_integer_value(size);
    result = 1;
  }
  json_decref(root);
  if (result != 1) memset(out, 0, sizeof *out);
  return result;
}
