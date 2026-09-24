#ifndef KNOB_BANK_H
#define KNOB_BANK_H

#include <math.h>

#include "preset_bank.h"

// Smallest change in knob position, as a fraction of full travel, that counts as a
// deliberate turn. Must stay above the ADC noise floor left by the 20 ms one-pole
// applied in main(): every accepted wobble walks the target value. It is also the
// coarsest step a knob can make, so a full sweep is ~100 increments.
static const float kKnobMoveThreshold = 0.01f;

// Positions this close to a rail count as "at the stop": the pot/ADC rarely reads an
// exact 0.0 or 1.0 (Process() tops out at 65535/65536), and a target that stalls a
// hair short of the endpoint is the bug this window closes. Two orders of magnitude
// below kKnobMoveThreshold, and only consulted inside an accepted movement, so a knob
// merely resting against a stop still writes nothing.
static const float kKnobRailWindow = 0.001f;

// Guard for the scaling divisors below.
static const float kKnobRangeEpsilon = 1e-4f;

// Range-scaled relative (jump-free) knob tracking across control-bank and preset
// changes.
//
// A knob never writes its absolute position, because the parameter it addresses is
// authored by the preset and has no relationship to where the pot happens to sit.
// Reset() snapshots knob positions only; each later movement past kKnobMoveThreshold
// is applied as a delta scaled by the range still available in the direction of
// travel, so turning a knob to a stop lands its target exactly on 0.0 or 1.0 from any
// starting position, while a partial turn moves the value proportionally. An
// untouched knob leaves its target exactly as the preset authored it, a knob turned in
// one bank cannot disturb the other bank's target (Reset() re-snapshots on every
// transition), and takeover never jumps to the pot's absolute position. The exactness
// at the stops comes from a rail snap that a knob must earn: only a knob that has
// already been turned since the last Reset() (`engaged`) may complete its travel into
// a rail with a sub-deadband movement.
struct KnobBank
{
    float last[kKnobCount];     // knob position at the last accepted movement
    bool  engaged[kKnobCount];  // true once this knob has been turned since Reset()
    bool  primed;               // false until Reset() has seen real positions

    KnobBank() : primed(false)
    {
        for (int i = 0; i < kKnobCount; i++)
        {
            last[i]    = 0.0f;
            engaged[i] = false;
        }
    }

    void Reset(const float* positions)
    {
        for (int i = 0; i < kKnobCount; i++)
        {
            last[i]    = positions[i];
            engaged[i] = false;
        }
        primed = true;
    }

    // Applies knob `i`'s movement since the last accepted update to `currentValue`,
    // writing the new target value to `out`. Returns false (and writes nothing) when
    // the knob has not moved far enough to count as a deliberate turn, or when the
    // movement leaves the target value unchanged.
    bool Update(int i, float position, float currentValue, float& out)
    {
        if (!primed)
            return false;

        const float from     = last[i];
        const float delta    = position - from;
        const bool  atBottom = position <= kKnobRailWindow;
        const bool  atTop    = position >= 1.0f - kKnobRailWindow;

        const bool moved = fabsf(delta) >= kKnobMoveThreshold;

        // A turn already in progress must land exactly on the endpoint even when its
        // final few degrees of travel fall inside the deadband: downward scaling keeps
        // the value proportional to the position, so a turn whose last accepted update
        // sat at 0.005 would otherwise stop at 0.005/start of the authored value - the
        // residual this whole change exists to remove. `engaged` restricts that to a
        // knob the user is actually turning: it is only set by an accepted movement and
        // is cleared by Reset(), so a knob that merely rests against a stop at power-up,
        // on a preset change, or on a bank change never slams its target to the rail.
        const bool railArrival = engaged[i]
                                 && ((atBottom && delta < 0.0f) || (atTop && delta > 0.0f));

        if (!moved && !railArrival)
            return false;

        last[i] = position;
        if (moved)
            engaged[i] = true;

        float value;
        if (delta > 0.0f)
        {
            const float headroom = 1.0f - from;
            value = (headroom <= kKnobRangeEpsilon)
                        ? 1.0f
                        : currentValue + delta * (1.0f - currentValue) / headroom;
        }
        else
        {
            value = (from <= kKnobRangeEpsilon)
                        ? 0.0f
                        : currentValue + delta * currentValue / from;
        }

        // At the stop, be exactly at the endpoint: the scaled arithmetic lands within
        // a float ulp of it, and the pot may bottom out a hair above zero.
        if (atBottom)
            value = 0.0f;
        else if (atTop)
            value = 1.0f;

        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;

        // Nothing to write: a knob held against a stop whose target is already at that
        // endpoint keeps re-arriving, and every write costs a SetParameter plus an
        // output-level recompute.
        if (value == currentValue)
            return false;

        out = value;
        return true;
    }
};

#endif
