/* NDP v1 — constants shared by every module. See docs/protocol/ndp-v1.md. */
#ifndef NDP_DEFS_H
#define NDP_DEFS_H

#include <stddef.h>
#include <stdint.h>

#define NDP_AGENT_VERSION "2.0.0"
#define NDP_PROTOCOL_VERSION 1
#define NDP_HEADER_SIZE 20
#define NDP_MAC_SIZE 16
#define NDP_DEFAULT_PORT 6464
#define NDP_DEFAULT_MAX_FRAME 65536u

enum ndp_kind {
  NDP_KIND_REQ = 1,
  NDP_KIND_RES = 2,
  NDP_KIND_DATA = 3,
  NDP_KIND_END = 4,
  NDP_KIND_ERR = 5,
  NDP_KIND_EVT = 6
};

#define NDP_FLAG_MAC 0x0001u
#define NDP_FLAG_MORE 0x0002u
#define NDP_FLAGS_KNOWN (NDP_FLAG_MAC | NDP_FLAG_MORE)

enum ndp_command {
  NDP_CMD_HELLO = 0x0001,
  NDP_CMD_PING = 0x0002,
  NDP_CMD_PAIR = 0x0004,
  NDP_CMD_PAIR_BEGIN = 0x0006,
  NDP_CMD_PAIR_REVEAL = 0x0007,
  NDP_CMD_PAIR_POLL = 0x0008,
  NDP_CMD_PAIR_LIST = 0x0009,
  NDP_CMD_PAIR_FORGET = 0x000A,
  NDP_CMD_AUTH = 0x0005,
  NDP_CMD_DEVICE_INFO = 0x0010,
  NDP_CMD_ACCESS_INFO = 0x0011,
  NDP_CMD_FS_LIST = 0x0020,
  NDP_CMD_FS_STAT = 0x0021,
  NDP_CMD_FS_READ = 0x0022,
  NDP_CMD_FS_WRITE = 0x0030,
  NDP_CMD_FS_MKDIR = 0x0031,
  NDP_CMD_FS_RENAME = 0x0032,
  NDP_CMD_FS_DELETE = 0x0033,
  NDP_CMD_FS_PURGE = 0x0034,
  NDP_CMD_FS_COPY = 0x0035,
  NDP_CMD_INSTALL_URL = 0x0040 /* asks the console to offer installing a title from a URL (the owner confirms on the console) */
};

enum ndp_fs_type { NDP_TYPE_FILE = 1, NDP_TYPE_DIR = 2 };

#define NDP_URL_MAX 1023u
#define NDP_DEFAULT_CHUNK 32768u
#define NDP_MIN_CHUNK 512u
#define NDP_LIST_PAGE_MAX 100

/* Status codes (spec §6). Functions in this library return these directly. */
enum ndp_status {
  NDP_OK = 0,
  NDP_ST_UNSUPPORTED_PROTOCOL = 1,
  NDP_ST_UNAUTHORIZED = 2,
  NDP_ST_FORBIDDEN_MODE = 3,
  NDP_ST_PROTECTED_PATH = 4,
  NDP_ST_NOT_FOUND = 5,
  NDP_ST_EXISTS = 6,
  NDP_ST_IO_ERROR = 7,
  NDP_ST_NO_SPACE = 8,
  NDP_ST_BAD_REQUEST = 9,
  NDP_ST_BUSY = 10,
  NDP_ST_TOO_LARGE = 11,
  NDP_ST_HASH_MISMATCH = 12,
  NDP_ST_TIMEOUT = 13,
  NDP_ST_UNSUPPORTED_COMMAND = 14,
  NDP_ST_HELLO_REQUIRED = 15,
  NDP_ST_BAD_FRAME = 16,
  NDP_ST_PATH_INVALID = 17
};

/* TLV tags (spec §7). */
enum ndp_tag {
  NDP_TAG_DETAIL = 0x0001,
  NDP_TAG_OS_RESULT = 0x0002,
  NDP_TAG_PROTOCOL = 0x0010, /* HELLO REQ: protocol_min; HELLO RES: chosen version */
  NDP_TAG_PROTOCOL_MAX = 0x0011,
  NDP_TAG_BRIDGE_NAME = 0x0012,
  NDP_TAG_NONCE = 0x0013,
  NDP_TAG_PLATFORM = 0x0014,
  NDP_TAG_AGENT_VERSION = 0x0015,
  NDP_TAG_AUTH = 0x0016,
  NDP_TAG_MODE = 0x0017,
  NDP_TAG_MAX_FRAME = 0x0018,
  NDP_TAG_SUPPORTED_MIN = 0x0019,
  NDP_TAG_SUPPORTED_MAX = 0x001A,
  NDP_TAG_PING_NONCE = 0x0020,
  NDP_TAG_PATH = 0x0030,
  NDP_TAG_CURSOR = 0x0031,
  NDP_TAG_NEXT_CURSOR = 0x0032,
  NDP_TAG_ENTRY = 0x0033,
  NDP_TAG_TYPE = 0x0034,
  NDP_TAG_SIZE = 0x0035,
  NDP_TAG_MTIME = 0x0036,
  NDP_TAG_OFFSET = 0x0037,
  NDP_TAG_LENGTH = 0x0038,
  NDP_TAG_CHUNK = 0x0039,
  NDP_TAG_WANT_HASH = 0x003A,
  NDP_TAG_SHA256 = 0x003B,
  NDP_TAG_TOTAL_SIZE = 0x003C,
  NDP_TAG_WILL_SEND = 0x003D,
  NDP_TAG_LIST_MORE = 0x003E,
  NDP_TAG_OVERWRITE = 0x0040,
  NDP_TAG_BACKUP = 0x0041,
  NDP_TAG_REPLACED = 0x0043,
  NDP_TAG_WRITTEN = 0x0044,
  NDP_TAG_MAX_CHUNK = 0x0045,
  NDP_TAG_TRASH_PATH = 0x0046,
  NDP_TAG_PAIRED_KEYS = 0x0047,
  NDP_TAG_PAIRING_OPEN = 0x0048,
  NDP_TAG_DEVICE_ID = 0x0049,
  NDP_TAG_KEY_ID = 0x004A,
  NDP_TAG_PROOF = 0x004B,
  NDP_TAG_LABEL = 0x004C,
  NDP_TAG_READ_ROOT = 0x004D,
  NDP_TAG_WRITE_ROOT = 0x004E,
  NDP_TAG_MODEL = 0x004F,
  NDP_TAG_FIRMWARE = 0x0050,
  NDP_TAG_RAM_TOTAL = 0x0051,
  NDP_TAG_APP_MEM_TOTAL = 0x0052,
  NDP_TAG_APP_MEM_FREE = 0x0053,
  NDP_TAG_SYS_MEM_TOTAL = 0x0054,
  NDP_TAG_SYS_MEM_FREE = 0x0055,
  NDP_TAG_SD_TOTAL = 0x0056,
  NDP_TAG_SD_FREE = 0x0057,
  NDP_TAG_NEW_PATH = 0x0058,
  NDP_TAG_PURGED = 0x0059,
  NDP_TAG_MORE = 0x005A,
  NDP_TAG_COPIED = 0x005B,
  NDP_TAG_COPY_MODE = 0x005C,
  NDP_TAG_PAIR_COMMIT = 0x0060,
  NDP_TAG_PAIR_PUB = 0x0061,
  NDP_TAG_PAIR_NONCE = 0x0062,
  NDP_TAG_PAIR_STATE = 0x0063,
  NDP_TAG_PAIR_ENTRY = 0x0064, /* PAIR_LIST response, one per paired key: key_id(4) || label (UTF-8, no NUL) */
  NDP_TAG_URL = 0x0065, /* INSTALL_URL REQ: an http:// or https:// URL (UTF-8, no NUL, at most NDP_URL_MAX bytes) */
  NDP_TAG_SEED = 0x0066 /* INSTALL_URL REQ: optional 16-byte seed the title needs (registered with the system after the install) */
};

/* Entries a filtered (traversal) FS_LIST reads per response, shown or not. */
#define NDP_TRAVERSAL_SCAN_MAX 64

/* Returned by ndp_agent_handle for frames that need no response (e.g. DATA frames of an upload). */
#define NDP_NO_REPLY ((size_t)-1)
/* Returned by ndp_agent_handle_frame when the connection must be closed without a reply
 * (failed MAC, unsealed frame after AUTH, too many authentication failures). */
#define NDP_CLOSE ((size_t)-2)

#endif
