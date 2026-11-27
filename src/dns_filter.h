#pragma once
/*
 * DnsFilter — a simplified "Pi-hole" for the ESP32.
 *
 * Listens for DNS queries on port 53 (all interfaces: AP clients and the home network).
 *   - domain on a blocklist -> answer 0.0.0.0 / ::  ("sinkhole")
 *   - otherwise             -> forward the query to a real DNS server (upstream)
 *                              and relay its answer to the client
 *
 * Two rule lists (see rule_list.h):
 *   blocklist  — the user's blocklist (/blocklist.txt)
 *   bypassList — DoH/DoT servers for the Guard (/doh.txt), active only while the Guard is on
 * Plus the Firefox canary (use-application-dns.net -> NXDOMAIN), which makes Firefox turn off its own DoH.
 */
#include <Arduino.h>
#include <WiFiUdp.h>
#include "rule_list.h"
#include "ring.h"

// Which list matched (for the log and the buttons on the page)
enum class RuleSource : uint8_t { None, Blocklist, Bypass, Canary };

struct DnsLogEntry {
  String     domain;
  String     rule;      // matching rule ("" = allowed)
  RuleSource src;
  IPAddress  client;    // who asked
  uint32_t   ms;        // millis() of the query
};

class DnsFilter {
public:
  static IPAddress defaultUpstream() { return IPAddress(1, 1, 1, 1); }   // Cloudflare
  static const std::string &firefoxCanary();     // use-application-dns.net

  DnsFilter();
  void begin(IPAddress apIp, IPAddress apMask);   // AP address + mask, so the upstream can't be us
  void loop();                                    // call continuously from loop()
  bool loadLists();

  RuleList &blocklist()  { return _block; }
  RuleList &bypassList() { return _bypass; }

  // ---- Settings ----
  // The bypass list and canary only apply while the Guard is on (AntiBypass::enabled())
  void setEnabled(bool e)    { _enabled = e; }
  bool enabled() const       { return _enabled; }
  // Board and router addresses in the home network: they can never be the upstream (loop).
  // Returns true if the current upstream had to be reset to the default.
  bool setStaAddrs(IPAddress self, IPAddress gateway);
  // nullptr = accepted; otherwise the reason the address is not allowed (then it is not changed)
  const char *setUpstream(IPAddress ip);
  IPAddress upstream() const { return _upstream; }

  // ---- Statistics and log ----
  // statFailed: upstream did not answer, was unreachable, or the query was evicted from a full queue
  uint32_t statTotal = 0, statBlocked = 0, statForwarded = 0, statFailed = 0;
  uint32_t statUpMs = 0, statUpN = 0;            // sum / count of upstream answer times (for the average)
  int  logCount() const { return _log.count(); }
  const DnsLogEntry &logAt(int i) const { return _log.at(i); }   // 0 = newest

private:
  struct Pending {                                // a query waiting for the upstream answer
    bool      used = false;
    uint16_t  newId = 0, origId = 0;
    IPAddress ip, upstream;                       // client, and the server we forwarded to
    uint16_t  port = 0;
    uint32_t  ms = 0;
  };
  static const int MAX_PENDING = 64;              // queries come from the AP and the home network

  const char *forbiddenUpstream(IPAddress ip) const;   // reason or nullptr
  void handleQuery(int len, IPAddress ip, uint16_t port);
  void handleUpstreamReply(int len, IPAddress from);
  void sendReply(int qEnd, uint16_t qtype, IPAddress ip, uint16_t port, uint8_t rcode);
  void sendHeaderOnly(IPAddress ip, uint16_t port, uint8_t rcode);
  void forward(int len, int qEnd, uint16_t qtype, IPAddress ip, uint16_t port);
  Pending *findPending(uint16_t id);
  void addLog(const char *domain, const std::string *rule, RuleSource src, IPAddress ip);

  WiFiUDP   _srv;                                 // client socket (port 53)
  WiFiUDP   _up;                                  // upstream socket
  IPAddress _apIp, _apMask, _staSelf, _staGw;
  IPAddress _upstream = defaultUpstream();
  bool      _enabled = true;
  uint8_t   _buf[1500];
  Pending   _pend[MAX_PENDING];
  RuleList  _block, _bypass;
  Ring<DnsLogEntry, 60> _log;
};
