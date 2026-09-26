import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';
import { PresetLink, RebootTimeoutError, type MidiTransport } from './client';
import { CMD, HEADER, fnv1a32, getU14, getU21, getU32, u14, u21, u32 } from './protocol';

const fixture = readFileSync(new URL('../../../tests/fixtures/two_presets.toml', import.meta.url), 'utf8');
const builtIn = readFileSync(new URL('../../../presets.toml', import.meta.url), 'utf8');
const enc = (s: string) => new TextEncoder().encode(s);

const CHUNK = 240;
const MAX_TEXT = 98304;

/** The pedal's protocol v1 state machine (src/preset_protocol.cpp) over a stored text. */
class FakePedal implements MidiTransport {
  text: Uint8Array;
  source = 0;
  dropReplies = 0;
  commitParseError: string | null = null;
  rebootTimesOut = false;
  reboots = 0;
  dataFrames: { seq: number; bytes: number }[] = [];
  private subscribers = new Set<(msg: Uint8Array) => void>();
  private session: { length: number; hash: number; received: number[]; nextSeq: number } | null = null;

  constructor(text: string) {
    this.text = enc(text);
  }

  subscribe(cb: (msg: Uint8Array) => void) {
    this.subscribers.add(cb);
    return () => void this.subscribers.delete(cb);
  }

  /** After a reboot, the transport reopens a port nobody answers on (a stale listing). */
  staleAfterReboot = false;

  async waitForReboot() {
    this.reboots++;
    if (this.rebootTimesOut) throw new RebootTimeoutError('pedal did not come back after reboot');
    if (this.staleAfterReboot) this.dropReplies = Infinity;
  }

  send(msg: Uint8Array) {
    expect(msg[0]).toBe(0xf0);
    expect(msg[msg.length - 1]).toBe(0xf7);
    expect([...msg.slice(1, 4)]).toEqual([...HEADER]);
    const cmd = msg[4];
    const body = msg.slice(5, -1);
    const [status, reply] = this.handle(cmd, body);
    if (this.dropReplies > 0) {
      this.dropReplies--;
      return;
    }
    const frame = Uint8Array.from([0xf0, ...HEADER, cmd | 0x40, status, ...reply, 0xf7]);
    queueMicrotask(() => this.subscribers.forEach((cb) => cb(frame)));
  }

  private handle(cmd: number, body: Uint8Array): [number, number[]] {
    switch (cmd) {
      case CMD.INFO:
        return [0, [1, ...u21(MAX_TEXT), ...u14(CHUNK), this.source, ...u21(this.text.length), ...u32(fnv1a32(this.text)), 2]];
      case CMD.READ: {
        const index = getU14(body, 0);
        if (index * CHUNK >= this.text.length) return [3, []];
        return [0, [...u14(index), ...u21(this.text.length), ...this.text.slice(index * CHUNK, (index + 1) * CHUNK)]];
      }
      case CMD.BEGIN:
        this.session = { length: getU21(body, 0), hash: getU32(body, 3), received: [], nextSeq: 0 };
        return [0, []];
      case CMD.DATA: {
        const seq = getU14(body, 0);
        const s = this.session;
        if (!s) return [2, u14(seq)];
        if (s.nextSeq > 0 && seq === s.nextSeq - 1) return [0, u14(seq)];
        if (seq !== s.nextSeq) return [3, u14(seq)];
        s.received.push(...body.slice(2));
        s.nextSeq++;
        this.dataFrames.push({ seq, bytes: body.length - 2 });
        return [0, u14(seq)];
      }
      case CMD.COMMIT: {
        const s = this.session;
        this.session = null;
        if (!s) return [2, []];
        const bytes = Uint8Array.from(s.received);
        if (bytes.length !== s.length) return [5, []];
        if (fnv1a32(bytes) !== s.hash) return [6, []];
        if (this.commitParseError) return [7, [...enc(this.commitParseError)]];
        this.text = bytes;
        this.source = 1;
        return [0, []];
      }
      case CMD.REVERT:
        this.session = null;
        this.text = enc(builtIn);
        this.source = 0;
        return [0, []];
      case CMD.ABORT:
        this.session = null;
        return [0, []];
      default:
        return [1, []];
    }
  }
}

const decode = (b: Uint8Array) => new TextDecoder().decode(b);

describe('PresetLink', () => {
  it('reads the active bank byte-exactly', async () => {
    const pedal = new FakePedal(fixture);
    const progress: number[] = [];
    const text = await new PresetLink(pedal).readAll((done) => progress.push(done));
    expect(text).toBe(fixture);
    expect(progress.at(-1)).toBe(enc(fixture).length);
  });

  it('uploads in 240-byte chunks with consecutive seqs and verifies after the reboot', async () => {
    const pedal = new FakePedal(builtIn);
    const info = await new PresetLink(pedal).upload(fixture);
    expect(decode(pedal.text)).toBe(fixture);
    expect(info).toMatchObject({ source: 1, activeHash: fnv1a32(enc(fixture)) });
    expect(pedal.dataFrames.map((f) => f.seq)).toEqual([...pedal.dataFrames.keys()]);
    expect(pedal.dataFrames.every((f) => f.bytes <= CHUNK)).toBe(true);
    expect(pedal.reboots).toBe(1);
  });

  it('reports a rejected bank with the pedal message and never waits for a reboot', async () => {
    const pedal = new FakePedal(builtIn);
    pedal.commitParseError = 'preset 0: blinks out of range 1..20';
    await expect(new PresetLink(pedal).upload(fixture)).rejects.toThrow(
      'COMMIT: ParseError: preset 0: blinks out of range 1..20',
    );
    expect(pedal.reboots).toBe(0);
    expect(decode(pedal.text)).toBe(builtIn);
  });

  it('resends a DATA frame whose reply was lost without duplicating text', async () => {
    const pedal = new FakePedal(builtIn);
    const link = new PresetLink(pedal, { replyTimeoutMs: 20 });
    const orig = pedal.send.bind(pedal);
    let dataSeen = 0;
    pedal.send = (msg) => {
      if (msg[4] === CMD.DATA && ++dataSeen === 2) pedal.dropReplies = 1;
      orig(msg);
    };
    await link.upload(fixture);
    expect(decode(pedal.text)).toBe(fixture);
  });

  it('gives up after two lost INFO replies', async () => {
    const pedal = new FakePedal(builtIn);
    pedal.dropReplies = 2;
    await expect(new PresetLink(pedal, { replyTimeoutMs: 20 }).info()).rejects.toThrow('INFO: no reply');
  });

  it('reverts to the built-in bank', async () => {
    const pedal = new FakePedal(fixture);
    pedal.source = 1;
    const info = await new PresetLink(pedal).revert();
    expect(info?.source).toBe(0);
  });

  it('resolves null when the pedal does not come back after the reboot', async () => {
    const pedal = new FakePedal(builtIn);
    pedal.rebootTimesOut = true;
    const link = new PresetLink(pedal);
    expect(await link.upload(fixture)).toBeNull();
    expect(decode(pedal.text)).toBe(fixture);
    expect(await link.revert()).toBeNull();
  });

  it('resolves null when the rebooted pedal does not answer INFO', async () => {
    const pedal = new FakePedal(builtIn);
    pedal.staleAfterReboot = true;
    expect(await new PresetLink(pedal, { replyTimeoutMs: 20 }).upload(fixture)).toBeNull();
    expect(decode(pedal.text)).toBe(fixture);
  });
});
