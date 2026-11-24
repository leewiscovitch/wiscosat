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

    // stopped on the way (a press, disarm): it ends at once, its counters stay
    armed_set(&a, F9, 100, 60000);
    armed_configured(&a, 5000);
    CHECK(run(&a, 5000, 10, 450) == 5);
    armed_stop(&a, 5460);
    CHECK(!a.firing && a.done && a.end_ms == 5460 && a.taken == 5);
    CHECK(!armed_due(&a, 5500));

    // a key that stays: armed again after its firing, with the last firing's counters kept until the next
    armed_rearm(&a);
    CHECK(a.armed && !a.firing && a.taken == 5);
    armed_configured(&a, 9000);
    CHECK(a.firing && a.taken == 0);
    armed_rearm(&a); // not while it fires
    CHECK(!a.armed);

    // ending 1000 ms after the first report the host took, not after the whole 60 s: the host takes none for
    // 500 ms, then all
    armed_set(&a, F9, 100, 60000);
    armed_end_after_taken(&a, 1000);
    armed_configured(&a, 20000);
    int due = 0;
    for (uint32_t t = 0; t <= 5000; t += 10)
        if (armed_due(&a, 20000 + t)) {
            due++;
            armed_result(&a, t >= 500);
        }
    CHECK(a.first_taken_ms == 20500 && !a.firing && a.end_ms == 21500);
    CHECK(due == 15 && a.taken == 10);
    // a host that takes none: the whole of for_ms
    armed_set(&a, F9, 100, 3000);
    armed_end_after_taken(&a, 1000);
    armed_configured(&a, 30000);
    due = 0;
    for (uint32_t t = 0; t <= 5000; t += 10)
        if (armed_due(&a, 30000 + t)) {
            due++;
            armed_result(&a, false);
        }
    CHECK(due == 30 && a.taken == 0 && a.end_ms == 33000);
}
