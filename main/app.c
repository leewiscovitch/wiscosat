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
#include "menu.h"
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
// erased once it has fired. With "sticky" (the knob's keys; the command's option) it is not erased: when its firing
// has ended it is armed again, for every later start, until a press of the knob or disarm takes it away.
typedef struct {
    uint8_t report[8];
    char name[KEYS_NAME_MAX + 1];
    uint16_t every_ms, for_ms, after_taken_ms;
    uint8_t sticky;
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
static void ui_refresh(void);

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
        armed_end_after_taken(&arm, arm_rec.after_taken_ms);
        arm_persist = true;
        armed_at_ms = now_ms();
        armed_from = "flash";
        ev("armed key loaded from flash");
    } else memset(&arm_rec, 0, sizeof(arm_rec));
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
        ui_refresh();
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
        xSemaphoreTake(arm_lock, portMAX_DELAY);
        bool again = arm_rec.sticky; // a disarm on the way took it
        if (again) armed_rearm(&arm);
        xSemaphoreGive(arm_lock);
        if (persist && !again) arm_save(false); // fired: erased from flash
        ev(again ? "armed key done, armed again (sticky)" : "armed key done");
        ui_refresh();
    }
}

static void arm_key(const uint8_t report[8], const char *name, uint32_t every_ms, uint32_t for_ms,
                    uint32_t after_taken_ms, bool persist, bool sticky, const char *from)
{
    xSemaphoreTake(arm_lock, portMAX_DELAY);
    armed_set(&arm, report, every_ms, for_ms);
    armed_end_after_taken(&arm, after_taken_ms);
    arm_rec.after_taken_ms = after_taken_ms;
    memcpy(arm_rec.report, report, 8);
    strlcpy(arm_rec.name, name, sizeof(arm_rec.name));
    arm_rec.every_ms = every_ms;
    arm_rec.for_ms = for_ms;
    arm_rec.sticky = sticky;
    arm_persist = persist || sticky;
    armed_at_ms = now_ms();
    armed_from = from;
    xSemaphoreGive(arm_lock);
    arm_save(persist || sticky);
    ev("key armed for next configured");
    ui_refresh();
}

// true when there was something to take away: an armed key, or one being sent (stopped at once)
static bool do_disarm(void)
{
    xSemaphoreTake(arm_lock, portMAX_DELAY);
    bool was = arm.armed || arm.firing;
    armed_clear(&arm);
    armed_stop(&arm, now_ms());
    arm_rec.sticky = 0;
    xSemaphoreGive(arm_lock);
    arm_save(false);
    if (was) ev("disarmed");
    ui_refresh();
    return was;
}

// ---------- the screen and the knob ----------
static const char *ui_version = "";
static volatile bool ui_ready; // app_main has drawn the first picture (it starts the screen's task)

// the idle picture: who the unit is; with a key armed or being sent, that key first
static void ui_refresh(void)
{
    if (!ui_ready || knob_browsing()) return; // the menu has the screen; it calls app_ui_refresh when it lets go
    char big[40], id[64];
    xSemaphoreTake(arm_lock, portMAX_DELAY);
    bool armed = arm.armed, sending = arm.firing;
    snprintf(big, sizeof(big), "%s %.20s", sending ? "Sending" : "Armed:", arm_rec.name);
    xSemaphoreGive(arm_lock);
    const char *name = enrolled ? settings.name : "unenrolled";
    if (!armed && !sending) return screen_show(name, enrolled ? settings.ip_str : "", ui_version);
    snprintf(id, sizeof(id), "%s  %s", name, enrolled ? settings.ip_str : "");
    screen_show(big, knob_present() ? (sending ? "press to stop" : "press to clear") : "", id);
}

void app_ui_refresh(void) { ui_refresh(); }

// the knob's key: sent every 100 ms at every start from now on, until taken away. It stops 10 s after the host took
// the first one (a firmware has seen its key by then); with no host taking any, after 60 s
#define KNOB_AFTER_TAKEN_MS 10000
const char *app_ui_arm(const char *name)
{
    uint8_t report[8];
    const char *err = NULL;
    if (keys_parse(name, report, &err)) return err;
    if (firing()) do_disarm();
    arm_key(report, name, ARMED_EVERY_MS_DEFAULT, ARMED_FOR_MS_MAX, KNOB_AFTER_TAKEN_MS, true, true, "knob");
    return NULL;
}

bool app_ui_disarm(void) { return do_disarm(); }

// one key or combination now, from the knob
const char *app_ui_tap(const char *name)
{
    uint8_t report[8];
    const char *err = NULL;
    if (keys_parse(name, report, &err)) return err;
    if (!usb_configured()) return "no host";
    ev("knob: a key sent");
    return kbd_tap(report);
}

// ---------- the knob's menu ----------
// The text the command menu gave is kept in NVS; with none, or one this firmware cannot read, the built-in list.
static SemaphoreHandle_t menu_lock;
static menu_t *menu;      // under menu_lock
static char *menu_text;   // the stored text; NULL: the built-in list
static const char *menu_fault; // why a stored text was not used

static void menu_init(void)
{
    static char err[MENU_ERR_MAX]; // not on the main task's stack
    nvs_handle_t h;
    size_t len = 0;
    menu_lock = xSemaphoreCreateMutex();
    menu = calloc(1, sizeof(*menu));
    if (!menu) return ev("menu: no memory");
    if (nvs_open("sat", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "menu", NULL, &len) == ESP_OK && len && len <= MENU_TEXT_MAX + 1 &&
            (menu_text = malloc(len)) && nvs_get_str(h, "menu", menu_text, &len) != ESP_OK) {
            free(menu_text);
            menu_text = NULL;
        }
        nvs_close(h);
    }
    if (menu_text && menu_parse(menu_text, menu, err, sizeof(err))) {
        ev("menu: the stored text is not readable, the built-in list");
        menu_fault = "the stored text is not readable by this firmware";
        free(menu_text);
        menu_text = NULL;
    }
    if (!menu_text) menu_parse(menu_builtin, menu, err, sizeof(err));
}

// a new text ("": back to the built-in list): checked whole first, so a refused one changes nothing
static const char *menu_set(const char *text, char *msg, size_t size)
{
    bool builtin = !text[strspn(text, " \t\r\n")];
    menu_t *m = calloc(1, sizeof(*m));
    char *copy = builtin ? NULL : strdup(text);
    if (!m || (!builtin && !copy)) {
        free(m), free(copy);
        return "menu: no memory";
    }
    if (menu_parse(builtin ? menu_builtin : text, m, msg, size)) {
        free(m), free(copy);
        return msg;
    }
    nvs_handle_t h;
    if (nvs_open("sat", NVS_READWRITE, &h) != ESP_OK) {
        free(m), free(copy);
        return "menu: the flash did not open";
    }
    esp_err_t e = builtin ? nvs_erase_key(h, "menu") : nvs_set_str(h, "menu", text);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK; // nothing was stored
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        free(m), free(copy);
        return "menu: not stored (the flash is full?)";
    }
    xSemaphoreTake(menu_lock, portMAX_DELAY);
    menu_t *old = menu;
    char *old_text = menu_text;
    menu = m;
    menu_text = copy;
    menu_fault = NULL;
    xSemaphoreGive(menu_lock);
    free(old), free(old_text);
    ev(builtin ? "menu: the built-in list" : "menu: a new text");
    return NULL;
}

int app_menu_view(int at, menu_view_t *out)
{
    xSemaphoreTake(menu_lock, portMAX_DELAY);
    int n = menu ? menu_view(menu, at, out) : 0;
    xSemaphoreGive(menu_lock);
    return n;
}

// into status: where the list is from and how long; with_text (the command menu's answer): the text itself
static void menu_state(cJSON *s, bool with_text)
{
    cJSON *j = cJSON_AddObjectToObject(s, "menu");
    xSemaphoreTake(menu_lock, portMAX_DELAY);
    cJSON_AddStringToObject(j, "source", menu_text ? "text" : "built-in");
    cJSON_AddNumberToObject(j, "entries", menu ? menu->n : 0);
    cJSON_AddNumberToObject(j, "menus", menu ? menu->nsections : 0);
    if (menu_fault) cJSON_AddStringToObject(j, "fault", menu_fault);
    if (with_text) cJSON_AddStringToObject(j, "text", menu_text ? menu_text : menu_builtin);
    xSemaphoreGive(menu_lock);
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
        if (a.after_taken_ms) cJSON_AddNumberToObject(j, "after_taken_ms", a.after_taken_ms);
        cJSON_AddBoolToObject(j, "persist", arm_persist);
        cJSON_AddBoolToObject(j, "sticky", arm_rec.sticky);
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

static cJSON *state_json_(bool menu_too)
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
    if (knob_present()) menu_state(s, menu_too);
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

static cJSON *state_json(void) { return state_json_(false); }

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
        arm_key(c->key.report, c->key.name, c->key.every_ms, c->key.for_ms, 0, c->key.persist, c->key.sticky,
                "command");
        return NULL;
    case CMD_DISARM: do_disarm(); return NULL;
    case CMD_PRESS: return kbd_hold(c->key.report, true);
    case CMD_RELEASE: return kbd_hold(c->key.name ? c->key.report : NULL, false);
    case CMD_TYPE: return do_type(c->type.text);
    case CMD_SEED:
    case CMD_UPDATE: return "start_raw's"; // not reached: app_handle_line hands these to start_raw
    case CMD_RESTART: return restart_soon();
    case CMD_IDENTIFY: return led_identify();
    case CMD_WAKE: return usb_wake();
    case CMD_MENU:
        if (!knob_present()) return "no knob on this board";
        return c->menu.text ? menu_set(c->menu.text, msg, size) : NULL;
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
    char msg[MENU_ERR_MAX];
    bool ready = false;
    if (!first_cmd_ms) first_cmd_ms = now_ms();
    if (proto_parse(line, len, &c, &err)) return proto_answer(err, NULL);
    const host_t h = {.configured = usb_configured(), .suspended = usb_suspended(), .firing = firing()};
    err = proto_check(&c, &h);
    if (!err && (c.kind == CMD_SEED || c.kind == CMD_UPDATE)) err = start_raw(&c, raw, &ready, msg, sizeof(msg));
    else if (!err) err = run(&c, msg, sizeof(msg));
    bool menu_too = c.kind == CMD_MENU;
    proto_free(&c);
    if (ready) return strdup(READY);
    return proto_answer(err, err ? NULL : state_json_(menu_too));
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
    menu_init();
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
    ui_version = esp_app_get_description()->version;
    ui_ready = true;
    ui_refresh();
    knob_start(); // an unenrolled board too: the knob needs no network
    if (!enrolled) {
        ev("unenrolled: no network");
        update_trial_start(); // it cannot prove itself: the old image comes back in 120 s
        return;
    }
    wifi_start(&settings);
    channel_start(&settings);
    update_trial_start(); // after an update: this image keeps itself only once it has joined and answered
}
