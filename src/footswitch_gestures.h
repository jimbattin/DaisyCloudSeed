#ifndef FOOTSWITCH_GESTURES_H
#define FOOTSWITCH_GESTURES_H

// Both footswitches as one gesture recognizer:
//   FS1 tap            toggle bypass (on release)
//   FS1 hold 5 s       save the current sound into the current preset (fires while held)
//   FS2 tap            next preset (on release)
//   FS2 hold           secondary knob bank; the release cycles only if no secondary knob wrote
//   FS1 + FS2 hold 5 s restore the current preset to factory (fires while held)
//
// libdaisy's Switch is an 8-bit shift register clocked once per audio block
// (libdaisy/src/hid/switch.cpp:44-47): Pressed() is state_ == 0xff, FallingEdge() is
// state_ == 0x80 (libdaisy/src/hid/switch.h:70-79). Each hold is engaged on Pressed()
// and released only on FallingEdge(), so a contact bounce during a hold (which never
// produces 0x80) cannot drop it, and the 6 ms between Pressed() clearing and
// FallingEdge() firing still counts as held (FS2: stays in the secondary bank).
//
// Once both switches have been held at the same time they form a chord: neither
// release toggles bypass or cycles the preset until both are up again, however far
// apart the two presses and releases land. The callback calls MarkEdited() when a
// secondary-bank knob wrote during the FS2 hold. Host-portable: no libdaisy.

// Update() calls (1 ms audio blocks) a long hold must last: 5 s.
constexpr int kLongHoldBlocks = 5000;

struct FootswitchEvents {
    bool toggleBypass  = false;  // FS1 released: not a long hold, not part of a chord
    bool cyclePreset   = false;  // FS2 released: not part of a chord, no secondary knob wrote
    bool savePreset    = false;  // FS1 held alone for kLongHoldBlocks (fires while held)
    bool restorePreset = false;  // FS1 + FS2 held together for kLongHoldBlocks (fires while held)
};

class FootswitchGestures
{
public:
    // Once per callback with each footswitch's Pressed()/FallingEdge().
    FootswitchEvents Update(bool bypassPressed, bool bypassFalling,
                            bool presetPressed, bool presetFalling)
    {
        FootswitchEvents ev;
        if (bypassPressed && !bypassHeld)
        {
            bypassHeld       = true;
            bypassHoldBlocks = 0;
        }
        if (presetPressed)
            presetHeld = true;

        if (bypassHeld && presetHeld)
        {
            // A chord: once both are down together, neither release does anything
            // until both are up, however far apart the presses/releases land.
            chord = true;
            if (chordBlocks < kLongHoldBlocks && ++chordBlocks == kLongHoldBlocks)
                ev.restorePreset = true;
        }
        else
        {
            chordBlocks = 0;
            if (bypassHeld && !chord && !bypassLongFired
                && ++bypassHoldBlocks == kLongHoldBlocks)
            {
                ev.savePreset   = true;
                bypassLongFired = true;
            }
        }

        if (bypassFalling && bypassHeld)
        {
            ev.toggleBypass = !chord && !bypassLongFired;
            bypassHeld      = false;
            bypassLongFired = false;
        }
        if (presetFalling && presetHeld)
        {
            ev.cyclePreset = !chord && !presetEdited;
            presetHeld     = false;
            presetEdited   = false;
        }
        if (!bypassHeld && !presetHeld)
            chord = false;
        return ev;
    }

    bool PresetHeld() const { return presetHeld; }  // secondary knob bank
    void MarkEdited() { presetEdited = true; }      // a secondary knob wrote during the FS2 hold

private:
    bool bypassHeld       = false;
    bool presetHeld       = false;
    bool presetEdited     = false;
    bool bypassLongFired  = false;  // save fired during this FS1 hold: its release is inert
    bool chord            = false;  // both were held together since both were last up
    int  bypassHoldBlocks = 0;
    int  chordBlocks      = 0;      // consecutive blocks with both held
};

#endif
