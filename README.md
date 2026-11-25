# wiscosat

Firmware for an ESP32-S3 board that is, for whatever machine it is plugged into, a **USB keyboard** and a small
**read-only USB drive** (a "seed": for example a cloud-init `CIDATA` volume), both driven over Wi-Fi. Use: typing at
a machine's firmware menus or boot loader, and handing an unattended installer its answer file, with no person at
the machine.

Status: one image for every unit, a unit's settings in its NVS, the channel TLS 1.3 with a pre-shared key per
unit, the Wi-Fi at a fixed address with back-off; a seed of up to 1 MB sent as raw bytes and shown to a running host
as a media change; updates over Wi-Fi into the other app slot with rollback; `restart`, `identify`.

## Hardware

Tried on a LILYGO T-Embed (ESP32-S3, WROOM-1-N16R8: 16 MB flash, 8 MB PSRAM), its USB-C port being the chip's
native USB. Any ESP32-S3 with its native USB on a connector should do.

On USB a unit is `303a:4006`, a boot keyboard and a read-only drive, maker `wiscosat`, and its serial number is its
MAC as 12 lower-case hex digits (the base MAC, as `esptool` prints it), so the machine it is plugged into names it
under `/dev/input/by-id/` and `/dev/disk/by-id/`, and tells two units apart.

### The screen

So a person tells units apart at a glance, the screen shows the unit's name (from its NVS) large, and its address and
the firmware's version small below; a board with no settings shows `unenrolled`. Drawn once, in a task of its own at a
low priority after USB has started, so the keyboard never waits for it. The backlight is dim (20 %, Kconfig
`SAT_SCREEN_BRIGHTNESS`), so it does not light the room or glare in a camera. Kconfig `SAT_SCREEN`: the T-Embed's
ST7789 (320x170 in landscape; its pins, the 35-row offset of a 170-row panel and `SAT_SCREEN_FLIP` for a board turned
round are settings), or `none` for a board with no screen. A fault on the way (no memory, the SPI bus) is an event in
`status`; the unit works on with a dark screen. What the screen shows is not refreshed: a changed address shows after
a restart.

### The battery

A board with a battery keeps running with its cable pulled (the T-Embed has a charger and a battery connector).
`status`'s `battery` says `mv` (the voltage, read when `status` asks: 16 samples, ESP-IDF's curve-fitting
calibration) and `usb_power`. Kconfig `SAT_BATTERY`: a divider on an ADC pin (the T-Embed: GPIO 4, 100k and 100k, so
`SAT_BATTERY_DIVIDER` 2; behind the switch that GPIO 46 keeps on), or `none` (`"battery": null`). A fault (the pin is
no ADC pin, no calibration) is an event and `battery.fault`.

On the T-Embed the pin reads the board's **supply rail**, not the cell alone: USB's 5 V through a Schottky diode
while a cable is in (4664 mV read), the battery once it is out (3490 mV read on a low cell). So the cell's own voltage
shows only with the cable out, and the reading says which power the unit is on: at or over `SAT_BATTERY_USB_MV`
(4400; a Li-ion cell never holds more than 4.2 V) is `usb_power: true`. That is the one way to tell a **pulled cable
from a host that is asleep**: on the bus both are the same (the unit stays configured and the bus suspended, the
event `usb suspended`), and no pin of the T-Embed sees the cable's 5 V (its schematic: VBUS goes to the charger and
the diode only). What it is not: a charge in percent (a Li-ion's voltage says that only roughly).

## Build

Needs ESP-IDF v5.5.6 (`IDF_PATH`, default `/opt/esp-idf`) with the `esp32s3` tools installed
(`./install.sh esp32s3` in the ESP-IDF tree).

```
./test         # the Linux tests (below); needs only cc and ESP-IDF's tree
./build        # the release image, into out/; nothing of a unit is built in
./build --dev  # a dev build, into out-dev/ (below)
./build --dev --broken  # the rollback check's image, into out-broken/ (below)
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

`./build --dev --broken` is a dev build that cannot prove itself after an update: `sdkconfig.broken`
(`CONFIG_SAT_BROKEN`) puts a Wi-Fi name no network has over the unit's settings, so it never joins, and the
bootloader takes the old image back (below, "Updates"). Into `out-broken/`, version ending in `-broken-dev`; for the
rollback check only.

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

## Raw bytes after a line (seed, update)

`seed` with files and `update` are followed by raw bytes, not by more lines: the unit answers the line
`{"ok":true,"ready":true}` (or an error, and nothing follows), the client sends exactly the bytes the line named
(`update`: `size`; `seed`: the files' sizes, one file after another in the line's order), and the unit answers the
final line. Bytes that stop for 10 s break the transfer off (a lost Wi-Fi looks like that), as does a closed
connection; the unit cleans up (below) and takes the next connection. Anything the client sends after the bytes
and before the final answer is dropped. So a seed is not held to one line (16 KB) and an image goes in about 14 s.

## The seed

The drive is a 1 MB FAT volume (wear levelling, 512-byte sectors, ESP-IDF's **safe** mode: about 16 KB/s, so a full
seed takes about a minute; `user-data` and `meta-data` a few seconds). A seed with files:

1. checked whole before anything changes (8 files, names, the room: `seedrules.c`), else refused, the old seed kept;
2. the volume taken from the host, which gets NOT READY, MEDIUM NOT PRESENT for `hold_ms` (default 2 s) and the
   whole write; every old file removed;
3. the bytes written straight to the files as they arrive;
4. every file read back from the volume against the SHA-256 the line named;
5. the volume labelled `CIDATA` and given back: the host's next TEST UNIT READY gets **UNIT ATTENTION, NOT READY TO
   READY CHANGE** (28h/00h) once, then the medium. A running Linux re-reads the drive (`udevadm monitor`: a
   `change` with `DISK_MEDIA_CHANGE=1` as it goes and as it comes back; the kernel logs the capacity 1984 → 0 →
   1984 blocks) with no replug.

A seed that breaks off, cannot be written or reads back wrong leaves the volume **wiped** (`EMPTY`), never half a
seed; the answer says so. The UNIT ATTENTION is answered in front of esp_tinyusb's own TEST UNIT READY by the
linker's `--wrap` (`main/CMakeLists.txt`, `usb_msc.c`).

### Reads and writes cut at the flash sectors (`wlfix.c`)

ESP-IDF v5.5.6's wear levelling gets a read or write wrong that starts inside one 4096-byte flash sector and ends
in the next. It keeps one spare flash sector that moves through the partition (one step every 16 erases), so two
neighbouring sectors of the volume are not always neighbours in the flash; `WL_Flash::write` and `::read` work out
where a piece starts and run straight on. When the spare sits right at that boundary, the end of the piece is
written into the spare, and the place it belongs stays erased.

The seed volume meets this: its clusters (4 KB) start at sector 97, one 512-byte sector into a flash sector, and
FatFs writes a whole cluster in one call when the bytes are there. So a cluster's last sector could be lost
whenever the spare stood behind its first seven. As seen (2026-11-23): a seed read back wrong about once in 19
(three files, 81 KB), a 400 KB file on the third try; the file pulled from the drive had exactly two runs of 512
bytes of `FFh`, each the last sector of a cluster (offset 3584 in its 4 KB), in two neighbouring clusters. The
read-back caught every one and the drive was wiped, so no host got a wrong seed.

`wlfix.c` cuts every `wl_read` and `wl_write` into pieces that stay inside one flash sector (`wlsplit.c` says
where, pure and tested), in front of ESP-IDF's own by the linker's `--wrap` (`main/CMakeLists.txt`), so FatFs's
and esp_tinyusb's calls are covered alike. ESP-IDF's master cuts the piece itself (`WL_Flash.cpp`: "an unaligned
request crossing a page boundary must write its head separately"); once the pinned version has that, `wlfix.c`,
`wlsplit.c` and the two `--wrap` can go.

Wear levelling's **performance** mode (one erase per 4 KB) was tried before this was known and dropped: a seed
written over the files of an earlier one read back wrong every time. Most likely the same fault (the spare moves
differently there); not tried again with the fix, safe mode is fast enough. Changing the mode also changes the
partition's layout: a unit that ran another mode needs its seed partition erased once
(`esptool erase_region 0x420000 0x100000`; the firmware formats it).

## Updates

`update` writes the image into the other app slot as it arrives (`esp_ota` with sequential writes, so nothing waits
for a whole slot's erase), checks the SHA-256 of what arrived against the line's, lets `esp_ota_end` check the image
itself, sets that slot to boot, answers, and restarts 1 s later. Anything short of that leaves the old slot booting
and the answer says why ("old version kept"). One update at a time; none while a seed is written.

The new image is **on trial** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, `status`: `update.trial`): it keeps itself
(`esp_ota_mark_app_valid_cancel_rollback`) only once it has joined the Wi-Fi **and** answered a command over the
channel; 120 s after its start without both, it marks itself invalid and the bootloader starts the old image
(`updrules.c` says which). Rollback lives in the **bootloader**, which an update does not replace: a unit whose
bootloader was flashed before this setting needs one full flash by `esptool` (`write_flash @flash_args`) first.

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
with nothing plugged in; a seed refused whole, with the file it is about; a seed's raw bytes split into its files
(an empty file takes none); an update refused before any byte (size 0, over the slot, one running, a seed being
written) and its end (bytes short, another SHA-256); the new image's trial (proven only with the Wi-Fi joined and a
command answered, rolled back at 120 s); the lockout (4 failures fine, the fifth
60 s, then 120 s, up to 900 s, across the clock's wrap); a unit's settings (an address `192.0.2.999` or none:
unenrolled); the USB serial number made from the MAC (`dc da 0c 52 37 38` → `dcda0c523738`); a read or write on the
seed partition cut at every flash sector's boundary (4096 bytes from offset 512: 3584 first; 81,000 bytes from
sector 97: 20 pieces, none over a boundary).

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
| `main/usb_msc.c` | the seed drive: the FAT partition, shown to the host read-only, taken away while a seed is written, its bytes streamed in and read back; the UNIT ATTENTION as it comes back |
| `main/wlfix.c`, `main/wlsplit.{c,h}` | every wear-levelled read and write cut at the flash sectors' boundaries (ESP-IDF v5.5.6 gets one that crosses wrong, "The seed"); `wlsplit` is the pure part |
| `main/update.c` | the update: the other slot, the SHA-256, the trial and the rollback |
| `main/updrules.{c,h}` | pure: what an update may do (its size, one at a time, its end) and the new image's trial |
| `main/led.c` | `identify`: the board's LED (Kconfig `SAT_LED`) |
| `main/battery.c` | `status`'s `battery`: the voltage on the ADC pin (Kconfig `SAT_BATTERY`) |
| `main/powerrules.{c,h}` | pure: whether a battery reading is USB power |
| `main/screen.c`, `main/font5x7.h` | the screen: the unit's name, address and version (Kconfig `SAT_SCREEN`); the 5x7 font, data only |
| `main/keys.{c,h}` | pure: key names (`ctrl-alt-del`) and characters (US layout) to an 8-byte boot report |
| `main/armed.{c,h}` | pure: the armed key's schedule and counters |
| `main/proto.{c,h}` | pure: a line to a command (every field checked, defaults filled in), and the answer line |
| `main/seedrules.{c,h}` | pure: what a seed may hold (8 files, names up to 60 characters FAT can keep, the room), where its raw bytes go |
| `main/sat.h` | how the device parts call each other |
| `tests/` | the Linux tests of the pure parts; `./test` runs them |
| `partitions.csv` | NVS (`0x9000`, 24 KB), the app slots' state, PHY data, two app slots (2 MB each), the seed (1 MB, FAT); 8 MB |
| `tools/mknvs` | a unit's NVS image from a CSV on standard input |
| `tools/sat-probe` | the bench client: one line over the channel, or a wrong key, or a plain line |
| `sdkconfig.defaults` | the settings that differ from ESP-IDF's defaults, each with its reason |
| `sdkconfig.dev`, `sdkconfig.broken`, `main/Kconfig.projbuild` | the dev build's setting `CONFIG_SAT_DEV` (`./build --dev`), the broken one's `CONFIG_SAT_BROKEN` (`--broken`), the LED (`SAT_LED`), the screen (`SAT_SCREEN_*`), the battery (`SAT_BATTERY_*`) |
| `main/idf_component.yml` | the managed components (`espressif/esp_tinyusb`); `dependencies.lock` pins them |

The pure parts include no ESP-IDF header, so they build on Linux as they are; the device parts call them.

## Commands

Inside the channel, one JSON object per line, one answer line back: `{"ok":true,"state":{...}}` or
`{"ok":false,"error":"..."}`. `state` holds, besides the trial's: `name`, `version`, `dev` (a dev build), `slot`
(the running app slot), `battery` ("The battery"), `update` (`running`, `trial`: `none`, `on trial`, `proven`, `rolling back`; `boots`: the slot
the next start boots), `wifi`, `channel`.

| Command | Fields | What |
|---|---|---|
| `status` | | the state |
| `key` | `name`; `when`: `now` (default) or `configured`; for `configured`: `every_ms` (20..5000, default 100), `for_ms` (0..60000, default 3000; 0 once), `persist`, `sticky` | now: down and up at once (`"no host"` when no host has configured the unit: nothing is queued). `configured`: armed, sent by the unit itself from the host's next configuration (a PC's firmware configures at every start). `persist`: kept in NVS through the unit's own power cut, erased once fired. `sticky` (implies `persist`): armed again after each firing, for every later start, until `disarm` or a press of the knob; `status`'s `armed` shows it and `from` (`command`, `knob`) |
| `disarm` | | the armed key goes |
| `press`, `release` | `name` (`release` without it: everything up) | hold keys down, let them go; a `key` or `type` meanwhile carries what is held (`press ctrl`, `key c`: Ctrl+C) and goes back to it after; at most six keys at once |
| `type` | `text` | typed on the US layout; refused whole if a character has no key |
| `seed` | `files` (`[{"name":"user-data","size":N,"sha256":"<64 hex>"},...]`), `hold_ms` (0..10000, default 2000) | `ready`, then the files' bytes raw (above); the drive's files replaced, read back, given back as a media change ("The seed"). No files: the drive wiped (no bytes follow). At most 8 files, names up to 60 characters, all within the drive; else refused whole before any byte, the old seed untouched, the error naming the file |
| `update` | `size` (1.. the slot), `sha256` | `ready`, then `size` bytes raw; the final answer, then a restart into the new slot 1 s later ("Updates") |
| `wake` | | a sleeping (suspended) host woken by USB remote wake-up; refused with `no host`, `the host is not asleep`, or `the host did not allow this unit to wake it` (the host has not enabled remote wake-up for it) |
| `menu` | `text` (optional) | the knob's menu ("The knob"). No `text`: the state with `menu` holding the text in use. A `text`: checked whole, then kept (NVS) and used; a bad one changes nothing and is refused with its line (`menu line 2: "f13" is no key, ...`). `""`: back to the built-in list. In `status`, `menu` has `source` (`text`, `built-in`), `entries`, `menus`, and `fault` when a stored text was not readable by this firmware; only on a board with a knob |
| `restart` | | answered, then restarted 1 s later (the host sees the USB device go and come back) |
| `identify` | | the LED blinks for 5 s (T-Embed: its ring of 7 APA102); a board with none: `no led on this board` |
| `download` | | a dev build only: answers `ok` with the state, then 300 ms later restarts into the ROM's download mode over the USB the app was using, USB-OTG (the unit shows as `303a:0009`, "ESP32-S3", serial number its MAC with colons: a serial port for `esptool`; not `303a:1001`, the USB-Serial/JTAG of BOOT held at reset); no keyboard, no drive, no network until it is flashed (`--after watchdog_reset` starts the app; esptool 4.8 or later) or replugged. The ROM resets its USB when `esptool` lets go, so a second `esptool` call waits for the port to come back. A release build answers `unknown command` |

The trial's `bootloader` command is gone (`unknown command`); updates over Wi-Fi replace it, and a dev build's
`download` is the way back when an update cannot be sent. A new seed volume is
left empty (the trial wrote two placeholder files), so a wipe survives a restart. The volume is labelled `CIDATA`
while it holds files and **`EMPTY` with none**: a wiped seed is no cloud-init seed, so a host's NoCloud never reads it
in place of a real `CIDATA` drive plugged in beside it (with two, which one is read is the device order's).

## The knob

On a board with one (Kconfig `SAT_KNOB`: the LilyGO T-Embed's rotary encoder and its button): for a unit used by
itself, away from its network, with its screen.

- **Turn:** a list on the screen. **Press** on a key (F1-F12, Delete, Enter, Esc in the built-in list): armed sticky
  (`Armed: f2`), nothing sent into what runs now; the unit sends it from the computer's next start, every 100 ms for
  up to 60 s, ending 10 s after the host took the first, and at every later start, until a press takes it away.
  Press on a combination: sent once, now. An entry of kind `hold` (Alt-SysRq s, u, b: restarts with no question)
  wants the knob held 2 s
- **Double press** (two within 350 ms; a single press therefore acts a third of a second late): the next menu. With
  no menu on the screen a press only takes away what is armed or being sent; the menu goes after 10 s untouched
- **The list is a text** (`main/menu.h` has the whole form), set by the command `menu`:
  ```
  # one entry a line: <label> | arm|send|hold | <keys>; [Name] starts a menu; # a comment
  [Keys]
  Setup | arm | f2
  Boot menu | arm | f9
  [Power]
  Restart | send | ctrl-alt-del
  Restart SysRq | hold | alt-sysrq-s 2000ms alt-sysrq-u 2000ms alt-sysrq-b
  Wake | send | wake
  ```
  Labels 1 to 20 characters of the screen's font (letters, digits, space, `- . : _ / ?`); `send` and `hold` take up
  to 12 keys, 100 ms apart, with a wait (`500ms`, 1 to 10000) where one is needed, and the word `wake`; up to 8
  menus, 40 entries, 4096 bytes. With no text of its own the unit uses `menu_builtin` (`main/menu.c`)

## Licence

See `LICENSE`.
