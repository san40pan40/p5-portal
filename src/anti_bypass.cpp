#include "anti_bypass.h"
#include "ring.h"
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_private/wifi.h>   // esp_wifi_internal_reg_rxcb, esp_wifi_internal_free_rx_buffer
#include <iterator>             // std::size

namespace AntiBypass {

// Well-known public DoH servers (Google, Cloudflare, Quad9, AdGuard, OpenDNS)
static const uint8_t DOH_IPS[][4] = {
  {8, 8, 8, 8},     {8, 8, 4, 4},          {1, 1, 1, 1},     {1, 0, 0, 1},
  {9, 9, 9, 9},     {149, 112, 112, 112},  {94, 140, 14, 14}, {94, 140, 15, 15},
  {208, 67, 222, 222}, {208, 67, 220, 220},
};
static const int DOH_COUNT = std::size(DOH_IPS);

static uint32_t      s_apIp = 0;
static uint32_t      s_dohIps[DOH_COUNT];
static esp_netif_t  *s_netif = nullptr;
static volatile bool s_enabled = true;
static volatile bool s_blockDohIps = false;
static volatile uint32_t s_cnt53 = 0, s_cnt853 = 0, s_cntDoh = 0;

// The log is written from the Wi-Fi driver task and read by the web server — hence the lock
static portMUX_TYPE        s_mux = portMUX_INITIALIZER_UNLOCKED;
static Ring<Attempt, 20>   s_log;

static void note(uint8_t client, uint32_t dst, uint16_t port) {
  portENTER_CRITICAL(&s_mux);
  s_log.push({client, dst, port, millis()});
  portEXIT_CRITICAL(&s_mux);
}

static bool isDohIp(uint32_t ip) {
  for (int i = 0; i < DOH_COUNT; i++)
    if (s_dohIps[i] == ip) return true;
  return false;
}

// Should this frame be dropped? Frame = Ethernet II: [dst MAC 6][src MAC 6][type 2][IPv4...]
static bool shouldDrop(const uint8_t *b, uint16_t len) {
  if (len < 14 + 20 + 4) return false;
  if (b[12] != 0x08 || b[13] != 0x00) return false;    // not IPv4
  const uint8_t *ip = b + 14;
  if ((ip[0] >> 4) != 4) return false;
  int ihl = (ip[0] & 0x0F) * 4;                        // IP header length
  if (ihl < 20 || 14 + ihl + 4 > len) return false;
  if (((ip[6] & 0x1F) << 8) | ip[7]) return false;     // not the first fragment — no ports
  uint8_t proto = ip[9];
  if (proto != 6 && proto != 17) return false;         // TCP and UDP only

  uint32_t dst;
  memcpy(&dst, ip + 16, 4);                            // same byte order as IPAddress
  if (dst == s_apIp) return false;                     // addressed to us — always allow

  const uint8_t *l4 = ip + ihl;
  uint16_t dport = (l4[2] << 8) | l4[3];
  uint8_t client = ip[15];                             // last byte of the sender IP

  if (dport == 53)  { s_cnt53  = s_cnt53 + 1;  note(client, dst, dport); return true; }
  if (dport == 853) { s_cnt853 = s_cnt853 + 1; note(client, dst, dport); return true; }
  if (dport == 443 && s_blockDohIps && isDohIp(dst)) {
    s_cntDoh = s_cntDoh + 1;
    note(client, dst, dport);
    return true;
  }
  return false;
}

// Our hook: the Wi-Fi driver calls it for EVERY frame from AP clients.
// It must be very fast — just a few comparisons.
static esp_err_t rxHook(void *buffer, uint16_t len, void *eb) {
  if (s_enabled && shouldDrop((const uint8_t *)buffer, len)) {
    esp_wifi_internal_free_rx_buffer(eb);              // dropped: free the driver buffer
    return ESP_OK;
  }
  return esp_netif_receive(s_netif, buffer, len, eb);  // as usual — into the network stack
}

// (Re)register the hook with the driver
static void attach() {
  s_netif = WiFi.AP.netif();
  if (s_netif) esp_wifi_internal_reg_rxcb(WIFI_IF_AP, rxHook);
}

void begin(IPAddress apIp) {
  s_apIp = (uint32_t)apIp;
  for (int i = 0; i < DOH_COUNT; i++)
    s_dohIps[i] = (uint32_t)IPAddress(DOH_IPS[i][0], DOH_IPS[i][1], DOH_IPS[i][2], DOH_IPS[i][3]);
  attach();
}

// ESP-IDF registers its own handler every time the AP (re)starts, overwriting ours.
// The simple, robust fix is to put ours back every 5 s (it is a single pointer write).
void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000) {
    last = millis();
    attach();
  }
}

void setEnabled(bool on)    { s_enabled = on; }
bool enabled()              { return s_enabled; }
void setBlockDohIps(bool on) { s_blockDohIps = on; }
bool blockDohIps()          { return s_blockDohIps; }

uint32_t blocked53()  { return s_cnt53; }
uint32_t blocked853() { return s_cnt853; }
uint32_t blockedDoh() { return s_cntDoh; }

int logCount() {
  portENTER_CRITICAL(&s_mux);
  int n = s_log.count();
  portEXIT_CRITICAL(&s_mux);
  return n;
}

Attempt logAt(int i) {
  portENTER_CRITICAL(&s_mux);
  Attempt a = s_log.at(i);
  portEXIT_CRITICAL(&s_mux);
  return a;
}

}  // namespace AntiBypass
