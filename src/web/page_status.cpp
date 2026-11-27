// Status page (/): uplink, internet, access point + clients, system info
#include "web.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include "app_config.h"
#include "app_network.h"
#include "anti_bypass.h"
#include "metrics.h"
#include "version_gen.h"

namespace Web {

static String kb(uint32_t used, uint32_t total) {
  return String(used / 1024) + T(ST_OF) + String(total / 1024) + T(ST_KB);
}

// LittleFS computes its usage by walking every block — cache it for a minute
static String fsUsage() {
  static uint32_t at = 0;
  static size_t used = 0, total = 0;
  if (!total || millis() - at > 60000) {
    at = millis();
    used = LittleFS.usedBytes();
    total = LittleFS.totalBytes();
  }
  return kb(used, total);
}

static String onOff(bool on, const char *offCls = "") {
  return on ? tag(T(ON_LOWER), "ok") : tag(T(OFF_LOWER), offCls);
}

// ---- Live values: shared by the page (first render) and /api/status (updates every 5 s) ----
static const char *inetCls(bool inet) { return inet ? "ok" : "bad"; }
static String inetLabel(bool inet, int ms) {
  return inet ? T(ST_INTERNET) + String(" · ") + String(ms) + T(ST_MS) : String(T(ST_INTERNET));
}
static String rssiVal(bool sta, int rssi) { return sta ? String(rssi) + " dBm" : String("—"); }
static const char *rssiCls(bool sta, int rssi) { return sta ? rssiClass(rssi) : "mut"; }
static String rssiLabel(bool sta, int rssi) {
  return sta ? T(ST_ROUTER_SIGNAL) + String(" · ") + rssiText(rssi) : String(T(ST_ROUTER_SIGNAL));
}
static String blockedLabel() {
  char buf[96];
  snprintf(buf, sizeof(buf), T(ST_BLOCKED_FMT), (unsigned)pct(dns.statBlocked, dns.statTotal));
  return buf;
}
static String freeRam() { return kb(ESP.getFreeHeap(), ESP.getHeapSize()); }

static void handleRoot() {
  const Config::Settings &cfg = Config::settings;
  bool sta = Net::staConnected();
  Net::Check n = Net::lastCheck();
  bool inet = sta && n.ok;
  int rssi = sta ? WiFi.RSSI() : 0;

  beginPage(T(MENU_STATUS), "/");                // values refresh live via app.js + /api/status
  String p;
  p.reserve(1200);                               // one buffer, reused for every card

  // ---- Tiles: the essentials at a glance ----
  p += "<div class=tiles>";
  p += tile(inet ? T(ST_YES) : T(ST_NO), inetLabel(inet, n.ms), inetCls(inet), "in", "inl");
  p += tile(rssiVal(sta, rssi), rssiLabel(sta, rssi), rssiCls(sta, rssi), "rs", "rsl");
  p += tile(String(WiFi.softAPgetStationNum()), T(ST_AP_CLIENTS), "", "cl");
  p += tile(String(dns.statBlocked), blockedLabel(), "", "bl", "bll");
  p += tile(fmtDur(Net::uptimeSec()), T(ST_UPTIME), "", "up");
  p += "</div>";
  // ---- History charts (last hour) ----
  p += histStart("sys", T(HI_LOAD));
  p += chart("c0,c1", T(HI_CPU), T(HI_CORES));
  p += chart("ram", T(HI_RAM));
  p += chart("cl", T(ST_AP_CLIENTS));
  p += chart("rs", T(ST_ROUTER_SIGNAL));
  p += "</div></section><div class=cols>";
  flushPage(p);

  // ---- Uplink (router) ----
  p += String("<section class=card><h3>") + T(ST_ROUTER_CARD) + "</h3><table>";
  if (!cfg.netCount) {
    p += row(T(ST_STATUS), tag(T(ST_NOT_CONFIGURED)));
    p += String("</table><p class=hint>") + T(ST_ROUTER_HINT) + "</p>";
  } else {
    int8_t an = Net::activeNet();
    const Config::KnownNet *act = an >= 0 && an < cfg.netCount ? &cfg.nets[an] : nullptr;
    p += row(T(ST_NETWORK), esc(sta ? WiFi.SSID() : act ? act->ssid : cfg.nets[0].ssid));
    if (!sta) {
      p += row(T(ST_STATUS), tag(T(ST_NOT_CONNECTED), "bad") + " <span class=mut>" + T(ST_RETRY_HINT) + "</span>");
    } else {
      p += row(T(ST_STATUS), tag(T(ST_CONNECTED), "ok") + " <span class=mut>" + fmtDur(Net::uptimeSec() - Net::staSince()) + "</span>");
      p += row(T(ST_SIGNAL), "<span class=" + String(rssiClass(rssi)) + ">" + String(rssi) + " dBm</span> (" + rssiText(rssi) + ")");
      p += row(T(ST_INTERNET_ROW), inet ? tag(T(ST_YES), "ok") + " " + String(n.ms) + T(ST_MS_TO) + Net::PROBE.toString()
                                        : tag(T(ST_NO), "bad"));
      if (n.at) p += row(T(ST_CHECKED), fmtDur((millis() - n.at) / 1000) + T(ST_AGO));
      p += row(T(ST_CHANNEL), String(WiFi.channel()));
      p += row(T(ST_MY_IP), "<b>" + WiFi.localIP().toString() + "</b> " +
                                (act && act->st.on ? tag(T(ST_STATIC), "ok") : tag(T(ST_FROM_DHCP))) +
                                "<br><span class=mut>" + T(ST_USE_AS_DNS) + "</span>");
      p += row(T(ST_GW_DNS), WiFi.gatewayIP().toString() + " / " + WiFi.dnsIP().toString());
      p += row("BSSID", WiFi.BSSIDstr());
    }
    p += row(T(ST_DROPS), String(Net::staDrops()));
    p += row(T(ST_KNOWN_NETS), String(cfg.netCount));
    p += "</table>";
  }
  p += "<div class=actions>" + postBtn("/netcheck", T(ST_CHECK_NOW), "g") +
       "<a class='btn g' href='/config'>" + T(ST_CHANGE_NET) + "</a></div></section>";
  flushPage(p);

  // ---- Access point and clients ----
  p += String("<section class=card><h3>") + T(ST_AP_CARD) + "</h3><table>";
  p += row(T(ST_NETWORK), "<b>" + esc(WiFi.softAPSSID()) + "</b>");
  p += row(T(ST_PANEL_ADDR), "http://" + WiFi.softAPIP().toString());
  p += row(T(ST_CHANNEL), String(WiFi.channel()) + (sta ? String(" <span class=mut>") + T(ST_SAME_AS_ROUTER) + "</span>" : String()));
  p += row("NAT", onOff(Net::natOn()));
  p += row(T(MENU_FILTER), onOff(dns.enabled(), "bad"));
  p += row(T(MENU_GUARD), onOff(AntiBypass::enabled(), "bad"));
  p += "</table>";
  flushPage(p);

  wifi_sta_list_t sl;
  if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK && sl.num > 0) {
    // MAC and signal come from the Wi-Fi driver, IP from the DHCP server; join them by MAC
    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM];
    for (int i = 0; i < sl.num; i++) {
      memcpy(pairs[i].mac, sl.sta[i].mac, 6);
      pairs[i].ip.addr = 0;
    }
    esp_netif_dhcps_get_clients_by_mac(WiFi.AP.netif(), sl.num, pairs);
    p += String("<h3 class=sub>") + T(ST_CLIENTS) + "</h3><div class=tw><table><tr><th>IP</th><th>MAC</th><th>" +
         T(ST_SIGNAL_COL) + "</th></tr>";
    for (int i = 0; i < sl.num; i++) {
      int r = sl.sta[i].rssi;
      p += "<tr><td>" + (pairs[i].ip.addr ? IPAddress(pairs[i].ip.addr).toString() : String("—")) +
           "</td><td class=mut>" + macStr(sl.sta[i].mac) + "</td><td class=" + rssiClass(r) + ">" +
           String(r) + " dBm</td></tr>";
    }
    p += "</table></div>";
  } else {
    p += String("<p class=hint>") + T(ST_NO_CLIENTS) + "</p>";
  }
  p += "</section>";
  flushPage(p);

  // ---- System ----
  p += String("<section class=card><h3>") + T(ST_SYSTEM) + "</h3><table>";
  p += row(T(ST_FREE_RAM), liveSpan("ram", freeRam()));
  p += row(T(ST_FREE_PSRAM), kb(ESP.getFreePsram(), ESP.getPsramSize()));
  p += row(T(ST_CHIP_TEMP), String(temperatureRead(), 1) + " °C");
  p += row(T(ST_FLASH), fsUsage());
  p += row(T(ST_AP_MAC), WiFi.softAPmacAddress());
  p += row(T(ST_FIRMWARE), FW_VERSION " (" __DATE__ ")");
  p += "</table><div class=actions>" +
       postBtn("/reboot", T(ST_REBOOT), "r", nullptr, "", T(ST_REBOOT_CONFIRM)) + "</div></section>";
  p += "</div>";
  endPage(p);
}

// GET /api/status: the values that change, for app.js (~300 bytes). Keys match the data-k
// attributes of the pages; [text, cls] also sets the colour class. Values are display-ready
// (localised); none of them can contain a quote or a backslash, so no JSON escaping is needed.
static void handleApiStatus() {
  bool sta = Net::staConnected();
  Net::Check n = Net::lastCheck();
  bool inet = sta && n.ok;
  int rssi = sta ? WiFi.RSSI() : 0;
  const char *hc, *ht = hdrState(hc);
  char j[600];
  snprintf(j, sizeof(j),
           "{\"hd\":[\"\",\"%s\"],\"ht\":\"%s\",\"cl\":%u,\"in\":[\"%s\",\"%s\"],\"inl\":\"%s\",\"rs\":[\"%s\",\"%s\"],"
           "\"rsl\":\"%s\",\"up\":\"%s\",\"dt\":%u,\"bl\":%u,\"bll\":\"%s\",\"df\":%u,\"dx\":%u,\"ram\":\"%s\",\"ver\":\"" FW_VERSION "\"}",
           hc, ht, (unsigned)WiFi.softAPgetStationNum(), inet ? T(ST_YES) : T(ST_NO), inetCls(inet),
           inetLabel(inet, n.ms).c_str(), rssiVal(sta, rssi).c_str(), rssiCls(sta, rssi), rssiLabel(sta, rssi).c_str(),
           fmtDur(Net::uptimeSec()).c_str(), (unsigned)dns.statTotal, (unsigned)dns.statBlocked, blockedLabel().c_str(),
           (unsigned)dns.statForwarded, (unsigned)dns.statFailed, freeRam().c_str());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// GET /api/hist?set=sys|dns: chart history for app.js
static void handleApiHist() { Metrics::sendHist(server.arg("set") == "dns"); }

static void handleNetCheck() {
  Net::requestCheck();                           // the check task runs within ~1 s
  redirectMsg("/", T(ST_CHECK_STARTED));
}

static void handleReboot() { sendRestartPage(T(RESTART_TITLE)); }

void registerStatusPage() {
  route("/",         HTTP_ANY,  handleRoot);
  route("/netcheck", HTTP_POST, handleNetCheck);
  route("/reboot",   HTTP_POST, handleReboot);
  route("/api/status", HTTP_GET, handleApiStatus);
  route("/api/hist",   HTTP_GET, handleApiHist);
}

}  // namespace Web
