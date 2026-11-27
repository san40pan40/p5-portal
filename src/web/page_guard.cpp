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
  p += cnt(AntiBypass::blocked53(), "відкинуто: чужий DNS (порт 53)");
  p += cnt(AntiBypass::blocked853(), "відкинуто: DNS-over-TLS (853)");
  p += cnt(AntiBypass::blockedDoh(), "відкинуто: DoH за IP");
  p += "</div><div class=grid>";

  p += "<section class=card><h3>Захист від обходу фільтра</h3>" + toggleRow("Охорона", on, "/bypass/toggle") +
       "<p class=hint>Пристрої не зможуть обійти DNS фільтр:</p><ul class=hint>"
       "<li>відкидаються DNS-запити не до нас (порт 53, напр. вручну прописаний 8.8.8.8)</li>"
       "<li>відкидається DNS-over-TLS (порт 853, Android «Приватний DNS»)</li>"
       "<li>заземлюються DoH/DoT-сервери зі списку нижче</li>"
       "<li>Firefox отримує сигнал вимкнути свій DoH</li></ul>"
       "<p class=hint>Після цього пристрої повертаються до нашого DNS.</p></section>";

  p += "<section class=card><h3>Жорсткий режим</h3>" + toggleRow("DoH за IP", dip, "/bypass/ip") +
       "<p class=hint>Додатково блокує HTTPS на IP відомих DoH-серверів (8.8.8.8, 1.1.1.1, 9.9.9.9…). "
       "Зупиняє застосунки з «зашитою» адресою DoH.</p>"
       "<p class=hint><span class=warn>Увага:</span> перестануть відкриватися й сайти на цих IP "
       "(напр. one.one.one.one). Працює лише разом з охороною.</p></section>";

  p += "<section class=card><h3>Останні спроби обходу</h3>";
  if (AntiBypass::logCount()) {
    const IPAddress &ap = Config::AP_IP;
    p += "<table><tr><th>коли</th><th>хто</th><th>куди намагався</th></tr>";
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
    p += "<p class=hint>Спроб обходу не було.</p>";
  }
  p += "</section>";

  // ---- Editable DoH/DoT server list ----
  p += "<section class=card id=list><h3>DoH/DoT-сервери (" + String(bl.size()) + ")</h3>"
       "<form method=POST action='/bypass/add'><div class=row>"
       "<input name=domains placeholder='dns.example.com'><button class='r s'>Додати</button></div></form>"
       "<p class=hint>Ті самі правила, що й у DNS фільтрі: <code>site.com</code>, <code>*.site.com</code>, "
       "<code>*site.com</code>. Діють лише коли охорону увімкнено.</p>";
  ruleTable(p, bl, "/bypass", "/bypass/del", []() {});
  p += "<p class=hint><code>" + String(DnsFilter::firefoxCanary().c_str()) +
       "</code> → «не існує» (сигнал Firefox вимкнути DoH) — працює завжди, коли охорону увімкнено.</p>"
       "<div class=row>" + postBtn("/bypass/reset", "Відновити типовий список", "g s", nullptr, "",
                                   "Замінити список типовим? Ваші зміни в ньому зникнуть.") + "</div></section>";
  p += "</div>";

  sendPage("Охорона", "/bypass", p);
}

static void handleToggle() {
  AntiBypass::setEnabled(!AntiBypass::enabled());
  finishToggle("/bypass", AntiBypass::enabled(), "Охорону увімкнено",
               "Охорону вимкнено — пристрої можуть обходити DNS фільтр");
}

static void handleIpToggle() {
  AntiBypass::setBlockDohIps(!AntiBypass::blockDohIps());
  finishToggle("/bypass", AntiBypass::blockDohIps(), "Жорсткий режим (DoH за IP) увімкнено", "Жорсткий режим вимкнено");
}

static void handleAdd() { ruleAdd(dns.bypassList(), "/bypass"); }
static void handleDel() { ruleDel(dns.bypassList(), "/bypass"); }

static void handleReset() {
  dns.bypassList().resetToDefaults();
  redirectMsg("/bypass", "Список DoH/DoT-серверів відновлено до типового");
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
