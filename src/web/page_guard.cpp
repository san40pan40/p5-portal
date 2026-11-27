// Guard page (/bypass): stops clients from bypassing the DNS filter
#include "web.h"
#include "app_config.h"
#include "anti_bypass.h"

namespace Web {

static void handleGuard() {
  bool on = AntiBypass::enabled(), dip = AntiBypass::blockDohIps();
  RuleList &bl = dns.bypassList();
  String p;
  p.reserve(6000);

  p += saveAlert(bl);
  auto cnt = [](uint32_t n, const char *label) { return tile(String(n), label, n ? "warn" : ""); };
  p += "<div class=tiles>";
  p += cnt(AntiBypass::blocked53(), T(GD_DROP53));
  p += cnt(AntiBypass::blocked853(), T(GD_DROP853));
  p += cnt(AntiBypass::blockedDoh(), T(GD_DROPDOH));
  p += "</div><div class=grid>";

  p += String("<section class=card><h3>") + T(GD_CARD) + "</h3>" + toggleRow(T(MENU_GUARD), on, "/bypass/toggle") +
       "<p class=hint>" + T(GD_INTRO) + "</p><ul class=hint><li>" + T(GD_LI53) + "</li><li>" + T(GD_LI853) +
       "</li><li>" + T(GD_LIDOH) + "</li><li>" + T(GD_LIFF) + "</li></ul><p class=hint>" + T(GD_OUTRO) + "</p></section>";

  p += String("<section class=card><h3>") + T(GD_STRICT_CARD) + "</h3>" + toggleRow(T(GD_DOH_IP), dip, "/bypass/ip") +
       "<p class=hint>" + T(GD_STRICT_TEXT) + "</p><p class=hint>" + T(GD_STRICT_WARN) + "</p></section>";

  p += String("<section class=card><h3>") + T(GD_ATTEMPTS) + "</h3>";
  if (AntiBypass::logCount()) {
    const IPAddress &ap = Config::AP_IP;
    p += String("<table><tr><th>") + T(DNS_COL_WHEN) + "</th><th>" + T(DNS_COL_WHO) + "</th><th>" + T(GD_COL_TARGET) + "</th></tr>";
    uint32_t t = millis();
    for (int i = 0; i < AntiBypass::logCount(); i++) {
      AntiBypass::Attempt a = AntiBypass::logAt(i);
      const char *what = a.port == 53 ? "DNS" : a.port == 853 ? "DoT" : "DoH";
      p += "<tr class=b><td class=mut>" + fmtDur((t - a.ms) / 1000) + "</td><td>" +
           IPAddress(ap[0], ap[1], ap[2], a.client).toString() + "</td><td>" +
           IPAddress(a.dst).toString() + ":" + String(a.port) + " " + tag(what, "bad") + "</td></tr>";
    }
    p += "</table>";
  } else {
    p += String("<p class=hint>") + T(GD_NO_ATTEMPTS) + "</p>";
  }
  p += "</section>";

  // ---- Editable DoH/DoT server list ----
  p += String("<section class=card id=list><h3>") + T(GD_SERVERS) + " (" + String(bl.size()) + ")</h3>"
       "<form method=POST action='/bypass/add'><div class=row><input name=domains placeholder='dns.example.com'>"
       "<button class='r s'>" + T(ADD) + "</button></div></form><p class=hint>" + T(GD_RULES_HINT) + "</p>";
  ruleTable(p, bl, "/bypass", "/bypass/del", []() {});
  p += String("<p class=hint><code>") + DnsFilter::firefoxCanary().c_str() + "</code>" + T(GD_CANARY_HINT) + "</p>"
       "<div class=row>" + postBtn("/bypass/reset", T(GD_RESET_LIST), "g s", nullptr, "", T(GD_RESET_CONFIRM)) +
       "</div></section></div>";

  sendPage(T(MENU_GUARD), "/bypass", p);
}

static void handleToggle() {
  AntiBypass::setEnabled(!AntiBypass::enabled());
  finishToggle("/bypass", AntiBypass::enabled(), T(GD_ON), T(GD_OFF));
}

static void handleIpToggle() {
  AntiBypass::setBlockDohIps(!AntiBypass::blockDohIps());
  finishToggle("/bypass", AntiBypass::blockDohIps(), T(GD_STRICT_ON), T(GD_STRICT_OFF));
}

static void handleAdd() { ruleAdd(dns.bypassList(), "/bypass"); }
static void handleDel() { ruleDel(dns.bypassList(), "/bypass"); }

static void handleReset() {
  dns.bypassList().resetToDefaults();
  redirectMsg("/bypass", T(GD_LIST_RESET));
}

void registerGuardPage() {
  route("/bypass",        HTTP_ANY,  handleGuard);
  route("/bypass/toggle", HTTP_POST, handleToggle);
  route("/bypass/ip",     HTTP_POST, handleIpToggle);
  route("/bypass/add",    HTTP_POST, handleAdd);
  route("/bypass/del",    HTTP_POST, handleDel);
  route("/bypass/reset",  HTTP_POST, handleReset);
}

}  // namespace Web
