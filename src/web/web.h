#pragma once
/*
 * Web — shared parts of the web panel: page frame (header + menu), styles, access control
 * (password + CSRF), routing and small HTML helpers. Each page lives in its own page_*.cpp
 * and registers its routes through route()/routeUpload(), so access control cannot be forgotten.
 *
 * Pages: /  Status · /dns  DNS filter · /bypass  Guard · /config  Settings (+ OTA)
 */
#include <Arduino.h>
#include "app.h"
#include "rule_list.h"

namespace Web {

void begin();                 // register all pages and start the HTTP server

// Each page group registers its own routes (defined in page_*.cpp)
void registerStatusPage();
void registerDnsPage();
void registerGuardPage();
void registerConfigPage();

// ---- Routing and access control ----
// Every route goes through the same check: same-origin POST (CSRF) + panel password.
void route(const char *uri, HTTPMethod method, void (*handler)());
// File upload: `upload` receives the chunks (and must call accessOk() itself), `done` replies.
void routeUpload(const char *uri, void (*done)(), void (*upload)());
bool accessOk();              // the single access rule, without sending any response

// ---- Page output ----
String pageStart(const char *title, const char *active, const char *extraHead = "");
extern const char PAGE_END[];
void sendPage(const char *title, const char *active, const String &body, int code = 200,
              const char *extraHead = "");
void sendError(const String &msg, const char *back);
void sendRestartPage(const char *heading);       // reply, then reboot
void redirectMsg(const char *url, const String &msg);      // 303 + green note on the next page
void redirectBack(const char *def, const String &msg);     // to ?back= (our pages only) or def

// ---- HTML pieces ----
String esc(const String &s);                     // HTML-escape (domains come from the network)
String urlEncode(const String &s);
String postBtn(const char *action, const char *label, const char *cls = "", const char *name = nullptr,
               const String &val = "", const char *confirmText = nullptr);
String tag(const char *text, const char *cls = "");
String tile(const String &value, const String &label, const char *cls = "");
String row(const String &k, const String &v);
String toggleRow(const char *title, bool on, const char *action);
String saveAlert(const RuleList &l);

// ---- Shared actions ----
void ruleAdd(RuleList &l, const char *page);
void ruleDel(RuleList &l, const char *page);
void finishToggle(const char *page, bool on, const char *onMsg, const char *offMsg);  // save + redirect

// ---- Formatting ----
String      fmtDur(uint32_t seconds);            // 93784 -> "1д 2г 3хв"
uint32_t    pct(uint32_t part, uint32_t total);  // 64-bit safe percentage
const char *rssiText(int rssi);
const char *rssiClass(int rssi);
String      macStr(const uint8_t *m);

// Rule list table: search (?q=), pages of 50 (?p=), a "Delete" button per rule.
// flush() lets big pages be sent in chunks.
template <class F>
void ruleTable(String &p, const RuleList &l, const char *page, const char *delAction, F flush) {
  const size_t PER = 50;
  String q = server.arg("q");
  q.toLowerCase();
  p += "<form method=GET action='" + String(page) + "#list' class=row style='margin-bottom:.6em'>"
       "<input name=q value='" + esc(q) + "' placeholder='пошук по списку'><button class='g s'>Знайти</button></form>";

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
    pager = "<div class=row style='margin:.5em 0'>";
    if (pg > 0) pager += "<a class='btn g s' href='" + base + String(pg - 1) + "#list'>&lsaquo; Назад</a>";
    pager += "<span class=hint>сторінка " + String(pg + 1) + " з " + String(pages) +
             " &middot; правил: " + String(matched) + "</span>";
    if (pg + 1 < pages) pager += "<a class='btn g s' href='" + base + String(pg + 1) + "#list'>Далі &rsaquo;</a>";
    pager += "</div>";
  }

  p += pager + "<table>";
  for (const std::string *r : rules) {
    String rule(r->c_str());
    p += "<tr><td>" + esc(rule) + "</td><td style='text-align:right'>" +
         postBtn(delAction, "Видалити", "g s", "domain", rule) + "</td></tr>";
    flush();
  }
  if (rules.empty()) p += "<tr><td class=mut>" + String(q.length() ? "нічого не знайдено" : "список порожній") + "</td></tr>";
  p += "</table>" + pager;
}

}  // namespace Web
