# TODO

## Done
- [x] Refactor into modules, English comments, README, MIT license
- [x] Web UI in Ukrainian and English (switch in the header and in Settings, no restart)
- [x] OTA upload from the browser (verified over the home network)
- [x] Several uplink networks: list of known networks with per-network static IP, strongest-network pick + roaming, nearby scan
- [x] Settings redesign (per-card forms, no restart except AP), consistent layout/components, toggle switches
- [x] Lighter UI: live status via `/api/status`, streamed pages, gzipped CSS/JS; 1-hour graphs (CPU, RAM, clients, RSSI, DNS) + top-10

## Next
1. **Releases + "check for updates"**
   - firmware version from `git describe` (PlatformIO extra script)
   - manual GitHub releases with `firmware.bin` (GitHub Actions later)
   - panel button: check the latest release and install it over HTTPS (repo is public — no token)
2. **Preset blocklists from this repo**
   - `blocklists/sources.json` + `scripts/build_lists.py`; a GitHub Action builds `.bin` (sorted 64-bit FNV-1a hashes)
     + `index.json` and publishes them to a rolling `lists` release (no binaries in git history)
   - panel: toggle a list on → download to LittleFS; off → delete; daily update check
   - lookup: binary search in the flash file (≈ 150k domains fit), user list stays in RAM
   - record source + license of each list in `index.json`
3. **More languages** — `lang/xx.json` in the repo, downloadable from the panel (keys = ids in `i18n_strings.h`)

## Ideas / known gaps
- DNS over TCP (large answers > 1232 bytes)
- Retry a timed-out upstream query via a second resolver (router / ISP drops DNS bursts to public resolvers)
- Optional power saving (lower CPU clock / TX power)
- Unit tests for `RuleList` on the PC (`pio test -e native`)
- `.vscode/extensions.json.optional` is a PlatformIO artifact — can be removed from the repo
