// ESP-IDF v5.5.6's wear levelling reads and writes wrongly over a flash sector's boundary: it keeps one spare flash
// sector that moves through the partition, so two neighbouring logical sectors are not always neighbours in the
// flash, but WL_Flash::write and ::read work out where a piece starts and then run straight on. A piece that starts
// inside one 4096-byte sector and ends in the next lands its end in the spare sector whenever the spare sits right
// there (and a read takes its end from it). FAT with 512-byte sectors does exactly that: the seed volume's clusters
// start one sector into a flash sector, so a whole cluster written at once crosses a boundary. Seen as a seed that
// read back with a cluster's last 512 bytes all FFh, about once in 19 (README "The seed").
// So every wl_read and wl_write is cut here into pieces that stay inside one flash sector. Linked in by
// -Wl,--wrap (main/CMakeLists.txt): FatFs's and esp_tinyusb's calls come here, __real_ is ESP-IDF's own.
// ESP-IDF's master cuts the piece itself; once the pinned version does, this file and its two --wrap can go.
#include "wear_levelling.h"
#include "wlsplit.h"

#define FLASH_SECTOR 4096 // the flash's erase size, wear levelling's page

esp_err_t __real_wl_write(wl_handle_t handle, size_t dest_addr, const void *src, size_t size);
esp_err_t __real_wl_read(wl_handle_t handle, size_t src_addr, void *dest, size_t size);

esp_err_t __wrap_wl_write(wl_handle_t handle, size_t dest_addr, const void *src, size_t size)
{
    if (!size) return __real_wl_write(handle, dest_addr, src, size);
    const uint8_t *p = src;
    while (size) {
        size_t k = wlsplit_first(dest_addr, size, FLASH_SECTOR);
        esp_err_t err = __real_wl_write(handle, dest_addr, p, k);
        if (err != ESP_OK) return err;
        dest_addr += k, p += k, size -= k;
    }
    return ESP_OK;
}

esp_err_t __wrap_wl_read(wl_handle_t handle, size_t src_addr, void *dest, size_t size)
{
    if (!size) return __real_wl_read(handle, src_addr, dest, size);
    uint8_t *p = dest;
    while (size) {
        size_t k = wlsplit_first(src_addr, size, FLASH_SECTOR);
        esp_err_t err = __real_wl_read(handle, src_addr, p, k);
        if (err != ESP_OK) return err;
        src_addr += k, p += k, size -= k;
    }
    return ESP_OK;
}
