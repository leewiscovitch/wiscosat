// How the device parts (usb_kbd.c, usb_msc.c, app.c) call each other. The pure parts have their own headers.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "menu.h"
#include "seedrules.h"
#include "settings.h"

// app.c
uint32_t now_ms(void);
void ev(const char *what);     // a line in status's events; a fixed string, never typed text
void app_usb_configured(void); // the watcher's call when a host has configured the unit
// A command whose raw bytes follow its line (seed, update): the channel feeds them to chunk, then asks end for the
// final answer line
typedef struct {
    uint32_t size;                                    // the bytes that follow; 0: none (the answer is final)
    const char *(*chunk)(const uint8_t *p, size_t n); // NULL taken; an error stops the transfer
    char *(*end)(const char *broke); // the final answer (free() it); broke: why the bytes stopped short, else NULL
} raw_t;
char *app_handle_line(const char *line, size_t len, raw_t *raw); // one line in, its answer out (free() it)
void app_answered(void); // the channel sent an answer: a new image on trial has proven its channel

// usb_kbd.c: the USB device (the descriptors of both interfaces) and the keyboard
void usb_start(void);
bool usb_configured(void);
bool usb_suspended(void);
uint32_t usb_last_configured_ms(void);
const char *usb_wake(void);                                  // wake a sleeping host; NULL sent, else why not
const char *kbd_tap(const uint8_t report[8]);                // down (with what is held), 30 ms, back to what is held
const char *kbd_hold(const uint8_t report[8], bool press);   // add to or take from what is held; release NULL: all
void kbd_state(cJSON *s);

// usb_msc.c: the seed drive
void seed_init(void);
size_t seed_room(int n); // the bytes n files may hold together (each file can waste most of a cluster)
const char *seed_wipe(uint32_t hold_ms);
// a seed of n files (seedrules checked them): the volume taken from the host, the old files gone; the bytes follow
// through seed_chunk, then seed_end checks every file read back against its SHA-256 and gives the volume back.
// Whatever goes wrong after seed_begin leaves the volume wiped (EMPTY), never half a seed
const char *seed_begin(const seed_file_t *files, int n, uint32_t hold_ms);
const char *seed_chunk(const uint8_t *p, size_t n);
const char *seed_end(const char *broke);
bool seed_busy(void);
void seed_state(cJSON *s);

// update.c: a new image into the other slot (esp_ota), checked against the command's SHA-256, then booted
const char *update_begin(uint32_t size, const char *sha256);
const char *update_chunk(const uint8_t *p, size_t n);
const char *update_end(const char *broke); // NULL: the other slot boots at the next restart
void update_trial_start(void); // a new image on trial: proves itself within 120 s, or the old one comes back
void update_answered(void);    // a command was answered over the channel (one of the two proofs)
void update_state(cJSON *s);
bool update_running(void);

// led.c: identify; NULL when the board's LED blinks, else why not ("no led")
const char *led_identify(void);

// app.c, for the knob: the idle picture again; a key armed to stay; what is armed or being sent taken away (true:
// there was something); a key or combination now. The const char *: NULL done, else why not
void app_ui_refresh(void);
const char *app_ui_arm(const char *name);
bool app_ui_disarm(void);
const char *app_ui_tap(const char *name);
// what the knob shows at entry at, copied out (the menu can change under the knob: the command menu); how many
// entries there are
int app_menu_view(int at, menu_view_t *out);

// battery.c: the battery's voltage (none: "battery": null)
void battery_init(void);
void battery_state(cJSON *s);

// knob.c: the board's knob picks a key to arm or a combination to send (none: nothing)
void knob_start(void);
bool knob_present(void);
bool knob_browsing(void); // its menu has the screen

// screen.c: one line large and two small lines below, drawn in a task of its own, again at every call (none: nothing)
void screen_show(const char *name, const char *line2, const char *line3);

// wifi.c: the network at the unit's fixed address, reconnecting with back-off
void wifi_start(const settings_t *s);
void wifi_state(cJSON *st);
bool wifi_ever_joined(void);

// channel.c: TLS 1.3 PSK on TCP 5577, one connection at a time, the lockout
void channel_start(const settings_t *s);
void channel_state(cJSON *st);
