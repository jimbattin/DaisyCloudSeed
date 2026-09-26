#ifndef PEDAL_LEDS_H
#define PEDAL_LEDS_H

#include <stdint.h>

#include "daisy_seed.h"

// LED2 preset-indicator timing, cached per preset by loadPreset().
struct BlinkPattern {
    int numBlinks;           // Number of times to blink
    uint32_t onDurationMs;   // How long LED stays on per blink
    uint32_t offDurationMs;  // How long LED stays off between blinks
    uint32_t pauseAfterMs;   // Pause after all blinks complete
};

// Boot: configures LED1 (active indicator) and LED2 (preset indicator), both off.
void LedsInit(daisy::Pin led1Pin, daisy::Pin led2Pin);
// LED1 lit while the effect is active.
void SetBypassLed(bool bypass);
// Pushes both LEDs to their pins. Once audio runs only the audio callback may call
// this: Led::Update() is a read-modify-write and would race with the main loop.
void UpdateLeds();
// Unrecoverable boot failure (preset parse, SDRAM pool exhausted, unexpected audio
// block size): both LEDs at 5 Hz forever, never returns. Drives the pins itself.
[[noreturn]] void FatalErrorLoop();
// Main loop only: LED2 preset pattern.
void StartPresetBlink(const BlinkPattern& pattern);
void ServicePresetBlink(bool bypass, const BlinkPattern& pattern);
// Main loop only: save/restore confirmation on LED1+LED2. ServiceConfirmBlink()
// returns false once idle; on completion it sets LED1 from `bypass` and hands LED2
// back to ServicePresetBlink().
void StartConfirmBlink();
bool ServiceConfirmBlink(bool bypass);

#endif
