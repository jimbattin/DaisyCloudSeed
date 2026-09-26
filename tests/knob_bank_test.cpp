// Host tests for KnobBank (src/knob_bank.h): parking, move threshold, takeover
// glide, apply epsilon, rails, bank change, and reset during a glide.
#include <math.h>

#include "check.h"
#include "knob_bank.h"

int main() {
    KnobBank  kb;
    float     pos[kKnobCount];
    float     cur[kKnobCount];
    KnobWrite w[kKnobCount];
    for (int i = 0; i < kKnobCount; i++) {
        pos[i] = 0.5f;
        cur[i] = 0.2f;
    }

    // First call snapshots and parks: nothing is written.
    CHECK(kb.Scan(0, false, pos, cur, w) == 0);

    // Below kKnobMoveThreshold: still parked.
    pos[2] = 0.505f;
    CHECK(kb.Scan(0, false, pos, cur, w) == 0);

    // Takeover: glide from the target's current value (0.2) to the pot (0.6).
    pos[2] = 0.6f;
    int n = kb.Scan(0, false, pos, cur, w);
    CHECK(n == 1);
    CHECK(w[0].knob == 2);
    CHECK(fabsf(w[0].value - (0.2f + 0.4f / 50.0f)) < 1e-6f);
    float prev = w[0].value;
    for (int step = 2; step <= 50; step++) {  // 49 more glide steps, 50 in total
        n = kb.Scan(0, false, pos, cur, w);
        CHECK(n == 1);
        CHECK(w[0].value > prev);
        prev = w[0].value;
    }
    CHECK(prev == 0.6f);  // the last glide step lands exactly on the pot
    CHECK(kb.Scan(0, false, pos, cur, w) == 0);

    // Live: changes below kKnobApplyEpsilon are dropped, larger ones written as-is.
    pos[2] = 0.6005f;
    CHECK(kb.Scan(0, false, pos, cur, w) == 0);
    pos[2] = 0.602f;
    n = kb.Scan(0, false, pos, cur, w);
    CHECK(n == 1);
    CHECK(w[0].value == 0.602f);

    // Rails: within kKnobRailWindow of a stop writes exactly 0.0 / 1.0.
    pos[2] = 0.998f;
    n = kb.Scan(0, false, pos, cur, w);
    CHECK(n == 1);
    CHECK(w[0].value == 1.0f);
    pos[2] = 0.9985f;
    CHECK(kb.Scan(0, false, pos, cur, w) == 0);
    pos[2] = 0.002f;
    n = kb.Scan(0, false, pos, cur, w);
    CHECK(n == 1);
    CHECK(w[0].value == 0.0f);

    // A knob parked near a stop stays parked: the raw move is below threshold.
    pos[3] = 0.0015f;
    CHECK(kb.Scan(0, true, pos, cur, w) == 0);
    pos[3] = 0.0f;
    CHECK(kb.Scan(0, false, pos, cur, w) == 0);

    // A bank change re-parks every knob; the next turn glides from the target.
    pos[4] = 0.9f;
    CHECK(kb.Scan(1, false, pos, cur, w) == 0);
    CHECK(kb.Scan(1, false, pos, cur, w) == 0);
    pos[4] = 0.8f;
    n = kb.Scan(1, false, pos, cur, w);
    CHECK(n == 1);
    CHECK(w[0].knob == 4);
    CHECK(fabsf(w[0].value - (0.2f + 0.6f / 50.0f)) < 1e-6f);

    // A reset mid-glide cancels it and parks the knob where it is.
    for (int i = 0; i < 10; i++)
        kb.Scan(1, false, pos, cur, w);
    CHECK(kb.Scan(1, true, pos, cur, w) == 0);
    CHECK(kb.Scan(1, false, pos, cur, w) == 0);

    return CheckSummary("knob_bank_test");
}
