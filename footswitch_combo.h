#ifndef FOOTSWITCH_COMBO_H
#define FOOTSWITCH_COMBO_H

// Both-footswitches-held gesture, decoupled from libdaisy's Switch debounce timing.
//
// libdaisy's Switch is an 8-bit shift register clocked once per audio block
// (libdaisy/src/hid/switch.cpp:45-47). Pressed() is state_ == 0xff and goes false 1 ms
// after release; FallingEdge() is state_ == 0x80 and only fires 7 ms later
// (libdaisy/src/hid/switch.h:73-79). A "both switches up" test therefore always clears
// before the release edges arrive, so each combo release edge is consumed by an explicit
// per-switch flag instead. The gesture likewise stays engaged until BOTH switches read
// released, so a release skew of up to ~100 ms cannot drop the secondary bank mid-turn.
struct FootswitchCombo
{
    struct Actions
    {
        bool fs1Release;  // footswitch 1 released outside a combo -> toggle bypass
        bool fs2Release;  // footswitch 2 released outside a combo -> cycle preset
    };

    bool active;    // secondary knob bank engaged
    bool eatFall1;  // consume footswitch 1's pending combo release edge
    bool eatFall2;

    FootswitchCombo() : active(false), eatFall1(false), eatFall2(false) {}

    Actions Update(bool fs1Pressed, bool fs2Pressed, bool fs1Falling, bool fs2Falling)
    {
        if (fs1Pressed && fs2Pressed)
        {
            active   = true;
            eatFall1 = true;
            eatFall2 = true;
        }
        else if (active && !fs1Pressed && !fs2Pressed)
        {
            active = false;
        }

        Actions a = {false, false};

        if (fs1Falling)
        {
            if (eatFall1) eatFall1 = false;
            else          a.fs1Release = true;
        }
        if (fs2Falling)
        {
            if (eatFall2) eatFall2 = false;
            else          a.fs2Release = true;
        }
        return a;
    }
};

#endif
