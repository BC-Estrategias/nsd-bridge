/* Transport-free agent core: bytes of one request frame in, one response frame out.
 * Platform code (sockets, filesystem, UI) lives outside this module. */
#ifndef NDP_AGENT_H
#define NDP_AGENT_H

#include "ndp/ndp_auth.h"
#include "ndp/ndp_frame.h"
#include "ndp/ndp_fs.h"
#include "ndp/ndp_policy.h"
#include "ndp/ndp_sha256.h"

/* What the console reports about itself (DEVICE_INFO). A field is sent only when its bit in `has` is set: the agent
 * never invents a value for something the platform cannot measure. */
#define NDP_DI_MODEL (1u << 0)
#define NDP_DI_FIRMWARE (1u << 1)
#define NDP_DI_RAM_TOTAL (1u << 2)
#define NDP_DI_APP_MEM (1u << 3) /* app_mem_total + app_mem_free */
#define NDP_DI_SYS_MEM (1u << 4) /* sys_mem_total + sys_mem_free */
#define NDP_DI_SD (1u << 5)      /* sd_total + sd_free */
typedef struct {
  uint32_t has;
  char model[40];
  char firmware[32];
  uint64_t ram_total;
  uint64_t app_mem_total, app_mem_free;
  uint64_t sys_mem_total, sys_mem_free;
  uint64_t sd_total, sd_free;
} ndp_device_info;

typedef struct {
  const char *platform;      /* "3ds", "host", ... */
  const char *agent_version; /* "0.1.0" */
  ndp_mode mode;
  const char *auth;          /* "none" | "required" | "paired" */
  uint32_t max_frame;
  /* Fills n random bytes (HELLO device nonce, device id). Returns 0, or non-zero when no SECURE random
   * source exists: with auth "required" the agent then refuses to start a session (never a weak nonce). */
  int (*random_bytes)(void *ctx, uint8_t *out, size_t n);
  void *random_ctx;
  /* Authentication (auth == "required"): key store and pairing window, owned by the platform. */
  ndp_keystore *keys;
  ndp_pairing *pairing;
  /* Called after the key store changed (a computer was paired) so the platform can persist it. */
  void (*keys_changed)(void *ctx, const ndp_keystore *keys);
  void *keys_ctx;
  /* Optional. NULL fs => FS commands answer UNSUPPORTED_COMMAND. NULL policy => spec defaults. */
  const ndp_fs_ops *fs;
  const ndp_policy *policy;
  /* Optional. Fills what the platform can measure (may take a while: on the 3DS reading the SD free space walks the
   * FAT). NULL => DEVICE_INFO answers UNSUPPORTED_COMMAND. Returns 0, or non-zero => IO_ERROR. */
  int (*device_info)(void *ctx, ndp_device_info *out);
  void *device_info_ctx;
  /* Optional. INSTALL_URL: the platform decides whether it can take the request now (the console only does while its
   * "receive from the browser" screen is open) and asks its owner to confirm. Exactly one of `url` / `path` is non-NULL
   * (`path`: a .cia already on the SD card, normalized, absolute). Returns NDP_OK when the request was taken;
   * otherwise a status (NDP_ST_BUSY with a `detail` text for "not open now / one already pending"). NULL => the command
   * answers UNSUPPORTED_COMMAND. The URL has been checked (http/https, printable ASCII, no spaces). */
  int (*install_url)(void *ctx, const char *url, const char *path, const uint8_t *sha256 /* NULL, or the 32-byte hash the file must have */,
                     const uint8_t *seed /* NULL, or the 16-byte seed of the title */, char *detail, size_t detail_cap);
  void *install_url_ctx;
  /* Set by the server per connection: a small id for the connection and its peer address (for the pairing prompt). */
  int conn_id;
  const char *peer;
} ndp_agent_config;

typedef struct {
  ndp_agent_config cfg;
  int hello_done;
  ndp_policy policy;
  struct {           /* directory kept open between FS_LIST pages */
    int open;
    void *handle;
    char path[NDP_PATH_MAX + 1];
    uint32_t pos;
  } dir;
  struct {           /* active FS_READ */
    int active;
    void *file;
    uint64_t remaining;
    uint64_t sent;
    uint32_t chunk;
    uint32_t request_id;
    uint16_t command;
    int want_hash;
    ndp_sha256_ctx sha;
  } xfer;
  struct {           /* active FS_WRITE (upload) */
    int active;
    int discard;     /* after an ERR: ignore this request's remaining DATA until END or the next REQ */
    uint32_t id;
    void *file;
    uint64_t declared, received;
    int overwrite, backup, target_exists, has_expected;
    uint8_t expected[32];
    char target[NDP_PATH_MAX + 1];
    char temp[NDP_PATH_MAX + 16];
    ndp_sha256_ctx sha;
  } up;
  struct {           /* authentication state of this connection */
    int required;
    int authed;
    int fails;
    uint8_t cn[16], dn[16];   /* HELLO nonces (bridge / console) */
    uint8_t session[32];
    uint64_t send_ctr, recv_ctr;
  } sec;
  uint64_t last_transfer_bytes; /* bytes of the most recently finished transfer */
} ndp_agent;

void ndp_agent_init(ndp_agent *a, const ndp_agent_config *cfg);
/* Replaces the access policy at run time (the owner edited the allowed folders on the console). */
void ndp_agent_set_policy(ndp_agent *a, const ndp_policy *p);

/* Handles one decoded frame and writes exactly one response frame (RES or ERR) into `out`.
 * `mac` is the 16-byte MAC that followed the payload, or NULL when the frame carried none. After a
 * successful AUTH every frame in both directions is sealed (spec §4.5): incoming ones are verified
 * here and every response is sealed before being returned, so `cap` must leave NDP_MAC_SIZE spare bytes.
 * Returns the response length, NDP_NO_REPLY when the frame needs no response (DATA of an upload),
 * NDP_CLOSE when the connection must be dropped, or 0 if `cap` is too small. */
size_t ndp_agent_handle_frame(ndp_agent *a, const ndp_header *hdr, const uint8_t *payload, const uint8_t *mac,
                              uint8_t *out, size_t cap);

/* Convenience for unauthenticated use (auth "none"): no MAC. */
size_t ndp_agent_handle(ndp_agent *a, const ndp_header *hdr, const uint8_t *payload, uint8_t *out, size_t cap);

/* Streaming (FS_READ): after ndp_agent_handle returned the RES that starts a transfer,
 * ndp_agent_streaming() is 1 and the platform loop calls ndp_agent_next_frame() to obtain the DATA
 * frames and the final END (or ERR) one at a time. Returns the frame length; 0 when there is no
 * active transfer. `cap` must be at least NDP_HEADER_SIZE + the negotiated max_frame. */
int ndp_agent_streaming(const ndp_agent *a);
/* True once the connection completed AUTH. */
int ndp_agent_authenticated(const ndp_agent *a);
/* 1 while a download or an upload is in progress (the reply for the current request is not final yet). */
int ndp_agent_busy(const ndp_agent *a);
/* Changes the access mode at runtime; dropping to READ_ONLY aborts an active upload. */
void ndp_agent_set_mode(ndp_agent *a, ndp_mode mode);
size_t ndp_agent_next_frame(ndp_agent *a, uint8_t *out, size_t cap);

/* Releases directory/file handles (connection closed or replaced). Safe to call repeatedly. */
void ndp_agent_close(ndp_agent *a);

const char *ndp_mode_name(ndp_mode m);

#endif
