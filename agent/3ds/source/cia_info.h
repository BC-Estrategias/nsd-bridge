/* What a CIA file says about itself, read from its first few KiB (header, certificate chain, ticket, TMD). Pure C: no
 * console services, so it is unit-tested on the host (agent/3ds/tests). The sizes are what the installer shows before it
 * installs anything and what it uses to guess the progress total. */
#ifndef CIA_INFO_H
#define CIA_INFO_H

#include <stddef.h>
#include <stdint.h>

/* Where AM puts the title, and how scary it is. */
typedef enum {
  CIA_CLASS_APP = 0,     /* application, update, DLC, demo...: SD card */
  CIA_CLASS_DSIWARE,     /* DSiWare: console memory (NAND), user data area */
  CIA_CLASS_SYSTEM,      /* system application/applet/module/data: NAND, can stop the console from booting if wrong */
  CIA_CLASS_FIRM         /* firmware titles: NOT supported by the installer */
} cia_class;

typedef struct {
  uint64_t title_id;
  uint16_t title_version; /* TMD version (major<<10 | minor<<4 | micro) */
  uint16_t content_count;
  uint64_t content_size;  /* bytes of content in the file */
  uint64_t total_size;    /* the whole file as the header describes it (every section aligned to 64 bytes) */
  int on_nand;            /* 1 = NAND, 0 = SD (same rule FBI uses) */
  int new3ds_only;        /* the title id says it needs a New 3DS */
  cia_class cls;
} cia_info;

/* The CIA header and everything up to the end of the title id/version fields of the TMD fit in this many bytes. */
#define CIA_PROBE_BYTES 0x4000u

/* 0 = filled in; -1 = not a CIA / inconsistent; -2 = `len` is too short to tell (read more, up to CIA_PROBE_BYTES). */
int cia_parse(const uint8_t *buf, size_t len, cia_info *out);

/* Class and destination from the title id alone. */
cia_class cia_classify(uint64_t title_id, int *on_nand);

#endif
