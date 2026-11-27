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
#include <string>
#include <unordered_set>
#include <vector>

class RuleList {
public:
  RuleList(const char *file, const char *const *defaults, size_t defaultCount)
    : _file(file), _defaults(defaults), _defaultCount(defaultCount) {}

  bool load();                                    // from the file; no file -> the default list
  void resetToDefaults();                         // restore the default list and save it
  int  add(const String &text);                   // many rules (hosts format too); returns how many were added
  bool remove(const String &rule);
  const std::string *match(const char *name, size_t len) const;   // matching rule or nullptr
  // A page for display: rules containing q (empty = all), alphabetical,
  // `limit` items from `offset`. `matched` = total matches.
  // Only sorts up to the end of the requested page, not the whole list.
  std::vector<const std::string *> page(const String &q, size_t offset, size_t limit, size_t &matched) const;
  size_t size() const { return _set.size(); }
  bool   lastSaveOk() const { return _saveOk; }

private:
  static String normalize(String d);              // string -> rule; "" if invalid
  bool insert(const std::string &rule);           // into _set and, if needed, _wild
  bool erase(const String &rule);
  bool save();

  const char        *_file;
  const char *const *_defaults;
  size_t             _defaultCount;
  bool               _saveOk = true;
  // unordered_set never moves its elements, so pointers to them stay valid
  std::unordered_set<std::string>  _set;
  std::vector<const std::string *> _wild;         // raw *suffix rules only
  mutable std::string _key;                       // reused lookup key — no malloc per query
};
