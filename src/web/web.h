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
#include "i18n.h"
#include "net_fetch.h"

namespace Web {

void begin();                 // register all pages and start the HTTP server

// Each page group registers its own routes (defined in page_*.cpp)
void registerStatusPage();   // also GET /api/status
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
// Streamed pages: beginPage() (HTTP header + frame), then build a card in a String and
// flushPage() it (sent only if longer than `over`; also serves DNS meanwhile), endPage() closes.
void beginPage(const char *title, const char *active, int code = 200, const char *extraHead = "");
void flushPage(String &p, size_t over = 0);
const char *hdrState(const char *&cls);         // header text; cls = colour class (ok/warn/bad/mut)
void endPage(String &p);
// Small pages built in one String
void sendPage(const char *title, const char *active, const String &body, int code = 200,
              const char *extraHead = "");
void sendError(const String &msg, const char *back);
String fetchErrText(const NetFetch::Status &st);   // why a NetFetch job failed (firmware or list)
// Reply, then reboot. apSsid set: the AP name/password changed — tell the user to reconnect to it.
void sendRestartPage(const char *heading, const String &apSsid = "");
void redirectMsg(const char *url, const String &msg);      // 303 + green note on the next page
void redirectBack(const char *def, const String &msg);     // to ?back= (our pages only) or def

// ---- HTML pieces ----
String esc(const String &s);                     // HTML-escape (domains come from the network)
String urlEncode(const String &s);
String postBtn(const char *action, const char *label, const char *cls = "", const char *name = nullptr,
               const String &val = "", const char *confirmText = nullptr);
String tag(const char *text, const char *cls = "");
// vk / lk: data-k keys of the value / label, updated live by app.js from /api/status
String tile(const String &value, const String &label, const char *cls = "", const char *vk = nullptr,
            const char *lk = nullptr);
String row(const String &k, const String &v);
String liveSpan(const char *key, const String &v);     // <span data-k=key>v</span>
String toggleRow(const char *title, bool on, const char *action);   // on/off switch form
String saveAlert(const RuleList &l);
// History charts card (drawn by app.js from /api/hist?set=...): histStart, chart()..., "</div></section>".
// keys = series of /api/hist (comma-separated), legend = series names split by '|' ("" = one series).
// total: per-interval counters — the header shows the sum for the hour (Σ), not the last 10 s (often 0)
String histStart(const char *set, const char *title);
String chart(const char *keys, const char *title, const char *legend = "", bool total = false);

// ---- Shared actions ----
void ruleAdd(RuleList &l, const char *page);
void ruleDel(RuleList &l, const char *page);
// Export: streamed text file, one rule per line. Import: routeUpload() pair — the upload part
// parses the file chunk by chunk (merge, or replace with ?replace field), done() reports the counts.
void ruleExport(const RuleList &l, const char *name, const char *file);
void ruleImportUpload(RuleList &l);
void ruleImportDone(RuleList &l, const char *page);
String ruleIo(const char *page);                 // export link + import form (page = "/dns" or "/bypass")
void finishToggle(const char *page, bool on, const char *onMsg, const char *offMsg);  // save + redirect

// ---- Formatting ----
String      fmtDur(uint32_t seconds);            // 93784 -> "1d 2h 3m"
uint32_t    pct(uint32_t part, uint32_t total);  // 64-bit safe percentage
const char *rssiText(int rssi);
const char *rssiClass(int rssi);
String      macStr(const uint8_t *m);

// Rule list table: search (?q=), pages of 20 (?p=), a ghost "✕" (delete) button per rule.
// Streamed: sends p in chunks (flushPage) while the rows are built.
void ruleTable(String &p, const RuleList &l, const char *page, const char *delAction);

}  // namespace Web
