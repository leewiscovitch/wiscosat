// The update: a new image into the other app slot (esp_ota with sequential writes, so nothing waits on erasing
// the slot first), its SHA-256 over what arrived checked against the command's, then that slot set to boot. The
// command's answer goes out, and app.c restarts 1 s later. updrules.c says what may start and what counts as done.
// A new image is on trial (the bootloader's rollback, CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): it keeps itself only
// once it has joined the Wi-Fi and answered a command; 120 s after its start without both, it asks the bootloader
// for the old image back.
#include <stdio.h>
#include <string.h>
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "sat.h"
#include "updrules.h"

static esp_ota_handle_t ota;
static const esp_partition_t *to;
static mbedtls_sha256_context sha;
static uint32_t size, received;
static char want[65];
static volatile bool running;
static const char *trial = "none"; // for status: none, on trial, proven, rolling back
static bool answered_once;

const char *update_begin(uint32_t sz, const char *sha256)
{
    to = esp_ota_get_next_update_partition(NULL);
    if (!to) return "update: no other app slot";
    const char *err = updrules_begin(sz, to->size, running, seed_busy());
    if (err) return err;
    if (esp_ota_begin(to, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) return "update: could not open the other slot";
    size = sz;
    received = 0;
    strlcpy(want, sha256, sizeof(want));
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    running = true;
    ev("update: receiving");
    return NULL;
}

const char *update_chunk(const uint8_t *p, size_t n)
{
    if (esp_ota_write(ota, p, n) != ESP_OK) return "update: could not write the other slot";
    mbedtls_sha256_update(&sha, p, n);
    received += n;
    return NULL;
}

const char *update_end(const char *broke)
{
    static char msg[96];
    uint8_t d[32];
    char got[65];
    mbedtls_sha256_finish(&sha, d);
    mbedtls_sha256_free(&sha);
    for (int i = 0; i < 32; i++) sprintf(got + 2 * i, "%02x", d[i]);
    const char *err = NULL;
    if (broke) {
        snprintf(msg, sizeof(msg), "update %s; old version kept", broke);
        err = msg;
    } else err = updrules_end(received, size, got, want);
    if (err) esp_ota_abort(ota);
    else if (esp_ota_end(ota) != ESP_OK) err = "update: the image does not check out (esp_ota_end); old version kept";
    else if (esp_ota_set_boot_partition(to) != ESP_OK) err = "update: could not set the new slot to boot";
    running = false;
    ev(err ? "update: failed, old version kept" : "update: done, restart into the new slot");
    return err;
}

static void check_trial(void *arg)
{
    const esp_partition_t *me = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    bool pending = esp_ota_get_state_partition(me, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    switch (updrules_verify(pending, wifi_ever_joined(), answered_once, now_ms())) {
    case VERIFY_NOTHING: break;
    case VERIFY_WAIT: return;
    case VERIFY_VALID:
        esp_ota_mark_app_valid_cancel_rollback();
        trial = "proven";
        ev("update: new image proven, kept");
        break;
    case VERIFY_ROLL_BACK:
        trial = "rolling back";
        ev("update: new image not proven in 120 s, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot(); // returns only if there is nothing to roll back to
        trial = "no old image to roll back to";
        break;
    }
    esp_timer_stop(*(esp_timer_handle_t *)arg);
}

void update_answered(void) { answered_once = true; }

bool update_running(void) { return running; }

void update_trial_start(void)
{
    static esp_timer_handle_t t;
    const esp_partition_t *me = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(me, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    trial = "on trial";
    ev("update: new image on trial");
    const esp_timer_create_args_t a = {.callback = check_trial, .arg = &t, .name = "trial"};
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_periodic(t, 1000 * 1000);
}

void update_state(cJSON *s)
{
    cJSON *u = cJSON_AddObjectToObject(s, "update");
    cJSON_AddBoolToObject(u, "running", running);
    cJSON_AddStringToObject(u, "trial", trial);
    const esp_partition_t *next = esp_ota_get_boot_partition();
    cJSON_AddStringToObject(u, "boots", next ? next->label : "");
}
