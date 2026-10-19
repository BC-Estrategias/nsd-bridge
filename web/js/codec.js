/* NDP v1 framing and TLV (docs/protocol/ndp-v1.md §2-3, §7): a port of bridge/packages/core/src/{constants,tlv,frame}.ts. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});

  const HEADER_SIZE = 20, MAC_SIZE = 16, PROTOCOL_VERSION = 1, DEFAULT_MAX_FRAME = 65536;
  const MAGIC = [0x4e, 0x44, 0x50, 0x31]; // "NDP1"
  const Kind = { REQ: 1, RES: 2, DATA: 3, END: 4, ERR: 5, EVT: 6 };
  const Flag = { MAC: 1, MORE: 2 };
  const Command = {
    HELLO: 1, PING: 2, PAIR: 4, AUTH: 5, DEVICE_INFO: 0x10, ACCESS_INFO: 0x11, FS_LIST: 0x20, FS_STAT: 0x21, FS_READ: 0x22,
    FS_WRITE: 0x30, FS_MKDIR: 0x31, FS_RENAME: 0x32, FS_DELETE: 0x33, FS_PURGE: 0x34, FS_COPY: 0x35, INSTALL_URL: 0x40, PAIR_BEGIN: 0x06, PAIR_REVEAL: 0x07, PAIR_POLL: 0x08,
    PAIR_LIST: 0x09, PAIR_FORGET: 0x0a,
  };
  const Status = {
    OK: 0, UNSUPPORTED_PROTOCOL: 1, UNAUTHORIZED: 2, FORBIDDEN_MODE: 3, PROTECTED_PATH: 4, NOT_FOUND: 5, EXISTS: 6, IO_ERROR: 7,
    NO_SPACE: 8, BAD_REQUEST: 9, BUSY: 10, TOO_LARGE: 11, HASH_MISMATCH: 12, TIMEOUT: 13, UNSUPPORTED_COMMAND: 14,
    HELLO_REQUIRED: 15, BAD_FRAME: 16, PATH_INVALID: 17,
  };
  const StatusName = Object.fromEntries(Object.entries(Status).map(([k, v]) => [v, k]));
  const Tag = {
    DETAIL: 0x01, OS_RESULT: 0x02, PROTOCOL: 0x10, PROTOCOL_MAX: 0x11, BRIDGE_NAME: 0x12, NONCE: 0x13, PLATFORM: 0x14,
    AGENT_VERSION: 0x15, AUTH: 0x16, MODE: 0x17, MAX_FRAME: 0x18, PING_NONCE: 0x20, PATH: 0x30, CURSOR: 0x31, NEXT_CURSOR: 0x32,
    ENTRY: 0x33, TYPE: 0x34, SIZE: 0x35, MTIME: 0x36, OFFSET: 0x37, LENGTH: 0x38, CHUNK: 0x39, WANT_HASH: 0x3a, SHA256: 0x3b,
    TOTAL_SIZE: 0x3c, WILL_SEND: 0x3d, LIST_MORE: 0x3e, OVERWRITE: 0x40, BACKUP: 0x41, REPLACED: 0x43, WRITTEN: 0x44,
    MAX_CHUNK: 0x45, TRASH_PATH: 0x46, PAIRED_KEYS: 0x47, PAIRING_OPEN: 0x48, DEVICE_ID: 0x49, KEY_ID: 0x4a, PROOF: 0x4b,
    LABEL: 0x4c, READ_ROOT: 0x4d, WRITE_ROOT: 0x4e, MODEL: 0x4f, FIRMWARE: 0x50, RAM_TOTAL: 0x51, APP_MEM_TOTAL: 0x52,
    APP_MEM_FREE: 0x53, SYS_MEM_TOTAL: 0x54, SYS_MEM_FREE: 0x55, SD_TOTAL: 0x56, SD_FREE: 0x57, NEW_PATH: 0x58, PURGED: 0x59, MORE: 0x5a, COPIED: 0x5b, COPY_MODE: 0x5c, PAIR_COMMIT: 0x60, PAIR_PUB: 0x61, PAIR_NONCE: 0x62, PAIR_STATE: 0x63, PAIR_ENTRY: 0x64, URL: 0x65,
  };

  /** An ERR frame, or a protocol/transport failure. */
  class NdpError extends Error {
    constructor(status, detail, message) {
      super(message || `${StatusName[status] || "ERROR"}${detail ? ": " + detail : ""}`);
      this.name = "NdpError";
      this.status = status;
      this.statusName = StatusName[status] || `UNKNOWN_${status}`;
      this.detail = detail;
    }
  }
  class NdpTransportError extends Error {
    constructor(message) { super(message); this.name = "NdpTransportError"; }
  }

  const enc = new TextEncoder(), dec = new TextDecoder("utf-8");
  const utf8 = (s) => enc.encode(s);
  const text = (b) => dec.decode(b);
  const concat = (...parts) => {
    let n = 0;
    for (const p of parts) n += p.length;
    const out = new Uint8Array(n);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
  };
  const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
  const fromHex = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));
  const equal = (a, b) => {
    if (a.length !== b.length) return false;
    let d = 0;
    for (let i = 0; i < a.length; i++) d |= a[i] ^ b[i];
    return d === 0;
  };

  // ---- TLV: [tag u16 LE][len u16 LE][value]
  const u8 = (v) => new Uint8Array([v & 255]);
  const u16 = (v) => new Uint8Array([v & 255, (v >> 8) & 255]);
  const u32 = (v) => { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v, true); return b; };
  const u64 = (v) => { const b = new Uint8Array(8); new DataView(b.buffer).setBigUint64(0, BigInt(v), true); return b; };
  const str = (s) => utf8(s);

  function encodeTlv(fields) {
    const parts = [];
    for (const [tag, value] of fields) {
      if (value.length > 0xffff) throw new RangeError("TLV value too long");
      const h = new Uint8Array(4);
      new DataView(h.buffer).setUint16(0, tag, true);
      new DataView(h.buffer).setUint16(2, value.length, true);
      parts.push(h, value);
    }
    return concat(...parts);
  }

  class Tlv {
    constructor(fields) { this.fields = fields; }
    first(tag) { const f = this.fields.find((x) => x[0] === tag); return f ? f[1] : undefined; }
    all(tag) { return this.fields.filter((x) => x[0] === tag).map((x) => x[1]); }
    str(tag) { const v = this.first(tag); return v === undefined ? undefined : text(v); }
    u8(tag) { const v = this.first(tag); return v && v.length === 1 ? v[0] : undefined; }
    u16(tag) { const v = this.first(tag); return v && v.length === 2 ? new DataView(v.buffer, v.byteOffset).getUint16(0, true) : undefined; }
    u32(tag) { const v = this.first(tag); return v && v.length === 4 ? new DataView(v.buffer, v.byteOffset).getUint32(0, true) : undefined; }
    /** Numbers up to 2^53 (SD sizes, file sizes): plenty for the SD card of a console. */
    u64(tag) { const v = this.first(tag); return v && v.length === 8 ? Number(new DataView(v.buffer, v.byteOffset).getBigUint64(0, true)) : undefined; }
  }

  function parseTlv(b) {
    const fields = [];
    let o = 0;
    const dv = new DataView(b.buffer, b.byteOffset, b.length);
    while (o < b.length) {
      if (o + 4 > b.length) throw new NdpTransportError("malformed TLV");
      const tag = dv.getUint16(o, true), len = dv.getUint16(o + 2, true);
      o += 4;
      if (o + len > b.length) throw new NdpTransportError("malformed TLV");
      fields.push([tag, b.subarray(o, o + len)]);
      o += len;
    }
    return new Tlv(fields);
  }

  // ---- frames
  function encodeHeader(h) {
    const out = new Uint8Array(HEADER_SIZE);
    const dv = new DataView(out.buffer);
    out.set(MAGIC, 0);
    out[4] = h.version === undefined ? PROTOCOL_VERSION : h.version;
    out[5] = h.kind;
    dv.setUint16(6, h.flags || 0, true);
    dv.setUint32(8, h.requestId >>> 0, true);
    dv.setUint16(12, h.command || 0, true);
    dv.setUint16(14, h.status || 0, true);
    dv.setUint32(16, h.payloadLength, true);
    return out;
  }

  function decodeHeader(b) {
    for (let i = 0; i < 4; i++) if (b[i] !== MAGIC[i]) throw new NdpTransportError("bad magic");
    const dv = new DataView(b.buffer, b.byteOffset, HEADER_SIZE);
    const flags = dv.getUint16(6, true);
    if (flags & ~(Flag.MAC | Flag.MORE)) throw new NdpTransportError("unknown frame flags");
    return {
      version: b[4], kind: b[5], flags, requestId: dv.getUint32(8, true), command: dv.getUint16(12, true),
      status: dv.getUint16(14, true), payloadLength: dv.getUint32(16, true),
    };
  }

  /** Streaming decoder: push() bytes, get the complete frames {header, payload, mac}. */
  class FrameDecoder {
    constructor(maxPayload) { this.maxPayload = maxPayload; this.buf = new Uint8Array(0); }
    push(chunk) {
      this.buf = this.buf.length ? concat(this.buf, chunk) : chunk;
      const frames = [];
      for (;;) {
        if (this.buf.length < HEADER_SIZE) break;
        const header = decodeHeader(this.buf);
        if (header.payloadLength > this.maxPayload) throw new NdpTransportError("frame payload too large");
        const macLen = header.flags & Flag.MAC ? MAC_SIZE : 0;
        const total = HEADER_SIZE + header.payloadLength + macLen;
        if (this.buf.length < total) break;
        frames.push({
          header, raw: this.buf.slice(0, HEADER_SIZE),
          payload: this.buf.slice(HEADER_SIZE, HEADER_SIZE + header.payloadLength),
          mac: macLen ? this.buf.slice(HEADER_SIZE + header.payloadLength, total) : null,
        });
        this.buf = this.buf.subarray(total);
      }
      return frames;
    }
  }

  NDP.codec = {
    HEADER_SIZE, MAC_SIZE, PROTOCOL_VERSION, DEFAULT_MAX_FRAME, Kind, Flag, Command, Status, StatusName, Tag, NdpError, NdpTransportError,
    utf8, text, concat, hex, fromHex, equal, u8, u16, u32, u64, str, encodeTlv, parseTlv, encodeHeader, decodeHeader, FrameDecoder,
  };
})();
