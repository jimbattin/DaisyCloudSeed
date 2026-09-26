// Host tests for FootswitchGestures (src/footswitch_gestures.h), driven through a
// model of libdaisy's Switch: an 8-bit shift register clocked once per block, with
// Pressed() == 0xff and FallingEdge() == 0x80 (libdaisy/src/hid/switch.h:68-74).
// A press latches on the 8th down block; a release edge fires on the 7th up block.
#include <stdint.h>

#include "check.h"
#include "footswitch_gestures.h"

struct SwitchModel {
    uint8_t s = 0;
    void Clock(bool down) { s = (uint8_t)((s << 1) | (down ? 1 : 0)); }
    bool Pressed() const { return s == 0xff; }
    bool Falling() const { return s == 0x80; }
};

struct Rig {
    SwitchModel        fs1, fs2;
    FootswitchGestures g;
    int bypass = 0, cycle = 0, save = 0, restore = 0;

    void Step(bool d1, bool d2) {
        fs1.Clock(d1);
        fs2.Clock(d2);
        const FootswitchEvents ev =
            g.Update(fs1.Pressed(), fs1.Falling(), fs2.Pressed(), fs2.Falling());
        bypass += ev.toggleBypass;
        cycle += ev.cyclePreset;
        save += ev.savePreset;
        restore += ev.restorePreset;
    }
    void Run(bool d1, bool d2, int n) {
        for (int i = 0; i < n; i++)
            Step(d1, d2);
    }
    bool Quiet() const { return bypass == 0 && cycle == 0 && save == 0 && restore == 0; }
};

int main() {
    {  // FS1 tap: bypass toggles on release.
        Rig r;
        r.Run(1, 0, 50);
        r.Run(0, 0, 20);
        CHECK(r.bypass == 1);
        CHECK(r.cycle == 0);
        CHECK(r.save == 0);
        CHECK(r.restore == 0);
    }
    {  // FS1 held 5 s: save fires once while held (latch on block 8, 5000th latched
       // block is block 5007); the release does not toggle bypass.
        Rig r;
        r.Run(1, 0, 5006);
        CHECK(r.save == 0);
        r.Step(1, 0);
        CHECK(r.save == 1);
        r.Run(1, 0, 1000);
        CHECK(r.save == 1);
        r.Run(0, 0, 20);
        CHECK(r.bypass == 0);
    }
    {  // FS2 tap: secondary bank held through the edge delay, preset cycles on release.
        Rig r;
        r.Run(0, 1, 50);
        CHECK(r.g.PresetHeld());
        r.Step(0, 0);
        CHECK(r.g.PresetHeld());
        r.Run(0, 0, 20);
        CHECK(r.cycle == 1);
        CHECK(!r.g.PresetHeld());
    }
    {  // FS2 hold with a secondary edit: no cycle; the next plain tap cycles again.
        Rig r;
        r.Run(0, 1, 50);
        r.g.MarkEdited();
        r.Run(0, 0, 20);
        CHECK(r.cycle == 0);
        r.Run(0, 1, 50);
        r.Run(0, 0, 20);
        CHECK(r.cycle == 1);
    }
    {  // Staggered chord shorter than 5 s: nothing fires at all.
        Rig r;
        r.Run(1, 0, 100);
        r.Run(1, 1, 100);
        r.Run(0, 1, 100);
        r.Run(0, 0, 20);
        CHECK(r.Quiet());
    }
    {  // FS1 lead-in turning into a 5 s chord: restore, not save; releases inert.
        Rig r;
        r.Run(1, 0, 4000);
        r.Run(1, 1, 5100);
        CHECK(r.restore == 1);
        CHECK(r.save == 0);
        r.Run(0, 0, 20);
        CHECK(r.bypass == 0);
        CHECK(r.cycle == 0);
    }
    {  // A bounce mid-hold never produces a falling edge: one toggle only.
        Rig r;
        r.Run(1, 0, 50);
        r.Run(0, 0, 2);
        r.Run(1, 0, 50);
        r.Run(0, 0, 20);
        CHECK(r.bypass == 1);
    }
    {  // A glitch that never reached Pressed() still produces 0x80: ignored.
        Rig r;
        r.Run(1, 0, 3);
        r.Run(0, 0, 20);
        CHECK(r.Quiet());
    }

    return CheckSummary("footswitch_gestures_test");
}
