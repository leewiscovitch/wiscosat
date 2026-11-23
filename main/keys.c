// Key names and characters to a boot keyboard report (keys.h).
#include <stdio.h>
#include <string.h>
#include "keys.h"

#define SHIFT 0x02

// the US layout from 0x20 (space) to 0x7E (~): the key code, and SHIFT where it is the key's upper character
static const uint8_t ascii[95][2] = {
    {0x2C, 0}, {0x1E, SHIFT}, {0x34, SHIFT}, {0x20, SHIFT}, {0x21, SHIFT}, {0x22, SHIFT}, {0x24, SHIFT}, // space ! " # $ % &
    {0x34, 0}, {0x26, SHIFT}, {0x27, SHIFT}, {0x25, SHIFT}, {0x2E, SHIFT}, {0x36, 0}, {0x2D, 0},         // ' ( ) * + , -
    {0x37, 0}, {0x38, 0},                                                                               // . /
    {0x27, 0}, {0x1E, 0}, {0x1F, 0}, {0x20, 0}, {0x21, 0}, {0x22, 0}, {0x23, 0}, {0x24, 0}, {0x25, 0},  // 0 to 8
    {0x26, 0},                                                                                          // 9
    {0x33, SHIFT}, {0x33, 0}, {0x36, SHIFT}, {0x2E, 0}, {0x37, SHIFT}, {0x38, SHIFT}, {0x1F, SHIFT},    // : ; < = > ? @
    {0x04, SHIFT}, {0x05, SHIFT}, {0x06, SHIFT}, {0x07, SHIFT}, {0x08, SHIFT}, {0x09, SHIFT}, {0x0A, SHIFT}, // A to G
    {0x0B, SHIFT}, {0x0C, SHIFT}, {0x0D, SHIFT}, {0x0E, SHIFT}, {0x0F, SHIFT}, {0x10, SHIFT}, {0x11, SHIFT}, // H to N
    {0x12, SHIFT}, {0x13, SHIFT}, {0x14, SHIFT}, {0x15, SHIFT}, {0x16, SHIFT}, {0x17, SHIFT}, {0x18, SHIFT}, // O to U
    {0x19, SHIFT}, {0x1A, SHIFT}, {0x1B, SHIFT}, {0x1C, SHIFT}, {0x1D, SHIFT},                           // V to Z
    {0x2F, 0}, {0x31, 0}, {0x30, 0}, {0x23, SHIFT}, {0x2D, SHIFT}, {0x35, 0},                           // [ \ ] ^ _ `
    {0x04, 0}, {0x05, 0}, {0x06, 0}, {0x07, 0}, {0x08, 0}, {0x09, 0}, {0x0A, 0}, {0x0B, 0}, {0x0C, 0},  // a to i
    {0x0D, 0}, {0x0E, 0}, {0x0F, 0}, {0x10, 0}, {0x11, 0}, {0x12, 0}, {0x13, 0}, {0x14, 0}, {0x15, 0},  // j to r
    {0x16, 0}, {0x17, 0}, {0x18, 0}, {0x19, 0}, {0x1A, 0}, {0x1B, 0}, {0x1C, 0}, {0x1D, 0},             // s to z
    {0x2F, SHIFT}, {0x31, SHIFT}, {0x30, SHIFT}, {0x35, SHIFT},                                         // { | } ~
};
_Static_assert(sizeof(ascii) / sizeof(ascii[0]) == '~' - ' ' + 1, "one row per printable character");

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

static const struct {
    const char *name;
    uint8_t bit;
} modifiers[] = {
    {"ctrl", 0x01}, {"control", 0x01}, {"shift", 0x02}, {"alt", 0x04}, {"super", 0x08}, {"win", 0x08}, {"gui", 0x08},
};

int keys_type(char c, uint8_t report[8])
{
    memset(report, 0, 8);
    unsigned char u = (unsigned char)c;
    if (c == '\n') report[2] = 0x28;
    else if (c == '\t') report[2] = 0x2B;
    else if (u >= ' ' && u <= '~') {
        report[0] = ascii[u - ' '][1];
        report[2] = ascii[u - ' '][0];
    } else return -1;
    return 0;
}

// one part of a name: a modifier, a named key or a single character
static int add_part(const char *t, uint8_t report[8], int *n, const char **err)
{
    static _Thread_local char msg[KEYS_NAME_MAX + 32];
    uint8_t key = 0, r[8];
    for (size_t i = 0; i < sizeof(modifiers) / sizeof(modifiers[0]); i++)
        if (!strcmp(t, modifiers[i].name)) {
            report[0] |= modifiers[i].bit;
            return 0;
        }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (!strcmp(t, named[i].name)) key = named[i].key;
    if (!key && strlen(t) == 1 && keys_type(t[0], r) == 0) {
        key = r[2];
        report[0] |= r[0];
    }
    if (!key) {
        snprintf(msg, sizeof(msg), "unknown key name \"%s\"", t);
        *err = msg;
        return -1;
    }
    if (*n == 6) {
        *err = "more than six keys";
        return -1;
    }
    report[2 + (*n)++] = key;
    return 0;
}

int keys_parse(const char *name, uint8_t report[8], const char **err)
{
    char buf[KEYS_NAME_MAX + 1];
    int n = 0;
    memset(report, 0, 8);
    if (!name || !name[0]) {
        *err = "empty key name";
        return -1;
    }
    if (strlen(name) > KEYS_NAME_MAX) {
        *err = "key name too long";
        return -1;
    }
    if (!strcmp(name, "-")) {
        report[2] = 0x2D;
        return 0;
    }
    strcpy(buf, name);
    // split on '-' by hand: strtok would pass over an empty part ("ctrl--") instead of refusing it
    for (char *t = buf, *end;; t = end + 1) {
        end = strchr(t, '-');
        if (end) *end = 0;
        if (!*t) {
            *err = "empty part in key name (\"minus\" is the - key)";
            return -1;
        }
        if (add_part(t, report, &n, err)) return -1;
        if (!end) break;
    }
    return 0;
}

int keys_merge(const uint8_t held[8], const uint8_t tap[8], uint8_t out[8])
{
    uint8_t r[8] = {(uint8_t)(held[0] | tap[0]), 0};
    int n = 2, lost = 0;
    for (int i = 2; i < 8; i++)
        if (held[i]) r[n++] = held[i];
    for (int i = 2; i < 8; i++) {
        if (!tap[i] || memchr(r + 2, tap[i], (size_t)(n - 2))) continue;
        if (n < 8) r[n++] = tap[i];
        else lost++;
    }
    memcpy(out, r, 8);
    return lost;
}
