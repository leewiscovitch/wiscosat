// The board's screen shows the unit's name, large, and its address and version below, so a person tells units
// apart at a glance. Which screen is a build choice (Kconfig SAT_SCREEN): an ST7789 on SPI (the LilyGO T-Embed's,
// 320x170, on the rail GPIO 46 keeps on, app.c), or none. Drawn in a task of its own at a low priority, after USB has
// started: the keyboard never waits for the screen. screen_show may be called again at any time, from any task: the
// newest text replaces the picture (an armed key, the knob's menu; app.c, knob.c). Only a change is drawn, nothing
// ticks. The backlight is dim by default (SAT_SCREEN_BRIGHTNESS).
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sat.h"

#if CONFIG_SAT_SCREEN_ST7789
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "font5x7.h"

#define W CONFIG_SAT_SCREEN_WIDTH
#define H CONFIG_SAT_SCREEN_HEIGHT
#define STRIP 10           // rows sent at a time
#define BIG_MAX 6          // the name's largest scale: 30x42 pixels a letter
#define SMALL 2            // the address and version: 10x14
#define GAP_BIG_SMALL 14   // pixels between the name and the first small line
#define GAP_SMALL 6        // between the small lines
#define INK_BIG 0xFFFF     // white
#define INK_SMALL 0xA514   // light grey (RGB565)

typedef struct {
    char line[3][40];
} screen_text_t;

static uint8_t *canvas; // W*H, one byte a pixel: 0 none, 1 the name's ink, 2 the small lines'
static uint16_t *strip;
static SemaphoreHandle_t sent;
static esp_lcd_panel_handle_t panel;
static QueueHandle_t texts; // one place: the newest text wins

static int text_width(const char *s, int scale)
{
    int n = strlen(s);
    return n ? (n * 6 - 1) * scale : 0;
}

static void text(int x, int y, const char *s, int scale, uint8_t ink)
{
    for (; *s; s++, x += 6 * scale) {
        const font5x7_t *g = font5x7_glyph(*s);
        for (int r = 0; r < 7 * scale; r++) {
            for (int c = 0; c < 5 * scale; c++) {
                int px = x + c, py = y + r;
                if (px < 0 || px >= W || py < 0 || py >= H) continue;
                if (g->rows[r / scale][c / scale] == '#') canvas[py * W + px] = ink;
            }
        }
    }
}

static void layout(const screen_text_t *t)
{
    int big = BIG_MAX;
    while (big > 1 && text_width(t->line[0], big) > W - 8) big--;
    int smalls = (t->line[1][0] != 0) + (t->line[2][0] != 0);
    int height = 7 * big + smalls * (GAP_BIG_SMALL + 7 * SMALL) - (smalls == 2 ? GAP_BIG_SMALL - GAP_SMALL : 0);
    int y = (H - height) / 2;
    text((W - text_width(t->line[0], big)) / 2, y, t->line[0], big, 1);
    y += 7 * big + GAP_BIG_SMALL;
    for (int i = 1; i < 3; i++) {
        if (!t->line[i][0]) continue;
        text((W - text_width(t->line[i], SMALL)) / 2, y, t->line[i], SMALL, 2);
        y += 7 * SMALL + GAP_SMALL;
    }
}

static bool on_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *d, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sent, &woken);
    return woken == pdTRUE;
}

static void backlight(int percent)
{
    ledc_timer_config_t tc = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                              .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_channel_config_t cc = {.gpio_num = CONFIG_SAT_SCREEN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
                                .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = percent * 255 / 100};
    if (ledc_timer_config(&tc) != ESP_OK || ledc_channel_config(&cc) != ESP_OK) ev("screen: no backlight");
}

// once: the panel, dark until the first picture is whole (no garbage from its RAM)
static const char *init(void)
{
    gpio_set_direction(CONFIG_SAT_SCREEN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_SAT_SCREEN_BL, 0);

    canvas = malloc(W * H);
    strip = heap_caps_malloc(W * STRIP * 2, MALLOC_CAP_DMA);
    sent = xSemaphoreCreateBinary();
    if (!canvas || !strip || !sent) return "screen: no memory";

    spi_bus_config_t bus = {.mosi_io_num = CONFIG_SAT_SCREEN_MOSI, .sclk_io_num = CONFIG_SAT_SCREEN_SCLK,
                            .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
                            .max_transfer_sz = W * STRIP * 2};
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return "screen: no spi bus";
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = {.dc_gpio_num = CONFIG_SAT_SCREEN_DC, .cs_gpio_num = CONFIG_SAT_SCREEN_CS,
                                         .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
                                         .spi_mode = 0, .trans_queue_depth = 4, .on_color_trans_done = on_sent};
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io) != ESP_OK) return "screen: no io";
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = CONFIG_SAT_SCREEN_RST,
                                     .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    if (esp_lcd_new_panel_st7789(io, &pc, &panel) != ESP_OK) return "screen: no panel";
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true); // an IPS panel: without it, white is black
    esp_lcd_panel_swap_xy(panel, true);      // landscape
#if CONFIG_SAT_SCREEN_FLIP
    esp_lcd_panel_mirror(panel, true, false);
#else
    esp_lcd_panel_mirror(panel, false, true);
#endif
    esp_lcd_panel_set_gap(panel, 0, CONFIG_SAT_SCREEN_GAP);
    return NULL;
}

static const char *draw(const screen_text_t *t)
{
    memset(canvas, 0, W * H);
    layout(t);
    for (int y = 0; y < H; y += STRIP) {
        int rows = H - y < STRIP ? H - y : STRIP;
        for (int i = 0; i < W * rows; i++) {
            uint8_t ink = canvas[y * W + i];
            uint16_t c = ink == 1 ? INK_BIG : ink == 2 ? INK_SMALL : 0;
            strip[i] = (c >> 8) | (c << 8); // the panel takes the high byte first
        }
        if (esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + rows, strip) != ESP_OK) return "screen: draw failed";
        xSemaphoreTake(sent, portMAX_DELAY); // the strip is free again
    }
    return NULL;
}

static void screen_task(void *arg)
{
    screen_text_t t;
    const char *e = init();
    if (e) {
        ev(e);
        vTaskDelete(NULL);
    }
    for (bool lit = false;;) {
        xQueueReceive(texts, &t, portMAX_DELAY);
        if ((e = draw(&t))) ev(e);
        if (lit) continue;
        esp_lcd_panel_disp_on_off(panel, true);
        backlight(CONFIG_SAT_SCREEN_BRIGHTNESS);
        lit = true;
    }
}

// the first call (app_main's, before anything else draws) starts the task
void screen_show(const char *name, const char *line2, const char *line3)
{
    screen_text_t t = {0};
    strlcpy(t.line[0], name ? name : "", sizeof(t.line[0]));
    strlcpy(t.line[1], line2 ? line2 : "", sizeof(t.line[1]));
    strlcpy(t.line[2], line3 ? line3 : "", sizeof(t.line[2]));
    if (!texts) {
        texts = xQueueCreate(1, sizeof(screen_text_t));
        if (!texts) return ev("screen: no memory");
        if (xTaskCreate(screen_task, "screen", 4096, NULL, 2, NULL) != pdPASS) return ev("screen: no task");
    }
    xQueueOverwrite(texts, &t);
}
#else
void screen_show(const char *name, const char *line2, const char *line3)
{
}
#endif
