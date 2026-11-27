#pragma once
/*
 * AntiBypass — the "Guard": stops AP clients from bypassing the DNS filter.
 *
 * The packet part intercepts frames from AP clients at the Wi-Fi driver level (before NAT) and drops:
 *   - UDP/TCP to port 53 not addressed to us — manually configured DNS (8.8.8.8 etc.)
 *   - UDP/TCP to port 853                     — DNS-over-TLS (Android "Private DNS")
 *   - (optional) TCP/UDP 443 to known DoH server IPs — DNS-over-HTTPS with a hard-coded IP
 * After being refused, most devices fall back to the DNS server from DHCP, i.e. us.
 * The DNS part (DoH servers by name, Firefox canary) is done by DnsFilter while enabled().
 *
 * Note: this uses an internal ESP-IDF API (esp_wifi_internal_reg_rxcb). It works, but is not
 * guaranteed across framework versions — re-check after updating the platform.
 */
#include <Arduino.h>

namespace AntiBypass {

struct Attempt {          // one bypass attempt
  uint8_t  client;        // last byte of the AP client's IP
  uint32_t dst;           // where it tried to go (same byte order as IPAddress)
  uint16_t port;          // 53 / 853 / 443
  uint32_t ms;            // millis()
};

void begin(IPAddress apIp);           // remember our address and install the hook
void loop();                          // re-registers the hook periodically (see the .cpp)

// Enabled flag — the single source of truth for both the packet part and DnsFilter
void setEnabled(bool on);
bool enabled();
void setBlockDohIps(bool on);
bool blockDohIps();

uint32_t blocked53();
uint32_t blocked853();
uint32_t blockedDoh();
int      logCount();
Attempt  logAt(int i);                // 0 = newest

}  // namespace AntiBypass
