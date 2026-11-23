# wiscosat

Firmware for an ESP32-S3 board that is, for whatever machine it is plugged into, a **USB keyboard** and a small
**read-only USB drive** (a "seed": for example a cloud-init `CIDATA` volume), both driven over Wi-Fi. Use: typing at
a machine's firmware menus or boot loader, and handing an unattended installer its answer file, with no person at
the machine.

Status: one image for every unit, a unit's settings in its NVS, the channel TLS 1.3 with a pre-shared key per
unit, the Wi-Fi at a fixed address with back-off. Next: updates over Wi-Fi with rollback, `restart`, `identify`.

## Hardware

Tried on a LILYGO T-Embed (ESP32-S3, WROOM-1-N16R8: 16 MB flash, 8 MB PSRAM), its USB-C port being the chip's
native USB. Any ESP32-S3 with its native USB on a connector should do.

On USB a unit is `303a:4006`, a boot keyboard and a read-only drive, maker `wiscosat`, and its serial number is its
MAC as 12 lower-case hex digits (the base MAC, as `esptool` prints it), so the machine it is plugged into names it
under `/dev/input/by-id/` and `/dev/disk/by-id/`, and tells two units apart.

## Build

Needs ESP-IDF v5.5.6 (`IDF_PATH`, default `/opt/esp-idf`) with the `esp32s3` tools installed
(`./install.sh esp32s3` in the ESP-IDF tree).

```
./test         # the Linux tests (below); needs only cc and ESP-IDF's tree
./build        # the release image, into out/; nothing of a unit is built in
./build --dev  # a dev build, into out-dev/ (below)
```

What comes out, in `out/`:

| File | What |
|---|---|
| `wiscosat.bin` | the app |
| `flash_args` | what goes where at a first flash (boot loader, partition table, the app slots' state, app) |
| `wiscosat.sha256` | the app's checksum |

The image's version is `git describe --tags --always --dirty` (`./build` gives it to ESP-IDF as `PROJECT_VER` each
time): it is in the app description, 32 bytes at offset 48 of `wiscosat.bin`, and in `status` as `version`.
Read it from a file with `dd if=out/wiscosat.bin bs=1 skip=48 count=32 2>/dev/null | tr -d '\0'`.

### The dev build

`./build --dev` builds into `out-dev/` (the same three files), with its own `sdkconfig` there, from
`sdkconfig.defaults` and then `sdkconfig.dev` (`CONFIG_SAT_DEV=y`, `main/Kconfig.projbuild`). It is a second build
folder, never a flag on the release image, so `out/wiscosat.bin` is always the release image. A dev build:

- has the command `download` (below): the unit restarts into the ROM's download mode, so `esptool` can flash it
  with no one pressing BOOT and RST;
- says `"dev": true` in `status` (a release build says `false`);
- has a version ending in `-dev`. `main/CMakeLists.txt` stops a build whose version and configuration disagree (a
  dev configuration built without `./build --dev`, or the other way round).

Keep a dev build to a unit kept for development: anyone who reaches a unit's channel with its key can then put it
in download mode, and from there write any image over USB.

## A unit's settings (enrolling)

A unit reads its settings from NVS, namespace `sat`, at every start. All strings but `key`:

| Key | What |
|---|---|
| `name` | the unit's name; also the channel's PSK identity (up to 32 characters) |
| `wifi_ssid`, `wifi_pass` | the Wi-Fi (no `wifi_pass`, or empty: an open network; else 8 to 63 characters) |
| `ip`, `mask`, `gw` | its fixed address (no DHCP), as dotted quads |
| `key` | blob, 32 bytes: the unit's key |

A board with no settings, or any of them missing or wrong (an address like `192.0.2.999`), is **unenrolled**: a
keyboard and a seed drive that never joins a network (`status` events: `unenrolled: no network`).

The NVS image, made from a CSV on standard input by `tools/mknvs` (ESP-IDF's `nvs_partition_gen.py`; the CSV's
rows are in the script's head). The CSV holds the Wi-Fi password and the key: pipe it in, keep the image on a tmpfs
and remove it after:

```
umask 077
openssl rand -hex 32 > /dev/shm/unit-1.key           # the unit's key, 64 hex characters; keep it somewhere safe
{ echo key,type,encoding,value; echo sat,namespace,,; echo name,data,string,unit-1
  echo wifi_ssid,data,string,example; echo "wifi_pass,data,string,$PASSWORD"
  echo ip,data,string,192.0.2.61; echo mask,data,string,255.255.255.0; echo gw,data,string,192.0.2.1
  echo "key,data,hex2bin,$(cat /dev/shm/unit-1.key)"; } | tools/mknvs --size 0x6000 /dev/shm/unit-1-nvs.bin
```

The first flash, the board in download mode (a board running another firmware with USB-Serial/JTAG: esptool resets
it by itself; one running this firmware: hold BOOT, press RST, let go), from `out/`, with `-e` erasing what was
there:

```
esptool.py --chip esp32s3 -p /dev/serial/by-id/<the board> write_flash -e @flash_args 0x9000 /dev/shm/unit-1-nvs.bin
rm /dev/shm/unit-1-nvs.bin
```

New settings alone: `write_flash 0x9000 <image>` (this also drops an armed key kept in flash). After a flash over
USB-Serial/JTAG the app may not start until the board is unplugged and plugged in again (seen on the T-Embed).

## The channel

TLS 1.3 only, with the unit's pre-shared key and an ephemeral (EC)DHE exchange (`PSK_EPHEMERAL`: the session keys
never come from the key alone), on TCP 5577; the PSK identity is the unit's name. One connection at a time; a
connection idle for 60 s is closed, a handshake over 5 s fails. Each failed handshake (a wrong key, another name, a
plain line with no TLS) is counted in `status` (`channel.failed_handshakes`). **Five in a row and the unit is
deaf** for 60 s: every connection is closed unread; each failure after that doubles it, up to 15 minutes; one good
handshake starts over. `channel.deaf_until_ms` says until when it was last deaf (the unit's uptime clock), also
after it.

`tools/sat-probe` is the bench client (Python 3.13 or later, standard library): `SAT_KEY_FILE=<key file>
tools/sat-probe 192.0.2.61 --name unit-1 [--line '{"cmd":"status"}'] [--count N] [--wrong-key | --plain]`.

## Wi-Fi and the watchdog

USB starts before the network and never waits for it: the unit is a keyboard about 0.4 s after it starts, whatever
the Wi-Fi does. The Wi-Fi joins at the fixed address; after a drop or a refused join it tries again after 1 s,
doubling to 60 s, back to 1 s once joined (`status`: `wifi.joined`, `drops`, `last_reason`, ESP-IDF's reason
code). The task watchdog restarts a firmware whose idle task has been starved for 5 s.

## Tests

`./test` builds the pure parts (below) with the host's `cc` and cJSON from ESP-IDF's tree (`$IDF_PATH`, no
ESP-IDF build, no board), with AddressSanitizer and UBSan, and runs `tests/`. It prints `test: N checks, 0 failed`
and exits 0; it builds and runs them twice, as the release image's parts and as the dev build's (`-DSAT_DEV`), so it
prints that line twice. A failed check prints its file, line and what failed, and exits 1. It needs ESP-IDF's tree only for
`components/json/cJSON` (exit 2 without it). Its output goes to `out/test/`. Both `./test` and `./build` must pass
before every commit.

What they pin, among others: key names to reports (`f9` → `0x42`, `alt-sysrq-b` → modifier `0x04`, keys `0x46 0x05`,
seven keys refused, an unknown name named in the error); the US layout for `type`; the armed key's schedule (100 ms
apart for 3000 ms is 30 reports, then never; missed slots skipped, not bunched; across the counter's wrap); the
lines (16,384 bytes the longest, `bootloader` gone, an old client's `token` field ignored; `download` only in a dev
build, else answered as any unknown command; `state.dev`); "no host" for a key now
with nothing plugged in; a seed refused whole, with the file it is about; the lockout (4 failures fine, the fifth
60 s, then 120 s, up to 900 s, across the clock's wrap); a unit's settings (an address `192.0.2.999` or none:
unenrolled); the USB serial number made from the MAC (`dc da 0c 52 37 38` → `dcda0c523738`).

## Layout

| Path | What |
|---|---|
| `main/app.c` | the wiring: start-up, the event list, the armed key's task, the commands, status |
| `main/channel.c` | the channel: TLS 1.3 PSK on TCP 5577, one connection at a time, one JSON object per line, the lockout |
| `main/wifi.c` | the Wi-Fi: the fixed address, the back-off |
| `main/settings.{c,h}` | a unit's settings: read from NVS (on the board), checked (pure) |
| `main/lockout.{c,h}` | pure: the channel's lockout |
| `main/usb_kbd.c` | the USB device (both interfaces' descriptors, TinyUSB) and the keyboard: tap, press and release; a report the host has not taken within 50 ms is given up |
| `main/usbserial.{c,h}` | pure: the USB serial number, the unit's MAC as 12 lower-case hex digits |
| `main/usb_msc.c` | the seed drive: the FAT partition, shown to the host read-only, taken away while a seed is written |
| `main/keys.{c,h}` | pure: key names (`ctrl-alt-del`) and characters (US layout) to an 8-byte boot report |
| `main/armed.{c,h}` | pure: the armed key's schedule and counters |
| `main/proto.{c,h}` | pure: a line to a command (every field checked, defaults filled in), and the answer line |
| `main/seedrules.{c,h}` | pure: what a seed may hold (8 files, names up to 60 characters FAT can keep, the room) |
| `main/sat.h` | how the device parts call each other |
| `tests/` | the Linux tests of the pure parts; `./test` runs them |
| `partitions.csv` | NVS (`0x9000`, 24 KB), the app slots' state, PHY data, two app slots (2 MB each), the seed (1 MB, FAT); 8 MB |
| `tools/mknvs` | a unit's NVS image from a CSV on standard input |
| `tools/sat-probe` | the bench client: one line over the channel, or a wrong key, or a plain line |
| `sdkconfig.defaults` | the settings that differ from ESP-IDF's defaults, each with its reason |
| `sdkconfig.dev`, `main/Kconfig.projbuild` | the dev build's one setting, `CONFIG_SAT_DEV` (`./build --dev`) |
| `main/idf_component.yml` | the managed components (`espressif/esp_tinyusb`); `dependencies.lock` pins them |

The pure parts include no ESP-IDF header, so they build on Linux as they are; the device parts call them.

## Commands

Inside the channel, one JSON object per line, one answer line back: `{"ok":true,"state":{...}}` or
`{"ok":false,"error":"..."}`. `state` holds, besides the trial's: `name`, `version`, `dev` (a dev build), `slot`
(the running app slot), `wifi`, `channel`.

| Command | Fields | What |
|---|---|---|
| `status` | | the state |
| `key` | `name`; `when`: `now` (default) or `configured`; for `configured`: `every_ms` (20..5000, default 100), `for_ms` (0..30000, default 3000; 0 once), `persist` | now: down and up at once (`"no host"` when no host has configured the unit: nothing is queued). `configured`: armed, sent by the unit itself from the host's next configuration (a PC's firmware configures at every start) |
| `disarm` | | the armed key goes |
| `press`, `release` | `name` (`release` without it: everything up) | hold keys down, let them go |
| `type` | `text` | typed on the US layout; refused whole if a character has no key |
| `seed` | `files` (`{"name":"content"}`), `encoding` (`text` default, or `base64`), `hold_ms` (0..10000, default 2000) | the drive's files replaced; no files: the drive wiped. At most 8 files, names up to 60 characters, all within the drive; else refused whole, the old seed untouched, the error naming the file |
| `update`, `restart`, `identify` | | parsed, answered "not in this firmware yet" |
| `download` | | a dev build only: answers `ok` with the state, then 300 ms later restarts into the ROM's download mode over the USB the app was using, USB-OTG (the unit shows as `303a:0009`, "ESP32-S3", serial number its MAC with colons: a serial port for `esptool`; not `303a:1001`, the USB-Serial/JTAG of BOOT held at reset); no keyboard, no drive, no network until it is flashed (`--after watchdog_reset` starts the app; esptool 4.8 or later) or replugged. The ROM resets its USB when `esptool` lets go, so a second `esptool` call waits for the port to come back. A release build answers `unknown command` |

The trial's `bootloader` command is gone (`unknown command`); updates over Wi-Fi replace it, and a dev build's
`download` is the way back when an update cannot be sent. A new seed volume is
left empty (the trial wrote two placeholder files), so a wipe survives a restart. The volume is labelled `CIDATA`
while it holds files and **`EMPTY` with none**: a wiped seed is no cloud-init seed, so a host's NoCloud never reads it
in place of a real `CIDATA` drive plugged in beside it (with two, which one is read is the device order's).

## Licence

See `LICENSE`.
