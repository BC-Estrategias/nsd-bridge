#include "trpack.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------------------------------------ URL */
void url_decode(const char *in, size_t n, char *out, size_t cap) {
  size_t i = 0, o = 0;
  if (!cap) return;
  while (i < n && in[i] && o + 1 < cap) {
    if (in[i] == '%' && i + 2 < n && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
      char h[3] = {in[i + 1], in[i + 2], 0};
      out[o++] = (char)strtol(h, NULL, 16);
      i += 3;
    } else {
      out[o++] = in[i++];
    }
  }
  out[o] = '\0';
}

void url_encode_path(const char *in, char *out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  if (!cap) return;
  for (; *in && o + 4 < cap; in++) {
    unsigned char c = (unsigned char)*in;
    if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
    else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
  }
  out[o] = '\0';
}

/* ------------------------------------------------------------------------------------------------ WebDAV */
/* Finds <[prefix:]name ...>content</[prefix:]name> or <[prefix:]name/> between s and end. */
static bool find_tag(const char *s, const char *end, const char *name, const char **content, const char **cend, const char **after) {
  size_t nl = strlen(name);
  const char *p = s;
  while (p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
    const char *q = p + 1, *t = q, *nm, *e, *gt, *c;
    if (q >= end || *q == '/' || *q == '?' || *q == '!') { p = q; continue; }
    while (t < end && isalnum((unsigned char)*t)) t++;
    nm = (t < end && *t == ':') ? t + 1 : q;
    e = nm;
    while (e < end && (isalnum((unsigned char)*e) || *e == '-')) e++;
    if ((size_t)(e - nm) != nl || strncasecmp(nm, name, nl) != 0) { p = q; continue; }
    gt = memchr(e, '>', (size_t)(end - e));
    if (!gt) return false;
    if (gt > e && gt[-1] == '/') { /* <d:collection/> */
      *content = *cend = gt;
      if (after) *after = gt + 1;
      return true;
    }
    c = gt + 1;
    while (c < end && (c = memchr(c, '<', (size_t)(end - c))) != NULL) {
      if (c + 1 < end && c[1] == '/') {
        const char *u = c + 2, *w = u, *nm2;
        while (w < end && isalnum((unsigned char)*w)) w++;
        nm2 = (w < end && *w == ':') ? w + 1 : u;
        if (nm2 + nl < end && !strncasecmp(nm2, name, nl) && nm2[nl] == '>') {
          *content = gt + 1;
          *cend = c;
          if (after) *after = nm2 + nl + 1;
          return true;
        }
      }
      c++;
    }
    return false;
  }
  return false;
}

int dav_parse(const char *xml, size_t n, dav_entry *out, int max) {
  const char *end = xml + n, *p = xml;
  int count = 0, idx = 0;
  while (p < end) {
    const char *rc, *re, *after, *hc, *he;
    char href[512], dec[512];
    dav_entry e;
    const char *seg;
    size_t hl;
    if (!find_tag(p, end, "response", &rc, &re, &after)) break;
    p = after;
    if (idx++ == 0) continue; /* the folder itself */
    if (!find_tag(rc, re, "href", &hc, &he, NULL)) continue;
    hl = (size_t)(he - hc);
    if (hl >= sizeof href) hl = sizeof href - 1;
    memcpy(href, hc, hl);
    href[hl] = '\0';
    while (hl > 1 && href[hl - 1] == '/') href[--hl] = '\0';
    url_decode(href, hl, dec, sizeof dec);
    seg = strrchr(dec, '/');
    seg = seg ? seg + 1 : dec;
    memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "%s", seg);
    {
      const char *cc, *ce;
      e.is_dir = find_tag(rc, re, "collection", &cc, &ce, NULL);
      if (!e.is_dir && find_tag(rc, re, "getcontentlength", &cc, &ce, NULL)) e.size = strtoull(cc, NULL, 10);
    }
    if (e.name[0] && count < max) out[count++] = e;
  }
  return count;
}

/* ------------------------------------------------------------------------------------------------ packs */
static bool is_hex16(const char *s) {
  int i;
  for (i = 0; i < 16; i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

bool tr_parse_ref(const char *lang, const char *id_and_name, const char *team, const char *version, tr_ref *out) {
  size_t i, l;
  memset(out, 0, sizeof *out);
  l = strlen(lang);
  if (l < 2 || l > 8) return false;
  for (i = 0; i < l; i++) if (!(islower((unsigned char)lang[i]) || isdigit((unsigned char)lang[i]) || lang[i] == '-')) return false;
  snprintf(out->lang, sizeof out->lang, "%s", lang);
  if (strlen(id_and_name) < 19 || !is_hex16(id_and_name) || strncmp(id_and_name + 16, " - ", 3) != 0) return false;
  for (i = 0; i < 16; i++) out->id[i] = (char)toupper((unsigned char)id_and_name[i]);
  out->id[16] = '\0';
  snprintf(out->game, sizeof out->game, "%s", id_and_name + 19);
  if (!out->game[0]) return false;
  l = strlen(team);
  if (l == 0 || l >= sizeof out->team || strchr(team, '/')) return false;
  snprintf(out->team, sizeof out->team, "%s", team);
  if (version[0] != 'v' || !isdigit((unsigned char)version[1])) return false;
  for (i = 1; version[i]; i++) if (!(isdigit((unsigned char)version[i]) || version[i] == '.')) return false;
  if (strlen(version) >= sizeof out->version || version[strlen(version) - 1] == '.') return false;
  snprintf(out->version, sizeof out->version, "%s", version);
  return true;
}

int tr_version_cmp(const char *a, const char *b) {
  if (*a == 'v') a++;
  if (*b == 'v') b++;
  while (*a || *b) {
    unsigned long x = strtoul(a, (char **)&a, 10), y = strtoul(b, (char **)&b, 10);
    if (x != y) return x < y ? -1 : 1;
    if (*a == '.') a++;
    if (*b == '.') b++;
    if (!*a && !*b) break;
  }
  return 0;
}

bool tr_skip_name(const char *name) {
  return !strncmp(name, "._", 2) || !strcmp(name, ".DS_Store") || !strcasecmp(name, "Thumbs.db") || !strcasecmp(name, "desktop.ini");
}

bool tr_path_ok(const char *rel) {
  const char *seg = rel;
  size_t total = strlen(rel);
  if (total == 0 || total > 200 || rel[0] == '/') return false;
  for (;;) {
    const char *e = strchr(seg, '/');
    size_t l = e ? (size_t)(e - seg) : strlen(seg), i;
    if (l == 0 || l > 64) return false;                       /* empty segment ("//" or a trailing "/") or too long */
    if ((l == 1 && seg[0] == '.') || (l == 2 && seg[0] == '.' && seg[1] == '.')) return false;
    if (seg[l - 1] == ' ' || seg[l - 1] == '.') return false; /* FAT drops them silently */
    for (i = 0; i < l; i++) {
      unsigned char c = (unsigned char)seg[i];
      if (c < 0x20 || c == 0x7F || strchr("\\:*?\"<>|", c)) return false;
    }
    if (!e) break;
    seg = e + 1;
  }
  return true;
}

/* ------------------------------------------------------------------------------------------------ JSON */
bool tr_json_get(const char *json, const char *key, char *out, size_t cap) {
  char pat[80];
  const char *p = json;
  size_t o = 0;
  if (!cap) return false;
  snprintf(pat, sizeof pat, "\"%s\"", key);
  for (;;) {
    p = strstr(p, pat);
    if (!p) return false;
    {
      const char *q = p + strlen(pat);
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
      if (*q != ':') { p += 1; continue; }
      q++;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
      if (*q != '"') return false;
      q++;
      while (*q && *q != '"' && o + 1 < cap) {
        if (*q == '\\' && q[1]) {
          q++;
          switch (*q) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'u':
              if (isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2]) && isxdigit((unsigned char)q[3]) && isxdigit((unsigned char)q[4])) {
                char h[5] = {q[1], q[2], q[3], q[4], 0};
                unsigned long cp = strtoul(h, NULL, 16);
                if (cp < 0x80) out[o++] = (char)cp;
                else if (cp < 0x800 && o + 2 < cap) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
                else if (o + 3 < cap) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
                q += 4;
              }
              break;
            default: out[o++] = *q; break;
          }
          q++;
        } else {
          out[o++] = *q++;
        }
      }
      out[o] = '\0';
      return true;
    }
  }
}

size_t tr_json_escape(const char *in, char *out, size_t cap) {
  size_t o = 0;
  if (!cap) return 0;
  for (; *in && o + 7 < cap; in++) {
    unsigned char c = (unsigned char)*in;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
    else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
    else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
    else out[o++] = (char)c;
  }
  out[o] = '\0';
  return o;
}
