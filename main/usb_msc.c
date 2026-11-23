// The seed drive: a 1 MB FAT volume labelled CIDATA (EMPTY while it holds no file) in the "seed" flash partition (wear levelling, 512-byte
// sectors), shown to the host as a read-only USB drive (for example cloud-init's NoCloud source).
// esp_tinyusb hands the volume to the host when the host configures the device, and back to the firmware when it
// is unconfigured; while the firmware has it mounted, the host's TEST UNIT READY gets "medium not present". A seed
// command uses exactly that to take it away while writing, and the medium comes back as a media change (UNIT
// ATTENTION, below). The files' bytes arrive after the command's line (seed_begin, seed_chunk, seed_end) and go
// straight onto the volume: a seed can fill it, and one that breaks off or reads back wrong leaves it wiped. What
// may be written is seedrules.c's to say.
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
static volatile bool changed; // the medium came back: the host's next TEST UNIT READY gets UNIT ATTENTION once
static uint32_t written_ms;
static size_t capacity, cluster; // the volume's data area and its cluster, in bytes
static SemaphoreHandle_t lock;

bool tud_msc_is_writable_cb(uint8_t lun) { return false; }

// esp_tinyusb answers TEST UNIT READY: "medium not present" while the firmware has the volume, ready once the host
// has it. Between the two a real drive says UNIT ATTENTION, NOT READY TO READY CHANGE (28h/00h) once, so a running
// OS reads the medium afresh instead of trusting what it cached. Linked in by -Wl,--wrap (main/CMakeLists.txt):
// TinyUSB's calls come here, __real_ is esp_tinyusb's own
bool __real_tud_msc_test_unit_ready_cb(uint8_t lun);
bool __wrap_tud_msc_test_unit_ready_cb(uint8_t lun)
{
    bool ready = __real_tud_msc_test_unit_ready_cb(lun);
    if (ready && changed) {
        changed = false;
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
        return false;
    }
    return ready;
}

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

// the label for the files there now (seedrules_label); after scan()
static void label(void)
{
    char l[24] = "", want[12] = "0:";
    strlcat(want, seedrules_label(nfiles), sizeof(want));
    f_getlabel("0:", l, NULL);
    if (strcmp(l, want + 2)) f_setlabel(want);
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
    measure();
    scan(); // a new volume stays empty: files come only from a seed command, and a wipe survives a restart
    label();
    state = "ready";
    // left mounted here: USB is not up yet; esp_tinyusb unmounts it for the host when the host configures
}

// remove every file not in the set to keep (keep NULL: all of them)
static void remove_files(const seed_file_t *keep, int n)
{
    DIR *d = opendir(SEED_PATH);
    if (!d) return;
    struct dirent *e;
    char path[sizeof(SEED_PATH) + 300];
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR) continue;
        bool k = false;
        for (int i = 0; i < n; i++)
            if (!strcasecmp(e->d_name, keep[i].name)) k = true;
        if (!k) {
            snprintf(path, sizeof(path), SEED_PATH "/%s", e->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

// the files as they are now listed, labelled, and the volume handed back; the host's next TEST UNIT READY gets
// one UNIT ATTENTION (28h/00h, not ready to ready change), then the medium
static void give_back(const char *err)
{
    scan();
    label();
    written_ms = now_ms();
    vTaskDelay(pdMS_TO_TICKS(200));
    changed = true;
    tinyusb_msc_storage_unmount();
    ev(err ? "seed: failed, wiped, medium back to host" : "seed: files written, medium back to host");
    state = err ? "write failed, wiped" : "ready";
    busy = false;
    xSemaphoreGive(lock);
}

// take the volume from the host and hold it away for hold_ms, so the host's polling sees "not present" first
static const char *take_away(uint32_t hold_ms)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    busy = true;
    if (take() != ESP_OK) {
        state = "could not take the volume from the host";
        busy = false;
        xSemaphoreGive(lock);
        return "seed: could not take the volume from the host";
    }
    ev("seed: medium taken from host");
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    return NULL;
}

const char *seed_wipe(uint32_t hold_ms)
{
    const char *err = take_away(hold_ms);
    if (err) return err;
    remove_files(NULL, 0);
    give_back(NULL);
    return NULL;
}

// the seed being received: its files, where the bytes are, the file open now
static struct {
    seed_file_t files[SEED_MAX_FILES];
    char names[SEED_MAX_FILES][SEED_NAME_MAX + 1], sha[SEED_MAX_FILES][65];
    int n, open_i;
    size_t pos;
    FILE *f;
} rx;

const char *seed_begin(const seed_file_t *nf, int n, uint32_t hold_ms)
{
    const char *err = take_away(hold_ms);
    if (err) return err;
    remove_files(NULL, 0); // all of them: whatever happens next, no old file passes for part of the new seed
    memset(&rx, 0, sizeof(rx));
    rx.n = n;
    rx.open_i = -1;
    for (int i = 0; i < n; i++) {
        strlcpy(rx.names[i], nf[i].name, sizeof(rx.names[0]));
        strlcpy(rx.sha[i], nf[i].sha256, sizeof(rx.sha[0]));
        rx.files[i] = (seed_file_t){rx.names[i], nf[i].size, rx.sha[i]};
    }
    return NULL;
}

static bool open_file(int i)
{
    char path[sizeof(SEED_PATH) + SEED_NAME_MAX + 2];
    if (rx.f) fclose(rx.f);
    snprintf(path, sizeof(path), SEED_PATH "/%s", rx.names[i]);
    rx.f = fopen(path, "wb");
    rx.open_i = i;
    return rx.f != NULL;
}

const char *seed_chunk(const uint8_t *p, size_t n)
{
    static char msg[SEED_NAME_MAX + 48];
    while (n) {
        int i;
        size_t left;
        if (seedrules_at(rx.files, rx.n, rx.pos, &i, &left)) return "seed: more bytes than the files' sizes";
        size_t k = n < left ? n : left;
        if ((i != rx.open_i && !open_file(i)) || fwrite(p, 1, k, rx.f) != k) {
            snprintf(msg, sizeof(msg), "seed: could not write \"%s\"", rx.names[i]);
            return msg;
        }
        p += k, n -= k, rx.pos += k;
    }
    return NULL;
}

const char *seed_end(const char *broke)
{
    static char msg[SEED_NAME_MAX + 80];
    const char *err = broke;
    if (rx.f && fclose(rx.f) && !err) err = "seed: could not write a file (fclose)";
    rx.f = NULL;
    if (broke) {
        snprintf(msg, sizeof(msg), "seed %s; the seed drive is wiped", broke);
        err = msg;
    }
    for (int i = 0; i < rx.n && !err; i++) { // the empty files, which no byte opened
        if (rx.files[i].size) continue;
        if (!open_file(i) || fclose(rx.f)) err = "seed: could not write an empty file";
        rx.f = NULL;
    }
    if (!err) { // every file read back from the volume against the SHA-256 the client sent
        scan();
        for (int i = 0; i < rx.n && !err; i++) {
            int j = 0;
            while (j < nfiles && strcasecmp(files[j].name, rx.names[i])) j++;
            if (j == nfiles || files[j].size != rx.files[i].size || strcmp(files[j].sha256, rx.sha[i])) {
                snprintf(msg, sizeof(msg), "seed: \"%s\" read back is not what was sent; the seed drive is wiped",
                         rx.names[i]);
                err = msg;
            }
        }
    }
    if (err) remove_files(NULL, 0);
    give_back(err);
    return err;
}

bool seed_busy(void) { return busy; }

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
