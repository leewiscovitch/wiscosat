// identify: the board's LED blinks for 5 s, so a person finds the unit among others. Which LED is a build choice
// (Kconfig SAT_LED): the T-Embed's ring of seven APA102 LEDs (data GPIO 42, clock GPIO 45, on the rail that GPIO 46
// keeps on, app.c), driven bit by bit; or none, and identify answers "no led on this board".
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "sat.h"

#define BLINK_MS 250
#define BLINK_FOR_MS 5000

#if CONFIG_SAT_LED_APA102
#define PIN_DATA CONFIG_SAT_LED_APA102_DATA
#define PIN_CLOCK CONFIG_SAT_LED_APA102_CLOCK
#define NLED CONFIG_SAT_LED_APA102_COUNT

static void byte_out(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_DATA, (b >> i) & 1);
        gpio_set_level(PIN_CLOCK, 1);
        gpio_set_level(PIN_CLOCK, 0);
    }
}

// every LED one colour; level 0 is off
static void ring(uint8_t level)
{
    for (int i = 0; i < 4; i++) byte_out(0x00); // start frame
    for (int i = 0; i < NLED; i++) {
        byte_out(0xE0 | 4); // brightness 4 of 31: seen, not blinding
        byte_out(level), byte_out(level), byte_out(level); // blue, green, red: white
    }
    for (int i = 0; i < (NLED + 15) / 16 + 4; i++) byte_out(0xFF); // end frame: clocks for the last LEDs
}

static TaskHandle_t blinker;

static void blink_task(void *arg)
{
    for (int i = 0; i < BLINK_FOR_MS / BLINK_MS; i++) {
        ring(i % 2 ? 0 : 255);
        vTaskDelay(pdMS_TO_TICKS(BLINK_MS));
    }
    ring(0);
    blinker = NULL;
    vTaskDelete(NULL);
}

const char *led_identify(void)
{
    if (blinker) return NULL; // blinking already: the 5 s run on
    gpio_set_direction(PIN_DATA, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_CLOCK, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_CLOCK, 0);
    if (xTaskCreate(blink_task, "identify", 2048, NULL, 3, &blinker) != pdPASS) return "identify: no task";
    ev("identify: blinking 5 s");
    return NULL;
}
#else
const char *led_identify(void) { return "no led on this board"; }
#endif
