/* NDP client over a WebSocket: the browser (or Node's global WebSocket) speaks NDP v1 to the console. Port of
 * bridge/packages/core/src/client.ts with the same rules: one request at a time, sealed frames after AUTH, writes are
 * never retried, an aborted transfer closes the connection. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const C = NDP.codec, A = NDP.auth;
  const { Kind, Flag, Command, Status, Tag, NdpError, NdpTransportError } = C;

  const SLOW_MS = 30000, COMMIT_MS = 60000, DEFAULT_MS = 8000;
  const DEFAULT_CHUNK = 32768, MIN_CHUNK = 512;
  const WS_BACKLOG = 512 * 1024;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  class NdpWsClient {
    constructor(url) {
      this.url = url;
      this.ws = null;
      this.decoder = new C.FrameDecoder(C.DEFAULT_MAX_FRAME);
      this.frames = [];
      this.waiter = null;
      this.failure = null;
      this.nextId = 0;
      this.chain = Promise.resolve();
      this.info = null;
      this.cn = null;
      this.session = null;
      this.pending = null;
      this.onclose = null;
    }

    open(timeoutMs = 6000) {
      return new Promise((resolve, reject) => {
        let ws;
        try { ws = new WebSocket(this.url); } catch (e) { reject(new NdpTransportError(String(e.message || e))); return; }
        ws.binaryType = "arraybuffer";
        this.ws = ws;
        const timer = setTimeout(() => { try { ws.close(); } catch (_) { /* ignore */ } reject(new NdpTransportError("could not connect (timeout)")); }, timeoutMs);
        ws.onopen = () => { clearTimeout(timer); resolve(this); };
        ws.onerror = () => { clearTimeout(timer); this.#fail(new NdpTransportError("connection error")); reject(new NdpTransportError("could not connect to the console")); };
        ws.onclose = (ev) => {
          clearTimeout(timer);
          this.replaced = !!ev && ev.code === 4001; // the console gave the connection to another page
          this.#fail(new NdpTransportError(this.replaced ? "connection taken over by another page" : "connection closed"));
          if (this.onclose) this.onclose();
        };
        ws.onmessage = (ev) => this.#onMessage(new Uint8Array(ev.data));
      });
    }

    get isClosed() { return this.failure !== null || !this.ws || this.ws.readyState > 1; }
    get authenticated() { return this.session !== null; }

    close() {
      try { if (this.ws) this.ws.close(); } catch (_) { /* ignore */ }
      this.#fail(new NdpTransportError("connection closed"));
    }

    #onMessage(chunk) {
      let frames;
      try { frames = this.decoder.push(chunk); } catch (e) { this.#fail(e); this.close(); return; }
      for (const f of frames) {
        if (!this.#verify(f)) { this.#fail(new NdpTransportError("frame authentication failed (missing or invalid MAC)")); this.close(); return; }
        this.frames.push(f);
      }
      this.#deliver();
    }

    #verify(f) {
      const s = this.session || this.pending;
      if (!s) return f.mac === null;
      if (!f.mac) return !this.session && f.header.kind === Kind.ERR; // an AUTH refusal comes unsealed
      const expect = A.frameMac(s.key, s.recvCtr, f.raw, f.payload);
      if (!C.equal(expect, f.mac)) return false;
      s.recvCtr++;
      if (!this.session) { this.session = s; this.pending = null; }
      return true;
    }

    #deliver() {
      if (this.waiter && this.frames.length) {
        const w = this.waiter;
        this.waiter = null;
        clearTimeout(w.timer);
        w.resolve(this.frames.shift());
      }
    }

    #fail(e) {
      if (!this.failure) this.failure = e;
      if (this.waiter) {
        const w = this.waiter;
        this.waiter = null;
        clearTimeout(w.timer);
        w.reject(this.failure);
      }
    }

    #next(timeoutMs) {
      if (this.frames.length) return Promise.resolve(this.frames.shift());
      if (this.failure) return Promise.reject(this.failure);
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => { this.waiter = null; reject(new NdpTransportError(`no response within ${timeoutMs} ms`)); }, timeoutMs);
        this.waiter = { resolve, reject, timer };
      });
    }

    #encode(init) {
      const payload = init.payload || new Uint8Array(0);
      const s = this.session;
      const header = C.encodeHeader({
        kind: init.kind, flags: (init.flags || 0) | (s ? Flag.MAC : 0), requestId: init.requestId, command: init.command,
        status: 0, payloadLength: payload.length,
      });
      if (!s) return C.concat(header, payload);
      const mac = A.frameMac(s.key, s.sendCtr, header, payload);
      s.sendCtr++;
      return C.concat(header, payload, mac);
    }

    async #send(bytes) {
      if (this.failure) throw this.failure;
      if (!this.ws || this.ws.readyState !== 1) throw new NdpTransportError("connection closed");
      this.ws.send(bytes);
      while (this.ws.bufferedAmount > WS_BACKLOG) { // let the socket drain instead of queueing a whole file in memory
        if (this.failure) throw this.failure;
        await sleep(15);
      }
    }

    #exclusive(fn) {
      this.queued = (this.queued || 0) + 1;
      const r = this.chain.then(fn, fn);
      const dec = () => { this.queued--; };
      r.then(dec, dec);
      this.chain = r.catch(() => undefined);
      return r;
    }

    /** True while any operation is queued or running. */
    get busy() { return (this.queued || 0) > 0; }

    #remoteError(f) {
      const t = C.parseTlv(f.payload);
      return new NdpError(f.header.status, t.str(Tag.DETAIL));
    }

    async #sendRequest(command, payload) {
      const id = (this.nextId = (this.nextId + 1) >>> 0 || 1);
      await this.#send(this.#encode({ kind: Kind.REQ, requestId: id, command, payload }));
      return id;
    }

    async #awaitResponse(id, timeoutMs = DEFAULT_MS) {
      for (;;) {
        const f = await this.#next(timeoutMs);
        if (f.header.requestId !== id) continue;
        if (f.header.kind === Kind.ERR) throw this.#remoteError(f);
        if (f.header.kind !== Kind.RES) throw new NdpTransportError(`unexpected frame kind ${f.header.kind}`);
        return f;
      }
    }

    request(command, payload = new Uint8Array(0), timeoutMs = DEFAULT_MS) {
      return this.#exclusive(async () => this.#awaitResponse(await this.#sendRequest(command, payload), timeoutMs));
    }

    // ---- session
    async hello() {
      const cn = crypto.getRandomValues(new Uint8Array(16));
      const res = await this.request(Command.HELLO, C.encodeTlv([
        [Tag.PROTOCOL, C.u16(1)], [Tag.PROTOCOL_MAX, C.u16(1)], [Tag.BRIDGE_NAME, C.str("web")], [Tag.NONCE, cn]]));
      const t = C.parseTlv(res.payload);
      const info = {
        protocol: t.u16(Tag.PROTOCOL), platform: t.str(Tag.PLATFORM), agentVersion: t.str(Tag.AGENT_VERSION), auth: t.str(Tag.AUTH),
        mode: t.str(Tag.MODE), maxFrame: t.u32(Tag.MAX_FRAME), deviceNonce: t.first(Tag.NONCE), deviceId: t.first(Tag.DEVICE_ID),
        pairedKeys: t.u8(Tag.PAIRED_KEYS), pairingOpen: t.u8(Tag.PAIRING_OPEN) === 1,
      };
      if (!info.deviceNonce || info.deviceNonce.length !== 16 || !info.platform) throw new NdpTransportError("malformed HELLO response");
      this.cn = cn;
      this.info = info;
      this.decoder = new C.FrameDecoder(info.maxFrame);
      return info;
    }

    async pair(code, label) {
      const lb = C.utf8(label);
      if (lb.length === 0 || lb.length > 15) throw new RangeError("label must be 1..15 bytes");
      const psk = A.derivePsk(code);
      const res = await this.request(Command.PAIR, C.encodeTlv([[Tag.LABEL, lb], [Tag.PROOF, A.proofOf(psk, "pair", this.cn, this.info.deviceNonce, lb)]]), SLOW_MS);
      const keyId = C.parseTlv(res.payload).first(Tag.KEY_ID);
      if (!keyId || !C.equal(keyId, A.keyIdOf(psk))) throw new NdpTransportError("the console returned an unexpected key id");
      return { psk, keyId };
    }

    /** Pairing by number comparison: `onNumber("482 913")` gets the number to compare with the console's screen; resolves with
     * {psk, keyId} once the person pressed A on the console. Rejects with an error whose .refused is "denied" | "full" if not. */
    async pairByNumber(label, onNumber, opts) {
      const o = opts || {};
      const lb = C.utf8(label);
      if (lb.length === 0 || lb.length > 15) throw new RangeError("label must be 1..15 bytes");
      const X = NDP.x25519;
      const secret = crypto.getRandomValues(new Uint8Array(32)), nonceB = crypto.getRandomValues(new Uint8Array(16));
      const pubB = X.publicKey(secret);
      const begin = C.parseTlv((await this.request(Command.PAIR_BEGIN, C.encodeTlv([[Tag.LABEL, lb], [Tag.PAIR_COMMIT, A.pair2Commit(pubB, nonceB)]]), SLOW_MS)).payload);
      const pubC = begin.first(Tag.PAIR_PUB), nonceC = begin.first(Tag.PAIR_NONCE);
      if (!pubC || pubC.length !== 32 || !nonceC || nonceC.length !== 16) throw new NdpTransportError("malformed PAIR_BEGIN response");
      await this.request(Command.PAIR_REVEAL, C.encodeTlv([[Tag.PAIR_PUB, pubB], [Tag.PAIR_NONCE, nonceB]]), SLOW_MS);
      const shared = X.shared(secret, pubC);
      if (!shared) throw new NdpTransportError("the console sent an invalid key");
      const keys = A.pair2Derive(shared, pubB, pubC, nonceB, nonceC, lb);
      onNumber(A.formatSas(keys.sas));
      const deadline = Date.now() + (o.timeoutMs || 90000);
      for (;;) {
        if (o.signal && o.signal.aborted) throw new NdpTransportError("aborted");
        const t = C.parseTlv((await this.request(Command.PAIR_POLL, new Uint8Array(0), SLOW_MS)).payload);
        const state = t.u8(Tag.PAIR_STATE);
        if (state === 1) {
          const proof = t.first(Tag.PROOF), kid = t.first(Tag.KEY_ID);
          if (!proof || !C.equal(proof, A.pair2OkProof(keys.psk, pubB, pubC)) || !kid || !C.equal(kid, keys.keyId))
            throw new NdpTransportError("the console derived a different key: someone may be in the middle; do not use this pairing");
          return { psk: keys.psk, keyId: keys.keyId };
        }
        if (state === 2 || state === 3) { const e = new NdpTransportError(state === 2 ? "refused" : "full"); e.refused = state === 2 ? "denied" : "full"; throw e; }
        if (Date.now() > deadline) throw new NdpTransportError("timed out waiting for the person at the console");
        await sleep(o.pollMs || 500);
      }
    }

    async authenticate(psk) {
      if (this.session) throw new Error("already authenticated");
      const payload = C.encodeTlv([[Tag.KEY_ID, A.keyIdOf(psk)], [Tag.PROOF, A.proofOf(psk, "auth", this.cn, this.info.deviceNonce)]]);
      this.pending = { key: A.sessionKeyOf(psk, this.cn, this.info.deviceNonce), sendCtr: 0, recvCtr: 0 };
      try {
        await this.request(Command.AUTH, payload);
      } catch (e) { this.pending = null; throw e; }
      if (!this.session) throw new NdpTransportError("AUTH answered without a valid MAC");
    }

    /** Every key currently paired with the console (requires AUTH). */
    async pairList() {
      const t = C.parseTlv((await this.request(Command.PAIR_LIST)).payload);
      return t.all(Tag.PAIR_ENTRY).map((v) => ({ keyId: v.subarray(0, 4), label: C.text(v.subarray(4)) }));
    }

    /** Removes one paired key from the console by its id (requires AUTH). Any authenticated device may forget
     * any other: pairing already grants full read/write over the opened folders, and SELECT x2 on the console
     * already forgets all of them at once. */
    async forgetPairing(keyId) {
      await this.request(Command.PAIR_FORGET, C.encodeTlv([[Tag.KEY_ID, keyId]]));
    }

    // ---- information
    async ping() {
      const t0 = performance.now();
      await this.request(Command.PING, C.encodeTlv([[Tag.PING_NONCE, C.u64(Date.now())]]));
      return performance.now() - t0;
    }

    async deviceInfo() {
      const t = C.parseTlv((await this.request(Command.DEVICE_INFO, new Uint8Array(0), SLOW_MS)).payload);
      const pair = (a, b) => (t.u64(a) !== undefined && t.u64(b) !== undefined ? { total: t.u64(a), free: t.u64(b) } : undefined);
      return { model: t.str(Tag.MODEL), firmware: t.str(Tag.FIRMWARE), ramTotal: t.u64(Tag.RAM_TOTAL), appMemory: pair(Tag.APP_MEM_TOTAL, Tag.APP_MEM_FREE),
        systemMemory: pair(Tag.SYS_MEM_TOTAL, Tag.SYS_MEM_FREE), sd: pair(Tag.SD_TOTAL, Tag.SD_FREE) };
    }

    async accessInfo() {
      const t = C.parseTlv((await this.request(Command.ACCESS_INFO)).payload);
      return { mode: t.str(Tag.MODE), readRoots: t.all(Tag.READ_ROOT).map(C.text), writeRoots: t.all(Tag.WRITE_ROOT).map(C.text) };
    }

    // ---- file system
    /** Asks the 3DS to offer installing a title from this URL; its owner confirms on the console. */
    async installUrl(url, sha256, seed) {
      const fields = [[Tag.URL, C.str(url)]];
      if (sha256) fields.push([Tag.SHA256, sha256]);
      if (seed) fields.push([Tag.SEED, seed]);
      await this.request(Command.INSTALL_URL, C.encodeTlv(fields));
    }

    /** Asks the 3DS to offer installing a .cia already on its SD card; its owner confirms on the console. */
    async installPath(path, sha256, seed) {
      const fields = [[Tag.PATH, C.str(path)]];
      if (sha256) fields.push([Tag.SHA256, sha256]);
      if (seed) fields.push([Tag.SEED, seed]);
      await this.request(Command.INSTALL_URL, C.encodeTlv(fields));
    }

    /** Sets the share the 3DS's Sources screen browses. Taken only while that screen is open on the console; applies
     * at once (nothing is downloaded or run by this alone). */
    async sourceLink(url) {
      await this.request(Command.SOURCE_LINK, C.encodeTlv([[Tag.URL, C.str(url)]]));
    }

    async stat(path) {
      const t = C.parseTlv((await this.request(Command.FS_STAT, C.encodeTlv([[Tag.PATH, C.str(path)]]))).payload);
      return { type: t.u8(Tag.TYPE) === 2 ? "dir" : "file", size: t.u64(Tag.SIZE) || 0, mtime: t.u64(Tag.MTIME) || null };
    }

    async list(path) {
      const out = [];
      let cursor = 0;
      for (;;) {
        const fields = [[Tag.PATH, C.str(path)]];
        if (cursor) fields.push([Tag.CURSOR, C.u32(cursor)]);
        const t = C.parseTlv((await this.request(Command.FS_LIST, C.encodeTlv(fields), SLOW_MS)).payload);
        for (const e of t.all(Tag.ENTRY)) {
          const dv = new DataView(e.buffer, e.byteOffset, e.length);
          out.push({ type: e[0] === 2 ? "dir" : "file", size: Number(dv.getBigUint64(1, true)), name: C.text(e.subarray(9)) });
        }
        const more = t.u8(Tag.LIST_MORE), next = t.u32(Tag.NEXT_CURSOR);
        if (!more) return out;
        if (next === undefined || next <= cursor) throw new NdpTransportError("directory listing did not advance");
        cursor = next;
      }
    }

    async mkdir(path) { await this.request(Command.FS_MKDIR, C.encodeTlv([[Tag.PATH, C.str(path)]]), SLOW_MS); }

    async rename(from, to) {
      const t = C.parseTlv((await this.request(Command.FS_RENAME, C.encodeTlv([[Tag.PATH, C.str(from)], [Tag.NEW_PATH, C.str(to)]]), SLOW_MS)).payload);
      return t.str(Tag.NEW_PATH) || to;
    }

    async delete(path) {
      const t = C.parseTlv((await this.request(Command.FS_DELETE, C.encodeTlv([[Tag.PATH, C.str(path)]]), SLOW_MS)).payload);
      return { trashPath: t.str(Tag.TRASH_PATH) };
    }

    /** Copies ONE file on the card (done by the console in bounded steps; never overwrites). `onProgress(copied, total)`;
     * `signal` aborts between steps and discards the partial copy. */
    async copy(from, to, opts) {
      const o = opts || {};
      const req = (mode) => C.encodeTlv([[Tag.PATH, C.str(from)], [Tag.NEW_PATH, C.str(to)], [Tag.COPY_MODE, C.u8(mode)]]);
      let mode = 1;
      try {
        for (;;) {
          if (o.signal && o.signal.aborted) throw new NdpTransportError("aborted");
          const t = C.parseTlv((await this.request(Command.FS_COPY, req(mode), SLOW_MS)).payload);
          mode = 0;
          const copied = Number(t.u64(Tag.COPIED) || 0), total = Number(t.u64(Tag.TOTAL_SIZE) || 0);
          if (o.onProgress) o.onProgress(copied, total);
          if (t.u8(Tag.MORE) !== 1) return { bytes: copied };
        }
      } catch (e) {
        if (mode === 0 && !this.isClosed) await this.request(Command.FS_COPY, req(2), SLOW_MS).catch(() => undefined);
        throw e;
      }
    }

    /** Permanently deletes an item inside a trash folder, or empties the trash (the folder itself). Repeats the console's
     * small batches until done; `onProgress(removed)` may return false to stop early. */
    async purge(path, onProgress) {
      let removed = 0;
      for (;;) {
        const t = C.parseTlv((await this.request(Command.FS_PURGE, C.encodeTlv([[Tag.PATH, C.str(path)]]), SLOW_MS)).payload);
        removed += t.u32(Tag.PURGED) || 0;
        if (t.u8(Tag.MORE) !== 1) return { removed, complete: true };
        if (onProgress && onProgress(removed) === false) return { removed, complete: false };
      }
    }

    /**
     * Streams a file (or a range). `onChunk(Uint8Array)` gets each DATA payload (copy it if you keep it).
     * Verifies the SHA-256 the console computes unless verify:false. If anything fails after the first RES, the
     * connection is closed (the remaining frames cannot be skipped safely): open a new client afterwards.
     */
    read(path, opts, onChunk) {
      const o = opts || {};
      const verify = o.verify !== false;
      const fields = [[Tag.PATH, C.str(path)]];
      if (o.offset) fields.push([Tag.OFFSET, C.u64(o.offset)]);
      if (o.length) fields.push([Tag.LENGTH, C.u64(o.length)]);
      fields.push([Tag.CHUNK, C.u32(Math.max(MIN_CHUNK, o.chunk || DEFAULT_CHUNK))]);
      if (verify) fields.push([Tag.WANT_HASH, C.u8(1)]);
      const payload = C.encodeTlv(fields);
      return this.#exclusive(async () => {
        const t0 = performance.now();
        const id = await this.#sendRequest(Command.FS_READ, payload);
        const res = C.parseTlv((await this.#awaitResponse(id)).payload);
        const totalSize = res.u64(Tag.TOTAL_SIZE), willSend = res.u64(Tag.WILL_SEND);
        if (totalSize === undefined || willSend === undefined) { this.close(); throw new NdpTransportError("malformed FS_READ response"); }
        const hash = verify ? new NDP.Sha256() : null;
        let bytes = 0;
        try {
          for (;;) {
            if (o.signal && o.signal.aborted) throw new NdpTransportError("aborted");
            const f = await this.#next(DEFAULT_MS);
            if (f.header.requestId !== id) continue;
            if (f.header.kind === Kind.DATA) {
              bytes += f.payload.length;
              if (hash) hash.update(f.payload);
              await onChunk(f.payload);
              if (o.onProgress) o.onProgress(bytes, willSend);
            } else if (f.header.kind === Kind.END) {
              if (bytes !== willSend) throw new NdpTransportError(`received ${bytes} bytes, expected ${willSend}`);
              if (verify) {
                const remote = C.parseTlv(f.payload).first(Tag.SHA256);
                if (!remote || !C.equal(remote, hash.digest())) throw new NdpError(Status.HASH_MISMATCH, "SHA-256 mismatch between the console and this page");
              }
              return { totalSize, bytes, verified: verify, ms: performance.now() - t0 };
            } else if (f.header.kind === Kind.ERR) {
              throw this.#remoteError(f);
            } else {
              throw new NdpTransportError(`unexpected frame kind ${f.header.kind} during a transfer`);
            }
          }
        } catch (e) {
          if (!(e instanceof NdpError) || e.status === Status.HASH_MISMATCH) this.close();
          throw e;
        }
      });
    }

    /** Whole file into memory (for the editor and previews). */
    async readBytes(path, opts) {
      const chunks = [];
      const r = await this.read(path, opts, (c) => { chunks.push(c.slice()); });
      return { data: C.concat(...chunks), totalSize: r.totalSize };
    }

    /**
     * Uploads `source` (File, Blob or Uint8Array) to `path`: the console writes a temporary file, verifies size and
     * SHA-256, then renames it over the target. `sha256` is computed here first (streamed in slices, so big files are
     * not held in memory). Never retried; on a failure mid-transfer nothing was changed on the card.
     */
    async write(path, source, opts) {
      const o = opts || {};
      const size = source.length !== undefined ? source.length : source.size;
      const slice = (a, b) => (source instanceof Uint8Array ? Promise.resolve(source.subarray(a, b)) : source.slice(a, b).arrayBuffer().then((x) => new Uint8Array(x)));
      const hasher = new NDP.Sha256();
      for (let off = 0; off < size; off += 1 << 20) {
        if (o.signal && o.signal.aborted) throw new NdpTransportError("aborted");
        hasher.update(await slice(off, Math.min(size, off + (1 << 20))));
        if (o.onHashProgress) o.onHashProgress(Math.min(size, off + (1 << 20)), size);
        await sleep(0);
      }
      const digest = hasher.digest();
      const fields = [[Tag.PATH, C.str(path)], [Tag.SIZE, C.u64(size)], [Tag.SHA256, digest]];
      if (o.overwrite) fields.push([Tag.OVERWRITE, C.u8(1)]);
      if (o.backup) fields.push([Tag.BACKUP, C.u8(1)]);
      const payload = C.encodeTlv(fields);
      return this.#exclusive(async () => {
        const t0 = performance.now();
        const id = await this.#sendRequest(Command.FS_WRITE, payload);
        const ready = C.parseTlv((await this.#awaitResponse(id, SLOW_MS)).payload);
        const maxChunk = Math.min(ready.u32(Tag.MAX_CHUNK) || DEFAULT_CHUNK, C.DEFAULT_MAX_FRAME);
        const chunkSize = Math.max(MIN_CHUNK, Math.min(o.chunk || DEFAULT_CHUNK, maxChunk));
        let sent = 0;
        const early = () => {
          const i = this.frames.findIndex((f) => f.header.requestId === id && f.header.kind === Kind.ERR);
          return i >= 0 ? this.#remoteError(this.frames.splice(i, 1)[0]) : null;
        };
        try {
          for (let off = 0; off < size; ) {
            if (o.signal && o.signal.aborted) throw new NdpTransportError("aborted");
            const err = early();
            if (err) throw err;
            const block = await slice(off, Math.min(size, off + 4 * chunkSize));
            for (let p = 0; p < block.length; p += chunkSize) {
              const piece = block.subarray(p, Math.min(block.length, p + chunkSize));
              const last = sent + piece.length >= size;
              await this.#send(this.#encode({ kind: Kind.DATA, requestId: id, command: Command.FS_WRITE, payload: piece, flags: last ? 0 : Flag.MORE }));
              sent += piece.length;
              if (o.onProgress) o.onProgress(sent, size);
            }
            off += block.length;
          }
          const err = early();
          if (err) throw err;
          await this.#send(this.#encode({ kind: Kind.END, requestId: id, command: Command.FS_WRITE }));
          const final = C.parseTlv((await this.#awaitResponse(id, COMMIT_MS)).payload);
          return { written: final.u64(Tag.WRITTEN) ?? size, replaced: final.u8(Tag.REPLACED) === 1, ms: performance.now() - t0, sha256: digest };
        } catch (e) {
          if (!(e instanceof NdpError)) this.close(); // the stream is in an unknown state
          throw e;
        }
      });
    }
  }

  NDP.NdpWsClient = NdpWsClient;
})();
