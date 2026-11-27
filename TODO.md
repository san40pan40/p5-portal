# TODO

## Done
- [x] Refactor into modules, English comments, README, MIT license
- [x] OTA upload from the browser (verified over the home network)
- [x] Several uplink networks: list of known networks with per-network static IP, strongest-network pick, roaming, nearby scan
- [x] Settings redesign (one form per card, restart only for AP changes), one layout and component system, switches
- [x] Lighter UI: live status via `/api/status`, pages sent in parts, gzip CSS/JS. 1-hour graphs (CPU, RAM, clients, RSSI, DNS) and top 10 tables
- [x] Releases and update check: version from `git describe`, release workflow on `v*` tags, installation over HTTPS from the panel
- [x] New partition table (2 × 1.5 MB app, 960 KB LittleFS), rule export and import
- [x] Preset blocklist: one merged 48-bit list from StevenBlack and HaGeZi Light. A manual workflow builds it. The board keeps it in PSRAM and checks for updates each day.
- [x] English-only UI (the Ukrainian text was removed to save flash)
- [x] CHANGELOG.md and release notes from it
- [x] Dev script in Python (`uv run scripts/dev.py`) for Windows and macOS

## Next
Nothing planned. Pick an item from Ideas.

## Ideas / known gaps
- DNS over TCP (large answers > 1232 bytes)
- Retry a timed-out upstream query through a second resolver (some routers or providers drop many DNS queries to public resolvers)
- Allowlist for the preset blocklist (an "Allow" button for preset hits in the query log)
- Panel security: Digest auth instead of Basic, option "panel only from the access point", delay after failed logins. HTTPS for the panel is not planned: self-signed certificate warnings, about 40 KB RAM per connection.
- Other chips: classic ESP32 with PSRAM is easy. C3/C6 (no PSRAM, one core) need a list lookup in flash.
- Optional power saving (lower CPU clock or TX power)
- Unit tests for `RuleList` on the PC (`pio test -e native`)
- `.vscode/extensions.json.optional` is a PlatformIO artifact. It can be removed from the repo.
