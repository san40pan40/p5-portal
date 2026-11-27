// Settings page (/config): known uplink networks (+ nearby scan), access point, panel password,
// system (firmware, OTA update, reboot). Each card is its own form; only the AP form restarts.
#include "web.h"
#include <WiFi.h>
#include <Update.h>        // OTA: writes the new firmware into the second app partition
#include "app_config.h"
#include "app_network.h"
#include "app_board.h"
#include "net_fetch.h"
#include "preset_lists.h"
#include "version_gen.h"

using Config::settings;

namespace Web {

static String card(I18n::Str title) { return String("<section class=card><h3>") + I18n::t(title) + "</h3>"; }
// Field: label text above its control (style.css .f)
static String field(I18n::Str text, const String &control) {
  return String("<label class=f><span>") + I18n::t(text) + "</span>" + control + "</label>";
}
static String checkbox(const char *name, bool on, const char *text) {
  return String("<label class=cb><input type=checkbox name=") + name + " value=1" + (on ? " checked" : "") + ">" + text + "</label>";
}

// Password inputs never carry the current value: empty means "keep" (app.js adds a show/hide button)
static String passInput(const char *name, const char *extra) {
  return String("<input name=") + name + " type=password maxlength=63 autocomplete=new-password placeholder='" +
         T(CF_PASS_PH) + "'" + extra + ">";
}

static const char IP_ATTR[] = " inputmode=decimal pattern='((25[0-5]|2[0-4]\\d|1?\\d?\\d)\\.){3}(25[0-5]|2[0-4]\\d|1?\\d?\\d)'";

static bool inRange(const String &ssid) {
  uint8_t n;
  const Net::Seen *s = Net::seen(n);
  for (uint8_t k = 0; k < n; k++)
    if (ssid == s[k].ssid) return true;
  return false;
}

static int8_t knownIndex(const String &ssid) {
  for (uint8_t i = 0; i < settings.netCount; i++)
    if (settings.nets[i].ssid == ssid) return i;
  return -1;
}

// Known networks table + add/edit form (?net=i edits, ?add=SSID pre-fills a new one) + nearby scan
static void netsCard(String &b) {
  const Config::Settings &s = settings;
  const int8_t act = Net::activeNet();
  b += card(I18n::CF_NETS_CARD) + "<div class=tw><table>";
  if (!s.netCount) b += String("<tr><td class=mut>") + T(CF_NETS_NONE) + "</td></tr>";
  // One row per network: name + state tags, address below it, actions on the right
  for (uint8_t i = 0; i < s.netCount; i++) {
    const Config::KnownNet &n = s.nets[i];
    b += "<tr><td><b>" + esc(n.ssid) + "</b> ";
    if (i == act && Net::staConnected()) b += tag(T(CF_NET_CONNECTED), "ok") + " ";
    if (inRange(n.ssid)) b += tag(T(CF_NET_IN_RANGE));
    b += "<br><small class=mut>" + (n.st.on ? n.st.ip.toString() : String(T(CF_NET_DHCP))) + "</small></td>"
         "<td class=a><a class='btn gh s' href='/config?net=" + String(i) + "#nf'>" +
         T(CF_NET_EDIT) + "</a> <form class=i method=POST action='/net/del' data-confirm='" + esc(T(CF_NET_DEL_CONFIRM)) +
         "'><input type=hidden name=i value=" + String(i) + "><input type=hidden name=os value='" + esc(n.ssid) +
         "'><button class='gh s bad'>" + T(CF_NET_DELETE) + "</button></form></td></tr>";
    flushPage(b, 1000);
  }
  b += "</table></div>";
  flushPage(b);

  // Nearby networks: the scan runs in the background, the page refreshes itself until it is done
  b += String("<div class=actions>") + postBtn("/net/scan", T(CF_SCAN_BTN), "g") + "</div>";
  if (server.hasArg("scan")) {
    b += String("<h3 id=scan class=sub>") + T(CF_SCAN_CARD) + "</h3>";
    if (Net::scanning()) {
      b += String("<p class=hint>") + T(CF_SCANNING) + "</p>";
    } else {
      uint8_t cnt, shown = 0;
      const Net::Seen *seen = Net::seen(cnt);
      b += "<div class=tw><table>";
      for (uint8_t k = 0; k < cnt; k++) {
        const Net::Seen &a = seen[k];
        if (!a.ssid[0]) continue;
        bool dup = false;                          // one row per SSID (the strongest BSSID comes first)
        for (uint8_t j = 0; j < k && !dup; j++) dup = !strcmp(seen[j].ssid, a.ssid);
        if (dup) continue;
        shown++;
        b += "<tr><td>" + esc(a.ssid) + "</td><td class='" + rssiClass(a.rssi) + "'>" + String(a.rssi) + " dBm</td><td>" +
             (a.open ? T(CF_SCAN_OPEN) : "&#128274;") + "</td><td class=a>" +
             (knownIndex(a.ssid) >= 0 ? tag(T(CF_NET_KNOWN))
                                      : String("<a class='btn gh s' href='/config?add=") + urlEncode(a.ssid) + "#nf'>" + T(CF_SCAN_ADD) + "</a>") +
             "</td></tr>";
        flushPage(b, 1000);
      }
      if (!shown) b += String("<tr><td class=mut>") + T(LIST_NOTHING_FOUND) + "</td></tr>";
      b += "</table></div>";
    }
  }
  flushPage(b);

  // Add / edit form
  int ei = server.hasArg("net") ? server.arg("net").toInt() : -1;
  if (ei >= s.netCount) ei = -1;
  static const Config::KnownNet EMPTY;
  const Config::KnownNet &n = ei >= 0 ? s.nets[ei] : EMPTY;
  String ssid = ei >= 0 ? n.ssid : server.arg("add");
  // Static IP fields: the stored values, else (the active network) what the router gave us
  const bool live = ei >= 0 && ei == act && Net::staConnected();
  auto val = [](IPAddress a, IPAddress fallback) {
    return (uint32_t)a ? a.toString() : (uint32_t)fallback ? fallback.toString() : String();
  };
  IPAddress none;
  b += String("<form id=nf method=POST action='/net/save'><h3 class=sub>") +
       (ei >= 0 ? T(CF_NET_FORM) : T(CF_NET_ADD)) + "</h3><input type=hidden name=i value='" + (ei >= 0 ? String(ei) : String()) +
       "'><input type=hidden name=os value='" + (ei >= 0 ? esc(n.ssid) : String()) + "'>" + field(I18n::CF_NET_SSID, "<input name=ssid required maxlength=32 value='" + esc(ssid) + "'>") +
       field(I18n::CF_NET_PASS, passInput("pass", " minlength=8")) +
       (ei >= 0 ? String("<p class=hint>") + T(CF_NET_PASS_KEEP) + "</p>" : String()) +
       "<details" + (n.st.on ? " open" : "") + "><summary>" + T(CF_STATIC_ADV) + "</summary>" +
       checkbox("st_on", n.st.on, T(CF_STATIC_ON)) +
       field(I18n::CF_BOARD_IP, String("<input name=st_ip") + IP_ATTR + " placeholder='192.168.1.53' value='" +
                                    val(n.st.ip, live ? WiFi.localIP() : none) + "'>") +
       field(I18n::CF_MASK, String("<input name=st_mask") + IP_ATTR + " value='" + val(n.st.mask, IPAddress(255, 255, 255, 0)) + "'>") +
       field(I18n::CF_GW, String("<input name=st_gw") + IP_ATTR + " placeholder='192.168.1.1' value='" +
                              val(n.st.gw, live ? WiFi.gatewayIP() : none) + "'>") +
       "<p class=hint>" + T(CF_STATIC_HINT1) + "<code>" + WiFi.macAddress() + "</code>" + T(CF_STATIC_HINT2) + "</p>" +
       "<p class=hint>" + T(CF_SECONDARY_HINT) + "</p></details><div class=actions><button>" + T(SAVE) + "</button>";
  if (ei >= 0 || ssid.length()) b += String("<a class='btn g' href='/config'>") + T(CANCEL) + "</a>";
  b += "</div></form>";
  flushPage(b);

  b += "</section>";
}

// ---------------------------------------------------------------- online update (GitHub releases)
// NetFetch runs the HTTPS job in its own task; the page shows its status, app.js polls /api/update
// while a job runs (#fw[data-r]) and reloads when it ends. #fwr (installed) is submitted by app.js.

// Leading "v1.2.3" of a version; a suffix ("-dirty", "-3-gabc") is ignored.
// false for anything without a tag ("4dc7444", "dev").
static bool parseVer(const char *v, unsigned (&n)[3]) {
  if (*v == 'v' || *v == 'V') v++;
  int used = 0;
  if (!isdigit((unsigned char)*v) || sscanf(v, "%u.%u.%u%n", &n[0], &n[1], &n[2], &used) != 3) return false;
  return !v[used] || v[used] == '-' || v[used] == '+';
}

// 1 = latest is newer, 0 = same, 2 = older, -1 = not comparable (install is offered anyway).
// Same numbers but not the same text ("v0.2.0-4-gabc" or "-dirty" after the tag): this build is newer.
static int fwNewer(const char *latest) {
  unsigned a[3], b[3];
  if (!parseVer(latest, a) || !parseVer(FW_VERSION, b)) return -1;
  for (int i = 0; i < 3; i++)
    if (a[i] != b[i]) return a[i] > b[i] ? 1 : 2;
  return strcmp(latest, FW_VERSION) ? 2 : 0;
}

static void fwBlock(String &b) {
  using namespace NetFetch;
  const Status st = fwStatus();                  // preset list jobs are on the DNS page
  const bool run = st.state == RUNNING, inst = st.job == JOB_FW_INSTALL;
  b += String("<div id=fw") + (run ? " data-r=1>" : ">");
  if (run && inst) {
    b += String("<p class=hint>") + T(FW_INSTALLING) + "<b data-p>" + st.progress + "%</b></p>";
  } else if (run) {
    b += String("<p class=hint>") + T(FW_CHECKING) + "</p>";
  } else if (st.state == DONE && inst) {
    b += String("<p class=ok>") + T(FW_INSTALLED) + "</p><form id=fwr method=POST action='/reboot'><button>" +
         T(ST_REBOOT) + "</button></form>";
  } else if (st.state == DONE && !st.latest[0]) {
    b += String("<p class=hint>") + T(FW_NO_REL) + "</p>";
  } else if (st.state == DONE) {
    const int nw = fwNewer(st.latest);
    b += String("<p class=hint>") + T(FW_LATEST) + "<b>" + esc(st.latest) + "</b> " +
         (nw == 1 ? tag(T(FW_NEWER), "warn") : nw == 0 ? tag(T(FW_UPTODATE), "ok")
                  : nw == 2 ? tag(T(FW_OLDER), "ok") : String()) + "</p>";
    if (nw == 1 || nw == -1)
      b += "<div class=actions>" + postBtn("/fw/install", T(FW_INSTALL_BTN), "", nullptr, "", T(FW_INSTALL_CONFIRM)) + "</div>";
  } else if (st.state == FAILED) {
    b += String("<p class=bad>") + esc(inst ? T(OTA_FAIL) + fetchErrText(st) + T(OTA_OLD_RUNS) : T(FW_CHECK_FAIL) + fetchErrText(st)) + "</p>";
  }
  b += "</div>";
}

static void fwStart(NetFetch::Job job) {
  // Other refusals (low RAM...) are kept in the status and shown in the System card
  if (NetFetch::start(job) == NetFetch::E_BUSY) return sendError(T(FW_E_BUSY), "/config");
  server.sendHeader("Location", "/config#sys", true);
  server.send(303, "text/plain", "");
}
static void handleFwCheck()   { fwStart(NetFetch::JOB_FW_CHECK); }
static void handleFwInstall() { fwStart(NetFetch::JOB_FW_INSTALL); }

// GET /api/update: {state: idle|run|done|fail, job, latest, progress, error} of the last firmware job,
// bl (preset list state code), blp (progress of the list download)
static void handleApiUpdate() {
  using namespace NetFetch;
  const Status st = fwStatus();
  static const char *const STATE[] = {"idle", "run", "done", "fail"};
  String err = st.state == FAILED ? fetchErrText(st) : String();
  err.replace("\\", "\\\\");
  err.replace("\"", "\\\"");
  String j = String("{\"state\":\"") + STATE[st.state] + "\",\"job\":\"" +
             (st.job == JOB_FW_INSTALL ? "install" : st.job == JOB_FW_CHECK ? "check" : "") + "\",\"latest\":\"" +
             st.latest + "\",\"progress\":" + st.progress + ",\"bl\":" + PresetLists::stateCode() +
             ",\"blp\":" + listStatus().progress + ",\"error\":\"" + err + "\"}";
  server.send(200, "application/json", j);
}

static void handleConfig() {
  const Config::Settings &s = settings;
  // While a scan runs the page reloads itself until the results are in
  const bool scanning = server.hasArg("scan") && Net::scanning();
  beginPage(T(MENU_SETTINGS), "/config", 200, scanning ? "<meta http-equiv=refresh content='3;url=/config?scan=1#scan'>" : "");
  String b;
  b.reserve(1200);                               // one buffer, reused for every card
  netsCard(b);
  flushPage(b);

  b += card(I18n::CF_AP_CARD) + "<form method=POST action='/save/ap'>" +
       field(I18n::CF_AP_SSID, "<input name=ap_ssid minlength=1 maxlength=32 required value='" + esc(s.apSsid) + "'>") +
       field(I18n::CF_AP_PASS, passInput("ap_pass", " minlength=8")) + "<p class=hint>" + T(CF_SAVE_HINT) +
       "</p><div class=actions><button>" + T(CF_SAVE_BTN) + "</button></div></form></section>";
  flushPage(b);
  b += card(I18n::CF_WEB_CARD) + "<form method=POST action='/save/web'><p class=hint>" + T(CF_LOGIN) + "<code>admin</code>" +
       T(CF_PASS_NOW) + (s.webPass.length() ? tag(T(CF_SET), "ok") : tag(T(CF_NOT_SET), "warn")) + "</p>" +
       field(I18n::CF_WEB_PASS, passInput("web_pass", "")) + checkbox("web_clear", false, T(CF_WEB_CLEAR)) +
       "<div class=actions><button>" + T(SAVE) + "</button></div></form></section>";
  flushPage(b);

  // System: firmware info, OTA (file upload form), reboot, password recovery
  b += String("<section class=card id=sys><h3>") + T(CF_SYS_CARD) + "</h3><p class=hint>" + T(CF_OTA_BUILD) +
       "<b>" FW_VERSION "</b> (" __DATE__ ")" + T(CF_OTA_FREE) + String(ESP.getFreeSketchSpace() / 1024) + T(ST_KB) + ".</p>";
  fwBlock(b);
  b += String("<form method=POST action='/update' enctype='multipart/form-data' "
              "onsubmit=\"this.querySelector('button').disabled=true;this.querySelector('span').textContent='") +
       T(CF_OTA_UPLOADING) + "'\"><div class=row><input type=file name=fw accept='.bin' required><button>" +
       T(CF_OTA_BTN) + "</button><span class=hint></span></div></form><p class=hint>" + T(CF_OTA_HINT) + "</p>" +
       "<div class=actions>" + (NetFetch::busy() ? String() : postBtn("/fw/check", T(FW_CHECK_BTN), "g")) +
       postBtn("/reboot", T(ST_REBOOT), "r", nullptr, "", T(ST_REBOOT_CONFIRM)) + "</div>" +
       "<details><summary>" + T(CF_FORGOT_CARD) + "</summary><p class=hint>" + T(CF_FORGOT_1) + "<code>" +
       Config::DEFAULT_AP_SSID + "</code> / <code>" + Config::DEFAULT_AP_PASS + "</code>" + T(CF_FORGOT_2) +
       "</p><p class=hint>" + T(CF_FORGOT_3) + "</p></details></section>";
  endPage(b);
}

// ---------------------------------------------------------------- known networks
static void handleNetSave() {
  Config::Settings &s = settings;
  String ssid = server.arg("ssid"), pass = server.arg("pass"), si = server.arg("i");
  // The SSID is used as typed (spaces are valid in a network name); only blank is rejected
  String blank = ssid;
  blank.trim();
  if (!blank.length() || ssid.length() > 32 || pass.length() > 63 || (pass.length() && pass.length() < 8))
    return sendError(T(CF_ERR_STA), "/config");

  // Editing: the index must still point at the network the page showed (the list may have changed)
  int i = si.length() ? si.toInt() : -1;
  if (si.length() && (i < 0 || i >= s.netCount || s.nets[i].ssid != server.arg("os")))
    return sendError(T(CF_NET_STALE), "/config");
  const int dup = knownIndex(ssid);
  // A known SSID added again: update only its password. The empty add form must not erase its static IP.
  const bool keepSt = i < 0 && dup >= 0;
  if (i < 0) i = dup;
  else if (dup >= 0 && dup != i) return sendError(String(T(CF_NET_KNOWN)) + ": " + ssid, "/config");
  if (i < 0 && s.netCount >= Config::MAX_NETS) return sendError(T(CF_NETS_FULL), "/config");

  // Static IP: parse into a temporary; switched on only if valid
  Config::StaticIp st;
  st.on = server.hasArg("st_on");
  String sIp = server.arg("st_ip"), sMask = server.arg("st_mask"), sGw = server.arg("st_gw");
  sIp.trim(); sMask.trim(); sGw.trim();
  String err = Config::checkStatic(sIp, sMask, sGw, st);
  if (st.on && err.length()) return sendError(err, "/config");

  const bool force = i >= 0 && i == Net::activeNet();   // the uplink in use was edited: reconnect
  if (i < 0) {
    i = s.netCount++;
    s.nets[i] = Config::KnownNet();
  }
  Config::KnownNet &n = s.nets[i];
  if (pass.length() || n.ssid != ssid) n.pass = pass;   // empty keeps the password only for the same SSID
  n.ssid = ssid;
  if (!keepSt) n.st = st;
  Config::save();
  Net::reselect(force);
  redirectMsg("/config", T(CF_NET_SAVED) + ssid);
}

static void handleNetDel() {
  Config::Settings &s = settings;
  String si = server.arg("i");
  int i = si.toInt();
  // The index must still point at the network the page showed (the list may have changed)
  if (!si.length() || i < 0 || i >= s.netCount || s.nets[i].ssid != server.arg("os"))
    return sendError(T(CF_NET_STALE), "/config");
  String name = s.nets[i].ssid;
  for (uint8_t j = i; j + 1 < s.netCount; j++) s.nets[j] = s.nets[j + 1];
  s.nets[--s.netCount] = Config::KnownNet();
  Config::save();
  Net::reselect();
  redirectMsg("/config", T(CF_NET_DELETED) + name);
}

static void handleNetScan() {
  Net::startScan();
  server.sendHeader("Location", "/config?scan=1#scan", true);
  server.send(303, "text/plain", "");
}

// ---------------------------------------------------------------- AP and panel password
// Validate BEFORE saving, so we never store settings the AP cannot start with
static void handleSaveAp() {
  Config::Settings &s = settings;
  String ssid = server.arg("ap_ssid"), pass = server.arg("ap_pass");
  ssid.trim();
  if (!Config::validApConfig(ssid, pass.length() ? pass : s.apPass)) return sendError(T(CF_ERR_AP), "/config");
  const bool changed = ssid != s.apSsid || pass.length();
  s.apSsid = ssid;
  if (pass.length()) s.apPass = pass;
  Config::save();
  sendRestartPage(T(CF_SAVED), changed ? ssid : String());
}

static void handleSaveWeb() {
  Config::Settings &s = settings;
  String pass = server.arg("web_pass");
  if (server.hasArg("web_clear")) s.webPass = "";
  else if (pass.length()) s.webPass = pass;
  Config::save();
  redirectMsg("/config", T(CF_SAVED));
}

// ---------------------------------------------------------------- OTA
// The file arrives in chunks (upload handler); then the normal handler sends the reply.
// Access is checked BEFORE the first chunk is written, otherwise a foreign file could get flashed.
static String s_otaError;
static bool   s_otaAllowed = false;
static bool   s_otaBegun = false;                 // THIS upload owns Update (the online fetch may use it too)

static void handleUpdateUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    s_otaError = "";
    s_otaBegun = false;
    s_otaAllowed = accessOk();
    if (!s_otaAllowed) return;
    if (NetFetch::busy()) { s_otaError = T(FW_E_BUSY); return; }   // an online update may be writing
    Serial.printf("[OTA] start: %s\n", u.filename.c_str());
    Board::setLed(30, 0, 40);                     // purple: update in progress
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) s_otaError = Update.errorString();
    else s_otaBegun = true;
  } else if (!s_otaAllowed || s_otaError.length()) {
    if (u.status == UPLOAD_FILE_ABORTED || u.status == UPLOAD_FILE_END) {
      if (s_otaBegun) Update.abort();             // never touch Update owned by the online fetch
      s_otaBegun = false;
    }
    return;                                       // rejected or already failed — ignore the rest
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) s_otaError = Update.errorString();
    dns.loop();                                   // keep DNS running during the upload
  } else if (u.status == UPLOAD_FILE_END) {
    // end(true): verify the image and mark the new partition as the boot partition
    if (!Update.end(true)) s_otaError = Update.errorString();
    else Serial.printf("[OTA] written %u bytes\n", (unsigned)u.totalSize);
    s_otaBegun = false;
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    s_otaBegun = false;
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
  route("/config",   HTTP_ANY,  handleConfig);
  route("/net/save", HTTP_POST, handleNetSave);
  route("/net/del",  HTTP_POST, handleNetDel);
  route("/net/scan", HTTP_POST, handleNetScan);
  route("/save/ap",  HTTP_POST, handleSaveAp);
  route("/save/web", HTTP_POST, handleSaveWeb);
  routeUpload("/update", handleUpdateDone, handleUpdateUpload);
  route("/fw/check",   HTTP_POST, handleFwCheck);
  route("/fw/install", HTTP_POST, handleFwInstall);
  route("/api/update", HTTP_GET,  handleApiUpdate);
}

}  // namespace Web
