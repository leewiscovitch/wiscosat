// A unit's settings, kept in its NVS (namespace "sat"): name, Wi-Fi, address, its key. Nothing of a unit is
// compiled in. settings_check is pure (tested on Linux); settings_load reads NVS (on the board only).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETTINGS_NS "sat"
#define SETTINGS_KEY_LEN 32

typedef struct {
    char name[33];      // the unit's name, also the channel's PSK identity
    char wifi_ssid[33]; // up to 32 bytes
    char wifi_pass[65]; // empty (an open network) or 8 to 63 characters
    char ip_str[16], mask_str[16], gw_str[16];
    uint8_t ip[4], mask[4], gw[4]; // parsed by settings_check
    uint8_t key[SETTINGS_KEY_LEN];
    size_t key_len;
} settings_t;

// a dotted quad, a.b.c.d, each 0 to 255 in decimal, nothing else around it
bool settings_parse_ip(const char *s, uint8_t out[4]);

// true when the set is whole and good (the addresses parsed into ip, mask, gw); false: the board is unenrolled
bool settings_check(settings_t *s);

// true: enrolled, *s filled; false: unenrolled (no Wi-Fi, a keyboard only). Never logs a value
bool settings_load(settings_t *s);
