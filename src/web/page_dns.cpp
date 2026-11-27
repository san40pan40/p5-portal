// DNS filter page (/dns): on/off, upstream, query log, add rules, blocklist
#include "web.h"
#include "app_network.h"
#include "app_config.h"

namespace Web {

// Sent in chunks: the blocklist can be large
static void handleDns() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  String p = pageStart("DNS фільтр", "/dns");
  auto flush = [&p](bool force = false) {         // send what we have once it gets big
    if (force || p.length() > 3000) {
      server.sendContent(p);
      p = "";
      dns.loop();                                 // keep answering DNS while the page is generated
    }
  };

  p += saveAlert(dns.blocklist());
  if (!Net::staConnected())
    p += "<div class=alert>Немає підключення до роутера: дозволені запити нікуди переслати.</div>";

  // ---- Tiles ----
  uint32_t tot = dns.statTotal;
  p += "<div class=tiles>";
  p += tile(String(tot), "DNS-запитів");
  p += tile(String(dns.statBlocked), "заблоковано · " + String(pct(dns.statBlocked, tot)) + "%", "bad");
  p += tile(String(dns.statForwarded), "переслано в upstream");
  p += tile(String(dns.statFailed), "без відповіді / помилок", dns.statFailed ? "warn" : "");
  p += tile(String(dns.blocklist().size()), "правил у списку");
  p += "</div>";

  // ---- On/off and upstream ----
  p += "<section class=card><h3>DNS фільтр</h3>" + toggleRow("DNS фільтр", dns.enabled(), "/dns/toggle") +
       "<form method=POST action='/dns/upstream' style='margin-top:.8em'><label>Upstream DNS — куди пересилати "
       "дозволені запити (1.1.1.1 Cloudflare, 8.8.8.8 Google, 9.9.9.9 Quad9)</label><div class=row>"
       "<input name=up value='" + dns.upstream().toString() + "'><button class=s>Зберегти</button></div></form></section>";
  flush(true);

  // ---- Recent queries ----
  p += "<section class=card><h3>Останні запити</h3><div class=row style='margin-bottom:.6em'>"
       "<input id=lf placeholder='фільтр журналу…' oninput=lgf(0)>"
       "<label><input type=checkbox id=lb onchange=lgf(0)>лише заблоковані</label>"
       "<a class='btn g s' href='/dns'>Оновити</a></div>"
       "<table id=lg><tr><th>коли</th><th>хто</th><th>домен</th><th></th></tr>";
  uint32_t now = millis();
  for (int i = 0; i < dns.logCount(); i++) {
    const DnsLogEntry &e = dns.logAt(i);
    bool blocked = e.src != RuleSource::None;
    p += "<tr data-d='" + esc(e.domain) + "'" + (blocked ? " class=b" : "") + "><td class=mut>" +
         fmtDur((now - e.ms) / 1000) + "</td><td class=mut>" + e.client.toString() + "</td><td>" + esc(e.domain);
    if (blocked && e.rule != e.domain) p += "<br><small class=mut>правило: " + esc(e.rule) + "</small>";
    p += "</td><td>";
    switch (e.src) {                              // the button depends on which list matched
      case RuleSource::None:      p += postBtn("/dns/add", "Блок", "r s", "domains", e.domain); break;
      case RuleSource::Blocklist: p += postBtn("/dns/del", "Дозволити", "g s", "domain", e.rule); break;
      case RuleSource::Bypass:    p += postBtn("/bypass/del?back=/dns", "Дозволити", "g s", "domain", e.rule); break;
      case RuleSource::Canary:    p += tag("Firefox DoH"); break;   // switched off together with the guard
    }
    p += "</td></tr>";
    flush();
  }
  if (dns.logCount() == 0)
    p += "<tr><td colspan=4 class=mut>поки порожньо — запити з'являться, щойно клієнти почнуть відкривати сайти</td></tr>";
  // Filter + pages of 20 run in the browser, so the search covers the whole log, not one page.
  // lgf(0) = first page (new filter), lgf(-1)/lgf(1) = newer/older
  p += "</table><div class=row id=lgp style='margin-top:.5em'>"
       "<button class='g s' onclick=lgf(-1)>&lsaquo; Новіші</button><span class=hint id=lgn></span>"
       "<button class='g s' onclick=lgf(1)>Старіші &rsaquo;</button></div>"
       "<script>var lgP=0;function lgf(d){var q=document.getElementById('lf').value.toLowerCase(),"
       "o=document.getElementById('lb').checked,all=document.querySelectorAll('#lg tr[data-d]'),m=[];"
       "all.forEach(function(r){r.style.display='none';if(r.dataset.d.indexOf(q)>=0&&(!o||r.classList.contains('b')))m.push(r)});"
       "var n=Math.max(1,Math.ceil(m.length/20));lgP=d?Math.min(n-1,Math.max(0,lgP+d)):0;"
       "m.slice(lgP*20,lgP*20+20).forEach(function(r){r.style.display=''});"
       "document.getElementById('lgn').textContent='сторінка '+(lgP+1)+' з '+n+' · записів: '+m.length;"
       "document.getElementById('lgp').style.display=n>1?'':'none'}lgf(0)</script></section>";
  flush(true);

  // ---- Add rules ----
  p += "<section class=card><h3>Заблокувати</h3><form method=POST action='/dns/add'>"
       "<textarea name=domains rows=3 placeholder='ads.example.com&#10;*.tracker.net&#10;*adserver.com'></textarea>"
       "<button class=r>Додати в чорний список</button></form>"
       "<details><summary>Як писати правила</summary><table>"
       "<tr><td><code>site.com</code></td><td>сам домен і всі піддомени</td></tr>"
       "<tr><td><code>*.site.com</code></td><td>лише піддомени (сам site.com працює)</td></tr>"
       "<tr><td><code>*site.com</code></td><td>усе, що закінчується на site.com: mysite.com, x.badsite.com…</td></tr>"
       "</table><p class=hint>По одному правилу на рядок (можна кілька через пробіл). "
       "Можна вставляти hosts-файли (<code>0.0.0.0 domain</code>), <code>#</code> — коментар.</p></details></section>";
  flush(true);

  // ---- Blocklist ----
  p += "<section class=card id=list><h3>Чорний список</h3>";
  ruleTable(p, dns.blocklist(), "/dns", "/dns/del", [&]() { flush(); });
  p += "</section>";
  p += PAGE_END;
  flush(true);
  server.sendContent("");                         // end of the chunked response
}

static void handleAdd() { ruleAdd(dns.blocklist(), "/dns"); }
static void handleDel() { ruleDel(dns.blocklist(), "/dns"); }

static void handleToggle() {
  dns.setEnabled(!dns.enabled());
  finishToggle("/dns", dns.enabled(), "DNS фільтр увімкнено", "DNS фільтр вимкнено — усі запити пропускаються");
}

static void handleUpstream() {
  IPAddress ip;
  if (!ip.fromString(server.arg("up")))
    return sendError("Невірний формат адреси upstream DNS (приклад: 1.1.1.1).", "/dns");
  // The filter itself knows every forbidden case (loops, special addresses) and explains why
  if (const char *why = dns.setUpstream(ip))
    return sendError(String("Цю адресу не можна вказати як upstream DNS: ") + why +
                     ". Вкажіть публічний DNS, напр. 1.1.1.1.", "/dns");
  Config::save();
  redirectMsg("/dns", "Upstream DNS: " + ip.toString());
}

void registerDnsPage() {
  route("/dns",          HTTP_ANY,  handleDns);
  route("/dns/add",      HTTP_POST, handleAdd);
  route("/dns/del",      HTTP_POST, handleDel);
  route("/dns/toggle",   HTTP_POST, handleToggle);
  route("/dns/upstream", HTTP_POST, handleUpstream);
}

}  // namespace Web
