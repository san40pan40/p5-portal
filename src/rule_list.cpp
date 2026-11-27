#include "rule_list.h"
#include <LittleFS.h>
#include <algorithm>

// "*site.com" is a raw suffix rule; "*.site.com" is subdomains-only (found by hash lookup)
static inline bool isRawWild(const std::string &r) { return r.size() > 1 && r[0] == '*' && r[1] != '.'; }

static bool isIpLiteral(const String &s) {
  IPAddress ip;
  return ip.fromString(s);
}

// ================================================================ lookup
// Walk the domain's suffixes: "a.b.site.com" -> "b.site.com" -> "site.com" -> "com".
// For each suffix: a hash lookup of "site.com" (domain + subdomains) and of "*.site.com"
// (subdomains only — so only when at least one more label is on the left).
// Finally, scan the raw *suffix rules.
const std::string *RuleList::match(const char *name, size_t len) const {
  for (size_t pos = 0; pos < len;) {
    _key.assign(name + pos, len - pos);
    auto it = _set.find(_key);
    if (it != _set.end()) return &*it;
    if (pos > 0) {
      _key.insert(0, "*.");
      it = _set.find(_key);
      if (it != _set.end()) return &*it;
    }
    const char *dot = (const char *)memchr(name + pos, '.', len - pos);
    if (!dot) break;
    pos = dot - name + 1;
  }
  for (const std::string *r : _wild) {
    size_t sl = r->size() - 1;                     // suffix without the star
    if (len >= sl && memcmp(name + len - sl, r->data() + 1, sl) == 0) return r;
  }
  return nullptr;
}

// ================================================================ changes
bool RuleList::insert(const std::string &rule) {
  if (_set.size() >= MAX_RULES || ESP.getFreeHeap() < MIN_FREE_HEAP) {
    _full = true;
    return false;
  }
  auto res = _set.insert(rule);
  if (res.second && isRawWild(*res.first)) _wild.push_back(&*res.first);
  return res.second;
}

bool RuleList::erase(const String &rule) {
  auto it = _set.find(std::string(rule.c_str()));
  if (it == _set.end()) return false;
  _wild.erase(std::remove(_wild.begin(), _wild.end(), &*it), _wild.end());
  _set.erase(it);
  return true;
}

// One token -> a rule, or "" if invalid.
// Accepts: "ads.com", "ADS.com.", "*.ads.com", "*ads.com"
String RuleList::normalize(String d) {
  d.trim();
  d.toLowerCase();
  while (d.endsWith(".")) d.remove(d.length() - 1);

  bool wild = d.startsWith("*");                 // a star is only allowed at the start
  String body = wild ? d.substring(1) : d;

  if (body.length() < 3 || body.length() > 253) return String();
  if (body.indexOf('.') < 0) return String();    // needs at least one dot
  if (body.indexOf("..") >= 0) return String();  // empty labels
  if (!wild && body.startsWith(".")) return String();
  if (!wild && isIpLiteral(body)) return String();   // "0.0.0.0" is not a domain
  for (unsigned i = 0; i < body.length(); i++) {
    char c = body[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_')) return String();
  }
  return d;
}

// One line: "domain", "*.domain", "domain1 domain2", hosts format "0.0.0.0 domain1 domain2"
// (a leading IP is skipped) or AdBlock "||domain^". "# ..." after a space and whole "!" lines are comments;
// AdBlock cosmetic rules ("site.com##.ad") are skipped whole — cutting them at '#' would block the site.
// Counts every rule token as added or skipped (invalid, duplicate, or over the limit).
void RuleList::addLine(String line, int &added, int &skipped) {
  int hash = line.indexOf('#');
  if (hash > 0 && line[hash - 1] != ' ' && line[hash - 1] != '\t') {
    skipped++;
    return;
  }
  if (hash >= 0) line.remove(hash);
  line.replace('\t', ' ');
  line.replace('\r', ' ');
  line.trim();
  if (!line.length() || line[0] == '!') return;

  bool first = true;
  int p = 0;
  while (p < (int)line.length() && !_full) {
    int sp = line.indexOf(' ', p);
    if (sp < 0) sp = line.length();
    String tok = line.substring(p, sp);
    p = sp + 1;
    if (!tok.length()) continue;
    if (first && isIpLiteral(tok)) { first = false; continue; }   // hosts-file format
    first = false;
    if (tok.startsWith("||")) {                  // AdBlock: ||host^ (options after ^ are ignored)
      int c = tok.indexOf('^');
      tok = tok.substring(2, c < 0 ? tok.length() : c);
    }
    String d = normalize(tok);
    if (d.length() && _impReplace) {             // import "replace": clear only once a valid rule arrives,
      _impReplace = false;                       // so an empty or wrong file does not wipe the list
      clearRules();
    }
    if (d.length() && insert(d.c_str())) added++;
    else skipped++;
  }
}

// Many lines (the add form)
int RuleList::add(const String &text) {
  int added = 0, skipped = 0, start = 0;
  _full = false;
  while (start <= (int)text.length() && !_full) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    addLine(text.substring(start, nl), added, skipped);
    start = nl + 1;
  }
  if (added) save();
  return added;
}

// ================================================================ streamed import (file upload)
void RuleList::importBegin(bool replace) {
  _impLine.clear();
  _impAdded = _impSkipped = 0;
  _impLong = false;
  _impReplace = replace;
  _full = false;
}

void RuleList::importData(const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len && !_full; i++) {
    char c = (char)buf[i];
    if (c == '\n') {
      if (_impLong) _impSkipped++;
      else addLine(String(_impLine.c_str()), _impAdded, _impSkipped);
      _impLine.clear();
      _impLong = false;
    } else if (_impLine.size() < IMPORT_LINE_MAX) {
      _impLine += c;
    } else {
      _impLong = true;                           // a line this long is no rule — skip it
    }
  }
}

bool RuleList::importEnd() {
  if (_impLong) _impSkipped++;
  else if (!_full && !_impLine.empty()) addLine(String(_impLine.c_str()), _impAdded, _impSkipped);
  std::string().swap(_impLine);                  // give the buffer back
  _impReplace = false;
  if (_impAdded) save();
  return _impAdded > 0;
}

void RuleList::clearRules() {
  _wild.clear();
  _set.clear();
  _full = false;
  _truncated = false;                            // an explicit reset/replace may rewrite the file
}

bool RuleList::remove(const String &rule) {
  String r = normalize(rule);
  if (!r.length() || !erase(r)) return false;
  save();
  return true;
}

void RuleList::resetToDefaults() {
  clearRules();
  for (size_t i = 0; i < _defaultCount; i++) insert(_defaults[i]);
  save();
}

void RuleList::forEach(const std::function<void(const std::string &)> &f) const {
  for (auto &s : _set) f(s);
}

std::vector<const std::string *> RuleList::page(const String &q, size_t offset, size_t limit,
                                                size_t &matched) const {
  std::vector<const std::string *> v;
  for (auto &s : _set)
    if (!q.length() || s.find(q.c_str()) != std::string::npos) v.push_back(&s);
  matched = v.size();
  // partial_sort orders only the first `end` elements — cheaper than a full sort
  size_t end = std::min(offset + limit, v.size());
  std::partial_sort(v.begin(), v.begin() + end, v.end(),
                    [](const std::string *a, const std::string *b) { return *a < *b; });
  v.resize(end);
  v.erase(v.begin(), v.begin() + std::min(offset, end));   // keep just the requested page
  return v;
}

// ================================================================ file
// Atomic save: write a temp file and replace the real one only if it was written COMPLETELY.
// A partial write (full flash, failure) leaves the old list untouched.
bool RuleList::save() {
  if (_truncated) return false;                  // would cut the file down to what fit in RAM
  String tmp = String(_file) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return _saveOk = false;
  bool ok = true;
  for (auto &s : _set) {                         // order in the file does not matter
    if (f.write((const uint8_t *)s.data(), s.size()) != s.size() || f.write('\n') != 1) {
      ok = false;
      break;
    }
  }
  f.close();
  if (!ok) {
    LittleFS.remove(tmp);
    return _saveOk = false;
  }
  LittleFS.remove(_file);
  return _saveOk = LittleFS.rename(tmp, _file);
}

bool RuleList::load() {
  _wild.clear();
  _set.clear();
  _full = false;
  _truncated = false;
  // If power was lost between remove and rename, only the temp file exists
  String tmp = String(_file) + ".tmp";
  String path = LittleFS.exists(_file) ? String(_file) : LittleFS.exists(tmp) ? tmp : String();
  if (!path.length()) {
    resetToDefaults();
    return _saveOk;
  }
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  _set.reserve(f.size() / 16 + 16);              // ~16 bytes per rule — fewer rehashes
  // Read in 1 KB blocks and split lines ourselves (byte-by-byte Stream reads are very slow).
  // Only save() writes this file, so the rules are already normalised — no re-validation.
  char buf[1024];
  std::string line;
  auto flushLine = [&]() {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) insert(line);
    line.clear();
  };
  while (!_full) {                               // at the limit: keep what fits, skip the rest
    size_t n = f.read((uint8_t *)buf, sizeof(buf));
    if (!n) break;
    for (size_t i = 0; i < n; i++) {
      if (buf[i] == '\n') flushLine();
      else line += buf[i];
    }
  }
  flushLine();                                   // last line without \n
  f.close();
  _truncated = _full;
  if (_full) Serial.printf("[RULES] %s: limit reached, loaded %u rules, saving disabled\n", _file, (unsigned)_set.size());
  return true;
}
