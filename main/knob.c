// The board's knob (Kconfig SAT_KNOB: the LilyGO T-Embed's rotary encoder with its push button, or none). Turning
// it walks a list on the screen; a press acts on the entry shown:
//   a key to arm (F1 to F12, Delete, Enter, Esc): sent by the unit itself from the next start of the computer it is
//     plugged into, every 100 ms for 60 s, and at every start after that, until a press takes it away. Nothing is
//     sent into what is running now
//   a combination to send (ctrl-alt-del, ...): once, now. The one that restarts a machine with no question
//     (Alt-SysRq s, u, b) wants the knob held for 2 s
// The list is a text (menu.h): the built-in one, or the one the command menu gave the unit.
// The text can hold several menus, each with a name ([Power]): a double press steps to the next one, turning stays
// inside one. A press therefore acts a third of a second late: it waits for a second one.
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
#define DOUBLE_MS 350 // two presses within this: a double press
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

static void show(const menu_view_t *v)
{
    static const char *const how[] = {"press to arm", "press to send", "hold 2 s to send"};
    char pos[40];
    snprintf(pos, sizeof(pos), "%s%s%d of %d", v->section, v->section[0] ? "  " : "", v->pos + 1, v->count);
    screen_show(v->e.label, how[v->e.kind], pos);
}

static void send(const menu_entry_t *e)
{
    const char *err = NULL;
    menu_step_t s;
    screen_show("Sending", e->label, "");
    for (int i = 0; !err && menu_step(e->keys, i, &s) == 1; i++) {
        if (s.wait_ms) vTaskDelay(pdMS_TO_TICKS(s.wait_ms));
        if (s.wait) continue;
        if (s.wake) {
            err = usb_wake();
            continue;
        }
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
    static menu_view_t v; // the entry shown, copied out of the menu
    int at = 1;           // where the menu opens the first time (F2 in the built-in list); then where it was left
    uint32_t touched = 0, press_ms = 0;
    bool was_down = false, acted = false, waiting = false; // waiting: a press that may yet become a double press
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int t = turned;
        if (t) turned -= t;
        uint32_t d = down_ms;
        bool down = d >= DEBOUNCE_MS, pressed = down && !was_down;
        if (!down) acted = false;
        was_down = down;

        // one press or two: a press waits DOUBLE_MS for a second one. two: the next menu. one: the entry's action
        bool once = false, twice = false;
        if (pressed && waiting && now_ms() - press_ms <= DOUBLE_MS) twice = true, waiting = false;
        else if (pressed) waiting = true, press_ms = now_ms();
        else if (waiting && now_ms() - press_ms > DOUBLE_MS) once = true, waiting = false;

        int n = app_menu_view(at, &v); // again each round: the menu can change under the knob (the command menu)
        at = n ? v.at : 0;
        if (!browsing) {
            if (once) app_ui_disarm(); // what is armed or being sent goes; with nothing there, nothing happens
            if (!n || (!t && !twice)) continue;
            if (twice) app_menu_view(at = v.next_first, &v); // a double press opens the next menu
            browsing = true; // a first click opens the menu where it was
            touched = now_ms();
            show(&v);
            continue;
        }
        if (!n) {
            browsing = false;
            app_ui_refresh();
            continue;
        }
        if (twice || t) {
            at = twice ? v.next_first : v.first + ((v.pos + t) % v.count + v.count) % v.count; // turning stays in its menu
            app_menu_view(at, &v);
            touched = now_ms();
            show(&v);
        }
        bool go = v.e.kind == MENU_HOLD ? down && d >= LONG_MS && !acted : once;
        if (go) {
            acted = true;
            waiting = false;
            if (v.e.kind == MENU_ARM) {
                menu_step_t s;
                const char *err = menu_step(v.e.keys, 0, &s) == 1 ? app_ui_arm(s.name) : "menu: no key";
                if (err) ev("knob: the key was not armed");
                browsing = false;
                app_ui_refresh();
                continue;
            }
            send(&v.e);
            turned = 0; // what was turned while it sent is not meant
            show(&v);
            touched = now_ms();
        } else if (down || waiting) touched = now_ms();
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
