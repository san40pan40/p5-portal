#pragma once
/*
 * PresetLists — big ready-made blocklists (StevenBlack, HaGeZi...) built by scripts/build_lists.py
 * and downloaded from the GitHub release "lists" into LittleFS (/bl/<id>.bin).
 *
 * A list file is a sorted array of FNV-1a hashes of domains, 64-bit or folded to 48-bit (hashBits in
 * the header; format: see build_lists.py). An active list is loaded into PSRAM as is (6 or 8 bytes
 * per entry) and a lookup is a binary search there:
 * reading 1 KB blocks from LittleFS per lookup was measured at ~1.7 ms per suffix (too slow).
 * Every entry also blocks its subdomains: match() walks the parents of the queried name.
 *
 * Everything except the download runs on the loop() thread (DNS + web): loop() is a small
 * scheduler that starts NetFetch jobs (index.json, then list files), and swaps a downloaded
 * file in (close, rename, reopen) only here, so a lookup never sees a half-written file.
 * Enabled list ids are stored in NVS (namespace "portal", key "bl", comma-separated).
 */
#include <Arduino.h>
#include "net_fetch.h"

namespace PresetLists {

constexpr uint8_t MAX_LISTS = 4;
constexpr size_t  HDR_SIZE  = 16;
constexpr size_t  FS_RESERVE = 64 * 1024;   // kept free in LittleFS for the user's rule lists

enum State : uint8_t { OFF, QUEUED, LOADING, READY, FAILED };

struct Info {                     // snapshot for the web page
  const char *id, *name, *license, *homepage, *updated;   // "" = unknown
  State    state;
  uint32_t count, bytes;          // local file if present, otherwise index.json values (0 = unknown)
  uint8_t  progress;              // LOADING: 0..100
  bool     active;                // the data is in PSRAM and blocks names (FAILED too: the old data)
  NetFetch::Status err;           // FAILED: why (the list tries again in an hour)
};

void begin();                     // after LittleFS.begin(): open enabled lists, read cached index.json
void loop();                      // scheduler: daily index check, downloads, swap
int  listCount();
Info info(int i);
int  find(const char *id);        // -1 = unknown id
// Turn a list on (queue the download) or off (close + delete the file). On: returns 0, or the KB the
// file needs when it does not fit (needKb, then nothing changes); freeKb() tells what is free.
uint32_t setEnabled(int i, bool on);
uint32_t freeKb();                // LittleFS free space minus the reserve, KB
uint32_t totalKb();
bool busy();                      // a list job is running
bool pending();                   // a job runs or a list waits for its download (the page polls /api/update)
uint32_t stateCode();             // changes when a list state changes or a job starts or ends

// The list that blocks this name (lowercase, no trailing dot), or -1. off = start of the matching
// suffix in name (the "rule").
int match(const char *name, size_t n, size_t &off);
const char *id(int i);

// Header check of a list file (16 bytes) against its total length; crc = payload crc32 from it.
// Returns the entry size (8 = 64-bit hashes, 6 = 48-bit), 0 = invalid.
uint8_t validHeader(const uint8_t *h, size_t fileLen, uint32_t &count, uint32_t &crc);

}  // namespace PresetLists
