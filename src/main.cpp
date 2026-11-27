/*
 * P5 Portal — Wi-Fi access point with DNS-based ad blocking ("mini Pi-hole") on an ESP32-S3.
 * Board: Waveshare ESP32-S3-Zero. Framework: Arduino-ESP32 3.x (PlatformIO + pioarduino).
 *
 * Modules:
 *   app_config  — settings in NVS + validation
 *   app_network — access point, home router uplink, NAT, internet checks
 *   app_board   — status LED and BOOT button (password reset)
 *   (the app_ prefix avoids clashes with framework headers: on Windows "network.h" == "Network.h")
 *   dns_filter  — DNS server with blocklists (rule_list) and the Firefox DoH canary
 *   anti_bypass — "Guard": drops attempts to bypass our DNS (ports 53/853, DoH)
 *   web/        — web panel pages
 */
#include <Arduino.h>
#include <LittleFS.h>
#include "app.h"
#include "app_config.h"
#include "app_network.h"
#include "app_board.h"
#include "anti_bypass.h"
#include "metrics.h"
#include "web/web.h"

WebServer server(80);
DnsFilter dns;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== P5 Portal start ===");
  Board::begin();
  Config::load();

  // Filesystem for the rule lists. true = format on first boot
  if (!LittleFS.begin(true)) Serial.println("[FS] LittleFS mount failed!");

  Net::begin();

  // DNS on port 53 (AP clients + home network); allowed queries go to the upstream resolver
  dns.loadLists();
  dns.begin(Config::AP_IP, Config::AP_MASK);
  AntiBypass::begin(Config::AP_IP);         // frame hook for AP clients
  Config::loadDns();
  Serial.printf("[DNS] filter %s, rules: %u, DoH servers: %u, upstream %s\n",
                dns.enabled() ? "on" : "off", (unsigned)dns.blocklist().size(),
                (unsigned)dns.bypassList().size(), dns.upstream().toString().c_str());

  Web::begin();
}

void loop() {
  dns.loop();
  server.handleClient();
  Net::loop();
  Board::loop();
  AntiBypass::loop();
  Metrics::loop();                          // chart samples every 10 s
  delay(1);                                 // yield the CPU to other tasks (Wi-Fi, netcheck)
}
