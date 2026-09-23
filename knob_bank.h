#ifndef KNOB_BANK_H
#define KNOB_BANK_H

#include <math.h>

#include "preset_bank.h"

// Fraction of full knob travel a knob must move before it takes over its current
// target. Sits well above the ADC noise floor left by the 20 ms one-pole applied
// in main(), and below the smallest deliberate turn.
static const float kKnobPickupThreshold = 0.01f;
// Re-write hysteresis once a knob is live; keeps SetParameter() off the 1 kHz
// jitter treadmill without being audible (1/2000 of range).
static const float kKnobApplyEpsilon = 0.0005f;

// Per-knob take-over tracking across control-bank and preset changes.
//
// Reset() snapshots the current knob positions and parks every knob. A parked
// knob reports no update no matter what its target's value is, so switching
// banks, switching presets, or booting never pushes a stale knob position into a
// parameter. The first physical movement past kKnobPickupThreshold makes that one
// knob live, and it then tracks continuously until the next Reset().
struct KnobBank
{
    float ref[kKnobCount];      // position at the last Reset()
    float applied[kKnobCount];  // last value reported as an update
    bool  live[kKnobCount];
    bool  primed;               // false until Reset() has seen real positions

    KnobBank() : primed(false)
    {
        for (int i = 0; i < kKnobCount; i++)
        {
            ref[i]     = 0.0f;
            applied[i] = 0.0f;
            live[i]    = false;
        }
    }

    void Reset(const float* positions)
    {
        for (int i = 0; i < kKnobCount; i++)
        {
            ref[i]  = positions[i];
            live[i] = false;
        }
        primed = true;
    }

    // True when knob `i` at `value` should be written to its current target.
    bool Update(int i, float value)
    {
        if (!primed)
            return false;

        if (!live[i])
        {
            if (fabsf(value - ref[i]) <= kKnobPickupThreshold)
                return false;
            live[i]    = true;
            applied[i] = value;
            return true;
        }

        if (fabsf(value - applied[i]) <= kKnobApplyEpsilon)
            return false;

        applied[i] = value;
        return true;
    }
};

#endif
