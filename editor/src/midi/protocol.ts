// USB-MIDI SysEx preset protocol v1. Normative definition: src/preset_protocol.h;
// host guide: docs/USB_MIDI.md. Pure: no browser APIs.

export const CMD = { INFO: 1, BEGIN: 2, DATA: 3, COMMIT: 4, REVERT: 5, READ: 6, ABORT: 7 } as const;
export type Command = (typeof CMD)[keyof typeof CMD];

export const CMD_NAMES: Record<number, string> = Object.fromEntries(
  Object.entries(CMD).map(([name, value]) => [value, name]),
);

export const STATUS_NAMES = [
  'Ok',
  'BadFrame',
  'NoSession',
  'BadSeq',
  'BadLength',
  'LengthMismatch',
  'HashMismatch',
  'ParseError',
  'FlashError',
] as const;

export const STATUS_PARSE_ERROR = 7;

export const HEADER = [0x7d, 0x43, 0x53] as const;

// 7-bit little-endian integers (docs/USB_MIDI.md section 5).
export const u14 = (v: number): number[] => [v & 0x7f, (v >>> 7) & 0x7f];
export const u21 = (v: number): number[] => [...u14(v), (v >>> 14) & 0x7f];
export const u32 = (v: number): number[] => [...u21(v), (v >>> 21) & 0x7f, (v >>> 28) & 0x0f];
export const getU14 = (b: ArrayLike<number>, i: number): number => b[i] | (b[i + 1] << 7);
export const getU21 = (b: ArrayLike<number>, i: number): number => getU14(b, i) | (b[i + 2] << 14);
export const getU32 = (b: ArrayLike<number>, i: number): number =>
  (getU21(b, i) | (b[i + 3] << 21) | ((b[i + 4] & 0x0f) << 28)) >>> 0;

/** FNV-1a 32-bit (docs/USB_MIDI.md section 6). */
export function fnv1a32(bytes: Uint8Array): number {
  let h = 0x811c9dc5;
  for (const b of bytes) h = Math.imul(h ^ b, 16777619) >>> 0;
  return h >>> 0;
}

export function buildFrame(cmd: number, body: ArrayLike<number> = []): Uint8Array {
  const out = new Uint8Array(body.length + 6);
  out[0] = 0xf0;
  out.set(HEADER, 1);
  out[4] = cmd;
  out.set(Array.from(body), 5);
  out[out.length - 1] = 0xf7;
  return out;
}

export interface Reply {
  cmd: number;
  status: number;
  body: Uint8Array;
}

/** A reply frame of ours (`F0 7D 43 53 cmd status body F7`), or null for anything else. */
export function parseReply(msg: Uint8Array): Reply | null {
  if (msg.length < 7 || msg[0] !== 0xf0 || msg[msg.length - 1] !== 0xf7) return null;
  if (msg[1] !== HEADER[0] || msg[2] !== HEADER[1] || msg[3] !== HEADER[2]) return null;
  return { cmd: msg[4], status: msg[5], body: msg.slice(6, -1) };
}

export interface InfoReply {
  version: number;
  maxTextBytes: number;
  chunkBytes: number;
  source: number;
  activeLength: number;
  activeHash: number;
  presetCount: number;
}

// Offsets as in tools/usb_preset_host.py info().
export function decodeInfo(b: Uint8Array): InfoReply {
  return {
    version: b[0],
    maxTextBytes: getU21(b, 1),
    chunkBytes: getU14(b, 4),
    source: b[6],
    activeLength: getU21(b, 7),
    activeHash: getU32(b, 10),
    presetCount: b[15],
  };
}

export class ProtocolError extends Error {
  status?: number;
  constructor(message: string, status?: number) {
    super(message);
    this.name = 'ProtocolError';
    this.status = status;
  }
}

/**
 * Reassembles SysEx messages from a MIDI byte stream: real-time bytes (F8-FF) are dropped
 * anywhere, F0 (re)starts a frame, F7 ends it, and any other status byte aborts it.
 */
export function createSysexAssembler(onMessage: (msg: Uint8Array) => void): (data: Uint8Array) => void {
  let buf: number[] | null = null;
  return (data) => {
    for (const b of data) {
      if (b >= 0xf8) continue;
      if (b === 0xf0) {
        buf = [b];
      } else if (b === 0xf7) {
        if (buf) {
          buf.push(b);
          onMessage(Uint8Array.from(buf));
          buf = null;
        }
      } else if (b >= 0x80) {
        buf = null;
      } else if (buf) {
        buf.push(b);
      }
    }
  };
}
