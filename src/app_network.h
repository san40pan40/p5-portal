#pragma once
/*
 * Network — our access point (AP) plus the optional uplink to the home router (STA), NAT,
 * Wi-Fi events, internet reachability checks and automatic reconnects.
 *
 *   phone --Wi-Fi--> ESP32-Portal (192.168.4.1) --Wi-Fi--> home router --> internet
 *
 * DHCP hands AP clients 192.168.4.1 as gateway AND DNS server, which is how every DNS query
 * reaches our filter. NAT (NAPT) forwards client traffic to the router.
 */
#include <Arduino.h>

namespace Net {

struct Check {          // result of the last internet check
  bool     ok;
  int      ms;          // TCP connect latency, ms
  uint32_t at;          // millis() of the check (0 = never)
};

extern const IPAddress PROBE;   // where the internet check connects to (1.1.1.1:443)

void begin();           // start AP (+ STA if configured) and the internet-check task
void loop();            // reconnect watchdog + feeding router addresses to the DNS filter

bool     staConnected();
bool     natOn();
uint32_t staSince();    // millis() when the uplink came up
uint32_t staDrops();    // how many times the uplink was lost
Check    lastCheck();
bool     internetOk();
void     requestCheck();  // wake the check task now

}  // namespace Net
