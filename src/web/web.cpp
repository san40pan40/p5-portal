#include "web.h"
#include <WiFi.h>
#include "app_config.h"
#include "app_network.h"
#include "assets_gz.h"
#include "version_gen.h"

namespace Web {

// ================================================================ styles and script
// Readable sources: src/web/style.css and app.js. scripts/embed_assets.py gzips them into
// assets_gz.h at build time; they are served compressed (every browser accepts gzip) and cached.
static void sendGz(const char *type, const uint8_t *data, size_t len) {
  server.sendHeader("Cache-Control", "max-age=86400");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, type, (const char *)data, len);
}

static void handleCss() { sendGz("text/css", STYLE_CSS_GZ, sizeof(STYLE_CSS_GZ)); }
static void handleJs()  { sendGz("application/javascript", APP_JS_GZ, sizeof(APP_JS_GZ)); }

// ================================================================ access control
// CSRF protection: any site a client visits could submit a form to our panel, and the browser
// would attach the Basic Auth password automatically. So POSTs are only accepted from our own
// pages: the Origin / Referer header (set by the browser) must name the same host.
static bool sameOrigin() {
  if (server.method() != HTTP_POST) return true;
  String src = server.header("Origin");
  if (!src.length()) src = server.header("Referer");
  if (!src.length()) return true;                // old clients / curl send neither — allow
  int s = src.indexOf("://");
  if (s < 0) return false;
  s += 3;
  int e = src.indexOf('/', s);
  String host = e < 0 ? src.substring(s) : src.substring(s, e);
  return host.equalsIgnoreCase(server.hostHeader());
}

// Panel password (HTTP Basic Auth, login "admin"). An empty password means open access.
static bool credsOk() {
  const String &pass = Config::settings.webPass;
  return !pass.length() || server.authenticate("admin", pass.c_str());
}

bool accessOk() { return sameOrigin() && credsOk(); }

static void guarded(void (*handler)()) {
  if (!sameOrigin()) return server.send(403, "text/plain", "Forbidden: cross-site request");
  if (!credsOk()) return server.requestAuthentication(BASIC_AUTH, "P5 Portal");
  handler();
}

void route(const char *uri, HTTPMethod method, void (*handler)()) {
  server.on(uri, method, [handler]() { guarded(handler); });
}

void routeUpload(const char *uri, void (*done)(), void (*upload)()) {
  server.on(uri, HTTP_POST, [done]() { guarded(done); }, upload);
}

// ================================================================ page frame
// Header state: dot/chip colour class + text (also sent by /api/status, so it stays live)
const char *hdrState(const char *&cls) {
  if (!Config::settings.netCount) { cls = "mut";  return T(HDR_NO_ROUTER_CFG); }
  if (!Net::staConnected())       { cls = "bad";  return T(HDR_NO_ROUTER); }
  if (!Net::internetOk())         { cls = "warn"; return T(HDR_NO_INTERNET); }
  cls = "ok";
  return T(HDR_ONLINE);
}

// Pages are streamed: beginPage() sends the HTTP header and the page frame, then each card is
// built in a small String and sent with flushPage(), endPage() closes. No whole-page String.
void beginPage(const char *title, const char *active, int code, const char *extraHead) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(code, "text/html; charset=utf-8", "");
  String h;
  h.reserve(1200);
  h += "<!DOCTYPE html><html lang=";
  h += T(LANG_CODE);
  h += "><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>";
  h += extraHead;
  h += "<title>";
  h += title;
  // ?v=version + build time: after a firmware update the browser fetches fresh styles, not cached ones
#define P5_ASSET_V FW_VERSION "-" __DATE__ __TIME__
  h += " · P5 Portal</title><link rel=stylesheet href='/style.css?v=" P5_ASSET_V "'>"
       "<script defer src='/app.js?v=" P5_ASSET_V "'></script></head><body><header><div class=top>";

  const char *cls, *txt = hdrState(cls);
  h += "<span class=brand><span class='dot ";
  h += cls;
  h += "' data-k=hd></span>P5 Portal</span><span class=chip data-k=ht>";
  h += txt;
  h += "</span><span class=chip>";
  h += T(HDR_CLIENTS);
  h += "<span data-k=cl>" + String(WiFi.softAPgetStationNum()) + "</span></span>";
  // Quick language switch: shows the other language, returns to the same page
  h += "<form class=i method=POST action='/lang'><input type=hidden name=back value='";
  h += active;
  h += "'><button class='g s' name=l value='";
  h += I18n::lang() == I18n::Lang::EN ? "uk" : "en";
  h += "'>";
  h += T(LANG_SWITCH);
  h += "</button></form></div><nav>";

  static const struct { const char *path; I18n::Str label; } MENU[] = {
    {"/", I18n::MENU_STATUS}, {"/dns", I18n::MENU_FILTER}, {"/bypass", I18n::MENU_GUARD}, {"/config", I18n::MENU_SETTINGS},
  };
  for (auto &m : MENU) {
    h += "<a href='";
    h += m.path;
    h += strcmp(m.path, active) == 0 ? "' class=on>" : "'>";
    h += I18n::t(m.label);
    h += "</a>";
  }
  h += "</nav></header><main>";
  if (server.hasArg("msg")) h += "<div class=note>" + esc(server.arg("msg")) + "</div>";   // result of the last action
  server.sendContent(h);
}

// An empty chunk would end the chunked response, so it is never sent here
void flushPage(String &p, size_t over) {
  if (p.length() <= over) return;
  server.sendContent(p);
  p = "";                                        // keeps the buffer for the next card
  dns.loop();                                    // keep answering DNS while the page is generated
}

void endPage(String &p) {
  p += "</main></body></html>";
  flushPage(p);
  server.sendContent("");                        // end of the chunked response
}

void sendPage(const char *title, const char *active, const String &body, int code, const char *extraHead) {
  beginPage(title, active, code, extraHead);
  if (body.length()) server.sendContent(body);
  String e;
  endPage(e);
}

void sendError(const String &msg, const char *back) {
  sendPage(T(ERROR_TITLE), "", String("<section class=card><h3>") + T(ERROR_TITLE) + "</h3><p class=bad>" + esc(msg) +
           "</p><div class=actions><a class='btn g' href='" + back + "'>" + T(BACK) + "</a></div></section>", 400);
}

// #rs makes app.js poll the board and reopen "/" as soon as it answers; meta refresh is the fallback
void sendRestartPage(const char *heading, const String &apSsid) {
  String b = String("<section class=card><h3>") + heading + "</h3>";
  if (apSsid.length()) b += String("<div class=alert>") + T(RESTART_CONNECT) + "<b>" + esc(apSsid) + "</b></div>";
  b += String("<p id=rs>") + T(RESTART_TEXT) + "</p></section>";
  sendPage(T(RESTART_TITLE), "", b, 200, "<meta http-equiv=refresh content='12;url=/'>");
  delay(800);
  ESP.restart();
}

void redirectMsg(const char *url, const String &msg) {
  String loc = String(url) + (strchr(url, '?') ? "&" : "?") + "msg=" + urlEncode(msg);
  server.sendHeader("Location", loc, true);
  server.send(303, "text/plain", "");
}

void redirectBack(const char *def, const String &msg) {
  String b = server.arg("back");
  redirectMsg(b == "/" || b == "/dns" || b == "/bypass" || b == "/config" ? b.c_str() : def, msg);
}

// ================================================================ HTML pieces
String esc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '<')  o += "&lt;";
    else if (c == '>')  o += "&gt;";
    else if (c == '&')  o += "&amp;";
    else if (c == '"')  o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

// The framework only has urlDecode, so encoding is done here (Cyrillic, spaces -> %XX)
String urlEncode(const String &s) {
  static const char HEX_[] = "0123456789ABCDEF";
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += HEX_[c >> 4]; o += HEX_[c & 15]; }
  }
  return o;
}

// A one-button POST form with an optional hidden field (its value is escaped here)
String postBtn(const char *action, const char *label, const char *cls, const char *name,
               const String &val, const char *confirmText) {
  String f = String("<form class=i method=POST action='") + action + "'";
  if (confirmText) f += String(" data-confirm='") + esc(confirmText) + "'";   // handled by app.js
  f += ">";
  if (name) f += String("<input type=hidden name=") + name + " value='" + esc(val) + "'>";
  f += String("<button class='") + cls + "'>" + label + "</button></form>";
  return f;
}

String tag(const char *text, const char *cls) { return String("<span class='tag ") + cls + "'>" + text + "</span>"; }

static String dataK(const char *key) { return key ? String(" data-k=") + key : String(); }

String tile(const String &value, const String &label, const char *cls, const char *vk, const char *lk) {
  return String("<div class=tile><b class='") + cls + "'" + dataK(vk) + ">" + value + "</b><span" + dataK(lk) + ">" +
         label + "</span></div>";
}

String histStart(const char *set, const char *title) {
  return String("<section class=card data-hist=") + set + " data-a='" + T(HI_AGO) + "' data-b='" + T(HI_NOW) +
         "'><h3>" + title + "</h3><div class=chs>";
}

String chart(const char *keys, const char *title, const char *legend, bool total) {
  return String("<div class=ch data-s=") + keys + (total ? " data-u=1" : "") + " data-t='" + title + "' data-l='" + legend +
         "'></div>";
}

String liveSpan(const char *key, const String &v) { return "<span" + dataK(key) + ">" + v + "</span>"; }

String row(const String &k, const String &v) { return "<tr><td class=k>" + k + "</td><td>" + v + "</td></tr>"; }

// On/off switch: "[o-] Title  state". The checkbox has no name: the action just flips the setting.
// app.js submits the form on change; without JS a small button is shown instead.
String toggleRow(const char *title, bool on, const char *action) {
  return String("<form class=row method=POST action='") + action + "'><label class=sw><input type=checkbox" +
         (on ? " checked" : "") + "><b>" + title + "</b><span class=mut>" + (on ? T(ON_LOWER) : T(OFF_LOWER)) +
         "</span></label><noscript><button class='g s'>" + (on ? T(TURN_OFF) : T(TURN_ON)) + "</button></noscript></form>";
}

String saveAlert(const RuleList &l) {
  if (l.truncated()) return String("<div class=alert>") + T(LIST_TRUNCATED) + "</div>";
  return l.lastSaveOk() ? String() : String("<div class=alert>") + T(SAVE_FAILED) + "</div>";
}

// Rule list table (declared in web.h)
void ruleTable(String &p, const RuleList &l, const char *page, const char *delAction) {
  const size_t PER = 20;
  String q = server.arg("q");
  q.toLowerCase();
  p += "<form method=GET action='" + String(page) + "#list' class=row>"
       "<input name=q value='" + esc(q) + "' placeholder='" + T(LIST_SEARCH) + "'><button class=g>" +
       T(LIST_FIND) + "</button></form>";

  size_t matched;
  size_t pg = server.arg("p").toInt();
  auto rules = l.page(q, pg * PER, PER, matched);
  size_t pages = matched ? (matched + PER - 1) / PER : 1;
  if (pg >= pages) {                     // page number out of range (the list shrank)
    pg = pages - 1;
    rules = l.page(q, pg * PER, PER, matched);
  }

  String pager;                          // shown above and below the table
  if (pages > 1) {
    String base = String(page) + "?q=" + urlEncode(q) + "&p=";
    pager = "<div class=row>";
    if (pg > 0) pager += "<a class='btn g s' href='" + base + String(pg - 1) + "#list'>" + T(LIST_PREV) + "</a>";
    char info[96];
    snprintf(info, sizeof(info), T(LIST_PAGE_FMT), (unsigned)(pg + 1), (unsigned)pages, (unsigned)matched);
    pager += String("<span class=hint>") + info + "</span>";
    if (pg + 1 < pages) pager += "<a class='btn g s' href='" + base + String(pg + 1) + "#list'>" + T(LIST_NEXT) + "</a>";
    pager += "</div>";
  }

  p += pager + "<div class=tw><table>";
  for (const std::string *r : rules) {
    String rule(r->c_str());
    // Icon-only ghost button (✕): the word goes into title / aria-label
    p += "<tr><td>" + esc(rule) + "</td><td class=a><form class=i method=POST action='" + delAction +
         "'><input type=hidden name=domain value='" + esc(rule) + "'><button class='gh s' title='" + T(DELETE) +
         "' aria-label='" + T(DELETE) + "'>&#10005;</button></form></td></tr>";
    flushPage(p, 1000);
  }
  if (rules.empty()) p += String("<tr><td class=mut>") + (q.length() ? T(LIST_NOTHING_FOUND) : T(LIST_EMPTY)) + "</td></tr>";
  p += "</table></div>" + pager;
}

// ================================================================ shared actions
void ruleAdd(RuleList &l, const char *page) {
  int n = l.add(server.arg("domains"));
  if (l.limitHit()) return redirectBack(page, T(RULES_ADDED) + String(n) + T(RULES_LIMIT));
  redirectBack(page, n ? T(RULES_ADDED) + String(n) : String(T(RULES_NONE_ADDED)));
}

void ruleDel(RuleList &l, const char *page) {
  String r = server.arg("domain");
  redirectBack(page, l.remove(r) ? T(RULE_REMOVED) + r : String(T(RULE_NOT_FOUND)));
}

void ruleExport(const RuleList &l, const char *name, const char *file) {
  server.sendHeader("Content-Disposition", String("attachment; filename=") + file);
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");
  String p;
  p.reserve(1100);
  p += "# P5 Portal ";
  p += name;
  p += " export " FW_VERSION "\n";                // never empty: an empty chunk would end the response
  l.forEach([&p](const std::string &r) {
    p.concat(r.data(), r.size());
    p += '\n';
    flushPage(p, 1000);                          // send in ~1 KB chunks, answer DNS meanwhile
  });
  flushPage(p);
  server.sendContent("");
}

static bool s_impOk, s_impRan, s_impAborted;     // access checked at the start / a file was parsed / it broke off
void ruleImportUpload(RuleList &l) {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    s_impOk = accessOk();
    // form fields before the file are already parsed, so the checkbox is known here
    if (s_impOk) l.importBegin(server.arg("replace") == "1");
  } else if (!s_impOk) {
    return;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    l.importData(u.buf, u.currentSize);
    dns.loop();                                  // keep DNS running during the upload
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    s_impAborted = u.status == UPLOAD_FILE_ABORTED;
    if (s_impAborted) l.importAbort();           // a partial file must not replace the saved list
    else l.importEnd();
    s_impOk = false;
    s_impRan = true;
  }
}

void ruleImportDone(RuleList &l, const char *page) {   // via routeUpload(): access already checked
  if (!s_impRan) return redirectMsg(page, T(RULES_NONE_ADDED));
  s_impRan = false;
  if (s_impAborted) return redirectMsg(page, T(IMP_ABORTED));
  String m = T(IMP_ADDED) + String(l.importAdded()) + T(IMP_SKIPPED) + String(l.importSkipped());
  if (l.limitHit()) m += T(RULES_LIMIT);
  if (l.importAdded() && l.truncated()) m += String(". ") + T(LIST_TRUNCATED);
  else if (l.importAdded() && !l.lastSaveOk()) m += String(". ") + T(SAVE_FAILED);
  redirectMsg(page, m);
}

String ruleIo(const char *page) {
  return String("<details><summary>") + T(LIST_IO) + "</summary><form method=POST action='" + page +
         "/import' enctype='multipart/form-data'><label class=cb><input type=checkbox name=replace value=1>" +
         T(IMP_REPLACE) + "</label><div class=row><input type=file name=f accept='.txt,text/plain' required><button>" +
         T(IMP_BTN) + "</button></div><p class=hint>" + T(IMP_HINT) + "</p></form><div class=actions><a class='btn g' href='" +
         page + "/export'>" + T(LIST_EXPORT) + "</a></div></details>";
}

// Language switch (header button or Settings): takes effect at once, no restart
static void handleLang() {
  I18n::Lang l;
  if (I18n::parseLang(server.arg("l"), l)) {
    I18n::setLang(l);
    Config::settings.lang = l;
    Config::save();
  }
  redirectBack("/", T(LANG_CHANGED));
}

void finishToggle(const char *page, bool on, const char *onMsg, const char *offMsg) {
  Config::save();
  redirectMsg(page, on ? onMsg : offMsg);
}

// ================================================================ formatting
String fmtDur(uint32_t s) {
  uint32_t d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
  if (d) return String(d) + T(DUR_D) + " " + String(h) + T(DUR_H) + " " + String(m) + T(DUR_M);
  if (h) return String(h) + T(DUR_H) + " " + String(m) + T(DUR_M);
  if (m) return String(m) + T(DUR_M) + " " + String(s % 60) + T(DUR_S);
  return String(s) + T(DUR_S);
}

uint32_t pct(uint32_t part, uint32_t total) { return total ? (uint64_t)part * 100 / total : 0; }

// Wi-Fi signal quality (dBm: closer to 0 is better)
const char *rssiText(int rssi) {
  if (rssi >= -55) return T(RSSI_EXCELLENT);
  if (rssi >= -67) return T(RSSI_GOOD);
  if (rssi >= -75) return T(RSSI_WEAK);
  return T(RSSI_POOR);
}

const char *rssiClass(int rssi) { return rssi >= -67 ? "ok" : rssi >= -75 ? "warn" : "bad"; }

String macStr(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

// ================================================================ start
void begin() {
  // WebServer only keeps the headers it was asked for; these two are needed for CSRF checks
  const char *hdrs[] = {"Origin", "Referer"};
  server.collectHeaders(hdrs, 2);

  registerStatusPage();
  registerDnsPage();
  registerGuardPage();
  registerConfigPage();
  route("/lang", HTTP_POST, handleLang);
  server.on("/style.css", HTTP_GET, handleCss);   // styles and script: no password needed, nothing secret
  server.on("/app.js",    HTTP_GET, handleJs);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
  Serial.printf("[WEB] http://%s\n", Config::AP_IP.toString().c_str());
}

}  // namespace Web
