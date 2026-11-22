# wiscosat: instructions for AI agents and humans

Canonical instruction file for every agent working in this repo (Claude Code reads it via `CLAUDE.md`).

Firmware for an ESP32-S3 that is a USB keyboard and a small seed drive, driven over Wi-Fi (`README.md`). C,
ESP-IDF. Public.

## Rules

- **No value of any one lab.** No Wi-Fi name, address, key, host name or unit name of a real place as a default,
  in an example or in a comment. Examples use `192.0.2.x` and `unit-1`. A unit's own values reach it from outside
  the repo, in its NVS (`README.md` "A unit's settings"). Before every push:
  `git grep -n -i 'wisco\|192\.168\|10\.42'`: only the repo's own name (and its owner's GitHub name) may match
- **No secret in the repo, in a log or on the serial port.** Typed text, keys and passwords are never logged
- **Every version pinned:** ESP-IDF (v5.5.6, `README.md`), the managed components (`dependencies.lock`, committed)
- **`./test` and `./build` must pass before every commit** (`./test`: the pure parts, on Linux). A new pure part
  (no ESP-IDF header) gets its tests in `tests/` first
- **`out/` is always the release image:** a dev build (`./build --dev`, the command `download`) goes to `out-dev/`
  only, its version ends in `-dev`. Anything only a dev build has sits under `#ifdef SAT_DEV`, and `./test` checks
  both builds
- **The flash layout fits 8 MB** (the smallest board in use), whatever the board has
- C in ESP-IDF's style: four spaces, `snake_case`, `static` for everything a file keeps to itself. Scripts in bash,
  `set -euo pipefail`, one clear line and a non-zero exit on failure
- Explain as well as automate: `README.md` (and `docs/` once it exists) says each step, why, what goes wrong
- Small commits about one thing, with the reason in the message. No force-push, no history rewrite
