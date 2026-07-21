# Enhanced Preset Fork
This is a fork from https://github.com/optilude/DaisyCloudSeed a fork that improved preset usability

This fork extends those capabilities and adds a few extras:

- More program storage (Moved the application to SRAM)
- Wider range of preset support offered by placing a limit on the number of delay lines for each preset
- *Through the Looking Glass* is available as preset 9 (Delay lines capped at 3 for this one only)
- *Dark Plate*: Preset #10 mostly adapted from from Ghost Note Audio's CloudSeedCore

# DaisyCloudSeed (GuitarML fork for Terrarium)
Cloud Seed is an open source algorithmic reverb plugin under the MIT license, which can be found at [ValdemarOrn/CloudSeed](https://github.com/ValdemarOrn/CloudSeed).
DaisyCloudSeed is a port to the Daisy environment for running on a Daisy Patch unit. This code (GuitarML's fork) further modifies DaisyCloudSeed
for use on the Terrarium guitar pedal. The processing has been changed to mono (from stereo), which allows up to 5 delay lines,
and fills out all of the Terrarium's controls. 

Watch the video demo on [YouTube](https://youtu.be/j-SGRWxBjz0)

![app](https://github.com/GuitarML/DaisyCloudSeed/blob/master/petal/pedal.jpg)

Download the cloudseed.bin for Daisy Seed from the [Releases](https://github.com/GuitarML/DaisyCloudSeed/releases) page.

## Getting started
The new code for Terrarium has been added to ```DaisyCloudSeed/petal```.
Build the daisy libraries and CloudSeed with (after installing the Daisy Toolchain):
```
./rebuild_libs.sh
cd petal/CloudSeed
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

# Control

| Control | Description | Comment |
| --- | --- | --- |
| Ctrl 1 | Dry Level | Adjusts the Dry level out |
| Ctrl 2 | Early Reverberation Level | Adjusts the Early Reverb stage output.  |
| Ctrl 3 | Late Reverberation Level | Adjusts the Late Reverb stage output |
| Ctrl 4 | Late Reverberation Feedback | Adjusts amount of signal fed back through the delay line. |
| Ctrl 5 | Early Reverberation Dampening | Controls amount of dampening for the early reverb stage. Actual parameter name is "TapDecay" |
| Ctrl 6 | Late Reverberation Decay | Adjust the decay time of the late reverberation stage. |
| SW 1 - 4 | Selectable Delay Lines | Turn on or off to engage from 1 to 5 delay lines (1 delay line is always on) The order doesn't matter, just the total number that are on. (i.e., 1st and 4th switch on is the same as 2nd and 3rd switch on)|
| FS 1 | Bypass/Active | Bypass / effect engaged |
| FS 2 | Cycle Preset | Loads the next available Preset, starts at beginning after the last in the list. These are the same as the original Cloud Seed plugin presets, except for "Through the Looking Glass" |
| LED 1 | Bypass/Active Indicator |Illuminated when effect is set to Active |
| LED 2 | Preset indicator | Number of flashes = current preset number |
| Audio In 1 | Audio input | Mono only for Terrarium |
| Audio Out 1 | Mix Out | Mono only for Terrarium |
