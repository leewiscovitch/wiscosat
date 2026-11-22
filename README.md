# wiscosat

Firmware for an ESP32-S3 board that is, for whatever machine it is plugged into, a **USB keyboard** and a small
**read-only USB drive** (a "seed": for example a cloud-init `CIDATA` volume), both driven over Wi-Fi. Use: typing at
a machine's firmware menus or boot loader, and handing an unattended installer its answer file, with no person at
the machine.

Status: the trial's firmware, split into parts with tests on Linux (one unit, the Wi-Fi and a shared token still
set at build time in `main/secrets.h`). The plan from here: a unit's settings in its NVS (one image for every unit),
the channel moved to TLS with a pre-shared key per unit, updates over Wi-Fi with rollback.

## Hardware

Tried on a LILYGO T-Embed (ESP32-S3, WROOM-1-N16R8: 16 MB flash, 8 MB PSRAM), its USB-C port being the chip's
native USB. Any ESP32-S3 with its native USB on a connector should do.

## Build

Needs ESP-IDF v5.5.6 (`IDF_PATH`, default `/opt/esp-idf`) with the `esp32s3` tools installed
(`./install.sh esp32s3` in the ESP-IDF tree).

```
./test                                      # the Linux tests (below); needs only cc and ESP-IDF's tree
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

## Tests

`./test` builds the four pure parts (below) with the host's `cc` and cJSON from ESP-IDF's tree (`$IDF_PATH`, no
ESP-IDF build, no board), with AddressSanitizer and UBSan, and runs `tests/`. It prints `test: N checks, 0 failed`
and exits 0; a failed check prints its file, line and what failed, and exits 1. It needs ESP-IDF's tree only for
`components/json/cJSON` (exit 2 without it). Its output goes to `out/test/`. Both `./test` and `./build` must pass
before every commit.

What they pin, among others: key names to reports (`f9` → `0x42`, `alt-sysrq-b` → modifier `0x04`, keys `0x46 0x05`,
seven keys refused, an unknown name named in the error); the US layout for `type`; the armed key's schedule (100 ms
apart for 3000 ms is 30 reports, then never; missed slots skipped, not bunched; across the counter's wrap); the
lines (16,384 bytes the longest, `bootloader` gone, a `token` field accepted and left to the caller); "no host" for a
key now with nothing plugged in; a seed refused whole, with the file it is about.

## Layout

| Path | What |
|---|---|
| `main/app.c` | the wiring: start-up, the event list, the armed key's task, the commands, the channel (TCP 5577, one JSON object per line), the Wi-Fi |
| `main/usb_kbd.c` | the USB device (both interfaces' descriptors, TinyUSB) and the keyboard: tap, press and release; a report the host has not taken within 50 ms is given up |
| `main/usb_msc.c` | the seed drive: the FAT partition, shown to the host read-only, taken away while a seed is written |
| `main/keys.{c,h}` | pure: key names (`ctrl-alt-del`) and characters (US layout) to an 8-byte boot report |
| `main/armed.{c,h}` | pure: the armed key's schedule and counters |
| `main/proto.{c,h}` | pure: a line to a command (every field checked, defaults filled in), and the answer line |
| `main/seedrules.{c,h}` | pure: what a seed may hold (8 files, names up to 60 characters FAT can keep, the room) |
| `main/sat.h` | how the device parts call each other |
| `tests/` | the Linux tests of the pure parts; `./test` runs them |
| `partitions.csv` | NVS, PHY data, the app (2 MB), the seed (1 MB, FAT) |
| `sdkconfig.defaults` | the settings that differ from ESP-IDF's defaults, each with its reason |
| `main/idf_component.yml` | the managed components (`espressif/esp_tinyusb`); `dependencies.lock` pins them |

The pure parts include no ESP-IDF header, so they build on Linux as they are; the device parts call them.

## Commands

One JSON object per line, one answer line back: `{"ok":true,"state":{...}}` or `{"ok":false,"error":"..."}`. Every
line carries `"token"` until the channel becomes TLS with a key per unit.

| Command | Fields | What |
|---|---|---|
| `status` | | the state |
| `key` | `name`; `when`: `now` (default) or `configured`; for `configured`: `every_ms` (20..5000, default 100), `for_ms` (0..30000, default 3000; 0 once), `persist` | now: down and up at once (`"no host"` when no host has configured the unit: nothing is queued). `configured`: armed, sent by the unit itself from the host's next configuration (a PC's firmware configures at every start) |
| `disarm` | | the armed key goes |
| `press`, `release` | `name` (`release` without it: everything up) | hold keys down, let them go |
| `type` | `text` | typed on the US layout; refused whole if a character has no key |
| `seed` | `files` (`{"name":"content"}`), `encoding` (`text` default, or `base64`), `hold_ms` (0..10000, default 2000) | the drive's files replaced; no files: the drive wiped. At most 8 files, names up to 60 characters, all within the drive; else refused whole, the old seed untouched, the error naming the file |
| `update`, `restart`, `identify` | | parsed, answered "not in this firmware yet" |

The trial's `bootloader` command is gone (`unknown command`); updates over Wi-Fi replace it. A new seed volume is
left empty (the trial wrote two placeholder files), so a wipe survives a restart.

## Licence

See `LICENSE`.
