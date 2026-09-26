# Hardware validation

A checklist for validating a firmware revision on a real pedal. It covers the USB-MIDI preset
link (docs/USB_MIDI.md) and checks that audio has not regressed. `make test` cannot reach any
of this: USB enumeration, QSPI writes, reboots, and what the pedal sounds like.

Run it for every revision that touches USB, storage, the audio callback, the main loop or the
build flags, and before any release. Record the results in the log at the end.

## Prerequisites

- **The pedal.** Terrarium pedal with a Daisy Seed running the Daisy bootloader
  (`make program-boot` once).
- **Connections.** A data-capable USB cable to the Seed's micro-USB port, plus a guitar or
  other audio source into the pedal and an amp or headphones on the output.
- **Linux host tools.** ALSA (`amidi`), `dfu-util`, Python 3 and the Arm toolchain (see
  CLAUDE.md, "Build System").
- **Test host script.** `tools/usb_preset_host.py`, run from the repo root. It finds the Daisy's
  rawmidi node itself; pass `--dev /dev/snd/midiC<card>D0` if it cannot. Commands below use:
  ```bash
  H="python3 tools/usb_preset_host.py"
  ```

Every flash of a different firmware image wipes the presets saved on the pedal (FS 1 held
5 s), and this checklist flashes several. Write down any saved sounds worth keeping first.

## 0. Baseline (before editing the revision)

The static checks (S3, S4) and the whine test (H11) compare against the previous revision. On
the unmodified tree, before making any change:

```bash
make 2>&1 | tee /tmp/baseline-build.txt
cp build/cloudseed.bin /tmp/baseline.bin
arm-none-eabi-objdump -d --no-show-raw-insn build/cloudseed.elf \
    | awk '/<_ZL13audioCallbackPKPKfPPfj>:/,/^$/' > /tmp/baseline-callback.txt
```

If you have already started editing, build the previous commit in a separate worktree
(`git worktree add /tmp/cs-baseline <commit>`, then `git submodule update --init --recursive`,
`make libs` and `make` in it) and take the same three files from there.

## Static checks (no pedal)

- **S1.** `make test`. Pass: every suite prints `N checks, 0 failed`.
- **S2.** `make presets-check`. Pass: `presets.toml: 10 presets valid, …`.
- **S3. Audio callback code.** Dump the new callback and compare its calls with the baseline:
  ```bash
  arm-none-eabi-objdump -d --no-show-raw-insn build/cloudseed.elf \
      | awk '/<_ZL13audioCallbackPKPKfPPfj>:/,/^$/' > /tmp/new-callback.txt
  calls() { grep -oE 'bl\s+[0-9a-f]+ <[^>]+>' "$1" | awk '{print $3}' | sort; }
  diff <(calls /tmp/baseline-callback.txt) <(calls /tmp/new-callback.txt) && echo SAME-CALLS
  wc -l /tmp/baseline-callback.txt /tmp/new-callback.txt
  ```
  Pass: `SAME-CALLS`, unless the revision meant to add work to the callback. A large growth
  in line count needs an explanation.
- **S4. Memory.** Run `make 2>&1 | tee /tmp/new-build.txt` and compare its memory table with
  `/tmp/baseline-build.txt`. Pass: every region is below 90 %, and every change is explained.

## Hardware tests

Flash the revision under test with `make program-dfu`: reset the Seed while holding BOOT, and
wait for the LED to blink rapidly before running the command.

### H1. Enumeration

`amidi -l` lists a port named after "Daisy Seed Built In". The pedal behaves as usual: LED2
blinks the preset number and audio passes.

### H2. INFO on the built-in bank

```bash
$H info
stat -c %s presets.toml
$H fnv presets.toml
```

Pass: `source=0` and `presetCount=10`. `activeLength` equals the file size and `activeHash`
equals the `fnv` output.

### H3. Read-back of the built-in bank

```bash
$H read /tmp/rb.toml && cmp /tmp/rb.toml presets.toml && echo MATCH
```

Pass: `MATCH`.

### H4. A rejected upload changes nothing

```bash
sed '0,/^blinks = 1$/s//blinks = 99/' presets.toml > /tmp/bad.toml
diff presets.toml /tmp/bad.toml     # exactly one line changed
$H upload /tmp/bad.toml
```

Pass:
- COMMIT prints `ParseError … preset 0: blinks out of range 1..20`.
- The pedal does not reboot, and the reverb is back right after the reply.
- `$H info` still shows `source=0`.

The `sed` assumes the first preset has `blinks = 1`. If it does not, edit any one value
out of range by hand.

### H5. A valid upload

```bash
sed '0,/^blinks = 1$/s//blinks = 3/' presets.toml > /tmp/p.toml
diff presets.toml /tmp/p.toml       # exactly one line changed
$H upload /tmp/p.toml
```

Pass:
- The output goes dry during the upload.
- COMMIT prints `Ok`, then the USB port disappears and returns after about 3 s.
- `$H info` shows `source=1` and `activeHash` equals `$H fnv /tmp/p.toml`.
- `$H read /tmp/rb.toml && cmp /tmp/rb.toml /tmp/p.toml` matches.
- On preset 1, LED2 blinks 3 times.
- The preset and bypass state from before the upload are restored after the reboot.

### H6. Session timeout and ABORT

```bash
$H begin /tmp/p.toml        # listen: dry immediately, reverb returns ~5 s later
$H beginabort /tmp/p.toml   # listen: dry, then reverb returns at once
```

Pass:
- The reverb returns when stated, with no click louder than a preset change.
- Knobs turned during the dry period do not jump the sound when the reverb returns; they are
  parked.

### H7. A bank change wipes saved presets

1. On the uploaded bank from H5, turn a knob until the sound is obviously different. Hold
   FS 1 for 5 s: both LEDs blink 3 times.
2. Power-cycle the pedal. Pass: the saved sound is still there.
3. Upload a different bank:
   ```bash
   sed '0,/^blinks = 1$/s//blinks = 4/' presets.toml > /tmp/p2.toml
   $H upload /tmp/p2.toml
   ```
   Pass: after the reboot, preset 1 sounds as the file defines (the saved sound is gone), and
   LED2 blinks 4 times.

### H8. REVERT

```bash
$H revert
```

Pass: `REVERT: Ok` and a reboot. `$H info` then shows `source=0` with the built-in hash from
H2, and preset 1 blinks once.

### H9. Flashing different firmware discards an upload

1. `$H upload /tmp/p.toml`, then check that `$H info` shows `source=1`.
2. Build a different image: add one comment line at the end of presets.toml, then
   `make program-dfu`.
3. `$H info` shows `source=0`. The stored bank is still in flash, but this image ignores it.
4. `$H revert` erases it; the pedal reboots.
5. Remove the comment line and run `make program-dfu` again. `$H info` shows `source=0`.

The comment line is what makes the image differ. A byte-identical reflash cannot be told apart
from a reboot, so it keeps an uploaded bank; that is intended. Without the REVERT in step 4, the
upload would therefore come back in step 5.

### H10. Audio headroom under USB traffic

Select preset 9, "Through the Looking Glass", with SWITCH_1 on (4 delay lines, the heaviest
shipped configuration), not bypassed, with audio playing through it.

1. Listen for 30 s with the USB link idle.
2. Run `$H flood 30` (INFO every 2 ms plus repeated full READs, with no upload session, so the
   reverb keeps running) and listen for those 30 s.

Pass:
- No crackle in either period.
- `flood` ends with `no errors`.

If step 2 crackles but step 1 does not, the USB traffic is stealing callback time. The planned
remedy is to also pass audio through for 5 s after every INFO or READ.

### H11. 1 kHz whine A/B

1. Flash the baseline:
   ```bash
   dfu-util -a 0 -s 0x90040000:leave -D /tmp/baseline.bin -d ,0483:df11
   ```
   Listen to the 1 kHz whine, bypassed and active, at a fixed amp volume.
2. Flash the revision under test with `make program-dfu` and repeat.

Pass: the whine is no louder on the revision under test.

## Results log

Copy one row per validated revision.

| Date | Commit | Tester | S1-S4 | H1-H9 | H10 | H11 | Notes |
|---|---|---|---|---|---|---|---|
| 2026-09-26 | 89dc63a | jbattin | pass | pass | pass | pass | Baseline c5609ec (last pre-USB). S3 SAME-CALLS, 1780 -> 1766 lines. S4: DTCM +15,136 B (USB stack), SRAM +11,108 B, RAM_D2_DMA +988 B (MIDI rx/tx), SDRAM +627,424 B (toml_arena 512 KiB + gUploadText 96 KiB + gUploadCheck). H10 flood: 4,250 INFO, 85 READ-all, no errors. |
