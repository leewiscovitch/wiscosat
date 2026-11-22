// The seed drive: a 1 MB FAT volume labelled CIDATA in the "seed" flash partition (wear levelling, 512-byte
// sectors), shown to the host as a read-only USB drive (for example cloud-init's NoCloud source).
// esp_tinyusb hands the volume to the host when the host configures the device, and back to the firmware when it
// is unconfigured; while the firmware has it mounted, the host's TEST UNIT READY gets "medium not present". A seed
// command uses exactly that to take it away while writing. What may be written is seedrules.c's to say.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "ff.h"
#include "sat.h"

#define SEED_PATH "/seed"

static struct {
    char name[SEED_NAME_MAX + 4];
    uint32_t size;
    char sha256[65];
} files[SEED_MAX_FILES];
static int nfiles;
static const char *state = "not started";
static volatile bool busy;
static uint32_t written_ms;
static size_t capacity, cluster; // the volume's data area and its cluster, in bytes
static SemaphoreHandle_t lock;

bool tud_msc_is_writable_cb(uint8_t lun) { return false; }

static bool sha_file(const char *path, uint32_t *size, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    uint8_t buf[256], d[32];
    size_t n;
    *size = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mbedtls_sha256_update(&c, buf, n);
        *size += n;
    }
    fclose(f);
    mbedtls_sha256_finish(&c, d);
    mbedtls_sha256_free(&c);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    return true;
}

// list the root of the mounted volume into files (the firmware must have it mounted)
static void scan(void)
{
    DIR *d = opendir(SEED_PATH);
    nfiles = 0;
    if (!d) return;
    struct dirent *e;
    char path[sizeof(SEED_PATH) + 300];
    while ((e = readdir(d)) && nfiles < SEED_MAX_FILES) {
        if (e->d_type == DT_DIR) continue;
        snprintf(path, sizeof(path), SEED_PATH "/%s", e->d_name);
        strlcpy(files[nfiles].name, e->d_name, sizeof(files[0].name));
        if (sha_file(path, &files[nfiles].size, files[nfiles].sha256)) nfiles++;
    }
    closedir(d);
}

// take the volume for the firmware at SEED_PATH (from then on the host sees "medium not present")
static esp_err_t take(void)
{
    if (!tinyusb_msc_storage_in_use_by_usb_host()) tinyusb_msc_storage_unmount(); // mounted elsewhere
    return tinyusb_msc_storage_mount(SEED_PATH);
}

static void label(void)
{
    char l[24] = "";
    f_getlabel("0:", l, NULL);
    if (strcmp(l, "CIDATA")) f_setlabel("0:CIDATA");
}

static void measure(void)
{
    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK) return;
#if FF_MAX_SS != FF_MIN_SS
    size_t sector = fs->ssize;
#else
    size_t sector = FF_MAX_SS;
#endif
    cluster = (size_t)fs->csize * sector;
    capacity = (size_t)(fs->n_fatent - 2) * cluster;
}

size_t seed_room(int n)
{
    size_t slack = (size_t)n * cluster;
    return capacity > slack ? capacity - slack : 0;
}

void seed_init(void)
{
    lock = xSemaphoreCreateMutex();
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "seed");
    wl_handle_t wl = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(wl_mount(p, &wl));
    const tinyusb_msc_spiflash_config_t cfg = {.wl_handle = wl};
    ESP_ERROR_CHECK(tinyusb_msc_storage_init_spiflash(&cfg));
    if (tinyusb_msc_storage_mount(SEED_PATH) != ESP_OK) { // formats an empty partition
        state = "mount failed";
        return;
    }
    label();
    measure();
    scan(); // a new volume stays empty: files come only from a seed command, and a wipe survives a restart
    state = "ready";
    // left mounted here: USB is not up yet; esp_tinyusb unmounts it for the host when the host configures
}

const char *seed_write(const seed_file_t *nf, int n, uint32_t hold_ms)
{
    static char msg[SEED_NAME_MAX + 48];
    xSemaphoreTake(lock, portMAX_DELAY);
    busy = true;
    const char *err = NULL;
    if (take() != ESP_OK) {
        err = "could not take the volume from the host";
        goto out;
    }
    ev("seed: medium taken from host");
    vTaskDelay(pdMS_TO_TICKS(hold_ms)); // let the host's polling see "not present" before anything changes
    // remove the files not in the new set (all of them for a wipe)
    DIR *d = opendir(SEED_PATH);
    if (d) {
        struct dirent *e;
        char path[sizeof(SEED_PATH) + 300];
        while ((e = readdir(d))) {
            if (e->d_type == DT_DIR) continue;
            bool keep = false;
            for (int i = 0; i < n; i++)
                if (!strcasecmp(e->d_name, nf[i].name)) keep = true;
            if (!keep) {
                snprintf(path, sizeof(path), SEED_PATH "/%s", e->d_name);
                unlink(path);
            }
        }
        closedir(d);
    }
    for (int i = 0; i < n && !err; i++) {
        char path[sizeof(SEED_PATH) + SEED_NAME_MAX + 2];
        snprintf(path, sizeof(path), SEED_PATH "/%s", nf[i].name);
        FILE *f = fopen(path, "wb");
        bool ok = f && fwrite(nf[i].data, 1, nf[i].size, f) == nf[i].size;
        if (f && fclose(f)) ok = false;
        if (!ok) {
            snprintf(msg, sizeof(msg), "could not write \"%s\"", nf[i].name);
            err = msg;
        }
    }
    label();
    scan();
    written_ms = now_ms();
    vTaskDelay(pdMS_TO_TICKS(200));
    tinyusb_msc_storage_unmount(); // the host's next TEST UNIT READY finds the medium again
    ev(err ? "seed: write failed, medium back to host" : "seed: files written, medium back to host");
out:
    state = err ? "write failed" : "ready";
    busy = false;
    xSemaphoreGive(lock);
    return err;
}

void seed_state(cJSON *s)
{
    cJSON *sd = cJSON_AddObjectToObject(s, "seed");
    cJSON_AddStringToObject(sd, "state", busy ? "writing (host sees no medium)" : state);
    cJSON_AddBoolToObject(sd, "host_has_medium", tinyusb_msc_storage_in_use_by_usb_host());
    cJSON_AddNumberToObject(sd, "written_ms", written_ms);
    cJSON_AddNumberToObject(sd, "capacity", capacity);
    cJSON *fl = cJSON_AddArrayToObject(sd, "files");
    for (int i = 0; i < nfiles; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", files[i].name);
        cJSON_AddNumberToObject(f, "size", files[i].size);
        cJSON_AddStringToObject(f, "sha256", files[i].sha256);
        cJSON_AddItemToArray(fl, f);
    }
}
