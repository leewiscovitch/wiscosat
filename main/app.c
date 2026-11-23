// The wiring: start-up, the event list, the armed key's task, the commands and status.
// The parts: keys.c (names to reports), armed.c (the armed key's schedule), proto.c (the lines), seedrules.c
// (what a seed may hold), lockout.c (the channel's lockout), settings.c (a unit's settings from NVS), usb_kbd.c
// (the USB device, the keyboard), usb_msc.c (the seed drive), wifi.c (the network), channel.c (TLS-PSK, the lines).
// An unenrolled board (no settings in NVS) is a keyboard and a seed drive with no network.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#ifdef SAT_DEV
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#endif
#include "cJSON.h"
#include "armed.h"
#include "keys.h"
#include "proto.h"
#include "sat.h"
#include "settings.h"

#define PIN_POWER_ON 46 // T-Embed: keeps the board's peripherals rail on

// ---------- events ----------
#define NEV 48
static struct {
    uint32_t ms;
    const char *what;
} events[NEV];
static volatile int nev;
static portMUX_TYPE ev_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void ev(const char *what)
{
    taskENTER_CRITICAL(&ev_mux);
    int i = nev;
    if (i == NEV) { // full: drop the oldest
        memmove(&events[0], &events[1], sizeof(events[0]) * (NEV - 1));
        i = NEV - 1;
    }
    events[i].ms = now_ms();
    events[i].what = what;
    nev = i + 1;
    taskEXIT_CRITICAL(&ev_mux);
}

static volatile uint32_t first_cmd_ms;
static settings_t settings;
static bool enrolled;

// ---------- the armed key ----------
// With "persist" it is kept in NVS so it survives the unit's own power cut (a cold start of the target), and
// erased once it has fired.
typedef struct {
    uint8_t report[8];
    char name[KEYS_NAME_MAX + 1];
    uint16_t every_ms, for_ms;
} arm_rec_t;
static SemaphoreHandle_t arm_lock;
static armed_t arm;               // under arm_lock
static arm_rec_t arm_rec;         // the name and schedule as armed, for status and NVS
static bool arm_persist;
static uint32_t armed_at_ms;
static const char *armed_from = "";
static struct {
    int not_listening, failed;
    const char *last_error;
} fire; // what armed_t does not count: why a report was not taken
static TaskHandle_t armer_h;

static void arm_save(bool keep)
{
    nvs_handle_t h;
    if (nvs_open("sat", NVS_READWRITE, &h) != ESP_OK) return;
    if (keep) nvs_set_blob(h, "arm", &arm_rec, sizeof(arm_rec));
    else nvs_erase_key(h, "arm");
    nvs_commit(h);
    nvs_close(h);
}

static void arm_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(arm_rec);
    if (nvs_open("sat", NVS_READONLY, &h) != ESP_OK) return;
    // a record of another size (an older firmware's) is ignored
    if (nvs_get_blob(h, "arm", &arm_rec, &len) == ESP_OK && len == sizeof(arm_rec)) {
        arm_rec.name[KEYS_NAME_MAX] = 0;
        armed_set(&arm, arm_rec.report, arm_rec.every_ms, arm_rec.for_ms);
        arm_persist = true;
        armed_at_ms = now_ms();
        armed_from = "flash";
        ev("armed key loaded from flash");
    }
    nvs_close(h);
}

void app_usb_configured(void)
{
    if (armer_h) xTaskNotifyGive(armer_h);
}

static bool firing(void)
{
    xSemaphoreTake(arm_lock, portMAX_DELAY);
    bool f = arm.firing;
    xSemaphoreGive(arm_lock);
    return f;
}

static void armer_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(arm_lock, portMAX_DELAY);
        armed_configured(&arm, usb_last_configured_ms()); // once: from now on it is no longer armed
        bool go = arm.firing, persist = arm_persist;
        uint8_t report[8];
        memcpy(report, arm.report, 8);
        xSemaphoreGive(arm_lock);
        if (!go) continue;
        memset(&fire, 0, sizeof(fire));
        ev("armed key firing");
        for (;;) {
            xSemaphoreTake(arm_lock, portMAX_DELAY);
            uint32_t wait = armed_wait_ms(&arm, now_ms());
            xSemaphoreGive(arm_lock);
            if (wait) vTaskDelay(pdMS_TO_TICKS(wait));
            xSemaphoreTake(arm_lock, portMAX_DELAY);
            bool due = armed_due(&arm, now_ms()), still = arm.firing;
            xSemaphoreGive(arm_lock);
            if (!still) break;
            if (!due) continue;
            const char *e = kbd_tap(report);
            if (e && (!strcmp(e, "no host") || !strncmp(e, "host asleep", 11))) fire.not_listening++;
            else if (e) fire.failed++, fire.last_error = e;
            xSemaphoreTake(arm_lock, portMAX_DELAY);
            armed_result(&arm, e == NULL);
            xSemaphoreGive(arm_lock);
        }
        if (persist) arm_save(false); // fired: erased from flash
        ev("armed key done");
    }
}

static void do_arm(const cmd_t *c)
{
    xSemaphoreTake(arm_lock, portMAX_DELAY);
    armed_set(&arm, c->key.report, c->key.every_ms, c->key.for_ms);
    memcpy(arm_rec.report, c->key.report, 8);
    strlcpy(arm_rec.name, c->key.name, sizeof(arm_rec.name));
    arm_rec.every_ms = c->key.every_ms;
    arm_rec.for_ms = c->key.for_ms;
    arm_persist = c->key.persist;
    armed_at_ms = now_ms();
    armed_from = "command";
    xSemaphoreGive(arm_lock);
    arm_save(c->key.persist);
    ev("key armed for next configured");
}

static void do_disarm(void)
{
    xSemaphoreTake(arm_lock, portMAX_DELAY);
    armed_clear(&arm);
    xSemaphoreGive(arm_lock);
    arm_save(false);
    ev("disarmed");
}

// ---------- status ----------
static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
    }
}

static void arm_state(cJSON *s)
{
    xSemaphoreTake(arm_lock, portMAX_DELAY);
    armed_t a = arm;
    xSemaphoreGive(arm_lock);
    if (a.armed) {
        cJSON *j = cJSON_AddObjectToObject(s, "armed");
        cJSON_AddStringToObject(j, "name", arm_rec.name);
        cJSON_AddNumberToObject(j, "every_ms", a.every_ms);
        cJSON_AddNumberToObject(j, "for_ms", a.for_ms);
        cJSON_AddBoolToObject(j, "persist", arm_persist);
        cJSON_AddNumberToObject(j, "armed_at_ms", armed_at_ms);
        cJSON_AddStringToObject(j, "from", armed_from);
    } else cJSON_AddNullToObject(s, "armed");
    if (a.firing || a.done) {
        cJSON *f = cJSON_AddObjectToObject(s, "last_fire");
        cJSON_AddStringToObject(f, "name", arm_rec.name);
        cJSON_AddBoolToObject(f, "running", a.firing);
        cJSON_AddNumberToObject(f, "configured_ms", a.configured_ms);
        if (a.taken) {
            cJSON_AddNumberToObject(f, "first_taken_ms", a.first_taken_ms);
            cJSON_AddNumberToObject(f, "first_taken_after_configured_ms", a.first_taken_ms - a.configured_ms);
        } else cJSON_AddNullToObject(f, "first_taken_ms");
        cJSON_AddNumberToObject(f, "end_ms", a.end_ms);
        cJSON_AddNumberToObject(f, "tried", a.tried);
        cJSON_AddNumberToObject(f, "taken", a.taken);
        cJSON_AddNumberToObject(f, "not_listening", fire.not_listening);
        cJSON_AddNumberToObject(f, "failed", fire.failed);
        if (fire.last_error) cJSON_AddStringToObject(f, "last_error", fire.last_error);
    } else cJSON_AddNullToObject(s, "last_fire");
}

static cJSON *state_json(void)
{
    cJSON *s = cJSON_CreateObject();
    kbd_state(s);
    cJSON_AddStringToObject(s, "name", settings.name);
    proto_add_build(s, esp_app_get_description()->version);
    const esp_partition_t *slot = esp_ota_get_running_partition();
    cJSON_AddStringToObject(s, "slot", slot ? slot->label : "");
    cJSON_AddNumberToObject(s, "uptime_ms", now_ms());
    cJSON_AddNumberToObject(s, "first_command_ms", first_cmd_ms);
    cJSON_AddStringToObject(s, "reset_reason", reset_reason());
    cJSON_AddStringToObject(s, "ip", settings.ip_str);
    wifi_state(s);
    channel_state(s);
    arm_state(s);
    seed_state(s);
    update_state(s);
    cJSON *a = cJSON_AddArrayToObject(s, "events");
    int n = nev;
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateArray();
        cJSON_AddItemToArray(e, cJSON_CreateNumber(events[i].ms));
        cJSON_AddItemToArray(e, cJSON_CreateString(events[i].what));
        cJSON_AddItemToArray(a, e);
    }
    return s;
}

// ---------- commands ----------
#ifdef SAT_DEV
// download (a dev build only): the ROM's download mode, so esptool can flash with no buttons. Run 300 ms after the
// command, so its answer has left first (trial 2's bootloader command, the same two lines)
static void download_now(void *arg)
{
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static const char *do_download(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = download_now, .name = "download"};
    if (!t && esp_timer_create(&args, &t) != ESP_OK) return "download: no timer";
    if (esp_timer_start_once(t, 300 * 1000) != ESP_OK) return "download: already on its way";
    ev("download mode in 300 ms");
    return NULL;
}
#endif

static const char *do_type(const char *text)
{
    for (; *text; text++) {
        uint8_t r[8];
        keys_type(*text, r); // proto checked every character
        const char *e = kbd_tap(r);
        if (e) return e;
    }
    return NULL;
}

// restart 1 s after the answer (restart, and a finished update): the answer leaves first
static void restart_now(void *arg) { esp_restart(); }

static const char *restart_soon(void)
{
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = restart_now, .name = "restart"};
    if (!t && esp_timer_create(&args, &t) != ESP_OK) return "restart: no timer";
    if (esp_timer_start_once(t, 1000 * 1000) != ESP_OK) return "restart: already on its way";
    ev("restart in 1 s");
    return NULL;
}

// the raw bytes' end (raw_t): the final answer line
static char *seed_done(const char *broke)
{
    const char *err = seed_end(broke);
    return proto_answer(err, err ? NULL : state_json());
}

static const char *update_chunk_(const uint8_t *p, size_t n) { return update_chunk(p, n); }

static char *update_done(const char *broke)
{
    const char *err = update_end(broke);
    if (!err) err = restart_soon();
    return proto_answer(err, err ? NULL : state_json());
}

#define READY "{\"ok\":true,\"ready\":true}"

// seed and update: checked, then started; the answer says ready and *raw takes the bytes. NULL and *ready false:
// done already (a wipe, a seed of only empty files). Else the error
static const char *start_raw(const cmd_t *c, raw_t *raw, bool *ready, char *msg, size_t size)
{
    const char *e;
    int bad = 0;
    *ready = false;
    if (c->kind == CMD_UPDATE) {
        if ((e = update_begin(c->update.size, c->update.sha256))) return e;
        *raw = (raw_t){c->update.size, update_chunk_, update_done};
        *ready = true;
        return NULL;
    }
    if (update_running()) return "an update is running";
    if (!c->seed.n) return seed_wipe(c->seed.hold_ms);
    if (seedrules_check(c->seed.files, c->seed.n, seed_room(c->seed.n), &e, &bad)) {
        snprintf(msg, size, "seed refused, file \"%s\": %s", c->seed.files[bad].name, e);
        return msg;
    }
    if ((e = seed_begin(c->seed.files, c->seed.n, c->seed.hold_ms))) return e;
    if (!c->seed.total) return seed_end(NULL); // only empty files: nothing follows the line
    *raw = (raw_t){c->seed.total, seed_chunk, seed_done};
    *ready = true;
    return NULL;
}

static const char *run(const cmd_t *c, char *msg, size_t size)
{
    switch (c->kind) {
    case CMD_STATUS: return NULL;
    case CMD_KEY:
        if (c->key.when == WHEN_NOW) return kbd_tap(c->key.report);
        do_arm(c);
        return NULL;
    case CMD_DISARM: do_disarm(); return NULL;
    case CMD_PRESS: return kbd_hold(c->key.report, true);
    case CMD_RELEASE: return kbd_hold(c->key.name ? c->key.report : NULL, false);
    case CMD_TYPE: return do_type(c->type.text);
    case CMD_SEED:
    case CMD_UPDATE: return "start_raw's"; // not reached: app_handle_line hands these to start_raw
    case CMD_RESTART: return restart_soon();
    case CMD_IDENTIFY: return led_identify();
#ifdef SAT_DEV
    case CMD_DOWNLOAD: return do_download();
#endif
    }
    return "unknown command";
}

char *app_handle_line(const char *line, size_t len, raw_t *raw)
{
    cmd_t c;
    const char *err = NULL;
    char msg[160];
    bool ready = false;
    if (!first_cmd_ms) first_cmd_ms = now_ms();
    if (proto_parse(line, len, &c, &err)) return proto_answer(err, NULL);
    const host_t h = {.configured = usb_configured(), .suspended = usb_suspended(), .firing = firing()};
    err = proto_check(&c, &h);
    if (!err && (c.kind == CMD_SEED || c.kind == CMD_UPDATE)) err = start_raw(&c, raw, &ready, msg, sizeof(msg));
    else if (!err) err = run(&c, msg, sizeof(msg));
    proto_free(&c);
    if (ready) return strdup(READY);
    return proto_answer(err, err ? NULL : state_json());
}

void app_answered(void) { update_answered(); }

void app_main(void)
{
    gpio_set_direction(PIN_POWER_ON, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_POWER_ON, 1);
    arm_lock = xSemaphoreCreateMutex();

    // NVS first: an armed key kept in flash must be armed before the host can configure us
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    arm_load();
    xTaskCreate(armer_task, "armer", 4096, NULL, 7, &armer_h);

    // the seed volume must be ready before the host can ask for it
    seed_init();
    // USB next: being a keyboard must not wait for Wi-Fi
    usb_start();

    // the network last, and only for an enrolled unit; nothing below waits for it
    enrolled = settings_load(&settings);
#if CONFIG_SAT_BROKEN
    // the rollback check's image (./build --dev --broken): a network no Wi-Fi has, so it never proves itself
    strlcpy(settings.wifi_ssid, "no-such-network-rollback-test", sizeof(settings.wifi_ssid));
    ev("broken build: wrong Wi-Fi name on purpose");
#endif
    const char *version = esp_app_get_description()->version;
    screen_show(enrolled ? settings.name : "unenrolled", enrolled ? settings.ip_str : "", version);
    if (!enrolled) {
        ev("unenrolled: no network");
        update_trial_start(); // it cannot prove itself: the old image comes back in 120 s
        return;
    }
    wifi_start(&settings);
    channel_start(&settings);
    update_trial_start(); // after an update: this image keeps itself only once it has joined and answered
}
