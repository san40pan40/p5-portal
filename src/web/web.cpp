#include "web.h"
#include <WiFi.h>
#include "app_config.h"
#include "app_network.h"

namespace Web {

// ================================================================ styles
// Served as a separate /style.css that the browser caches, so pages stay small and fast
static const char CSS[] = R"CSS(
:root{--bg:#0f1b2d;--card:#16263d;--line:#243a57;--txt:#e7eef8;--mut:#8aa4c4;--acc:#3b82f6;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444;--hd:#0b1524}
*{box-sizing:border-box}
body{margin:0;font:15px/1.45 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--txt)}
header{position:sticky;top:0;z-index:9;background:var(--hd);border-bottom:1px solid var(--line)}
.top{display:flex;align-items:center;gap:.5em;padding:.55em 1em;max-width:980px;margin:auto}
.brand{font-weight:700;font-size:1.1em;margin-right:auto;white-space:nowrap}
.dot{display:inline-block;width:.7em;height:.7em;border-radius:50%;margin-right:.45em;vertical-align:middle}
.chip{font-size:.78em;padding:.2em .65em;border-radius:99px;background:var(--card);color:var(--mut);white-space:nowrap}
nav{display:flex;gap:.1em;padding:0 .5em;max-width:980px;margin:auto;overflow-x:auto}
nav a{color:var(--mut);text-decoration:none;padding:.55em .9em;border-bottom:2px solid transparent;white-space:nowrap}
nav a:hover{color:var(--txt)}
nav a.on{color:var(--txt);border-bottom-color:var(--acc)}
main{max-width:980px;margin:auto;padding:1em}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:1em;align-items:start}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1em;margin-bottom:1em}
.grid .card{margin-bottom:0}
.card h3{margin:0 0 .7em;font-size:.82em;color:var(--mut);text-transform:uppercase;letter-spacing:.05em}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:.7em;margin-bottom:1em}
.tile{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:.75em .9em}
.tile b{display:block;font-size:1.35em;line-height:1.2}
.tile span{color:var(--mut);font-size:.8em}
table{width:100%;border-collapse:collapse;font-size:.92em}
td,th{padding:.45em .35em;border-bottom:1px solid var(--line);text-align:left;vertical-align:top;word-break:break-word}
th{color:var(--mut);font-weight:500;font-size:.85em}
td.k{color:var(--mut);width:42%}
tr:last-child td{border-bottom:0}
tr.b td{color:#fca5a5}
input,textarea{width:100%;padding:.6em;margin:.25em 0 .8em;border-radius:8px;border:1px solid var(--line);background:var(--hd);color:var(--txt);font:inherit}
input[type=checkbox]{width:auto;margin:0 .4em 0 0;vertical-align:middle}
label{font-size:.85em;color:var(--mut)}
button,.btn{background:var(--acc);color:#fff;border:0;border-radius:8px;padding:.5em 1em;font:inherit;cursor:pointer;text-decoration:none;display:inline-block}
button:hover,.btn:hover{filter:brightness(1.15)}
.r{background:var(--bad)}.g{background:#334155}.s{padding:.25em .65em;font-size:.85em}
form.i{display:inline;margin:0}
.row{display:flex;gap:.5em;align-items:center;flex-wrap:wrap}
.row input{margin:0;flex:1;min-width:140px}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.mut{color:var(--mut)}
.tag{display:inline-block;font-size:.75em;font-weight:600;padding:.12em .55em;border-radius:99px;background:#334155;color:var(--txt)}
.tag.ok{background:#14532d;color:#bbf7d0}.tag.bad{background:#7f1d1d;color:#fecaca}.tag.warn{background:#78350f;color:#fde68a}
.alert{background:#3b2a0b;border:1px solid var(--warn);border-radius:10px;padding:.6em .85em;margin-bottom:1em}
.note{background:#0f2e1d;border:1px solid var(--ok);border-radius:10px;padding:.6em .85em;margin-bottom:1em}
.hint{color:var(--mut);font-size:.85em;margin:.4em 0}
code{background:var(--hd);padding:.1em .35em;border-radius:4px}
details summary{cursor:pointer;color:var(--mut);margin:.3em 0}
h2{margin:.2em 0 .8em}
)CSS";

static void handleCss() {
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send(200, "text/css", CSS);
}

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
String pageStart(const char *title, const char *active, const char *extraHead) {
  String h;
  h.reserve(2500);
  h += "<!DOCTYPE html><html lang=uk><head><meta charset=utf-8>"
       "<meta name=viewport content='width=device-width,initial-scale=1'>";
  h += extraHead;
  h += "<title>";
  h += title;
  // ?v=build time: after a firmware update the browser fetches fresh styles, not cached ones
  h += " · P5 Portal</title><link rel=stylesheet href='/style.css?v=" __DATE__ __TIME__ "'></head><body><header><div class=top>";

  const char *col, *txt;
  if (!Config::settings.staSsid.length()) { col = "var(--mut)";  txt = "роутер не налаштовано"; }
  else if (!Net::staConnected())          { col = "var(--bad)";  txt = "немає роутера"; }
  else if (!Net::internetOk())            { col = "var(--warn)"; txt = "немає інтернету"; }
  else                                    { col = "var(--ok)";   txt = "інтернет є"; }
  h += "<span class=brand><span class=dot style='background:";
  h += col;
  h += "'></span>P5 Portal</span><span class=chip>";
  h += txt;
  h += "</span><span class=chip>клієнтів: " + String(WiFi.softAPgetStationNum()) + "</span></div><nav>";

  static const char *const MENU[][2] = {
    {"/", "Статус"}, {"/dns", "DNS фільтр"}, {"/bypass", "Охорона"}, {"/config", "Налаштування"},
  };
  for (auto &m : MENU) {
    h += "<a href='";
    h += m[0];
    h += strcmp(m[0], active) == 0 ? "' class=on>" : "'>";
    h += m[1];
    h += "</a>";
  }
  h += "</nav></header><main>";
  if (server.hasArg("msg")) h += "<div class=note>" + esc(server.arg("msg")) + "</div>";   // result of the last action
  return h;
}

const char PAGE_END[] = "</main></body></html>";

// Sent in chunks (frame, body, end) — no extra copy of the whole page into one string
void sendPage(const char *title, const char *active, const String &body, int code, const char *extraHead) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(code, "text/html; charset=utf-8", "");
  server.sendContent(pageStart(title, active, extraHead));
  server.sendContent(body);
  server.sendContent(PAGE_END);
  server.sendContent("");                        // end of the chunked response
}

void sendError(const String &msg, const char *back) {
  sendPage("Помилка", "", "<section class=card><h3>Помилка</h3><p class=bad>" + esc(msg) +
           "</p><a class=btn href='" + back + "'>&larr; Назад</a></section>", 400);
}

void sendRestartPage(const char *heading) {
  sendPage("Перезавантаження", "", String("<section class=card><h3>") + heading +
           "</h3><p>Плата перезавантажується. Якщо змінювали назву чи пароль точки — "
           "перепідключіться до неї. Сторінка відкриється сама за 12 секунд.</p></section>",
           200, "<meta http-equiv=refresh content='12;url=/'>");
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
  redirectMsg(b == "/" || b == "/dns" || b == "/bypass" ? b.c_str() : def, msg);
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
  if (confirmText) f += String(" onsubmit=\"return confirm('") + confirmText + "')\"";
  f += ">";
  if (name) f += String("<input type=hidden name=") + name + " value='" + esc(val) + "'>";
  f += String("<button class='") + cls + "'>" + label + "</button></form>";
  return f;
}

String tag(const char *text, const char *cls) { return String("<span class='tag ") + cls + "'>" + text + "</span>"; }

String tile(const String &value, const String &label, const char *cls) {
  return String("<div class=tile><b class='") + cls + "'>" + value + "</b><span>" + label + "</span></div>";
}

String row(const String &k, const String &v) { return "<tr><td class=k>" + k + "</td><td>" + v + "</td></tr>"; }

// "Title [ON] [Turn off]"
String toggleRow(const char *title, bool on, const char *action) {
  return String("<div class=row><b>") + title + "</b>" + tag(on ? "УВІМКНЕНО" : "ВИМКНЕНО", on ? "ok" : "bad") +
         postBtn(action, on ? "Вимкнути" : "Увімкнути", on ? "g s" : "s") + "</div>";
}

String saveAlert(const RuleList &l) {
  return l.lastSaveOk() ? String()
                        : String("<div class=alert>Не вдалося зберегти список на флеш — "
                                 "останні зміни діють лише до перезавантаження.</div>");
}

// ================================================================ shared actions
void ruleAdd(RuleList &l, const char *page) {
  int n = l.add(server.arg("domains"));
  redirectBack(page, n ? "Додано правил: " + String(n)
                       : String("Нічого не додано: такі правила вже є або формат невірний"));
}

void ruleDel(RuleList &l, const char *page) {
  String r = server.arg("domain");
  redirectBack(page, l.remove(r) ? "Видалено: " + r : String("Правило не знайдено"));
}

void finishToggle(const char *page, bool on, const char *onMsg, const char *offMsg) {
  Config::save();
  redirectMsg(page, on ? onMsg : offMsg);
}

// ================================================================ formatting
String fmtDur(uint32_t s) {
  uint32_t d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
  if (d) return String(d) + "д " + String(h) + "г " + String(m) + "хв";
  if (h) return String(h) + "г " + String(m) + "хв";
  if (m) return String(m) + "хв " + String(s % 60) + "с";
  return String(s) + "с";
}

uint32_t pct(uint32_t part, uint32_t total) { return total ? (uint64_t)part * 100 / total : 0; }

// Wi-Fi signal quality (dBm: closer to 0 is better)
const char *rssiText(int rssi) {
  if (rssi >= -55) return "відмінний";
  if (rssi >= -67) return "добрий";
  if (rssi >= -75) return "слабкий";
  return "дуже слабкий";
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
  server.on("/style.css", HTTP_GET, handleCss);   // styles: no password needed, nothing secret
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
  Serial.printf("[WEB] http://%s\n", Config::AP_IP.toString().c_str());
}

}  // namespace Web
