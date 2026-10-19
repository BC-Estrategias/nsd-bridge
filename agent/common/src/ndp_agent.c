#include "ndp/ndp_agent.h"
#include "ndp/ndp_path.h"

#include <string.h>

#include "ndp_agent_internal.h"

const char *ndp_mode_name(ndp_mode m) {
  switch (m) {
    case NDP_MODE_READ_ONLY: return "READ_ONLY";
    case NDP_MODE_DEVELOPMENT: return "DEVELOPMENT";
    case NDP_MODE_FULL: return "FULL";
  }
  return "READ_ONLY";
}

void ndp_agent_init(ndp_agent *a, const ndp_agent_config *cfg) {
  memset(a, 0, sizeof *a);
  a->cfg = *cfg;
  if (cfg->policy) a->policy = *cfg->policy;
  else ndp_policy_init_default(&a->policy);
  a->sec.required = cfg->auth && strcmp(cfg->auth, "required") == 0;
}

/* Response frames are built with the payload written directly after the header slot. */
size_t ndp_agent_finish(uint8_t *out, size_t cap, const ndp_header *req, uint8_t kind, uint16_t status,
                        const ndp_tlv_w *w) {
  ndp_header h;
  if (w->overflow) return 0;
  h.version = NDP_PROTOCOL_VERSION;
  h.kind = kind;
  h.flags = 0;
  h.request_id = req->request_id;
  h.command = req->command;
  h.status = status;
  h.payload_len = 0;
  return ndp_frame_encode(out, cap, &h, out + NDP_HEADER_SIZE, w->len, NULL);
}

size_t ndp_agent_error(uint8_t *out, size_t cap, const ndp_header *req, uint16_t status, const char *detail,
                       int with_supported) {
  ndp_tlv_w w;
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  if (with_supported) {
    ndp_tlv_put_u16(&w, NDP_TAG_SUPPORTED_MIN, NDP_PROTOCOL_VERSION);
    ndp_tlv_put_u16(&w, NDP_TAG_SUPPORTED_MAX, NDP_PROTOCOL_VERSION);
  }
  ndp_tlv_put_str(&w, NDP_TAG_DETAIL, detail);
  return ndp_agent_finish(out, cap, req, NDP_KIND_ERR, status, &w);
}

static size_t do_hello(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *vmin, *vmax, *nonce;
  size_t lmin, lmax, lnon;
  unsigned lo, hi, chosen;
  ndp_tlv_w w;
  uint8_t device_nonce[16];
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_PROTOCOL, &vmin, &lmin) ||
      !ndp_tlv_find(pl, req->payload_len, NDP_TAG_PROTOCOL_MAX, &vmax, &lmax) ||
      !ndp_tlv_find(pl, req->payload_len, NDP_TAG_NONCE, &nonce, &lnon))
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "missing field", 0);
  if (lmin != 2 || lmax != 2 || lnon != 16) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "bad field size", 0);
  lo = ndp_tlv_get_u16(vmin);
  hi = ndp_tlv_get_u16(vmax);
  if (lo > hi) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "protocol_min > protocol_max", 0);
  chosen = hi < NDP_PROTOCOL_VERSION ? hi : NDP_PROTOCOL_VERSION;
  if (chosen < (lo > NDP_PROTOCOL_VERSION ? lo : NDP_PROTOCOL_VERSION))
    return ndp_agent_error(out, cap, req, NDP_ST_UNSUPPORTED_PROTOCOL, "no common protocol version", 1);
  ndp_agent_fs_close(a); /* a new HELLO starts a clean session */
  memset(device_nonce, 0, sizeof device_nonce);
  if (a->cfg.random_bytes) {
    int rc = a->cfg.random_bytes(a->cfg.random_ctx, device_nonce, sizeof device_nonce);
    if (rc != 0 && a->sec.required) return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "no secure random", 0);
  } else if (a->sec.required) {
    return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "no secure random", 0);
  }
  a->hello_done = 1;
  memcpy(a->sec.cn, nonce, 16);
  memcpy(a->sec.dn, device_nonce, 16);
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_u16(&w, NDP_TAG_PROTOCOL, (uint16_t)chosen);
  ndp_tlv_put_str(&w, NDP_TAG_PLATFORM, a->cfg.platform);
  ndp_tlv_put_str(&w, NDP_TAG_AGENT_VERSION, a->cfg.agent_version);
  ndp_tlv_put(&w, NDP_TAG_NONCE, device_nonce, sizeof device_nonce);
  ndp_tlv_put_str(&w, NDP_TAG_AUTH, a->cfg.auth);
  ndp_tlv_put_str(&w, NDP_TAG_MODE, ndp_mode_name(a->cfg.mode));
  ndp_tlv_put_u32(&w, NDP_TAG_MAX_FRAME, a->cfg.max_frame);
  if (a->sec.required) ndp_agent_hello_auth_fields(a, &w);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

static size_t do_ping(const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *n;
  size_t ln;
  ndp_tlv_w w;
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_PING_NONCE, &n, &ln) || ln != 8)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "ping_nonce required", 0);
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_u64(&w, NDP_TAG_PING_NONCE, ndp_tlv_get_u64(n));
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

void ndp_agent_set_policy(ndp_agent *a, const ndp_policy *p) { a->policy = *p; }

/* ACCESS_INFO: what this client may touch. Lets an assistant plan instead of guessing from PROTECTED_PATH errors. */
static size_t do_access_info(ndp_agent *a, const ndp_header *req, uint8_t *out, size_t cap) {
  ndp_tlv_w w;
  int i;
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_str(&w, NDP_TAG_MODE, ndp_mode_name(a->cfg.mode));
  for (i = 0; i < a->policy.read_roots.count; i++) ndp_tlv_put_str(&w, NDP_TAG_READ_ROOT, a->policy.read_roots.entries[i]);
  for (i = 0; i < a->policy.write_roots.count; i++) ndp_tlv_put_str(&w, NDP_TAG_WRITE_ROOT, a->policy.write_roots.entries[i]);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

/* DEVICE_INFO: model, firmware, RAM, memory regions, SD card capacity. Read-only, allowed in every mode. */
static size_t do_device_info(ndp_agent *a, const ndp_header *req, uint8_t *out, size_t cap) {
  ndp_device_info di;
  ndp_tlv_w w;
  if (!a->cfg.device_info) return ndp_agent_error(out, cap, req, NDP_ST_UNSUPPORTED_COMMAND, "unknown command", 0);
  memset(&di, 0, sizeof di);
  if (a->cfg.device_info(a->cfg.device_info_ctx, &di) != 0)
    return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "cannot read device information", 0);
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  if (di.has & NDP_DI_MODEL) ndp_tlv_put_str(&w, NDP_TAG_MODEL, di.model);
  if (di.has & NDP_DI_FIRMWARE) ndp_tlv_put_str(&w, NDP_TAG_FIRMWARE, di.firmware);
  if (di.has & NDP_DI_RAM_TOTAL) ndp_tlv_put_u64(&w, NDP_TAG_RAM_TOTAL, di.ram_total);
  if (di.has & NDP_DI_APP_MEM) {
    ndp_tlv_put_u64(&w, NDP_TAG_APP_MEM_TOTAL, di.app_mem_total);
    ndp_tlv_put_u64(&w, NDP_TAG_APP_MEM_FREE, di.app_mem_free);
  }
  if (di.has & NDP_DI_SYS_MEM) {
    ndp_tlv_put_u64(&w, NDP_TAG_SYS_MEM_TOTAL, di.sys_mem_total);
    ndp_tlv_put_u64(&w, NDP_TAG_SYS_MEM_FREE, di.sys_mem_free);
  }
  if (di.has & NDP_DI_SD) {
    ndp_tlv_put_u64(&w, NDP_TAG_SD_TOTAL, di.sd_total);
    ndp_tlv_put_u64(&w, NDP_TAG_SD_FREE, di.sd_free);
  }
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

/* INSTALL_URL: validates the URL, then hands it to the platform (which asks the owner on the console). */
static size_t do_install_url(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *u = NULL, *p = NULL, *h = NULL;
  size_t lu = 0, lp = 0, lh = 0, i;
  char url[NDP_URL_MAX + 1], path[NDP_PATH_MAX + 1], detail[96];
  const char *purl = NULL, *ppath = NULL;
  int rc, has_url, has_path;
  ndp_tlv_w w;
  if (!a->cfg.install_url) return ndp_agent_error(out, cap, req, NDP_ST_UNSUPPORTED_COMMAND, "unknown command", 0);
  has_url = ndp_tlv_find(pl, req->payload_len, NDP_TAG_URL, &u, &lu);
  has_path = ndp_tlv_find(pl, req->payload_len, NDP_TAG_PATH, &p, &lp);
  if (has_url == has_path) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "send either a url or a path", 0);
  if (has_url) {
    if (lu == 0 || lu > NDP_URL_MAX) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "url required", 0);
    for (i = 0; i < lu; i++)
      if (u[i] <= 0x20 || u[i] >= 0x7F) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "url must be printable ASCII without spaces", 0);
    memcpy(url, u, lu);
    url[lu] = '\0';
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
      return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "url must start with http:// or https://", 0);
    purl = url;
  } else {
    size_t n;
    if (ndp_path_normalize(p, lp, path, sizeof path) != NDP_OK || path[0] != '/')
      return ndp_agent_error(out, cap, req, NDP_ST_PATH_INVALID, "invalid path", 0);
    n = strlen(path);
    if (n < 6 || (path[n - 4] != '.') || (path[n - 3] | 0x20) != 'c' || (path[n - 2] | 0x20) != 'i' || (path[n - 1] | 0x20) != 'a')
      return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "only .cia files can be installed", 0);
    ppath = path;
  }
  if (ndp_tlv_find(pl, req->payload_len, NDP_TAG_SHA256, &h, &lh) && lh != 32)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "sha256 must be 32 bytes", 0);
  detail[0] = '\0';
  rc = a->cfg.install_url(a->cfg.install_url_ctx, purl, ppath, lh == 32 ? h : NULL, detail, sizeof detail);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail[0] ? detail : "the console cannot take this request now", 0);
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

static size_t handle_inner(ndp_agent *a, const ndp_header *req, const uint8_t *payload, uint8_t *out,
                           size_t cap) {
  if (req->version != NDP_PROTOCOL_VERSION)
    return ndp_agent_error(out, cap, req, NDP_ST_UNSUPPORTED_PROTOCOL, "unsupported frame version", 1);
  if (a->up.active || a->up.discard) {
    if ((req->kind == NDP_KIND_DATA || req->kind == NDP_KIND_END) && req->request_id == a->up.id)
      return ndp_agent_upload_frame(a, req, payload, out, cap);
    if (a->up.discard && req->kind == NDP_KIND_REQ) a->up.discard = 0; /* a new request resynchronizes */
  }
  if (req->kind != NDP_KIND_REQ) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "expected REQ", 0);
  if (!ndp_tlv_validate(payload, req->payload_len))
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "malformed payload", 0);
  if (a->xfer.active || a->up.active) return ndp_agent_error(out, cap, req, NDP_ST_BUSY, "transfer in progress", 0);
  if (req->command == NDP_CMD_HELLO) {
    if (a->sec.authed) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "already authenticated", 0);
    return do_hello(a, req, payload, out, cap);
  }
  if (!a->hello_done) return ndp_agent_error(out, cap, req, NDP_ST_HELLO_REQUIRED, "send HELLO first", 0);
  if (a->sec.required) {
    if (!a->sec.authed) { /* before AUTH only PAIR and AUTH are served (spec §4.6) */
      if (req->command == NDP_CMD_PAIR) return ndp_agent_pair(a, req, payload, out, cap);
      if (req->command == NDP_CMD_PAIR_BEGIN) return ndp_agent_pair_begin(a, req, payload, out, cap);
      if (req->command == NDP_CMD_PAIR_REVEAL) return ndp_agent_pair_reveal(a, req, payload, out, cap);
      if (req->command == NDP_CMD_PAIR_POLL) return ndp_agent_pair_poll(a, req, payload, out, cap);
      if (req->command == NDP_CMD_AUTH) return ndp_agent_auth(a, req, payload, out, cap);
      return ndp_agent_error(out, cap, req, NDP_ST_UNAUTHORIZED, "authentication required", 0);
    }
    if (req->command == NDP_CMD_PAIR || req->command == NDP_CMD_AUTH)
      return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "already authenticated", 0);
  }
  if (req->command == NDP_CMD_PING) return do_ping(req, payload, out, cap);
  if (req->command == NDP_CMD_ACCESS_INFO) return do_access_info(a, req, out, cap);
  if (req->command == NDP_CMD_DEVICE_INFO) return do_device_info(a, req, out, cap);
  if (req->command == NDP_CMD_INSTALL_URL) return do_install_url(a, req, payload, out, cap);
  if (req->command == NDP_CMD_PAIR_LIST) return ndp_agent_pair_list(a, req, out, cap);
  if (req->command == NDP_CMD_PAIR_FORGET) return ndp_agent_pair_forget(a, req, payload, out, cap);
  if (a->cfg.fs && (req->command == NDP_CMD_FS_LIST || req->command == NDP_CMD_FS_STAT ||
                    req->command == NDP_CMD_FS_READ))
    return ndp_agent_fs_handle(a, req, payload, out, cap);
  if (a->cfg.fs && a->cfg.fs->file_create) {
    if (req->command == NDP_CMD_FS_WRITE) return ndp_agent_write_start(a, req, payload, out, cap);
    if (req->command == NDP_CMD_FS_MKDIR) return ndp_agent_mkdir(a, req, payload, out, cap);
    if (req->command == NDP_CMD_FS_DELETE) return ndp_agent_delete(a, req, payload, out, cap);
    if (req->command == NDP_CMD_FS_RENAME) return ndp_agent_rename(a, req, payload, out, cap);
    if (req->command == NDP_CMD_FS_PURGE) return ndp_agent_purge(a, req, payload, out, cap);
    if (req->command == NDP_CMD_FS_COPY) return ndp_agent_copy(a, req, payload, out, cap);
  }
  return ndp_agent_error(out, cap, req, NDP_ST_UNSUPPORTED_COMMAND, "unknown command", 0);
}

size_t ndp_agent_handle_frame(ndp_agent *a, const ndp_header *hdr, const uint8_t *payload, const uint8_t *mac,
                              uint8_t *out, size_t cap) {
  size_t n;
  if (a->sec.authed) {
    /* every frame must be sealed with the next counter value (spec §4.5) */
    uint8_t hb[NDP_HEADER_SIZE], expect[NDP_MAC_SIZE];
    if (!mac || !(hdr->flags & NDP_FLAG_MAC)) return NDP_CLOSE;
    ndp_header_encode(hdr, hb);
    ndp_frame_mac(a->sec.session, 32, a->sec.recv_ctr, hb, payload, hdr->payload_len, expect);
    if (!ndp_ct_equal(expect, mac, NDP_MAC_SIZE)) return NDP_CLOSE;
    a->sec.recv_ctr++;
  } else if (mac) {
    return NDP_CLOSE; /* a MAC before AUTH cannot be verified: protocol violation */
  }
  n = handle_inner(a, hdr, payload, out, cap);
  if (a->sec.authed && n != NDP_NO_REPLY && n != NDP_CLOSE && n != 0) n = ndp_agent_seal(a, out, n, cap);
  return n;
}

size_t ndp_agent_handle(ndp_agent *a, const ndp_header *hdr, const uint8_t *payload, uint8_t *out, size_t cap) {
  return ndp_agent_handle_frame(a, hdr, payload, NULL, out, cap);
}

int ndp_agent_authenticated(const ndp_agent *a) { return a->sec.authed; }

int ndp_agent_streaming(const ndp_agent *a) { return a->xfer.active; }

size_t ndp_agent_next_frame(ndp_agent *a, uint8_t *out, size_t cap) {
  size_t n;
  if (!a->xfer.active) return 0;
  /* leave room for the MAC of a sealed session */
  n = ndp_agent_fs_next_frame(a, out, a->sec.authed && cap > NDP_MAC_SIZE ? cap - NDP_MAC_SIZE : cap);
  if (n && a->sec.authed) n = ndp_agent_seal(a, out, n, cap);
  return n;
}

int ndp_agent_busy(const ndp_agent *a) { return a->xfer.active || a->up.active; }

void ndp_agent_set_mode(ndp_agent *a, ndp_mode mode) {
  a->cfg.mode = mode;
  if (mode == NDP_MODE_READ_ONLY && a->up.active && a->cfg.fs) ndp_agent_upload_abort(a, 1);
}

void ndp_agent_close(ndp_agent *a) {
  if (a->cfg.fs && a->up.active) ndp_agent_upload_abort(a, 0);
  ndp_agent_fs_close(a);
}
