// The channel's lockout: five failed handshakes in a row and the unit answers nobody for 60 s; each failure after
// that doubles it, up to 15 minutes; one good handshake starts over. Pure, tested on Linux.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LOCKOUT_AFTER 5
#define LOCKOUT_FIRST_MS 60000u
#define LOCKOUT_MAX_MS 900000u

typedef struct {
    uint32_t failed;        // failed handshakes since the start (for status), never reset
    uint32_t in_row;        // failed handshakes since the last good one
    uint32_t deaf_ms;       // the last time deaf, 0 when not deaf since the last good handshake
    uint32_t deaf_until_ms; // when it hears (or heard) again; kept after a good handshake, for status
} lockout_t;

void lockout_fail(lockout_t *l, uint32_t now_ms);
void lockout_ok(lockout_t *l);
bool lockout_deaf(const lockout_t *l, uint32_t now_ms);
uint32_t lockout_until(const lockout_t *l, uint32_t now_ms); // when deaf: until when; else 0
