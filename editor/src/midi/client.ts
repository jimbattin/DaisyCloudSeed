import {
  CMD,
  CMD_NAMES,
  STATUS_NAMES,
  STATUS_PARSE_ERROR,
  ProtocolError,
  buildFrame,
  decodeInfo,
  fnv1a32,
  getU14,
  getU21,
  parseReply,
  u14,
  u21,
  u32,
  type InfoReply,
  type Reply,
} from './protocol';

export interface MidiTransport {
  send(msg: Uint8Array): void;
  subscribe(cb: (msg: Uint8Array) => void): () => void;
  /** Resolves once the pedal's ports are back after a reboot; rejects with RebootTimeoutError. */
  waitForReboot(): Promise<void>;
}

export class RebootTimeoutError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'RebootTimeoutError';
  }
}

export interface LinkOptions {
  replyTimeoutMs?: number;
  commitTimeoutMs?: number;
  revertTimeoutMs?: number;
}

export type Progress = (done: number, total: number) => void;

/** Stop-and-wait client for protocol v1: one request in flight at a time. */
export class PresetLink {
  private readonly replyTimeoutMs: number;
  private readonly commitTimeoutMs: number;
  private readonly revertTimeoutMs: number;

  constructor(
    private readonly transport: MidiTransport,
    opts: LinkOptions = {},
  ) {
    this.replyTimeoutMs = opts.replyTimeoutMs ?? 1000;
    this.commitTimeoutMs = opts.commitTimeoutMs ?? 10000;
    this.revertTimeoutMs = opts.revertTimeoutMs ?? 5000;
  }

  /**
   * Sends one request and resolves with its reply. DATA and READ replies must echo the
   * request's seq/index, so a late reply to an earlier resend is never taken for this one.
   */
  private async call(cmd: number, body: number[] = [], timeoutMs = this.replyTimeoutMs): Promise<Reply> {
    const name = CMD_NAMES[cmd];
    const echo = cmd === CMD.DATA || cmd === CMD.READ ? getU14(body, 0) : null;
    // INFO, READ and DATA are answered identically when repeated (docs/USB_MIDI.md section 10).
    const attempts = cmd === CMD.INFO || cmd === CMD.READ || cmd === CMD.DATA ? 2 : 1;
    const frame = buildFrame(cmd, body);

    for (let attempt = 0; attempt < attempts; attempt++) {
      const { promise, resolve } = Promise.withResolvers<Reply | null>();
      const unsubscribe = this.transport.subscribe((msg) => {
        const r = parseReply(msg);
        if (!r || r.cmd !== (cmd | 0x40)) return;
        if (echo !== null && r.status === 0 && getU14(r.body, 0) !== echo) return;
        clearTimeout(timer);
        unsubscribe();
        resolve(r);
      });
      const timer = setTimeout(() => {
        unsubscribe();
        resolve(null);
      }, timeoutMs);
      try {
        this.transport.send(frame);
      } catch (e) {
        clearTimeout(timer);
        unsubscribe();
        throw e;
      }
      const reply = await promise;
      if (!reply) continue;
      if (reply.status !== 0) {
        const status = STATUS_NAMES[reply.status] ?? `status ${reply.status}`;
        const detail = reply.status === STATUS_PARSE_ERROR ? `: ${String.fromCharCode(...reply.body)}` : '';
        throw new ProtocolError(`${name}: ${status}${detail}`, reply.status);
      }
      return reply;
    }
    throw new ProtocolError(`${name}: no reply`);
  }

  async info(): Promise<InfoReply> {
    return decodeInfo((await this.call(CMD.INFO)).body);
  }

  /** The active bank's text, checked against INFO's activeHash. */
  async readAll(onProgress?: Progress): Promise<string> {
    const chunks: Uint8Array[] = [];
    let received = 0;
    let total = -1;
    for (let index = 0; total < 0 || received < total; index++) {
      const { body } = await this.call(CMD.READ, u14(index));
      total = getU21(body, 2);
      const data = body.slice(5);
      if (data.length === 0 && received < total) throw new ProtocolError('READ: empty chunk');
      chunks.push(data);
      received += data.length;
      onProgress?.(Math.min(received, total), total);
    }
    const bytes = new Uint8Array(received);
    let off = 0;
    for (const c of chunks) {
      bytes.set(c, off);
      off += c.length;
    }
    const info = await this.info();
    if (fnv1a32(bytes) !== info.activeHash) throw new ProtocolError('READ: hash mismatch');
    return new TextDecoder('ascii').decode(bytes);
  }

  /**
   * Uploads a whole bank. COMMIT Ok means the pedal stored it; the pedal then reboots.
   * Resolves with the post-reboot INFO, or null when the pedal's ports did not come back
   * in time (stored, but unverified).
   */
  async upload(text: string, onProgress?: Progress): Promise<InfoReply | null> {
    const bytes = new TextEncoder().encode(text);
    const hash = fnv1a32(bytes);
    const info = await this.info();
    if (info.version !== 1) throw new ProtocolError(`INFO: unsupported protocol version ${info.version}`);
    if (bytes.length > info.maxTextBytes) {
      throw new ProtocolError(`text is ${bytes.length} bytes; the pedal accepts at most ${info.maxTextBytes}`);
    }

    await this.call(CMD.BEGIN, [...u21(bytes.length), ...u32(hash)]);
    const chunk = info.chunkBytes;
    const count = Math.ceil(bytes.length / chunk);
    try {
      for (let seq = 0; seq < count; seq++) {
        const part = bytes.subarray(seq * chunk, (seq + 1) * chunk);
        await this.call(CMD.DATA, [...u14(seq), ...part]);
        onProgress?.(Math.min((seq + 1) * chunk, bytes.length), bytes.length);
      }
    } catch (e) {
      await this.abort().catch(() => undefined);
      throw e;
    }
    await this.call(CMD.COMMIT, [], this.commitTimeoutMs);

    const after = await this.afterReboot();
    if (!after) return null;
    if (after.source !== 1 || after.activeHash !== hash) {
      throw new ProtocolError('pedal did not activate the upload');
    }
    return after;
  }

  /** Reverts to the built-in bank; same null contract as upload(). */
  async revert(): Promise<InfoReply | null> {
    await this.call(CMD.REVERT, [], this.revertTimeoutMs);
    const after = await this.afterReboot();
    if (!after) return null;
    if (after.source !== 0) throw new ProtocolError('pedal did not revert');
    return after;
  }

  async abort(): Promise<void> {
    await this.call(CMD.ABORT);
  }

  /**
   * INFO from the rebooted pedal, or null when it cannot be verified: its ports did not come
   * back, or they came back but INFO went unanswered (a stale port a browser still lists).
   */
  private async afterReboot(): Promise<InfoReply | null> {
    try {
      await this.transport.waitForReboot();
      return await this.info();
    } catch (e) {
      if (e instanceof RebootTimeoutError || (e instanceof ProtocolError && e.status === undefined)) return null;
      throw e;
    }
  }
}
