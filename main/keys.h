// Key names and characters to an 8-byte boot keyboard report. Pure: no ESP-IDF in it, tested on Linux.
// A report: [0] the modifiers (bit 0 ctrl, 1 shift, 2 alt, 3 super; the left ones), [1] 0, [2..7] up to six key
// codes (USB HID usage page 7).
#pragma once
#include <stdint.h>

#define KEYS_NAME_MAX 63 // the longest name keys_parse takes

// "f9", "alt-sysrq-b", "ctrl-alt-del": modifiers and up to six keys, '-' between them ("-" alone is the minus key,
// "minus" in a combination). "sysrq" is "printscreen". 0, or -1 with *err set (it names the part not known).
int keys_parse(const char *name, uint8_t report[8], const char **err);

// one character on the US layout (with shift where it needs it); -1 for a character it has no key for
int keys_type(char c, uint8_t report[8]);

// held and tap together, as a tap sends them while keys are held (`press`): the modifiers of both; held's keys, then
// tap's keys not already there, up to six. Returns how many of tap's keys found no room (0: all in).
int keys_merge(const uint8_t held[8], const uint8_t tap[8], uint8_t out[8]);
