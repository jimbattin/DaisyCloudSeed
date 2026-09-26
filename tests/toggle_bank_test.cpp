// Host tests for ToggleBank (src/toggle_bank.h): parking on first scan, bank
// change and forced reset; immediate writes once a lever is flipped.
#include "check.h"
#include "toggle_bank.h"

int main() {
    ToggleBank  tb;
    ToggleWrite w[kToggleCount];
    bool        lever[kToggleCount] = {true, false, true, false};

    // First call snapshots and parks; an unchanged repeat writes nothing.
    CHECK(tb.Scan(0, false, lever, w) == 0);
    CHECK(tb.Scan(0, false, lever, w) == 0);

    // A flipped lever writes its position immediately, once.
    lever[1] = true;
    int n = tb.Scan(0, false, lever, w);
    CHECK(n == 1);
    CHECK(w[0].toggle == 1);
    CHECK(w[0].on == true);
    CHECK(tb.Scan(0, false, lever, w) == 0);

    // A bank change re-parks, even when a lever moves in the same call.
    lever[0] = false;
    CHECK(tb.Scan(1, false, lever, w) == 0);
    lever[0] = true;
    n = tb.Scan(1, false, lever, w);
    CHECK(n == 1);
    CHECK(w[0].toggle == 0);
    CHECK(w[0].on == true);

    // forceReset (preset load) re-parks the same way.
    lever[2] = false;
    CHECK(tb.Scan(1, true, lever, w) == 0);
    CHECK(tb.Scan(1, false, lever, w) == 0);

    return CheckSummary("toggle_bank_test");
}
