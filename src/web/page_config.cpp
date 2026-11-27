// Settings page (/config): language, access point, home Wi-Fi, static IP, panel password, OTA update
#include "web.h"
#include <WiFi.h>
#include <Update.h>        // OTA: writes the new firmware into the second app partition
#include "app_config.h"
#include "app_network.h"
#include "app_board.h"

using Config::settings;

namespace Web {

static String card(I18n::Str title) { return String("<section class=card><h3>") + I18n::t(title) + "</h3>"; }
static String label(I18n::Str text) { return String("<label>") + I18n::t(text) + "</label>"; }

// Passwords are never put into the page: an empty field means "keep the current one"
static void handleConfig() {
  const Config::Settings &s = settings;
  bool sta = Net::staConnected();

  // Language: its own small form, applied at once without a restart
  bool en = I18n::lang() == I18n::Lang::EN;
  String b = card(I18n::CF_LANG_CARD) + "<form method=POST action='/lang' class=row>"
             "<input type=hidden name=back value='/config'>"
             "<button class='" + String(en ? "g s" : "s") + "' name=l value=uk>Українська</button>"
             "<button class='" + String(en ? "s" : "g s") + "' name=l value=en>English</button></form></section>";

  b += "<form method=POST action='/save'><div class=grid>";
  b += card(I18n::CF_AP_CARD) + label(I18n::CF_AP_SSID) +
       "<input name=ap_ssid maxlength=32 required value='" + esc(s.apSsid) + "'>" + label(I18n::CF_AP_PASS) +
       "<input name=ap_pass type=password minlength=8 maxlength=63 autocomplete=new-password></section>";
  b += card(I18n::CF_STA_CARD) + label(I18n::CF_STA_SSID) +
       "<input name=sta_ssid maxlength=32 value='" + esc(s.staSsid) + "'>" + label(I18n::CF_STA_PASS) +
       "<input name=sta_pass type=password maxlength=63 autocomplete=new-password><p class=hint>" + T(CF_NOW) +
       (s.staSsid.length() ? (sta ? T(ST_CONNECTED) : T(ST_NOT_CONNECTED)) : T(ST_NOT_CONFIGURED)) + "</p></section>";

  // Empty static-IP fields are pre-filled with what the router gave us — only the IP needs changing
  auto val = [](IPAddress a, IPAddress fallback) {
    return (uint32_t)a ? a.toString() : (uint32_t)fallback ? fallback.toString() : String();
  };
  IPAddress none;
  b += card(I18n::CF_STATIC_CARD) + "<label><input type=checkbox name=st_on value=1" + String(s.st.on ? " checked" : "") +
       ">" + T(CF_STATIC_ON) + "</label>" +
       label(I18n::CF_BOARD_IP) + "<input name=st_ip placeholder='192.168.1.53' value='" + val(s.st.ip, sta ? WiFi.localIP() : none) + "'>" +
       label(I18n::CF_MASK) + "<input name=st_mask value='" + val(s.st.mask, IPAddress(255, 255, 255, 0)) + "'>" +
       label(I18n::CF_GW) + "<input name=st_gw placeholder='192.168.1.1' value='" + val(s.st.gw, sta ? WiFi.gatewayIP() : none) + "'>" +
       "<p class=hint>" + T(CF_STATIC_HINT1) + "<code>" + WiFi.macAddress() + "</code>" + T(CF_STATIC_HINT2) + "</p>" +
       "<p class=hint>" + T(CF_SECONDARY_HINT) + "</p></section>";
  b += card(I18n::CF_WEB_CARD) + "<p class=hint>" + T(CF_LOGIN) + "<code>admin</code>" + T(CF_PASS_NOW) +
       (s.webPass.length() ? tag(T(CF_SET), "ok") : tag(T(CF_NOT_SET), "warn")) + "</p>" + label(I18n::CF_WEB_PASS) +
       "<input name=web_pass type=password maxlength=63 autocomplete=new-password>"
       "<label><input type=checkbox name=web_clear value=1>" + T(CF_WEB_CLEAR) + "</label></section>";
  b += card(I18n::CF_FORGOT_CARD) + "<p class=hint>" + T(CF_FORGOT_1) + "<code>" + Config::DEFAULT_AP_SSID + "</code> / <code>" +
       Config::DEFAULT_AP_PASS + "</code>" + T(CF_FORGOT_2) + "</p><p class=hint>" + T(CF_FORGOT_3) + "</p></section>";
  b += String("</div><div class=card><button>") + T(CF_SAVE_BTN) + "</button> <span class=hint>" + T(CF_SAVE_HINT) +
       "</span></div></form>";

  // OTA — a separate form (file upload), not inside the settings form
  b += card(I18n::CF_OTA_CARD) + "<p class=hint>" + T(CF_OTA_BUILD) + __DATE__ + " " + __TIME__ + T(CF_OTA_FREE) +
       String(ESP.getFreeSketchSpace() / 1024) + T(ST_KB) + ".</p>"
       "<form method=POST action='/update' enctype='multipart/form-data' "
       "onsubmit=\"this.querySelector('button').disabled=true;this.querySelector('span').textContent='" +
       T(CF_OTA_UPLOADING) + "'\"><input type=file name=fw accept='.bin' required><div class=row><button>" +
       T(CF_OTA_BTN) + "</button><span class=hint></span></div></form><p class=hint>" + T(CF_OTA_HINT) + "</p></section>";
  sendPage(T(MENU_SETTINGS), "/config", b);
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

  if (!Config::validApConfig(newApSsid, newApPass.length() ? newApPass : s.apPass)) return sendError(T(CF_ERR_AP), "/config");
  if (newStaSsid.length() > 32 || newStaPass.length() > 63) return sendError(T(CF_ERR_STA), "/config");

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
  sendRestartPage(T(CF_SAVED));
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
    s_otaError = T(OTA_ABORTED);
  }
}

static void handleUpdateDone() {                  // via routeUpload(): access already checked
  Board::refreshLed();
  if (s_otaError.length() || !Update.isFinished())
    return sendError(T(OTA_FAIL) + (s_otaError.length() ? s_otaError : String(T(OTA_NO_FILE))) + T(OTA_OLD_RUNS), "/config");
  sendRestartPage(T(OTA_OK));
}

void registerConfigPage() {
  route("/config", HTTP_ANY,  handleConfig);
  route("/save",   HTTP_POST, handleSave);
  routeUpload("/update", handleUpdateDone, handleUpdateUpload);
}

}  // namespace Web
