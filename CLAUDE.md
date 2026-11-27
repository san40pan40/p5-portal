# P5 Portal — notes for Claude

ESP32-S3 Wi-Fi AP + NAT + DNS ad filter + web panel. Board: Waveshare ESP32-S3-Zero (4 MB flash, 2 MB PSRAM, WS2812 on GPIO21).
Public repo: github.com/san40pan40/p5-portal (MIT). Plan / roadmap: `TODO.md` — read it only when picking the next task.

## Rules
- Reply to the user in **Ukrainian**; code, comments, docs, commits in **English**.
- UI is **English only** (Ukrainian was dropped to save flash; no downloadable languages). Never hard-code text in pages — add it to `src/i18n_strings.h` and use `T(ID)`.
- Never commit or read `dump/`, `tools/`, `.pio/` (third-party firmware dumps, backups, build output).
- Commit identity is repo-local (already set). **Claude pushes** after each commit (`GIT_TERMINAL_PROMPT=0 git push`). Tags / GitHub releases / running workflows publish things — ask first.
- Local scripts: **Python, run with `uv run scripts/<x>.py`** (PEP 723 header, stdlib), must work on Windows and macOS (the user will continue on a Mac). No new PowerShell/bash scripts.
- GitHub CLI: `gh` (Windows: `C:\Program Files\GitHub CLI\gh.exe`), logged in as the repo owner.
- Keep changes small and in the existing style; no new libraries without asking.
- Docs (README, TODO, CLAUDE.md, release notes) and code comments: **ASD-STE100 Simplified Technical English** — always use the `/asd-ste100` skill when writing them. If the skill is not available: short sentences (≤ 20 words for instructions, ≤ 25 for descriptions), active voice, imperative for instructions, one topic per sentence, one meaning per word, no idioms.
- **CHANGELOG.md** (Keep a Changelog): add every user-visible change to `## [Unreleased]` in the same commit. Before a version tag, rename Unreleased to `## [X.Y.Z] - date`. The release workflow uses that section as the release notes (Markdown, one item per line — never one long line).
- When the user states a lasting rule or preference ("from now on", "always", tooling, language, workflow), record it here in Rules right away.

## Decisions (keep unless the user changes them)
- Partition table `partitions_p5.csv`: 2 × 1.5 MB app, 960 KB LittleFS, no coredump. Changing it needs a USB flash and wipes LittleFS (export rules first).
- Updates: HTTPS from GitHub releases (`/releases/latest` redirect), built-in Mozilla CA bundle. Releases are built by `.github/workflows/release.yml` on `v*` tags.
- Preset blocklist: one merged list `p5.bin` (48-bit hashes) built by `scripts/build_lists.py` via the manual `blocklists` workflow, published to the `lists` pre-release (never "latest"); rebuilt only when an upstream list changed. Board keeps it in PSRAM.
- No ArduinoJson: own minimal JSON reader. Own small UI component system in `src/web/style.css` (spacing scale, tags, switches, button roles) — reuse it, keep pages consistent.

## Workflow (the main session is the curator — the user should not need to manage models)
When the user says "далі", "next", "зроби пункт N" or describes a feature:
1. Pick the task (next unchecked item in `TODO.md` if not named). Do not read the code yourself beyond what is needed to split it.
2. Non-trivial (several modules, new storage format, networking, tight flash/RAM) → **p5-architect** (Opus) for a plan. Trivial → skip.
3. Each step → **p5-coder** (Sonnet): pass the step text + the relevant plan lines; it builds, flashes and checks.
   Independent steps may run in parallel only if they touch different files.
4. Feature finished → **p5-reviewer** once; send real findings back to p5-coder.
5. Commit (English message), update `TODO.md` (tick the item), push, tell the user in Ukrainian in ≤ 8 lines what changed.
Small questions, one-line fixes, explanations → answer directly, no agents.
Escalate to the user only for real decisions (UX choice, breaking settings format, flash space running out).
When a feature is done, suggest `/clear` before the next one (fresh context is cheaper).

## Build / flash / test (use the script — it prints only what matters)
```
powershell -File scripts/dev.ps1            # build + OTA + smoke test (default)
powershell -File scripts/dev.ps1 build      # compile only (errors + RAM/Flash summary)
powershell -File scripts/dev.ps1 check      # smoke test only: pages 200 + DNS blocks doubleclick.net
powershell -File scripts/dev.ps1 usb        # first flash / recovery over USB
```
Board on the home LAN: `192.168.50.62` (override `-Ip`); panel password via `-Pass` or `$env:P5_PASS` (none now).
pio: `C:\Users\san40\.platformio\penv\Scripts\pio.exe` (Python 3.11 penv). Flash use ≈ 90% of the 1.5 MB app partition — watch it.
(`dev.ps1` is being ported to `uv run scripts/dev.py` — prefer that once it exists.)

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
