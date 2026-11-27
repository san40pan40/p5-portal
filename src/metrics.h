#pragma once
/*
 * Metrics — history for the charts (one sample every 10 s, the last hour) and top-10 tables.
 * Fixed-size buffers only (~6 KB), nothing grows; everything resets on reboot.
 *   sys: CPU load per core (%), free internal heap (KB), AP clients, router RSSI (dBm, 0 = no link)
 *   dns: per-interval queries, blocked, failed, average upstream answer time (ms)
 * Top tables use "Space-Saving": 32 counters; a new key replaces the smallest one (count = min + 1).
 */
#include <Arduino.h>

namespace Metrics {

const int INTERVAL_S = 10;
const int POINTS = 360;                          // 1 hour
const int TOP_SLOTS = 32;
const int DOMAIN_LEN = 40;                       // stored domain length incl. the 0; longer = truncated

struct TopDomain { char d[DOMAIN_LEN]; uint32_t n; };
struct TopIp     { uint32_t ip; uint32_t n; };

void loop();                                     // call from loop(): takes a sample every 10 s
// Called by the DNS filter for every query; blocked = the domain if a rule matched
void dnsQuery(uint32_t clientIp, const char *blocked);

// Top-10 in descending order; returns how many (entries with n > 0)
int topBlocked(const TopDomain *out[10]);
int topClients(const TopIp *out[10]);

void sendHist(bool dns);                         // GET /api/hist body, streamed (oldest -> newest)

}  // namespace Metrics
