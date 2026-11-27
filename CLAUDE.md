# P5 Portal — notes for Claude

ESP32-S3 Wi-Fi AP + NAT + DNS ad filter + web panel. Board: Waveshare ESP32-S3-Zero (4 MB flash, 2 MB PSRAM, WS2812 on GPIO21).
Public repo: github.com/san40pan40/p5-portal (MIT). Plan / roadmap: `TODO.md` — read it only when picking the next task.

## Rules
- Reply to the user in **Ukrainian**; code, comments, docs, commits in **English**.
- UI strings: never hard-code text in pages — add `X(ID, "uk", "en")` to `src/i18n_strings.h` and use `T(ID)`.
- Never commit or read `dump/`, `tools/`, `.pio/` (third-party firmware dumps, backups, build output).
- Commit identity is repo-local (already set). The user pushes (`git push`) — shell cannot enter credentials.
- Keep changes small and in the existing style; no new libraries without asking.

## Build / flash / test (use the script — it prints only what matters)
```
powershell -File scripts/dev.ps1            # build + OTA + smoke test (default)
powershell -File scripts/dev.ps1 build      # compile only (errors + RAM/Flash summary)
powershell -File scripts/dev.ps1 check      # smoke test only: pages 200 + DNS blocks doubleclick.net
powershell -File scripts/dev.ps1 usb        # first flash / recovery over USB
```
Board on the home LAN: `192.168.50.62` (override `-Ip`); panel password via `-Pass` or `$env:P5_PASS` (none now).
pio: `C:\Users\san40\.platformio\penv\Scripts\pio.exe` (Python 3.11 penv). Flash use ≈ 88% of the app partition — watch it.

## Code map (`src/`, ~2.5k lines)
| File | What |
|---|---|
| `main.cpp` | `setup()`/`loop()` only; globals `server`, `dns` (declared in `app.h`) |
| `app_config.*` | `Config::settings` (NVS ns "portal"), defaults, `save`, `resetPasswords`, static-IP validation |
| `app_network.*` | `Net::` AP/STA start, NAPT, Wi-Fi events, internet check task (TCP 1.1.1.1:443) |
| `app_board.*` | `Board::` LED colours, BOOT 5 s → reset passwords |
| `dns_filter.*` | UDP:53 server, upstream forward, log `Ring<DnsLogEntry,60>`, stats, canary |
| `rule_list.*` | `RuleList`: exact set + wildcard vector, suffix walk, LittleFS save (tmp+rename), `page()` |
| `anti_bypass.*` | Wi-Fi RX hook on AP: drop :53/:853 to foreign IPs, optional DoH-IP :443 |
| `i18n*` | X-macro string table, `I18n::setLang/t`, lang in NVS key "lang" |
| `web/web.*` | shared HTML helpers (`sendPage`, `tile`, `row`, `postBtn`, `ruleTable`...), auth (`route`/`guarded`: Origin check + Basic auth admin) |
| `web/page_*.cpp` | `/` status, `/dns` filter, `/bypass` guard, `/config` settings + `/update` OTA |

Adding a page: `page_x.cpp` with `registerXPage()` → declare in `web.h`, call in `Web::begin`, menu entry in `pageStart` (web.cpp).
