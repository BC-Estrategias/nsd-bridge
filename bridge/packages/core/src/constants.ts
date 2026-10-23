// NDP v1 constants — mirror of docs/protocol/ndp-v1.md and agent/common/include/ndp/ndp_defs.h.

export const MAGIC = new Uint8Array([0x4e, 0x44, 0x50, 0x31]); // "NDP1"
export const PROTOCOL_VERSION = 1;
export const HEADER_SIZE = 20;
export const MAC_SIZE = 16;
export const DEFAULT_PORT = 6464;
export const DEFAULT_MAX_FRAME = 65536;

export const Kind = { REQ: 1, RES: 2, DATA: 3, END: 4, ERR: 5, EVT: 6 } as const;

export const Flag = { MAC: 0x0001, MORE: 0x0002 } as const;
export const FLAGS_KNOWN = Flag.MAC | Flag.MORE;

export const Command = {
  HELLO: 0x0001,
  PING: 0x0002,
  PAIR: 0x0004,
  AUTH: 0x0005,
  DEVICE_INFO: 0x0010,
  ACCESS_INFO: 0x0011,
  FS_LIST: 0x0020,
  FS_STAT: 0x0021,
  FS_READ: 0x0022,
  FS_WRITE: 0x0030,
  FS_MKDIR: 0x0031,
  FS_RENAME: 0x0032,
  FS_DELETE: 0x0033,
  FS_PURGE: 0x0034,
  FS_COPY: 0x0035,
  INSTALL_URL: 0x0040,
  SOURCE_LINK: 0x0041,
  PAIR_BEGIN: 0x0006,
  PAIR_REVEAL: 0x0007,
  PAIR_POLL: 0x0008,
  PAIR_LIST: 0x0009,
  PAIR_FORGET: 0x000a,
} as const;

export const FsType = { FILE: 1, DIR: 2 } as const;
export const DEFAULT_CHUNK = 32768;
export const MIN_CHUNK = 512;

export const Status = {
  OK: 0,
  UNSUPPORTED_PROTOCOL: 1,
  UNAUTHORIZED: 2,
  FORBIDDEN_MODE: 3,
  PROTECTED_PATH: 4,
  NOT_FOUND: 5,
  EXISTS: 6,
  IO_ERROR: 7,
  NO_SPACE: 8,
  BAD_REQUEST: 9,
  BUSY: 10,
  TOO_LARGE: 11,
  HASH_MISMATCH: 12,
  TIMEOUT: 13,
  UNSUPPORTED_COMMAND: 14,
  HELLO_REQUIRED: 15,
  BAD_FRAME: 16,
  PATH_INVALID: 17,
} as const;

export type StatusName = keyof typeof Status;

export function statusName(code: number): StatusName | `UNKNOWN_${number}` {
  for (const [name, value] of Object.entries(Status)) if (value === code) return name as StatusName;
  return `UNKNOWN_${code}`;
}

export const Tag = {
  DETAIL: 0x0001,
  OS_RESULT: 0x0002,
  PROTOCOL: 0x0010, // HELLO REQ: protocol_min; HELLO RES: chosen version
  PROTOCOL_MAX: 0x0011,
  BRIDGE_NAME: 0x0012,
  NONCE: 0x0013,
  PLATFORM: 0x0014,
  AGENT_VERSION: 0x0015,
  AUTH: 0x0016,
  MODE: 0x0017,
  MAX_FRAME: 0x0018,
  SUPPORTED_MIN: 0x0019,
  SUPPORTED_MAX: 0x001a,
  PING_NONCE: 0x0020,
  PATH: 0x0030,
  CURSOR: 0x0031,
  NEXT_CURSOR: 0x0032,
  ENTRY: 0x0033,
  TYPE: 0x0034,
  SIZE: 0x0035,
  MTIME: 0x0036,
  OFFSET: 0x0037,
  LENGTH: 0x0038,
  CHUNK: 0x0039,
  WANT_HASH: 0x003a,
  SHA256: 0x003b,
  TOTAL_SIZE: 0x003c,
  WILL_SEND: 0x003d,
  LIST_MORE: 0x003e,
  OVERWRITE: 0x0040,
  BACKUP: 0x0041,
  REPLACED: 0x0043,
  WRITTEN: 0x0044,
  MAX_CHUNK: 0x0045,
  TRASH_PATH: 0x0046,
  PAIRED_KEYS: 0x0047,
  PAIRING_OPEN: 0x0048,
  DEVICE_ID: 0x0049,
  KEY_ID: 0x004a,
  PROOF: 0x004b,
  LABEL: 0x004c,
  READ_ROOT: 0x004d,
  WRITE_ROOT: 0x004e,
  MODEL: 0x004f,
  FIRMWARE: 0x0050,
  RAM_TOTAL: 0x0051,
  APP_MEM_TOTAL: 0x0052,
  APP_MEM_FREE: 0x0053,
  SYS_MEM_TOTAL: 0x0054,
  SYS_MEM_FREE: 0x0055,
  SD_TOTAL: 0x0056,
  SD_FREE: 0x0057,
  NEW_PATH: 0x0058,
  PURGED: 0x0059,
  MORE: 0x005a,
  COPIED: 0x005b,
  COPY_MODE: 0x005c,
  PAIR_COMMIT: 0x0060,
  PAIR_PUB: 0x0061,
  PAIR_NONCE: 0x0062,
  PAIR_STATE: 0x0063,
  PAIR_ENTRY: 0x0064,
  URL: 0x0065,
  SEED: 0x0066,
} as const;

export type Mode = "READ_ONLY" | "DEVELOPMENT" | "FULL";
export const MODES: readonly Mode[] = ["READ_ONLY", "DEVELOPMENT", "FULL"];
