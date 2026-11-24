// The knob's menu as a text: what the list on the screen holds, in which order. One entry a line,
//   <label> | <what> | <keys>
// label: what the screen shows, 1 to 20 of the characters its font has (letters, digits, space and - . : _ / ?)
// what:  arm   one key, sent by the unit itself from the computer's next start (and every later one)
//        send  the keys, once, now, at a press of the knob
//        hold  as send, but the knob must be held for 2 s (for what restarts a machine with no question)
// keys:  key names as keys.h takes them (f2, delete, ctrl-alt-del, alt-sysrq-b), blanks between them; send and
//        hold take up to 12, sent 100 ms apart, and a wait written 500ms (1 to 10000) in between where one is needed
// A line that starts with # is a comment; empty lines are skipped. Up to 40 entries, 4096 bytes.
// Pure: no ESP-IDF in it, tested on Linux. The unit keeps the text (NVS); with none it uses menu_builtin.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "keys.h"

#define MENU_TEXT_MAX 4096
#define MENU_ENTRIES_MAX 40
#define MENU_LABEL_MAX 20
#define MENU_KEYS_MAX 160 // the bytes of an entry's keys field
#define MENU_STEPS_MAX 12 // the keys of a send or hold entry (waits not counted)
#define MENU_WAIT_MS_MAX 10000
#define MENU_GAP_MS 100   // between two keys with no wait written

typedef enum { MENU_ARM, MENU_SEND, MENU_HOLD } menu_kind_t;

typedef struct {
    char label[MENU_LABEL_MAX + 1];
    menu_kind_t kind;
    char keys[MENU_KEYS_MAX + 1]; // the field as written, blanks trimmed
} menu_entry_t;

typedef struct {
    int n;
    menu_entry_t e[MENU_ENTRIES_MAX];
} menu_t;

extern const char menu_builtin[]; // the list a unit has with no text of its own

// 0 and *m filled, or -1 and why in err ("menu line 4: ..."; it names the line and the part not understood).
// err is the caller's (MENU_ERR_MAX bytes hold any): no buffer of its own, so no task pays for one
#define MENU_ERR_MAX 192
int menu_parse(const char *text, menu_t *m, char *err, size_t size);

typedef struct {
    bool wait;        // a wait of wait_ms; else a key
    uint32_t wait_ms; // for a key: the gap before it (0 for the first, MENU_GAP_MS after another key, 0 after a wait)
    uint8_t report[8];
    char name[KEYS_NAME_MAX + 1];
} menu_step_t;

// the i-th part (from 0) of an entry's keys: 1 and *out filled, 0 no more, -1 not understood (menu_parse refuses
// such a text, so never for an entry it gave)
int menu_step(const char *keys, int i, menu_step_t *out);
