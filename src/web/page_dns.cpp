// DNS filter page (/dns): on/off, upstream, query log, add rules, blocklist
#include "web.h"
#include "app_network.h"
#include "app_config.h"

namespace Web {

// Sent in chunks: the blocklist can be large
static void handleDns() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  String p = pageStart(T(MENU_FILTER), "/dns");
  auto flush = [&p](bool force = false) {         // send what we have once it gets big
    if (force || p.length() > 3000) {
      server.sendContent(p);
      p = "";
      dns.loop();                                 // keep answering DNS while the page is generated
    }
  };

  p += saveAlert(dns.blocklist());
  if (!Net::staConnected()) p += String("<div class=alert>") + T(DNS_NO_ROUTER) + "</div>";

  // ---- Tiles ----
  uint32_t tot = dns.statTotal;
  char buf[64];
  snprintf(buf, sizeof(buf), T(DNS_BLOCKED_FMT), (unsigned)pct(dns.statBlocked, tot));
  p += "<div class=tiles>";
  p += tile(String(tot), T(DNS_QUERIES));
  p += tile(String(dns.statBlocked), buf, "bad");
  p += tile(String(dns.statForwarded), T(DNS_FORWARDED));
  p += tile(String(dns.statFailed), T(DNS_FAILED), dns.statFailed ? "warn" : "");
  p += tile(String(dns.blocklist().size()), T(DNS_RULES));
  p += "</div>";

  // ---- On/off and upstream ----
  p += String("<section class=card><h3>") + T(MENU_FILTER) + "</h3>" + toggleRow(T(MENU_FILTER), dns.enabled(), "/dns/toggle") +
       "<form method=POST action='/dns/upstream' style='margin-top:.8em'><label>" + T(DNS_UPSTREAM_LABEL) +
       "</label><div class=row><input name=up value='" + dns.upstream().toString() + "'><button class=s>" + T(SAVE) +
       "</button></div></form></section>";
  flush(true);

  // ---- Recent queries ----
  p += String("<section class=card><h3>") + T(DNS_RECENT) + "</h3><div class=row style='margin-bottom:.6em'>"
       "<input id=lf placeholder='" + T(DNS_LOG_FILTER) + "' oninput=lgf(0)>"
       "<label><input type=checkbox id=lb onchange=lgf(0)>" + T(DNS_ONLY_BLOCKED) + "</label>"
       "<a class='btn g s' href='/dns'>" + T(REFRESH) + "</a></div>"
       "<table id=lg><tr><th>" + T(DNS_COL_WHEN) + "</th><th>" + T(DNS_COL_WHO) + "</th><th>" + T(DNS_COL_DOMAIN) +
       "</th><th></th></tr>";
  uint32_t now = millis();
  for (int i = 0; i < dns.logCount(); i++) {
    const DnsLogEntry &e = dns.logAt(i);
    bool blocked = e.src != RuleSource::None;
    p += "<tr data-d='" + esc(e.domain) + "'" + (blocked ? " class=b" : "") + "><td class=mut>" +
         fmtDur((now - e.ms) / 1000) + "</td><td class=mut>" + e.client.toString() + "</td><td>" + esc(e.domain);
    if (blocked && e.rule != e.domain) p += String("<br><small class=mut>") + T(DNS_RULE) + esc(e.rule) + "</small>";
    p += "</td><td>";
    switch (e.src) {                              // the button depends on which list matched
      case RuleSource::None:      p += postBtn("/dns/add", T(DNS_BLOCK_BTN), "r s", "domains", e.domain); break;
      case RuleSource::Blocklist: p += postBtn("/dns/del", T(DNS_ALLOW_BTN), "g s", "domain", e.rule); break;
      case RuleSource::Bypass:    p += postBtn("/bypass/del?back=/dns", T(DNS_ALLOW_BTN), "g s", "domain", e.rule); break;
      case RuleSource::Canary:    p += tag("Firefox DoH"); break;   // switched off together with the Guard
    }
    p += "</td></tr>";
    flush();
  }
  if (dns.logCount() == 0) p += String("<tr><td colspan=4 class=mut>") + T(DNS_LOG_EMPTY) + "</td></tr>";
  // Filter + pages of 20 run in the browser, so the search covers the whole log, not one page.
  // lgf(0) = first page (new filter), lgf(-1)/lgf(1) = newer/older
  p += String("</table><div class=row id=lgp style='margin-top:.5em'>"
              "<button class='g s' onclick=lgf(-1)>") + T(DNS_NEWER) + "</button><span class=hint id=lgn></span>"
       "<button class='g s' onclick=lgf(1)>" + T(DNS_OLDER) + "</button></div>"
       "<script>var lgP=0;function lgf(d){var q=document.getElementById('lf').value.toLowerCase(),"
       "o=document.getElementById('lb').checked,all=document.querySelectorAll('#lg tr[data-d]'),m=[];"
       "all.forEach(function(r){r.style.display='none';if(r.dataset.d.indexOf(q)>=0&&(!o||r.classList.contains('b')))m.push(r)});"
       "var n=Math.max(1,Math.ceil(m.length/20));lgP=d?Math.min(n-1,Math.max(0,lgP+d)):0;"
       "m.slice(lgP*20,lgP*20+20).forEach(function(r){r.style.display=''});"
       "document.getElementById('lgn').textContent='" + T(DNS_JS_PAGE) + "'+(lgP+1)+'" + T(DNS_JS_OF) + "'+n+'" +
       T(DNS_JS_ENTRIES) + "'+m.length;"
       "document.getElementById('lgp').style.display=n>1?'':'none'}lgf(0)</script></section>";
  flush(true);

  // ---- Add rules ----
  p += String("<section class=card><h3>") + T(DNS_BLOCK_CARD) + "</h3><form method=POST action='/dns/add'>"
       "<textarea name=domains rows=3 placeholder='ads.example.com&#10;*.tracker.net&#10;*adserver.com'></textarea>"
       "<button class=r>" + T(DNS_ADD_BTN) + "</button></form>"
       "<details><summary>" + T(DNS_HOWTO) + "</summary><table>"
       "<tr><td><code>site.com</code></td><td>" + T(DNS_RULE_PLAIN) + "</td></tr>"
       "<tr><td><code>*.site.com</code></td><td>" + T(DNS_RULE_SUB) + "</td></tr>"
       "<tr><td><code>*site.com</code></td><td>" + T(DNS_RULE_SUFFIX) + "</td></tr>"
       "</table><p class=hint>" + T(DNS_RULES_HINT) + "</p></details></section>";
  flush(true);

  // ---- Blocklist ----
  p += String("<section class=card id=list><h3>") + T(DNS_BLOCKLIST) + "</h3>";
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
  finishToggle("/dns", dns.enabled(), T(DNS_TURNED_ON), T(DNS_TURNED_OFF));
}

static void handleUpstream() {
  IPAddress ip;
  if (!ip.fromString(server.arg("up"))) return sendError(T(DNS_UP_BAD_FORMAT), "/dns");
  // The filter itself knows every forbidden case (loops, special addresses) and explains why
  if (const char *why = dns.setUpstream(ip))
    return sendError(String(T(DNS_UP_REJECTED)) + why + T(DNS_UP_SUGGEST), "/dns");
  Config::save();
  redirectMsg("/dns", T(DNS_UP_SET) + ip.toString());
}

void registerDnsPage() {
  route("/dns",          HTTP_ANY,  handleDns);
  route("/dns/add",      HTTP_POST, handleAdd);
  route("/dns/del",      HTTP_POST, handleDel);
  route("/dns/toggle",   HTTP_POST, handleToggle);
  route("/dns/upstream", HTTP_POST, handleUpstream);
}

}  // namespace Web
