#include "check.h"
#include "armed.h"

static const uint8_t F9[8] = {0, 0, 0x42};

// call armed_due every step ms from start for span ms, every report taken; how many were due
static int run(armed_t *a, uint32_t start, uint32_t step, uint32_t span)
{
    int due = 0;
    for (uint32_t t = 0; t <= span; t += step)
        if (armed_due(a, start + t)) {
            due++;
            armed_result(a, true);
        }
    return due;
}

void test_armed(void)
{
    armed_t a;

    // nothing happens before the host configures
    armed_set(&a, F9, 100, 3000);
    CHECK(a.armed && !a.firing);
    CHECK(!armed_due(&a, 0));
    CHECK(!memcmp(a.report, F9, 8));

    // 100 ms apart for 3000 ms: 30 reports, then never again
    armed_configured(&a, 1000);
    CHECK(!a.armed && a.firing);
    CHECK(run(&a, 1000, 1, 40000) == 30);
    CHECK(!a.firing && a.done);
    CHECK(a.tried == 30 && a.taken == 30);
    CHECK(a.first_taken_ms == 1000);
    CHECK(!armed_due(&a, 100000));
    // a second configured does not fire it again: it was used
    armed_configured(&a, 200000);
    CHECK(!a.firing && run(&a, 200000, 1, 5000) == 0);

    // across the wrap of the millisecond counter (49 days)
    armed_set(&a, F9, 100, 3000);
    armed_configured(&a, 0xFFFFFF00u);
    CHECK(run(&a, 0xFFFFFF00u, 1, 40000) == 30);

    // fallen behind: slots are skipped, never bunched up
    armed_set(&a, F9, 100, 3000);
    armed_configured(&a, 0);
    CHECK(run(&a, 0, 250, 10000) == 12);

    // for_ms 0: once
    armed_set(&a, F9, 100, 0);
    armed_configured(&a, 0);
    CHECK(run(&a, 0, 1, 1000) == 1);

    // not taken: tried counts, taken does not, first_taken_ms stays 0 until one is
    armed_set(&a, F9, 100, 300);
    armed_configured(&a, 0);
    CHECK(armed_due(&a, 0));
    armed_result(&a, false);
    CHECK(a.tried == 1 && a.taken == 0 && a.first_taken_ms == 0);
    CHECK(!armed_due(&a, 50));
    CHECK(armed_wait_ms(&a, 50) == 50);
    CHECK(armed_due(&a, 100));
    armed_result(&a, true);
    CHECK(a.tried == 2 && a.taken == 1 && a.first_taken_ms == 100);

    // disarmed before the host came: nothing fires
    armed_set(&a, F9, 100, 3000);
    armed_clear(&a);
    armed_configured(&a, 0);
    CHECK(!a.firing && run(&a, 0, 1, 5000) == 0);
}
