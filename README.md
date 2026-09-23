# Enhanced Preset Fork
This is a fork from https://github.com/optilude/DaisyCloudSeed a fork that improved preset usability

This fork extends those capabilities and adds a few extras:

- Switch remap: SW1 selects delay-line count (2 vs. the preset maximum), SW3 engages a
  classic Reverse Delay (with Ctrl 4 becoming its 20 ms - 2 s time control), SW4 routes that
  reverse (off = into the reverb's wet tail, on = straight into the output mix), and SW2 keeps
  the "Bloom" effect, similar to a "reverse reverb" (Bloom reverses the order of tap gains.
  Has no effect on patches with a single tap.)
- More program storage (Moved the application to SRAM)
- Wider range of preset support offered by placing a limit on the number of delay lines for each preset
- *Through the Looking Glass* is available as preset 9 (Delay lines capped at 4 for this one only)
- *Dark Plate*: Preset #10 mostly adapted from from Ghost Note Audio's CloudSeedCore
- Delay line count is toggled by SW1 (off = 2 lines, on = the preset's maximum, up to 5)
- Bypass state is saved between power cycles
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
timing, the per-preset delay-line cap, and all 45 reverb parameters. The file is embedded into
the firmware image, so changing a preset is:
```
# edit presets.toml, then:
make presets-check   # validates the file with the same parser the pedal uses
make
make program-dfu
```
`make presets-check` fails on an unknown or misplaced parameter, a missing group, a bad range,
or any drift from the original factory values. If a broken file is ever flashed, the pedal
blinks **both** LEDs together at 5 Hz and stays silent instead of running with junk settings.

Preset order is stored in flash: append new `[[preset]]` entries at the end, and bump
`SETTINGS_VERSION` in `cloudseed.cpp` if you reorder or delete any.

# Control

| Control | Description | Comment |
| --- | --- | --- |
| Ctrl 1 | Dry Level | Adjusts the Dry level out |
| Ctrl 2 | Early Reverberation Level | Adjusts the Early Reverb stage output.  |
| Ctrl 3 | Late Reverberation Level | Adjusts the Late Reverb stage output |
| Ctrl 4 | Late Reverberation Feedback **/ Reverse Time** | With SW3 off: adjusts amount of signal fed back through the delay line. With SW3 on: sets the reverse window from 20 ms (fully CCW) to 2 s (fully CW), antilog (~537 ms at centre), and the feedback falls back to the active preset's value from `presets.toml`. |
| Ctrl 5 | Early Reverberation Dampening | Controls amount of dampening for the early reverb stage. Actual parameter name is "TapDecay" |
| Ctrl 6 | Late Reverberation Decay | Adjust the decay time of the late reverberation stage. |
| SW 1 | Delay Lines | Off = 2 delay lines; On = the current preset's maximum (5, or 4 for "Through the Looking Glass"). |
| SW 2 | Bloom | Reverses the order of multi-tap delay gains, resulting in subsequent taps getting louder rather than quietier. |
| SW 3 | Reverse Delay | Off = dry + reverb only; On = enables the reverse voice, routed per SW4 (into the reverb tail, or mixed straight into the output), with Ctrl 4 setting its length. |
| SW 4 | Reverse Routing | Chooses where the SW3 reverse goes. Off = into the reverb (the reversed guitar feeds the wet tail; the forward dry pass-through stays clean via dry-gain cancellation). On = direct mix (a reversed copy of the reverb output is mixed straight into the output). Only audible when SW3 is on. |
| FS 1 | Bypass/Active | Bypass / effect engaged |
| FS 2 | Cycle Preset | Loads the next available Preset, starts at beginning after the last in the list. These are the same as the original Cloud Seed plugin presets, except for "Through the Looking Glass" |
| LED 1 | Bypass/Active Indicator |Illuminated when effect is set to Active |
| LED 2 | Preset indicator | Number of flashes = current preset number |
| Audio In 1 | Audio input | Mono only for Terrarium |
| Audio Out 1 | Mix Out | Mono only for Terrarium |
