#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trpack.h"

static char *slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  char *b;
  assert(f);
  fseek(f, 0, SEEK_END);
  *n = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  b = malloc(*n + 1);
  assert(fread(b, 1, *n, f) == *n);
  b[*n] = '\0';
  fclose(f);
  return b;
}

int main(int argc, char **argv) {
  const char *sample = argc > 1 ? argv[1] : "agent/3ds/tests/data/propfind_sample.xml";
  dav_entry e[32];
  tr_ref r;
  char out[64];
  size_t n;
  char *xml = slurp(sample, &n);
  int c, i, dirs = 0, files = 0;

  /* a real Nextcloud answer: the folder itself is skipped, names are decoded, folders and files told apart */
  c = dav_parse(xml, n, e, 32);
  assert(c >= 5);
  for (i = 0; i < c; i++) { if (e[i].is_dir) dirs++; else files++; }
  assert(dirs == 1 && e[0].is_dir && !strcmp(e[0].name, "PRESENTES"));
  assert(files == c - 1);
  {
    bool found = false;
    for (i = 0; i < c; i++) if (!strcmp(e[i].name, "HoT Wallpaper.png") && !e[i].is_dir && e[i].size > 1000000) found = true;
    assert(found);
  }
  free(xml);
  /* other prefixes and empty answers */
  {
    const char *x = "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>/a/</D:href></D:response>"
                    "<D:response><D:href>/a/b%20c.txt</D:href><D:propstat><D:prop><D:getcontentlength>12</D:getcontentlength></D:prop></D:propstat></D:response></D:multistatus>";
    assert(dav_parse(x, strlen(x), e, 32) == 1 && !strcmp(e[0].name, "b c.txt") && e[0].size == 12 && !e[0].is_dir);
    assert(dav_parse("", 0, e, 32) == 0);
    assert(dav_parse("<multistatus></multistatus>", 27, e, 32) == 0);
  }
  assert(dav_parse(NULL, 0, e, 0) == 0);

  /* url encode / decode */
  url_decode("a%20b%C3%A7%2", 13, out, sizeof out);
  assert(!strcmp(out, "a b\xC3\xA7%2"));
  url_encode_path("traducoes/ptbr/0004 - Zelda Majora's/v1.0", out, sizeof out);
  assert(!strcmp(out, "traducoes/ptbr/0004%20-%20Zelda%20Majora%27s/v1.0"));

  /* pack names */
  assert(tr_parse_ref("ptbr", "00040000000fc500 - Bravely Default", "jumpmanclub-brasil", "v2.0", &r));
  assert(!strcmp(r.id, "00040000000FC500") && !strcmp(r.game, "Bravely Default") && !strcmp(r.lang, "ptbr") && !strcmp(r.version, "v2.0"));
  assert(tr_parse_ref("en", "000400000005DA00 - Final Fantasy 1 3DS - Remix", "desconhecida", "v2", &r) && !strcmp(r.game, "Final Fantasy 1 3DS - Remix"));
  assert(!tr_parse_ref("PTBR", "00040000000FC500 - X", "t", "v1", &r));          /* upper case language */
  assert(!tr_parse_ref("ptbr", "00040000000FC50 - X", "t", "v1", &r));           /* 15 digits */
  assert(!tr_parse_ref("ptbr", "00040000000FC500 X", "t", "v1", &r));            /* no " - " */
  assert(!tr_parse_ref("ptbr", "00040000000FC500 - ", "t", "v1", &r));           /* no name */
  assert(!tr_parse_ref("ptbr", "00040000000FC500 - X", "a/b", "v1", &r));        /* team with a slash */
  assert(!tr_parse_ref("ptbr", "00040000000FC500 - X", "t", "1.0", &r));         /* no v */
  assert(!tr_parse_ref("ptbr", "00040000000FC500 - X", "t", "v1.", &r));
  assert(!tr_parse_ref("ptbr", "00040000000FC500 - X", "t", "v1.0-beta", &r));
  assert(!tr_parse_ref("ptbr", "00040000000FC500 - X", "", "v1", &r));

  /* versions compare by number, not by text */
  assert(tr_version_cmp("v1.10", "v1.9") > 0 && tr_version_cmp("v1.9", "v1.10") < 0);
  assert(tr_version_cmp("v2", "v2.0") == 0 && tr_version_cmp("v1.2.1", "v1.2") > 0 && tr_version_cmp("v1.2", "v1.2") == 0);

  /* what a pack may write */
  assert(tr_path_ok("romfs/message/texts.bin") && tr_path_ok("code.ips") && tr_path_ok("exefs/code.bin") && tr_path_ok("romfs/Pok\xC3\xA9mon.bin"));
  assert(!tr_path_ok("") && !tr_path_ok("/etc/passwd") && !tr_path_ok("../x") && !tr_path_ok("romfs/../../x") && !tr_path_ok("romfs//x"));
  assert(!tr_path_ok("romfs/") && !tr_path_ok("a\\b") && !tr_path_ok("a:b") && !tr_path_ok("a/b?") && !tr_path_ok("a/./b") && !tr_path_ok("x/y ") && !tr_path_ok("x/y."));
  assert(!tr_path_ok("a/b\x01") && !tr_path_ok("a/b\x7F"));
  {
    char longseg[80];
    memset(longseg, 'a', 70);
    longseg[70] = '\0';
    assert(!tr_path_ok(longseg));
  }
  assert(tr_skip_name("._code.ips") && tr_skip_name(".DS_Store") && tr_skip_name("Thumbs.db") && tr_skip_name("DESKTOP.INI") && !tr_skip_name("code.ips") && !tr_skip_name(".hidden"));

  /* pacote.json */
  {
    const char *j = "{\n \"formato\": 1,\n \"jogo\": \"Pok\\u00e9mon X\",\n \"equipe\":\"E4T (Elite dos Quatro Tradu\\u00e7\\u00f5es)\",\n \"fonte\": \"https://x/y\\\"z\", \"regiao\" : \"Livre\"}";
    assert(tr_json_get(j, "jogo", out, sizeof out) && !strcmp(out, "Pok\xC3\xA9mon X"));
    assert(tr_json_get(j, "equipe", out, sizeof out) && !strncmp(out, "E4T (Elite dos Quatro Tradu", 27));
    assert(tr_json_get(j, "fonte", out, sizeof out) && !strcmp(out, "https://x/y\"z"));
    assert(tr_json_get(j, "regiao", out, sizeof out) && !strcmp(out, "Livre"));
    assert(!tr_json_get(j, "formato", out, sizeof out));   /* a number, not a string */
    assert(!tr_json_get(j, "nada", out, sizeof out));
    assert(tr_json_get(j, "equipe", out, 8) && strlen(out) == 7); /* never overruns */
  }
  {
    char esc[64];
    size_t l = tr_json_escape("a\"b\\c\nd\x01", esc, sizeof esc);
    assert(l == strlen(esc) && !strcmp(esc, "a\\\"b\\\\c\\nd\\u0001"));
  }
  printf("trpack: all ok\n");
  return 0;
}
