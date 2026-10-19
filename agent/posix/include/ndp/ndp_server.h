/* Connection handling shared by every platform that has BSD-style sockets (the host agent and the 3DS agent via
 * libctru). Two listeners, several kinds of clients, all non-blocking and served from the platform's main loop:
 *   - the NDP port: ONE raw NDP client (the CLI / MCP server), one request at a time;
 *   - the web port (optional): short HTTP requests that fetch the embedded web page, and ONE WebSocket client (the
 *     page itself) that speaks the very same NDP inside WebSocket binary messages (spec §15).
 * The raw client and the web client are independent (each has its own agent state and authentication), so opening
 * the page does not disconnect the MCP server and vice versa.
 *
 * Policy: a new connection of a kind REPLACES the current client of that kind (a stale connection from a crashed
 * Bridge must never lock the console out). Idle clients are dropped after `idle_timeout_ms`.
 * ndp_server_step() never blocks longer than `timeout_ms` (plus bounded send waits). */
#ifndef NDP_SERVER_H
#define NDP_SERVER_H

#include "ndp/ndp_agent.h"
#include "ndp/ndp_frame.h"
#include "ndp/ndp_ws.h"

typedef struct {
  void *ctx;
  /* Monotonic milliseconds. Required. */
  uint64_t (*now_ms)(void *ctx);
  /* One log line, without trailing newline. May be NULL. */
  void (*log)(void *ctx, const char *line);
  /* The key store changed (a computer was paired, or the pairings were cleared): persist it. May be NULL. */
  void (*keys_changed)(void *ctx, const ndp_keystore *keys);
} ndp_server_platform;

/* The page served on the web port (built from web/ into a gzip-compressed HTML by scripts/build-web.py). */
typedef struct {
  const uint8_t *index_gz;
  size_t index_gz_len;
  const char *version; /* shown in the ETag; "" if unknown */
} ndp_web_assets;

#define NDP_WEB_HTTP_SLOTS 3

typedef enum { NDP_CONN_NONE = 0, NDP_CONN_NDP = 1, NDP_CONN_WS = 2 } ndp_conn_kind;

/* One NDP-speaking client (raw TCP or WebSocket). */
typedef struct {
  int fd;
  ndp_conn_kind kind;
  char peer[24];
  uint64_t last_activity_ms;

  ndp_agent agent;
  ndp_decoder dec;
  uint8_t rbuf[NDP_HEADER_SIZE + NDP_DEFAULT_MAX_FRAME + NDP_MAC_SIZE];

  /* Output queue: one frame at a time, sent without blocking (a 64 KiB DATA frame may need several send() calls).
   * For a WebSocket client the WebSocket header sits right before the frame inside `out`. Stream frames are produced
   * only when the previous one is fully out. */
  uint8_t out[NDP_WS_HEADER_MAX + NDP_HEADER_SIZE + NDP_DEFAULT_MAX_FRAME + NDP_MAC_SIZE];
  size_t out_len, out_off;

  /* Input received but not yet decoded (a recv may carry several frames). */
  uint8_t in[16384];
  size_t in_len, in_pos;

  /* WebSocket only */
  ndp_ws_dec wsd;
  const uint8_t *pend; /* decoded message bytes not yet fed to the NDP decoder */
  size_t pend_len;
  uint8_t ctl[140];    /* a pong or close frame waiting for the frame in flight to finish */
  size_t ctl_len, ctl_off;
  int close_after_ctl;

  /* Bookkeeping for the request currently being answered (for the [OK]/[ERR] log line). */
  int cur_active;
  uint32_t cur_id;
  uint16_t cur_cmd;
  uint64_t cur_t0;
} ndp_conn;

/* A short HTTP exchange: read the request head, answer, close. */
typedef struct {
  int fd;
  char peer[24];
  uint64_t started_ms;
  char head[NDP_HTTP_HEAD_MAX];
  size_t head_len;
  char resp[768];
  size_t resp_len, resp_off;
  const uint8_t *body;
  size_t body_len, body_off;
  int responding;
} ndp_http_conn;

typedef struct {
  ndp_server_platform plat;
  ndp_agent_config agent_cfg;
  uint32_t idle_timeout_ms;
  uint32_t p2_prompt_ms; /* how long the number stays on the screen before the request expires (tests shorten it) */

  int listen_fd;
  int client_fd;        /* the raw NDP client (= nc[0].fd), -1 if none */
  uint32_t listen_addr; /* s_addr, network byte order (0 = INADDR_ANY) */
  uint16_t port;        /* actual listening port, host byte order */
  char peer[24];        /* "a.b.c.d" of the raw NDP client, "" if none */
  uint64_t last_activity_ms;

  int web_listen_fd;    /* -1 when the web page is not being served */
  uint16_t web_port;
  int web_client_fd;    /* the WebSocket client (= nc[1].fd), -1 if none */
  char web_peer[24];
  const ndp_web_assets *web_assets;
  int web_open;         /* 1 = the page's WebSocket needs no pairing (the TCP port still does); see ndp_server_set_web_open */
  uint32_t web_requests; /* HTTP pages served */

  /* statistics, monotonically increasing */
  uint32_t connections_opened;
  uint32_t connections_closed;
  uint32_t requests;

  ndp_policy policy;     /* the access policy in force (the owner can change it while running) */
  ndp_keystore keys;     /* paired computers (persisted by the platform) */
  ndp_pairing pairing;   /* the pairing window (spans connections) */

  ndp_conn nc[2];        /* [0] raw NDP, [1] WebSocket */
  ndp_http_conn http[NDP_WEB_HTTP_SLOTS];
} ndp_server;

void ndp_server_init(ndp_server *s, const ndp_server_platform *plat, const ndp_agent_config *cfg);

/* Starts listening on `s_addr` (network byte order) and `port` (0 = ephemeral). Closes any previous
 * listener/client first. Returns 0, or a negative errno-style value. */
int ndp_server_listen(ndp_server *s, uint32_t s_addr, uint16_t port);

/* The web page (call before ndp_server_web_listen). NULL disables serving it. */
void ndp_server_set_web(ndp_server *s, const ndp_web_assets *assets);
/* 1 = browsers using the page's WebSocket are served without pairing (auth "none" for those connections only, and
 * without the pairing list): the owner still controls the mode and the opened folders on the console, and the TCP port
 * used by the CLI and the MCP keeps requiring pairing. 0 (the default) = the page pairs like any other client. */
void ndp_server_set_web_open(ndp_server *s, int open);
/* Starts/stops the web listener (`port` 0 = ephemeral). 0 or a negative errno-style value. While it is closed,
 * browsers get connection refused. */
int ndp_server_web_listen(ndp_server *s, uint32_t s_addr, uint16_t port);
void ndp_server_web_close(ndp_server *s);
int ndp_server_web_enabled(const ndp_server *s);

/* Replaces the access policy at run time (the owner edited the allowed folders on the console). It applies to
 * the current connection from its next request on and to every later one. */
void ndp_server_set_policy(ndp_server *s, const ndp_policy *p);

/* Loads the persisted key store (call before listening). Pass the bytes read from the store file. */
void ndp_server_set_keys(ndp_server *s, const ndp_keystore *keys);
/* Opens the pairing window with `code` for `duration_ms` (spec §4.1). The platform generates the code
 * from a secure random source and shows it to the user. */
void ndp_server_open_pairing(ndp_server *s, const uint8_t code[NDP_CODE_BYTES], uint32_t duration_ms);
void ndp_server_close_pairing(ndp_server *s);
/* Pairing by number comparison (spec §4.8). While a request waits for the person, the platform shows
 * pairing.p2_label, pairing.p2_peer and pairing.p2_sas, and calls _decide() when a button is pressed. The number is
 * never written to the log. */
#define NDP_P2_PROMPT_MS 60000u
#define NDP_P2_COMMIT_MS 30000u
int ndp_server_pair2_pending(const ndp_server *s);
int ndp_server_pair2_decide(ndp_server *s, int approve);
/* Milliseconds left in the window, 0 when it is closed. */
uint32_t ndp_server_pairing_remaining_ms(ndp_server *s);
/* Forgets every paired computer (persisted through keys_changed). The current client stays connected
 * until it disconnects; its session key is no longer valid for new sessions. */
void ndp_server_clear_keys(ndp_server *s);
/* Forgets one paired computer (by its 4-byte key id) and lets the platform persist the change. Returns 0, or -1 if unknown. */
int ndp_server_forget_key(ndp_server *s, const uint8_t key_id[4]);

/* Changes the access mode (also for the current connection). Dropping to READ_ONLY aborts an upload. */
void ndp_server_set_mode(ndp_server *s, ndp_mode mode);

/* Closes the client and the listener. Safe to call at any time. */
void ndp_server_close(ndp_server *s);

/* One iteration: waits up to timeout_ms for activity, then accepts/reads/answers/streams/times out.
 * Streaming is done in bounded slices (a few frames per call) so the platform loop stays responsive.
 * Returns 1 if something visible changed (connect, disconnect, request), 0 if not, or -1 when the
 * listener is broken and the caller should re-listen (typically after a network loss). */
int ndp_server_step(ndp_server *s, int timeout_ms);

#endif
