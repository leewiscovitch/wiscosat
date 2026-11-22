// Satellite keyboard, second trial (throwaway). LILYGO T-Embed, ESP32-S3-WROOM-1.
// USB: interface 0 a boot keyboard (subclass 1, protocol 1, 8-byte reports, no report id),
//      interface 1 a read-only mass-storage "seed" drive (FAT, label CIDATA) for cloud-init's NoCloud source.
// Wi-Fi: TCP port 5577, one JSON object per line in (up to 16 KB), one JSON line out. Every command carries "token".
// New in trial 2: keys armed for the host's next "configured" (kept in flash if asked), seed files replaced over
// Wi-Fi, several keys in one report, press and release as separate commands.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "tusb_msc_storage.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "ff.h"
#include "secrets.h"

#define PORT 5577
#define LINE_LEN 16384 // one JSON line; seed files of a few KB, base64, fit
#define PIN_POWER_ON 46 // T-Embed: keeps the board's peripherals rail on

// ---------- USB descriptors (unchanged from trial 1) ----------
// USB HID 1.11 appendix B.1 / E.6, the 63-byte boot keyboard report descriptor
static const uint8_t report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};
_Static_assert(sizeof(report_desc) == 63, "boot keyboard descriptor is 63 bytes");

static const tusb_desc_device_t dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // Espressif; trial only
    .idProduct = 0x4006, // keyboard + seed drive; same descriptors as trial 1's second build
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CFG_LEN, 0, 500),
    // itf 0, string 4, boot protocol keyboard (macro sets subclass 1), EP 0x81 IN, 8 bytes, 10 ms
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(report_desc), 0x81, 8, 10),
    // itf 1, string 5, mass storage (SCSI, bulk only), EP 0x02 OUT, EP 0x82 IN, 64 bytes
    TUD_MSC_DESCRIPTOR(1, 5, 0x02, 0x82, 64),
};

static const char *str_desc[] = {(char[]){0x09, 0x04}, "wiscosat", "satellite keyboard", "0001", "boot keyboard", "seed drive"};

// ---------- events ----------
#define NEV 48
static struct {
    uint32_t ms;
    const char *what;
} events[NEV];
static volatile int nev;
static portMUX_TYPE ev_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void ev(const char *what)
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

// ---------- seed drive ----------
// A 1 MB FAT volume labelled CIDATA in the "seed" flash partition (wear levelling, 512-byte sectors).
// The host only ever gets it read-only. esp_tinyusb hands the volume to the host when the host configures the
// device, and back to the firmware when it is unconfigured; while the firmware has it mounted, the host's
// TEST UNIT READY gets "medium not present". The seed command uses exactly that to take it away while writing.
static const char USER_DATA[] = "#cloud-config\n# placeholder seed file, replace with the seed command\n";
static const char META_DATA[] = "instance-id: satellite-placeholder\nlocal-hostname: satellite-seed-test\n";
#define SEED_PATH "/seed"
#define NFILES 8
static struct {
    char name[64];
    uint32_t size;
    char sha256[65];
} seed_files[NFILES];
static int nseed;
static const char *seed_state = "not started";
static volatile bool seed_busy;
static uint32_t seed_written_ms, seed_away_ms;
static SemaphoreHandle_t seed_lock;

bool tud_msc_is_writable_cb(uint8_t lun) { return false; }

static bool sha_file(const char *path, uint32_t *size, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    uint8_t buf[256], d[32];
    size_t n;
    *size = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mbedtls_sha256_update(&c, buf, n);
        *size += n;
    }
    fclose(f);
    mbedtls_sha256_finish(&c, d);
    mbedtls_sha256_free(&c);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    return true;
}

// list the root of the mounted volume into seed_files (firmware must have it mounted)
static void seed_scan(void)
{
    DIR *d = opendir(SEED_PATH);
    nseed = 0;
    if (!d) return;
    struct dirent *e;
    char path[300];
    while ((e = readdir(d)) && nseed < NFILES) {
        if (e->d_type == DT_DIR) continue;
        snprintf(path, sizeof(path), SEED_PATH "/%s", e->d_name);
        strlcpy(seed_files[nseed].name, e->d_name, sizeof(seed_files[0].name));
        if (sha_file(path, &seed_files[nseed].size, seed_files[nseed].sha256)) nseed++;
    }
    closedir(d);
}

// take the volume for the firmware at SEED_PATH (from then on the host sees "medium not present")
static esp_err_t seed_take(void)
{
    if (!tinyusb_msc_storage_in_use_by_usb_host()) tinyusb_msc_storage_unmount(); // mounted elsewhere ("/data")
    return tinyusb_msc_storage_mount(SEED_PATH);
}

static void put_if_missing(const char *path, const char *text)
{
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return; // trial 1 rewrote compiled-in files here; now files from the seed command must survive a restart
    }
    f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void seed_label(void)
{
    char label[24] = "";
    f_getlabel("0:", label, NULL);
    if (strcmp(label, "CIDATA")) f_setlabel("0:CIDATA");
}

static void seed_init(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "seed");
    wl_handle_t wl = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(wl_mount(p, &wl));
    const tinyusb_msc_spiflash_config_t cfg = {.wl_handle = wl};
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&cfg));
    if (tinyusb_msc_storage_mount(SEED_PATH) != ESP_OK) { // formats an empty partition
        seed_state = "mount failed";
        return;
    }
    seed_label();
    put_if_missing(SEED_PATH "/user-data", USER_DATA);
    put_if_missing(SEED_PATH "/meta-data", META_DATA);
    seed_scan();
    seed_state = "ready";
    // left mounted here: USB is not up yet; esp_tinyusb unmounts it for the host when the host configures
}

typedef struct {
    const char *name;
    uint8_t *data;
    size_t len;
} newfile_t;

// replace the volume's files; the host does not have the medium while this runs
static const char *seed_write(newfile_t *nf, int n, int hold_ms)
{
    xSemaphoreTake(seed_lock, portMAX_DELAY);
    seed_busy = true;
    const char *err = NULL;
    seed_away_ms = now_ms();
    if (seed_take() != ESP_OK) {
        err = "could not take the volume from the host";
        goto out;
    }
    ev("seed: medium taken from host");
    vTaskDelay(pdMS_TO_TICKS(hold_ms)); // let the host's polling see "not present" before anything changes
    // remove files not in the new set
    DIR *d = opendir(SEED_PATH);
    if (d) {
        struct dirent *e;
        char path[300];
        while ((e = readdir(d))) {
            if (e->d_type == DT_DIR) continue;
            bool keep = false;
            for (int i = 0; i < n; i++)
                if (!strcmp(e->d_name, nf[i].name)) keep = true;
            if (!keep) {
                snprintf(path, sizeof(path), SEED_PATH "/%s", e->d_name);
                unlink(path);
            }
        }
        closedir(d);
    }
    for (int i = 0; i < n && !err; i++) {
        char path[300];
        snprintf(path, sizeof(path), SEED_PATH "/%s", nf[i].name);
        FILE *f = fopen(path, "wb");
        if (!f) err = "could not open a file for writing";
        else {
            if (fwrite(nf[i].data, 1, nf[i].len, f) != nf[i].len) err = "short write";
            fclose(f);
        }
    }
    seed_label();
    seed_scan();
    seed_written_ms = now_ms();
    vTaskDelay(pdMS_TO_TICKS(200));
    tinyusb_msc_storage_unmount(); // the host's next TEST UNIT READY finds the medium again
    ev(err ? "seed: write failed, medium back to host" : "seed: files written, medium back to host");
out:
    seed_state = err ? "write failed" : "ready";
    seed_busy = false;
    xSemaphoreGive(seed_lock);
    return err;
}

// ---------- state ----------
static SemaphoreHandle_t key_lock;
static uint8_t last_report[8];
static volatile uint8_t leds;
static volatile int protocol = -1; // -1 never set by host, 0 boot, 1 report
static volatile uint32_t keys_sent, first_mount_ms, wifi_ip_ms, first_cmd_ms, last_configured_ms;
static char ip_str[16] = "";
static volatile bool want_bootloader;
static uint32_t usb_started_ms;
static uint8_t held_mod, held_keys[6]; // press/release state

static bool listening(void) { return tud_mounted() && !tud_suspended(); }

// ---------- armed key ----------
// A key armed now is sent by the satellite itself on the host's next "configured" (a USB bus reset and
// SET_CONFIGURATION, as a PC's firmware does at every start), every every_ms for for_ms, then disarmed.
// With "persist" it is kept in NVS so it survives the satellite's own power cut (a cold start of the target).
typedef struct {
    char name[48];
    uint16_t every_ms, for_ms;
    uint8_t persist;
} arm_t;
static arm_t arm;
static volatile bool armed;
static uint32_t armed_at_ms;
static const char *armed_from = "";
static struct {
    bool active, done;
    uint32_t configured_ms, first_taken_ms, end_ms;
    int tried, taken, not_listening, failed;
    const char *last_error;
    char name[48];
} fire;
static TaskHandle_t armer_h;

// ---------- TinyUSB HID callbacks ----------
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { return report_desc; }
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t *buf, uint16_t len)
{
    if (type != HID_REPORT_TYPE_INPUT || len < 8) return 0;
    memcpy(buf, last_report, 8);
    return 8;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type, uint8_t const *buf, uint16_t len)
{
    if (type == HID_REPORT_TYPE_OUTPUT && len >= 1) leds = buf[0];
}
void tud_hid_set_protocol_cb(uint8_t instance, uint8_t p)
{
    protocol = p;
    ev(p == 0 ? "host set boot protocol" : "host set report protocol");
}
// the host has collected a report: the moment that counts as "taken"
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    if (fire.active && !fire.first_taken_ms && len >= 3 && report[2]) fire.first_taken_ms = now_ms();
}

// watches the bus state; 1 ms resolution
static void usb_watch(void *arg)
{
    bool m = false, s = false;
    for (;;) {
        bool nm = tud_mounted(), ns = tud_suspended();
        if (nm != m) {
            if (nm) {
                last_configured_ms = now_ms();
                if (!first_mount_ms) first_mount_ms = last_configured_ms;
                if (armed && armer_h) xTaskNotifyGive(armer_h);
            } else {
                protocol = -1; // a bus reset puts HID back to its default; the host sets it again if it cares
                memset(held_keys, 0, sizeof(held_keys));
                held_mod = 0;
            }
            ev(nm ? "usb configured" : "usb unconfigured");
            m = nm;
        }
        if (ns != s) {
            ev(ns ? "usb suspended" : "usb resumed");
            s = ns;
        }
        vTaskDelay(1);
    }
}

// ---------- keys ----------
static bool send_report(uint8_t mod, const uint8_t keys[6])
{
    uint8_t r[8] = {mod, 0};
    memcpy(r + 2, keys, 6);
    for (int i = 0; i < 200 && !tud_hid_ready(); i++) vTaskDelay(1);
    if (!tud_hid_ready()) return false;
    memcpy(last_report, r, 8);
    return tud_hid_report(0, r, 8);
}

static bool wait_taken(void)
{
    for (int i = 0; i < 200 && !tud_hid_ready(); i++) vTaskDelay(1);
    return tud_hid_ready();
}

// modifiers down, then all keys down together, 30 ms, then a report with everything up
static const char *tap(uint8_t mod, const uint8_t keys[6])
{
    static const uint8_t none[6] = {0};
    if (!listening()) return "target not listening";
    xSemaphoreTake(key_lock, portMAX_DELAY);
    bool ok = true;
    if (mod && keys[0]) { // modifiers first, as a person does
        ok = send_report(mod, none);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ok = ok && send_report(mod, keys);
    vTaskDelay(pdMS_TO_TICKS(30));
    bool up = send_report(0, none);
    vTaskDelay(pdMS_TO_TICKS(20));
    up = up && wait_taken(); // the all-up report must really have been collected by the host
    xSemaphoreGive(key_lock);
    if (!ok) return "host did not take the key report";
    if (!up) return "host did not take the key-up report";
    keys_sent++;
    return NULL;
}

// US layout: ASCII to key code and shift
static bool ascii_key(char c, uint8_t *key, uint8_t *mod)
{
    static const char plain[] = "-=[]\\#;'`,./", shifted[] = "_+{}|~:\"~<>?";
    static const char digsh[] = "!@#$%^&*()";
    const char *p;
    if (c >= 'a' && c <= 'z') *key = 0x04 + (c - 'a');
    else if (c >= 'A' && c <= 'Z') *key = 0x04 + (c - 'A'), *mod |= 0x02;
    else if (c >= '1' && c <= '9') *key = 0x1E + (c - '1');
    else if (c == '0') *key = 0x27;
    else if (c == '\n') *key = 0x28;
    else if (c == '\t') *key = 0x2B;
    else if (c == ' ') *key = 0x2C;
    else if (c == '#') *key = 0x20, *mod |= 0x02;
    else if (c == '~') *key = 0x35, *mod |= 0x02;
    else if ((p = strchr(digsh, c)) && c) *key = (p - digsh == 9) ? 0x27 : 0x1E + (p - digsh), *mod |= 0x02;
    else if ((p = strchr(plain, c)) && c) *key = 0x2D + (p - plain);
    else if ((p = strchr(shifted, c)) && c) *key = 0x2D + (p - shifted), *mod |= 0x02;
    else return false;
    return true;
}

static const struct {
    const char *name;
    uint8_t key;
} named[] = {
    {"enter", 0x28}, {"esc", 0x29}, {"backspace", 0x2A}, {"tab", 0x2B}, {"space", 0x2C}, {"minus", 0x2D},
    {"capslock", 0x39}, {"f1", 0x3A}, {"f2", 0x3B}, {"f3", 0x3C}, {"f4", 0x3D}, {"f5", 0x3E}, {"f6", 0x3F},
    {"f7", 0x40}, {"f8", 0x41}, {"f9", 0x42}, {"f10", 0x43}, {"f11", 0x44}, {"f12", 0x45},
    {"printscreen", 0x46}, {"sysrq", 0x46}, {"scrolllock", 0x47}, {"pause", 0x48}, {"insert", 0x49},
    {"home", 0x4A}, {"pageup", 0x4B}, {"delete", 0x4C}, {"del", 0x4C}, {"end", 0x4D}, {"pagedown", 0x4E},
    {"right", 0x4F}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52},
};

static uint8_t mod_of(const char *t)
{
    if (!strcmp(t, "ctrl") || !strcmp(t, "control")) return 0x01;
    if (!strcmp(t, "shift")) return 0x02;
    if (!strcmp(t, "alt")) return 0x04;
    if (!strcmp(t, "super") || !strcmp(t, "win") || !strcmp(t, "gui")) return 0x08;
    return 0;
}

// "alt-sysrq-b" -> modifiers 0x04, keys {0x46, 0x05}; up to six keys
static const char *parse_combo(const char *name, uint8_t *mod, uint8_t keys[6])
{
    char buf[48], *save, *t;
    int n = 0;
    *mod = 0;
    memset(keys, 0, 6);
    if (strlen(name) >= sizeof(buf)) return "key name too long";
    strcpy(buf, name);
    if (!strcmp(buf, "-")) {
        keys[0] = 0x2D;
        return NULL;
    }
    for (t = strtok_r(buf, "-", &save); t; t = strtok_r(NULL, "-", &save)) {
        uint8_t m = mod_of(t), k = 0;
        if (m) {
            *mod |= m;
            continue;
        }
        for (int i = 0; i < sizeof(named) / sizeof(named[0]); i++)
            if (!strcmp(t, named[i].name)) k = named[i].key;
        if (!k && !(strlen(t) == 1 && ascii_key(t[0], &k, mod))) return "unknown key name";
        if (n == 6) return "more than six keys";
        keys[n++] = k;
    }
    if (!*mod && !n) return "empty key name";
    return NULL;
}

static const char *do_key(const char *name)
{
    uint8_t mod, keys[6];
    const char *e = parse_combo(name, &mod, keys);
    return e ? e : tap(mod, keys);
}

// press: add to what is held and send; release: take away (or everything when no name) and send
static const char *do_hold(const char *name, bool press)
{
    uint8_t mod = 0, keys[6] = {0};
    if (name) {
        const char *e = parse_combo(name, &mod, keys);
        if (e) return e;
    } else if (press) return "no name";
    if (!listening()) return "target not listening";
    xSemaphoreTake(key_lock, portMAX_DELAY);
    if (press) {
        held_mod |= mod;
        for (int i = 0; i < 6 && keys[i]; i++) {
            bool have = false;
            int j;
            for (j = 0; j < 6 && held_keys[j]; j++)
                if (held_keys[j] == keys[i]) have = true;
            if (!have && j < 6) held_keys[j] = keys[i];
        }
    } else if (!name) {
        held_mod = 0;
        memset(held_keys, 0, 6);
    } else {
        held_mod &= ~mod;
        uint8_t left[6] = {0};
        int n = 0;
        for (int j = 0; j < 6 && held_keys[j]; j++)
            if (!memchr(keys, held_keys[j], 6)) left[n++] = held_keys[j];
        memcpy(held_keys, left, 6);
    }
    bool ok = send_report(held_mod, held_keys) && (vTaskDelay(pdMS_TO_TICKS(10)), wait_taken());
    xSemaphoreGive(key_lock);
    if (!ok) return "host did not take the report";
    keys_sent++;
    return NULL;
}

static const char *do_type(const char *text)
{
    for (; *text; text++) {
        uint8_t mod = 0, keys[6] = {0};
        if (!ascii_key(*text, &keys[0], &mod)) return "character not on the US layout";
        const char *e = tap(mod, keys);
        if (e) return e;
    }
    return NULL;
}

// ---------- armed key: storage and the firing task ----------
static void arm_save(void)
{
    nvs_handle_t h;
    if (nvs_open("sat", NVS_READWRITE, &h) != ESP_OK) return;
    if (armed && arm.persist) nvs_set_blob(h, "arm", &arm, sizeof(arm));
    else nvs_erase_key(h, "arm");
    nvs_commit(h);
    nvs_close(h);
}

static void arm_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(arm);
    if (nvs_open("sat", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "arm", &arm, &len) == ESP_OK && len == sizeof(arm)) {
        armed = true;
        armed_at_ms = now_ms();
        armed_from = "flash";
        ev("armed key loaded from flash");
    }
    nvs_close(h);
}

static void armer_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!armed) continue;
        arm_t a = arm;
        uint8_t mod, keys[6];
        armed = false; // once: the next configured event is the one
        memset(&fire, 0, sizeof(fire));
        strlcpy(fire.name, a.name, sizeof(fire.name));
        fire.configured_ms = last_configured_ms;
        fire.active = true;
        ev("armed key firing");
        parse_combo(a.name, &mod, keys); // checked when armed
        for (int i = 0;; i++) {
            uint32_t due = fire.configured_ms + (uint32_t)i * a.every_ms;
            if (due - fire.configured_ms > a.for_ms) break;
            int32_t wait = (int32_t)(due - now_ms());
            if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
            else if (wait < -(int32_t)a.every_ms) continue; // fell behind: skip the slot, do not bunch up
            fire.tried++;
            const char *e = tap(mod, keys);
            if (!e) fire.taken++;
            else if (!strcmp(e, "target not listening")) fire.not_listening++;
            else fire.failed++, fire.last_error = e;
        }
        fire.end_ms = now_ms();
        fire.active = false;
        fire.done = true;
        if (a.persist) arm_save(); // armed is false now: erases it from flash
        ev("armed key done");
    }
}

// ---------- commands ----------
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

static cJSON *state_json(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "target", listening() ? "listening" : "not listening");
    cJSON_AddBoolToObject(s, "configured", tud_mounted());
    cJSON_AddBoolToObject(s, "suspended", tud_suspended());
    cJSON_AddStringToObject(s, "protocol", protocol < 0 ? "not set by host" : protocol == 0 ? "boot" : "report");
    cJSON_AddNumberToObject(s, "leds", leds);
    cJSON_AddNumberToObject(s, "keys_sent", keys_sent);
    cJSON_AddNumberToObject(s, "uptime_ms", now_ms());
    cJSON_AddNumberToObject(s, "first_configured_ms", first_mount_ms);
    cJSON_AddNumberToObject(s, "last_configured_ms", last_configured_ms);
    cJSON_AddNumberToObject(s, "wifi_ip_ms", wifi_ip_ms);
    cJSON_AddNumberToObject(s, "first_command_ms", first_cmd_ms);
    cJSON_AddStringToObject(s, "reset_reason", reset_reason());
    cJSON_AddStringToObject(s, "ip", ip_str);
    cJSON_AddNumberToObject(s, "usb_started_ms", usb_started_ms);

    if (armed) {
        cJSON *a = cJSON_AddObjectToObject(s, "armed");
        cJSON_AddStringToObject(a, "name", arm.name);
        cJSON_AddNumberToObject(a, "every_ms", arm.every_ms);
        cJSON_AddNumberToObject(a, "for_ms", arm.for_ms);
        cJSON_AddBoolToObject(a, "persist", arm.persist);
        cJSON_AddNumberToObject(a, "armed_at_ms", armed_at_ms);
        cJSON_AddStringToObject(a, "from", armed_from);
    } else cJSON_AddNullToObject(s, "armed");
    if (fire.active || fire.done) {
        cJSON *f = cJSON_AddObjectToObject(s, "last_fire");
        cJSON_AddStringToObject(f, "name", fire.name);
        cJSON_AddBoolToObject(f, "running", fire.active);
        cJSON_AddNumberToObject(f, "configured_ms", fire.configured_ms);
        if (fire.first_taken_ms) {
            cJSON_AddNumberToObject(f, "first_taken_ms", fire.first_taken_ms);
            cJSON_AddNumberToObject(f, "first_taken_after_configured_ms", fire.first_taken_ms - fire.configured_ms);
        } else cJSON_AddNullToObject(f, "first_taken_ms");
        cJSON_AddNumberToObject(f, "end_ms", fire.end_ms);
        cJSON_AddNumberToObject(f, "tried", fire.tried);
        cJSON_AddNumberToObject(f, "taken", fire.taken);
        cJSON_AddNumberToObject(f, "not_listening", fire.not_listening);
        cJSON_AddNumberToObject(f, "failed", fire.failed);
        if (fire.last_error) cJSON_AddStringToObject(f, "last_error", fire.last_error);
    } else cJSON_AddNullToObject(s, "last_fire");

    cJSON *sd = cJSON_AddObjectToObject(s, "seed");
    cJSON_AddStringToObject(sd, "state", seed_busy ? "writing (host sees no medium)" : seed_state);
    cJSON_AddBoolToObject(sd, "host_has_medium", tinyusb_msc_storage_in_use_by_usb_host());
    cJSON_AddNumberToObject(sd, "written_ms", seed_written_ms);
    cJSON *fl = cJSON_AddArrayToObject(sd, "files");
    for (int i = 0; i < nseed; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", seed_files[i].name);
        cJSON_AddNumberToObject(f, "size", seed_files[i].size);
        cJSON_AddStringToObject(f, "sha256", seed_files[i].sha256);
        cJSON_AddItemToArray(fl, f);
    }

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

static int num(cJSON *req, const char *k, int dflt)
{
    cJSON *v = cJSON_GetObjectItem(req, k);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

// {"cmd":"key","name":"f9","when":"configured","every_ms":100,"for_ms":1500,"persist":true}
static const char *do_arm(cJSON *req, const char *name)
{
    uint8_t mod, keys[6];
    const char *e = parse_combo(name, &mod, keys);
    if (e) return e;
    int every = num(req, "every_ms", 100), dur = num(req, "for_ms", 1500);
    if (every < 20 || every > 5000) return "every_ms must be 20..5000";
    if (dur < 0 || dur > 30000) return "for_ms must be 0..30000";
    if (fire.active) return "an armed key is firing now";
    memset(&arm, 0, sizeof(arm));
    strlcpy(arm.name, name, sizeof(arm.name));
    arm.every_ms = every;
    arm.for_ms = dur;
    arm.persist = cJSON_IsTrue(cJSON_GetObjectItem(req, "persist"));
    armed_at_ms = now_ms();
    armed_from = "command";
    armed = true;
    arm_save();
    ev("key armed for next configured");
    return NULL;
}

// {"cmd":"seed","encoding":"base64","files":{"user-data":"...","meta-data":"..."},"hold_ms":2000}
static const char *do_seed(cJSON *req)
{
    cJSON *files = cJSON_GetObjectItem(req, "files");
    const char *enc = cJSON_GetStringValue(cJSON_GetObjectItem(req, "encoding"));
    bool b64 = enc && !strcmp(enc, "base64");
    if (enc && !b64 && strcmp(enc, "text")) return "encoding must be text or base64";
    if (!cJSON_IsObject(files)) return "no files object";
    int hold = num(req, "hold_ms", 2000);
    if (hold < 0 || hold > 10000) return "hold_ms must be 0..10000";
    newfile_t nf[NFILES];
    int n = 0;
    const char *err = NULL;
    cJSON *f;
    cJSON_ArrayForEach(f, files)
    {
        const char *txt = cJSON_GetStringValue(f);
        if (n == NFILES) err = "too many files";
        else if (!txt) err = "file content must be a string";
        else if (!f->string[0] || strlen(f->string) > 60 || strpbrk(f->string, "/\\:*?\"<>|")) err = "bad file name";
        if (err) break;
        size_t tl = strlen(txt), len = tl;
        uint8_t *data = malloc(tl + 1);
        if (!data) {
            err = "out of memory";
            break;
        }
        if (b64) {
            if (mbedtls_base64_decode(data, tl + 1, &len, (const uint8_t *)txt, tl)) {
                free(data);
                err = "bad base64";
                break;
            }
        } else memcpy(data, txt, tl);
        nf[n++] = (newfile_t){f->string, data, len};
    }
    if (!err && !n) err = "no files";
    if (!err && fire.active) err = "an armed key is firing now";
    if (!err) err = seed_write(nf, n, hold);
    for (int i = 0; i < n; i++) free(nf[i].data);
    return err;
}

static char *handle_line(const char *line)
{
    const char *err = NULL;
    cJSON *req = cJSON_Parse(line), *resp = cJSON_CreateObject();
    if (!first_cmd_ms) first_cmd_ms = now_ms();
    if (!cJSON_IsObject(req)) err = "not a JSON object";
    else {
        const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(req, "token"));
        const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(req, "cmd"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(req, "name"));
        const char *when = cJSON_GetStringValue(cJSON_GetObjectItem(req, "when"));
        if (!tok || strcmp(tok, TOKEN)) err = "bad token";
        else if (!cmd) err = "no cmd";
        else if (!strcmp(cmd, "status")) err = NULL;
        else if (!strcmp(cmd, "key")) {
            if (!name) err = "no name";
            else if (!when || !strcmp(when, "now")) err = do_key(name);
            else if (!strcmp(when, "configured")) err = do_arm(req, name);
            else err = "when must be now or configured";
        } else if (!strcmp(cmd, "disarm")) {
            armed = false;
            arm_save();
            ev("disarmed");
        } else if (!strcmp(cmd, "press")) err = do_hold(name, true);
        else if (!strcmp(cmd, "release")) err = do_hold(name, false);
        else if (!strcmp(cmd, "type")) {
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(req, "text"));
            err = text ? do_type(text) : "no text";
        } else if (!strcmp(cmd, "seed")) err = do_seed(req);
        else if (!strcmp(cmd, "bootloader")) want_bootloader = true; // acted on after the answer is sent
        else err = "unknown cmd";
    }
    cJSON_AddBoolToObject(resp, "ok", err == NULL);
    if (err) cJSON_AddStringToObject(resp, "error", err);
    else cJSON_AddItemToObject(resp, "state", state_json());
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(req);
    return out;
}

static void conn_task(void *arg)
{
    int fd = (int)(intptr_t)arg, len = 0;
    char *buf = malloc(LINE_LEN);
    struct timeval tv = {.tv_sec = 60};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    while (buf) {
        int n = recv(fd, buf + len, LINE_LEN - 1 - len, 0);
        if (n <= 0) break;
        len += n;
        buf[len] = 0;
        char *nl;
        while ((nl = strchr(buf, '\n'))) {
            *nl = 0;
            char *out = handle_line(buf);
            if (out) {
                send(fd, out, strlen(out), 0);
                send(fd, "\n", 1, 0);
                free(out);
            }
            if (want_bootloader) { // restart into the ROM's download mode, for flashing with no buttons
                vTaskDelay(pdMS_TO_TICKS(300));
                REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
                esp_restart();
            }
            len -= (nl + 1 - buf);
            memmove(buf, nl + 1, len + 1);
        }
        if (len >= LINE_LEN - 1) { // line too long: say so instead of a silent close
            static const char e[] = "{\"ok\":false,\"error\":\"line too long\"}\n";
            send(fd, e, sizeof(e) - 1, 0);
            break;
        }
    }
    free(buf);
    close(fd);
    vTaskDelete(NULL);
}

static void server_task(void *arg)
{
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(s, (struct sockaddr *)&a, sizeof(a));
    listen(s, 4);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (xTaskCreate(conn_task, "conn", 8192, (void *)(intptr_t)fd, 5, NULL) != pdPASS) close(fd);
    }
}

// ---------- Wi-Fi ----------
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ev("wifi disconnected");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        if (!wifi_ip_ms) wifi_ip_ms = now_ms();
        ev("wifi got ip");
    }
}

void app_main(void)
{
    gpio_set_direction(PIN_POWER_ON, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_POWER_ON, 1);
    key_lock = xSemaphoreCreateMutex();
    seed_lock = xSemaphoreCreateMutex();

    // NVS first now: an armed key kept in flash must be armed before the host can configure us
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
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &dev_desc,
        .string_descriptor = str_desc,
        .string_descriptor_count = sizeof(str_desc) / sizeof(str_desc[0]),
        .external_phy = false,
        .configuration_descriptor = cfg_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    usb_started_ms = now_ms();
    ev("usb stack started");
    xTaskCreate(usb_watch, "usbwatch", 3072, NULL, 8, NULL);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wic);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    xTaskCreate(server_task, "server", 4096, NULL, 5, NULL);
}
