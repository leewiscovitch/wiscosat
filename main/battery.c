// The battery's voltage, for status. Which battery is a build choice (Kconfig SAT_BATTERY): a divider from the
// battery to an ADC pin (the LilyGO T-Embed: GPIO 4, two 100k, so the pin sees half; behind the switch that GPIO 46
// keeps on, app.c), or none. Read when status asks: 16 samples, calibrated by ESP-IDF's curve fitting, averaged.
// On the T-Embed the pin reads the supply rail: USB's 5 V through a diode while a cable is in, the cell once it is out,
// so the reading also says whether the unit runs on USB power (powerrules.h): the one way to tell a pulled cable from
// a host that is asleep (both look the same on the bus; no pin sees the cable's 5 V). The cell's own voltage is
// therefore read only with the cable out. No battery is needed: with none, the cable's USB is read. Not a charge in
// percent: a Li-ion's voltage says that only roughly.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "sat.h"
#include "powerrules.h"

#if CONFIG_SAT_BATTERY_ADC
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#define SAMPLES 16

static adc_oneshot_unit_handle_t unit;
static adc_cali_handle_t cali;
static adc_channel_t chan;
static SemaphoreHandle_t lock;
static const char *fault; // a fixed string: why there is no reading

void battery_init(void)
{
    adc_unit_t u;
    lock = xSemaphoreCreateMutex();
    if (adc_oneshot_io_to_channel(CONFIG_SAT_BATTERY_GPIO, &u, &chan) != ESP_OK) {
        fault = "the battery's pin is no ADC pin";
    } else {
        const adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = u};
        const adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        const adc_cali_curve_fitting_config_t kcfg = {
            .unit_id = u, .chan = chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        if (adc_oneshot_new_unit(&ucfg, &unit) != ESP_OK || adc_oneshot_config_channel(unit, chan, &ccfg) != ESP_OK)
            fault = "the battery's ADC did not start";
        else if (adc_cali_create_scheme_curve_fitting(&kcfg, &cali) != ESP_OK)
            fault = "the battery's ADC has no calibration";
    }
    if (fault) ev(fault);
}

void battery_state(cJSON *s)
{
    cJSON *b = cJSON_AddObjectToObject(s, "battery");
    if (fault) {
        cJSON_AddStringToObject(b, "fault", fault);
        return;
    }
    int sum = 0, n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(unit, chan, &raw) == ESP_OK && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
            sum += mv;
            n++;
        }
    }
    xSemaphoreGive(lock);
    if (!n) {
        cJSON_AddStringToObject(b, "fault", "the battery's ADC did not answer");
        return;
    }
    int mv = sum / n * CONFIG_SAT_BATTERY_DIVIDER;
    cJSON_AddNumberToObject(b, "mv", mv);
    // a board with no battery on the cable reads USB (usb_power true); a pin wired to nothing reads under the floor
    switch (powerrules_source(mv, CONFIG_SAT_BATTERY_USB_MV, CONFIG_SAT_BATTERY_FLOOR_MV)) {
    case POWER_USB: cJSON_AddBoolToObject(b, "usb_power", true); break;
    case POWER_BATTERY: cJSON_AddBoolToObject(b, "usb_power", false); break;
    default:
        cJSON_AddNullToObject(b, "usb_power");
        cJSON_AddStringToObject(b, "fault", "no supply on the battery pin (a board without one?)");
    }
}
#else
void battery_init(void) {}
void battery_state(cJSON *s) { cJSON_AddNullToObject(s, "battery"); }
#endif
