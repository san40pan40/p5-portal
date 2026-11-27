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

// LittleFS computes its usage by walking every block — cache it for a minute
static String fsUsage() {
  static uint32_t at = 0;
  static String s;
  if (!s.length() || millis() - at > 60000) {
    at = millis();
    s = String(LittleFS.usedBytes() / 1024) + " з " + String(LittleFS.totalBytes() / 1024) + " КБ";
  }
  return s;
}

static void handleRoot() {
  const Config::Settings &cfg = Config::settings;
  bool sta = Net::staConnected();
  Net::Check n = Net::lastCheck();
  bool inet = sta && n.ok;
  int rssi = sta ? WiFi.RSSI() : 0;
  uint32_t tot = dns.statTotal;

  String p;
  p.reserve(7000);                               // one allocation instead of many reallocs

  // ---- Tiles: the essentials at a glance ----
  p += "<div class=tiles>";
  p += tile(inet ? "є" : "немає", inet ? "інтернет · " + String(n.ms) + " мс" : String("інтернет"), inet ? "ok" : "bad");
  if (sta) p += tile(String(rssi) + " dBm", String("сигнал роутера · ") + rssiText(rssi), rssiClass(rssi));
  else     p += tile("—", "сигнал роутера", "mut");
  p += tile(String(WiFi.softAPgetStationNum()), "клієнтів на точці");
  p += tile(String(dns.statBlocked), "заблоковано · " + String(pct(dns.statBlocked, tot)) + "% запитів");
  p += tile(fmtDur(millis() / 1000), "працює без перезапуску");
  p += "</div><div class=grid>";

  // ---- Uplink (router) ----
  p += "<section class=card><h3>Роутер (інтернет)</h3><table>";
  if (!cfg.staSsid.length()) {
    p += row("Статус", tag("не налаштовано"));
    p += "</table><p class=hint>Вкажіть домашній Wi-Fi у налаштуваннях — через нього клієнти отримають інтернет.</p>";
  } else {
    p += row("Мережа", esc(cfg.staSsid));
    if (!sta) {
      p += row("Статус", tag("не підключено", "bad") + " <span class=mut>пробуємо знову кожні 30 с</span>");
    } else {
      p += row("Статус", tag("підключено", "ok") + " <span class=mut>" + fmtDur((millis() - Net::staSince()) / 1000) + "</span>");
      p += row("Сигнал", "<span class=" + String(rssiClass(rssi)) + ">" + String(rssi) + " dBm</span> (" + rssiText(rssi) + ")");
      p += row("Інтернет", inet ? tag("є", "ok") + " " + String(n.ms) + " мс до " + Net::PROBE.toString()
                                : tag("немає", "bad"));
      if (n.at) p += row("Перевірено", fmtDur((millis() - n.at) / 1000) + " тому");
      p += row("Канал", String(WiFi.channel()));
      p += row("Мій IP", "<b>" + WiFi.localIP().toString() + "</b> " +
                             (cfg.st.on ? tag("статична", "ok") : tag("від DHCP")) +
                             "<br><span class=mut>можна вказати в роутері як DNS-сервер</span>");
      p += row("Шлюз / DNS роутера", WiFi.gatewayIP().toString() + " / " + WiFi.dnsIP().toString());
      p += row("BSSID", WiFi.BSSIDstr());
    }
    p += row("Розривів зв'язку", String(Net::staDrops()));
    p += "</table>";
  }
  p += "<div class=row style='margin-top:.7em'>" + postBtn("/netcheck", "Перевірити інтернет", "s") +
       "<a class='btn g s' href='/config'>Змінити мережу</a></div></section>";

  // ---- Access point and clients ----
  p += "<section class=card><h3>Точка доступу</h3><table>";
  p += row("Мережа", "<b>" + esc(WiFi.softAPSSID()) + "</b>");
  p += row("Адреса панелі", "http://" + WiFi.softAPIP().toString());
  p += row("Канал", String(WiFi.channel()) + (sta ? " <span class=mut>(як у роутера)</span>" : ""));
  p += row("NAT", Net::natOn() ? tag("увімкнено", "ok") : tag("вимкнено"));
  p += row("DNS фільтр", dns.enabled() ? tag("увімкнено", "ok") : tag("вимкнено", "bad"));
  p += row("Охорона", AntiBypass::enabled() ? tag("увімкнено", "ok") : tag("вимкнено", "bad"));
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
    p += "<h3 style='margin-top:1em'>Клієнти</h3><table><tr><th>IP</th><th>MAC</th><th>сигнал</th></tr>";
    for (int i = 0; i < sl.num; i++) {
      int r = sl.sta[i].rssi;
      p += "<tr><td>" + (pairs[i].ip.addr ? IPAddress(pairs[i].ip.addr).toString() : String("—")) +
           "</td><td class=mut>" + macStr(sl.sta[i].mac) + "</td><td class=" + rssiClass(r) + ">" +
           String(r) + " dBm</td></tr>";
    }
    p += "</table>";
  } else {
    p += "<p class=hint>Клієнтів поки немає.</p>";
  }
  p += "</section>";

  // ---- System ----
  p += "<section class=card><h3>Система</h3><table>";
  p += row("Вільна RAM", String(ESP.getFreeHeap() / 1024) + " з " + String(ESP.getHeapSize() / 1024) + " КБ");
  p += row("Вільна PSRAM", String(ESP.getFreePsram() / 1024) + " з " + String(ESP.getPsramSize() / 1024) + " КБ");
  p += row("Температура чипа", String(temperatureRead(), 1) + " °C");
  p += row("Флеш (LittleFS)", fsUsage());
  p += row("MAC точки", WiFi.softAPmacAddress());
  p += row("Прошивка", String(__DATE__) + " " + __TIME__);
  p += "</table><div class=row style='margin-top:.7em'>" +
       postBtn("/reboot", "Перезавантажити", "r s", nullptr, "", "Перезавантажити плату?") + "</div></section>";
  p += "</div>";

  sendPage("Статус", "/", p, 200, "<meta http-equiv=refresh content=15>");   // auto-refresh every 15 s
}

static void handleNetCheck() {
  Net::requestCheck();                           // the check task runs within ~1 s
  redirectMsg("/", "Перевірку інтернету запущено — результат з'явиться за кілька секунд");
}

static void handleReboot() { sendRestartPage("Перезавантаження"); }

void registerStatusPage() {
  route("/",         HTTP_ANY,  handleRoot);
  route("/netcheck", HTTP_POST, handleNetCheck);
  route("/reboot",   HTTP_POST, handleReboot);
}

}  // namespace Web
