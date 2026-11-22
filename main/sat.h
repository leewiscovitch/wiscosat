// How the device parts (usb_kbd.c, usb_msc.c, app.c) call each other. The pure parts have their own headers.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "seedrules.h"
#include "settings.h"

// app.c
uint32_t now_ms(void);
void ev(const char *what);     // a line in status's events; a fixed string, never typed text
void app_usb_configured(void); // the watcher's call when a host has configured the unit
char *app_handle_line(const char *line, size_t len); // one line in, the answer line out (free() it)

// usb_kbd.c: the USB device (the descriptors of both interfaces) and the keyboard
void usb_start(void);
bool usb_configured(void);
bool usb_suspended(void);
uint32_t usb_last_configured_ms(void);
const char *kbd_tap(const uint8_t report[8]);                // down, 30 ms, up; NULL when the host took both
const char *kbd_hold(const uint8_t report[8], bool press);   // add to or take from what is held; release NULL: all
void kbd_state(cJSON *s);

// usb_msc.c: the seed drive
void seed_init(void);
size_t seed_room(int n); // the bytes n files may hold together (each file can waste most of a cluster)
const char *seed_write(const seed_file_t *files, int n, uint32_t hold_ms); // n 0: wipe
void seed_state(cJSON *s);

// wifi.c: the network at the unit's fixed address, reconnecting with back-off
void wifi_start(const settings_t *s);
void wifi_state(cJSON *st);

// channel.c: TLS 1.3 PSK on TCP 5577, one connection at a time, the lockout
void channel_start(const settings_t *s);
void channel_state(cJSON *st);
