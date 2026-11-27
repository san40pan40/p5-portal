# Changelog

This file lists the changes in each release of P5 Portal.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The version numbers follow [Semantic Versioning](https://semver.org/).

The release workflow copies the section of a version into the notes of its GitHub release. Add each change to **Unreleased** first. Before you push a version tag, rename **Unreleased** to that version.

## [Unreleased]

### Added
- Preset blocklist. One switch on the DNS page turns on a merged list of about 115,000 domains (StevenBlack and HaGeZi Light).
  - The board downloads the list from the `lists` release and keeps it in PSRAM.
  - The board checks for a new version of the list each day.
- `CHANGELOG.md`. The release workflow uses it for the release notes.

### Changed
- The panel is in English only. The language switch is removed. This makes the firmware 15 KB smaller.
- During a daily update of the preset blocklist, the old list continues to block. If the download fails, the board tries again after 1 hour.
- The DNS page shows the download progress of the preset blocklist without a manual reload.
- The update check shows "older than this build" when the running firmware is newer than the latest release.
- README: new text that describes the current features.

## [0.2.0] - 2026-11-27

### Important
- This version changes the partition table: 2 × 1.5 MB for firmware, 960 KB for files.
- A board with older firmware needs one installation over USB. OTA cannot change the partition table.
- The USB installation erases the rule lists. The settings stay.

### Added
- Several uplink networks (up to 5). Each network has its own password and its own static-IP settings.
- Roaming: the board connects to the strongest known network. On a weak signal it moves to a stronger known network.
- Scan for nearby networks on the settings page. Add a network with one click.
- Update check and installation of the latest GitHub release over HTTPS.
- Firmware version on the settings and status pages.
- Export and import of rule lists (plain, hosts and AdBlock format).
- Graphs for the last hour: CPU load, free memory, clients, signal, DNS queries.
- Top 10 blocked domains and top 10 clients on the DNS page.

### Changed
- New settings page. Each card has its own **Save** button. Only a change of the access point restarts the board.
- One layout system for all pages: equal spacing, equal control height, switches instead of on/off buttons.
- The status page updates live and does not reload.
- The board sends pages in parts and sends compressed CSS and JS. This uses less memory.

### Fixed
- The uptime counter no longer goes back to zero after 49.7 days.
- A very large rule list no longer causes a restart loop. The limit is 5000 rules.

## 0.1.0

First version (no tag): access point with NAT, DNS filter, Guard, web panel, OTA upload from the browser.

[Unreleased]: https://github.com/san40pan40/p5-portal/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/san40pan40/p5-portal/releases/tag/v0.2.0
