#ifndef PRESET_FOOTSWITCH_H
#define PRESET_FOOTSWITCH_H

// FOOTSWITCH_2: tap = next preset, hold = secondary knob bank.
//
// libdaisy's Switch is an 8-bit shift register clocked once per audio block
// (libdaisy/src/hid/switch.cpp:44-47): Pressed() is state_ == 0xff, FallingEdge() is
// state_ == 0x80 (libdaisy/src/hid/switch.h:70-79). The hold is engaged on Pressed()
// and released only on FallingEdge(), so a contact bounce during the hold (which never
// produces 0x80) cannot drop the secondary bank or clear the edit flag, and the 6 ms
// between Pressed() clearing and FallingEdge() firing stays in the secondary bank.
struct PresetFootswitch
{
    bool held;    // secondary knob bank engaged
    bool edited;  // a secondary-bank knob wrote a value during the current hold

    PresetFootswitch() : held(false), edited(false) {}

    // Call once per callback with FOOTSWITCH_2's Pressed()/FallingEdge(). Returns true
    // when the release should cycle the preset.
    bool Update(bool pressed, bool falling)
    {
        if (pressed)
            held = true;
        if (falling && held)
        {
            const bool cycle = !edited;
            held   = false;
            edited = false;
            return cycle;
        }
        return false;
    }
};

#endif
