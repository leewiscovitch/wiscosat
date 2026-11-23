// What an update may do (updrules.h).
#include <stdio.h>
#include <string.h>
#include "updrules.h"

static _Thread_local char msg[80];

const char *updrules_begin(uint32_t size, size_t slot_size, bool running, bool seed_busy)
{
    if (running) return "an update is running";
    if (seed_busy) return "the seed is being written";
    if (!size) return "size must be at least 1";
    if (size > slot_size) {
        snprintf(msg, sizeof(msg), "the image is larger than the slot (%zu bytes)", slot_size);
        return msg;
    }
    return NULL;
}

const char *updrules_end(uint32_t received, uint32_t size, const char *got, const char *want)
{
    if (received != size) {
        snprintf(msg, sizeof(msg), "update: only %lu of %lu bytes came", (unsigned long)received, (unsigned long)size);
        return msg;
    }
    if (strcmp(got, want)) return "update: the image's sha256 is not the one the command named; old version kept";
    return NULL;
}

verify_t updrules_verify(bool pending, bool joined, bool answered, uint32_t uptime_ms)
{
    if (!pending) return VERIFY_NOTHING;
    if (joined && answered) return VERIFY_VALID;
    return uptime_ms >= UPDRULES_TRIAL_MS ? VERIFY_ROLL_BACK : VERIFY_WAIT;
}
