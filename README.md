# wiscosat

Firmware for an ESP32-S3 board that is, for whatever machine it is plugged into, a **USB keyboard** and a small
**read-only USB drive** (a "seed": for example a cloud-init `CIDATA` volume), both driven over Wi-Fi. Use: typing at
a machine's firmware menus or boot loader, and handing an unattended installer its answer file, with no person at
the machine.

Status: this is the trial's firmware as it was proven on a bench (one unit, the Wi-Fi and a shared token set at
build time in `main/secrets.h`). The plan from here: a unit's settings in its NVS (one image for every unit), the
channel moved to TLS with a pre-shared key per unit, updates over Wi-Fi with rollback, unit tests on Linux.

## Hardware

Tried on a LILYGO T-Embed (ESP32-S3, WROOM-1-N16R8: 16 MB flash, 8 MB PSRAM), its USB-C port being the chip's
native USB. Any ESP32-S3 with its native USB on a connector should do.

## Build

Needs ESP-IDF v5.5.6 (`IDF_PATH`, default `/opt/esp-idf`) with the `esp32s3` tools installed
(`./install.sh esp32s3` in the ESP-IDF tree).

```
cp main/secrets.h.example main/secrets.h    # then fill in the Wi-Fi name, password and the token
./build
```

What comes out, in `out/`:

| File | What |
|---|---|
| `wiscosat.bin` | the app |
| `flash_args` | what goes where at a first flash (boot loader, partition table, app) |
| `wiscosat.sha256` | the app's checksum |

The first flash, the board in download mode (hold BOOT, plug in), from `out/`:

```
esptool.py --chip esp32s3 -p /dev/serial/by-id/<the board> write_flash @flash_args
```

## Layout

| Path | What |
|---|---|
| `main/main.c` | the firmware: USB (TinyUSB: HID keyboard + MSC), Wi-Fi, the command channel (one JSON object per line on TCP), the seed's FAT partition |
| `partitions.csv` | NVS, PHY data, the app (2 MB), the seed (1 MB, FAT) |
| `sdkconfig.defaults` | the settings that differ from ESP-IDF's defaults, each with its reason |
| `main/idf_component.yml` | the managed components (`espressif/esp_tinyusb`); `dependencies.lock` pins them |

## Licence

See `LICENSE`.
