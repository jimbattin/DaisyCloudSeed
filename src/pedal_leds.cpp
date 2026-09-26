#include "pedal_leds.h"

using namespace daisy;

// Save / restore confirmation: both LEDs blink together this many times.
constexpr int      CONFIRM_BLINKS   = 3;
constexpr uint32_t CONFIRM_BLINK_MS = 80;  // on and off time

// Blink state machine
struct BlinkState {
    bool         active             = false;
    int          currentBlink       = 0;
    bool         ledOn              = false;
    uint32_t     lastTransitionTime = 0;
    BlinkPattern pattern            = {};
};

static Led gLed1;  // active indicator (on when not bypassed)
static Led gLed2;  // preset indicator
static BlinkState gPresetBlink;

static bool     gConfirmActive  = false;  // main loop only
static uint32_t gConfirmStartMs = 0;

void LedsInit(Pin led1Pin, Pin led2Pin) {
    gLed1.Init(led1Pin, false);
    gLed1.Update();

    gLed2.Init(led2Pin, false);
    gLed2.Update();
}

void SetBypassLed(bool bypass) {
    gLed1.Set(bypass ? 0.0f : 1.0f);
}

void UpdateLeds() {
    gLed1.Update();
    gLed2.Update();
}

// Blink both LEDs at 5 Hz forever and never start audio, so the failure is
// unmistakable on the pedal. Runs before any audio callback, so it drives the LEDs
// itself.
void FatalErrorLoop() {
    while (true) {
        gLed1.Set(1.0f); gLed2.Set(1.0f);
        gLed1.Update(); gLed2.Update();
        System::Delay(100);
        gLed1.Set(0.0f); gLed2.Set(0.0f);
        gLed1.Update(); gLed2.Update();
        System::Delay(100);
    }
}

// Start a blink sequence. LED2 itself is pushed to the pin by the audio callback.
void StartPresetBlink(const BlinkPattern& pattern) {
    gPresetBlink.active = true;
    gPresetBlink.currentBlink = 0;
    gPresetBlink.ledOn = false;
    gPresetBlink.lastTransitionTime = System::GetNow();
    gPresetBlink.pattern = pattern;
    gLed2.Set(0.0f);
}

// Update blink state machine (call this in main loop)
void ServicePresetBlink(bool bypass, const BlinkPattern& pattern) {
    // If bypassed, turn off LED2 and deactivate blinking
    if (bypass) {
        if (gPresetBlink.active) {
            gPresetBlink.active = false;
            gLed2.Set(0.0f);
        }
        return;
    }

    // If not active and not bypassed, restart the blink sequence
    if (!gPresetBlink.active) {
        StartPresetBlink(pattern);
        return;
    }

    uint32_t now = System::GetNow();
    uint32_t elapsed = now - gPresetBlink.lastTransitionTime;

    if (gPresetBlink.ledOn) {
        // LED is currently on, check if it's time to turn it off
        if (elapsed >= gPresetBlink.pattern.onDurationMs) {
            gLed2.Set(0.0f);
            gPresetBlink.ledOn = false;
            gPresetBlink.lastTransitionTime = now;
            gPresetBlink.currentBlink++;
        }
    } else {
        // LED is currently off
        if (gPresetBlink.currentBlink >= gPresetBlink.pattern.numBlinks) {
            // All blinks complete, check if pause is done
            if (elapsed >= gPresetBlink.pattern.pauseAfterMs) {
                // Restart the sequence instead of stopping
                gPresetBlink.currentBlink = 0;
                gPresetBlink.ledOn = false;
                gPresetBlink.lastTransitionTime = now;
            }
        } else {
            // More blinks to go, check if it's time to turn LED on again
            if (elapsed >= gPresetBlink.pattern.offDurationMs) {
                gLed2.Set(1.0f);
                gPresetBlink.ledOn = true;
                gPresetBlink.lastTransitionTime = now;
            }
        }
    }
}

// Main loop only: starts the save/restore confirmation blink.
void StartConfirmBlink() {
    gConfirmActive  = true;
    gConfirmStartMs = System::GetNow();
}

// Drives LED1+LED2 together through CONFIRM_BLINKS on/off cycles, shown even when
// bypassed. Returns false once idle; on completion restores LED1 to the bypass state
// and hands LED2 back to ServicePresetBlink(), which restarts the preset pattern.
bool ServiceConfirmBlink(bool bypass) {
    if (!gConfirmActive)
        return false;
    const uint32_t step = (System::GetNow() - gConfirmStartMs) / CONFIRM_BLINK_MS;
    if (step >= 2u * CONFIRM_BLINKS) {
        gConfirmActive = false;
        gLed1.Set(bypass ? 0.0f : 1.0f);
        gLed2.Set(0.0f);
        gPresetBlink.active = false;
        return false;
    }
    const float level = (step % 2u == 0u) ? 1.0f : 0.0f;
    gLed1.Set(level);
    gLed2.Set(level);
    return true;
}
