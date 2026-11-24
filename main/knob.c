// The board's knob (Kconfig SAT_KNOB: the LilyGO T-Embed's rotary encoder with its push button, or none). Turning
// it walks a list on the screen; a press acts on the entry shown:
//   a key to arm (F1 to F12, Delete, Enter, Esc): sent by the unit itself from the next start of the computer it is
//     plugged into, every 100 ms for 60 s, and at every start after that, until a press takes it away. Nothing is
//     sent into what is running now
//   a combination to send (ctrl-alt-del, ...): once, now. The one that restarts a machine with no question
//     (Alt-SysRq s, u, b) wants the knob held for 2 s
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
#define SYSRQ_GAP_MS 2000 // between s (sync), u (read-only) and b (restart)
#define SHOWN_MS 1500     // how long "Sent" or why not stays

typedef enum { ARM, SEND, SEND_LONG } kind_t;
static const struct {
    const char *label;
    kind_t kind;
    const char *keys[3];
} entries[] = {
    {"F1", ARM, {"f1"}},         {"F2", ARM, {"f2"}},       {"F3", ARM, {"f3"}},   {"F4", ARM, {"f4"}},
    {"F5", ARM, {"f5"}},         {"F6", ARM, {"f6"}},       {"F7", ARM, {"f7"}},   {"F8", ARM, {"f8"}},
    {"F9", ARM, {"f9"}},         {"F10", ARM, {"f10"}},     {"F11", ARM, {"f11"}}, {"F12", ARM, {"f12"}},
    {"Delete", ARM, {"delete"}}, {"Enter", ARM, {"enter"}}, {"Esc", ARM, {"esc"}},
    {"ctrl-alt-del", SEND, {"ctrl-alt-del"}},
    {"Restart SysRq", SEND_LONG, {"alt-sysrq-s", "alt-sysrq-u", "alt-sysrq-b"}},
    {"ctrl-alt-F2", SEND, {"ctrl-alt-f2"}},
};
#define NENTRIES ((int)(sizeof(entries) / sizeof(entries[0])))

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

static void show(int i)
{
    static const char *const how[] = {"press to arm", "press to send", "hold 2 s to send"};
    char pos[24];
    snprintf(pos, sizeof(pos), "%d of %d", i + 1, NENTRIES);
    screen_show(entries[i].label, how[entries[i].kind], pos);
}

static void send(int i)
{
    const char *err = NULL;
    screen_show("Sending", entries[i].label, "");
    for (int k = 0; k < 3 && entries[i].keys[k] && !err; k++) {
        if (k) vTaskDelay(pdMS_TO_TICKS(SYSRQ_GAP_MS));
        err = app_ui_tap(entries[i].keys[k]);
        // the restart's own key: the host is gone before it takes the key-up
        if (err && entries[i].kind == SEND_LONG && k == 2) err = NULL;
    }
    screen_show(err ? "Not sent" : "Sent", entries[i].label, err ? err : "");
    vTaskDelay(pdMS_TO_TICKS(SHOWN_MS));
}

static void knob_task(void *arg)
{
    int at = 1; // F2: where the menu opens the first time; after that where it was left
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
                browsing = true;
                touched = now_ms();
                show(at);
            }
            continue;
        }
        if (t) {
            at = ((at + t) % NENTRIES + NENTRIES) % NENTRIES;
            touched = now_ms();
            show(at);
        }
        bool go = entries[at].kind == SEND_LONG ? down && d >= LONG_MS && !acted : pressed;
        if (go) {
            acted = true;
            if (entries[at].kind == ARM) {
                const char *err = app_ui_arm(entries[at].keys[0]);
                if (err) ev(err);
                browsing = false;
                app_ui_refresh();
                continue;
            }
            send(at);
            turned = 0; // what was turned while it sent is not meant
            show(at);
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
