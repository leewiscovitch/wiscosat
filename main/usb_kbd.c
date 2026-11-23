// The USB device and its keyboard. USB: interface 0 a boot keyboard (subclass 1, protocol 1, 8-byte reports, no
// report id), interface 1 the read-only seed drive (usb_msc.c). The reports come from keys.c.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "esp_mac.h"
#include "keys.h"
#include "sat.h"
#include "usbserial.h"

// a report the host has not collected within this long is given up (the host is gone, or not polling)
#define GIVE_UP_MS 50

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
    .idVendor = 0x303A,  // Espressif's
    .idProduct = 0x4006, // keyboard + seed drive
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

// the serial number is the unit's MAC (usbserial.h), filled in by usb_start
static char serial[USBSERIAL_LEN + 1] = "000000000000";
static const char *str_desc[] = {(char[]){0x09, 0x04}, "wiscosat", "satellite keyboard", serial, "boot keyboard",
                                 "seed drive"};

static SemaphoreHandle_t key_lock;
static uint8_t last_report[8];
static volatile uint8_t leds;
static volatile int protocol = -1; // -1 never set by the host, 0 boot, 1 report
static volatile uint32_t keys_sent, first_configured_ms, last_configured_ms;
static uint32_t usb_started_ms;
static uint8_t held[8]; // press and release: what is held now, as a report

bool usb_configured(void) { return tud_mounted(); }
bool usb_suspended(void) { return tud_suspended(); }
uint32_t usb_last_configured_ms(void) { return last_configured_ms; }

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

// watches the bus state; 1 ms resolution
static void usb_watch(void *arg)
{
    bool m = false, s = false;
    for (;;) {
        bool nm = tud_mounted(), ns = tud_suspended();
        if (nm != m) {
            if (nm) {
                last_configured_ms = now_ms();
                if (!first_configured_ms) first_configured_ms = last_configured_ms;
                app_usb_configured();
            } else {
                protocol = -1; // a bus reset puts HID back to its default; the host sets it again if it cares
                memset(held, 0, sizeof(held));
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

// ---------- sending ----------
static bool ready_within(int ms)
{
    for (int i = 0; i < ms && !tud_hid_ready(); i++) vTaskDelay(1);
    return tud_hid_ready();
}

static bool send_report(const uint8_t r[8])
{
    if (!ready_within(GIVE_UP_MS)) return false;
    memcpy(last_report, r, 8);
    return tud_hid_report(0, r, 8);
}

static const char *no_host(void)
{
    if (!tud_mounted()) return "no host";
    if (tud_suspended()) return "host asleep (USB suspended)";
    return NULL;
}

// modifiers down, then all keys down together, 30 ms, then back to what is held (press): everything else up.
// A tap while keys are held carries them, as a key pressed on a keyboard with ctrl held down does
const char *kbd_tap(const uint8_t tap[8])
{
    const char *e = no_host();
    if (e) return e;
    xSemaphoreTake(key_lock, portMAX_DELAY);
    uint8_t report[8];
    if (keys_merge(held, tap, report)) {
        xSemaphoreGive(key_lock);
        return "too many keys held for this one (six at most)";
    }
    bool ok = true;
    if (report[0] != held[0] && report[2]) { // modifiers first, as a person does
        uint8_t mods[8];
        memcpy(mods, held, 8);
        mods[0] = report[0];
        ok = send_report(mods);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ok = ok && send_report(report);
    vTaskDelay(pdMS_TO_TICKS(30));
    bool up = send_report(held);
    vTaskDelay(pdMS_TO_TICKS(20));
    up = up && ready_within(GIVE_UP_MS); // the all-up report must really have been collected by the host
    xSemaphoreGive(key_lock);
    if (!ok) return "host did not take the key report";
    if (!up) return "host did not take the key-up report";
    keys_sent++;
    return NULL;
}

const char *kbd_hold(const uint8_t report[8], bool press)
{
    const char *e = no_host();
    if (e) return e;
    xSemaphoreTake(key_lock, portMAX_DELAY);
    if (press) keys_merge(held, report, held); // a key past the sixth is not held (as before)
    else if (!report) memset(held, 0, sizeof(held));
    else {
        uint8_t left[8] = {held[0] & ~report[0]};
        int n = 2;
        for (int j = 2; j < 8 && held[j]; j++)
            if (!memchr(report + 2, held[j], 6)) left[n++] = held[j];
        memcpy(held, left, sizeof(held));
    }
    bool ok = send_report(held) && (vTaskDelay(pdMS_TO_TICKS(10)), ready_within(GIVE_UP_MS));
    xSemaphoreGive(key_lock);
    if (!ok) return "host did not take the report";
    keys_sent++;
    return NULL;
}

void kbd_state(cJSON *s)
{
    bool listening = tud_mounted() && !tud_suspended();
    cJSON_AddStringToObject(s, "target", listening ? "listening" : "not listening");
    cJSON_AddBoolToObject(s, "configured", tud_mounted());
    cJSON_AddBoolToObject(s, "suspended", tud_suspended());
    cJSON_AddStringToObject(s, "protocol", protocol < 0 ? "not set by host" : protocol == 0 ? "boot" : "report");
    cJSON_AddNumberToObject(s, "leds", leds);
    cJSON_AddNumberToObject(s, "keys_sent", keys_sent);
    cJSON_AddNumberToObject(s, "first_configured_ms", first_configured_ms);
    cJSON_AddNumberToObject(s, "last_configured_ms", last_configured_ms);
    cJSON_AddNumberToObject(s, "usb_started_ms", usb_started_ms);
}

void usb_start(void)
{
    key_lock = xSemaphoreCreateMutex();
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) usbserial_from_mac(mac, serial); // the base MAC, as esptool shows
    const tinyusb_config_t cfg = {
        .device_descriptor = &dev_desc,
        .string_descriptor = str_desc,
        .string_descriptor_count = sizeof(str_desc) / sizeof(str_desc[0]),
        .external_phy = false,
        .configuration_descriptor = cfg_desc,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    usb_started_ms = now_ms();
    ev("usb stack started");
    xTaskCreate(usb_watch, "usbwatch", 3072, NULL, 8, NULL);
}
