# USB-MIDI preset upload protocol

This document is for anyone writing a host program (typically a web page) that uploads a
preset bank to the pedal, reads the active bank back, or reverts to the built-in bank. No
firmware knowledge is needed. The normative definition is
[src/preset_protocol.h](../src/preset_protocol.h); if the two ever disagree, the code wins.

## 1. Overview

The Daisy Seed's micro-USB port is a class-compliant USB-MIDI device at every boot. The
operating system needs no driver; the port is named after the USB product string
"Daisy Seed Built In" (`libdaisy/src/usbd/usbd_desc.c:77`).

Over that port, MIDI System Exclusive (SysEx) messages can:
- **upload** a complete `presets.toml` text, which replaces the preset bank;
- **read back** the text of the bank that is active right now;
- **revert** to the bank built into the firmware.

The pedal keeps working as a pedal throughout, except during an upload (see section 3).

## 2. Requirements

- A browser with Web MIDI: Chrome, Edge or Firefox. Safari has no Web MIDI.
- SysEx access: `navigator.requestMIDIAccess({ sysex: true })`. Browsers only allow this in
  a secure context (`https://` or `http://localhost`) and ask the user for permission.
- A data-capable USB cable from the computer to the Seed's micro-USB port (some cables are
  charge-only).

## 3. What happens on the pedal

- **Audio during an upload.** From BEGIN until the session ends (COMMIT, ABORT, an error,
  or 5 s without upload traffic), the reverb is switched off and the pedal passes the dry
  signal through. INFO and READ do not affect audio.
- **Reboot.** A successful upload or revert reboots the pedal. The USB port disappears and
  comes back after about 3 s (the Daisy bootloader's 2.5 s grace window, then the firmware
  re-enumerates). Reconnect to the port afterwards.
- **Saved sounds are wiped.** Sounds saved on the pedal (footswitch 1 held 5 s) belong to one
  bank. Whenever the active bank's text changes (an upload, a revert, or a reflash), they are
  discarded at the next boot.
- **Reflashing discards the upload.** An uploaded bank is tied to the firmware image that
  stored it. After a different firmware is flashed, the pedal boots its built-in bank. The
  text stays in flash until a REVERT or the next upload, so flashing the original image again
  brings it back.
- **Validation.** The pedal checks the text with the same parser `make presets-check` uses,
  before anything is written to flash. A rejected text changes nothing, and the reply carries
  the parser's message. The rules are those listed in the header of `presets.toml`; in short:
  ASCII only, 1 to 16 presets, every parameter present and within 0.0 to 1.0.

## 4. Message format

Every message from the host is one SysEx message:

```
F0 7D 43 53 <cmd> <body...> F7
```

- `F0` / `F7`: SysEx start / end.
- `7D`: the MIDI manufacturer ID reserved for non-commercial and educational use. A
  commercial product would need an ID assigned by the MIDI Association.
- `43 53`: the ASCII letters "CS" (CloudSeed), which separate this protocol from any other
  `7D` traffic.
- `<cmd>`: the command number (section 7).
- `<body>`: command-specific bytes. Every byte between `F0` and `F7` is below `0x80`, as MIDI
  requires.

Every reply from the pedal has this form:

```
F0 7D 43 53 <cmd | 0x40> <status> <body...> F7
```

The reply command is the request's command with bit 6 set (INFO `01` is answered by `41`).
`<status>` is one of the codes in section 8. The pedal ignores, without replying, any SysEx
whose first three bytes after `F0` are not `7D 43 53`.

## 5. Numbers

Multi-byte numbers are split into 7-bit groups, least significant group first:

| Type | Bytes | Range |
|---|---|---|
| u14 | 2 | 0 .. 16,383 |
| u21 | 3 | 0 .. 2,097,151 |
| u32 | 5 | 0 .. 2^32 - 1 (the fifth byte carries bits 28-31) |

Worked example: length 49,000 = `0x00BF68`.
- bits 0-6: `0x68`
- bits 7-13: `0x7E`
- bits 14-20: `0x02`

So the u21 encoding is `68 7E 02`.

```js
const u14 = v => [v & 0x7f, (v >>> 7) & 0x7f];
const u21 = v => [...u14(v), (v >>> 14) & 0x7f];
const u32 = v => [...u21(v), (v >>> 21) & 0x7f, (v >>> 28) & 0x0f];
const getU14 = (b, i) => b[i] | (b[i + 1] << 7);
const getU21 = (b, i) => getU14(b, i) | (b[i + 2] << 14);
const getU32 = (b, i) => (getU21(b, i) | (b[i + 3] << 21) | ((b[i + 4] & 0x0f) << 28)) >>> 0;
```

## 6. Hash

Uploads and INFO use the 32-bit FNV-1a hash of the text bytes:

```js
function fnv1a32(bytes) {           // bytes: Uint8Array of the file
  let h = 0x811c9dc5;
  for (const b of bytes) h = Math.imul(h ^ b, 0x01000193);
  return h >>> 0;
}
```

Test values: the empty input gives `0x811C9DC5`, `"a"` gives `0xE40C292C`, and
`"CloudSeed"` gives `0x63D3C197`.

## 7. Commands

All hex examples below are complete messages. They use an example bank of 48,988 bytes with
hash `0x70B4FE00` and 10 presets; the real values are whatever the active text gives
(INFO reports them).

### INFO (`01`)

Asks what the pedal supports and which bank is active. Works at any time.

Request: no body.

Reply body after the status:

| Field | Type | Meaning |
|---|---|---|
| version | 1 byte | protocol version, currently 1 |
| maxTextBytes | u21 | largest accepted text: 98,304 |
| chunkBytes | u14 | bytes per DATA / READ chunk: 240 |
| source | 1 byte | 0 = built-in bank, 1 = uploaded bank |
| activeLength | u21 | length of the active bank's text |
| activeHash | u32 | FNV-1a of the active bank's text |
| presetCount | 1 byte | presets in the active bank |

```
host:  F0 7D 43 53 01 F7
pedal: F0 7D 43 53 41 00 01 00 00 06 70 01 00 5C 7E 02 00 7C 53 05 07 0A F7
```

Status: always `Ok`.

### BEGIN (`02`)

Starts an upload session: declares the length and hash of the text that follows. A BEGIN
during a session discards that session and starts over. Audio switches to dry passthrough.

Request body: length u21 (1 .. 98,304), hash u32 (FNV-1a of the whole text).

```
host:  F0 7D 43 53 02 5C 7E 02 00 7C 53 05 07 F7
pedal: F0 7D 43 53 42 00 F7
```

Statuses: `Ok`; `BadLength` if the length is 0 or above 98,304 (no session is started).

### DATA (`03`)

Sends the next piece of the text. Chunks are numbered from 0, and every chunk except the last
should carry exactly 240 bytes (fewer is accepted; more is `BadFrame`). The text bytes go into
the message unchanged: presets.toml is ASCII, so every byte is already below `0x80`.

Request body: seq u14, then 1 to 240 text bytes. Reply body: seq u14 (echo).

```
host:  F0 7D 43 53 03 00 00 23 20 43 6C 6F 75 64 53 65 65 64 F7    (seq 0, "# CloudSeed")
pedal: F0 7D 43 53 43 00 00 00 F7
```

If the reply to a chunk is lost, send the same chunk again: repeating the most recently
accepted seq is answered `Ok` and not appended twice.

Statuses: `Ok`; `NoSession` (no BEGIN, or the session ended); `BadSeq` (not the next or the
repeated seq); `BadLength` (more bytes than BEGIN declared); `BadFrame` (no payload, or more
than 240 bytes). Every status except `Ok` ends the session.

### COMMIT (`04`)

Finishes the upload. The pedal checks the byte count, the hash and the preset rules. If all
pass, it writes the text to flash (up to about 3 s), replies, and reboots 100 ms later.

Request: no body.

```
host:  F0 7D 43 53 04 F7
pedal: F0 7D 43 53 44 00 F7    (then the port disappears for ~3 s)
```

A rejected text gets status 7 and the parser's message as ASCII (at most 200 bytes). Preset
numbers in messages count from 0:

```
pedal: F0 7D 43 53 44 07 70 72 65 73 65 74 20 30 3A 20 62 6C 69 6E 6B 73 20 6F 75 74
       20 6F 66 20 72 61 6E 67 65 20 31 2E 2E 32 30 F7
       ("preset 0: blinks out of range 1..20")
```

Statuses: `Ok`; `NoSession`; `LengthMismatch` (fewer bytes received than declared);
`HashMismatch`; `ParseError`; `FlashError`. COMMIT always ends the session, whatever the
outcome. After anything but `Ok` the pedal keeps running its current bank, with one
exception: a `FlashError` while an uploaded bank is active also reboots, because writing the
new text had already erased the old one. The pedal then comes back on the built-in bank.

### REVERT (`05`)

Discards the uploaded bank. Ends any session, replies, and reboots 100 ms later into the
built-in bank. Also works when no bank is uploaded (the pedal still reboots).

```
host:  F0 7D 43 53 05 F7
pedal: F0 7D 43 53 45 00 F7
```

Statuses: `Ok`; `FlashError` (no reboot).

### READ (`06`)

Reads the active bank's text in 240-byte chunks: chunk `index` covers bytes
`index * 240` up to `index * 240 + 239`. Works at any time and does not touch an upload session.

Request body: index u14. Reply body: index u14, activeLength u21, then the chunk's bytes (240,
or fewer for the last chunk).

```
host:  F0 7D 43 53 06 03 00 F7
pedal: F0 7D 43 53 46 00 03 00 5C 7E 02 <240 text bytes> F7
```

The 48,988-byte example bank is 205 chunks (indices 0 to 204); the last carries 28 bytes.

Statuses: `Ok`; `BadSeq` when `index * 240` is at or past the end of the text.

### ABORT (`07`)

Ends the upload session, if any, without storing anything. Audio returns to normal at once.

```
host:  F0 7D 43 53 07 F7
pedal: F0 7D 43 53 47 00 F7
```

Status: always `Ok`.

### Anything else

An unknown command, or a known command with the wrong body length, is answered with
`BadFrame` (the reply command is still `cmd | 0x40`).

## 8. Status codes

| Code | Name | Meaning | What the host should do |
|---|---|---|---|
| 0 | Ok | Done | Continue |
| 1 | BadFrame | Unknown command or wrong body length | Fix the host code; for DATA, restart from BEGIN |
| 2 | NoSession | DATA or COMMIT without an open session (never begun, timed out, or ended by an error) | Restart from BEGIN |
| 3 | BadSeq | DATA out of order, or READ past the end | Upload: restart from BEGIN. Read: stop, the text is complete |
| 4 | BadLength | BEGIN length 0 or too large, or DATA beyond the declared length | Check the file size; restart from BEGIN |
| 5 | LengthMismatch | COMMIT before all declared bytes arrived | Restart from BEGIN |
| 6 | HashMismatch | Received bytes do not match the declared hash | Check the hash code; restart from BEGIN |
| 7 | ParseError | The text breaks a preset rule; body = the message | Show the message to the user |
| 8 | FlashError | Writing or erasing flash failed | Retry once; if it persists, the flash may be faulty |

## 9. Walkthroughs

**Upload**
1. INFO: check `version == 1`, and that the file is at most `maxTextBytes` long.
2. BEGIN with the file's length and FNV-1a hash.
3. DATA chunks 0, 1, 2, ... of 240 bytes, waiting for each reply before sending the next.
4. COMMIT, and wait up to 10 s for the reply.
5. On `Ok`, wait for the port to disappear and return (about 3 s), reconnect, and send INFO:
   `source` is 1 and `activeHash` equals the file's hash.

```mermaid
sequenceDiagram
    participant H as Host (browser)
    participant P as Pedal
    H->>P: INFO
    P-->>H: version, limits, active bank
    H->>P: BEGIN length, hash
    Note over P: audio: dry passthrough
    P-->>H: Ok
    loop every 240-byte chunk
        H->>P: DATA seq, bytes
        P-->>H: Ok, seq
    end
    H->>P: COMMIT
    Note over P: check length, hash, preset rules;<br/>write flash (up to ~3 s)
    P-->>H: Ok
    Note over P: reboot: USB port gone ~3 s
    H->>P: INFO (after reconnecting)
    P-->>H: source = 1, activeHash = file hash
```

**Read back**
1. READ index 0. The reply's `activeLength` gives the total size.
2. READ index 1, 2, ... until `activeLength` bytes have been collected (or `BadSeq`).
3. Optionally, compare the FNV-1a of the result with INFO's `activeHash`.

**Revert**: REVERT, wait for the reboot, and send INFO: `source` is 0.

## 10. Timing and limits

- **One message at a time.** Send a request only after the previous reply has arrived.
- **Timeouts.** Wait up to 10 s for the COMMIT reply and 1 s for every other reply. If a
  reply does not arrive, resend the same request (DATA repeats are safe) or restart from BEGIN.
- **Session timeout.** A session with no BEGIN or DATA for 5 s ends by itself, and audio
  returns to normal. INFO and READ do not keep a session alive.
- **Limits.** Text at most 98,304 bytes, ASCII only (every byte below `0x80`, comments
  included), at most 16 presets. The chunk size is 240 bytes. The pedal accepts SysEx
  messages of up to 256 bytes between `F0` and `F7`; longer ones are dropped without a reply.
- **Other MIDI traffic.** Real-time bytes (`F8`-`FF`) may appear anywhere and are ignored.
  Any other status byte inside a SysEx message aborts that message.
