// The channel: TLS 1.3 with the unit's pre-shared key and an ephemeral (EC)DHE exchange, on TCP 5577. The PSK
// identity is the unit's name. One connection at a time; inside it, one JSON object per line each way (proto.h).
// A failed handshake counts towards the lockout (lockout.h): while deaf, a connection is closed unread.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"
#include "lockout.h"
#include "proto.h"
#include "sat.h"

#define PORT "5577"
#define HANDSHAKE_MS 5000 // a handshake that stalls longer is a failed one
#define IDLE_MS 60000     // a connection with no line for this long is closed
#define RAW_STALL_MS 10000 // raw bytes (seed, update) that stop for this long: the transfer is broken off
#define BUF_LEN (PROTO_LINE_MAX + 2) // a whole line, its newline and a NUL

static const settings_t *me;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_ssl_config conf;
static lockout_t lock; // only the server task writes it; status reads a copy
static portMUX_TYPE lock_mux = portMUX_INITIALIZER_UNLOCKED;

// only the unit's own name has a key; any other identity fails the handshake
static int psk_cb(void *arg, mbedtls_ssl_context *ssl, const unsigned char *id, size_t id_len)
{
    if (id_len != strlen(me->name) || memcmp(id, me->name, id_len)) return -1;
    return mbedtls_ssl_set_hs_psk(ssl, me->key, sizeof(me->key));
}

static bool send_all(mbedtls_ssl_context *ssl, const char *p, size_t n)
{
    while (n) {
        int r = mbedtls_ssl_write(ssl, (const unsigned char *)p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return false;
        p += r, n -= r;
    }
    return true;
}

static bool send_line(mbedtls_ssl_context *ssl, char *out)
{
    bool sent = out && send_all(ssl, out, strlen(out)) && send_all(ssl, "\n", 1);
    free(out);
    if (sent) app_answered();
    return sent;
}

// the raw bytes after a seed's or an update's line: the have bytes already in buf first, then the rest. The final
// answer is sent; false when the connection is done with (broken, or the bytes stopped)
static bool take_raw(mbedtls_ssl_context *ssl, char *buf, size_t have, const raw_t *raw)
{
    static char why[64];
    uint32_t got = 0;
    const char *err = NULL, *broke = NULL;
    mbedtls_ssl_conf_read_timeout(&conf, RAW_STALL_MS);
    while (got < raw->size && !err) {
        int n = have;
        if (!n) {
            uint32_t want = raw->size - got;
            n = mbedtls_ssl_read(ssl, (unsigned char *)buf, want < BUF_LEN - 1 ? want : BUF_LEN - 1);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
                n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
                continue;
            if (n <= 0) {
                snprintf(why, sizeof(why), "broke off after %lu of %lu bytes", (unsigned long)got,
                         (unsigned long)raw->size);
                broke = why;
                break;
            }
        }
        have = 0;
        err = raw->chunk((const uint8_t *)buf, n);
        got += n;
    }
    mbedtls_ssl_conf_read_timeout(&conf, IDLE_MS);
    // a chunk's error is the answer; a broken transfer is cleaned up, its answer most likely lost
    char *out = raw->end(err ? err : broke);
    return send_line(ssl, out) && !err && !broke;
}

static void serve(mbedtls_ssl_context *ssl, char *buf)
{
    size_t len = 0;
    mbedtls_ssl_conf_read_timeout(&conf, IDLE_MS);
    for (;;) {
        int n = mbedtls_ssl_read(ssl, (unsigned char *)buf + len, BUF_LEN - 1 - len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;
        if (n <= 0) return; // closed, timed out or broken
        len += n;
        buf[len] = 0;
        char *nl;
        while ((nl = memchr(buf, '\n', len))) {
            raw_t raw = {0};
            char *out = app_handle_line(buf, nl - buf, &raw);
            len -= (nl + 1 - buf);
            memmove(buf, nl + 1, len + 1);
            if (!send_line(ssl, out)) {
                if (raw.size) free(raw.end("the client went before its bytes"));
                return;
            }
            if (raw.size) { // the raw bytes the line named; nothing else is read in between
                size_t have = len < raw.size ? len : raw.size;
                // take_raw reads into buf: whatever came after the raw bytes, before the final answer, is dropped
                // (a client waits for that answer)
                if (!take_raw(ssl, buf, have, &raw)) return;
                len = 0;
                buf[0] = 0;
            }
        }
        if (len >= BUF_LEN - 1) { // no newline in more than a line's bytes: say so instead of a silent close
            send_all(ssl, PROTO_LINE_TOO_LONG "\n", sizeof(PROTO_LINE_TOO_LONG "\n") - 1);
            return;
        }
    }
}

static void one(mbedtls_net_context *client, char *buf)
{
    if (lockout_deaf(&lock, now_ms())) return; // closed unread
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_conf_read_timeout(&conf, HANDSHAKE_MS);
    int r = mbedtls_ssl_setup(&ssl, &conf);
    if (!r) {
        mbedtls_ssl_set_bio(&ssl, client, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);
        while ((r = mbedtls_ssl_handshake(&ssl)) == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            ;
        taskENTER_CRITICAL(&lock_mux);
        if (r) lockout_fail(&lock, now_ms());
        else lockout_ok(&lock);
        taskEXIT_CRITICAL(&lock_mux);
        if (r) ev(lockout_deaf(&lock, now_ms()) ? "handshake failed, deaf" : "handshake failed");
        else {
            serve(&ssl, buf);
            mbedtls_ssl_close_notify(&ssl);
        }
    }
    mbedtls_ssl_free(&ssl);
}

static void server_task(void *arg)
{
    char *buf = malloc(BUF_LEN);
    mbedtls_net_context listen, client;
    mbedtls_net_init(&listen);
    while (!buf || mbedtls_net_bind(&listen, NULL, PORT, MBEDTLS_NET_PROTO_TCP)) {
        ev("channel: cannot listen"); // no memory or no socket: the watchdog of a stuck unit is a restart
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!buf) buf = malloc(BUF_LEN);
    }
    for (;;) {
        mbedtls_net_init(&client);
        if (mbedtls_net_accept(&listen, &client, NULL, 0, NULL) == 0) one(&client, buf);
        mbedtls_net_free(&client);
    }
}

void channel_start(const settings_t *s)
{
    me = s;
    psa_crypto_init();
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"wiscosat", 8);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    // TLS 1.3 only, and only PSK with (EC)DHE: the session keys never come from the key alone
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_tls13_key_exchange_modes(&conf, MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL);
    mbedtls_ssl_conf_psk_cb(&conf, psk_cb, NULL);
    xTaskCreate(server_task, "channel", 12288, NULL, 5, NULL);
}

void channel_state(cJSON *st)
{
    taskENTER_CRITICAL(&lock_mux);
    lockout_t l = lock;
    taskEXIT_CRITICAL(&lock_mux);
    cJSON *c = cJSON_AddObjectToObject(st, "channel");
    cJSON_AddNumberToObject(c, "failed_handshakes", l.failed);
    // the last time it was deaf, also once it hears again (a status after it shows when it was): 0 never
    cJSON_AddNumberToObject(c, "deaf_until_ms", l.deaf_until_ms);
    cJSON_AddBoolToObject(c, "deaf", lockout_deaf(&l, now_ms()));
}
