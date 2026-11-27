// Status page (/): uplink, internet, access point + clients, system info
#include "web.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include "app_config.h"
#include "app_network.h"
#include "anti_bypass.h"

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

static void handleRoot() {
  const Config::Settings &cfg = Config::settings;
  bool sta = Net::staConnected();
  Net::Check n = Net::lastCheck();
  bool inet = sta && n.ok;
  int rssi = sta ? WiFi.RSSI() : 0;
  uint32_t tot = dns.statTotal;
  char buf[96];

  String p;
  p.reserve(7000);                               // one allocation instead of many reallocs

  // ---- Tiles: the essentials at a glance ----
  p += "<div class=tiles>";
  p += tile(inet ? T(ST_YES) : T(ST_NO),
            inet ? T(ST_INTERNET) + String(" · ") + String(n.ms) + T(ST_MS) : String(T(ST_INTERNET)),
            inet ? "ok" : "bad");
  if (sta) p += tile(String(rssi) + " dBm", T(ST_ROUTER_SIGNAL) + String(" · ") + rssiText(rssi), rssiClass(rssi));
  else     p += tile("—", T(ST_ROUTER_SIGNAL), "mut");
  p += tile(String(WiFi.softAPgetStationNum()), T(ST_AP_CLIENTS));
  snprintf(buf, sizeof(buf), T(ST_BLOCKED_FMT), (unsigned)pct(dns.statBlocked, tot));
  p += tile(String(dns.statBlocked), buf);
  p += tile(fmtDur(millis() / 1000), T(ST_UPTIME));
  p += "</div><div class=grid>";

  // ---- Uplink (router) ----
  p += String("<section class=card><h3>") + T(ST_ROUTER_CARD) + "</h3><table>";
  if (!cfg.staSsid.length()) {
    p += row(T(ST_STATUS), tag(T(ST_NOT_CONFIGURED)));
    p += String("</table><p class=hint>") + T(ST_ROUTER_HINT) + "</p>";
  } else {
    p += row(T(ST_NETWORK), esc(cfg.staSsid));
    if (!sta) {
      p += row(T(ST_STATUS), tag(T(ST_NOT_CONNECTED), "bad") + " <span class=mut>" + T(ST_RETRY_HINT) + "</span>");
    } else {
      p += row(T(ST_STATUS), tag(T(ST_CONNECTED), "ok") + " <span class=mut>" + fmtDur((millis() - Net::staSince()) / 1000) + "</span>");
      p += row(T(ST_SIGNAL), "<span class=" + String(rssiClass(rssi)) + ">" + String(rssi) + " dBm</span> (" + rssiText(rssi) + ")");
      p += row(T(ST_INTERNET_ROW), inet ? tag(T(ST_YES), "ok") + " " + String(n.ms) + T(ST_MS_TO) + Net::PROBE.toString()
                                        : tag(T(ST_NO), "bad"));
      if (n.at) p += row(T(ST_CHECKED), fmtDur((millis() - n.at) / 1000) + T(ST_AGO));
      p += row(T(ST_CHANNEL), String(WiFi.channel()));
      p += row(T(ST_MY_IP), "<b>" + WiFi.localIP().toString() + "</b> " +
                                (cfg.st.on ? tag(T(ST_STATIC), "ok") : tag(T(ST_FROM_DHCP))) +
                                "<br><span class=mut>" + T(ST_USE_AS_DNS) + "</span>");
      p += row(T(ST_GW_DNS), WiFi.gatewayIP().toString() + " / " + WiFi.dnsIP().toString());
      p += row("BSSID", WiFi.BSSIDstr());
    }
    p += row(T(ST_DROPS), String(Net::staDrops()));
    p += "</table>";
  }
  p += "<div class=row style='margin-top:.7em'>" + postBtn("/netcheck", T(ST_CHECK_NOW), "s") +
       "<a class='btn g s' href='/config'>" + T(ST_CHANGE_NET) + "</a></div></section>";

  // ---- Access point and clients ----
  p += String("<section class=card><h3>") + T(ST_AP_CARD) + "</h3><table>";
  p += row(T(ST_NETWORK), "<b>" + esc(WiFi.softAPSSID()) + "</b>");
  p += row(T(ST_PANEL_ADDR), "http://" + WiFi.softAPIP().toString());
  p += row(T(ST_CHANNEL), String(WiFi.channel()) + (sta ? String(" <span class=mut>") + T(ST_SAME_AS_ROUTER) + "</span>" : String()));
  p += row("NAT", onOff(Net::natOn()));
  p += row(T(MENU_FILTER), onOff(dns.enabled(), "bad"));
  p += row(T(MENU_GUARD), onOff(AntiBypass::enabled(), "bad"));
  p += "</table>";

  wifi_sta_list_t sl;
  if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK && sl.num > 0) {
    // MAC and signal come from the Wi-Fi driver, IP from the DHCP server; join them by MAC
    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM];
    for (int i = 0; i < sl.num; i++) {
      memcpy(pairs[i].mac, sl.sta[i].mac, 6);
      pairs[i].ip.addr = 0;
    }
    esp_netif_dhcps_get_clients_by_mac(WiFi.AP.netif(), sl.num, pairs);
    p += String("<h3 style='margin-top:1em'>") + T(ST_CLIENTS) + "</h3><table><tr><th>IP</th><th>MAC</th><th>" +
         T(ST_SIGNAL_COL) + "</th></tr>";
    for (int i = 0; i < sl.num; i++) {
      int r = sl.sta[i].rssi;
      p += "<tr><td>" + (pairs[i].ip.addr ? IPAddress(pairs[i].ip.addr).toString() : String("—")) +
           "</td><td class=mut>" + macStr(sl.sta[i].mac) + "</td><td class=" + rssiClass(r) + ">" +
           String(r) + " dBm</td></tr>";
    }
    p += "</table>";
  } else {
    p += String("<p class=hint>") + T(ST_NO_CLIENTS) + "</p>";
  }
  p += "</section>";

  // ---- System ----
  p += String("<section class=card><h3>") + T(ST_SYSTEM) + "</h3><table>";
  p += row(T(ST_FREE_RAM), kb(ESP.getFreeHeap(), ESP.getHeapSize()));
  p += row(T(ST_FREE_PSRAM), kb(ESP.getFreePsram(), ESP.getPsramSize()));
  p += row(T(ST_CHIP_TEMP), String(temperatureRead(), 1) + " °C");
  p += row(T(ST_FLASH), fsUsage());
  p += row(T(ST_AP_MAC), WiFi.softAPmacAddress());
  p += row(T(ST_FIRMWARE), String(__DATE__) + " " + __TIME__);
  p += "</table><div class=row style='margin-top:.7em'>" +
       postBtn("/reboot", T(ST_REBOOT), "r s", nullptr, "", T(ST_REBOOT_CONFIRM)) + "</div></section>";
  p += "</div>";

  sendPage(T(MENU_STATUS), "/", p, 200, "<meta http-equiv=refresh content=15>");   // auto-refresh every 15 s
}

static void handleNetCheck() {
  Net::requestCheck();                           // the check task runs within ~1 s
  redirectMsg("/", T(ST_CHECK_STARTED));
}

static void handleReboot() { sendRestartPage(T(RESTART_TITLE)); }

void registerStatusPage() {
  route("/",         HTTP_ANY,  handleRoot);
  route("/netcheck", HTTP_POST, handleNetCheck);
  route("/reboot",   HTTP_POST, handleReboot);
}

}  // namespace Web
