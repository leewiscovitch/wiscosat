#include "check.h"
#include "lockout.h"

void test_lockout(void)
{
    lockout_t l = {0};
    uint32_t t = 1000;

    // four failures in a row: still listening
    for (int i = 0; i < 4; i++) lockout_fail(&l, t);
    CHECK(!lockout_deaf(&l, t));
    CHECK(l.failed == 4);

    // the fifth: deaf for 60 s
    lockout_fail(&l, t);
    CHECK(lockout_deaf(&l, t));
    CHECK(lockout_deaf(&l, t + 59999));
    CHECK(!lockout_deaf(&l, t + 60000));
    CHECK(lockout_until(&l, t) == t + 60000);
    CHECK(lockout_until(&l, t + 60000) == 0);

    // after it, one more failure: deaf again, 120 s
    t += 60000;
    lockout_fail(&l, t);
    CHECK(lockout_deaf(&l, t + 119999));
    CHECK(!lockout_deaf(&l, t + 120000));

    // doubling up to 15 minutes, no further
    uint32_t expect[] = {240000, 480000, 900000, 900000};
    for (int i = 0; i < 4; i++) {
        t += 1000000;
        lockout_fail(&l, t);
        CHECK(lockout_deaf(&l, t + expect[i] - 1));
        CHECK(!lockout_deaf(&l, t + expect[i]));
    }
    CHECK(l.failed == 10);

    // one good handshake resets it: four failures are fine again; the count of failures is kept for status
    t += 1000000;
    uint32_t was = l.deaf_until_ms;
    lockout_ok(&l);
    CHECK(l.deaf_until_ms == was && !lockout_deaf(&l, was - 1));
    for (int i = 0; i < 4; i++) lockout_fail(&l, t);
    CHECK(!lockout_deaf(&l, t));
    lockout_fail(&l, t);
    CHECK(lockout_deaf(&l, t + 59999) && !lockout_deaf(&l, t + 60000));
    CHECK(l.failed == 15);

    // the millisecond clock wraps (49 days): still 60 s
    lockout_t w = {0};
    uint32_t near = 0xFFFFFFFFu - 1000;
    for (int i = 0; i < 5; i++) lockout_fail(&w, near);
    CHECK(lockout_deaf(&w, near + 30000));
    CHECK(!lockout_deaf(&w, near + 60000));
}
