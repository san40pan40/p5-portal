#pragma once
/*
 * RuleList — a list of domain-blocking rules stored in a file on LittleFS.
 * Used twice: the blocklist (/blocklist.txt) and the DoH/DoT server list (/doh.txt).
 *
 * Rule types:
 *   site.com     -> the domain and all subdomains   (site.com, ads.site.com)
 *   *.site.com   -> subdomains only                  (ads.site.com, but NOT site.com)
 *   *site.com    -> anything ending with it          (site.com, mysite.com, x.badsite.com)
 *
 * The first two are matched by hash lookups over the domain's suffixes (fast for any list size).
 * The third ("raw" suffix) is a linear scan, so keep such rules few.
 */
#include <Arduino.h>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

class RuleList {
public:
  // Rules live in internal RAM (~80 bytes each). Hard cap per list, and inserting also stops while
  // free internal heap is below MIN_FREE_HEAP — so a huge paste or file cannot starve Wi-Fi/lwIP
  // (and load() cannot crash on every boot).
  static constexpr size_t   MAX_RULES     = 5000;
  static constexpr uint32_t MIN_FREE_HEAP = 40 * 1024;

  RuleList(const char *file, const char *const *defaults, size_t defaultCount)
    : _file(file), _defaults(defaults), _defaultCount(defaultCount) {}

  bool load();                                    // from the file; no file -> the default list
  void resetToDefaults();                         // restore the default list and save it
  int  add(const String &text);                   // many rules (hosts format too); returns how many were added
  bool limitHit() const { return _full; }         // the last add()/load()/import stopped at the limit
  // Streamed import (file upload): importBegin, importData per chunk (lines may span chunks),
  // importEnd saves once. replace: the old rules go when the first valid rule arrives.
  void importBegin(bool replace);
  void importData(const uint8_t *buf, size_t len);
  bool importEnd();                               // true if anything was added
  void importAbort();                             // upload broke off: reload the saved file, save nothing
  int  importAdded() const { return _impAdded; }
  int  importSkipped() const { return _impSkipped; }
  void forEach(const std::function<void(const std::string &)> &f) const;   // every rule, any order
  bool remove(const String &rule);
  const std::string *match(const char *name, size_t len) const;   // matching rule or nullptr
  // A page for display: rules containing q (empty = all), alphabetical,
  // `limit` items from `offset`. `matched` = total matches.
  // Only sorts up to the end of the requested page, not the whole list.
  std::vector<const std::string *> page(const String &q, size_t offset, size_t limit, size_t &matched) const;
  size_t size() const { return _set.size(); }
  bool   lastSaveOk() const { return _saveOk; }
  bool   truncated() const { return _truncated; }  // load() hit the limit: saving is refused

private:
  static String normalize(String d);              // string -> rule; "" if invalid
  static constexpr size_t IMPORT_LINE_MAX = 300;
  void addLine(String line, int &added, int &skipped);
  void importLine();
  void clearRules();
  bool insert(const std::string &rule);           // into _set and, if needed, _wild
  bool erase(const String &rule);
  bool save();

  const char        *_file;
  const char *const *_defaults;
  size_t             _defaultCount;
  bool               _saveOk = true;
  bool               _full = false;
  bool               _truncated = false;          // the file holds more than was loaded
  bool               _impReplace = false, _impLong = false, _impFirst = false;
  int                _impAdded = 0, _impSkipped = 0;
  std::string        _impLine;                    // import: the line split by a chunk boundary
  // unordered_set never moves its elements, so pointers to them stay valid
  std::unordered_set<std::string>  _set;
  std::vector<const std::string *> _wild;         // raw *suffix rules only
  mutable std::string _key;                       // reused lookup key — no malloc per query
};
