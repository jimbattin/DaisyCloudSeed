#ifndef KNOB_BANK_H
#define KNOB_BANK_H

#include <math.h>

#include "preset_bank.h"

// How far a parked knob must move from its Reset() snapshot before it takes over its
// target, as a fraction of full travel. Must sit far above the ADC noise left by the
// 20 ms one-pole applied in main().
constexpr float kKnobMoveThreshold = 0.01f;

// A live knob tracks the pot but does not re-write the engine for noise: a change
// smaller than this (0..1 knob space) is dropped. Rail values are exempt so the last
// fraction of travel into a stop still lands exactly.
constexpr float kKnobApplyEpsilon = 0.001f;

// Positions this close to a rail are written as exactly 0.0 / 1.0. The top reading is
// at most 65535/65536, and libdaisy documents ~0.002 of bleed at the bottom of the pots
// (libdaisy/src/hid/ctrl.cpp:3-4).
constexpr float kKnobRailWindow = 0.003f;

// Takeover glide length in Update() calls, i.e. audio blocks (1 ms each): the first
// write after a knob goes live ramps from the target's current value to the pot instead
// of stepping, because engine parameters are not smoothed downstream.
constexpr int kKnobGlideBlocks = 50;

struct KnobWrite
{
    int   knob;   // 0..kKnobCount-1, presets.toml knob order
    float value;  // 0..1
};

// Absolute knob takeover across control-bank and preset changes.
//
// Reset() snapshots knob positions and parks every knob. A parked knob writes nothing,
// so power-up, a preset load, and entering or leaving the secondary bank never change a
// parameter on their own. A knob that moves kKnobMoveThreshold from its snapshot goes
// live and glides its target linearly from the target's current value to the pot's
// (possibly still moving) position over kKnobGlideBlocks calls, landing exactly on it.
// From then on it writes its own position whenever that changes by kKnobApplyEpsilon
// or reaches a rail. Once the glide ends, the knob position is the parameter value.
// A Reset() during a glide cancels it and leaves the target where the glide had got to.
class KnobBank
{
public:
    // One callback's worth of knob handling. Re-parks every knob when the bank changed,
    // when `forceReset` is set (a preset was loaded), or on the first call; then fills
    // `writes` (capacity kKnobCount) and returns how many. `currentValues[i]` is the
    // current value of knob i's target in `bank`. A call that re-parks returns 0.
    int Scan(int bank, bool forceReset, const float* positions, const float* currentValues,
             KnobWrite* writes)
    {
        if (!primed || forceReset || bank != activeBank)
            Reset(positions, bank);

        int n = 0;
        for (int i = 0; i < kKnobCount; i++)
        {
            float value;
            if (Update(i, positions[i], currentValues[i], value))
            {
                writes[n].knob  = i;
                writes[n].value = value;
                n++;
            }
        }
        return n;
    }

private:
    float snapshot[kKnobCount]  = {};  // position at the last Reset()
    float applied[kKnobCount]   = {};  // last value written (glide start before the first write)
    float glideFrom[kKnobCount] = {};  // target value when the knob went live
    int   glideStep[kKnobCount] = {};  // 0 = not gliding, else next step 1..kKnobGlideBlocks
    bool  live[kKnobCount]      = {};  // true once the knob has taken over its target
    int   activeBank            = 0;   // bank the snapshot was taken for
    bool  primed                = false;  // false until Reset() has seen real positions

    void Reset(const float* positions, int bank)
    {
        for (int i = 0; i < kKnobCount; i++)
        {
            snapshot[i]  = positions[i];
            applied[i]   = 0.0f;
            glideFrom[i] = 0.0f;
            glideStep[i] = 0;
            live[i]      = false;
        }
        activeBank = bank;
        primed     = true;
    }

    // Reports the value knob `i` should write this block. `currentValue` is the target's
    // current value (only read on the call that takes over). Returns false, writing
    // nothing to `out`, while parked or while the value would not change.
    bool Update(int i, float position, float currentValue, float& out)
    {
        if (!primed)
            return false;

        float target = position;
        if (target <= kKnobRailWindow)
            target = 0.0f;
        else if (target >= 1.0f - kKnobRailWindow)
            target = 1.0f;

        if (!live[i])
        {
            // The parked test uses the raw position, so a knob resting against a stop
            // is still parked and cannot slam its target to the rail.
            if (fabsf(position - snapshot[i]) < kKnobMoveThreshold)
                return false;
            live[i]      = true;
            glideFrom[i] = currentValue;
            applied[i]   = currentValue;
            glideStep[i] = 1;
        }

        float value;
        if (glideStep[i] > 0)
        {
            const int k = glideStep[i];
            if (k >= kKnobGlideBlocks)
            {
                value        = target;  // land exactly on the pot
                glideStep[i] = 0;
            }
            else
            {
                value = glideFrom[i]
                        + (target - glideFrom[i]) * ((float)k / (float)kKnobGlideBlocks);
                glideStep[i] = k + 1;
            }
            if (value == applied[i])
                return false;
        }
        else
        {
            value = target;
            if (value == applied[i])
                return false;
            const bool atRail = (value == 0.0f || value == 1.0f);
            if (!atRail && fabsf(value - applied[i]) < kKnobApplyEpsilon)
                return false;
        }

        applied[i] = value;
        out        = value;
        return true;
    }
};

#endif
