#include "check.h"
#include "updrules.h"

#define SLOT (2u * 1024 * 1024)

void test_updrules(void)
{
    // an update may start: idle, a size within the slot
    CHECK(updrules_begin(900000, SLOT, false, false) == NULL);
    CHECK(updrules_begin(SLOT, SLOT, false, false) == NULL);
    // refused before any byte: nothing to write, over the slot, one already running, a seed being written
    CHECK_HAS(updrules_begin(0, SLOT, false, false), "size");
    CHECK_HAS(updrules_begin(SLOT + 1, SLOT, false, false), "slot");
    CHECK_HAS(updrules_begin(900000, SLOT, true, false), "an update is running");
    CHECK_HAS(updrules_begin(900000, SLOT, false, true), "seed");

    // the end: every byte and the SHA-256 the command named
    const char *a = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *b = "f123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CHECK(updrules_end(10, 10, a, a) == NULL);
    CHECK_HAS(updrules_end(9, 10, a, a), "9 of 10");
    CHECK_HAS(updrules_end(10, 10, b, a), "sha256");

    // the new image proves itself: joined the Wi-Fi and answered a command, within 120 s, or it is rolled back
    CHECK(updrules_verify(false, false, false, 500000) == VERIFY_NOTHING); // not on trial: nothing to do
    CHECK(updrules_verify(true, false, false, 1000) == VERIFY_WAIT);
    CHECK(updrules_verify(true, true, false, 1000) == VERIFY_WAIT);
    CHECK(updrules_verify(true, false, true, 1000) == VERIFY_WAIT);
    CHECK(updrules_verify(true, true, true, 1000) == VERIFY_VALID);
    CHECK(updrules_verify(true, true, true, UPDRULES_TRIAL_MS + 5000) == VERIFY_VALID); // proven is proven
    CHECK(updrules_verify(true, true, false, UPDRULES_TRIAL_MS - 1) == VERIFY_WAIT);
    CHECK(updrules_verify(true, true, false, UPDRULES_TRIAL_MS) == VERIFY_ROLL_BACK);
    CHECK(updrules_verify(true, false, false, UPDRULES_TRIAL_MS) == VERIFY_ROLL_BACK);
    CHECK(UPDRULES_TRIAL_MS == 120000);
}
