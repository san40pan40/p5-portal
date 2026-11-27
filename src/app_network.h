#pragma once
/*
 * Network — our access point (AP) plus the optional uplink to the home router (STA), NAT,
 * Wi-Fi events, internet reachability checks and automatic reconnects.
 *
 *   phone --Wi-Fi--> ESP32-Portal (192.168.4.1) --Wi-Fi--> home router --> internet
 *
 * DHCP hands AP clients 192.168.4.1 as gateway AND DNS server, which is how every DNS query
 * reaches our filter. NAT (NAPT) forwards client traffic to the router.
 *
 * Several known uplinks (Config::settings.nets): while disconnected the board scans every 30 s
 * and joins the strongest known network (pinned to its BSSID); with one known network it just
 * retries, as before. While connected it scans only when the signal is weak (< -75 dBm) and
 * switches if a known AP is >= 8 dB better (120 s hold-off). Scans are shared with the web UI.
 */
#include <Arduino.h>
#include <esp_timer.h>

namespace Net {

struct Check {          // result of the last internet check
  bool     ok;
  int      ms;          // TCP connect latency, ms
  uint32_t at;          // millis() of the check (0 = never)
};

struct Seen {           // one access point from the last scan
  char    ssid[33];
  uint8_t bssid[6];
  int8_t  rssi;
  uint8_t ch;
  bool    open;
};
constexpr uint8_t MAX_SEEN = 20;

extern const IPAddress PROBE;   // where the internet check connects to (1.1.1.1:443)

void begin();           // start AP (+ STA if configured) and the internet-check task
void loop();            // reconnect watchdog + feeding router addresses to the DNS filter

bool     staConnected();
bool     natOn();
uint32_t staSince();    // uptimeSec() when the uplink came up
// Seconds since boot from the 64-bit esp_timer (millis() wraps after 49.7 days)
inline uint32_t uptimeSec() { return esp_timer_get_time() / 1000000; }
uint32_t staDrops();    // how many times the uplink was lost
Check    lastCheck();
bool     internetOk();
void     requestCheck();  // wake the check task now

bool        startScan();          // start an async scan (true if started or already running)
bool        scanning();
const Seen *seen(uint8_t &n);     // last scan: strongest first, one entry per BSSID, no hidden
uint32_t    scanAt();             // millis() of the last finished scan (0 = never)

int8_t   activeNet();     // index in settings.nets of the uplink in use / being tried, -1 = none
// Call after settings.nets changed: re-index, drop a deleted uplink. force: the active entry
// itself was edited (password / static IP) — reconnect to apply it.
void     reselect(bool force = false);

}  // namespace Net
