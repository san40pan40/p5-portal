# P5 Portal

A pocket ad-blocking Wi-Fi access point on a tiny **ESP32-S3** board — a small "Pi-hole" that is also a router.

Connect your phone or laptop to the board's Wi-Fi: it shares your home internet and quietly drops ads and trackers at the DNS level. It can also serve as the DNS server for your whole home network.

## Features

- **Own Wi-Fi access point** with NAT — the board connects to your home router and shares the internet.
- **DNS filter** — blocked domains resolve to `0.0.0.0`, the rest go to an upstream resolver (1.1.1.1 by default).
  - Rules: `site.com` (domain + subdomains), `*.site.com` (subdomains only), `*site.com` (any suffix).
  - Paste hosts-file lines (`0.0.0.0 ads.example.com`) directly.
  - Live query log with one-click **Block** / **Allow**.
- **Guard** — stops devices from bypassing the filter:
  - drops DNS to other servers (port 53) and DNS-over-TLS (port 853, Android "Private DNS"),
  - sinkholes known DNS-over-HTTPS servers (editable list),
  - tells Firefox to switch off its built-in DoH,
  - optional strict mode: blocks HTTPS to well-known DoH IPs.
- **DNS for the whole home network** — give the board a static IP and point your router's DHCP DNS at it.
- **Web panel** — status (uplink, signal, internet latency, clients, memory), filter, guard and settings; optional password; CSRF-protected.
- **OTA updates** — upload a new `firmware.bin` from the browser; the old firmware keeps running if anything fails.
- **Password reset without a computer** — hold **BOOT** for 5 s while running: only passwords are reset, everything else is kept.
- **Status LED** — orange starting · blue no router · yellow no internet · green OK · purple updating.

## Hardware

[Waveshare ESP32-S3-Zero](https://www.waveshare.com/wiki/ESP32-S3-Zero) (ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM, native USB). Other ESP32-S3 boards with ≥ 4 MB flash should work with small changes (LED pin).

## Quick start

1. Install [VS Code](https://code.visualstudio.com/) + the **PlatformIO IDE** extension and open this folder.
2. Plug the board in and press **Upload** (→). Later updates can go over the air.
3. Join the Wi-Fi **`ESP32-Portal`** (password `12345678`) and open **http://192.168.4.1**.
4. **Settings** → enter your home Wi-Fi so the board can reach the internet. Change the AP password.

### Use it as the DNS server for your home network

1. **Settings → "Board as DNS for the home network"**: enable a static IP outside your router's DHCP range.
2. In the router: DHCP → DNS server = that IP.
3. Check from a computer: `nslookup doubleclick.net <board IP>` → `0.0.0.0`.

A secondary DNS such as 8.8.8.8 keeps the internet working if the board is offline, but devices often query both, so some ads will get through.

## Updating

**Settings → Firmware update (OTA)** → choose `firmware.bin` from a [release](../../releases) or from `.pio/build/esp32-s3-zero/` after building. Settings and lists are preserved.

## Project layout

```
src/
  main.cpp          setup() and loop() only
  app_config.*      settings in NVS + validation
  app_network.*     access point, uplink, NAT, internet checks
  app_board.*       status LED, BOOT-button password reset
  dns_filter.*      DNS server: blocklists, forwarding, query log
  rule_list.*       a rule list stored on LittleFS (blocklist, DoH servers)
  anti_bypass.*     Guard: packet-level bypass blocking
  ring.h            small ring buffer for logs
  web/              web panel: shared frame + one file per page
```

## Limitations

- DNS over UDP only — very large answers (> 1232 bytes, rare) that need a TCP retry will fail.
- The blocklist lives in RAM: comfortably a few thousand rules.
- One radio: the access point follows the router's channel; clients briefly reconnect if it changes.
- The Guard hooks an internal ESP-IDF API; re-check it after updating the platform.

## Roadmap

- Ukrainian / English interface (more languages downloadable from this repo)
- "Check for updates" in the panel, installing releases from GitHub
- Preset blocklists from this repo, toggled from the panel
- Several uplink networks with automatic best-signal selection

## License

[MIT](LICENSE) © 2026 san40pan40
