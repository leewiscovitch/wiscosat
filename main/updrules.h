// What an update may do: the checks before its first byte and after its last, and the new image's trial (it must
// prove itself within 120 s, or the bootloader takes the old one back). Pure, tested on Linux.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDRULES_TRIAL_MS 120000 // a new image has this long from its start to join the Wi-Fi and answer a command

// NULL when an update of size bytes may start into a slot of slot_size; else why not. running: an update is being
// received now; seed_busy: the seed drive is being written
const char *updrules_begin(uint32_t size, size_t slot_size, bool running, bool seed_busy);

// once the bytes stopped: NULL when all size came and their SHA-256 (got) is the one the command named (want)
const char *updrules_end(uint32_t received, uint32_t size, const char *got, const char *want);

typedef enum {
    VERIFY_NOTHING,   // this image is not on trial
    VERIFY_WAIT,      // on trial, not proven yet, time left
    VERIFY_VALID,     // proven: keep it (esp_ota_mark_app_valid_cancel_rollback)
    VERIFY_ROLL_BACK, // the time is up: back to the old image
} verify_t;

// pending: the running image is on trial (ESP_OTA_IMG_PENDING_VERIFY); joined: the Wi-Fi was joined; answered: a
// command came over the channel and was answered; uptime_ms since this start
verify_t updrules_verify(bool pending, bool joined, bool answered, uint32_t uptime_ms);
