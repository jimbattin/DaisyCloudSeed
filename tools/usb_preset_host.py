#!/usr/bin/env python3
"""Linux test host for the pedal's USB-MIDI preset protocol (docs/USB_MIDI.md,
src/preset_protocol.h), used by docs/HARDWARE_TESTS.md. Python 3 stdlib only: it
talks raw MIDI bytes to the ALSA rawmidi node /dev/snd/midiC<card>D0.

  usb_preset_host.py [--dev PATH] info
  usb_preset_host.py [--dev PATH] read OUT
  usb_preset_host.py [--dev PATH] upload FILE
  usb_preset_host.py [--dev PATH] begin FILE      # BEGIN only (session timeout test)
  usb_preset_host.py [--dev PATH] beginabort FILE # BEGIN then ABORT
  usb_preset_host.py [--dev PATH] revert
  usb_preset_host.py [--dev PATH] flood SECONDS   # INFO every 2 ms + READ-all loop, no session
  usb_preset_host.py fnv FILE
"""
import os, select, sys, time

HDR = bytes([0x7D, 0x43, 0x53])
STATUS = ["Ok", "BadFrame", "NoSession", "BadSeq", "BadLength", "LengthMismatch",
          "HashMismatch", "ParseError", "FlashError"]
CHUNK = 240


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def u14(v): return bytes([v & 0x7F, (v >> 7) & 0x7F])
def u21(v): return u14(v) + bytes([(v >> 14) & 0x7F])
def u32(v): return u21(v) + bytes([(v >> 21) & 0x7F, (v >> 28) & 0x0F])
def g14(b): return b[0] | b[1] << 7
def g21(b): return g14(b) | b[2] << 14
def g32(b): return g21(b) | b[3] << 21 | (b[4] & 0x0F) << 28


def find_dev():
    # /proc/asound/cards: " 2 [BuiltIn        ]: USB-Audio - Daisy Seed Built In"
    for line in open("/proc/asound/cards"):
        parts = line.split()
        if parts and parts[0].isdigit() and "Daisy" in line:
            dev = f"/dev/snd/midiC{parts[0]}D0"
            if os.path.exists(dev):
                return dev
    sys.exit("no Daisy MIDI device found; pass --dev /dev/snd/midiC<card>D0")


class Link:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)
        self.buf = bytearray()

    def send(self, cmd, body=b""):
        os.write(self.fd, bytes([0xF0]) + HDR + bytes([cmd]) + body + bytes([0xF7]))

    def recv(self, timeout):
        """Next reply frame of ours: (cmd, status, body) or None on timeout."""
        end = time.monotonic() + timeout
        while True:
            while 0xF0 in self.buf and 0xF7 in self.buf[self.buf.index(0xF0):]:
                s = self.buf.index(0xF0)
                e = self.buf.index(0xF7, s)
                frame = bytes(self.buf[s + 1:e])
                del self.buf[:e + 1]
                if len(frame) >= 5 and frame[:3] == HDR:
                    return frame[3], frame[4], frame[5:]
            left = end - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                self.buf += os.read(self.fd, 4096)

    def call(self, cmd, body=b"", timeout=1.0):
        self.send(cmd, body)
        r = self.recv(timeout)
        if r is None:
            sys.exit(f"timeout waiting for reply to cmd 0x{cmd:02x}")
        rcmd, st, rbody = r
        if rcmd != (cmd | 0x40):
            sys.exit(f"unexpected reply cmd 0x{rcmd:02x} to 0x{cmd:02x}")
        return st, rbody


def show(name, st, extra=""):
    print(f"{name}: {STATUS[st] if st < len(STATUS) else st} {extra}".rstrip())


def info(link, quiet=False):
    st, b = link.call(0x01)
    d = dict(version=b[0], maxTextBytes=g21(b[1:]), chunkBytes=g14(b[4:]), source=b[6],
             activeLength=g21(b[7:]), activeHash=g32(b[10:]), presetCount=b[15])
    if not quiet:
        show("INFO", st, " ".join(f"{k}={v:#010x}" if k == "activeHash" else f"{k}={v}"
                                  for k, v in d.items()))
    return d


def read_all(link, quiet=False):
    out, index, length = bytearray(), 0, None
    while length is None or len(out) < length:
        st, b = link.call(0x06, u14(index))
        if st != 0:
            sys.exit(f"READ {index}: {STATUS[st]}")
        if g14(b) != index:
            sys.exit(f"READ {index}: echoed index {g14(b)}")
        length = g21(b[2:])
        out += b[5:]
        index += 1
    if not quiet:
        print(f"READ: {len(out)} bytes in {index} chunks, fnv={fnv(out):#010x}")
    return bytes(out)


def begin(link, text):
    st, _ = link.call(0x02, u21(len(text)) + u32(fnv(text)))
    show("BEGIN", st, f"length={len(text)} hash={fnv(text):#010x}")
    return st


def upload(link, text):
    if begin(link, text) != 0:
        return
    seq = 0
    for off in range(0, len(text), CHUNK):
        st, b = link.call(0x03, u14(seq) + text[off:off + CHUNK])
        if st != 0:
            show(f"DATA {seq}", st)
            return
        seq += 1
    print(f"DATA: {seq} chunks Ok")
    t0 = time.monotonic()
    st, b = link.call(0x04, timeout=10.0)
    show("COMMIT", st, f"({time.monotonic() - t0:.2f} s) {b.decode('ascii', 'replace')}")


def main():
    args = sys.argv[1:]
    dev = None
    if args[:1] == ["--dev"]:
        dev, args = args[1], args[2:]
    if not args:
        sys.exit(__doc__)
    cmd = args[0]
    if cmd == "fnv":
        print(f"{fnv(open(args[1], 'rb').read()):#010x}")
        return
    link = Link(dev or find_dev())
    if cmd == "info":
        info(link)
    elif cmd == "read":
        open(args[1], "wb").write(read_all(link))
    elif cmd == "upload":
        upload(link, open(args[1], "rb").read())
    elif cmd == "begin":
        begin(link, open(args[1], "rb").read())
    elif cmd == "beginabort":
        begin(link, open(args[1], "rb").read())
        show("ABORT", link.call(0x07)[0])
    elif cmd == "revert":
        show("REVERT", link.call(0x05, timeout=5.0)[0])
    elif cmd == "flood":
        end = time.monotonic() + float(args[1])
        n = reads = 0
        while time.monotonic() < end:
            info(link, quiet=True)
            n += 1
            if n % 50 == 0:
                read_all(link, quiet=True)
                reads += 1
            time.sleep(0.002)
        print(f"flood: {n} INFO, {reads} READ-all, no errors")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
