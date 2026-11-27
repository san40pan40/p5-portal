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
static volatile bool     s_planned       = false;   // the next disconnect is ours (roam / list change)

// ---- Uplink selection and the scan cache. Touched only from loop() / web handlers (one task) ----
static constexpr uint32_t RETRY_MS   = 30000;   // disconnected: re-scan / retry period
static constexpr uint32_t ROAM_MS    = 30000;   // connected: signal check period
static constexpr uint32_t HOLD_MS    = 120000;  // no roaming this soon after a switch
static constexpr int      ROAM_WEAK  = -75;     // dBm: below this, look for a better AP
static constexpr int      ROAM_GAIN  = 8;       // dB: switch only to a clearly better AP
static constexpr uint32_t ROAM_BACKOFF_MIN = 300000;   // a fruitless roam scan: wait 5 min...
static constexpr uint32_t ROAM_BACKOFF_MAX = 900000;   // ...doubling up to 15 min
static constexpr int      ROAM_RESET_DB    = 5;        // RSSI moved this much: back-off reset
static constexpr uint32_t PENDING_MAX_MS   = 2000;     // waiting for our disconnect before begin()

static int8_t   s_active     = -1;      // settings.nets index in use / being tried
static char     s_activeSsid[33] = "";  // its SSID, to find it again after the list changes
static uint32_t s_attemptAt  = 0;       // millis() of the last connect attempt
static uint32_t s_switchAt   = 0;       // millis() of the last switch made while connected
static uint8_t  s_rr         = 0;       // round-robin cursor over nets missing from the scan
static uint8_t  s_cycle      = 0;       // pick cycles since the last connection
static uint32_t s_roamWait   = 0;       // roam back-off (0 = none)
static uint32_t s_roamFailAt = 0;       // millis() of the last fruitless roam scan
static int      s_roamRssi   = 0;       // signal at that time
static int8_t   s_pending    = -1;      // nets index to begin() once the old link is down
static bool     s_pendingAp  = false;   // s_pendingSeen is valid (pin BSSID/channel)
static Seen     s_pendingSeen;
static uint32_t s_pendingAt  = 0;

static Seen     s_seen[MAX_SEEN];
static uint8_t  s_seenN      = 0;
static uint32_t s_scanAt     = 0;
static bool     s_scanning   = false;

// ---- Internet check result. Written only by checkTask; the three fields change together ----
static Check         s_check = {false, -1, 0};
static portMUX_TYPE  s_mux   = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t  s_task  = nullptr;

bool     staConnected() { return s_staConnected; }
bool     natOn()        { return s_natOn; }
uint32_t staSince()     { return s_staSince; }
uint32_t staDrops()     { return s_staDrops; }
int8_t   activeNet()    { return s_active; }
bool     scanning()     { return s_scanning; }
uint32_t scanAt()       { return s_scanAt; }
const Seen *seen(uint8_t &n) { n = s_seenN; return s_seen; }

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
    s_planned = false;
    s_staSince = uptimeSec();
    s_addrsDirty = true;
    IPAddress ip = WiFi.localIP();
    Serial.printf("[STA] connected to \"%s\", IP=%s, RSSI %d dBm\n", WiFi.SSID().c_str(),
                  ip.toString().c_str(), WiFi.RSSI());
    if (Config::inApSubnet(ip))
      Serial.println("[STA] WARNING: router network overlaps the AP network — addresses will conflict");
    if (!s_natOn) {                                // enable NAT once
      s_natOn = WiFi.AP.enableNAPT(true);
      Serial.printf("[NAT] %s\n", s_natOn ? "enabled" : "FAILED");
    }
    requestCheck();
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED && s_staConnected) {
    s_staConnected = false;
    if (s_planned) {                               // our own switch, not a failure
      s_planned = false;
      Serial.println("[STA] left uplink (switching)");
    } else {
      s_staDrops = s_staDrops + 1;
      Serial.println("[STA] uplink lost");
    }
  }
}

static int8_t knownIndex(const char *ssid) {
  for (uint8_t i = 0; i < settings.netCount; i++)
    if (settings.nets[i].ssid == ssid) return i;
  return -1;
}

static bool inScan(const String &ssid) {
  for (uint8_t k = 0; k < s_seenN; k++)
    if (ssid == s_seen[k].ssid) return true;
  return false;
}

// The actual join. Never called while the STA link is up: WiFi.begin() would then block
// in disconnect(true, 1000).
static void beginNet(int8_t i, const Seen *ap) {
  const Config::KnownNet &net = settings.nets[i];
  if (net.st.on) {
    // The board's own resolver is a public DNS, NOT the router: if the router uses the
    // board as its DNS server, that would be a loop.
    WiFi.config(net.st.ip, net.st.gw, net.st.mask, DnsFilter::defaultUpstream());
    Serial.printf("[STA] static IP %s, gateway %s\n", net.st.ip.toString().c_str(), net.st.gw.toString().c_str());
  } else {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);   // DHCP (drops a previous static IP)
  }
  if (ap) {
    Serial.printf("[STA] connecting to \"%s\" (%02X:%02X:%02X:%02X:%02X:%02X, ch %u, %d dBm)...\n",
                  net.ssid.c_str(), ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3],
                  ap->bssid[4], ap->bssid[5], ap->ch, ap->rssi);
    WiFi.begin(net.ssid.c_str(), net.pass.c_str(), ap->ch, ap->bssid);
  } else {
    Serial.printf("[STA] connecting to \"%s\"...\n", net.ssid.c_str());
    WiFi.begin(net.ssid.c_str(), net.pass.c_str());
  }
  // With a static IP the addresses are known up front — loop protection works even
  // before the router connects. With DHCP the GOT_IP event sets the flag.
  if (net.st.on) s_addrsDirty = true;
}

// Join nets[i]; `ap` pins the BSSID/channel from the scan (nullptr: let the driver choose).
// If the link is still up (roam, edited uplink), drop it without waiting and join from loop().
static void connectTo(int8_t i, const Seen *ap) {
  const Config::KnownNet &net = settings.nets[i];
  s_active = i;
  strlcpy(s_activeSsid, net.ssid.c_str(), sizeof(s_activeSsid));
  s_attemptAt = millis();
  s_roamWait = 0;
  if (s_staConnected) {                // a roam: the disconnect that follows is planned
    s_planned = true;
    s_switchAt = millis();
  }
  if (WiFi.STA.connected()) {
    WiFi.disconnect(false, false, 0);  // async: DISCONNECTED arrives later
    s_pending = i;
    s_pendingAp = ap != nullptr;
    if (ap) s_pendingSeen = *ap;
    s_pendingAt = millis();
    return;
  }
  s_pending = -1;
  beginNet(i, ap);
}

// Disconnected: join the strongest known network from the last scan. Known networks missing
// from the scan (hidden SSID, out of range) get a round-robin try every other cycle.
static void pickAndConnect() {
  const uint8_t n = settings.netCount;
  if (!n) return;
  if (n == 1) { connectTo(0, nullptr); return; }   // one network: no pinning, as before

  int8_t missing = -1;
  for (uint8_t j = 0; j < n; j++) {
    uint8_t i = (s_rr + j) % n;
    if (!inScan(settings.nets[i].ssid)) { missing = i; break; }
  }
  const bool tryMissing = missing >= 0 && (s_cycle++ & 1);
  if (!tryMissing) {
    for (uint8_t k = 0; k < s_seenN; k++) {         // sorted: first hit is the strongest
      int8_t i = knownIndex(s_seen[k].ssid);
      if (i >= 0) { connectTo(i, &s_seen[k]); return; }
    }
  }
  if (missing >= 0) {
    s_rr = missing + 1;
    connectTo(missing, nullptr);
  } else {
    s_attemptAt = millis();                         // nothing to try; wait for the next cycle
  }
}

// Connected with a weak signal: switch if a known AP is clearly better.
// Nothing better: back off (5 min, doubling up to 15) so AP clients do not stall every 30 s.
static void maybeRoam() {
  if (settings.netCount < 2 || millis() - s_switchAt < HOLD_MS) return;
  const int cur = WiFi.RSSI();
  if (cur == 0 || cur >= ROAM_WEAK) return;
  uint8_t curB[6];
  WiFi.BSSID(curB);
  for (uint8_t k = 0; k < s_seenN; k++) {
    const Seen &a = s_seen[k];
    if (!memcmp(a.bssid, curB, 6)) break;           // ours is the strongest known AP
    int8_t i = knownIndex(a.ssid);
    if (i < 0) continue;
    if (a.rssi >= cur + ROAM_GAIN) {
      Serial.printf("[STA] roaming: %d dBm -> \"%s\" %d dBm\n", cur, a.ssid, a.rssi);
      connectTo(i, &a);
      return;
    }
    break;
  }
  s_roamWait = s_roamWait ? min(s_roamWait * 2, ROAM_BACKOFF_MAX) : ROAM_BACKOFF_MIN;
  s_roamFailAt = millis();
  s_roamRssi = cur;
  Serial.printf("[STA] no better uplink at %d dBm, next roam scan in %u min\n", cur, (unsigned)(s_roamWait / 60000));
}

// Copy the scan into the cache: strongest first, one entry per BSSID, hidden SSIDs skipped
static void collectScan(int16_t n) {
  s_seenN = 0;
  for (int16_t r = 0; r < n; r++) {
    String ssid = WiFi.SSID(r);
    if (!ssid.length()) continue;
    uint8_t b[6];
    if (!WiFi.BSSID(r, b)) continue;
    int8_t rssi = WiFi.RSSI(r);

    uint8_t k = 0;
    while (k < s_seenN && memcmp(s_seen[k].bssid, b, 6)) k++;
    if (k < s_seenN) {                              // duplicate BSSID: keep the stronger one
      if (rssi <= s_seen[k].rssi) continue;
      memmove(&s_seen[k], &s_seen[k + 1], (s_seenN - k - 1) * sizeof(Seen));
      s_seenN--;
    }
    uint8_t pos = s_seenN;
    while (pos > 0 && s_seen[pos - 1].rssi < rssi) pos--;
    if (pos >= MAX_SEEN) continue;
    uint8_t last = s_seenN < MAX_SEEN ? s_seenN : MAX_SEEN - 1;   // the weakest may fall off
    memmove(&s_seen[pos + 1], &s_seen[pos], (last - pos) * sizeof(Seen));
    Seen &e = s_seen[pos];
    strlcpy(e.ssid, ssid.c_str(), sizeof(e.ssid));
    memcpy(e.bssid, b, 6);
    e.rssi = rssi;
    e.ch   = WiFi.channel(r);
    e.open = WiFi.encryptionType(r) == WIFI_AUTH_OPEN;
    if (s_seenN < MAX_SEEN) s_seenN++;
  }
}

bool startScan() {
  if (s_scanning) return true;
  // A connect attempt in progress blocks scanning: stop it (not a drop — we are not
  // connected). The finished scan picks a network again.
  if (!s_staConnected && settings.netCount && (WiFi.getMode() & WIFI_MODE_STA))
    WiFi.disconnect(false, false, 0);
  if (WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING) {
    s_scanning = true;
    return true;
  }
  Serial.println("[STA] scan failed to start");
  if (!s_staConnected) pickAndConnect();            // do not stay idle after the disconnect
  return false;
}

void reselect(bool force) {
  int8_t idx = s_activeSsid[0] ? knownIndex(s_activeSsid) : -1;
  if (s_active >= 0 && idx < 0) {                   // the uplink in use was deleted
    s_pending = -1;
    Serial.printf("[STA] \"%s\" removed from the list — disconnecting\n", s_activeSsid);
    if (s_staConnected) s_planned = true;
    if (WiFi.getMode() & WIFI_MODE_STA) WiFi.disconnect(false, false, 0);
    s_activeSsid[0] = 0;
  }
  s_active = idx;
  s_rr = 0;
  s_cycle = 0;
  if (idx < 0) s_attemptAt = millis() - RETRY_MS - 1;   // pick a network on the next loop()
  else if (force) {
    // connectTo() drops the old link first (WiFi.begin() with the same SSID/password would keep it)
    Serial.printf("[STA] \"%s\" changed — reconnecting\n", s_activeSsid);
    connectTo(idx, nullptr);
  }
}

void begin() {
  const Config::Settings &s = settings;
  const bool hasNet = s.netCount > 0;

  // Create the check task BEFORE Wi-Fi, so the "connected" event can already wake it
  xTaskCreate(checkTask, "netcheck", 6144, nullptr, 1, &s_task);

  WiFi.onEvent(onWifiEvent);
  WiFi.mode(hasNet ? WIFI_AP_STA : WIFI_AP);

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
  //    One known network: connect at once. Several: scan first, then join the strongest.
  if (s.netCount == 1) {
    connectTo(0, nullptr);
  } else if (hasNet) {
    s_attemptAt = millis();
    startScan();
  }
}

void loop() {
  // Feed the board/router addresses to the DNS filter so it never forwards to them (loop).
  // Done here rather than in the event handler, so `dns` is only touched from one task.
  if (s_addrsDirty) {
    s_addrsDirty = false;
    static const Config::StaticIp NONE;
    const Config::StaticIp &st = s_active >= 0 ? settings.nets[s_active].st : NONE;
    IPAddress self = st.on ? st.ip : WiFi.localIP();
    IPAddress gw   = st.on ? st.gw : WiFi.gatewayIP();
    if (dns.setStaAddrs(self, gw))
      Serial.println("[DNS] upstream pointed at the board/router (loop) — reset to 1.1.1.1");
  }

  // A join waits for our own disconnect (connectTo while the link was up)
  if (s_pending >= 0) {
    if (WiFi.STA.connected() && millis() - s_pendingAt < PENDING_MAX_MS) return;
    int8_t i = s_pending;
    s_pending = -1;
    if (i < settings.netCount) beginNet(i, s_pendingAp ? &s_pendingSeen : nullptr);
  }

  // A link (re)appeared or went away: roaming checks start from scratch
  static bool wasConnected = false;
  if (s_staConnected != wasConnected) {
    wasConnected = s_staConnected;
    s_roamWait = 0;
  }

  // A scan (ours or the web UI's) finished: fill the cache, then pick / roam on it.
  // While connected it counts as the roam check (a web scan does not add one of its own).
  static uint32_t lastRoamCheck = 0;
  if (s_scanning) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    s_scanning = false;
    if (n >= 0) {
      collectScan(n);
      s_scanAt = millis();
    }
    WiFi.scanDelete();
    if (settings.netCount) {
      if (!s_staConnected) pickAndConnect();
      else {
        lastRoamCheck = millis();
        maybeRoam();
      }
    }
    return;
  }

  if (!settings.netCount) return;
  if (s_staConnected) {
    s_cycle = 0;
    // Scan only when the signal is weak: the AP follows the STA channel, so a scan
    // costs clients a short hiccup
    if (settings.netCount > 1 && millis() - lastRoamCheck > ROAM_MS) {
      lastRoamCheck = millis();
      int rssi = WiFi.RSSI();
      if (s_roamWait && abs(rssi - s_roamRssi) >= ROAM_RESET_DB) s_roamWait = 0;   // conditions changed
      if (millis() - s_switchAt > HOLD_MS && rssi != 0 && rssi < ROAM_WEAK &&
          (!s_roamWait || millis() - s_roamFailAt >= s_roamWait))
        startScan();
    }
  } else if (millis() - s_attemptAt > RETRY_MS) {
    // Safety net: if the router stays away, retry every 30 s (several networks: re-scan)
    s_attemptAt = millis();
    if (settings.netCount > 1) startScan();
    else if (s_active != 0) connectTo(0, nullptr);
    else WiFi.reconnect();
  }
}

}  // namespace Net
