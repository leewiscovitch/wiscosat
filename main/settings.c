// A unit's settings (settings.h). The checks are pure; reading NVS is built on the board only.
#include <string.h>
#include "settings.h"

bool settings_parse_ip(const char *s, uint8_t out[4])
{
    for (int part = 0; part < 4; part++) {
        unsigned v = 0;
        int digits = 0;
        for (; *s >= '0' && *s <= '9'; s++, digits++) {
            if (digits == 3) return false;
            v = v * 10 + (unsigned)(*s - '0');
        }
        if (!digits || v > 255) return false;
        out[part] = (uint8_t)v;
        if (part < 3 && *s++ != '.') return false;
    }
    return *s == 0;
}

bool settings_check(settings_t *s)
{
    const char *end = memchr(s->wifi_pass, 0, sizeof(s->wifi_pass));
    size_t pass = end ? (size_t)(end - s->wifi_pass) : sizeof(s->wifi_pass);
    return s->name[0] && s->wifi_ssid[0] && (pass == 0 || (pass >= 8 && pass <= 63)) &&
           s->key_len == SETTINGS_KEY_LEN && settings_parse_ip(s->ip_str, s->ip) &&
           settings_parse_ip(s->mask_str, s->mask) && settings_parse_ip(s->gw_str, s->gw);
}

#ifdef ESP_PLATFORM
#include "nvs.h"

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t size)
{
    size_t len = size;
    return nvs_get_str(h, key, out, &len) == ESP_OK;
}

bool settings_load(settings_t *s)
{
    nvs_handle_t h;
    memset(s, 0, sizeof(*s));
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    s->key_len = sizeof(s->key);
    bool ok = get_str(h, "name", s->name, sizeof(s->name)) &&
              get_str(h, "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid)) &&
              get_str(h, "ip", s->ip_str, sizeof(s->ip_str)) && get_str(h, "mask", s->mask_str, sizeof(s->mask_str)) &&
              get_str(h, "gw", s->gw_str, sizeof(s->gw_str)) && nvs_get_blob(h, "key", s->key, &s->key_len) == ESP_OK;
    // no password: an open network
    if (ok && !get_str(h, "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass))) s->wifi_pass[0] = 0;
    nvs_close(h);
    if (ok && settings_check(s)) return true;
    memset(s, 0, sizeof(*s)); // the key never lingers in a half-read set
    return false;
}
#endif
