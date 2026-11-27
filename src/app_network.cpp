#include "app_network.h"
#include <WiFi.h>
#include "app.h"
#include "app_config.h"

using Config::settings;

namespace Net {

const IPAddress PROBE(1, 1, 1, 1);

// ---- Uplink state. Written only by the Wi-Fi event handler (Arduino events task) ----
static volatile bool     s_staConnected  = false;
static volatile bool     s_natOn         = false;
static volatile bool     s_addrsDirty    = false;   // board/router addresses changed -> tell the DNS filter
static volatile uint32_t s_staSince      = 0;
static volatile uint32_t s_staDrops      = 0;

// ---- Internet check result. Written only by checkTask; the three fields change together ----
static Check         s_check = {false, -1, 0};
static portMUX_TYPE  s_mux   = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t  s_task  = nullptr;

bool     staConnected() { return s_staConnected; }
bool     natOn()        { return s_natOn; }
uint32_t staSince()     { return s_staSince; }
uint32_t staDrops()     { return s_staDrops; }

Check lastCheck() {
  portENTER_CRITICAL(&s_mux);
  Check c = s_check;
  portEXIT_CRITICAL(&s_mux);
  return c;
}

bool internetOk()   { return s_staConnected && lastCheck().ok; }
void requestCheck() { if (s_task) xTaskNotifyGive(s_task); }

// connect() blocks for up to 2 s, so it runs in its own FreeRTOS task and never stalls
// DNS or the web server. The task sleeps until woken (requestCheck) or for 30 s.
static void checkTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    if (!s_staConnected) continue;
    WiFiClient c;
    uint32_t t = millis();
    bool ok = c.connect(PROBE, 443, 2000);
    Check r = {ok, ok ? (int)(millis() - t) : -1, millis()};
    c.stop();
    portENTER_CRITICAL(&s_mux);
    bool changed = r.ok != s_check.ok;
    s_check = r;
    portEXIT_CRITICAL(&s_mux);
    if (changed) Serial.printf("[NET] internet %s\n", ok ? "up" : "DOWN");
  }
}

// The Wi-Fi driver tells us when the uplink appears or disappears
static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    s_staConnected = true;
    s_staSince = millis();
    s_addrsDirty = true;
    IPAddress ip = WiFi.localIP();
    Serial.printf("[STA] connected, IP=%s, RSSI %d dBm\n", ip.toString().c_str(), WiFi.RSSI());
    if (Config::inApSubnet(ip))
      Serial.println("[STA] WARNING: router network overlaps the AP network — addresses will conflict");
    if (!s_natOn) {                                // enable NAT once
      s_natOn = WiFi.AP.enableNAPT(true);
      Serial.printf("[NAT] %s\n", s_natOn ? "enabled" : "FAILED");
    }
    requestCheck();
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && s_staConnected) {
    s_staConnected = false;
    s_staDrops = s_staDrops + 1;
    Serial.println("[STA] uplink lost");
  }
}

void begin() {
  const Config::Settings &s = settings;

  // Create the check task BEFORE Wi-Fi, so the "connected" event can already wake it
  xTaskCreate(checkTask, "netcheck", 6144, nullptr, 1, &s_task);

  WiFi.onEvent(onWifiEvent);
  WiFi.mode(s.staSsid.length() ? WIFI_AP_STA : WIFI_AP);

  // 1) Our access point. If it fails with the stored settings, fall back to the factory
  //    defaults for this boot only (nothing is written to NVS).
  if (!WiFi.softAP(s.apSsid.c_str(), s.apPass.c_str())) {
    Serial.println("[AP]  failed with stored settings — starting with factory defaults");
    WiFi.softAP(Config::DEFAULT_AP_SSID, Config::DEFAULT_AP_PASS);
  }
  // DHCP gives clients gateway AND DNS = AP_IP, i.e. this board
  WiFi.softAPConfig(Config::AP_IP, Config::AP_IP, Config::AP_MASK, Config::DHCP_START, Config::AP_IP);
  Serial.printf("[AP]  SSID=\"%s\"  IP=%s\n", WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str());

  // 2) Home router. We do not wait here: onWifiEvent reports the connection.
  if (s.staSsid.length()) {
    Serial.printf("[STA] connecting to \"%s\"...\n", s.staSsid.c_str());
    if (s.st.on) {
      // The board's own resolver is a public DNS, NOT the router: if the router uses the
      // board as its DNS server, that would be a loop.
      WiFi.config(s.st.ip, s.st.gw, s.st.mask, DnsFilter::defaultUpstream());
      Serial.printf("[STA] static IP %s, gateway %s\n", s.st.ip.toString().c_str(), s.st.gw.toString().c_str());
    }
    WiFi.begin(s.staSsid.c_str(), s.staPass.c_str());
  }
  // With a static IP the addresses are known up front — loop protection works even
  // before the router connects
  s_addrsDirty = s.st.on;
}

void loop() {
  // Feed the board/router addresses to the DNS filter so it never forwards to them (loop).
  // Done here rather than in the event handler, so `dns` is only touched from one task.
  if (s_addrsDirty) {
    s_addrsDirty = false;
    const Config::StaticIp &st = settings.st;
    IPAddress self = st.on ? st.ip : WiFi.localIP();
    IPAddress gw   = st.on ? st.gw : WiFi.gatewayIP();
    if (dns.setStaAddrs(self, gw))
      Serial.println("[DNS] upstream pointed at the board/router (loop) — reset to 1.1.1.1");
  }

  // Safety net: if the router stays away, ask the driver to retry every 30 s
  static uint32_t lastRetry = 0;
  if (settings.staSsid.length() && !s_staConnected && millis() - lastRetry > 30000) {
    lastRetry = millis();
    WiFi.reconnect();
  }
}

}  // namespace Net
