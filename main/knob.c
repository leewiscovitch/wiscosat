// The board's knob (Kconfig SAT_KNOB: the LilyGO T-Embed's rotary encoder with its push button, or none). Turning
// it walks a list on the screen; a press acts on the entry shown:
//   a key to arm (F1 to F12, Delete, Enter, Esc): sent by the unit itself from the next start of the computer it is
//     plugged into, every 100 ms for 60 s, and at every start after that, until a press takes it away. Nothing is
//     sent into what is running now
//   a combination to send (ctrl-alt-del, ...): once, now. The one that restarts a machine with no question
//     (Alt-SysRq s, u, b) wants the knob held for 2 s
// The list is a text (menu.h): the built-in one, or the one the command menu gave the unit.
// With no menu on the screen a press takes away what is armed or being sent, and does nothing else: a bump in a bag
// arms and sends nothing. The menu goes after 10 s untouched. For a unit used by itself, away from its network.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sat.h"

#if CONFIG_SAT_KNOB_ENCODER
#include "driver/gpio.h"
#include "esp_timer.h"

#define MENU_IDLE_MS 10000 // untouched this long: the menu goes
#define DEBOUNCE_MS 30
#define LONG_MS 2000
#define SHOWN_MS 1500     // how long "Sent" or why not stays

// read every millisecond in the timer's task
static volatile int turned;       // detents not yet taken by the task (+ clockwise)
static volatile uint32_t down_ms; // how long the button has been down; 0 up
static volatile bool browsing;

static void sample(void *arg)
{
    // a quadrature step: old state << 2 | new state, the states A << 1 | B
    static const int8_t step[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    static uint8_t last = 3;
    static int8_t moved;
    uint8_t now = gpio_get_level(CONFIG_SAT_KNOB_A) << 1 | gpio_get_level(CONFIG_SAT_KNOB_B);
    if (now != last) {
        moved += step[last << 2 | now];
        last = now;
        if (now == 3) { // at rest again: a detent when it came the whole way round, nothing when it bounced back
#if CONFIG_SAT_KNOB_REVERSE
            if (moved >= 2) turned--;
            else if (moved <= -2) turned++;
#else
            if (moved >= 2) turned++;
            else if (moved <= -2) turned--;
#endif
            moved = 0;
        }
    }
    if (gpio_get_level(CONFIG_SAT_KNOB_BUTTON)) down_ms = 0;
    else if (down_ms < 60000) down_ms++;
}

static void show(const menu_entry_t *e, int i, int n)
{
    static const char *const how[] = {"press to arm", "press to send", "hold 2 s to send"};
    char pos[24];
    snprintf(pos, sizeof(pos), "%d of %d", i + 1, n);
    screen_show(e->label, how[e->kind], pos);
}

static void send(const menu_entry_t *e)
{
    const char *err = NULL;
    menu_step_t s;
    screen_show("Sending", e->label, "");
    for (int i = 0; !err && menu_step(e->keys, i, &s) == 1; i++) {
        if (s.wait_ms) vTaskDelay(pdMS_TO_TICKS(s.wait_ms));
        if (s.wait) continue;
        err = app_ui_tap(s.name);
        // Alt-SysRq b restarts the host before it takes the key-up: that key's error is the success
        size_t n = strlen(s.name);
        if (err && n >= 7 && !strcmp(s.name + n - 7, "sysrq-b")) err = NULL;
    }
    screen_show(err ? "Not sent" : "Sent", e->label, err ? err : "");
    vTaskDelay(pdMS_TO_TICKS(SHOWN_MS));
}

static void knob_task(void *arg)
{
    static menu_entry_t e; // the entry shown, copied out of the menu
    int at = 1;            // where the menu opens the first time (F2 in the built-in list); then where it was left
    uint32_t touched = 0;
    bool was_down = false, acted = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int t = turned;
        if (t) turned -= t;
        uint32_t d = down_ms;
        bool down = d >= DEBOUNCE_MS, pressed = down && !was_down;
        if (!down) acted = false;
        was_down = down;

        if (!browsing) {
            if (pressed) app_ui_disarm(); // what is armed or being sent goes; with nothing there, nothing happens
            else if (t) { // the first click opens the menu where it was
                int n = app_menu_entry(-1, NULL);
                if (!n) continue;
                if (at >= n) at = 0;
                app_menu_entry(at, &e);
                browsing = true;
                touched = now_ms();
                show(&e, at, n);
            }
            continue;
        }
        int n = app_menu_entry(-1, NULL); // it can change under the knob (the command menu)
        if (!n) {
            browsing = false;
            app_ui_refresh();
            continue;
        }
        if (t || at >= n) {
            at = at >= n ? 0 : ((at + t) % n + n) % n;
            app_menu_entry(at, &e);
            touched = now_ms();
            show(&e, at, n);
        }
        bool go = e.kind == MENU_HOLD ? down && d >= LONG_MS && !acted : pressed;
        if (go) {
            acted = true;
            if (e.kind == MENU_ARM) {
                menu_step_t s;
                const char *err = menu_step(e.keys, 0, &s) == 1 ? app_ui_arm(s.name) : "menu: no key";
                if (err) ev("knob: the key was not armed");
                browsing = false;
                app_ui_refresh();
                continue;
            }
            send(&e);
            turned = 0; // what was turned while it sent is not meant
            show(&e, at, n);
            touched = now_ms();
        } else if (down) touched = now_ms();
        if (now_ms() - touched >= MENU_IDLE_MS) {
            browsing = false;
            app_ui_refresh();
        }
    }
}

void knob_start(void)
{
    const gpio_config_t in = {.pin_bit_mask = 1ULL << CONFIG_SAT_KNOB_A | 1ULL << CONFIG_SAT_KNOB_B |
                                              1ULL << CONFIG_SAT_KNOB_BUTTON,
                              .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = sample, .name = "knob"};
    if (gpio_config(&in) != ESP_OK || esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, 1000) != ESP_OK ||
        xTaskCreate(knob_task, "knob", 4096, NULL, 3, NULL) != pdPASS)
        ev("knob: not started");
}

bool knob_present(void) { return true; }
bool knob_browsing(void) { return browsing; }
#else
void knob_start(void) {}
bool knob_present(void) { return false; }
bool knob_browsing(void) { return false; }
#endif
