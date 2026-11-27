// DNS filter page (/dns): on/off, upstream, query log, add rules, blocklist
#include "web.h"
#include "app_network.h"
#include "app_config.h"
#include "metrics.h"
#include "preset_lists.h"

namespace Web {

// Top-10 card: header row of the table, or a hint while there is no data yet
static void topStart(String &p, const char *title, int n, const char *c1, const char *c2, const char *c3) {
  p += "<section class=card><h3>";
  p += title;
  p += "</h3>";
  if (!n) {
    p += "<p class=hint>";
    p += T(TOP_EMPTY);
    p += "</p>";
    return;
  }
  p += "<div class=tw><table><tr><th>";
  p += c1;
  p += "</th><th>";
  p += c2;
  p += "</th><th>";
  p += c3;
  p += "</th></tr>";
}

static void topEnd(String &p, int n) { p += n ? "</table></div></section>" : "</section>"; }

// Streamed card by card: the blocklist can be large
static void handleDns() {
  beginPage(T(MENU_FILTER), "/dns");
  String p;
  p.reserve(1200);                               // one buffer, reused for every card
  auto flush = [&p](bool force = false) { flushPage(p, force ? 0 : 1000); };

  p += saveAlert(dns.blocklist());
  if (!Net::staConnected()) p += String("<div class=alert>") + T(DNS_NO_ROUTER) + "</div>";

  // ---- Tiles ----
  uint32_t tot = dns.statTotal;
  char buf[128];
  snprintf(buf, sizeof(buf), T(DNS_BLOCKED_FMT), (unsigned)pct(dns.statBlocked, tot));
  p += "<div class=tiles>";
  p += tile(String(tot), T(DNS_QUERIES), "", "dt");              // live via /api/status
  p += tile(String(dns.statBlocked), buf, "", "bl");
  p += tile(String(dns.statForwarded), T(DNS_FORWARDED), "", "df");
  p += tile(String(dns.statFailed), T(DNS_FAILED), dns.statFailed ? "warn" : "", "dx");
  p += tile(String(dns.blocklist().size()), T(DNS_RULES));
  p += "</div>";
  // ---- History charts (last hour) ----
  p += histStart("dns", T(HI_DNS));
  p += chart("q,b", T(HI_QUERIES), T(HI_QB), true);
  p += chart("x", T(DNS_FAILED), "", true);
  p += chart("ms", T(HI_UPMS));
  p += "</div></section>";
  flush(true);

  // ---- Top 10 (since boot) ----
  const Metrics::TopDomain *td[10];
  int n = Metrics::topBlocked(td);
  p += "<div class=grid>";
  topStart(p, T(TOP_BLOCKED), n, T(DNS_COL_DOMAIN), T(TOP_TIMES), "");
  for (int i = 0; i < n; i++) {
    const char *d = td[i]->d;
    size_t len = strlen(d);
    p += "<tr><td>";
    p += esc(d);
    p += "</td><td>";
    p += td[i]->n;
    p += "</td><td class=a>";
    // "Allow" removes the rule that matched (a truncated name may not match it any more)
    const std::string *r = nullptr;
    const char *act = nullptr;
    if (len < Metrics::DOMAIN_LEN - 1) {
      if ((r = dns.blocklist().match(d, len))) act = "/dns/del";
      else if ((r = dns.bypassList().match(d, len))) act = "/bypass/del?back=/dns";
    }
    if (act) p += postBtn(act, T(DNS_ALLOW_BTN), "gh s", "domain", r->c_str());
    p += "</td></tr>";
  }
  topEnd(p, n);
  const Metrics::TopIp *tc[10];
  n = Metrics::topClients(tc);
  topStart(p, T(TOP_CLIENTS), n, T(DNS_COL_WHO), T(TOP_QUERIES), "%");
  for (int i = 0; i < n; i++) {
    uint32_t ip = tc[i]->ip;
    snprintf(buf, sizeof(buf), "<tr><td>%u.%u.%u.%u</td><td>%u</td><td class=mut>%u%%</td></tr>", (unsigned)(ip & 255),
             (unsigned)(ip >> 8 & 255), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 24), (unsigned)tc[i]->n,
             (unsigned)pct(tc[i]->n, tot));
    p += buf;
  }
  topEnd(p, n);
  p += "</div>";
  flush(true);

  // ---- On/off and upstream | add rules (side by side) ----
  p += String("<div class=grid><section class=card><h3>") + T(MENU_FILTER) + "</h3>" +
       toggleRow(T(MENU_FILTER), dns.enabled(), "/dns/toggle") + "<form method=POST action='/dns/upstream'><label class=f><span>" +
       T(DNS_UPSTREAM_LABEL) + "</span><span class=row><input name=up value='" + dns.upstream().toString() + "'><button>" +
       T(SAVE) + "</button></span></label></form></section>";
  flush(true);

  p += String("<section class=card><h3>") + T(DNS_BLOCK_CARD) + "</h3><form method=POST action='/dns/add'>"
       "<textarea name=domains rows=3 placeholder='ads.example.com&#10;*.tracker.net&#10;*adserver.com'></textarea>"
       "<div class=actions><button>" + T(DNS_ADD_BTN) + "</button></div></form>"
       "<details><summary>" + T(DNS_HOWTO) + "</summary><table>"
       "<tr><td><code>site.com</code></td><td>" + T(DNS_RULE_PLAIN) + "</td></tr>"
       "<tr><td><code>*.site.com</code></td><td>" + T(DNS_RULE_SUB) + "</td></tr>"
       "<tr><td><code>*site.com</code></td><td>" + T(DNS_RULE_SUFFIX) + "</td></tr>"
       "</table><p class=hint>" + T(DNS_RULES_HINT) + "</p></details></section></div>";
  flush(true);

  // ---- Preset lists: a switch per list. While a job runs or a list waits, app.js polls /api/update.
  // It reloads the page when "bl" (the state code) is not the same as data-r. ----
  p += String("<section class=card id=bl") + (PresetLists::pending() ? " data-r=" + String(PresetLists::stateCode()) : "") +
       "><h3>" + T(BL_CARD) + "</h3>";
  for (int i = 0; i < PresetLists::listCount(); i++) {
    const PresetLists::Info l = PresetLists::info(i);
    p += toggleRow(esc(l.name).c_str(), l.state != PresetLists::OFF, (String("/dns/bl?id=") + l.id).c_str());
    p += "<p class=hint>";
    if (!strncmp(l.homepage, "https://", 8)) p += "<a href='" + esc(l.homepage) + "' target=_blank rel=noopener>" + esc(l.license) + "</a>";
    else p += esc(l.license);
    if (l.count) p += String(" · ") + l.count + T(BL_DOMAINS);
    if (l.bytes) p += String(" · ") + (l.bytes + 1023) / 1024 + T(ST_KB);
    if (l.updated[0]) p += String(" · ") + T(BL_UPDATED) + esc(l.updated);
    p += " ";
    switch (l.state) {
      case PresetLists::OFF:     break;
      case PresetLists::QUEUED:  p += tag(T(BL_QUEUED), "warn"); break;
      case PresetLists::LOADING: p += tag((String(T(BL_LOADING)) + "<b data-p>" + l.progress + "%</b>").c_str()); break;
      case PresetLists::READY:   p += tag(T(BL_READY), "ok"); break;
      case PresetLists::FAILED:
        if (l.active) p += tag(T(BL_READY), "ok");
        p += "<br><span class=bad>" + String(T(BL_FAILED)) + esc(fetchErrText(l.err)) + "</span>"; break;
    }
    p += "</p>";
  }
  p += String("<p class=hint>") + T(BL_FREE) + PresetLists::freeKb() + T(BL_OF) + PresetLists::totalKb() + T(ST_KB) +
       ". " + T(BL_HINT) + "</p></section>";
  flush(true);

  // ---- Recent queries ----
  p += String("<section class=card><h3>") + T(DNS_RECENT) + "</h3><div class=row>"
       "<input id=lf placeholder='" + T(DNS_LOG_FILTER) + "' oninput=lgf(0)>"
       "<label class=cb><input type=checkbox id=lb onchange=lgf(0)>" + T(DNS_ONLY_BLOCKED) + "</label>"
       "<a class='btn g' href='/dns'>" + T(REFRESH) + "</a></div>"
       "<div class=tw><table id=lg><tr><th>" + T(DNS_COL_WHEN) + "</th><th>" + T(DNS_COL_WHO) + "</th><th>" + T(DNS_COL_DOMAIN) +
       "</th><th></th></tr>";
  uint32_t now = millis();
  for (int i = 0; i < dns.logCount(); i++) {
    const DnsLogEntry &e = dns.logAt(i);
    bool blocked = e.src != RuleSource::None;
    p += "<tr data-d='" + esc(e.domain) + "'" + (blocked ? " class=b" : "") + "><td class=mut>" +
         fmtDur((now - e.ms) / 1000) + "</td><td class=mut>" + e.client.toString() + "</td><td>" + esc(e.domain);
    if (blocked && e.rule != e.domain) p += String("<br><small class=mut>") + T(DNS_RULE) + esc(e.rule) + "</small>";
    p += "</td><td class=a>";
    switch (e.src) {                              // the button depends on which list matched
      case RuleSource::None:      p += postBtn("/dns/add", T(DNS_BLOCK_BTN), "gh s bad", "domains", e.domain); break;
      case RuleSource::Blocklist: p += postBtn("/dns/del", T(DNS_ALLOW_BTN), "gh s", "domain", e.rule); break;
      case RuleSource::Bypass:    p += postBtn("/bypass/del?back=/dns", T(DNS_ALLOW_BTN), "gh s", "domain", e.rule); break;
      case RuleSource::Canary:    p += tag("Firefox DoH"); break;   // switched off together with the Guard
      case RuleSource::Preset:                    // no per-domain allowlist for presets yet
        p += String("<span class=tag title='") + T(BL_NO_ALLOW) + "'>" + PresetLists::id(e.list) + "</span>";
        break;
    }
    p += "</td></tr>";
    flush();
  }
  if (dns.logCount() == 0) p += String("<tr><td colspan=4 class=mut>") + T(DNS_LOG_EMPTY) + "</td></tr>";
  // Filter + pages of 20 run in the browser (lgf() in app.js), so the search covers the whole log
  p += String("</table></div><div class=row id=lgp><button class='g s' onclick=lgf(-1)>") +
       T(DNS_NEWER) + "</button><span class=hint id=lgn data-a='" + T(DNS_JS_PAGE) + "' data-b='" + T(DNS_JS_OF) +
       "' data-c='" + T(DNS_JS_ENTRIES) + "'></span><button class='g s' onclick=lgf(1)>" + T(DNS_OLDER) +
       "</button></div></section>";
  flush(true);

  // ---- Blocklist ----
  p += String("<section class=card id=list><h3>") + T(DNS_BLOCKLIST) + "</h3>";
  ruleTable(p, dns.blocklist(), "/dns", "/dns/del");
  p += ruleIo("/dns") + "</section>";
  endPage(p);
}

static void handleAdd() { ruleAdd(dns.blocklist(), "/dns"); }
static void handleDel() { ruleDel(dns.blocklist(), "/dns"); }
static void handleExport() { ruleExport(dns.blocklist(), "blocklist", "p5-blocklist.txt"); }
static void handleImport() { ruleImportUpload(dns.blocklist()); }
static void handleImportDone() { ruleImportDone(dns.blocklist(), "/dns"); }

static void handleToggle() {
  dns.setEnabled(!dns.enabled());
  finishToggle("/dns", dns.enabled(), T(DNS_TURNED_ON), T(DNS_TURNED_OFF));
}

// Switch of a preset list: on queues the download (refused when it cannot fit), off deletes the file
static void handleList() {
  const int i = PresetLists::find(server.arg("id").c_str());
  if (i < 0) return sendError(T(BL_UNKNOWN), "/dns");
  const bool on = PresetLists::info(i).state == PresetLists::OFF;
  if (uint32_t need = PresetLists::setEnabled(i, on)) {
    NetFetch::Status st;
    st.err = NetFetch::E_SPACE;
    st.code = need;
    snprintf(st.detail, sizeof(st.detail), "%u", (unsigned)PresetLists::freeKb());
    return sendError(fetchErrText(st), "/dns");
  }
  redirectMsg("/dns", on ? T(BL_ON) : T(BL_OFF));
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
  route("/dns/export",   HTTP_GET,  handleExport);
  routeUpload("/dns/import", handleImportDone, handleImport);
  route("/dns/toggle",   HTTP_POST, handleToggle);
  route("/dns/upstream", HTTP_POST, handleUpstream);
  route("/dns/bl",       HTTP_POST, handleList);
}

}  // namespace Web
