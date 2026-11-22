// The PSK spike (throwaway; removed again when the real channel is built): joins the Wi-Fi, listens on TCP 5577,
// takes TLS with a pre-shared key (identity SPIKE_NAME, key SPIKE_KEY_HEX), reads one line and answers one JSON
// line: the line back, the suite, the TLS version, the handshake's time on the unit and the free heap.
// Everything it learns goes to the console too (USB serial/JTAG), so a failing handshake can be read there.

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"
#include "spike_secrets.h"

#define PORT "5577"
static const char *TAG = "spike";

static unsigned char psk[32];
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_ssl_config conf;
static volatile bool have_ip;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Only SPIKE_NAME has a key; any other identity fails the handshake
static int psk_cb(void *arg, mbedtls_ssl_context *ssl, const unsigned char *id, size_t id_len)
{
    if (id_len != strlen(SPIKE_NAME) || memcmp(id, SPIKE_NAME, id_len) != 0) {
        ESP_LOGW(TAG, "unknown identity (%u bytes)", (unsigned)id_len);
        return -1;
    }
    return mbedtls_ssl_set_hs_psk(ssl, psk, sizeof(psk));
}

static void serve_one(mbedtls_net_context *client)
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    char err[96];
    int r = mbedtls_ssl_setup(&ssl, &conf);
    if (r != 0) goto out;
    mbedtls_ssl_set_bio(&ssl, client, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);

    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int64_t t0 = esp_timer_get_time();
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) break;
    }
    int64_t hs_us = esp_timer_get_time() - t0;
    if (r != 0) {
        mbedtls_strerror(r, err, sizeof(err));
        ESP_LOGW(TAG, "handshake failed: -0x%04x %s (%lld ms)", -r, err, hs_us / 1000);
        goto out;
    }
    const char *suite = mbedtls_ssl_get_ciphersuite(&ssl);
    const char *version = mbedtls_ssl_get_version(&ssl);

    char line[256];
    size_t n = 0;
    while (n < sizeof(line) - 1) {
        r = mbedtls_ssl_read(&ssl, (unsigned char *)line + n, sizeof(line) - 1 - n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_SSL_PROTO_TLS1_3
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
        if (r <= 0) break;
        n += r;
        if (memchr(line, '\n', n)) break;
    }
    line[n] = 0;
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    // the line is the client's own test text, plain ASCII with no quotes (client.py sends it so)
    for (char *p = line; *p; p++) if (*p == '"' || *p == '\\' || *p < 0x20) *p = '?';

    char answer[512];
    int len = snprintf(answer, sizeof(answer),
        "{\"ok\":true,\"echo\":\"%s\",\"suite\":\"%s\",\"version\":\"%s\",\"handshake_ms\":%lld,"
        "\"heap_free\":%u,\"heap_free_before\":%u,\"heap_min_ever\":%u,\"heap_largest\":%u}\n",
        line, suite, version, hs_us / 1000,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_before,
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "%s", answer);
    for (int off = 0; off < len;) {
        r = mbedtls_ssl_write(&ssl, (unsigned char *)answer + off, len - off);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) break;
        off += r;
    }
    mbedtls_ssl_close_notify(&ssl);
out:
    mbedtls_ssl_free(&ssl);
    mbedtls_net_free(client);
}

static void server_task(void *arg)
{
    mbedtls_net_context listen, client;
    mbedtls_net_init(&listen);
    while (!have_ip) vTaskDelay(pdMS_TO_TICKS(100));
    int r = mbedtls_net_bind(&listen, NULL, PORT, MBEDTLS_NET_PROTO_TCP);
    if (r != 0) {
        ESP_LOGE(TAG, "bind failed: -0x%04x", -r);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "listening on %s, free heap %u", PORT, (unsigned)esp_get_free_heap_size());
    for (;;) {
        mbedtls_net_init(&client);
        if (mbedtls_net_accept(&listen, &client, NULL, 0, NULL) != 0) continue;
        serve_one(&client);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        have_ip = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "address " IPSTR, IP2STR(&e->ip_info.ip));
        have_ip = true;
    }
}

void app_main(void)
{
    for (int i = 0; i < 32; i++)
        psk[i] = (unsigned char)(hexval(SPIKE_KEY_HEX[2 * i]) << 4 | hexval(SPIKE_KEY_HEX[2 * i + 1]));

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wic);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, SPIKE_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, SPIKE_WIFI_PASS, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);

    psa_crypto_init();
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)TAG, strlen(TAG));
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_read_timeout(&conf, 5000);
    mbedtls_ssl_conf_psk_cb(&conf, psk_cb, NULL);
#ifdef MBEDTLS_SSL_PROTO_TLS1_3
    // TLS 1.3: only PSK with (EC)DHE, so the session keys never come from the key alone (plan D2)
    mbedtls_ssl_conf_tls13_key_exchange_modes(&conf, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL);
#endif

    xTaskCreate(server_task, "server", 12288, NULL, 5, NULL);

    // the address again every 10 s: a serial monitor opened after the start still sees it
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_netif_ip_info_t ip;
        if (have_ip && esp_netif_get_ip_info(sta, &ip) == ESP_OK)
            ESP_LOGI(TAG, "address " IPSTR ", free heap %u, min ever %u", IP2STR(&ip.ip),
                     (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
        else
            ESP_LOGI(TAG, "no address yet");
    }
}
