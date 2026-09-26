#ifndef TOGGLE_BANK_H
#define TOGGLE_BANK_H

#include "preset_bank.h"

struct ToggleWrite
{
    int  toggle;  // 0..kToggleCount-1, presets.toml toggle order
    bool on;      // lever up
};

// Parked toggle takeover, the lever counterpart of KnobBank. Reset() snapshots every
// lever and parks it: power-up, a preset load, and entering or leaving the secondary
// bank never change a target on their own, so a loaded preset keeps its stored toggle
// values whatever the levers say. A lever that differs from its snapshot takes over
// and writes its position; from then on it writes on every change. No glide: every
// toggle target is on/off.
class ToggleBank
{
public:
    // One callback's worth of toggle handling. Re-parks when the bank changed, when
    // `forceReset` is set (a preset was loaded), or on the first call; a re-parking
    // call returns 0. Otherwise fills `writes` (capacity kToggleCount), returns how many.
    int Scan(int bank, bool forceReset, const bool* positions, ToggleWrite* writes)
    {
        if (!primed || forceReset || bank != activeBank)
        {
            Reset(bank, positions);
            return 0;
        }
        int n = 0;
        for (int i = 0; i < kToggleCount; i++)
        {
            if (positions[i] != last[i])
            {
                last[i]          = positions[i];
                writes[n].toggle = i;
                writes[n].on     = positions[i];
                n++;
            }
        }
        return n;
    }

private:
    void Reset(int bank, const bool* positions)
    {
        for (int i = 0; i < kToggleCount; i++)
            last[i] = positions[i];
        activeBank = bank;
        primed     = true;
    }

    bool last[kToggleCount] = {};  // lever positions at the last Scan()
    int  activeBank         = 0;
    bool primed             = false;
};

#endif
