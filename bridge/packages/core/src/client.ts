import { createHash, randomBytes } from "node:crypto";
import { createReadStream } from "node:fs";
import { stat as statLocal } from "node:fs/promises";
import { connect as tcpConnect, type Socket } from "node:net";
import { performance } from "node:perf_hooks";
import {
  Command, DEFAULT_CHUNK, DEFAULT_MAX_FRAME, DEFAULT_PORT, Flag, FsType, Kind, MIN_CHUNK, PROTOCOL_VERSION, Status, Tag,
  type Mode, MODES,
} from "./constants.ts";
import { NdpProtocolError, NdpRemoteError, NdpTransportError } from "./errors.ts";
import { bytesEqual, keyIdOf, LABEL_MAX, proofOf, sessionKeyOf, derivePsk, type KeyStore } from "./auth.ts";
import { encodeFrame, encodeHeader, frameMac, FrameDecoder, type Frame, type FrameInit } from "./frame.ts";
import { formatSas, PAIR2_NONCE, pair2Commit, pair2Derive, pair2OkProof, PairingRefusedError, randomNonce, randomSecret, x25519Public, x25519Shared } from "./pair2.ts";
import { normalizePath } from "./path.ts";
import { encodeTlv, parseTlv, str, u8, u16, u32, u64, type TlvField } from "./tlv.ts";

export interface ConnectOptions {
  host: string;
  port?: number;
  /** Per-attempt timeout. */
  connectTimeoutMs?: number;
  /** Attempts for transient failures (host unreachable / timeout). ECONNREFUSED is never retried. Default 3. */
  attempts?: number;
  requestTimeoutMs?: number;
  bridgeName?: string;
}

export interface FsEntry {
  name: string;
  type: "file" | "dir";
  size: number;
}

export interface FsStat {
  type: "file" | "dir";
  size: number;
  /** Seconds since the Unix epoch, or null when the device does not know. */
  mtime: number | null;
}

export interface ReadOptions {
  offset?: number;
  /** Bytes to read (0 or undefined = to the end of the file). */
  length?: number;
  chunk?: number;
  /** Ask the agent for a SHA-256 of the bytes it sent and verify it locally (default true). */
  verify?: boolean;
  onProgress?: (bytes: number, total: number) => void;
}

export interface ReadResult {
  /** File size on the device (not the number of bytes read). */
  totalSize: number;
  bytes: number;
  sha256: Uint8Array | null;
  verified: boolean;
  ms: number;
}

export interface WriteOptions {
  /** Replace an existing file (default false: fails with EXISTS). */
  overwrite?: boolean;
  /** With overwrite: keep the previous version as `<name>.bak`. */
  backup?: boolean;
  chunk?: number;
  onProgress?: (bytes: number, total: number) => void;
}

export interface WriteResult {
  written: number;
  sha256: Uint8Array;
  replaced: boolean;
  ms: number;
}

/** What the console reports about itself (agent >= 1.1.0). A field is absent when the console could not measure it. */
export interface DeviceInfo {
  /** "New Nintendo 3DS XL", "Nintendo 2DS", ... */
  model?: string;
  /** System version as shown in Settings ("11.17.0-50U"), or the kernel version when that is not readable. */
  firmware?: string;
  /** Physical RAM of the model: 128 MiB (old) or 256 MiB (new). */
  ramTotal?: number;
  /** Memory region of the foreground app (where the agent itself runs), bytes. */
  appMemory?: { total: number; free: number };
  /** SYSTEM memory region, bytes. */
  systemMemory?: { total: number; free: number };
  /** The SD card, bytes. */
  sd?: { total: number; free: number };
}

/** What the console's owner has opened to this computer (they choose it ON the console). */
export interface AccessInfo {
  mode: Mode;
  /** Folders readable (recursively). Directories above them can be listed only to navigate down (spec §11.1). */
  readRoots: string[];
  /** Folders writable when the mode allows writes. Always a subset of readRoots. */
  writeRoots: string[];
}

export interface HelloInfo {
  protocol: number;
  platform: string;
  agentVersion: string;
  deviceNonce: Uint8Array;
  auth: string;
  mode: Mode;
  maxFrame: number;
  /** Console identity (present when the agent requires authentication). */
  deviceId?: Uint8Array;
  /** How many computers the console is paired with. */
  pairedKeys?: number;
  /** True while the console's pairing window is open. */
  pairingOpen?: boolean;
}

/** Sealed-channel state after a successful AUTH (spec §4.5). */
interface Session {
  key: Uint8Array;
  sendCtr: bigint;
  recvCtr: bigint;
}

/** Operations that create/commit things on the SD card can legitimately take several seconds. */
const SLOW_FS_TIMEOUT_MS = 30_000;
const WRITE_COMMIT_TIMEOUT_MS = 60_000;

const TRANSIENT_CONNECT_CODES = new Set(["EHOSTUNREACH", "EHOSTDOWN", "ENETUNREACH", "ETIMEDOUT"]);

/** True for connect failures worth retrying. ECONNREFUSED (nothing listening) is deliberately not one. */
export function isTransientConnectError(code: string | undefined): boolean {
  return code !== undefined && TRANSIENT_CONNECT_CODES.has(code);
}

interface Waiter {
  resolve: (f: Frame) => void;
  reject: (e: Error) => void;
  timer: NodeJS.Timeout;
}

/** One connection, one request at a time (spec §1). */
export class NdpClient {
  readonly #socket: Socket;
  readonly #requestTimeoutMs: number;
  readonly #bridgeName: string;
  #decoder = new FrameDecoder(DEFAULT_MAX_FRAME);
  #frames: Frame[] = [];
  #waiter: Waiter | null = null;
  #failure: Error | null = null;
  #nextId = 0;
  #chain: Promise<unknown> = Promise.resolve();
  #info: HelloInfo | null = null;
  #cn: Uint8Array | null = null;
  /** Set once AUTH succeeded: from then on every frame in both directions carries a MAC. */
  #session: Session | null = null;
  /** Set while the AUTH request is in flight: its RES is the first sealed frame. */
  #pending: Session | null = null;

  private constructor(socket: Socket, opts: ConnectOptions) {
    this.#socket = socket;
    this.#requestTimeoutMs = opts.requestTimeoutMs ?? 5000;
    this.#bridgeName = opts.bridgeName ?? "ndev/0.1.0";
    socket.on("data", (chunk) => {
      const b = typeof chunk === "string" ? Buffer.from(chunk) : chunk;
      this.#onData(new Uint8Array(b.buffer, b.byteOffset, b.length));
    });
    socket.on("error", (e) => this.#fail(new NdpTransportError(`socket error: ${e.message}`)));
    socket.on("close", () => this.#fail(new NdpTransportError("connection closed")));
  }

  /**
   * Connects, retrying transient failures with a growing pause. Measured on a real 3DS: the first
   * connection after a quiet period can fail with EHOSTUNREACH (ARP not resolved while the console's
   * Wi-Fi radio is in power-save) and succeed on the next attempt.
   */
  static async connect(opts: ConnectOptions): Promise<NdpClient> {
    const attempts = Math.max(1, opts.attempts ?? 3);
    for (let i = 1; ; i++) {
      try {
        return await NdpClient.#connectOnce(opts);
      } catch (e) {
        if (i >= attempts || !(e instanceof NdpTransportError) || !isTransientConnectError(e.code)) throw e;
        await new Promise((r) => setTimeout(r, 250 * 2 ** (i - 1)));
      }
    }
  }

  /**
   * connect + hello + (when the console requires it) authenticate with the key stored for that console.
   * Throws NdpRemoteError(UNAUTHORIZED) with an actionable message when this computer is not paired
   * or the console forgot the pairing.
   */
  static async open(opts: ConnectOptions, keys?: KeyStore): Promise<{ client: NdpClient; info: HelloInfo }> {
    const client = await NdpClient.connect(opts);
    try {
      const info = await client.hello();
      if (info.auth === "required") {
        const known = info.deviceId && keys ? keys.find(info.deviceId) : undefined;
        if (!known)
          throw new NdpRemoteError(
            Status.UNAUTHORIZED,
            `this computer is not paired with the console${info.pairingOpen ? " (its pairing window is open: run 'ndev pair <host>')" : " (press Y on the console to open its pairing window, then run 'ndev pair <host>')"}`,
          );
        try {
          await client.authenticate(new Uint8Array(Buffer.from(known.psk, "hex")));
        } catch (e) {
          if (e instanceof NdpRemoteError && e.status === Status.UNAUTHORIZED)
            throw new NdpRemoteError(
              Status.UNAUTHORIZED,
              "the console rejected the stored pairing key (it was probably reset): press Y on the console and run 'ndev pair <host>' again",
            );
          throw e;
        }
        if (known.lastHost !== opts.host) keys?.save({ ...known, lastHost: opts.host });
      }
      return { client, info };
    } catch (e) {
      client.close();
      throw e;
    }
  }

  static #connectOnce(opts: ConnectOptions): Promise<NdpClient> {
    const port = opts.port ?? DEFAULT_PORT;
    const timeoutMs = opts.connectTimeoutMs ?? 5000;
    return new Promise((resolve, reject) => {
      const socket = tcpConnect({ host: opts.host, port });
      socket.setNoDelay(true);
      const timer = setTimeout(() => {
        socket.destroy();
        reject(new NdpTransportError(`connect to ${opts.host}:${port} timed out after ${timeoutMs} ms`, "ETIMEDOUT"));
      }, timeoutMs);
      socket.once("connect", () => {
        clearTimeout(timer);
        socket.removeAllListeners("error");
        resolve(new NdpClient(socket, opts));
      });
      socket.once("error", (e) => {
        clearTimeout(timer);
        reject(new NdpTransportError(`cannot connect to ${opts.host}:${port}: ${e.message}`, (e as NodeJS.ErrnoException).code));
      });
    });
  }

  get info(): HelloInfo | null {
    return this.#info;
  }

  /** True once the connection failed or was closed (e.g. the agent dropped it after its idle timeout). */
  get isClosed(): boolean {
    return this.#failure !== null || this.#socket.destroyed;
  }

  close(): void {
    this.#socket.destroy();
  }

  #onData(chunk: Uint8Array): void {
    let frames: Frame[];
    try {
      frames = this.#decoder.push(chunk);
    } catch (e) {
      this.#fail(e instanceof Error ? e : new NdpTransportError(String(e)));
      this.#socket.destroy();
      return;
    }
    for (const f of frames) {
      if (!this.#verifyIncoming(f)) {
        this.#fail(new NdpProtocolError(Status.BAD_FRAME, "frame authentication failed (missing or invalid MAC)"));
        this.#socket.destroy();
        return;
      }
      this.#frames.push(f);
    }
    this.#deliver();
  }

  /** Checks the MAC and counter of a frame arriving on an authenticated (or authenticating) channel. */
  #verifyIncoming(f: Frame): boolean {
    const s = this.#session ?? this.#pending;
    if (!s) return f.mac === null; // an agent must not send MACs before AUTH
    if (!f.mac) {
      // While AUTH is in flight a refusal (ERR) legitimately comes unsealed; nothing else may.
      return !this.#session && f.header.kind === Kind.ERR;
    }
    const expect = frameMac(s.key, s.recvCtr, encodeHeader(f.header), f.payload);
    if (!bytesEqual(expect, f.mac)) return false;
    s.recvCtr += 1n;
    if (!this.#session) {
      // First sealed frame = the AUTH RES: the channel is now authenticated.
      this.#session = s;
      this.#pending = null;
    }
    return true;
  }

  /** encodeFrame that seals the frame once the channel is authenticated. */
  #encode(init: FrameInit): Uint8Array {
    const s = this.#session;
    if (!s) return encodeFrame(init);
    const payload = init.payload ?? new Uint8Array(0);
    const header = encodeHeader({
      version: init.version ?? PROTOCOL_VERSION, kind: init.kind, flags: (init.flags ?? 0) | Flag.MAC,
      requestId: init.requestId, command: init.command, status: init.status ?? 0, payloadLength: payload.length,
    });
    const mac = frameMac(s.key, s.sendCtr, header, payload);
    s.sendCtr += 1n;
    return encodeFrame({ ...init, mac });
  }

  #deliver(): void {
    if (this.#waiter && this.#frames.length) {
      const w = this.#waiter;
      this.#waiter = null;
      clearTimeout(w.timer);
      w.resolve(this.#frames.shift() as Frame);
    }
  }

  #fail(e: Error): void {
    if (!this.#failure) this.#failure = e;
    if (this.#waiter) {
      const w = this.#waiter;
      this.#waiter = null;
      clearTimeout(w.timer);
      w.reject(this.#failure);
    }
  }

  #nextFrame(timeoutMs: number): Promise<Frame> {
    if (this.#frames.length) return Promise.resolve(this.#frames.shift() as Frame);
    if (this.#failure) return Promise.reject(this.#failure);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#waiter = null;
        reject(new NdpTransportError(`no response within ${timeoutMs} ms`));
      }, timeoutMs);
      this.#waiter = { resolve, reject, timer };
    });
  }

  /** Writes with backpressure. Never waits on a dead socket: a close/error while waiting rejects. */
  async #writeFrame(bytes: Uint8Array): Promise<void> {
    if (this.#failure) throw this.#failure;
    if (this.#socket.destroyed) throw new NdpTransportError("connection closed");
    if (this.#socket.write(bytes)) return;
    await new Promise<void>((resolve, reject) => {
      const done = (err?: Error) => {
        this.#socket.off("drain", onDrain);
        this.#socket.off("close", onClose);
        this.#socket.off("error", onError);
        if (err) reject(err);
        else resolve();
      };
      const onDrain = () => done();
      const onClose = () => done(this.#failure ?? new NdpTransportError("connection closed"));
      const onError = (e: Error) => done(this.#failure ?? new NdpTransportError(`socket error: ${e.message}`));
      this.#socket.once("drain", onDrain);
      this.#socket.once("close", onClose);
      this.#socket.once("error", onError);
    });
  }

  #exclusive<T>(fn: () => Promise<T>): Promise<T> {
    const result = this.#chain.then(fn, fn);
    this.#chain = result.catch(() => undefined);
    return result;
  }

  #sendRequest(command: number, payload: Uint8Array): number {
    if (this.#failure) throw this.#failure;
    const id = (this.#nextId = (this.#nextId + 1) >>> 0 || 1);
    this.#socket.write(this.#encode({ kind: Kind.REQ, requestId: id, command, payload }));
    return id;
  }

  /** Waits for the RES that answers `id`; an ERR frame rejects with NdpRemoteError. */
  async #awaitResponse(id: number, timeoutMs: number = this.#requestTimeoutMs): Promise<Frame> {
    for (;;) {
      const f = await this.#nextFrame(timeoutMs);
      if (f.header.requestId !== id) continue; // stale frame from an abandoned request
      if (f.header.kind === Kind.ERR) throw this.#remoteError(f);
      if (f.header.kind !== Kind.RES)
        throw new NdpProtocolError(f.header.status, `unexpected frame kind ${f.header.kind}`);
      return f;
    }
  }

  #remoteError(f: Frame): NdpRemoteError {
    const tlv = parseTlv(f.payload);
    return new NdpRemoteError(f.header.status, tlv.str(Tag.DETAIL), tlv.u32(Tag.OS_RESULT));
  }

  /** Sends a REQ and resolves with the matching RES; an ERR frame rejects with NdpRemoteError. */
  request(command: number, payload: Uint8Array = new Uint8Array(0), timeoutMs?: number): Promise<Frame> {
    return this.#exclusive(() => this.#awaitResponse(this.#sendRequest(command, payload), timeoutMs));
  }

  async hello(): Promise<HelloInfo> {
    const cn = new Uint8Array(randomBytes(16));
    const payload = encodeTlv([
      [Tag.PROTOCOL, u16(PROTOCOL_VERSION)],
      [Tag.PROTOCOL_MAX, u16(PROTOCOL_VERSION)],
      [Tag.BRIDGE_NAME, str(this.#bridgeName)],
      [Tag.NONCE, cn],
    ]);
    const res = await this.request(Command.HELLO, payload);
    const t = parseTlv(res.payload);
    const protocol = t.u16(Tag.PROTOCOL);
    const platform = t.str(Tag.PLATFORM);
    const agentVersion = t.str(Tag.AGENT_VERSION);
    const nonce = t.first(Tag.NONCE);
    const auth = t.str(Tag.AUTH);
    const mode = t.str(Tag.MODE);
    const maxFrame = t.u32(Tag.MAX_FRAME);
    if (
      protocol === undefined || platform === undefined || agentVersion === undefined || nonce === undefined ||
      nonce.length !== 16 || auth === undefined || mode === undefined || maxFrame === undefined ||
      !(MODES as readonly string[]).includes(mode)
    )
      throw new NdpProtocolError(9, "malformed HELLO response");
    const deviceId = t.first(Tag.DEVICE_ID);
    const pairedKeys = t.u8(Tag.PAIRED_KEYS);
    const pairingOpen = t.u8(Tag.PAIRING_OPEN);
    if (auth === "required" && (deviceId === undefined || deviceId.length !== 16))
      throw new NdpProtocolError(Status.BAD_REQUEST, "malformed HELLO response (no device_id)");
    this.#cn = cn;
    this.#info = {
      protocol, platform, agentVersion, deviceNonce: nonce, auth, mode: mode as Mode, maxFrame,
      ...(deviceId ? { deviceId } : {}),
      ...(pairedKeys !== undefined ? { pairedKeys } : {}),
      ...(pairingOpen !== undefined ? { pairingOpen: pairingOpen !== 0 } : {}),
    };
    this.#decoder = new FrameDecoder(maxFrame);
    return this.#info;
  }

  /** Model, firmware, memory and SD capacity (agent >= 1.1.0; UNSUPPORTED_COMMAND on older agents). Reading the SD free
   * space can take a few seconds on a large card. */
  async deviceInfo(): Promise<DeviceInfo> {
    const t = parseTlv((await this.request(Command.DEVICE_INFO, new Uint8Array(0), SLOW_FS_TIMEOUT_MS)).payload);
    const num = (tag: number): number | undefined => {
      const v = t.u64(tag);
      return v === undefined ? undefined : Number(v);
    };
    const pair = (totalTag: number, freeTag: number) => {
      const total = num(totalTag), free = num(freeTag);
      return total !== undefined && free !== undefined ? { total, free } : undefined;
    };
    const out: DeviceInfo = {};
    const model = t.str(Tag.MODEL), firmware = t.str(Tag.FIRMWARE), ram = num(Tag.RAM_TOTAL);
    if (model !== undefined) out.model = model;
    if (firmware !== undefined) out.firmware = firmware;
    if (ram !== undefined) out.ramTotal = ram;
    const app = pair(Tag.APP_MEM_TOTAL, Tag.APP_MEM_FREE), sys = pair(Tag.SYS_MEM_TOTAL, Tag.SYS_MEM_FREE), sd = pair(Tag.SD_TOTAL, Tag.SD_FREE);
    if (app) out.appMemory = app;
    if (sys) out.systemMemory = sys;
    if (sd) out.sd = sd;
    return out;
  }

  /** The folders the console's owner allows this computer to read and write (agent >= 0.6.0). */
  async accessInfo(): Promise<AccessInfo> {
    const res = await this.request(Command.ACCESS_INFO);
    const t = parseTlv(res.payload);
    const mode = t.str(Tag.MODE);
    if (mode === undefined || !(MODES as readonly string[]).includes(mode))
      throw new NdpProtocolError(Status.BAD_REQUEST, "malformed ACCESS_INFO response");
    const dec = new TextDecoder();
    return {
      mode: mode as Mode,
      readRoots: t.all(Tag.READ_ROOT).map((b) => dec.decode(b)),
      writeRoots: t.all(Tag.WRITE_ROOT).map((b) => dec.decode(b)),
    };
  }

  /** True once AUTH succeeded and every frame is being sealed. */
  get authenticated(): boolean {
    return this.#session !== null;
  }

  #requireHello(): { cn: Uint8Array; dn: Uint8Array } {
    if (!this.#cn || !this.#info) throw new Error("call hello() first");
    return { cn: this.#cn, dn: this.#info.deviceNonce };
  }

  /**
   * Pairs with the console using the code it displays (its pairing window must be open). Returns the
   * key to persist. The code is typed by the human; it is never sent — only a proof derived from it.
   */
  async pair(code: Uint8Array, label: string): Promise<{ psk: Uint8Array; keyId: Uint8Array }> {
    const { cn, dn } = this.#requireHello();
    const labelBytes = new TextEncoder().encode(label);
    if (labelBytes.length === 0 || labelBytes.length > LABEL_MAX) throw new RangeError(`label must be 1..${LABEL_MAX} bytes`);
    const psk = derivePsk(code);
    const res = await this.request(
      Command.PAIR,
      encodeTlv([[Tag.LABEL, labelBytes], [Tag.PROOF, proofOf(psk, "pair", cn, dn, labelBytes)]]),
      SLOW_FS_TIMEOUT_MS, // the console writes its key file to the SD card before answering
    );
    const keyId = parseTlv(res.payload).first(Tag.KEY_ID);
    const expected = keyIdOf(psk);
    if (!keyId || !bytesEqual(keyId, expected)) throw new NdpProtocolError(Status.BAD_REQUEST, "console returned an unexpected key_id");
    return { psk, keyId };
  }

  /**
   * Pairs by number comparison (agent >= 1.3.0; spec §4.8): the person opens the pairing window on the console (Y), this runs, and
   * `onNumber` receives the 6-digit number to compare with the one on the console's screen (they press A there when equal). Resolves
   * with the key once the console approved; rejects with PairingRefusedError when it said no. The key never crosses the network.
   */
  async pairByNumber(label: string, onNumber: (sas: string) => void, opts: { pollMs?: number; timeoutMs?: number } = {}): Promise<{ psk: Uint8Array; keyId: Uint8Array }> {
    const labelBytes = new TextEncoder().encode(label);
    if (labelBytes.length === 0 || labelBytes.length > LABEL_MAX) throw new RangeError(`label must be 1..${LABEL_MAX} bytes`);
    this.#requireHello();
    const secret = randomSecret(), pubB = x25519Public(secret), nonceB = randomNonce();
    const begin = parseTlv((await this.request(Command.PAIR_BEGIN, encodeTlv([[Tag.LABEL, labelBytes], [Tag.PAIR_COMMIT, pair2Commit(pubB, nonceB)]]))).payload);
    const pubC = begin.first(Tag.PAIR_PUB), nonceC = begin.first(Tag.PAIR_NONCE);
    if (!pubC || pubC.length !== 32 || !nonceC || nonceC.length !== PAIR2_NONCE) throw new NdpProtocolError(Status.BAD_REQUEST, "malformed PAIR_BEGIN response");
    await this.request(Command.PAIR_REVEAL, encodeTlv([[Tag.PAIR_PUB, pubB], [Tag.PAIR_NONCE, nonceB]]));
    const shared = x25519Shared(secret, pubC);
    if (!shared) throw new NdpProtocolError(Status.BAD_REQUEST, "the console sent an invalid key");
    const keys = pair2Derive(shared, pubB, pubC, nonceB, nonceC, labelBytes);
    onNumber(formatSas(keys.sas));
    const deadline = Date.now() + (opts.timeoutMs ?? 90_000);
    for (;;) {
      const t = parseTlv((await this.request(Command.PAIR_POLL, new Uint8Array(0), SLOW_FS_TIMEOUT_MS)).payload);
      const state = t.u8(Tag.PAIR_STATE);
      if (state === 1) {
        const proof = t.first(Tag.PROOF), kid = t.first(Tag.KEY_ID);
        if (!proof || !bytesEqual(proof, pair2OkProof(keys.psk, pubB, pubC)) || !kid || !bytesEqual(kid, keys.keyId))
          throw new NdpProtocolError(Status.UNAUTHORIZED, "the console derived a different key: someone may be in the middle; do not use this pairing");
        return { psk: keys.psk, keyId: keys.keyId };
      }
      if (state === 2) throw new PairingRefusedError("denied", "the console refused (or the request expired)");
      if (state === 3) throw new PairingRefusedError("full", "the console's pairing storage is full: press SELECT twice on it to forget the old ones");
      if (Date.now() > deadline) throw new NdpTransportError("timed out waiting for the person at the console");
      await new Promise((r) => setTimeout(r, opts.pollMs ?? 500));
    }
  }

  /** Proves knowledge of the PSK and switches the connection to sealed frames. */
  async authenticate(psk: Uint8Array): Promise<void> {
    const { cn, dn } = this.#requireHello();
    if (this.#session) throw new Error("already authenticated");
    const payload = encodeTlv([[Tag.KEY_ID, keyIdOf(psk)], [Tag.PROOF, proofOf(psk, "auth", cn, dn)]]);
    this.#pending = { key: sessionKeyOf(psk, cn, dn), sendCtr: 0n, recvCtr: 0n };
    try {
      await this.request(Command.AUTH, payload);
    } catch (e) {
      this.#pending = null;
      throw e;
    }
    if (!this.#session) throw new NdpProtocolError(Status.UNAUTHORIZED, "AUTH answered without a valid MAC");
  }

  /** Every key currently paired with the console (requires AUTH). */
  async pairList(): Promise<Array<{ keyId: Uint8Array; label: string }>> {
    const res = await this.request(Command.PAIR_LIST);
    return parseTlv(res.payload)
      .all(Tag.PAIR_ENTRY)
      .map((v) => ({ keyId: v.subarray(0, 4), label: new TextDecoder().decode(v.subarray(4)) }));
  }

  /** Removes one paired key from the console by its id (requires AUTH). Any authenticated key may forget any other:
   * pairing already grants full read/write over the opened folders, and SELECT x2 on the console already forgets all. */
  async forgetPairing(keyId: Uint8Array): Promise<void> {
    if (keyId.length !== 4) throw new RangeError("keyId must be 4 bytes");
    await this.request(Command.PAIR_FORGET, encodeTlv([[Tag.KEY_ID, keyId]]));
  }

  /** Asks a 3DS to offer installing a title from this URL (http:// or https://). The console only takes it while its
   * "Receive from the browser" screen is open, and its owner confirms there before anything is downloaded. */
  async installUrl(url: string, sha256?: Uint8Array): Promise<void> {
    const fields: TlvField[] = [[Tag.URL, str(url)]];
    if (sha256) {
      if (sha256.length !== 32) throw new RangeError("sha256 must be 32 bytes");
      fields.push([Tag.SHA256, sha256]);
    }
    await this.request(Command.INSTALL_URL, encodeTlv(fields));
  }

  /** Asks a 3DS to offer installing a .cia that is already on its SD card (for example one just uploaded). Same rules as
   * installUrl: the console takes it only while its "Receive from the browser" screen is open, and its owner confirms. */
  async installFile(path: string, sha256?: Uint8Array): Promise<void> {
    const fields: TlvField[] = [[Tag.PATH, str(normalizePath(path))]];
    if (sha256) {
      if (sha256.length !== 32) throw new RangeError("sha256 must be 32 bytes");
      fields.push([Tag.SHA256, sha256]);
    }
    await this.request(Command.INSTALL_URL, encodeTlv(fields));
  }

  async stat(path: string): Promise<FsStat> {
    const res = await this.request(Command.FS_STAT, encodeTlv([[Tag.PATH, str(normalizePath(path))]]));
    const t = parseTlv(res.payload);
    const type = t.u8(Tag.TYPE);
    const size = t.u64(Tag.SIZE);
    const mtime = t.u64(Tag.MTIME);
    if ((type !== FsType.FILE && type !== FsType.DIR) || size === undefined || mtime === undefined)
      throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_STAT response");
    return { type: type === FsType.DIR ? "dir" : "file", size: Number(size), mtime: mtime === 0n ? null : Number(mtime) };
  }

  /** Lists a directory, following the agent's pages. Entries are in the device's order. */
  async list(path: string): Promise<FsEntry[]> {
    const p = normalizePath(path);
    const out: FsEntry[] = [];
    const decoder = new TextDecoder("utf-8", { fatal: false });
    let cursor = 0;
    for (;;) {
      const fields: TlvField[] = [[Tag.PATH, str(p)]];
      if (cursor) fields.push([Tag.CURSOR, u32(cursor)]);
      const t = parseTlv((await this.request(Command.FS_LIST, encodeTlv(fields))).payload);
      for (const e of t.all(Tag.ENTRY)) {
        if (e.length < 10) throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_LIST entry");
        const dv = new DataView(e.buffer, e.byteOffset, e.byteLength);
        out.push({
          type: e[0] === FsType.DIR ? "dir" : "file",
          size: Number(dv.getBigUint64(1, true)),
          name: decoder.decode(e.subarray(9)),
        });
      }
      const more = t.u8(Tag.LIST_MORE);
      const next = t.u32(Tag.NEXT_CURSOR);
      if (more === undefined || next === undefined) throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_LIST response");
      if (!more) return out;
      if (next <= cursor) throw new NdpProtocolError(Status.BAD_REQUEST, "FS_LIST cursor did not advance");
      cursor = next;
    }
  }

  /**
   * Streams a file (or a range) from the device, calling `onChunk` for each DATA frame. Holds the
   * connection for the whole transfer. If anything fails after the RES, the connection is closed
   * (the remaining frames of the stream cannot be skipped safely).
   */
  async read(path: string, opts: ReadOptions, onChunk: (chunk: Uint8Array) => void | Promise<void>): Promise<ReadResult> {
    const verify = opts.verify ?? true;
    const fields: TlvField[] = [[Tag.PATH, str(normalizePath(path))]];
    if (opts.offset) fields.push([Tag.OFFSET, u64(BigInt(opts.offset))]);
    if (opts.length) fields.push([Tag.LENGTH, u64(BigInt(opts.length))]);
    fields.push([Tag.CHUNK, u32(Math.max(MIN_CHUNK, opts.chunk ?? DEFAULT_CHUNK))]);
    if (verify) fields.push([Tag.WANT_HASH, u8(1)]);
    const payload = encodeTlv(fields);

    return this.#exclusive(async () => {
      const t0 = performance.now();
      const id = this.#sendRequest(Command.FS_READ, payload);
      const res = parseTlv((await this.#awaitResponse(id)).payload);
      const totalSize = res.u64(Tag.TOTAL_SIZE);
      const willSend = res.u64(Tag.WILL_SEND);
      if (totalSize === undefined || willSend === undefined) {
        this.close();
        throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_READ response");
      }
      const hash = verify ? createHash("sha256") : null;
      let bytes = 0;
      try {
        for (;;) {
          const f = await this.#nextFrame(this.#requestTimeoutMs);
          if (f.header.requestId !== id) continue;
          if (f.header.kind === Kind.DATA) {
            bytes += f.payload.length;
            hash?.update(f.payload);
            await onChunk(f.payload);
            opts.onProgress?.(bytes, Number(willSend));
          } else if (f.header.kind === Kind.END) {
            if (BigInt(bytes) !== willSend)
              throw new NdpProtocolError(Status.IO_ERROR, `received ${bytes} bytes, expected ${willSend}`);
            const remote = verify ? parseTlv(f.payload).first(Tag.SHA256) : undefined;
            const local = hash ? new Uint8Array(hash.digest()) : null;
            if (verify) {
              if (!remote || !local || Buffer.compare(remote, local) !== 0)
                throw new NdpProtocolError(Status.HASH_MISMATCH, "SHA-256 mismatch between agent and bridge");
            }
            return { totalSize: Number(totalSize), bytes, sha256: local, verified: verify, ms: performance.now() - t0 };
          } else if (f.header.kind === Kind.ERR) {
            throw this.#remoteError(f);
          } else {
            throw new NdpProtocolError(Status.BAD_FRAME, `unexpected frame kind ${f.header.kind} during transfer`);
          }
        }
      } catch (e) {
        // An ERR frame ends the stream in an orderly way; anything else leaves it in an unknown state.
        if (!(e instanceof NdpRemoteError)) this.close();
        throw e;
      }
    });
  }

  /** Reads into memory. `maxBytes` bounds the request itself (the agent never sends more). */
  async readBytes(path: string, opts: ReadOptions & { maxBytes?: number } = {}): Promise<ReadResult & { data: Uint8Array; truncated: boolean }> {
    const maxBytes = opts.maxBytes ?? 4 * 1024 * 1024;
    const chunks: Uint8Array[] = [];
    const length = opts.length ? Math.min(opts.length, maxBytes) : maxBytes;
    const r = await this.read(path, { ...opts, length }, (c) => void chunks.push(c.slice()));
    const data = new Uint8Array(r.bytes);
    let o = 0;
    for (const c of chunks) {
      data.set(c, o);
      o += c.length;
    }
    return { ...r, data, truncated: (opts.offset ?? 0) + r.bytes < r.totalSize };
  }

  /**
   * "Deletes" by MOVING the item (file or whole directory) into `<write root>/.ndp-trash/` on the device.
   * Nothing is destroyed; the returned path is where it can be found (and read) afterwards.
   * The first deletion under a write root creates the trash folder, which takes ~6 s on a real 3DS.
   */
  async delete(path: string): Promise<{ trashPath: string }> {
    const res = await this.request(Command.FS_DELETE, encodeTlv([[Tag.PATH, str(normalizePath(path))]]), SLOW_FS_TIMEOUT_MS);
    const trashPath = parseTlv(res.payload).str(Tag.TRASH_PATH);
    if (!trashPath) throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_DELETE response");
    return { trashPath };
  }

  /**
   * PERMANENTLY deletes an item that is inside a trash folder (`<write root>/.ndp-trash/...`), or empties the trash when
   * `path` is the trash folder itself (agent >= 1.2.2). Anything outside a trash is refused with PROTECTED_PATH. The console
   * works in small batches, so this repeats the request until it reports nothing left; `onProgress` gets the running total
   * of files and folders removed, and returning `false` from it stops after the current batch. Not idempotent-safe to retry
   * blindly after a transport error, but re-running it simply continues where the disk stands.
   */
  async purge(path: string, onProgress?: (removed: number) => boolean | void): Promise<{ removed: number; complete: boolean }> {
    let removed = 0;
    for (;;) {
      const res = await this.request(Command.FS_PURGE, encodeTlv([[Tag.PATH, str(normalizePath(path))]]), SLOW_FS_TIMEOUT_MS);
      const t = parseTlv(res.payload);
      removed += t.u32(Tag.PURGED) ?? 0;
      const more = t.u8(Tag.MORE) === 1;
      if (!more) return { removed, complete: true };
      if (onProgress?.(removed) === false) return { removed, complete: false };
    }
  }

  /**
   * Copies ONE file on the card (agent >= 1.3.0) without it passing through this computer: the console copies in bounded steps
   * into `<to>.ndp-tmp` and renames it into place at the end, so a failure never leaves a half-written destination. Never
   * overwrites (EXISTS). `signal` aborts between steps and discards the partial copy. Folders are the caller's job (mkdir + copy
   * each file).
   */
  async copy(from: string, to: string, opts: { onProgress?: (copied: number, total: number) => void; signal?: AbortSignal } = {}): Promise<{ bytes: number }> {
    const fields = (mode: number): TlvField[] => [[Tag.PATH, str(normalizePath(from))], [Tag.NEW_PATH, str(normalizePath(to))], [Tag.COPY_MODE, u8(mode)]];
    let mode = 1;
    try {
      for (;;) {
        if (opts.signal?.aborted) throw new NdpTransportError("aborted");
        const res = await this.request(Command.FS_COPY, encodeTlv(fields(mode)), SLOW_FS_TIMEOUT_MS);
        mode = 0;
        const t = parseTlv(res.payload);
        const copied = Number(t.u64(Tag.COPIED) ?? 0n), total = Number(t.u64(Tag.TOTAL_SIZE) ?? 0n);
        opts.onProgress?.(copied, total);
        if (t.u8(Tag.MORE) !== 1) return { bytes: copied };
      }
    } catch (e) {
      if (mode === 0 && !this.isClosed) await this.request(Command.FS_COPY, encodeTlv(fields(2)), SLOW_FS_TIMEOUT_MS).catch(() => undefined); // drop the partial copy
      throw e;
    }
  }

  async mkdir(path: string): Promise<void> {
    // Measured on a real 3DS: creating a directory takes ~5.7 s (files take ~85 ms), so wait generously.
    await this.request(Command.FS_MKDIR, encodeTlv([[Tag.PATH, str(normalizePath(path))]]), SLOW_FS_TIMEOUT_MS);
  }

  /**
   * Moves or renames a file or folder (agent >= 1.2.0). Never overwrites: an existing destination fails with EXISTS.
   * Both paths must be writable; items cannot be moved INTO the trash (that is what delete does) but can be moved out of it.
   * Returns the destination path as the console normalized it.
   */
  async rename(from: string, to: string): Promise<string> {
    const res = await this.request(
      Command.FS_RENAME,
      encodeTlv([[Tag.PATH, str(normalizePath(from))], [Tag.NEW_PATH, str(normalizePath(to))]]),
      SLOW_FS_TIMEOUT_MS,
    );
    return parseTlv(res.payload).str(Tag.NEW_PATH) ?? normalizePath(to);
  }

  /**
   * Uploads `size` bytes produced by `chunks` to `path` (temp file + verify + rename on the device).
   * `sha256` (the digest of the whole content) is sent up front so the device can refuse a corrupt
   * transfer before touching anything; the digest it computes is checked again here.
   */
  async write(
    path: string,
    source: { size: number; sha256: Uint8Array; chunks: AsyncIterable<Uint8Array> | Iterable<Uint8Array> },
    opts: WriteOptions = {},
  ): Promise<WriteResult> {
    const fields: TlvField[] = [
      [Tag.PATH, str(normalizePath(path))],
      [Tag.SIZE, u64(BigInt(source.size))],
      [Tag.SHA256, source.sha256],
    ];
    if (opts.overwrite) fields.push([Tag.OVERWRITE, u8(1)]);
    if (opts.backup) fields.push([Tag.BACKUP, u8(1)]);
    const payload = encodeTlv(fields);

    return this.#exclusive(async () => {
      const t0 = performance.now();
      const id = this.#sendRequest(Command.FS_WRITE, payload);
      const ready = parseTlv((await this.#awaitResponse(id, SLOW_FS_TIMEOUT_MS)).payload); // ERR here: nothing was created
      const maxChunk = Math.min(ready.u32(Tag.MAX_CHUNK) ?? DEFAULT_CHUNK, DEFAULT_MAX_FRAME);
      const chunkSize = Math.max(MIN_CHUNK, Math.min(opts.chunk ?? DEFAULT_CHUNK, maxChunk));
      let sent = 0;

      const early = (): NdpRemoteError | undefined => {
        // the agent aborted (I/O error, ...): stop sending instead of streaming into the void
        const i = this.#frames.findIndex((f) => f.header.requestId === id && f.header.kind === Kind.ERR);
        return i >= 0 ? this.#remoteError(this.#frames.splice(i, 1)[0] as Frame) : undefined;
      };
      const send = async (bytes: Uint8Array) => {
        const frame = this.#encode({
          kind: Kind.DATA, requestId: id, command: Command.FS_WRITE, payload: bytes,
          flags: sent + bytes.length < source.size ? Flag.MORE : 0,
        });
        await this.#writeFrame(frame);
        sent += bytes.length;
        opts.onProgress?.(sent, source.size);
      };

      for await (const block of source.chunks) {
        for (let o = 0; o < block.length; o += chunkSize) {
          const err = early();
          if (err) throw err;
          await send(block.subarray(o, Math.min(block.length, o + chunkSize)));
        }
      }
      if (sent !== source.size) {
        this.close(); // we promised `size` bytes and cannot deliver them: the stream is unusable
        throw new NdpProtocolError(Status.BAD_REQUEST, `source produced ${sent} bytes, declared ${source.size}`);
      }
      const err = early();
      if (err) throw err;
      await this.#writeFrame(this.#encode({ kind: Kind.END, requestId: id, command: Command.FS_WRITE }));

      let final: Frame;
      try {
        final = await this.#awaitResponse(id, WRITE_COMMIT_TIMEOUT_MS);
      } catch (e) {
        if (e instanceof NdpTransportError && e.code === undefined)
          throw new NdpTransportError(
            `the device did not confirm the upload of ${path} within ${WRITE_COMMIT_TIMEOUT_MS} ms — the file may or may not have been written; check it with 'ndev stat'`,
          );
        throw e;
      }
      const res = parseTlv(final.payload);
      const written = res.u64(Tag.WRITTEN);
      const digest = res.first(Tag.SHA256);
      if (written === undefined || !digest || digest.length !== 32)
        throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_WRITE response");
      if (Buffer.compare(digest, source.sha256) !== 0 || Number(written) !== source.size)
        throw new NdpProtocolError(Status.HASH_MISMATCH, "the device reports different content than was sent");
      return { written: Number(written), sha256: new Uint8Array(digest), replaced: res.u8(Tag.REPLACED) === 1, ms: performance.now() - t0 };
    });
  }

  writeBytes(path: string, data: Uint8Array, opts: WriteOptions = {}): Promise<WriteResult> {
    const sha256 = new Uint8Array(createHash("sha256").update(data).digest());
    return this.write(path, { size: data.length, sha256, chunks: [data] }, opts);
  }

  /** Uploads a local file (hashed first so the device can verify it before committing). */
  async writeFile(path: string, localPath: string, opts: WriteOptions = {}): Promise<WriteResult> {
    const size = (await statLocal(localPath)).size;
    const hash = createHash("sha256");
    for await (const chunk of createReadStream(localPath)) hash.update(chunk as Buffer);
    const sha256 = new Uint8Array(hash.digest());
    return this.write(path, { size, sha256, chunks: createReadStream(localPath, { highWaterMark: 64 * 1024 }) as AsyncIterable<Uint8Array> }, opts);
  }

  /** Round-trip time in milliseconds; verifies the echoed nonce. */
  async ping(): Promise<number> {
    const nonce = BigInt(`0x${randomBytes(8).toString("hex")}`);
    const t0 = performance.now();
    const res = await this.request(Command.PING, encodeTlv([[Tag.PING_NONCE, u64(nonce)]]));
    const rtt = performance.now() - t0;
    if (parseTlv(res.payload).u64(Tag.PING_NONCE) !== nonce) throw new NdpProtocolError(9, "PING nonce mismatch");
    return rtt;
  }
}
