// Settings page (/config): access point, home Wi-Fi, static IP, panel password, OTA update
#include "web.h"
#include <WiFi.h>
#include <Update.h>        // OTA: writes the new firmware into the second app partition
#include "app_config.h"
#include "app_network.h"
#include "app_board.h"

using Config::settings;

namespace Web {

// Passwords are never put into the page: an empty field means "keep the current one"
static void handleConfig() {
  const Config::Settings &s = settings;
  bool sta = Net::staConnected();

  String b = "<form method=POST action='/save'><div class=grid>";
  b += "<section class=card><h3>Власна точка доступу</h3>"
       "<label>Назва мережі (1–32 символи)</label><input name=ap_ssid maxlength=32 required value='" + esc(s.apSsid) + "'>"
       "<label>Новий пароль (8–63 символи; порожньо — не змінювати)</label>"
       "<input name=ap_pass type=password minlength=8 maxlength=63 autocomplete=new-password></section>";
  b += "<section class=card><h3>Домашній Wi-Fi (інтернет)</h3>"
       "<label>Назва мережі роутера (порожньо — не підключатись)</label>"
       "<input name=sta_ssid maxlength=32 value='" + esc(s.staSsid) + "'>"
       "<label>Новий пароль роутера (порожньо — не змінювати)</label>"
       "<input name=sta_pass type=password maxlength=63 autocomplete=new-password>"
       "<p class=hint>Зараз: " + String(s.staSsid.length() ? (sta ? "підключено" : "не підключено") : "не налаштовано") +
       "</p></section>";

  // Empty static-IP fields are pre-filled with what the router gave us — only the IP needs changing
  auto val = [](IPAddress a, IPAddress fallback) {
    return (uint32_t)a ? a.toString() : (uint32_t)fallback ? fallback.toString() : String();
  };
  IPAddress none;
  String vIp   = val(s.st.ip, sta ? WiFi.localIP() : none);
  String vMask = val(s.st.mask, IPAddress(255, 255, 255, 0));
  String vGw   = val(s.st.gw, sta ? WiFi.gatewayIP() : none);
  b += "<section class=card><h3>Плата як DNS для домашньої мережі</h3>"
       "<label><input type=checkbox name=st_on value=1" + String(s.st.on ? " checked" : "") +
       ">фіксована IP-адреса в мережі роутера</label>"
       "<label>IP плати</label><input name=st_ip placeholder='192.168.1.53' value='" + vIp + "'>"
       "<label>Маска</label><input name=st_mask value='" + vMask + "'>"
       "<label>Шлюз (адреса роутера)</label><input name=st_gw placeholder='192.168.1.1' value='" + vGw + "'>"
       "<p class=hint>Оберіть адресу <b>поза діапазоном DHCP</b> роутера (або закріпіть її в роутері за MAC "
       "<code>" + WiFi.macAddress() + "</code>). Потім у роутері: DHCP → DNS-сервер = ця адреса.</p>"
       "<p class=hint><span class=warn>Другий DNS (напр. 8.8.8.8)</span> пристрої часто питають паралельно з першим — "
       "тоді частина реклами проскакуватиме. Найнадійніше — вказати лише плату.</p></section>";
  b += "<section class=card><h3>Пароль веб-панелі</h3>"
       "<p class=hint>Логін: <code>admin</code>. Зараз пароль " +
       String(s.webPass.length() ? tag("задано", "ok") : tag("не задано", "warn")) + "</p>"
       "<label>Новий пароль панелі (порожньо — не змінювати)</label>"
       "<input name=web_pass type=password maxlength=63 autocomplete=new-password>"
       "<label><input type=checkbox name=web_clear value=1>прибрати пароль панелі</label></section>";
  b += "<section class=card><h3>Забули пароль?</h3>"
       "<p class=hint>Коли плата працює — затисніть кнопку <b>BOOT</b> на 5 секунд (світлодіод блимає червоним). "
       "Скинуться <b>лише паролі</b>: точка стане <code>" + String(Config::DEFAULT_AP_SSID) + "</code> / <code>" +
       Config::DEFAULT_AP_PASS + "</code>, панель — без пароля.</p>"
       "<p class=hint>Роутер, статична IP, налаштування і списки залишаться.</p></section>";
  b += "</div><div class=card><button>Зберегти і перезавантажити</button> "
       "<span class=hint>Плата перезавантажиться, точка зникне на ~10 секунд.</span></div></form>";

  // OTA — a separate form (file upload), not inside the settings form
  b += "<section class=card><h3>Оновлення прошивки (OTA)</h3>"
       "<p class=hint>Поточна збірка: " + String(__DATE__) + " " + __TIME__ + ". Вільно під прошивку: " +
       String(ESP.getFreeSketchSpace() / 1024) + " КБ.</p>"
       "<form method=POST action='/update' enctype='multipart/form-data' "
       "onsubmit=\"this.querySelector('button').disabled=true;this.querySelector('span').textContent="
       "'Завантаження… не закривайте сторінку (до хвилини)'\">"
       "<input type=file name=fw accept='.bin' required><div class=row>"
       "<button>Оновити</button><span class=hint></span></div></form>"
       "<p class=hint>Файл <code>firmware.bin</code> — з релізу на GitHub або з "
       "<code>.pio/build/esp32-s3-zero/</code> після збірки. Налаштування і списки збережуться.</p></section>";
  sendPage("Налаштування", "/config", b);
}

// Validate everything BEFORE saving, so we never store settings the AP cannot start with
static void handleSave() {
  Config::Settings &s = settings;
  String newApSsid  = server.arg("ap_ssid");
  String newApPass  = server.arg("ap_pass");
  String newStaSsid = server.arg("sta_ssid");
  String newStaPass = server.arg("sta_pass");
  String newWebPass = server.arg("web_pass");
  newApSsid.trim();
  newStaSsid.trim();

  if (!Config::validApConfig(newApSsid, newApPass.length() ? newApPass : s.apPass))
    return sendError("Назва точки — від 1 до 32 символів, пароль — від 8 до 63 (вимога WPA2).", "/config");
  if (newStaSsid.length() > 32 || newStaPass.length() > 63)
    return sendError("Назва мережі роутера — до 32 символів, пароль — до 63.", "/config");

  // Static IP: parse into a temporary and apply only if valid
  Config::StaticIp newSt;
  newSt.on = server.hasArg("st_on");
  String sIp = server.arg("st_ip"), sMask = server.arg("st_mask"), sGw = server.arg("st_gw");
  sIp.trim(); sMask.trim(); sGw.trim();
  String err = Config::checkStatic(sIp, sMask, sGw, newSt);
  if (newSt.on && err.length()) return sendError(err, "/config");
  s.st = newSt;

  s.apSsid = newApSsid;
  if (newApPass.length()) s.apPass = newApPass;
  if (newStaSsid != s.staSsid) s.staPass = "";     // different router — the old password is useless
  s.staSsid = newStaSsid;
  if (newStaPass.length()) s.staPass = newStaPass;
  if (server.hasArg("web_clear")) s.webPass = "";
  else if (newWebPass.length()) s.webPass = newWebPass;
  Config::save();
  sendRestartPage("Налаштування збережено");
}

// ---------------------------------------------------------------- OTA
// The file arrives in chunks (upload handler); then the normal handler sends the reply.
// Access is checked BEFORE the first chunk is written, otherwise a foreign file could get flashed.
static String s_otaError;
static bool   s_otaAllowed = false;

static void handleUpdateUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    s_otaError = "";
    s_otaAllowed = accessOk();
    if (!s_otaAllowed) return;
    Serial.printf("[OTA] start: %s\n", u.filename.c_str());
    Board::setLed(30, 0, 40);                     // purple: update in progress
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) s_otaError = Update.errorString();
  } else if (!s_otaAllowed || s_otaError.length()) {
    if (u.status == UPLOAD_FILE_ABORTED) Update.abort();
    return;                                       // rejected or already failed — ignore the rest
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) s_otaError = Update.errorString();
    dns.loop();                                   // keep DNS running during the upload
  } else if (u.status == UPLOAD_FILE_END) {
    // end(true): verify the image and mark the new partition as the boot partition
    if (!Update.end(true)) s_otaError = Update.errorString();
    else Serial.printf("[OTA] written %u bytes\n", (unsigned)u.totalSize);
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    s_otaError = "завантаження перервано";
  }
}

static void handleUpdateDone() {                  // via routeUpload(): access already checked
  Board::refreshLed();
  if (s_otaError.length() || !Update.isFinished())
    return sendError("Оновлення не вдалося: " + (s_otaError.length() ? s_otaError : String("файл не отримано")) +
                     ". Стара прошивка працює далі.", "/config");
  sendRestartPage("Прошивку оновлено");
}

void registerConfigPage() {
  route("/config", HTTP_ANY,  handleConfig);
  route("/save",   HTTP_POST, handleSave);
  routeUpload("/update", handleUpdateDone, handleUpdateUpload);
}

}  // namespace Web
