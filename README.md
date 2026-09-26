# Enhanced Preset Fork
This is a fork from https://github.com/optilude/DaisyCloudSeed a fork that improved preset usability

This fork extends those capabilities and adds a few extras:

- Switch remap: SW1 selects delay-line count (the preset's default vs. its maximum), SW3
  engages a classic Reverse Delay, SW4 routes that reverse (off = into the reverb's wet tail,
  on = straight into the output mix), and SW2 keeps the "Bloom" effect, similar to a "reverse
  reverb" (Bloom reverses the order of tap gains. Has no effect on patches with a single tap.)
- Per-preset knob mapping: each preset's `[preset.knob_map]` in
  [presets.toml](presets.toml) assigns all six knobs a primary function and a **secondary**
  function reached by holding the **preset footswitch (FS 2)** down
- Per-preset toggle mapping: each preset's `[preset.toggle_map]` assigns all four toggle
  switches a primary and a secondary (FS 2 held) on/off target; the defaults are the SW1-SW4
  functions above
- More program storage (Moved the application to SRAM)
- Wider range of preset support offered by placing a limit on the number of delay lines for each preset
- *Through the Looking Glass* is available as preset 9 (Delay lines capped at 4 for this one only)
- *Dark Plate*: Preset #10 mostly adapted from from Ghost Note Audio's CloudSeedCore
- Delay line count is toggled by SW1 between each preset's `default_delay_lines` and
  `max_delay_lines` (both set in presets.toml; 2 and up to 5 as shipped)
- Bypass state is saved between power cycles
- Per-preset user save: hold FS 1 for 5 s to store the current sound into the current preset;
  hold FS 1 + FS 2 together for 5 s to restore that preset to its factory values. Saved
  presets survive power cycles and are discarded when different firmware is flashed
- Agent-guided performance optimizations
- Reduced 1khz whine while active or bypassed 
- Presets are defined in [presets.toml](presets.toml), not in C++. The file is compiled into
  the firmware image and parsed at boot; edit values there and reflash

# DaisyCloudSeed (GuitarML fork for Terrarium)
Cloud Seed is an open source algorithmic reverb plugin under the MIT license, which can be found at [ValdemarOrn/CloudSeed](https://github.com/ValdemarOrn/CloudSeed).
DaisyCloudSeed is a port to the Daisy environment for running on a Daisy Patch unit. This code (GuitarML's fork) further modifies DaisyCloudSeed
for use on the Terrarium guitar pedal. The processing has been changed to mono (from stereo), which allows up to 5 delay lines,
and fills out all of the Terrarium's controls. 

![Pedal Picture](pedal.png)

Download the cloudseed.bin for Daisy Seed from the [Releases](https://github.com/GuitarML/DaisyCloudSeed/releases) page.

## Getting started
The new code for Terrarium has been added to ```DaisyCloudSeed/petal```.
Build the daisy libraries and CloudSeed with (after installing the Daisy Toolchain):
```
make libs
make
```

Then flash your terrarium with the following commands (or use the [Electrosmith Web Programmer](https://electro-smith.github.io/Programmer/))
**NOTE** This fork (of a fork) uses BOOT_SRAM, so you'll need to program the bootloader accordingly. A hardware bug in either the bootloader or your Daisy itself may also require you to follow a goofy little procedure to actually get program-dfu to work
```
# from the petal/CloudSeed directory...
# using USB (after entering bootloader mode)
make program-boot
# Hit restart and then hold Boot on your DaisySeed until you see a rapid blink
make program-dfu
```

## Editing presets

All ten presets live in [presets.toml](presets.toml) at the repo root - names, LED blink
timing, the per-preset default and maximum delay-line counts, all 46 reverb parameters, the
knob and toggle maps, the reverse-delay window (`[preset.params.reverse] delay`), and the
stored state of every toggle target. The file is embedded into the firmware image, so changing a
preset is:
```
# edit presets.toml, then:
make                 # validates presets.toml, then builds
make program-dfu
```
`make` runs the file through the same parser the pedal uses and refuses to build firmware if it
fails: unknown or misplaced parameters, a missing group, an unknown preset or top-level key, a
bad range, TOML syntax errors, or a document too large for the boot-time parse arena. So a
broken preset file can no longer reach the pedal - where the only symptom would be **both**
LEDs blinking together at 5 Hz with no audio.

`make presets-check` runs the same check without building firmware. It fails only on a file the
parser rejects; preset values themselves are free to change.

Preset order is stored in flash: append new `[[preset]]` entries at the end, and bump
`SETTINGS_VERSION` in `cloudseed.cpp` if you reorder or delete any.

## Saving and restoring presets on the pedal

presets.toml is the **factory** version of every preset and is never modified on the pedal.

- **Save**: hold FS 1 for 5 s. The current sound - all reverb parameters, including every knob
  change, plus the reverse-delay window and the state of every toggle target - replaces the
  current preset. Only that preset is
  affected. Both LEDs blink 3 times quickly to confirm, and releasing FS 1 does not toggle
  bypass.
- **Restore factory**: hold FS 1 and FS 2 together for 5 s. They need not go down or come up at
  exactly the same moment. The current preset reverts to its presets.toml values right away,
  and its saved version is erased. Both LEDs blink 3 times; no bypass toggle or preset change
  happens on release.

Saved presets are stored in QSPI flash alongside the bypass/preset settings and survive power
cycles. Flashing any different firmware (including a presets.toml edit) discards every saved
preset, so the pedal boots with the factory versions. Re-flashing a byte-identical `.bin` keeps
them. The knob and toggle maps, the delay-line counts, and blink timing always come from
presets.toml.

## Knob mapping

Every preset carries a `[preset.knob_map]` table naming what each knob does:

```toml
[preset.knob_map]
knob1_a = "output.DryOut"        # primary: knob 1 normally
...
knob1_b = "input.PreDelay"       # secondary: knob 1 while FS 2 is held
knob6_b = "reverse.delay"        # the reverse-delay window length ([preset.params.reverse] delay)
```

Each value is a quoted `"group.Parameter"` naming a parameter from that preset's own
`[preset.params.<group>]` section, or the pseudo-target `"reverse.delay"`. All twelve keys are
required and `make` rejects an unknown knob key, an unknown group or parameter, a parameter
that lives in a different group, and `LineCount` (it belongs to the `delay_lines.max` toggle
target).

Knobs are **absolute**: once a knob has taken over, its position is the parameter value. After
power-up, a preset change, or switching knob banks (including letting go of FS 2)
every knob is *parked* and writes nothing, so a freshly loaded preset sounds exactly as authored -
even with a knob sitting at a stop. The first deliberate turn takes the parameter to the knob's
position with a 50 ms glide from its current value (no step, no click), and from then on the
parameter follows the knob 1:1. The stops are exact: fully counter-clockwise is 0.0 (that is
what makes the three output-level knobs silence the pedal) and fully clockwise is 1.0. A knob
dialled in the secondary bank is parked when you release the footswitches; its next turn glides
its primary target to the knob's position.

Holding FS 2 is a gesture, not a tap: the bank stays on the secondary map until FS 2 is
released. If any secondary knob changed a parameter during the hold, the release does nothing;
if none did, the release cycles to the next preset as a tap would. A knob brushed by less than
1% of its travel does not count.

`input.HighPass` and `input.LowPass` switch their filter on the first time their knob is
turned (unless a toggle targets that filter's enable - then the toggle owns it); every other
gated parameter (the shelves, the in-loop cutoff, the diffusers) must be enabled in
`[preset.params.*]` or by a toggle to be audible.

## Toggle mapping

Every preset also carries a `[preset.toggle_map]` table naming what each toggle switch does
(toggle1..toggle4 = SW 1..SW 4):

```toml
[preset.toggle_map]
toggle1_a = "delay_lines.max"      # primary: SW 1 normally
toggle2_a = "early.isReverse"
toggle3_a = "reverse.enabled"
toggle4_a = "reverse.direct_mix"
toggle1_b = "delay_lines.max"      # secondary: SW 1 flipped while FS 2 is held
toggle2_b = "early.isReverse"
toggle3_b = "reverse.enabled"
toggle4_b = "reverse.direct_mix"
```

All eight keys are required. Accepted targets (lever up = on):

| Target | Notes |
| --- | --- |
| `delay_lines.max` | Off = the preset's `default_delay_lines`, on = its `max_delay_lines` (default SW 1) |
| `early.isReverse` | Bloom (default SW 2) |
| `reverse.enabled` | Reverse voice on (default SW 3) |
| `reverse.direct_mix` | Off = reverse feeds the reverb tail, on = reversed reverb mixed into the output (default SW 4) |
| `input.HiPassEnabled`, `input.LowPassEnabled` | Input high-pass / low-pass on/off. While a toggle targets one, knobs on `HighPass` / `LowPass` no longer switch that filter on |
| `early_diffusion.DiffusionEnabled`, `late_diffusion.LateDiffusionEnabled` | Flipping clears that diffuser's buffers |
| `early_diffusion.DiffusionStages`, `late_diffusion.LateDiffusionStages` | Off = 1 allpass stage, on = 2; a flip overwrites the stored stage value with 0.0/1.0 |
| `late_eq.LowShelfEnabled`, `late_eq.HighShelfEnabled`, `late_eq.CutoffEnabled` | Low shelf / high shelf / low-pass on the tail on/off (inside each line's feedback path) |
| `late.LateStageTap` | On = each line outputs from before its delay (after the late diffuser, which then runs first), so the tail starts one `LineDelay` sooner; off = output after the delay |
| `late.Interpolation` | More CPU; may crackle at 5 lines |

Continuous parameters, `LineCount`, `reverse.delay` (knob only) and `InputMix`/`CrossSeed`
(no effect in mono) are rejected. The stored state of each target is part of the preset: the
reverb parameters in `[preset.params.*]`, and `[preset.params.delay_lines] max` plus
`[preset.params.reverse] enabled` / `direct_mix` for the three pedal functions (all 0.0 = off
as shipped).

Toggles are **parked** like knobs: after power-up, a preset change, or switching banks the
preset's stored values apply whatever the levers say, and a lever writes nothing until it is
flipped. Then it sets its target to the lever position, and follows the lever from there. So
after a preset loads with a lever already up, flip it down and up again to turn its target on.
Flipping a toggle while FS 2 is held writes its `_b` target and, like a secondary knob turn,
cancels the preset change on the FS 2 release.

# Control

| Control | Description | Comment |
| --- | --- | --- |
| Ctrl 1 | Dry Level | Adjusts the Dry level out |
| Ctrl 2 | Early Reverberation Level | Adjusts the Early Reverb stage output.  |
| Ctrl 3 | Late Reverberation Level | Adjusts the Late Reverb stage output |
| Ctrl 4 | Late Reverberation Feedback | Adjusts amount of signal fed back through the late diffusion allpass chain. |
| Ctrl 5 | Early Reverberation Dampening | Controls amount of dampening for the early reverb stage. Actual parameter name is "TapDecay" |
| Ctrl 6 | Late Reverberation Decay | Adjust the decay time of the late reverberation stage. |
| SW 1 | Delay Lines (default target `delay_lines.max`) | Off = the preset's `default_delay_lines` (2 in every shipped preset); On = its `max_delay_lines` (5, or 4 for "Through the Looking Glass"). |
| SW 2 | Bloom (default target `early.isReverse`) | Reverses the order of multi-tap delay gains, resulting in subsequent taps getting louder rather than quietier. |
| SW 3 | Reverse Delay (default target `reverse.enabled`) | Off = dry + reverb only; On = enables the reverse voice, routed per `reverse.direct_mix` (SW 4 by default: into the reverb tail, or mixed straight into the output). Its window length is the knob mapped to `reverse.delay` (Ctrl 6 secondary by default). |
| SW 4 | Reverse Routing (default target `reverse.direct_mix`) | Chooses where the reverse goes. Off = into the reverb (the reversed guitar feeds the wet tail; the forward dry pass-through stays clean via dry-gain cancellation). On = direct mix (a reversed copy of the reverb output is mixed straight into the output). Only audible when the reverse voice is on. |
| FS 1 | Bypass/Active | Bypass / effect engaged. Acts on **release**. A release after a 5 s hold, or while FS 2 is (or was, during the same press) held, does not toggle bypass. |
| FS 1 (held 5 s) | Save preset | Stores the current sound into the current preset (survives power cycles; see "Saving and restoring presets"). Both LEDs blink 3× to confirm. |
| FS 1 + FS 2 (held 5 s) | Restore factory preset | Reverts the current preset to its presets.toml values and erases its saved version. Both LEDs blink 3× to confirm. |
| FS 2 | Cycle Preset | Tap: loads the next available Preset, starts at beginning after the last in the list. Acts on **release**. These are the same as the original Cloud Seed plugin presets, except for "Through the Looking Glass" |
| FS 2 (held) | Secondary knob and toggle bank | While held, every knob controls its `knobN_b` target instead of `knobN_a`, and a toggle flip writes its `toggleN_b` target. The release skips the preset change if a secondary knob was turned or a toggle flipped during the hold, or if FS 1 was also pressed. |
| LED 1 | Bypass/Active Indicator |Illuminated when effect is set to Active. Blinks 3× with LED 2 to confirm a save or restore. |
| LED 2 | Preset indicator | Number of flashes = current preset number. Blinks 3× with LED 1 to confirm a save or restore. |
| Audio In 1 | Audio input | Mono only for Terrarium |
| Audio Out 1 | Mix Out | Mono only for Terrarium |
