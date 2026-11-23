// Wi-Fi: joins the network named in the settings at the unit's fixed address (no DHCP). Every drop is followed by
// a new try after a back-off, 1 s doubling to 60 s, back to 1 s once joined. Nothing here waits: the tries run from
// a timer, so USB (started before) never waits for the network.
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sat.h"

#define BACKOFF_FIRST_MS 1000
#define BACKOFF_MAX_MS 60000

static esp_timer_handle_t retry;
static uint32_t backoff_ms = BACKOFF_FIRST_MS;
static volatile uint32_t joined_ms, drops;
static volatile bool joined;
static uint8_t last_reason;

static void try_again(void *arg) { esp_wifi_connect(); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // a fixed address: up as soon as the link is
        joined = true;
        backoff_ms = BACKOFF_FIRST_MS;
        if (!joined_ms) joined_ms = now_ms();
        ev("wifi joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        last_reason = d->reason;
        if (joined) ev("wifi dropped");
        joined = false;
        drops++;
        esp_timer_stop(retry);
        esp_timer_start_once(retry, (uint64_t)backoff_ms * 1000);
        backoff_ms = backoff_ms >= BACKOFF_MAX_MS / 2 ? BACKOFF_MAX_MS : backoff_ms * 2;
    }
}

void wifi_start(const settings_t *s)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_dhcpc_stop(sta);
    esp_netif_ip_info_t ip = {
        .ip.addr = ESP_IP4TOADDR(s->ip[0], s->ip[1], s->ip[2], s->ip[3]),
        .netmask.addr = ESP_IP4TOADDR(s->mask[0], s->mask[1], s->mask[2], s->mask[3]),
        .gw.addr = ESP_IP4TOADDR(s->gw[0], s->gw[1], s->gw[2], s->gw[3]),
    };
    esp_netif_set_ip_info(sta, &ip);

    const esp_timer_create_args_t ta = {.callback = try_again, .name = "wifi retry"};
    esp_timer_create(&ta, &retry);
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wic);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s->wifi_pass, sizeof(wc.sta.password));
    esp_wifi_set_storage(WIFI_STORAGE_RAM); // the settings stay in our own namespace only
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
}

bool wifi_ever_joined(void) { return joined_ms != 0; }

void wifi_state(cJSON *st)
{
    cJSON *w = cJSON_AddObjectToObject(st, "wifi");
    cJSON_AddBoolToObject(w, "joined", joined);
    cJSON_AddNumberToObject(w, "first_joined_ms", joined_ms);
    cJSON_AddNumberToObject(w, "drops", drops);
    cJSON_AddNumberToObject(w, "last_reason", last_reason);
}
