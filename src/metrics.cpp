#include "metrics.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "app.h"
#include "app_network.h"
#include "ring.h"

namespace Metrics {

struct SysSample { uint8_t c0, c1, cl; int8_t rs; uint16_t heap; };
struct DnsSample { uint16_t q, b, x, ms; };
static const uint16_t NO_MS = 0xFFFF;               // no upstream answers in the interval

static Ring<SysSample, POINTS> sys;              // 6 B x 360
static Ring<DnsSample, POINTS> dnsH;             // 8 B x 360
static TopDomain topDom[TOP_SLOTS];
static TopIp     topIp[TOP_SLOTS];

// Space-Saving insert: bump a matching entry, or replace the smallest one (empty = 0)
template <class E, class Match, class Set>
static void topAdd(E *t, Match match, Set set) {
  int slot = 0;
  for (int i = 0; i < TOP_SLOTS; i++) {
    if (t[i].n && match(t[i])) { t[i].n++; return; }
    if (t[i].n < t[slot].n) slot = i;
  }
  uint32_t base = t[slot].n;
  set(t[slot]);
  t[slot].n = base + 1;
}

// Indices of the 10 largest entries (selection; 32 entries is tiny)
template <class E>
static int topSort(const E *t, const E *out[10]) {
  bool used[TOP_SLOTS] = {};
  int k = 0;
  for (; k < 10; k++) {
    int best = -1;
    for (int i = 0; i < TOP_SLOTS; i++)
      if (!used[i] && t[i].n && (best < 0 || t[i].n > t[best].n)) best = i;
    if (best < 0) break;
    used[best] = true;
    out[k] = &t[best];
  }
  return k;
}

void dnsQuery(uint32_t clientIp, const char *blocked) {
  topAdd(topIp, [&](const TopIp &e) { return e.ip == clientIp; }, [&](TopIp &e) { e.ip = clientIp; });
  if (!blocked) return;
  topAdd(topDom, [&](const TopDomain &e) { return strncmp(e.d, blocked, DOMAIN_LEN - 1) == 0; },
         [&](TopDomain &e) { strlcpy(e.d, blocked, DOMAIN_LEN); });
}

int topBlocked(const TopDomain *out[10]) { return topSort(topDom, out); }
int topClients(const TopIp *out[10]) { return topSort(topIp, out); }

static uint16_t clamp16(uint32_t v) { return v > 65534 ? 65534 : v; }

// CPU load from FreeRTOS run-time stats (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, esp_timer µs):
// load = 100% - share of the interval the core's idle task ran. Two counter reads per sample.
void loop() {
  static uint32_t at = 0, idle[2], q, b, x, upMs, upN;
  static int64_t t0;
  static bool started = false;
  uint32_t now = millis();
  if (started && now - at < INTERVAL_S * 1000) return;
  at = now;
  int64_t t = esp_timer_get_time();
  uint32_t i0 = ulTaskGetIdleRunTimeCounterForCore(0), i1 = ulTaskGetIdleRunTimeCounterForCore(1);
  if (started) {
    uint32_t el = (uint32_t)(t - t0);            // µs; the idle counters are 32-bit µs (wrap-safe deltas)
    auto load = [el](uint32_t d) -> uint8_t { return d >= el ? 0 : 100 - (uint64_t)d * 100 / el; };
    SysSample &s = sys.next();
    s.c0 = load(i0 - idle[0]);
    s.c1 = load(i1 - idle[1]);
    s.cl = WiFi.softAPgetStationNum();
    s.rs = Net::staConnected() ? WiFi.RSSI() : 0;
    s.heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;

    DnsSample &d = dnsH.next();
    uint32_t dn = dns.statUpN - upN;
    d.q = clamp16(dns.statTotal - q);
    d.b = clamp16(dns.statBlocked - b);
    d.x = clamp16(dns.statFailed - x);
    d.ms = dn ? clamp16((dns.statUpMs - upMs) / dn) : NO_MS;
  }
  started = true;
  t0 = t;
  idle[0] = i0;
  idle[1] = i1;
  q = dns.statTotal; b = dns.statBlocked; x = dns.statFailed;
  upMs = dns.statUpMs; upN = dns.statUpN;
}

// Value of series k (SERIES order) in sample i (0 = newest); INT32_MIN = no value (null)
static const char *const SERIES[2][5] = {{"c0", "c1", "ram", "cl", "rs"}, {"q", "b", "x", "ms", nullptr}};
static int32_t value(bool isDns, int k, int i) {
  if (isDns) {
    const DnsSample &s = dnsH.at(i);
    return k == 0 ? s.q : k == 1 ? s.b : k == 2 ? s.x : s.ms == NO_MS ? INT32_MIN : s.ms;
  }
  const SysSample &s = sys.at(i);
  return k == 0 ? s.c0 : k == 1 ? s.c1 : k == 2 ? s.heap : k == 3 ? s.cl : s.rs ? s.rs : INT32_MIN;
}

// {"iv":10,"n":count,"<key>":[oldest..newest],...}; the last element is "now".
// Streamed in ~1 KB chunks from a stack buffer, so the reply never needs one big String.
void sendHist(bool isDns) {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  char o[1100];
  int cnt = isDns ? dnsH.count() : sys.count();
  int n = snprintf(o, sizeof(o), "{\"iv\":%d,\"n\":%d", INTERVAL_S, cnt);
  for (int k = 0; k < 5 && SERIES[isDns][k]; k++) {
    n += snprintf(o + n, sizeof(o) - n, ",\"%s\":[", SERIES[isDns][k]);
    for (int i = cnt - 1; i >= 0; i--) {
      if (n > 1000) { server.sendContent(o, n); n = 0; }
      int32_t v = value(isDns, k, i);
      n += snprintf(o + n, sizeof(o) - n, v == INT32_MIN ? "%snull" : "%s%d", i == cnt - 1 ? "" : ",", (int)v);
    }
    o[n++] = ']';
  }
  o[n++] = '}';
  server.sendContent(o, n);
  server.sendContent("");
}

}  // namespace Metrics
