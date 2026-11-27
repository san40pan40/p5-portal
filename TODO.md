# TODO

## Done
- [x] Refactor into modules, English comments, README, MIT license
- [x] Web UI in Ukrainian and English (switch in the header and in Settings, no restart)
- [x] OTA upload from the browser (verified over the home network)

## Next
1. **Several uplink networks**
   - store a list of known networks; each has its own SSID, password and **its own static-IP settings**
     ("Board as DNS for the home network"), because different places use different subnets
   - connect to the strongest known network; roam when RSSI drops below ~-75 dBm and another is ≥ 8 dB better
   - "nearby networks" scan in Settings to add a network with one click
2. **Releases + "check for updates"**
   - firmware version from `git describe` (PlatformIO extra script)
   - manual GitHub releases with `firmware.bin` (GitHub Actions later)
   - panel button: check the latest release and install it over HTTPS (repo is public — no token)
3. **Preset blocklists from this repo**
   - `blocklists/index.json` + source `.hosts` files + compiled `.bin` (sorted 64-bit FNV-1a hashes)
   - `scripts/build_lists.py` builds the `.bin` files and the index
   - panel: toggle a list on → download to LittleFS; off → delete; daily update check
   - lookup: binary search in the flash file (≈ 150k domains fit), user list stays in RAM
   - record source + license of each list in `index.json`
4. **More languages** — `lang/xx.json` in the repo, downloadable from the panel (keys = ids in `i18n_strings.h`)

## Ideas / known gaps
- DNS over TCP (large answers > 1232 bytes)
- CPU load and memory graphs on the status page; optional power saving (lower CPU clock / TX power)
- Unit tests for `RuleList` on the PC (`pio test -e native`)
- `.vscode/extensions.json.optional` is a PlatformIO artifact — can be removed from the repo
