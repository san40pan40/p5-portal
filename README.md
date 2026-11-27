# P5 Portal

P5 Portal is a small Wi-Fi access point that blocks ads. It runs on an **ESP32-S3** board. It works as a small "Pi-hole" and as a router at the same time.

Connect your phone or laptop to the Wi-Fi of the board. The board shares your home internet connection. It blocks ads and trackers at the DNS level. The board can also be the DNS server for all devices in your home network.

## Features

- **Own Wi-Fi access point** with NAT. The board connects to your home router and shares the internet connection.
- **Several uplink networks.** The board keeps a list of up to 5 known networks. Each network has its own password and its own static-IP settings.
  - The board connects to the strongest known network.
  - When the signal is weaker than −75 dBm, the board moves to a known network that is at least 8 dB stronger.
  - The settings page scans for networks near the board. Add a network with one click.
- **DNS filter.** A blocked domain gets the answer `0.0.0.0`. The board sends all other queries to an upstream resolver (1.1.1.1 by default).
  - Rules: `site.com` (the domain and its subdomains), `*.site.com` (only the subdomains), `*site.com` (any name that ends with `site.com`).
  - You can paste lines from a hosts file (`0.0.0.0 ads.example.com`).
  - Export the rules to a text file. Import rules from a text file in plain, hosts or AdBlock (`||host^`) format.
  - The query log shows each query live, with a **Block** or **Allow** button.
- **Preset blocklist.** One switch turns on a merged list of about 115,000 domains (StevenBlack and HaGeZi Light). The board downloads the list from this repository and checks for a new version each day.
- **Guard.** Guard stops devices that try to go around the filter:
  - It drops DNS queries to other servers (port 53).
  - It drops DNS-over-TLS (port 853, "Private DNS" on Android).
  - It blocks the names of known DNS-over-HTTPS servers. You can edit this list.
  - It tells Firefox to switch off its own DNS-over-HTTPS.
  - Strict mode (optional): it blocks HTTPS to the IP addresses of well-known DNS-over-HTTPS servers.
- **DNS for the whole home network.** Give the board a static IP. Then set this IP as the DNS server in the DHCP settings of your router.
- **Web panel.**
  - Pages: status, filter, guard and settings.
  - The status page updates live. It shows graphs for the last hour: CPU load, free memory, clients, signal and DNS queries.
  - The DNS page shows the top 10 blocked domains and the top 10 clients.
  - A password for the panel is optional. The panel has CSRF protection.
- **Updates from GitHub.** The settings page checks for the latest release and installs it over HTTPS. You can also upload a `firmware.bin` file from the browser. If the update fails, the old firmware continues to run.
- **Password reset without a computer.** Hold the **BOOT** button for 5 seconds while the board runs. The board resets only the passwords and keeps all other settings.
- **Status LED.** Orange: starting. Blue: no router. Yellow: no internet. Green: OK. Purple: update in progress.

## Hardware

Board: [Waveshare ESP32-S3-Zero](https://www.waveshare.com/wiki/ESP32-S3-Zero) (ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM, native USB). Other ESP32-S3 boards with 4 MB flash or more can work with small changes, for example a different LED pin. The preset blocklist needs PSRAM.

## Quick start

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE** extension. Open this folder.
2. Connect the board with USB and push **Upload** (→). After the first installation, you can update over Wi-Fi.
3. Connect to the Wi-Fi network **`ESP32-Portal`** (password `12345678`). Open **http://192.168.4.1**.
4. Go to **Settings**. Add your home Wi-Fi network, so the board can get to the internet.
5. Change the password of the access point.

### Use the board as the DNS server of your home network

1. In **Settings**, edit your home network. Open **Advanced: static IP**. Set an IP address that is outside the DHCP range of your router.
2. In the router, set this IP address as the DNS server in the DHCP settings.
3. On a computer, run `nslookup doubleclick.net <board IP>`. The answer must be `0.0.0.0`.

You can add a second DNS server, for example 8.8.8.8. Then the internet continues to work when the board is off. But devices often send queries to both servers, so some ads can get through.

## Updating

- **Online:** go to **Settings → System** and push **Check for updates**. If a newer release is available, push **Install**.
- **File:** go to **Settings → System** and select a `firmware.bin` file. Get the file from a [release](../../releases) or from `.pio/build/esp32-s3-zero/` after a build.

The update keeps all settings and rule lists.

Version 0.2.0 changed the partition table. A board with older firmware needs one installation over USB. This installation erases the rule lists, so export them first and import them after the installation.

## Releases

A GitHub Actions workflow (`.github/workflows/release.yml`) makes the releases. When you push a version tag, the workflow compiles the firmware. Then it publishes the file `firmware.bin` in a new release.

```
git tag v0.x.y && git push --tags
```

A tag with a suffix (for example `v0.x.y-rc1`) makes a pre-release. The update check of the panel ignores pre-releases.

## Blocklists

Git does not keep the preset blocklist. The workflow `.github/workflows/blocklists.yml` builds it. Start the workflow manually: **Actions → blocklists → Run workflow**.

1. The workflow runs `scripts/build_lists.py`.
2. The script downloads the upstream lists from `blocklists/sources.json`.
3. If no upstream list changed, the workflow stops and does nothing.
4. If a list changed, the script merges all lists into one file `p5.bin`. The file holds sorted 48-bit hashes of about 115,000 domains (about 675 KB).
5. The workflow publishes `p5.bin` and `index.json` in the [`lists`](../../releases/tag/lists) pre-release.

The board loads the list into PSRAM. One lookup takes less than 0.2 ms.

| List | License |
|---|---|
| [StevenBlack hosts](https://github.com/StevenBlack/hosts) (base) | MIT |
| [HaGeZi Multi LIGHT](https://github.com/hagezi/dns-blocklists) | GPL-3.0 |

Each list keeps its upstream license. The workflow publishes the license texts and `NOTICE.txt` together with the list.

To build the list on your computer, run `uv run scripts/build_lists.py`. The output goes to `blocklists/out/`.

## Project layout

```
src/
  main.cpp          setup() and loop() only
  app_config.*      settings in NVS, validation
  app_network.*     access point, uplink networks, roaming, NAT, internet check
  app_board.*       status LED, password reset with the BOOT button
  dns_filter.*      DNS server: filter, forwarding, query log
  rule_list.*       rule list on LittleFS (blocklist, DoH servers), export and import
  preset_lists.*    preset blocklist: download, check, lookup in PSRAM
  net_fetch.*       HTTPS downloads in a background task (updates, blocklist)
  metrics.*         graph data for the last hour, top 10 tables
  anti_bypass.*     Guard: blocks bypass attempts at packet level
  i18n*             table of UI texts
  ring.h            small ring buffer for logs
  web/              web panel: shared frame, CSS, JS, one file per page
scripts/            build helpers (Python)
partitions_p5.csv   flash layout: 2 x 1.5 MB firmware, 960 KB LittleFS
```

## Limitations

- DNS over UDP only. Very large answers (more than 1232 bytes) need a retry over TCP. These queries fail. Such answers are rare.
- Your own blocklist is in RAM. It holds a few thousand rules (the limit is 5000 rules).
- The board has one radio. The access point uses the same channel as the router. When the channel changes, clients reconnect for a short time.
- Guard uses an internal ESP-IDF API. Check Guard again after a platform update.
- Some routers or internet providers drop DNS queries to public resolvers when many queries come at the same time. If you see many failed queries, use the router as the upstream resolver.

## License

[MIT](LICENSE) © 2026 san40pan40
